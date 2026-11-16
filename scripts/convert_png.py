#!/usr/bin/env -S uv run --script

# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=11"]
# ///

"""
Converts a PNG into the 8-bit palette format the Vita's package installer accepts.

A VPK whose tile PNGs are anything else is rejected outright: the install dies with
0x8010113D and the package says nothing about why. 24-bit TrueColor is the format that
gets shipped by mistake, because a renderer asked for "-depth 8" happily produces 8 bits
*per channel*, and `magick identify` then reports the result as an 8-bit Palette image.
So this script decides the format itself and then re-reads the file it wrote: the check
parses PNG chunks rather than asking a tool what it thinks it made.

A valid output is color type 3, bit depth 8, non-interlaced, with IHDR, PLTE, optional
tRNS (only when the source really has transparency), IDAT and IEND, and no ancillary
chunk. The ancillary ones matter as much as the palette: ImageMagick writes tEXt/date and
bKGD by default, and the installer has been seen to object to those too.

Usage:
    uv run scripts/convert_png.py <input.png> [...] [--out PATH | --out-dir DIR] [--force]
    uv run scripts/convert_png.py --check <input.png> [...]

Args:
    input: One or more PNGs to convert, or to verify in --check mode.
    out: Destination for a single input. May name the input itself to convert in place.
    out-dir: Write every input into DIR under its own name (batch, e.g. a whole tile set).
    force: Overwrite an existing destination. Required to convert a file onto itself.

Note:
    Two encoders, chosen by `--quantizer`, and Pillow is the default because its dependency
    comes from this file's own metadata: a fresh machine needs only uv, and the result does not
    drift with a Homebrew version. Measured against the 24-bit render of the tile sources (mean
    squared error per channel, lower is better): on the icon Pillow scores 0.034 and pngquant
    0.027; on the livearea gradient Pillow scores 0.0006 against pngquant's 0.077, and an
    ImageMagick `PNG8:` write — what this tree shipped before — scores 1.48 and 1.01. No
    `--quality` range is passed to pngquant on purpose: asked for 256 colours it still chose 62
    for the gradient, and the quality floor is what lets it. Encoding runs into a sibling
    temporary file renamed into place, so a rejected conversion leaves the original untouched,
    and the rename only happens after the written file has been read back and verified.
"""

import argparse
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageChops

# Chunks a tile may carry. tRNS only earns its place when the source has real transparency,
# but it is tolerated either way: a palette image with transparency cannot express it otherwise.
ALLOWED_CHUNKS = {b"IHDR", b"PLTE", b"tRNS", b"IDAT", b"IEND"}
REQUIRED_CHUNKS = {b"IHDR", b"PLTE", b"IDAT", b"IEND"}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_COLORS = 256
COLOR_TYPE_NAMES = {0: "gray", 2: "RGB", 3: "palette", 4: "gray+alpha", 6: "RGBA"}
# Pillow 13 offers these three on Quantize-eligible images; there is no Riemersma mode here,
# whatever other PNG quantizers advertise.
DITHER_MODES = {"none": Image.Dither.NONE, "ordered": Image.Dither.ORDERED, "floyd-steinberg": Image.Dither.FLOYDSTEINBERG}
MISSING_PNGQUANT_HINT = "brew install pngquant, or pass --quantizer pillow"


class PngError(Exception):
    """A problem the user can act on; reported as `error: <message>` with exit 1."""


def read_chunks(path: Path) -> list[tuple[bytes, bytes]]:
    """Returns the PNG's (type, data) chunks in file order, rejecting a malformed container."""
    try:
        blob = path.read_bytes()
    except OSError as error:
        raise PngError(f"cannot read {path}: {error.strerror or error}") from None
    if not blob.startswith(PNG_SIGNATURE):
        raise PngError(f"{path} is not a PNG (starts with {blob[:8]!r})")

    chunks: list[tuple[bytes, bytes]] = []
    offset = len(PNG_SIGNATURE)
    while offset + 12 <= len(blob):
        (length,) = struct.unpack_from(">I", blob, offset)
        kind = blob[offset + 4 : offset + 8]
        chunks.append((kind, blob[offset + 8 : offset + 8 + length]))
        offset += 12 + length
    if chunks and offset != len(blob):
        raise PngError(f"{path} has {len(blob) - offset} trailing byte(s): truncated or padded PNG")
    if not chunks:
        raise PngError(f"{path} carries no chunks")
    return chunks


def inspect(path: Path) -> dict:
    """Describes an image in exactly the terms the installer's rejection is judged by."""
    chunks = read_chunks(path)
    if chunks[0][0] != b"IHDR":
        raise PngError(f"{path}: first chunk is {chunks[0][0].decode(errors='replace')}, expected IHDR")

    width, height, depth, color_type, _compression, _filter, interlace = struct.unpack(">IIBBBBB", chunks[0][1])
    kinds = [kind for kind, _ in chunks]
    problems: list[str] = []
    if color_type != 3:
        problems.append(f"color type {color_type} ({COLOR_TYPE_NAMES.get(color_type, 'unknown')}), must be 3 (palette)")
    if depth != 8:
        problems.append(f"bit depth {depth}, must be 8")
    if interlace != 0:
        problems.append(f"interlace method {interlace}, must be 0")
    if (extra := sorted(name.decode(errors="replace") for name in set(kinds) - ALLOWED_CHUNKS)):
        problems.append(f"ancillary chunk(s) {', '.join(extra)}")
    if missing := sorted(name.decode() for name in REQUIRED_CHUNKS - set(kinds)):
        problems.append(f"missing chunk(s) {', '.join(missing)}")

    palette = next((data for kind, data in chunks if kind == b"PLTE"), b"")
    return {
        "path": path,
        "width": width,
        "height": height,
        "depth": depth,
        "color_type": color_type,
        "colors": len(palette) // 3,
        "chunks": [kind.decode(errors="replace") for kind in kinds],
        "size": path.stat().st_size,
        "problems": problems,
    }


def source_has_alpha(path: Path) -> bool:
    """True when some pixel is actually transparent, not merely an opaque alpha channel."""
    with Image.open(path) as image:
        alpha = image.convert("RGBA").getchannel("A")
    low, _high = alpha.getextrema()
    return low < 255


def pngquant_command(source: Path, target: Path, colors: int, dither: str) -> list[str]:
    """Builds the pngquant invocation; --speed 1 buys the best palette it will build."""
    argv = ["pngquant", "--speed", "1", "--force"]
    if dither == "none":
        argv.append("--nofs")
    # No --quality range: it lets pngquant decide it needs fewer colours than asked (the
    # gradient came back with 62 of 256, 300x the error of Pillow). The colour count is
    # positional and belongs before --; after it pngquant reads another input file.
    return [*argv, "--output", str(target), str(colors), "--", str(source)]


def run_pngquant(source: Path, target: Path, colors: int, dither: str) -> None:
    """Runs pngquant into target, turning a refusal or a crash into a PngError."""
    result = subprocess.run(pngquant_command(source, target, colors, dither), capture_output=True, text=True)
    if result.returncode != 0 or not target.exists():
        lines = (result.stderr or result.stdout).strip().splitlines()
        reason = lines[-1] if lines else f"exit code {result.returncode}"
        raise PngError(f"pngquant produced nothing for {source}: {reason}")


def run_pillow(source: Path, target: Path, colors: int, dither: str) -> None:
    """Quantizes with Pillow, preferring libimagequant where this build has it."""
    with Image.open(source) as image:
        image.load()
        pixels = image.convert("RGBA") if source_has_alpha(source) else image.convert("RGB")
    for method in (Image.Quantize.LIBIMAGEQUANT, Image.Quantize.MEDIANCUT, Image.Quantize.FASTOCTREE):
        try:
            result = pixels.quantize(colors=colors, method=method, dither=DITHER_MODES[dither])
        except ValueError:
            continue  # this Pillow build lacks that quantizer; official wheels ship no libimagequant
        # Pad the palette to the full 256 entries. Pillow derives the bit depth from how many
        # entries the palette has, so a flat-fill tile quantizes to one colour and comes out a
        # 1-bit colormap PNG, which the installer has never been asked to accept. Unused entries
        # are never indexed, so the padding is invisible; it costs 768 bytes and guarantees depth 8.
        padded = result.copy()
        padded.putpalette(((result.getpalette() or []) + [0] * 768)[:768])
        padded.save(target, format="PNG", optimize=True)
        return
    raise PngError(f"Pillow has no usable quantizer for {source}")


def encode(source: Path, target: Path, colors: int, dither: str, quantizer: str, force: bool) -> str:
    """Writes a verified palette PNG for source into target via a temporary file; names the encoder."""
    if not source.is_file():
        raise PngError(f"no such file: {source}")
    if not 2 <= colors <= MAX_COLORS:
        raise PngError(f"--colors must be from 2 to {MAX_COLORS}, got {colors}")
    if target.exists() and not force:
        raise PngError(f"{target} already exists; pass --force to overwrite")

    source_info = inspect(source)  # also proves the input is a PNG at all
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f".{target.stem}.part{target.suffix}")  # keeps .png: both encoders pick the format from the name
    used = quantizer
    try:
        if quantizer == "pngquant" and shutil.which("pngquant") is None:
            raise PngError(f"pngquant not found on PATH: {MISSING_PNGQUANT_HINT}")
        if used == "pngquant":
            run_pngquant(source, temporary, colors, dither)
        else:
            run_pillow(source, temporary, colors, dither)

        written = inspect(temporary)  # trust the bytes on disk, not the encoder's exit code
        if written["problems"]:
            raise PngError(f"{used} produced an uninstallable PNG: {'; '.join(written['problems'])}")
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)

    if source_info["problems"]:
        print(f"  note: {source} itself would have been rejected: {'; '.join(source_info['problems'])}")
    return used


def fidelity(source: Path, target: Path) -> str:
    """PSNR of the conversion against its source, or an empty string when it cannot be measured."""
    try:
        with Image.open(source) as before, Image.open(target) as after:
            before, after = before.convert("RGB"), after.convert("RGB")
            if before.size != after.size:
                return ""
            diff = ImageChops.difference(before, after)
            squares = [
                sum(count * index * index for index, count in enumerate(diff.getchannel(channel).histogram()))
                for channel in "RGB"
            ]
    except (OSError, ValueError):
        return ""  # fidelity is a courtesy; failing to measure it must not fail the conversion
    mse = sum(squares) / (3 * before.width * before.height)
    return ", lossless" if mse == 0 else f", PSNR {10 * math.log10(255 * 255 / mse):.1f} dB"


def report(source: Path, target: Path, used: str) -> None:
    """Prints what was written, by which encoder, and how far it drifted."""
    info = inspect(target)
    print(
        f"{source} -> {target} "
        f"({used}, {info['colors']} colors, {info['width']}x{info['height']}, {info['size']} B{fidelity(source, target)})"
    )


def resolve_targets(inputs: list[Path], out: str | None, out_dir: str | None) -> list[tuple[Path, Path]]:
    """Pairs each input with its destination: --out for one file, --out-dir for many, else a sibling."""
    if out and out_dir:
        raise PngError("--out and --out-dir are mutually exclusive")
    if out and len(inputs) > 1:
        raise PngError(f"--out takes a single input, got {len(inputs)}; use --out-dir for several")
    if out:
        return [(inputs[0], Path(out).expanduser())]
    if out_dir:
        directory = Path(out_dir).expanduser()
        return [(source, directory / source.name) for source in inputs]
    return [(source, source.with_name(f"{source.stem}-8bit{source.suffix}")) for source in inputs]


def check(paths: list[Path]) -> int:
    """Verifies each file without touching it; returns the number the installer would reject."""
    rejected = 0
    for path in paths:
        info = inspect(path)
        detail = "; ".join(info["problems"]) or f"{info['colors']} colors, {info['width']}x{info['height']}, {', '.join(info['chunks'])}"
        print(f"  {'ok  ' if not info['problems'] else 'FAIL'} {path}  {detail}")
        rejected += bool(info["problems"])
    return rejected


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="convert_png",
        description="Convert PNGs to the 8-bit palette format the Vita installer requires.",
        epilog="example: uv run scripts/convert_png.py --out sce_sys_sshd/icon0.png --force /tmp/icon0.png",
    )
    parser.add_argument("inputs", nargs="+", metavar="input.png", help="PNG(s) to convert, or to verify with --check")
    parser.add_argument("--out", help="destination for a single input (may be the input itself)")
    parser.add_argument("--out-dir", dest="out_dir", help="write each input into DIR under its own name")
    parser.add_argument("-f", "--force", action="store_true", help="overwrite an existing destination")
    parser.add_argument("--colors", type=int, default=MAX_COLORS, help=f"palette size, 2-{MAX_COLORS} (default {MAX_COLORS})")
    parser.add_argument("--dither", choices=sorted(DITHER_MODES), default="floyd-steinberg", help="dithering (default floyd-steinberg)")
    parser.add_argument("--quantizer", choices=["pillow", "pngquant"], default="pillow", help="encoder to use (default pillow, whose Pillow dependency this file declares; pngquant needs it on PATH)")
    parser.add_argument("--check", action="store_true", help="verify without converting; exit 1 if any file would be rejected")
    args = parser.parse_args(argv)

    try:
        inputs = [Path(name).expanduser() for name in args.inputs]
        if args.check:
            if args.force or args.out or args.out_dir:
                raise PngError("--check writes nothing; drop --out, --out-dir and --force")
            if (rejected := check(inputs)):
                raise PngError(f"{rejected} of {len(inputs)} file(s) would be rejected by the installer")
            return 0
        for source, target in resolve_targets(inputs, args.out, args.out_dir):
            report(source, target, encode(source, target, args.colors, args.dither, args.quantizer, args.force))
    except PngError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

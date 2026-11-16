#!/usr/bin/env -S uv run --script

# /// script
# requires-python = ">=3.13,<3.14"
# dependencies = []
# ///

"""
Reads a Vita crash dump (psp2core-*.psp2dmp) and says where the crash happened.

The dump is an ELF core file, optionally gzipped, whose PT_NOTE segment carries three
Sony-specific notes (MODULE_INFO, THREAD_INFO, THREAD_REG_INFO) and whose PT_LOAD segments
hold the memory image. This script reads those directly with the standard library, so a
fresh machine needs only uv (which fetches Python 3.13) plus the VitaSDK binutils on PATH.

Addresses are symbolised against the ELF the dump's app was built from. That has to be the
very file that produced the installed VPK: rebuild first and every function name is wrong.
The match is made by module name, so the ELF's file name (`ssh_server_vita`) must equal the
module name the dump reports; override with --module when it does not.

Usage:
    scripts/parse_dump.py <dump.psp2dmp> <build/ssh_server_vita> [--module NAME] [--stack WORDS]
        [--no-disas]

Args:
    dump: The crash dump, as copied off ux0:data/.
    elf: The unstripped ELF the app was linked as (the file next to the .velf).
    module: Module name to symbolise against the ELF. Defaults to the ELF's file name.
    stack: How many words of the stack to print from SP. Default 32.
    no-disas: Skip the objdump disassembly around PC and LR.

Note:
    addr2line and objdump are called as arm-vita-eabi-addr2line and arm-vita-eabi-objdump
    and have to be on PATH ($VITASDK/bin). A crashed thread is any thread with a nonzero stop
    reason; threads parked in the kernel report zero. A Thumb address has its low bit set, so
    the bit is stripped before an address goes to addr2line.
"""

import argparse
import gzip
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

STOP_REASONS = {
    0: "No reason",
    0x30002: "Undefined instruction exception",
    0x30003: "Prefetch abort exception",
    0x30004: "Data abort exception",
    0x60080: "Division by zero",
}
STATUSES = {1: "Running", 8: "Waiting", 16: "Not started"}
GPR_NAMES = {13: "SP", 14: "LR", 15: "PC"}

PT_LOAD = 1
PT_NOTE = 4
PF_X = 1

# Offsets inside the Sony note records, which carry no field names of their own.
MODULE_HEAD_SIZE = 0x50
MODULE_SEG_SIZE = 0x14
MODULE_FOOT_SIZE = 0x10
THREAD_NAME_OFF = 8
THREAD_STATUS_OFF = 0x30
THREAD_STOP_OFF = 0x74
THREAD_PC_OFF = 0x9C


@dataclass
class Segment:
    start: int
    size: int


@dataclass
class Module:
    name: str
    segments: list[Segment] = field(default_factory=list)


@dataclass
class Thread:
    uid: int
    name: str
    status: int
    stop_reason: int
    pc: int
    gpr: list[int] = field(default_factory=list)


@dataclass
class Core:
    modules: list[Module]
    threads: list[Thread]
    memory: list[tuple[int, bytes]]


def u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def c_str(buf: bytes, off: int) -> str:
    end = buf.index(b"\0", off) if b"\0" in buf[off:] else len(buf)
    return buf[off:end].decode("ascii", "replace")


def program_headers(elf: bytes) -> list[tuple[int, int, int, int, int]]:
    """(p_type, p_offset, p_vaddr, p_filesz, p_flags) for every ELF32 program header."""
    if elf[:4] != b"\x7fELF" or elf[4] != 1:
        sys.exit("not a 32-bit ELF file")
    (phoff,) = struct.unpack_from("<I", elf, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    headers = []
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, _memsz, p_flags, _align = struct.unpack_from(
            "<8I", elf, phoff + i * phentsize
        )
        headers.append((p_type, p_offset, p_vaddr, p_filesz, p_flags))
    return headers


def notes(elf: bytes, offset: int, size: int) -> dict[str, bytes]:
    out = {}
    pos, end = offset, offset + size
    while pos + 12 <= end:
        namesz, descsz, _type = struct.unpack_from("<3I", elf, pos)
        pos += 12
        name = elf[pos : pos + namesz].rstrip(b"\0").decode("ascii", "replace")
        pos += (namesz + 3) & ~3
        out[name] = elf[pos : pos + descsz]
        pos += (descsz + 3) & ~3
    return out


def read_core(path: Path) -> Core:
    raw = path.read_bytes()
    if raw[:2] == b"\x1f\x8b":
        raw = gzip.decompress(raw)
    found: dict[str, bytes] = {}
    memory = []
    for p_type, p_offset, p_vaddr, p_filesz, _flags in program_headers(raw):
        if p_type == PT_NOTE:
            found.update(notes(raw, p_offset, p_filesz))
        elif p_type == PT_LOAD:
            memory.append((p_vaddr, raw[p_offset : p_offset + p_filesz]))
    for needed in ("MODULE_INFO", "THREAD_INFO", "THREAD_REG_INFO"):
        if needed not in found:
            sys.exit(f"{path}: no {needed} note, so this is not a Vita crash dump")
    return Core(parse_modules(found["MODULE_INFO"]), parse_threads(found), memory)


def parse_modules(data: bytes) -> list[Module]:
    modules = []
    off = 8
    for _ in range(u32(data, 4)):
        num_segs = u32(data, off + 0x4C)
        module = Module(c_str(data, off + 0x24))
        off += MODULE_HEAD_SIZE
        for i in range(num_segs):
            seg = off + i * MODULE_SEG_SIZE
            module.segments.append(Segment(u32(data, seg + 8), u32(data, seg + 12)))
        off += num_segs * MODULE_SEG_SIZE + MODULE_FOOT_SIZE
        modules.append(module)
    return modules


def parse_threads(found: dict[str, bytes]) -> list[Thread]:
    threads: dict[int, Thread] = {}
    data = found["THREAD_INFO"]
    off = 8
    for _ in range(u32(data, 4)):
        size = u32(data, off)
        rec = data[off : off + size]
        thread = Thread(
            uid=u32(rec, 4),
            name=c_str(rec, THREAD_NAME_OFF),
            status=u16(rec, THREAD_STATUS_OFF),
            stop_reason=u32(rec, THREAD_STOP_OFF),
            pc=u32(rec, THREAD_PC_OFF),
        )
        threads[thread.uid] = thread
        off += size
    data = found["THREAD_REG_INFO"]
    off = 8
    for _ in range(u32(data, 4)):
        size = u32(data, off)
        rec = data[off : off + size]
        thread = threads.get(u32(rec, 4))
        if thread is not None:
            thread.gpr = [u32(rec, 8 + 4 * i) for i in range(16)]
        off += size
    return list(threads.values())


class Symbolizer:
    def __init__(self, core: Core, elf_path: Path, module_name: str):
        self.core = core
        self.elf_path = elf_path
        self.module_name = module_name
        self.text_vaddr = self.exec_vaddr(elf_path)

    @staticmethod
    def exec_vaddr(elf_path: Path) -> int:
        """Link address of the ELF's executable segment, which addr2line is keyed on."""
        for p_type, _off, p_vaddr, _filesz, flags in program_headers(elf_path.read_bytes()):
            if p_type == PT_LOAD and flags & PF_X:
                return p_vaddr
        sys.exit(f"{elf_path}: no executable PT_LOAD segment")

    def locate(self, vaddr: int):
        for module in self.core.modules:
            for num, seg in enumerate(module.segments, 1):
                if seg.start <= vaddr < seg.start + seg.size:
                    return module, num, vaddr - seg.start
        return None

    def describe(self, vaddr: int) -> str:
        where = self.locate(vaddr & ~1)
        if where is None:
            return f"0x{vaddr:x}"
        module, num, offset = where
        text = f"0x{vaddr:x} ({module.name}@{num} + 0x{offset:x}"
        if module.name == self.module_name and num == 1:
            text += f" => {self.addr2line(offset)}"
        return text + ")"

    def addr2line(self, offset: int) -> str:
        out = subprocess.run(
            [
                "arm-vita-eabi-addr2line",
                "-e",
                str(self.elf_path),
                "-f",
                "-p",
                "-C",
                hex(offset + self.text_vaddr),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        return out.stdout.strip() or "??"

    def disassemble(self, vaddr: int) -> str | None:
        where = self.locate(vaddr & ~1)
        if where is None or where[0].name != self.module_name or where[1] != 1:
            return None
        link = where[2] + self.text_vaddr
        out = subprocess.run(
            [
                "arm-vita-eabi-objdump",
                "-d",
                f"--start-address=0x{link - 0x10:x}",
                f"--stop-address=0x{link + 0x10:x}",
                str(self.elf_path),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        marker = f"{link:x}:"
        body = [
            line for line in out.stdout.splitlines() if line.strip() and not line.startswith(("Disassembly", str(self.elf_path)))
        ]
        return "\n".join(f"!!! {line}" if line.strip().startswith(marker) else f"    {line}" for line in body)

    def read(self, vaddr: int, size: int) -> bytes | None:
        for start, data in self.core.memory:
            if start <= vaddr < start + len(data):
                return data[vaddr - start : vaddr - start + size]
        return None


def report(core: Core, sym: Symbolizer, stack_words: int, disas: bool) -> int:
    print("=== THREADS ===")
    for t in core.threads:
        reason = STOP_REASONS.get(t.stop_reason, "")
        print(f"  {t.name}")
        print(f"      id: 0x{t.uid:x}")
        print(f"      stop reason: 0x{t.stop_reason:x} ({reason})")
        print(f"      status: 0x{t.status:x} ({STATUSES.get(t.status, '')})")
        print(f"      pc: {sym.describe(t.pc)}")
    crashed = [t for t in core.threads if t.stop_reason != 0]
    if not crashed:
        print("\nno thread has a stop reason; nothing crashed in this dump")
        return 1
    for t in crashed:
        print(f'\n=== THREAD "{t.name}" <0x{t.uid:x}> CRASHED ({STOP_REASONS.get(t.stop_reason, hex(t.stop_reason))}) ===')
        if not t.gpr:
            print("no register record for this thread")
            continue
        print("\nREGISTERS:")
        for i, value in enumerate(t.gpr):
            label = GPR_NAMES.get(i, f"R{i}")
            print(f"  {label:>3}: {sym.describe(value) if i >= 13 else f'0x{value:x}'}")
        if disas:
            for label, value in (("PC", t.gpr[15]), ("LR", t.gpr[14])):
                text = sym.disassemble(value)
                if text:
                    print(f"\nDISASSEMBLY AROUND {label}: 0x{value:x}")
                    print(text)
        sp = t.gpr[13]
        print(f"\nSTACK FROM SP (0x{sp:x}), {stack_words} words:")
        for i in range(stack_words):
            raw = sym.read(sp + 4 * i, 4)
            if raw is None or len(raw) < 4:
                print("  (stack memory is not in the dump past this point)")
                break
            word = u32(raw, 0)
            known = sym.locate(word & ~1)
            note = f"  {sym.describe(word)}" if known else ""
            print(f"  0x{sp + 4 * i:08x}: 0x{word:08x}{note}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Symbolise a Vita crash dump.")
    ap.add_argument("dump", type=Path)
    ap.add_argument("elf", type=Path)
    ap.add_argument("--module", help="module name to match against the ELF (default: its file name)")
    ap.add_argument("--stack", type=int, default=32, metavar="WORDS")
    ap.add_argument("--no-disas", action="store_true")
    args = ap.parse_args()

    core = read_core(args.dump)
    sym = Symbolizer(core, args.elf, args.module or args.elf.name)
    if not any(m.name == sym.module_name for m in core.modules):
        names = ", ".join(m.name for m in core.modules)
        print(
            f"warning: no module named {sym.module_name!r} in the dump ({names}); nothing will be symbolised, pass --module",
            file=sys.stderr,
        )
    return report(core, sym, args.stack, not args.no_disas)


if __name__ == "__main__":
    sys.exit(main())

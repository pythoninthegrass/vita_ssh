# AGENTS.md

## Provenance

- Standalone repo, not a fork. It began as a fork of Rompelhd's VitaSSH
  (<https://github.com/rompelhd/VitaSSH>), whose history was dropped when this became its own
  project; packaging and installation also follow TheOfficialFloW's VitaShell. Both are
  credited in README.md. Upstream is effectively unmaintained and is not a seam to keep
  clean: this repo is canonical, so fix cruft rather than working around it.

## Build

- `task` (Taskfile, <https://taskfile.dev>) is the entry point — never run `cmake` directly.
  Bare `cmake` dies with "Please define VITASDK" because CMakeLists.txt reads it from the
  environment. `task` with no args lists tasks.
- Order matters: `task setup` → `task build`. Setup builds the OpenSSL 1.1.1 Vita port
  then libssh2 into `$VITASDK` (libssh2 configures against that OpenSSL; openssl first
  is required, not a preference).
- Both tasks fingerprint inputs and skip when current. After editing a portlib recipe or
  lib source, force: `task --force build`, `task --force setup:openssl`,
  `task --force setup:libssh2`. The libssh2 version is pinned by `pkgver=` in
  `libs/libssh2/VITABUILD`, which `setup:libssh2` reads.
- Setup stages ~160 MB of OpenSSL/libssh2 sources under `.task/portlibs` (gitignored).
  `task clean` wipes `build/` and `.task/` but leaves the libs installed in `$VITASDK`.
- No tests, lint, or CI exist. A successful `task build` producing both
  `build/ssh_client_vita.vpk` and `build/ssh_server_vita.vpk` is the only verification.
  Output is arm-vita-eabi (cross-compiled); nothing here builds or runs on the host.
- One cmake invocation builds both apps, and `task build` exits non-zero if either fails
  to compile (make normally still produces the other app's VPK in the same pass). For a
  compile-and-link check of one app, build its **executable** target:
  `cmake --build build --target ssh_server_vita`. The `.self` and `.vpk` targets are
  stamped files — vita.cmake gives the VPK no edge on the SELF, which is why
  CMakeLists.txt adds `add_dependencies(<app>.vpk-vpk <app>.self-self)` by hand. Without
  it a VPK is packed once on a clean tree and never repacked, no matter how many sources
  change. Packages come from `task build`, never from asking make for a `.vpk`.

## Configuring VITASDK

- Resolution order: `task build VITASDK=<path>` → `.env` → `~/.local/opt/vitasdk`.
  Copy `.env.example` to `.env` (gitignored) for machine-local config. `build` fingerprints
  `.env`, so editing it re-runs cmake.
- Changing the SDK path requires `rm -rf build/` — `CMakeCache.txt` pins absolute
  toolchain paths (`CMAKE_C_COMPILER`, `CMAKE_AR`, …) from whichever value configured it.
- The root taskfile deliberately has **no `env:` block**, and it should stay that way. An
  `env:` entry sharing a name with a `vars:` key silently blocks the dotenv value from
  reaching `{{.VITASDK}}`, and `env: PATH` never applies at all — a variable already in
  the process environment outranks Taskfile env values. Instead, `build` passes `VITASDK=`
  to cmake explicitly and the portlib recipes `export PATH` themselves.
- The `| default` fallback is declared **once**, in the root `vars:`. `taskfiles/setup.yml`
  is not a usable entrypoint — `task --taskfile taskfiles/setup.yml libssh2` dies on its own
  relative paths — so the parent's vars are always in scope for the dozen or so
  `{{.VITASDK}}` expansions there. The fallback earns its keep on a blank value (e.g.
  `VITASDK=` empty in `.env`), which would otherwise install portlibs into `/arm-vita-eabi`
  and drop the toolchain off `PATH`.
- `dotenv` only works in the entrypoint taskfile: it is not supported inside included
  taskfiles, so keep it in `taskfile.yml`, not `taskfiles/setup.yml`.

## Source quirks

- Two apps, two `add_executable` lists in `CMakeLists.txt`: the client
  (`ssh_client_vita`, entry `src/main.c`, links libssh2) and the server
  (`ssh_server_vita`, entry `src/sshd.c`, links libssh with `-DWITH_SERVER`).
  New `.c` files must be added to the right list by hand; nothing globs.
  `src/network.c` is the one source in both.
- Headers live in `include/`, sources in `src/`.
- Script filenames are snake_case — `render_tiles.sh`, `convert_png.py`, `gen_wordlist.py`,
  `smoke.sh`. No hyphenated filenames under `scripts/` or `assets/`: a new tool with a
  multi-word name takes underscores, like every other identifier in the tree.
- Each app packages its own tile: the client's `vita_create_vpk` reads `sce_sys/`, the
  server's reads `sce_sys_sshd/`; both pack to `sce_sys/` inside their own VPK, so only the
  source directory differs. The PNGs are generated — edit the SVG sources in `assets/` and
  re-render with `assets/render_tiles.sh`, never touch the PNGs by hand. That script is the only
  sanctioned way to make a tile: it renders with `rsvg-convert`, hands the 24-bit result to
  `scripts/convert_png.py` (a uv PEP 723 script — `uv run scripts/convert_png.py --check <png>`
  verifies without writing), which quantizes, strips ancillary chunks, and reads the file back
  before renaming it into place. Every tile PNG must come out an **8-bit palette PNG** or the
  installer rejects the whole VPK with `0x8010113D`: colour type 3, bit depth 8, non-interlaced,
  and only `IHDR,PLTE,IDAT,IEND` — ImageMagick's default `tEXt`/`bKGD` chunks are rejected too.
  Check with `file tile.png` → must say `8-bit colormap`; `magick identify` cannot tell you this
  (it reports 8-bit depth per channel and calls a TrueColor render a `Palette` image). Note that
  "-depth 8" alone means 8 bits *per channel*, which looks correct and is not.
- With no `authorized_keys`, the server prints a generated **passphrase** instead: `PASSPHRASE_WORDS`
  words from a 1024-entry table in `src/sshd_hostkey.c`, concatenated with no separators. The table's
  three properties — exactly 2^10 entries (the generator indexes it with 10 random bits, so a short
  table is an out-of-bounds read), no entry a prefix of another (no separators means the reader segments
  it by eye, and a prefix pair renders two different secrets identically), and long enough to keep
  `password[64]` in `include/sshd.h` from truncating — are all checked by
  `python3 scripts/gen_wordlist.py --check`. Never edit the array by hand: edit `scripts/wordlist-pool.txt`,
  run `--emit`, paste, re-check. `--check` fails on a table the pool would not produce.
- In `taskfiles/setup.yml`, the `env -u CPPFLAGS -u LDFLAGS -u PKG_CONFIG_*` around
  libssh2's configure is load-bearing: a host Homebrew OpenSSL leaking into the probe
  passes the "Crypto library: OpenSSL" check, then fails to link against the 1.1.1 Vita
  port. Don't "clean that up".
- SFTP is libssh's stock server in `src/sftp_server.c`, adapted without forking the library. libssh 0.12.2 has no callback surface for overriding handlers (`message_handlers[]` and `process_client_message()` are static), so there are exactly two seams: `-Wl,--wrap` on the libc calls the stock handlers make (listed in `CMakeLists.txt`, server target only, translating through `sh_path.c` only while `active` is set), and `handle_rename()`, which answers a packet in `sh_sftp_data()` before the stock callback sees it. Adding a wrapped symbol means adding both the `__wrap_` function with its `__real_` declaration and the `-Wl,--wrap=` line.
- Opcodes the server overrides, and why. `chmod` and `chown` answer `SSH_FX_OP_UNSUPPORTED` (via `ENOSYS`) because the Vita has no permission or ownership model and a false OK makes the client believe it; `setstat` runs size, chmod, chown, then utimes in that order and stops at the first failure, so a setstat that combines permissions with times fails before the times are applied. `utimes` is honoured for real (sceIoChstat sets times). `truncate` is translated and then read back, answering `ENOSYS` if the size did not change. `statvfs` answers `ENOSYS` (free space is an sceIo devctl, not a statvfs). `stat` and `lstat` normalise every entry to uid 0, gid 0, nlink 1 and a plain file or directory mode (0644/0755, 0444/0555 on read-only devices), so no client resolves a name that does not exist and nothing is ever reported as a symlink; `readlink` and `symlink` fail `ENOSYS` from `src/vita_shims.c`. `RENAME` and `posix-rename@openssh.com` are implemented in `handle_rename()` because the stock server leaves both unsupported: a different device answers `SSH_FX_FAILURE` ("cross-device rename not supported", never `OP_UNSUPPORTED`, which invites a retry by another route, and never a silent copy), a device root or read-only target is refused, plain `RENAME` onto an existing path fails as SFTP v3 requires, and `posix-rename` replaces a file by unlink-then-rename (not atomic) and never replaces a directory. `fstat` and `fsetstat` are left to the stock `process_unsupported`, which already answers honestly.

## Context7 Libraries

- astral-sh/docs
- j178/prek
- jdx/mise
- libssh2/libssh2
- mrlesk/backlog.md 
- openssl/openssl
- websites/taskfile_dev
- websites/vitasdk

<!-- BACKLOG.MD MCP GUIDELINES START -->
<!-- backlog.md-instructions-version: 1.55.0 -->

<CRITICAL_INSTRUCTION>

## BACKLOG WORKFLOW INSTRUCTIONS

This project uses Backlog.md MCP for all task and project management activities.

**CRITICAL GUIDANCE**

- If your client supports MCP resources, read `backlog://workflow/overview` to understand when and how to use Backlog for this project.
- If your client only supports tools or the above request fails, call `backlog.get_backlog_instructions()` to load the tool-oriented overview. Use the `instruction` selector when you need `task-creation`, `task-execution`, or `task-finalization`.

- **First time working here?** Read the overview resource IMMEDIATELY to learn the workflow
- **Already familiar?** You should have the overview cached ("## Backlog.md Overview (MCP)")
- **When to read it**: BEFORE creating tasks, or when you're unsure whether to track work

These guides cover:

- Decision framework for when to create tasks
- Search-first workflow to avoid duplicates
- Links to detailed guides for task creation, execution, and finalization
- MCP tools reference

You MUST read the overview resource to understand the complete workflow. The information is NOT summarized here.

</CRITICAL_INSTRUCTION>

<!-- BACKLOG.MD MCP GUIDELINES END -->

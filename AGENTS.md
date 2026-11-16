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
- Order matters: `task setup` → `task build`. Setup builds the OpenSSL 1.1.1 Vita port, then libssh2 (the client's library), then libssh (the server's library) into `$VITASDK`. libssh2 and libssh both configure against that OpenSSL, so openssl first is required, not a preference.
- Both tasks fingerprint inputs and skip when current. After editing a portlib recipe or lib source, force: `task --force build`, `task --force setup:openssl`, `task --force setup:libssh2`, `task --force setup:libssh`. Each portlib's version is pinned by `pkgver=` in its `libs/<name>/VITABUILD`, which the matching `setup:` task reads; libssh also pins `sha256sums=` there, and `setup:libssh` refuses a tarball that does not match it. A libssh version bump therefore means updating both lines, and then checking that `libs/libssh/vita.patch` still applies, because the recipe re-extracts and re-patches on every run.
- Setup stages ~160 MB of OpenSSL/libssh2/libssh sources under `.task/portlibs` (gitignored). `task clean` wipes `build/` and `.task/` but leaves the libs installed in `$VITASDK`.
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

## libssh portlib (the server's library)

- `libs/libssh/VITABUILD` and `setup:libssh` in `taskfiles/setup.yml` are two copies of one recipe and must be edited together. libssh is static only, built out-of-tree into a wiped directory, staged, then copied into `$VITASDK`, and the task fails if `libssh.a` is missing afterwards (a cmake configure that quietly found no OpenSSL still exits 0).
- These options look redundant or tidy-able and are not; each one has already cost a debugging cycle:
  - `-DBUILD_SHARED_LIBS=OFF` is spelled with the S. `BUILD_SHARED_LIB` is accepted by cmake as an unused variable and quietly builds the shared library anyway.
  - `-DCMAKE_POSITION_INDEPENDENT_CODE=OFF` is not enough on its own. PIC emits `R_ARM_GOT_BREL` relocations and `vita-elf-create` dies with "Invalid relocation type 25". libssh's `cmake/Modules/DefineCMakeDefaults.cmake` hard-sets the variable with a plain `set()`, which outranks the cache, so the command-line flag does nothing until `libs/libssh/vita.patch` makes that `set()` conditional. Drop the patch hunk and the flag stops working with no warning.
  - `-DHAVE_GCC_THREAD_LOCAL_STORAGE=OFF` keeps libssh's log level, log callback and userdata out of `__thread` storage. A thread made with `sceKernelCreateThread` has no TLS block, so `ssh_set_log_level()` and `ssh_set_log_callback()` look like they succeeded while libssh reads level 0 with no callback and logs nothing, ever. The server runs on exactly such a thread.
  - The rest of `vita.patch` exists because the Vita newlib has no `<net/if.h>`, no `<sys/un.h>` and no termios; libssh includes the first two unguarded and the build dies before any guard can help. `if_nametoindex()` and `sockaddr_un` are stubbed to fail at runtime on client-only paths a server never takes.
  - The `env -u CPPFLAGS -u CFLAGS …` scrub and `-DOPENSSL_ROOT_DIR` are both there because cmake reads those variables from the environment and `FindOpenSSL` will happily return Homebrew's `openssl@3`. The task asserts `OPENSSL_CRYPTO_LIBRARY` in `CMakeCache.txt` resolves into `$VITASDK` rather than trusting configure's output. Same reasoning as the libssh2 note below.
  - `-DTHREADS_HAVE_PTHREAD_ARG=NO` is passed by the recipe, but no failure from removing it has been recorded; leave it alone until someone records one.
- libssh's server side is not available to an app by linking `-lssh` alone:
  - `target_compile_definitions(<app> PRIVATE WITH_SERVER)` is required. libssh compiles the `ssh_bind_*` and `sftp_server_*` declarations out of its installed public headers unless `WITH_SERVER` is defined, and it defines that only in its own private `config.h`, so a consumer without it gets no `ssh_bind_*` and no `sftp_server_*`.
  - The libraries are `ssh`, `ssl`, `crypto`, `z` (libssh's compression path) and `pthread` (libssh's `pthread.c`; the SDK ships `libpthread.a`), plus `SceAppMgr_stub` and `SceCommonDialog_stub`. The last two look unrelated to SSH: vita2d's shared-framebuffer path references them, and omitting either breaks the link. `ScePower_stub` is the idle-timer tick that keeps the device awake while the server runs. `png` and `bz2` are deliberately absent from the server, since nothing there pulls vita2d's image loaders.
  - libssh2 and libssh never mix: the client links only libssh2, the server only libssh.

## Source quirks

- Two apps, two `add_executable` lists in `CMakeLists.txt`: the client (`ssh_client_vita`, entry `src/main.c`, links libssh2) and the server (`ssh_server_vita`, entry `src/sshd.c`, links libssh with `-DWITH_SERVER`). New `.c` files must be added to the right list by hand; nothing globs, and a file missing from both lists is never compiled and never reports an error. The client list is `main.c`, `input.c`, `network.c`, `ssh_client.c`, `shell.c`, `text.c`, `menu.c`. The server list is `sshd.c`, `sshd_session.c`, `sshd_builtins.c`, `sshd_hostkey.c`, `sftp_server.c`, `sh_path.c`, `vita_shims.c`, `network.c`. `src/network.c` is the one source in both.
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
- Opcodes the server overrides, and why. `chmod` and `chown` answer `SSH_FX_OP_UNSUPPORTED` (via `ENOSYS`) because the Vita has no permission or ownership model and a false OK makes the client believe it; `setstat` runs size, chmod, chown, then utimes in that order and stops at the first failure, so a setstat that combines permissions with times fails before the times are applied. The one exception is `handle_perm_setstat()`: a `SETSTAT` or `FSETSTAT` that carries only permission bits and asks for exactly the mode already reported (0644 for a file, 0755 for a directory) is answered OK, because that leaves the file as the client believes it is and it is what scp sends after every upload; an `FSETSTAT` names a handle, not a path, so it is compared against the file mode. Any other request, including one with times, still answers unsupported. `utimes` is honoured for real (sceIoChstat sets times). `truncate` is translated and then read back, answering `ENOSYS` if the size did not change. `statvfs` answers `ENOSYS` (free space is an sceIo devctl, not a statvfs). `stat` and `lstat` normalise every entry to uid 0, gid 0, nlink 1 and a plain file or directory mode (0644/0755, 0444/0555 on read-only devices), so no client resolves a name that does not exist and nothing is ever reported as a symlink; `readlink` and `symlink` fail `ENOSYS` from `src/vita_shims.c`. `RENAME` and `posix-rename@openssh.com` are implemented in `handle_rename()` because the stock server leaves both unsupported: a different device answers `SSH_FX_FAILURE` ("cross-device rename not supported", never `OP_UNSUPPORTED`, which invites a retry by another route, and never a silent copy), a device root or read-only target is refused, plain `RENAME` onto an existing path fails as SFTP v3 requires, and `posix-rename` replaces a file by unlink-then-rename (not atomic) and never replaces a directory. `fstat` and `fsetstat` are left to the stock `process_unsupported`, which already answers honestly.
- SFTP transport rules, each learned the hard way on a device. (1) Nothing may block inside libssh from a callback: the session is attached to the poll event, and a blocking `ssh_channel_write` waits by polling the shared event context from inside the poll loop that called it, which stalls the session and strands others. So `sh_sftp_new()` sets the session non-blocking, `--wrap=ssh_channel_write` queues the stock handlers' replies, `drain_out()` hands libssh only what `ssh_channel_window_size()` allows, new requests wait while more than 128 KB of replies are queued, and `sh_sftp_pump()` runs on every poll-loop pass from `sh_session_pump()`. (2) `--wrap` also rewrites calls made inside libc itself: newlib's `opendir` is `open(path, O_DIRECTORY)`, so `__wrap_opendir` clears `active` around the real call or the already translated path is translated again and fails `ENOTDIR`. Any new wrapper that calls into libc has to ask whether libc calls another wrapped function. (3) Ending a session: on client EOF the server sends exit-status 0, EOF and close, then waits (3 s at most) for the channel to be closed before the transport is torn down; disconnecting early makes the client's ssh exit 255 and scp reports a failed copy. (4) libssh formats `ls -l` names with `ctime_r()` and passes the result to `asctime_r()` unchecked, and newlib's `localtime_r` returns NULL for a time sceRtc rejects, so every stat handed to libssh goes through `usable_time()`.

## Server platform rules

The layering, bottom to top, is `sh_path.c` (POSIX path to device path), `sshd_builtins.c` (the command table), `sshd_session.c` (one SSH session), `sshd.c` (net bring-up, accept loop, poll loop, console). Each rule below comes from the platform, not from taste, and each looks like something worth "fixing".

- **One thread, one poll loop.** Everything (vita2d, the pad, the sockets, every session) is driven from `server_thread()`, so nothing is locked and nothing may block. There is no fork, so a command runs inside the server: the line editor only marks a line complete and `sh_session_pump()` runs it from the loop, never from a libssh callback. A running command cannot be interrupted by Ctrl-C, because noticing the keystroke needs the socket the command is blocking; walks are bounded by `MAX_DEPTH` and read limits instead. The thread is created with an explicit stack because a homebrew main thread's is too small for libssh's crypto and the tree walks.
- **One accept per readable-listening-fd event.** `accept()` is called by hand, once, in the poll callback (so the peer address is known before the handshake). Never turn it into a drain loop: the listening fd is blocking, so the second `accept()` with nothing queued parks the whole loop.
- **Callbacks return zero to accept.** For the pty, shell, exec and subsystem requests, 0 accepts and nonzero makes libssh send the failure. The service-request callback accepts the name `"ssh-userauth"` (not `"ssh-connection"`, which is only named inside the later userauth request); returning nonzero there makes libssh disconnect with code 11 and no error string, before any auth callback runs. Auth callbacks return `SSH_AUTH_SUCCESS` / `SSH_AUTH_DENIED`, not 0/1.
- **No `printf`, `puts` or `stdout`.** There is no terminal; libc stdout goes to the Vita's debug screen, not to the user. A builtin writes to its own channel with `sh_printf()` (stdout, redirectable with `>` / `>>`), `sh_eprintf()` (errors, always the terminal) or `sh_write()`. The Vita's own screen and `sshd.log` get `sh_console_log()`. `snprintf` into a buffer is fine.
- **The working directory is per session.** libc's cwd is a single static in newlib's `lib_a-fs.o` shared by the whole process, so one session calling `chdir()` would move every other session. Never call `chdir()` or `getcwd()`. A session's cwd and home live in `sh_fsctx_t`, read with `sh_session_get_fsctx()`; only `cd` and session setup write it, through `sh_session_set_cwd()`.

## The path layer

`include/sh_path.h` is the only place allowed to know how a POSIX path becomes a Vita device path (`/ux0/data/x` becomes `ux0:data/x`). The Vita has no unified tree, no root, and no symlinks; `sceIo*` understands only `dev:` prefixes.

- Builtins and SFTP handlers speak POSIX (`/ux0/...`, `~`, relative to the session cwd). Resolve with `sh_path_resolve()` or `sh_path_sce()`, and pass the resulting `sce` string to the libc call (`open`, `stat`, `opendir`, `unlink`, ...), which newlib maps onto `sceIo*`. Never build a `ux0:` string by hand.
- Normalise in POSIX space and translate once, immediately before the file call. `..` is applied against the component stack and clamps at the device root, so a relative path can never leave its device. Normalising a `dev:` path instead is where `ux0:../vs0:` bugs live, because the sceIo spelling has no root to stop at. Never translate a path twice either: translating an already translated path is the cause of the `opendir` bug in the SFTP notes above.
- An unknown first component is `ENOENT`. Never fall back to a default device, because that turns a typo into a file on the wrong device. `/` is a synthetic listing of the device table (`SH_TARGET_ROOT`), not a directory: `sh_path_sce()` fails with `ENOTSUP` for it, and `cd /` is special-cased in `cmd_cd`.
- No symlink faking. There are none, so `readlink` and `symlink` fail `ENOSYS` and `lstat` never reports one; do not add a command that pretends otherwise.
- Check `dev->writable` before any mutation (`check_writable()` in `sshd_builtins.c`), so a write to `/os0` says "Read-only file system" instead of an opaque `sceIo` code. The table marks a device writable when unsure, because an error the user can read beats a false refusal they cannot work around.
- Removable devices (`sd0`, `uma0`, `xmc0`, ...) exist only while something is in them; `sh_device_present()` probes them.
- The helpers `sh_basename()`, `sh_join()`, `sh_parent()` and `sh_is_dot()` are pure string functions with no syscalls; use them rather than `strrchr`. Buffers are `SH_POSIX_PATH_MAX` / `SH_SCE_PATH_MAX` (1024), and every function returns `ENAMETOOLONG` rather than truncating.

## Platform metadata quirks

- **Directories are identified by `stat()` and `S_ISDIR(st.st_mode)`.** Newlib's `struct dirent` here has no `d_type` (so no `DT_DIR`); it carries `d_stat`, a `SceIoStat`, which nothing in this tree reads. Every directory walk calls `stat()` on the entry's resolved path.
- **There is no usable path-length constant.** `PATH_MAX` is not defined by `<limits.h>` plus `<dirent.h>` here, and `NAME_MAX` only arrives through `<sys/param.h>` (the sources carry an `#ifndef NAME_MAX` / `#define NAME_MAX 255` fallback; 255 is the POSIX minimum and FAT's long-name limit). Use `SH_POSIX_PATH_MAX` and `SH_SCE_PATH_MAX` for paths and `NAME_MAX + 1` for a single name, and keep buffers on the heap or small: a Vita thread's stack is whatever `sceKernelCreateThread` was given.
- **There are no permission bits, owners or link counts.** FAT `stat()` fills `st_mode` with the file type and nothing else, so formatting it directly prints nine dashes. `synth_mode()` in `sshd_builtins.c` builds 0755/0644 from the type, and drops the write bits on a read-only device; `normalise_stat()` in `sftp_server.c` does the same for SFTP (see the SFTP notes above).
- **Times can be unrepresentable.** `localtime_r()` returns `NULL` for a time `sceRtc` rejects (a synthetic stat's 0, a FAT entry with no date) and libssh does not check it. Pass times through `usable_time()` before they leave the server.
- **Free space is a devctl, not `statvfs`.** `df` uses `sceIoDevctl` with `0x3001` (`DEVCTL_GET_DEV_INFO`) and `psp2/io/devctl.h`; `statvfs` answers `ENOSYS`.
- **`strerror()` on a `sceNet` failure** may be a `0x80......` code that newlib cannot name; log the number, not just the text.
- The POSIX surfaces libssh references that the platform lacks (`dup2`, `waitpid`, uid/gid lookups, symlinks) are stubbed to fail `ENOSYS` in `src/vita_shims.c`. A new undefined reference at link time belongs there if it is a genuine absence, and gets a failure, not a fake success. The calls the Vita does have (`open`/`read`/`stat`/`opendir`/`rename`/`socket`/`poll`...) are NOT shimmed; newlib already maps them.

## Adding a builtin command

1. Write `static int cmd_<name>(sh_session_t *s, int argc, char **argv)` in `src/sshd_builtins.c`. It runs inside the server's poll loop: return an exit status (0 success, nonzero failure), keep it bounded, and never block on the network or sleep.
2. Resolve every path argument through `resolve_arg(s, "<name>", arg, &t)`, which fills an `sh_target_t` and prints the failure in the command's own voice. Use `t.sce` for the libc call, and `stat()` plus `S_ISDIR` for the type. Call `check_writable()` before anything that creates, changes or removes. For a path-less default use the cwd by passing `NULL`; bare `cd` passes `"~"` for home.
3. Output goes through `sh_printf()`, errors through `sh_eprintf()` in the form `<name>: <arg>: <strerror(errno)>`. No `printf`, no `chdir()`, no hand-built `ux0:` strings. A long walk checks `sh_session_interrupted()` and respects `MAX_DEPTH`.
4. Add a row to the `builtins[]` table at the bottom of the file, in alphabetical order (`help` prints the table in order). The row is `{"name", cmd_name, "usage -- one-line description"}`. Nothing else registers it, and `src/sshd_builtins.c` is already in the server's `add_executable` list.
5. Run `task build`, or `cmake --build build --target ssh_server_vita` for the server alone (see Build above). Compile and link is the only check available here; behaviour is verified on a device.

## Context7 Libraries

- astral-sh/docs
- j178/prek
- jdx/mise
- libssh2/libssh2
- websites/api_libssh_stable
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

# AGENTS.md

## Provenance

- Standalone repo, not a fork. It began as a fork of Rompelhd's VitaSSH
  (<https://github.com/rompelhd/VitaSSH>), whose history was dropped when this became its own
  project; packaging and installation also follow TheOfficialFloW's VitaShell. Both are
  credited in README.md. Upstream is effectively unmaintained and is not a seam to keep
  clean: this repo is canonical, so fix cruft rather than working around it.

## Machine-local context

- Personal and environment identifiers — the maintainer's name, the Vita endpoints, the host key fingerprint, the devbox hostnames — live in the gitignored `CLAUDE.local.md` and nowhere else. Read it before any device run; if it is missing, ask for the endpoint rather than guessing one.
- Never write those values into tracked files, backlog task notes and commit messages included. Tasks refer to them generically (`the user`, `the home Vita`, `the work Vita`, `devbox A`/`devbox B`) and point at `CLAUDE.local.md` for the real values.

## Build

- `task` (Taskfile, <https://taskfile.dev>) is the entry point — never run `cmake` directly.
  Bare `cmake` dies with "Please define VITASDK" because CMakeLists.txt reads it from the
  environment. `task` with no args lists tasks.
- Order matters: `task setup` → `task build`. Setup builds the OpenSSL 1.1.1 Vita port, then libssh2 (the client's library), then libssh (the server's library) into `$VITASDK`. libssh2 and libssh both configure against that OpenSSL, so openssl first is required, not a preference.
- Both tasks fingerprint inputs and skip when current. After editing a portlib recipe or lib source, force: `task --force build`, `task --force setup:openssl`, `task --force setup:libssh2`, `task --force setup:libssh`. Each portlib's version is pinned by `pkgver=` in its `libs/<name>/VITABUILD`, which the matching `setup:` task reads; libssh also pins `sha256sums=` there, and `setup:libssh` refuses a tarball that does not match it. A libssh version bump therefore means updating both lines, and then checking that `libs/libssh/vita.patch` still applies, because the recipe re-extracts and re-patches on every run.
- Setup stages ~160 MB of OpenSSL/libssh2/libssh sources under `.task/portlibs` (gitignored). `task clean` wipes `build/` and `.task/` but leaves the libs installed in `$VITASDK`.
- The host-runnable checks are `python3 -I scripts/test_vpk_zip.py` (zip fixtures against `src/vpk_zip.c`, pure C plus zlib), `python3 -I scripts/test_vpk_head.py` (`src/vpk_head.c` against an independent Python transcription of VitaShell's `makeHeadBin`), and `python3 -I scripts/test_vpk_updater.py` (one-shot updater handoff, targeted shutdown, failure logging and relaunch with stubbed Vita APIs), `python3 -I scripts/test_sh_grep.py` (`src/sh_grep.c` under ASan/UBSan: BRE matching, the bounded line reader, and the step-budget refusal), and `python3 -I scripts/test_sh_headtail.py` (`src/sh_headtail.c` under ASan/UBSan: the line and byte windows of head and tail, checked against a naive reference written from the specification and pinned against real `ghead`/`gtail`, over every string up to nine bytes from `{a, b, newline}`, at chunk boundaries, and through the chunk-at-a-time sink). `python3 -I scripts/test_sh_textops.py` (`src/sh_textops.c` under ASan/UBSan: `sh_wc`, `sh_sort` and `sh_uniq` and the `sh_cmp` comparison, checked against naive references written from the specification and pinned against real `gwc`/`gsort`/`guniq`, over every string up to six bytes from `{a, b, newline, space, 1, -}` for every flag combination, through short 7-byte fills and failing sources and stopping sinks). Its `-u` rule — one line per run of equal keys on the already sorted list, comparing keys without the byte tie-break, with the survivor the first of the run in sorted order — was pinned against `gsort` on the cases where a transitive group-by-key (keep the earliest input line) silently differs, so it must not be "tidied" into the transitive form. It is run under `timeout` too, since the backwards walk in head/tail and the sort's own merge can spin rather than fail. Every host binary here is run under `timeout`, and exit 124 is a failure, not a pass. Everything else has no tests, lint, or CI. A successful `task build` producing both
  `build/ssh_client_vita.vpk` and `build/ssh_server_vita.vpk` is the only verification.
  Output is arm-vita-eabi (cross-compiled); nothing here builds or runs on the host.
- The server is at the size ceiling `vita-elf-create` enforces, and it fails as an abort, not a link error: `Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps`. Nothing in the message says "too big", so a new builtin that trips it reads as a broken toolchain. Measured on the server's own ELF: the limit sat 3284 bytes past a clean build's text, and `head` plus `tail` cost 3474 (3404 before the multi-file header loop). That is why `src/sshd_builtins.c` alone is compiled `-Os` (`set_source_files_properties` in CMakeLists.txt) — code layout only, and it buys back more than a command costs. Remove that line and the next command added fails the build. `arm-vita-eabi-size build/ssh_server_vita` is the reading to watch.
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

## libcurl portlib (HTTPS downloads; not linked into an app yet)

- `libs/curl/VITABUILD` and `setup:curl` in `taskfiles/setup.yml` are two copies of one recipe and must be edited together, including the `ac_cv_*` overrides. `pkgver=` and `sha256sums=` are read by the task, and it refuses a tarball that does not match. Only `lib/` and `include/` are built and installed (no curl tool), http and https only, static only. `setup:curl` is part of `setup:portlibs`, and `libcurl.a` is a source of `task build`.
- The version is pinned at 8.17.0 because curl removed OpenSSL 1.1.1 support in 8.18.0 (its `docs/DEPRECATE.md`) and the SDK's OpenSSL port is 1.1.1. A bump past 8.17.x means moving the OpenSSL port first.
- Each override looks tidy-able and is not:
  - `ac_cv_header_stdatomic_h=no`: the SDK's `stdatomic.h` is clang's and uses `__c11_atomic_*` builtins gcc lacks, so `easy_lock.h` fails to compile.
  - `ac_cv_func_pipe2=no ac_cv_func_pipe=no ac_cv_func_eventfd=no`: configure's checks only link, the Vita libc has stubs, and the headers do not declare `pipe2`.
  - `curl_disallow_socketpair=yes`: `--disable-socketpair` only defines `CURL_DISABLE_SOCKETPAIR` and the link check still sets `HAVE_SOCKETPAIR`, so with unix sockets off `socketpair.h` stops at `#error "unsupported Unix domain and socketpair build combo"`. The `ac_cv_func_socketpair` cache variable is not what that check reads.
  - The `env -u CPPFLAGS ... PKG_CONFIG_*` scrub and `--with-openssl=$VITASDK/arm-vita-eabi` are the same Homebrew-OpenSSL defence as libssh2. The task asserts `USE_OPENSSL` in `lib/curl_config.h` and that no host path reached the flags rather than trusting configure's output.
- Settled on a device (work Vita, 2026-11-17, scratch probe on a 1 MB-stack thread, network brought up with `src/network.c`):
  - Name resolution: the blocking resolver is right. libc `getaddrinfo()` and `gethostbyname()` both resolve through sceNet with no extra setup, so no threaded resolver or c-ares is needed.
  - CA store: libcurl built without a CA path fails every HTTPS request with `unable to get local issuer certificate (20)`, so `CURLOPT_CAINFO` must always be set. `vs0:data/external/cert/CA_LIST.cer` (315 KB, concatenated PEM with `#SCEI Root CA` comment lines) verified `https://github.com/` with HTTP 200, so the system store needs nothing shipped. curl's own `cacert.pem` (200 KB) in `ux0:data/` also verified, and is the fallback if a firmware lacks the system file.
  - `curl_global_init()` and a TLS handshake against OpenSSL 1.1.1t run fine on a `sceKernelCreateThread` thread.
- Nothing links libcurl yet. An app that does will need `curl ssl crypto z` plus `SceNet_stub` and `SceNetCtl_stub`, and must not also link libssh2 (libcurl is built without it, so the two do not mix).

## Source quirks

- Two user apps and one updater, with explicit `add_executable` lists in `CMakeLists.txt`: the client (`ssh_client_vita`, entry `src/main.c`, links libssh2), the server (`ssh_server_vita`, entry `src/sshd.c`, links libssh with `-DWITH_SERVER`), and the minimal updater (`ssh_updater_vita`, `src/sshd_updater.c` plus `src/vpk_promote.c`). New `.c` files must be added to the right list by hand; nothing globs. The client list is `main.c`, `input.c`, `network.c`, `ssh_client.c`, `shell.c`, `text.c`, `menu.c`. The server list is `sshd.c`, `sshd_session.c`, `sshd_builtins.c`, `sshd_hostkey.c`, `sftp_server.c`, `sh_path.c`, `sh_grep.c`, `sh_headtail.c`, `sh_textops.c`, `sh_stdin.c`, `vita_shims.c`, `network.c`, `sshd_vpk.c`, `vpk_promote.c`, `vpk_zip.c`, `vpk_head.c`, plus the generated `build/vpk_head_template.c`. `src/network.c` is shared by client and server; only server and updater share `src/vpk_promote.c`.
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
- Opcodes the server overrides, and why. `chmod` and `chown` answer `SSH_FX_OP_UNSUPPORTED` (via `ENOSYS`) because the Vita has no permission or ownership model and a false OK makes the client believe it; `setstat` runs size, chmod, chown, then utimes in that order and stops at the first failure, so a setstat that combines permissions with times fails before the times are applied. The one exception is `handle_perm_setstat()`: a `SETSTAT` or `FSETSTAT` that carries only permission bits and asks for exactly the mode already reported (0644 for a file, 0755 for a directory) is answered OK, because that leaves the file as the client believes it is and it is what some clients send after a copy (no client has been observed sending it; OpenSSH 10.6's `scp -s` does not, see below); an `FSETSTAT` names a handle, not a path, so it is compared against the file mode. What `scp -s` actually sends after an upload is an `FSETSTAT` carrying only `SSH_FILEXFER_ATTR_SIZE` (flags 1, a 64-bit size, no permission bits, whatever the source file's mode), read off the wire on a device; `handle_size_fsetstat()` answers it OK when the open file already has that size. libssh keeps the handle's fd in a private struct, so the handler mirrors the head of `struct sftp_handle` from the pinned 0.12.2 and trusts the fd only when it is one of the server's own open files, else it falls back to unsupported; a libssh bump means rechecking that layout. Any other request, including one with times or a size that differs, still answers unsupported. A write, mkdir, rmdir or unlink on a read-only device answers `SSH_FX_PERMISSION_DENIED`: `writable_target()` sets `EACCES`, because libssh's `unix_errno_to_ssh_stat()` maps only `EACCES`/`EPERM` to that status and answers `EROFS` with a bare `SSH_FX_FAILURE`, which clients print as "Failure". Rename gets the same status from its own `EROFS` case in `reply_rename_error()`. `utimes` is honoured for real (sceIoChstat sets times). `truncate` is translated and then read back, answering `ENOSYS` if the size did not change. `statvfs` answers `ENOSYS` (free space is an sceIo devctl, not a statvfs). `stat` and `lstat` normalise every entry to uid 0, gid 0, nlink 1 and a plain file or directory mode (0644/0755, 0444/0555 on read-only devices), so no client resolves a name that does not exist and nothing is ever reported as a symlink; `readlink` and `symlink` fail `ENOSYS` from `src/vita_shims.c`. `RENAME` and `posix-rename@openssh.com` are implemented in `handle_rename()` because the stock server leaves both unsupported: a different device answers `SSH_FX_FAILURE` ("cross-device rename not supported", never `OP_UNSUPPORTED`, which invites a retry by another route, and never a silent copy), a device root or read-only target is refused, plain `RENAME` onto an existing path fails as SFTP v3 requires, and `posix-rename` replaces a file by unlink-then-rename (not atomic) and never replaces a directory. `fstat` and every `fsetstat` not described above are left to the stock `process_unsupported`, which already answers honestly.
- SFTP transport rules, each learned the hard way on a device. (1) Nothing may block inside libssh from a callback: the session is attached to the poll event, and a blocking `ssh_channel_write` waits by polling the shared event context from inside the poll loop that called it, which stalls the session and strands others. So `sh_sftp_new()` sets the session non-blocking, `--wrap=ssh_channel_write` queues the stock handlers' replies, `drain_out()` hands libssh only what `ssh_channel_window_size()` allows, new requests wait while more than 128 KB of replies are queued, and `sh_sftp_pump()` runs on every poll-loop pass from `sh_session_pump()`. (2) `--wrap` also rewrites calls made inside libc itself: newlib's `opendir` is `open(path, O_DIRECTORY)`, so `__wrap_opendir` clears `active` around the real call or the already translated path is translated again and fails `ENOTDIR`. Any new wrapper that calls into libc has to ask whether libc calls another wrapped function. (3) Ending a session: on client EOF the server sends exit-status 0, EOF and close, then waits (3 s at most) for the channel to be closed before the transport is torn down; disconnecting early makes the client's ssh exit 255 and scp reports a failed copy. (4) libssh formats `ls -l` names with `ctime_r()` and passes the result to `asctime_r()` unchecked, and newlib's `localtime_r` returns NULL for a time sceRtc rejects, so every stat handed to libssh goes through `usable_time()`. (5) `ssh_channel_write` runs libssh's packet loop, so the client's next request can reach `sh_sftp_data()` in the middle of `drain_out()`; `drain_out()` sets `busy` while it writes so that request is only queued, because a nested `drain_out()` moves `out_off` under the outer call and bytes go out twice while others are skipped (a download desynchronises past roughly 10 MB with no error logged).

## Server platform rules

The layering, bottom to top, is `sh_path.c` (POSIX path to device path), `sshd_builtins.c` (the command table), `sshd_session.c` (one SSH session), `sshd.c` (net bring-up, accept loop, poll loop, console). Each rule below comes from the platform, not from taste, and each looks like something worth "fixing".

- **One thread, one poll loop.** Everything (vita2d, the pad, the sockets, every session) is driven from `server_thread()`, so nothing is locked and nothing may block. There is no fork, so a command runs inside the server: the line editor only marks a line complete and `sh_session_pump()` runs it from the loop, never from a libssh callback. A running command cannot be interrupted by Ctrl-C, because noticing the keystroke needs the socket the command is blocking; walks are bounded by `MAX_DEPTH` and read limits instead. The thread is created with an explicit stack because a homebrew main thread's is too small for libssh's crypto and the tree walks.
- **A pipeline is sequential, not streaming.** `a | b | c` runs the segments one after another inside the one poll-loop pass: each segment but the last runs with `sh_write()` captured into a memory buffer (`sh_session_capture_start/_end`), and `sh_session_capture_end()` makes it the next segment's stdin. Both the capture and stdin are bounded by `SSHD_STDIN_MAX` (1 MB); a segment that produces more fails the pipeline with `sh: <cmd>: output too large for a pipe (limit N bytes)` and the later segments do not run. The `|` word must be a separate, unquoted word like `>` and `<`: a glued `a|b` stays one word and a quoted `"|"` is an argument. A `>` redirect on a segment is checked before the capture, so it wins and the next segment reads an empty stdin; `sh_eprintf` is never captured. A segment other than the last cannot change session state, as in a subshell: `exit` is skipped and the cwd is restored after `cd`. The status is the last segment's, and an empty segment (`| x`, `x |`, `x | | y`) is a syntax error (status 2) that runs nothing. The capture does not make a slow segment yield: a CPU- or I/O-bound builtin such as `du /ux0` (about 170 s on a device) starves every other session whether or not it sits in a pipeline, so a new long-running builtin has to yield to `sh_server_poll_slice()` itself.
- **An exec session ignores the client's EOF.** `ssh host cmd` with no stdin sends EOF right behind the exec request, and treating it as the end of the session makes `sh_session_interrupted()` stop the command after its first chunk (`cat` of a large file came back as exactly 64 KB). Only sftp and shell sessions finish on EOF.
- **A command's output is paced by the client's window and by the socket.** A blocking `ssh_channel_write` waits with no timeout, twice over: for the client's window, and in its closing `ssh_channel_flush()` for the TCP socket to drain, which the Vita's small socket buffer reaches long before the client's ~2 MB window closes (a stalled `cat` parked the server for 11 minutes in a device run, accepting connections whose commands never ran). So `sh_session_pump()` sets the session non-blocking while a command runs (`in_pump`), and `chan_write_raw()` writes at most `ssh_channel_window_size()` bytes, first waiting in `wait_for_room()` until the window is open and `ssh_get_status()` shows no `SSH_WRITE_PENDING`. The wait runs `sh_server_poll_slice()` (one `ssh_event_dopoll` of 50 ms) so every session's socket is still read; memory held is one chunk plus what libssh already framed, not the output size. An exec command waits for libssh's buffer to empty after queueing exit status and EOF, because the session is freed right after: without it the tail of the output is cut off. A client that stays stalled for `WINDOW_STALL_S` (30 s), or whose session closed, marks the session finished, which stops the command and tears the session down. While one command waits, other sessions' commands and sftp pumps run only when it finishes. The wait is only legal from the pump: writes from a libssh callback (banner, line-editor echo) keep the blocking session and plain write, because `ssh_event_dopoll` is not reentrant from inside its own callbacks. sftp is unaffected; it paces itself in `drain_out()`.
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
3. Output goes through `sh_printf()`, errors through `sh_eprintf()` in the form `<name>: <arg>: <strerror(errno)>`. The dispatcher speaks for the shell itself with an `sh:` prefix instead: `sh: <name>: command not found` (status 127), `sh: syntax error near unexpected token `|'` (status 2), and `sh: <file>: <reason>` for a redirect or `<` target. A builtin never prints those. No `printf`, no `chdir()`, no hand-built `ux0:` strings. A long walk checks `sh_session_interrupted()` and respects `MAX_DEPTH`.
4. Add a row to the `builtins[]` table at the bottom of the file, in alphabetical order (`help` prints the table in order). The row is `{"name", cmd_name, "usage -- one-line description"}`. Nothing else registers it, and `src/sshd_builtins.c` is already in the server's `add_executable` list.
5. Run `task build`, or `cmake --build build --target ssh_server_vita` for the server alone (see Build above). Compile and link is the only check available here; behaviour is verified on a device.
6. If the command needs the network, read [`docs/vcmd.md`](docs/vcmd.md) first. `vcmd` is the only builtin that opens a socket and is the working example of doing it without freezing the poll loop: copy its `net_wait()` shape (zero-timeout `poll()` plus `sh_server_poll_slice()`, a hard deadline, non-blocking connect/send/receive, `getsockopt(SO_ERROR)` after a *successful* writability wait) rather than writing a bare `connect()`/`recv()`. That file also lists its error strings, saying which were confirmed on a device and which are source-only, and documents the plugin command surfaces it drives on ports 1338/1339.

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

## `vpk install` (the server's self-update)

- The server is linked **UNSAFE** (`vita_create_self(... UNSAFE)`); a safe-mode app cannot see `ux0:app`, `ux0:patch` or the device info `df` reads. UNSAFE is also why the server now binds port 22 instead of falling back to 2222.
- `ux0:app/<TITLEID>` is read-only to the app even when UNSAFE, and a copy placed in `ux0:patch/<TITLEID>` is not used at launch. Both were tried on a device. The only route that installs is `scePromoterUtilityPromotePkgWithRif` on an unpacked package, which is what `src/sshd_vpk.c` does; do not reintroduce a file-copy install.
- The promoter refuses a running target (`0x80101114`). The server bundles `updater.vpk`
  (ROM000072, distinct from the earlier ROM000071 test bubble), stages the server in
  `ux0:data/vitasshd-update/server`, promotes the helper, closes the command's SSH channel,
  launches the helper and immediately exits. The helper targets only ROM000070 for shutdown,
  promotes the stage, records `result.txt`, and relaunches the server even on failure.
  `include/vpk_update.h` is the shared path contract. Never reuse VitaShell's `ux0:data/pkg`
  or the host-key/authorized_keys directory for staging. The `ready` marker is one-shot:
  manually launching the updater must not replay an installation.
- The server VPK has an explicit dependency on the helper VPK target as well as its own
  SELF target; retain both so a helper change is bundled in the same build pass.
- The promoter needs `sce_sys/package/head.bin` in the package directory. Its template is `third_party/vitashell/head.bin` (VitaShell's, GPL-3.0, not covered by the repo's Unlicense), compiled in through `build/vpk_head_template.c`, which CMake generates at configure time. Do not edit the template, and keep its licence file next to it.
- `opendir()` on a missing path answers `ENOTDIR`, not `ENOENT`: ask `stat()` whether something exists.
- `sh_printf` does not understand `%zu`; cast to `unsigned` and use `%u`.
- A rebuild needs a new `__DATE__ __TIME__` to be told apart on a device: `touch` is not enough because the task fingerprint ignores it, so `task --force build`, then read the `sftp: server code built` line in `sshd.log` (written when the first SFTP session starts).

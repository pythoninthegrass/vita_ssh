# VitaSSH

SSH in both directions on the PlayStation Vita. One tree, one build command, two
homebrew apps that install side by side on the same memory card:

| App | Tile | TitleID | Built on | What it is |
| --- | --- | --- | --- | --- |
| `ssh_client_vita` | VitaSSH | `ROM000069` | libssh2 | Connects **out** to any SSH server, with a vita2d terminal and saved profiles |
| `ssh_server_vita` | VitaSSHd | `ROM000070` | libssh | Listens for connections **in** from a devbox and gives you a shell over the Vita's devices |

Both are experimental. The client is the older, better-travelled half; the server is the
half that makes the device worth logging into, and it is new — read its status note before
planning on it.

## Screenshot

<img width="960" height="544" alt="VitaSSH client terminal" src="https://github.com/user-attachments/assets/6dd46da1-1ae0-4790-9b9a-85d6aa143d9d" />

*The client's interactive shell: `neofetch` on an Alpine box over a pty session. The banner
block at the top is the terminal's limits showing — see [Terminal](#terminal).*

---

## Client — `ssh_client_vita` (`ROM000069`)

A libssh2 client with its own terminal renderer. Pick or define a profile, connect over
WiFi, and run commands on the remote host from the Vita.

- **Connect** to any SSH server that takes a password, on any port you type in.
- **Two ways to run something**: <kbd>○</kbd> sends one command over an exec channel,
  <kbd>△</kbd> opens an interactive pty session (`TERM=xterm`) with Ctrl-C, Tab, arrow
  keys on the shoulders, and a resize that tells the server.
- **Profiles**: up to 10, edited through the on-screen keyboard, kept in
  `ux0:data/vitassh/profiles.dat`.
- **Command history**: the last 32 single commands, kept in `ux0:data/vitassh/history.dat`.
  With the terminal showing, <kbd>↑</kbd> <kbd>↓</kbd> walk the list newest-first and
  <kbd>○</kbd> or <kbd>✕</kbd> opens the keyboard prefilled with the selected command.
- **Network**: sceNet and sceNetCtl bring-up with the state reported to the terminal, so a
  radio that never came up is not mistaken for a server that is down.

### Controls

| Where | Input | Does |
| --- | --- | --- |
| Main menu | <kbd>↑</kbd> <kbd>↓</kbd> <kbd>✕</kbd> | Profiles · About · Exit; <kbd>START</kbd> exits |
| Profiles | <kbd>△</kbd> | New profile (IME: host, port, user, password) |
| Profiles | <kbd>✕</kbd> | Connect with the selected profile |
| Profiles | <kbd>□</kbd> | Edit the selected profile |
| Profiles | <kbd>START</kbd> | Delete the selected profile (press twice) |
| Terminal | <kbd>△</kbd> | Interactive shell |
| Terminal | <kbd>○</kbd> or <kbd>✕</kbd> | One command; the keyboard opens prefilled with the selected history entry |
| Terminal | <kbd>↑</kbd> / <kbd>↓</kbd> | Older / newer command in history |
| Terminal | touch drag | Scroll the output |
| Terminal | back touch | Jump to the bottom |
| Terminal | <kbd>START</kbd> | Leave the shell, or leave to the menu and disconnect |
| Shell | <kbd>△</kbd> | Type a line to the pty |
| Shell | <kbd>□</kbd> | Special-key menu (Ctrl-C, Tab, arrows) |
| Shell | <kbd>✕</kbd> | Ctrl-C |
| Shell | <kbd>○</kbd> | Tab |
| Shell | <kbd>L</kbd> / <kbd>R</kbd> | Up / Down |
| Shell | <kbd>SELECT</kbd> | Resize the terminal |

### Terminal

A 120×24 cell grid drawn with vita2d and the Vita's PGF font. It implements the CSI subset
that ordinary command output needs: cursor movement and positioning, erase line and screen,
SGR colours including bold and bright, save and restore cursor. Scrollback is 1000 lines
with a scrollbar, driven by touch.

What that deliberately leaves out is what full-screen programs ask for — see
[known issues](#known-issues).

### Where it is limited

Honest list, because the useful part of a README is telling you what will disappoint you:

- **Password authentication only.** No public key auth, and no verification of the
  server's host key either: the client authenticates to whatever completed the handshake.
  Do not log in over a network you do not trust. Tracked in `TASK-005`.
- **No full-screen programs.** `vim`, `top` and pagers redraw over themselves because the
  alternate screen and scroll regions are not implemented. `TASK-007`.
- **Fonts, not Unicode.** Output is drawn from the PGF font, so the ASCII range is
  dependable and box-drawing characters, many line-drawing glyphs and non-Latin scripts are
  not — the garbled banner in the screenshot above is exactly this.
- **Profiles store the password in plaintext** on the memory card, next to the host and
  username.
- Single commands are capped at 256 characters of input.

---

## Server — `ssh_server_vita` (`ROM000070`)

A libssh-based server: `ssh` at the Vita from a devbox and you get a shell whose filesystem
is the Vita's own devices.

> **Status.** The shell, exec mode and SFTP have been smoke-tested on a device with `scripts/smoke.sh` (`TASK-001.04`, `TASK-002`), from both a macOS devbox (OpenSSH 10.6) and a Linux one (OpenSSH 9.6), which agree check for check. A few edge cases are still unproven; see Known issues.

### On the Vita's screen

The server app is a console, not a terminal: it shows listening state, the Vita's IP, the
port, the session count, the host key fingerprint and whether that key is on storage, how
you are allowed to sign in and which `authorized_keys` said so, and a rolling log of
connections, logins and commands. <kbd>START</kbd> stops the server.

- Listens on **port 22**, falling back to **2222** if something already holds it. The port
  in use is printed.
- Up to **4 concurrent sessions**, each with its own working directory, driven from one
  poll loop, so a slow `du` in one session cannot starve the others. Four is a hardware
  limit rather than a preference: each session is its own libssh session with its own
  buffers on a device with no swap.

### Where its files live

Four directories, tried in the order below, each one the same relative path under the
device's own `data` directory. The first the server can create and write is the one it
uses.

| Device | Path in a file manager | Path in the shell | Survives a reboot |
| --- | --- | --- | --- |
| `ux0:` internal storage or memory card | `ux0:data/vitasshd` | `/ux0/data/vitasshd` | yes — **use this one** |
| `uma0:` a USB drive in the host port | `uma0:data/vitasshd` | `/uma0/data/vitasshd` | yes, while plugged in |
| `xmc0:` the Vita TV's own card | `xmc0:data/vitasshd` | `/xmc0/data/vitasshd` | yes, while mounted |
| `ur0:` RAM disk | `ur0:data/vitasshd` | `/ur0/data/vitasshd` | no — it is RAM |

Why a list rather than one path: which partition a homebrew app can actually write is
something only the device knows. `ux0:` is the one every file manager reaches, but it is
not guaranteed to be present (a PCH-2000 with no card), writable, or roomy. So:

- **The host key is read from the first directory that has one.** Nothing is generated while
  a key exists anywhere on the device — a second identity is worse than a slow boot.
- **A new host key is saved in the first directory that accepts it.** If none does, the key
  lives in memory, the server still runs, and the status line reads
  `not saved: new fingerprint each boot` so you hear it from the Vita rather than from your
  client.
- **`authorized_keys` is read from the first directory that has the file** — never a merge of
  all four. Two files both claiming to authorise you is how a card you forgot about starts
  letting people in.
- **`~` is the directory the server could write**, or the first that exists at all, or
  `/ux0`.

Nothing about that search is silent: the screen and its log say where the key came from or
went to, which `authorized_keys` was read, how many of its lines were keys, and how many
were not. Read the screen once and you know which directory your device is using.

The screen prints a path the way the server hands it to the device — `ux0:/data/vitasshd`,
the first column's directory with the slash moved one character. Both spell the same place;
a file manager takes either.

One letter apart and worth saying: the **client** keeps its profiles in `ux0:data/vitassh`,
the **server**'s directory is `ux0:data/vitasshd`.

### Signing in

The host key is Ed25519, generated on first run and saved as `ssh_host_ed25519_key` with
its `.pub` beside it, in the first directory that will have it. The fingerprint is on the
screen in the `SHA256:…` form OpenSSH prints, and the `.pub` exists so you can check that
the two agree: copy it off the card and `ssh-keygen -lf ssh_host_ed25519_key.pub` prints
the same string.

There are two sign-in states, and the console tells you which one you are in:

1. **`authorized_keys` present** — public key authentication. One key per line in the usual
   `ssh-ed25519 AAAA… comment` form, `#` comments allowed, up to 32 keys; a malformed line
   is skipped, named on the console with its line number, and counted on the screen rather
   than losing you the file. **Any username is accepted; the key is the credential.** The
   console reads `N key(s) in ux0:/data/vitasshd/authorized_keys`. Option prefixes
   (`from="…"`, `no-pty`) are accepted and ignored — this server has no use for them.
   Trailing `\r` from a Windows editor is tolerated; a line over 4 KB is not.
2. **No `authorized_keys`** — the server generates a 12-character password and prints it on
   the Vita's screen, again for any username. The alphabet drops the letters and digits that
   are hard to tell apart on a small screen at arm's length. This is a bootstrap for getting
   your key onto a device that has no keyboard, not a way to live: it stops working the
   moment `authorized_keys` has a key in it.

A file that is there but yields no keys falls back to the password rather than refusing
everything — and says so, because "my key is installed and it will not let me in" is not
something a person should have to debug over a 12-line screen.

The host key, the password and every key exchange are seeded from the Vita's hardware RNG.
If that seed is not available the server refuses to start rather than mint a predictable
host key and let you find out later.

### From a devbox

Install `ssh_server_vita.vpk`, launch it, and note the IP and the sign-in line.

```sh
# bootstrap: any username, password as shown on the Vita's screen
ssh vita@192.168.1.50

# once your public key is in ux0:data/vitasshd/authorized_keys
ssh -i ~/.ssh/id_ed25519 vita@192.168.1.50

# one command, no shell
ssh -i ~/.ssh/id_ed25519 vita@192.168.1.50 'ls /ux0/vita'

# if 22 was taken, the console says so and you want:
ssh -p 2222 -i ~/.ssh/id_ed25519 vita@192.168.1.50
```

`ssh-copy-id` will not work yet: it sends a compound `sh -c` command line, and this shell has no `sh` and no `;`, `&&` or `||` chaining, so install
`authorized_keys` yourself — VitaShell, a USB copy, or any route that writes a file to
`ux0:data/vitasshd/` — and restart the server so it re-reads it.

### Copying files with sftp and scp

The server speaks SFTP, so `sftp` works as usual and `scp` works through its SFTP mode.

```sh
sftp -i ~/.ssh/id_ed25519 vita@192.168.1.50

scp -s -i ~/.ssh/id_ed25519 game.vpk vita@192.168.1.50:/ux0/data/
scp -s -i ~/.ssh/id_ed25519 vita@192.168.1.50:/ux0/data/vitasshd/sshd.log .
```

Paths are the same POSIX paths the shell uses: `/` lists the devices, `/ux0` is the memory card, and a session starts in `ux0:data/vitasshd`. `-s` selects scp's SFTP mode, which needs OpenSSH 8.7 or later and is the default from 9.0 (an older client has no `-s`, so use `sftp`); the legacy scp protocol (`scp -O`) is not supported, because the shell has no `scp` command to run.

What to expect, because the Vita is not a Unix filesystem:

- An upload is written to `NAME.part` and renamed over `NAME` when the transfer finishes, so a transfer that is cut off leaves nothing under the real name, and the `.part` file is removed when the session ends.
- Every file reports owner 0:0 and mode 0644 (directories 0755, read-only devices 0444 and 0555), and nothing is ever reported as a symlink.
- `chmod` and `chown` fail with "Operation unsupported" instead of pretending to work. `scp` sends a chmod after every upload, so each upload prints `scp: remote fsetstat: Operation unsupported`; the file still arrives byte for byte and `scp` exits 0, for a 0644 file and for a 0755 one alike (checked with OpenSSH 10.6 on macOS).
- File times cannot be set, so `scp -p` and `sftp put -p` copy the data and leave the times alone.
- `rename` works within a device. Across devices it is refused rather than copied, and renaming onto a directory is refused. `sftp`'s `rename` replaces an existing file.
- At most four sessions run at once; a fifth connection is refused.

### When your client says the host key has changed

```
@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @
```

OpenSSH is comparing the key the Vita just offered against the one it recorded for that
address, and it is right to stop: on a normal host that message is what a man in the middle
looks like. Here there are two ordinary causes, and the Vita's own `host key` line tells you
which one you are looking at.

| The screen says | What happened | Do this |
| --- | --- | --- |
| `not saved: new fingerprint each boot` | No data directory would accept the key, so a new one is minted every time you start the server | Fix the write, not the client: see the [data directories](#where-its-files-live) above. Until then expect the warning every boot. |
| `saved in <path>` | That file changed underneath the client — corrupted by a save that did not finish, deleted, or replaced | Copy the `.pub` off the card and run `ssh-keygen -lf ssh_host_ed25519_key.pub`; its `SHA256:…` should match the screen. |

Then drop the stale entry, and note the bracketed form — OpenSSH records a non-standard port
as `[address]:port`, so the plain form will not match:

```sh
ssh-keygen -R 192.168.1.50          # port 22
ssh-keygen -R "[192.168.1.50]:2222" # port 2222
```

Reconnect and compare the fingerprint in the prompt against the Vita's screen before
accepting it. If you verify nothing else about this device, verify that one line: it is the
only thing between you and typing a password to whoever happens to be answering on port 22.

A key change is only a nuisance once, and only if the same fingerprint comes back. Check it
across a reboot: same `SHA256:…` on the screen before and after means the key is genuinely
persisted, and the warning should never come back for this address again.

### The shell

The Vita has no fork or exec, so this is a busybox-style shell: commands are functions
running in the server's own process, the working directory belongs to the session rather
than to libc, and POSIX paths are mapped onto sceIo devices at the boundary.

```
cat  cd  cp  date  devices  df  du  echo  env  exit  grep  head  help
ls   mkdir  mv   pwd     rm   rmdir  sort  stat  tail  touch  uname  uniq  vcmd  vpk  wc  whoami
```

`vcmd HOST PORT [LINE]` sends one newline-terminated line to a TCP port and prints the
reply, which is how you drive a plugin's debug socket from a session you are already
sitting in -- `vcmd 127.0.0.1 1339 status` for the Wi-Fi Turbo remote control, `1338`
for the vitacompanion command port. It stands in for that plugin's `tools/bench/vcmd.py`
client, so you do not have to leave the terminal to talk to the device; the command
itself is generic and any request/response port will take it. It is not called `netcat`
because it is not one: one line out, the reply in, then the socket is closed.

Two limits worth knowing. It takes a numeric IPv4 address only -- a name would need
`gethostbyname()`, and a builtin may not block the server's one poll loop on an unbounded
resolver lookup. And the whole exchange gets five seconds, served non-blocking so other
sessions keep running while it waits. Loopback is asymmetric: `:1339` answers because it
lives on the plugin's own thread, but `127.0.0.1:22` is this server waiting on itself, so
it just spends the deadline.

`grep PATTERN [FILE...]` prints the lines that match, and reads a pipe as well as files
(`ls /ux0 | grep data`). **Pattern syntax: POSIX basic regular expressions** — `.`,
`[ ]`, `^`, `$`, and the operators `\( \)`, `\{m,n\}`, `\|`, `\+`, `\?`. This is real
regex, not literal substring matching; unescaped `+ ? ( ) { } |` are ordinary bytes, the
way BRE defines them. Options: `-i` case-insensitive, `-v` invert, `-n` line numbers,
`-c` count only, `-l` file names only, `-q` quiet (exit status only). With no FILE it
reads the session's stdin; with several files each match is prefixed `file:`. Exit status
follows the convention: **0** a line matched, **1** none did, **2** an error (bad pattern,
unreadable file). A file that cannot be read is reported as `grep: FILE: reason` and the
other files are still searched. A line longer than **4096 bytes** is reported as too long
rather than matched, never silently truncated into a wrong answer; and a pattern too
expensive to match in the step budget is refused rather than allowed to hang the session.

`head [-n N | -c N] [FILE]` prints the first N lines (**10** by default) or the first N
bytes, and `tail [-n N | -c N] [FILE]` the last — the whole file when it holds fewer lines
than you asked for. With no FILE both read the session's stdin. The two agree with `grep`
on what a line is: one ends at a newline, and a file whose last line has none still prints
that line without inventing one, so a trailing newline never comes back as a blank line.
`head -n 0` and `tail -n 0` print nothing and exit **0**; GNU exits 1 for those, which is
the one place either tool deliberately differs, because an empty answer here is what you
asked for rather than a failure.

`tail` reads the file **backwards in 4 KB steps** and stops as soon as it holds the answer,
so `tail -n 20` of a 12 MB log costs twenty lines of reading and not the file; `head` stops
at the Nth newline. Options are `-n` for lines and `-c` for bytes, written `-n20` or
`-n 20`, and `tail +20` is accepted as the older spelling of `tail -n +20`. A line over
**4096 bytes** is reported as too long (`head: FILE: line too long (limit 4096 bytes)`, exit 1) instead of being cut to the prefix that fits. Both commands stream, so the lines before it have already been written, and `tail` may also have written up to 4096 bytes of the long line before it reports the error; `head` writes none of it. `-f` is refused with a plain
"following a file is not supported": waiting for a file to grow means sleeping, and a
sleeping command parks every other session in this server's one poll loop. Exit status is
**0** on success and **1** on a failure such as an unreadable file or a line over the
bound; a bad option or a count operand that is not a non-negative integer is **2**. With
several FILE arguments each answer is preceded by `==> name <==`, the coreutils header, and
a file that cannot be read is reported as `head: FILE: reason` while the others are still
printed (the exit status is then nonzero, because a partial answer you cannot tell is
partial is the worse result). Each file is streamed in turn and none is held while the next
is read, so several names cost no more memory than one.

`wc [-l] [-w] [-c] [FILE]` counts **lines, words and bytes** of a file or (with no FILE)
the session's stdin. A line is counted the way `grep` and `head` count one — by newline,
so a last line without a newline is still a line and a trailing newline never comes back as
a blank. A word is a run of non-blank bytes. With no flag all three columns print; with any
of `-l`, `-w`, `-c` only those print. With no FILE each count is right-aligned in seven, as
`wc` prints it; with file names the columns share one width (the widest count across every
row) and each row ends with the name, with a `total` row when more than one file is given.
`sort [-n] [-r] [-u] [FILE]` orders whole **lines**, and with no FILE reads stdin. Plain
`sort` is byte order (an unsigned byte compare, a proper prefix first); `-n` compares the
leading numeric value instead — leading blanks, an optional sign, digits and an optional
fraction — with byte order only as the tie-break, so `9` sorts under `10` while `1`, `01`
and `1.0` keep their byte order among themselves. `-r` reverses. `-u` keeps one line per
key, comparing keys *without* the byte tie-break, so `sort -nu` of `1`, `01`, `1.0` prints a
single line. The line kept from a run of equal keys is the one `sort -u` would keep: the
first of that run in the sorted order. `uniq [-c] [-d] [-u] [FILE]` collapses runs of
adjacent **byte-identical** lines and prints one line per run, in input order; unlike
`sort -u` it never reorders, so unsorted input with repeats scattered through it prints
each repeat run separately, which is why `uniq` follows `sort`. `-c` prefixes each line with
its run count right-aligned in seven, `-d` keeps only runs of two or more, `-u` only runs of
exactly one, and `-d` with `-u` prints nothing.

All three hold the whole input in memory before answering (there is no fork, and a walk may
not park the poll loop reading a socket a byte at a time), so an input over **4 MB** is
refused rather than answered short — `wc: FILE: file too large (limit 4194304 bytes)` — a
single line over **65536 bytes** by the two that keep lines is `line too long`, and a `-n`
key with more than **32 digits** is `numeric field too long`. Those bounds are the tool's
own; exit status is **0** on success, **1** on a read or bound failure, **2** on a bad
option or (for `uniq`) a second file name.

[`docs/vcmd.md`](docs/vcmd.md) has the arguments, the error strings, and the command
surfaces both plugins expose on those ports.

`/` lists the mounted devices and `/ux0` is the memory card; `devices` and `df` tell you
what is actually there. Quoting (`'…'`, `"…"`, `\`) works, `*` and `?` expand in arguments
unless quoted, and the line editor keeps 12 lines of history per session. `help` and
`help COMMAND` are the reference on the device.

The Vita has no filesystem root, so two names are answered by the server itself rather
than by sceIo: `/` (the device table) and `/etc/os-release`, which `cat`, `ls` and `stat`
all serve. It reports what the firmware API returns, so `VERSION_ID` and `uname -a` can
never disagree. For a device reporting 3.74:

```
$ uname -a
Vita vita 3.74 VitaSDK armv7a
$ cat /etc/os-release
NAME="PlayStation Vita"
VERSION="3.74"
ID=psvita
ID_LIKE=vitasdk
VERSION_ID="3.74"
BUILD_ID="0x03740000"
PRETTY_NAME="PlayStation Vita 3.74"
VITA_MODEL=vita
VITA_SSHD_BUILD="<__DATE__ __TIME__ of the server build>"
HOME_URL="https://www.reddit.com/r/vitahacks"
SUPPORT_URL="https://github.com/pythoninthegrass/vita_ssh"
```

Bare `uname` still prints just `Vita`. The release is the firmware the kernel reports, not
a compiled-in number, so a quoted `uname -a` or os-release block in a bug report is quoting
the device; a spoofed firmware reports its own spoofed value. `/etc` exists only in the
shell: SFTP and `scp` treat `/etc/os-release` as the unknown path it is, and `>` cannot
write to it, so `cat /etc/os-release > /ux0/data/x` works while `echo hi > /etc/os-release`
fails.

`vpk list FILE` prints a package's title ID and files without writing anything.
`vpk install FILE` accepts only the server's own title (`ROM000070`), unpacks and
CRC-checks it under `ux0:data/vitasshd-update/server`, and generates its package header.
The system installer cannot replace a running title, so the server installs its bundled
**VitaSSHd Updater** (`ROM000072`), closes the SSH command, launches the updater and exits.
The updater promotes the staged server and relaunches it, including after an installer
failure. Reconnect after the server returns; SSH success means **staged for handoff**,
not that installation has finished. Read `/ux0/data/vitasshd-update/result.txt` for the
installer result (`0x00000000` means success).
This file records the last helper run; failures before handoff are reported on SSH
and do not replace that previous result.

This requires a one-time VitaShell install of an updater-capable server build.
The updater VPK is bundled inside the server VPK; it needs no separate manual install.
Staging never uses VitaShell's `ux0:data/pkg` or the credentials in `ux0:data/vitasshd`.
A staging failure leaves the installed server unchanged. Promotion is a system operation:
there is no app-managed rollback, and hardware tests show that files omitted from the
new package are removed from the app directory. Keep persistent files in the data
directory and retain a VPK for VitaShell recovery.
After the updater has exited, `/ux0/data/vitasshd-update` can be removed to reclaim staging
space; subsequent installs also replace the previous stage. Do not delete it during a handoff.
The server and updater are built UNSAFE for system installation and app launch.

A line is one command, or commands joined with `|`. There is **no `;`, `&&` or `||` chaining**; those characters are ordinary text to the tokenizer. A pipeline runs its commands one after another, not at the same time: each command's output is held in memory (up to 1 MB, past which the pipeline stops with an error naming the limit) and becomes the next command's stdin, so `ls /ux0 | grep data` works. The `|` must be a separate, unquoted word (`echo a|b` is just text, as is `"|"`), and `exit` or `cd` before the last command of a pipeline has no effect. A command name the shell does not know prints `sh: NAME: command not found` and exits 127; a malformed pipeline (`| cat`, `echo x |`) prints `sh: syntax error near unexpected token `|'` and exits 2. Redirection exists in one form: `>` or `>>` as a separate, unquoted word followed by a file name (`echo hi > /ux0/data/x.txt`), and it redirects stdout only. `echo hi>x` is just text.

---

## Building

You need [VitaSDK](https://docs.vitasdk.org) (`brew install vitasdk`) and
[`task`](https://taskfile.dev) (`brew install task`, or `mise use -g task`). Nothing else
has to be installed by hand — `task setup` builds the libraries both apps link:

```bash
task setup   # checks the toolchain, then builds OpenSSL, libssh2 and libssh into $VITASDK (skips when installed)
task build   # configures and builds both apps' SELF + VPK into build/ (skips when nothing changed)
task clean   # remove build/, task's fingerprints, and the staged portlib sources
```

`task build` runs CMake; it builds both apps in one invocation and exits non-zero if either
fails. The outputs are `build/ssh_client_vita.vpk` and `build/ssh_server_vita.vpk`.

`task setup` compiles the `libs/*/VITABUILD` recipes into the SDK, in this order because
both SSH libraries configure against the OpenSSL built there:

| Package | Version | Built for |
| --- | --- | --- |
| OpenSSL | 1.1.1 — [SonicMastr/vita-openssl3](https://github.com/SonicMastr/vita-openssl3), branch `OpenSSL_1_1_1-vita` | both, since the two SSH libraries configure against it |
| libssh2 | 1.11.1 | the client |
| libssh | 0.12.2, with `libs/libssh/vita.patch` applied | the server |

Each package checks for the library it installs and skips itself when the SDK already has
it, so re-running `task setup` after a pull is cheap. After editing a recipe, force just
that one: `task --force setup:openssl`, `setup:libssh2` or `setup:libssh`. Sources stage
under `.task/portlibs` (~160 MB, gitignored); `task clean` removes them and leaves the
libraries already installed in `$VITASDK` alone.

`VITASDK` resolves from `task build VITASDK=<path>`, then `.env`, then
`~/.local/opt/vitasdk`. Run `cp .env.example .env` and set it there for a machine-local
default. Changing the SDK path means a fresh `build/`, because `CMakeCache.txt` pins
absolute toolchain paths.

To compile and link one app while you work on it, build its executable target rather than
asking make for a VPK (the `.self` and `.vpk` targets are stamped files):

```bash
cmake --build build --target ssh_server_vita
```

There are no tests, no lint and no CI. A successful `task build` that produces both VPKs is
the verification.

## Installing

1. Copy the `.vpk` to the Vita — VitaShell, the content manager, whatever you already use.
2. Install it. The two apps have different titleids and can both be installed.
3. Get on WiFi. The server shows the address it thinks it has.
4. Client: add a profile under **Profiles** and connect. Server: launch it and read the
   screen.

Each app ships its own tile. The server's is rendered from the SVG sources in `assets/`
(`assets/sshd-icon0.svg`, `assets/sshd-bg0.svg`) by one command — edit the SVG and run
`assets/render_tiles.sh`, never touch the PNGs:

```
assets/render_tiles.sh          # render from assets/*.svg, then verify every tile
assets/render_tiles.sh --check  # verify only; exits 1 if any tile would be rejected
```

The client's tile PNGs (`sce_sys/`) arrived without their sources; giving them SVG originals is
`TASK-009`.

Every tile PNG has to be an **8-bit palette PNG** — color type 3, bit depth 8, `IHDR,PLTE,IDAT,IEND`
and nothing else — or the Vita's package installer refuses the whole VPK and the install dies with
`0x8010113D`. That is why the render goes through a converter instead of straight out of the
renderer, and why the script reads back what it wrote: a 24-bit TrueColor PNG looks fine to most
tools, and the ancillary chunks (`tEXt` dates, `bKGD`) that image libraries add by default are
rejected too. To check a suspect tile by hand:

```
file sce_sys_sshd/icon0.png    # must say: 8-bit colormap, non-interlaced
```

`8-bit/color RGB` or `8-bit/color RGBA` means the installer will reject it. `magick identify`
cannot tell you this — it quotes 8-bit depth per channel for a 24-bit image and calls both
files `Palette`, which is how a TrueColor tile got shipped in the first place. Beware the same
trap in a render command: `-depth 8` means 8 bits *per channel*, so `PNG24:` output looks correct
and is not. This is the first thing to look at when a VPK will not install.

The converter is `scripts/convert_png.py`, a self-contained uv script (`uv run
scripts/convert_png.py <in.png> --out <out.png>`); `assets/render_tiles.sh` calls it. It quantizes
with Pillow by default and `--quantizer pngquant` opts into that instead — measured against the
24-bit renders, Pillow was 50 to 1000 times closer on the livearea gradient and effectively tied
on the icon, and it needs nothing but uv.

---

## How the tree fits together

```
src/main.c input.c network.c ssh_client.c shell.c text.c menu.c   client (libssh2)
src/sshd.c sshd_session.c sshd_builtins.c sshd_hostkey.c          server (libssh)
src/sh_path.c vita_shims.c
src/network.c                                                      the one shared source
include/                                                           headers for both
libs/*/VITABUILD + taskfiles/setup.yml                             portlib recipes
assets/ sce_sys/ sce_sys_sshd/                                     tiles and livearea
backlog/                                                           the queue
```

Two `add_executable` lists in `CMakeLists.txt`, one per app, and nothing globs: a new `.c`
file has to be added to the right list or no compiler ever sees it. The client never links
libssh and the server never links libssh2. `AGENTS.md` is the working document for the
build quirks that cost someone a day each — the flag shadowing in `vita.cmake`, the VPK
rebuild edge, the Homebrew OpenSSL that leaks into a configure probe and lies about it.

A few of the interesting decisions, while we're here:

- The Vita has no fork or exec, so the server's commands run in-process and the shell owns
  its own cwd per session. Two people logged in cannot move each other's relative paths.
- One vita2d context serves the graphics and the text console: this SDK has no
  `sceDebugScreenInit`, so there is no debug screen to fall back to, and `printf` is not
  how anything reaches the user.
- The IME hands over UTF-16; SSH speaks bytes; the conversion is done on the way in.
- libssh's server and SFTP declarations are compiled out of its public headers unless
  `WITH_SERVER` is defined, which only its own private config does — hence the define on
  the server target.
- The host key is imported into the bind (`SSH_BIND_OPTIONS_IMPORT_KEY`) rather than handed
  over as a path, because libssh re-reading it off storage fails on exactly the devices
  where you need it to work.

---

## Known issues

Client:

- Full-screen programs do not work (`TASK-007`); the terminal implements the CSI sequences
  ordinary output uses and ignores the rest.
- No scroll-region support, so a program that draws its own frame scrolls the whole screen.
- A single command's output is collected in a 16 KB buffer before it is drawn; anything past
  that is dropped without a word. Use the interactive shell for output that big.
- Sessions drop out on a weak radio; reconnecting is manual.

Server:

- SFTP cannot set permissions, owners or file times, and the legacy scp protocol (`scp -O`) is not supported; see "Copying files with sftp and scp" for what that means in practice. There is no statvfs over SFTP.
- The exec, pty, sftp and scp checks match between macOS (OpenSSH 10.6) and Linux (OpenSSH 9.6); Windows clients have not been tried.
- A cross-device `mv` is only as testable as the hardware: the checks pass against a Vita with a USB drive mounted as `uma0`, and `scripts/smoke.sh` skips them on a unit with nothing in the USB port. Plug a drive in before running them.
- A running command cannot be interrupted. The server is one thread, so the keystroke that would stop it arrives on the socket the command is blocking; Ctrl-C at the prompt works, Ctrl-C during a `cp -r` does not. Long recursive walks are bounded by a depth limit instead.
- At most 4 sessions are open at once (a hardware limit). A connection that has not logged in within 30 seconds is dropped, and when all 4 slots are taken a new connection takes the slot of the longest-waiting one that has not logged in. So a client that is slow to authenticate can lose its slot to a newcomer while the table is full, and a fifth connection is refused outright only when all 4 sessions have logged in.
- The bootstrap password is compared in non-constant time, and it exists to be replaced.
  Use `authorized_keys` for anything you care about.
- If no data directory is writable, the host key lives in memory and every boot introduces
  itself with a new fingerprint. The server works; your `known_hosts` file does not. The
  `host key` line on the Vita's screen says which case you are in — see
  [when your client says the host key has changed](#when-your-client-says-the-host-key-has-changed).

---

## Contributing

The queue is the backlog in [`backlog/`](backlog), and it is the place to look for work
before proposing your own:

```bash
backlog task list                 # grouped by status
backlog task view TASK-006        # one task, with its acceptance criteria
```

Unclaimed, self-contained, and reasonable to start on:

| Task | Work |
| --- | --- |
| `TASK-005.01` | private key authentication in the client |
| `TASK-005.02` | host key verification in the client |
| `TASK-007` | alternate screen and scroll regions, so `top` and `vim` work |
| `TASK-008` | touch selection in the menus and profile list |
| `TASK-009` | SVG sources for the client's tile |
| `TASK-010` | dim the server's screen while it runs (needs the brightness call found first; the public ScePower stub has none) |
| `TASK-011` | a full-screen text editor builtin for the server's shell |

Ground rules, all of them inherited from how this repo is built:

- Read [`AGENTS.md`](AGENTS.md) first. It is the build contract, and several lines in it
  exist because the obvious thing does not work.
- `task` is the entry point; never run `cmake` directly. A successful `task build` producing both VPKs is the only check that runs without a device. [`scripts/smoke.sh`](scripts/smoke.sh) checks a running server over the LAN, [`docs/throughput.md`](docs/throughput.md) has the throughput procedure and baseline, and [`docs/development.md`](docs/development.md) covers installing a build on the Vita and debugging it from the log and crash dumps.
- The two apps keep separate link lists. The client stays a libssh2 client and the server
  stays a libssh server.
- Tile PNGs are generated. Edit the SVG in `assets/` and run `assets/render_tiles.sh`; it verifies
  every committed tile on the way out. The installer rejects anything that is not an 8-bit palette
  PNG (`0x8010113D`), so never write a tile PNG with an image tool directly.
- Want something that is not in the backlog? Open a task for it rather than opening a PR
  that assumes it.

---

## Provenance

VitaSSH is its own project: a standalone repo with its own history, no upstream branch to
keep in step with, and no reason to preserve a mistake because it came from somewhere else.
What it stands on:

- It began as a fork of [VitaSSH](https://github.com/rompelhd/VitaSSH) by rompelhd. The
  client's shape is where that started; the code has since diverged past recognition, and
  upstream is unmaintained.
- Packaging and installation follow the conventions of
  [VitaShell](https://github.com/TheOfficialFloW/VitaShell) by TheOfficialFloW. The server's `vpk install` command follows VitaShell's installer (unpack, write `sce_sys/package/head.bin`, call `scePromoterUtilityPromotePkgWithRif`) and separate-updater handoff pattern. It uses its own staging directory. One file from VitaShell is vendored: `third_party/vitashell/head.bin`, the package-header template, under VitaShell's GPL-3.0 with its licence text beside it. The code that fills it in, `src/vpk_head.c`, is this repo's own.
- The OpenSSL Vita port that `task setup` compiles is
  [SonicMastr/vita-openssl3](https://github.com/SonicMastr/vita-openssl3).
- The libraries doing the real work — libssh2, libssh, vita2d, VitaSDK — are listed with
  their versions and licence terms under [Licence](#licence). None of them is vendored; the
  recipes that build them into `$VITASDK` are in `libs/`. The only third-party file in the tree
  is the VitaShell template above, in `third_party/`.

## Licence

The sources written for this repository are released into the public domain under the
[Unlicense](LICENSE.md). Do what you want with them, including selling them, without asking.

That dedication does not cover other people's files, and it relicenses nothing anybody else wrote. The one such file in the tree is `third_party/vitashell/head.bin`, VitaShell's package-header template, which stays under VitaShell's GPL-3.0 (the licence text is beside it, with the upstream commit and checksum in `third_party/vitashell/README.md`). The server app embeds that file, so a server VPK carries it: check what GPL-3.0 asks of you before you redistribute one. Both apps also ship statically linked third-party code, which stays under its own terms:

**libssh — LGPL-2.1.** The server app links
[libssh](https://www.libssh.org) 0.12.2 statically (copyright Aris Adamantiadis and the libssh
team), under the GNU Lesser General Public License, version 2.1 or any later version. It is not
vendored here: `task setup` fetches it from libssh.org and builds it with
[`libs/libssh/VITABUILD`](libs/libssh/VITABUILD) plus
[`libs/libssh/vita.patch`](libs/libssh/vita.patch), so the exact recipe for the copy in your VPK
is in this repo. That is also what makes the LGPL's relinking condition practical rather than
theoretical — take the sources here, build a modified libssh against the same recipe, and relink
the app. Reverse engineering for the purpose of debugging such modifications is permitted under
LGPL-2.1 §6. The licence text is at
<https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>; libssh's own sources carry the notice
in their file headers.

The other libraries compiled into the VPKs, for completeness:

| Library | Version | Licence | In |
| --- | --- | --- | --- |
| [libssh2](https://www.libssh2.org) | 1.11.1 | BSD-3-Clause | client |
| [OpenSSL Vita port](https://github.com/SonicMastr/vita-openssl3) | 1.1.1 | dual OpenSSL / SSLeay | both |
| [vita2d](https://github.com/grapheneSystems/Vita2D) | SDK-provided | see upstream | both |
| [VitaSDK](https://docs.vitasdk.org) toolchain and stubs | — | see upstream | both |
| [VitaShell](https://github.com/TheOfficialFloW/VitaShell) `head.bin` | upstream commit `8120486` | GPL-3.0 | server |

None of them is relicensed by this repo's dedication, and nothing above is legal advice — if you
redistribute a VPK, carry the licence texts of the libraries inside it.

---

> Experimental. Proof of concept. Vita homebrew.

This software is provided *as-is*, for educational and experimental purposes. Use it
responsibly, on machines you are allowed to touch, and in compliance with applicable laws
and terms of service.

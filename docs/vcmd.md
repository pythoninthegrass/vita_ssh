# vcmd — one line to a TCP port

`vcmd` is a shell builtin that opens a TCP connection, sends one newline-terminated
line, prints what comes back and closes the socket. It exists so a plugin's debug
port can be driven from a session you are already sitting in, instead of from a
script on a laptop.

It is deliberately **not** `netcat`: one line out, the reply in, then the socket is
gone. The distinction matters because a real `nc` can sit and pump a bidirectional
pipe, and a builtin in the server's single poll loop must never do that (see *Why it
is written this way* below).

```
vcmd HOST PORT [LINE]
```

| Argument | Required | Meaning |
| --- | --- | --- |
| `HOST` | yes | numeric IPv4 address only. `127.0.0.1` reaches the Vita's own plugins |
| `PORT` | yes | TCP port, 1–65535 |
| `LINE` | no | the request. Arguments are joined with single spaces; with no `LINE` a bare newline is sent, which is enough for a server that needs no argument |

Exit status is `0` when a reply was received, `1` on any failure — checked on a device for
the success path, a refused port, a usage error and a rejected name. Output goes through
`sh_write()`, so `>` and `>>` redirect it like any other builtin (also device-checked).

```bash
vcmd 127.0.0.1 1339 status                      # the Wi-Fi Turbo remote control
vcmd 127.0.0.1 1339 status > /ux0/data/x.txt    # redirect works
vcmd 192.168.1.60 1338 "press cross; release cross"  # vitacompanion, if installed
```

Quote anything with spaces or semicolons so it arrives as one argument — `split_line()`
in `src/sshd_builtins.c` handles `'…'`, `"…"` and `\`.

## Errors, and what they mean

The distinction between a refused port and a dead address is the entire point of the
command, so the two are kept visually obvious and timed. The *Seen* column says how each
row was confirmed: **device** means it was printed by a real run on the home Vita,
**source** means the string is in `cmd_vcmd` but the path was not reached — no reason to
doubt it, but it has not been observed.

| Output | Seen | Meaning |
| --- | --- | --- |
| `vcmd: usage: vcmd HOST PORT [LINE]` | device | missing `HOST` or `PORT` |
| `vcmd: <host>: not an IPv4 address (names are not resolved)` | device | a name was given; see the constraint below |
| `vcmd: <port>: bad port` | device | not a number, or outside 1–65535 |
| `vcmd: <host>:<port>: connect failed: <text> (socket error 0x<n>)` | device | the peer actively refused. **Returns in about 1.5 s** |
| `vcmd: <host>:<port>: Connection timed out` | device | nothing answered. **Spends the full 5 s budget, then gives up** |
| `vcmd: <host>:<port>: <text> while sending` | source | the connection died while the request went out |
| `vcmd: <host>:<port>: connection closed while sending` | source | the peer went away before the request was accepted |
| `vcmd: <host>:<port>: <text> while reading` | source | the connection died mid-reply |
| `vcmd: reply truncated at <n> bytes` | source | the reply hit the 64 KiB cap; the prefix shown is all there is |
| `vcmd: request too long` | source | the request exceeds `NET_LINE_MAX` (1024). Checked before and during the send, so it cannot partially go out |
| `vcmd: socket: <text>` | source | `socket()` itself failed |
| `vcmd: out of memory` | source | the request or reply buffer could not be allocated |

The `socket error 0x…` number is printed because sceNet hands back codes newlib's
`strerror()` cannot name — a refused port on loopback reports `0x3d` (`ENODATA`) and
would otherwise read as "No data", which is a misleading answer from a diagnostic tool.
This is the general `strerror()`-on-sceNet rule in AGENTS.md.

## Command surfaces it can drive

Two plugins on the Vita expose a newline-request/response TCP port. The protocol shape
is the same on both: connect, send a line ending in `\n`, read until EOF.

### Wi-Fi Turbo — port 1339

Requires **Settings → Network → Wi-Fi Turbo → Developer mode** to be on; with it off
there is no listener at all. `vcmd status` is the only one of these verified on a device
in the writing of this document — the rest are transcribed from the plugin's own `help`
output and `user/dispatch.c`, and were not executed because several reassociate the
radio.

| Command | Effect | Risk to your session |
| --- | --- | --- |
| `help` | the plugin's own command list | none |
| `status` | ssid, netctl state, rssi dBm and %, snr, noise floor, channel | none |
| `selftest` | subsystem self-check | none |
| `log [dump]`, `log level <err\|warn\|info\|debug\|trace>`, `log clear` | read and set the plugin log | none |
| `profile maxperf\|balanced\|maxbatt` | power-save profile | **reassociates** — needs `confirm` within 45 s |
| `ht40 on\|off` | 40 MHz channel width | **reassociates** — needs `confirm` within 45 s |
| `confirm` | approves the pending change | — |
| `revert` | undoes a pending change | — |
| `autoreconnect on\|off` | reconnect after a drop | none |
| `keepwifi on\|off` | hold off auto-standby (default on) | none |
| `hud off\|compact\|extended`, `hud pos tl\|tr\|bl\|br` | on-screen HUD | none |
| `hudgame on\|off`, `hudrate on\|off`, `hudauto on\|off` | HUD scope and rate | none |
| `roam on\|off`, `roam level 65\|70\|75\|80`, `roam now`, `roam precheck on\|off`, `roam status` | Smart Mesh Roaming | `roam now` may roam |
| `mac status\|stock\|custom\|random\|set <addr>\|pernet on` | MAC address policy | **changes your MAC** |
| `adv scan` | scan available advanced verbs | none |
| `speedtest <ip> <port> [secs] [rcvbuf_kb]`, `udpsink <port> [secs]` | throughput probes | saturates the radio while running |
| `netdown` | command netctl to disconnect | **drops the link** — kills your ssh session |
| `icm` | the shell's saved-network auto-connect | none |
| `icmtest` | exercise the reconnect path | **drops the link for up to ~65 s** |
| `devmode on\|off` | toggles the listener itself | **`off` closes port 1339 behind you** |

`devmode` is accepted by the dispatcher but is not in the plugin's `help` list.

Note that a Wi-Fi-dropping command run over a loopback `vcmd` still kills the *ssh
session* you typed it in, because that session rides the same radio. The `vcmd`
invocation itself returns first; you just lose the shell afterwards.

### vitacompanion — port 1338

Not installed on the test device; this is transcribed from the upstream README.
vitacompanion also opens an FTP server on **1337**. Its `press` and `release` need a
**kernel** module — there is no user-mode equivalent, so those two cannot be built into this
server (see the next section).

| Command | Arguments |
| --- | --- |
| `help` | — |
| `version` | — |
| `launch` | `<TITLEID>` |
| `quit` | `<TITLEID>` or `all` |
| `promote` | `<directory>` — an extracted app directory, not a VPK |
| `reboot` | — |
| `screen` | `on` or `off` |
| `nosleep` | `on`, `off`, `status` |
| `press` | `cross`, `left-stick <x> <y>`, `front-touch <slot> <x> <y>`, … |
| `release` | an input target, or `all` |
| `wait` | duration ending in `ms` or `s`, for chained commands |

Commands chain with `;`, e.g. `press cross; wait 100ms; release cross`.

`nosleep` and Wi-Fi Turbo's `keepwifi` both suppress the idle timer. If both plugins are
ever installed, expect them to fight over the power callback.

### Where the plugin boundary is, and what cannot be built in

The obvious question is why this server does not simply implement vitacompanion's verbs and
save installing a plugin. For most of them the answer is "it can, and four of them are
filed". For input the answer is structural, and it is the same answer twice: **vitacompanion
is a plugin pair; this server is an app.**

Its README says it plainly: *"Simulates controller and touch input through a small kernel
module"*. It installs `vitacompanion_kernel.skprx` under `*KERNEL` and `vitacompanion.suprx`
under `*main` in `ur0:/tai/config.txt`, so taiHEN loads them **into every app that starts**.
That is how `press cross` reaches the game you are playing. This server is a self —
`vita_create_self(${VITA_SERVER_NAME}.self ${VITA_SERVER_NAME} UNSAFE)` in `CMakeLists.txt` —
so the moment the app you would want to drive becomes foreground, this server is suspended
and then destroyed. There is no ssh session left to type `press` with.

The headers agree, and they name the exact API that is out of reach. The injection calls are
`ksceCtrlSetButtonEmulation()` (`psp2kern/ctrl.h:163`), `ksceCtrlSetAnalogEmulation()`
(`:185`) and `ksceCtrlRegisterVirtualControllerDriver()` (`:201`) — **kernel** headers.
Nothing like them exists in user `psp2/ctrl.h`, where every `sceCtrlSet*` is
`SetRapidFire`, `SetActuator` (rumble), `SetLightBar` or `SetButtonIntercept`. So
`press`/`release` are unavailable twice over: no user-mode API, and no live process to call
one. Reach for vitacompanion for those, and install **both** modules — the user module does
not load the kernel one itself.

`sceCtrlSetRapidFire` is the shortcut that tempts people and does not work. It is the only
user-side call that synthesises button state (`Mask, Trigger, Target, Delay, Make, Break`,
16 rule slots), but it is a *repeating* rule, buttons only — no sticks, no touch — and it
applies while this app owns input. It cannot produce a discrete `press`/`release` pair in
someone else's app.

The rest, from SDK inspection on the tree in use. Planned means a filed task; **unverified**
means the claim comes from a header or a name, not a device run.

| vitacompanion verb | Built in here? | Basis |
| --- | --- | --- |
| `launch <TID>` | planned, TASK-023.02 | `sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=…")` is already called at `src/sshd_vpk.c:459` and `src/sshd_updater.c:44`; `SceAppMgr_stub` is already linked |
| `quit <TID>` | planned, TASK-023.04 | `sceAppMgrGetIdByName` + `sceAppMgrDestroyAppByName` already at `src/sshd_updater.c:27-29` |
| `quit all` | partly, TASK-023.04 | `sceAppMgrDestroyOtherApp()` exists. *Listing* running apps needs `sceAppMgrGetRunningAppIdListForShell`, whose `ForShell` suffix suggests it wants shell privileges — **unverified**, expect `EPERM` from an app |
| `screen off\|on` | planned, TASK-023.03 | `scePowerRequestDisplayOff` / `scePowerRequestDisplayOn` are in user `psp2/power.h`; `ScePower_stub` is already linked. These are **not** the dimming knob TASK-010 wants — there is no brightness setter in the public stub |
| `nosleep` | planned, TASK-023.01 | the behaviour is already the server's (`scePowerSetIdleTimerCount(0)` at `src/sshd.c:600`); only the command surface is missing |
| `promote <dir>` | already superseded | `vpk install` does this and more: `head.bin` template, staging, updater handoff |
| `version` | pointless | TASK-021 made `uname -a` and `cat /etc/os-release` report the kernel's own firmware word (`src/sshd_sysinfo.c`), so this verb would only re-send values the shell already prints |
| `reboot` | planned, TASK-024 | `scePowerRequestColdReset()` is a user API (`psp2/power.h:160`) and `ScePower_stub` is already linked. The task is not the call but deferring it until every session has flushed — a builtin may not sleep, and yanking the transport early makes `ssh` exit 255. Non-interactive by necessity (`reboot now` / `+90` / `cancel`): builtins have no stdin, so a "are you sure?" prompt is not implementable and would be an unbounded wait in the poll loop. `shutdown -r now` / `-r +90` / `-c` is accepted as a Linux-shaped spelling over the same engine, and `-h` refuses: the public stub exports **no** power-off, `ColdReset` being the only reset verb |
| `help`, `wait` | pointless | `help` exists; `wait` only exists to chain a scripted protocol |
| `press`, `release` | **no** | kernel emulation API. See above |

Two decisions worth recording, because both look like the tidier option until tried:

- **These verbs are not subcommands of `vcmd`.** `vcmd` is a transport — `HOST PORT [LINE]`.
  If `vcmd press cross` sometimes meant "send these bytes to `:1338`" and sometimes meant
  "act locally", a silent no-op would be indistinguishable from a real press. For input that
  is the worst possible place to hide an ambiguity. The in-tree model for subcommands is
  `vpk list FILE | vpk install FILE` (`src/sshd_builtins.c`): a builtin that owns a noun.
- **The `:1338` protocol is not reimplemented for drop-in compatibility.** Input cannot work
  here, so a clone would half-respond and fail confusingly where "install vitacompanion" is
  the honest answer.

`nosleep off` also deserves a warning before anyone implements it: the idle-timer hold is
load-bearing (the comment at `src/sshd.c:592` explains that a session dying to standby dies
anyway), so a switch that stops the tick *while a session is connected* lets the device
sleep mid-session and kills the session that asked for it.

## Why it is written this way

Each of these is a constraint, not a preference.

- **Nothing may block.** A builtin runs inside the server's single poll loop, so every
  other session, the console and the pad are frozen while it runs — and a running
  command cannot be interrupted by Ctrl-C, because noticing the keystroke needs the very
  socket it is blocked on. `net_wait()` therefore does `poll()` with a zero timeout and
  calls `sh_server_poll_slice()` between checks, the same discipline `wait_for_room()`
  uses for the client's window. Connect, send and receive are all non-blocking.
- **Five seconds, hard.** `NET_DEADLINE_S`. A dead address costs a hitch, not the
  eleven-minute stall a bare blocking `connect()` to an unanswered address has already
  cost this server once.
- **Numeric IPv4 only.** `gethostbyname()` would block this thread for however long the
  resolver takes, with no bound to put on it. Names are refused with a clear message
  rather than half-supported.
- **Loopback is asymmetric.** `127.0.0.1:1339` answers fine, because that listener lives
  on the plugin's own thread and keeps running while the command waits. `127.0.0.1:22` is
  this server waiting on itself — bounded at 5 s, but useless, so do not reach for it as
  a self-test.
- **A refused connect reports through `SO_ERROR`.** A refused non-blocking connect
  completes as *successful* writability, so the wait must be interrogated with
  `getsockopt(SO_ERROR)` even when it succeeds; skip that and a closed port surfaces as
  `EPIPE` from the send and reads as "Broken pipe".
- **Reply is capped at 64 KiB** (`NET_REPLY_MAX`), so a chatty service cannot exhaust the
  heap. The truncation is announced rather than silent.

Reaching a plugin over loopback is also simply more reliable than reaching it over the
network from another machine: an access point that will not bridge frames to the Vita
still lets the Vita talk to itself, and the `:1339` surface answered instantly from
inside the device while it was unreachable from outside for the whole of a debugging
session.

## Prior art

| Project | What came from it |
| --- | --- |
| [vita-turbo-wifi](https://github.com/juanjsebgarcia/vita-turbo-wifi) | The `:1339` surface. Its [`tools/bench/vcmd.py`](https://github.com/juanjsebgarcia/vita-turbo-wifi/blob/main/tools/bench/vcmd.py) is a 17-line client for the same port; this builtin replaces it for anyone already in an ssh session, and keeps its name |
| [vitacompanion](https://github.com/devnoname120/vitacompanion) | The command-server shape. Wi-Fi Turbo's own listener is headed *"the TCP listener (vitacompanion style)"* in `user/dispatch.c`, which is why the two ports speak the same protocol |
| [robsdedude/vitacompanion](https://github.com/robsdedude/vitacompanion) | The `src/cmd_definitions.c` fork the upstream README points at for adding commands |
| [OpenBSD `nc`](https://man.openbsd.org/nc) | The one-shot request/response semantics, and the name this command deliberately does not take |
| [Rompelhd's VitaSSH](https://github.com/rompelhd/VitaSSH) | The shell this builtin was added to (see Provenance in AGENTS.md) |

The implementation is `cmd_vcmd` and `net_wait` in `src/sshd_builtins.c`; the row in the
`builtins[]` table is what registers it.

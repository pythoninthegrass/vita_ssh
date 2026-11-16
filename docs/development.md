# Development workflow

Nothing here runs on the host: the apps are cross-compiled for the Vita, so every change is checked by building, installing the VPK on a device and exercising it over the network. Build with `task setup` then `task build` (see AGENTS.md); this file covers what happens after the VPK exists.

## Install a build on the Vita

1. Open VitaShell on the Vita.
2. Press Select to start the USB mount. The Vita shows up on the Mac as a volume (`/Volumes/Untitled` here).
3. Copy the VPK over. VitaShell installs from `downloads/`:

   ```bash
   cp build/ssh_server_vita.vpk /Volumes/Untitled/downloads/
   ```

4. Eject the volume from the Mac. The disk number changes between mounts, so find it first with `diskutil list external`:

   ```bash
   diskutil eject /dev/disk10
   ```

5. Press Circle on the Vita to cancel the USB mount.
6. In VitaShell, open `ux0:downloads/` and install the VPK.
7. Press the PS button to leave VitaShell, then open the vitasshd app. The server prints `ready: ssh any@<ip> -p 2222` on its console once it is listening.
8. Test with ssh and sftp (scp support is still to come):

   ```bash
   scripts/smoke.sh          # exec, pty and sftp checks
   scripts/smoke.sh sftp     # sftp half only
   ```

   The scripts need key auth and a terminal on the same LAN as the Vita. The authoring agent's sandbox cannot reach the device, so run them yourself.

## Debugging

### The server log

The server writes `ux0:/data/vitasshd/sshd.log`. Read it from a shell session on the Vita itself:

```bash
ssh -p 2222 any@<ip> "cat sshd.log"
```

The shell has no `tail`, so the whole file comes back and the newest lines are at the bottom. A `=== VitaSSHd starting` line in the middle of the log means the app restarted, which after a `subsystem "sftp"` line usually means it crashed. The file is also reachable over the USB mount at `data/vitasshd/sshd.log`.

SFTP requests are logged one line each, except READ and WRITE, which would flood the log during a transfer: `sftp: request type N, M bytes`. The types are the SSH_FXP numbers from libssh's `sftp.h`: 1 INIT, 3 OPEN, 4 CLOSE, 7 LSTAT, 8 FSTAT, 9 SETSTAT, 11 OPENDIR, 12 READDIR, 13 REMOVE, 14 MKDIR, 15 RMDIR, 16 REALPATH, 17 STAT, 18 RENAME, 200 EXTENDED. `sftp: request type N failed (rc)` means the stock callback rejected a request.

Reading a log for a hang: the last logged request is the last thing the server saw. If the server accepted the subsystem and logged nothing, the client's INIT never arrived. If requests stop after a CLOSE and the client does not exit, look at how the session ends, not at the handlers. Other connections still being accepted proves the poll loop is alive.

### Crash dumps

A crash leaves `ux0:data/psp2core-<epoch>-<id>-eboot.bin.psp2dmp`. The epoch in the name is when it crashed (`date -r <epoch>` on the Mac), which is how to pick the newest. Dumps accumulate from every app, so match the time against the run you care about.

1. Mount over USB as above and copy the dumps off. Copy them before rebuilding.
2. Do not rebuild before parsing. Addresses are resolved against `build/ssh_server_vita`, which has to be the ELF that produced the VPK installed on the device.
3. Parse with `scripts/parse_dump.py`, a PEP 723 uv script with no dependencies (it reads the dump's ELF notes with the standard library; the format follows xyzz's [vita-parse-core](https://github.com/xyzz/vita-parse-core), which is unlicensed and so is not vendored here):

   ```bash
   PATH=$HOME/.local/opt/vitasdk/bin:$PATH \
     scripts/parse_dump.py <dump>.psp2dmp build/ssh_server_vita
   ```

   `arm-vita-eabi-addr2line` and `arm-vita-eabi-objdump` have to be on `PATH`, which the SDK `bin` directory provides. The ELF's file name has to match the module name in the dump (`ssh_server_vita`); `--module NAME` overrides it. `--stack N` sets how many stack words to print and `--no-disas` skips the disassembly.
4. Read the output top down. The `=== THREADS ===` list names the thread that crashed and its stop reason (`Data abort exception` is a bad memory access, `Undefined instruction` is usually a jump into data). `DISASSEMBLY AROUND PC` marks the faulting instruction with `!!!`, and `DISASSEMBLY AROUND LR` shows the caller. The stack dump lists return addresses with their `ssh_server_vita@1 + offset` symbols.

Worked example from TASK-002.02: the `sshd` thread took a data abort in `asctime_r` with `R0 = 0`, and the link register pointed into `ctime_r`, which calls `localtime_r` and passes the result on without checking it. libssh's directory listing formats every entry's mtime that way, and newlib's `localtime_r` returns NULL for a time sceRtc rejects, so a stat with mtime 0 killed the server. `grep ctime_r .task/portlibs/libssh-*/src/sftpserver.c` finds the call site.

### Checking what was linked

The SFTP layer replaces libc calls inside libssh with `-Wl,--wrap`. To confirm a wrap took effect in the stock handlers, disassemble the built ELF and look for the call:

```bash
arm-vita-eabi-objdump -d build/ssh_server_vita | rg 'bl.*<__wrap_(chmod|chown|truncate|statvfs)>'
```

A call to the bare libc name from inside `process_setstat` or `process_extended_statvfs` means the `-Wl,--wrap=` line is missing from `CMakeLists.txt`.

### Narrowing a failure without the scripts

Run the client by hand with verbose output and note which command stalls or kills the server:

```bash
sftp -vv -P 2222 any@<ip>
```

Then compare against `sshd.log`. A single command per client invocation (`sftp -b - <<< "ls /"`) separates a handler bug from an interaction between requests.

# Turbo Storage

[Turbo Storage](https://github.com/juanjsebgarcia/vita-turbo-storage) is a taiHEN plugin (MIT, firmware 3.60–3.74) that speeds up SD2Vita and official PS Vita memory card I/O by handing the system's own storage driver a non-cacheable staging buffer and CPU-copying the result, which skips the per-call data-cache clean and invalidate that costs roughly 40% of a stock read. Writes are never deferred; transfers under 32 KiB, on error, or into non-cacheable memory take the stock path. This file records how it is installed on the test device and what it measured against this server.

Installed: v0.4.0 (`8b48170`), test device at firmware 3.73, SD2Vita mounted as `ux0` and the official memory card as `uma0` by YAMT. It runs alongside YAMT, gamesd and Wi-Fi Turbo without problems; upstream also lists those as compatible.

## Install

Three files go to `ur0:tai/`, and one line goes under each of three sections of the config file taiHEN reads (here `ur0:tai/config.txt`; if `ux0:tai/config.txt` exists it wins, so check first). Back the config up before editing — on the test device the backup lives at `ux0:data/config.txt.bak-pre-turbo`.

```
# Turbo Storage (https://github.com/juanjsebgarcia/vita-turbo-storage)
*KERNEL
ur0:tai/storageturbo_kernel.skprx
*main
ur0:tai/storageturbo.suprx
*NPXS10015
ur0:tai/storageturbo_settings.suprx
```

Upload over this server's own SFTP (it is UNSAFE, so `ur0:` is writable), then reboot — a "Refresh taiHEN config" is not enough, the kernel module loads at boot only. The Settings page appears at *Settings → System → Turbo Storage*; both accelerations are opt-in and off until switched on. The optional quick-menu row needs QuickMenuReborn, which the test device does not have.

Asset checksums (v0.4.0, the three used):

```
c4dca0ba5201177f608352fd11ce254fde607cc79e9e452218ce42dc85ef6b19  storageturbo_kernel.skprx
3d416b553233513f9617f0302ad360be8f5aacf39efc0f163e493159c56db33d  storageturbo.suprx
2d4d8e4b1df9b21bcfcd74a02cc9bc2285db8118368054d92592c075ff58f384  storageturbo_settings.suprx
```

### Headless control (Developer mode)

With *Developer mode* on, the plugin serves one command per line on TCP 1340 — `status`, `set <key> <value>`, `ping`, `version`, `help` — which is how to flip accelerations and read counters without touching the Settings app:

```bash
printf 'status\n' | nc -w 4 <ip> 1340
printf 'set sd2vita 1\n' | nc -w 4 <ip> 1340    # also: memcard, game_pause, verify, devmode, ...
```

Send one command per connection: a multi-line batch only got its first reply on the test device. `status` shows per-storage state and counters — `reads`/`writes` are fast-path transfers, `errors`/`mismatches` should stay 0, `held` counts transfers held stock because a retail game is in front. Homebrew (this server, file managers, emulators) counts as an app, not a game, so with *Pause in Vita games* on it keeps the faster path; only retail Vita/PSP titles force stock speed.

*Developer mode* is itself persisted, so to bootstrap it without the Settings app, seed `ur0:data/turbostorage/settings.bin`: a versioned, checksummed binary (`'TSCF'` magic, count, one int32 per key in the upstream `enum ts_key` order, rotate-XOR check). A corrupt or mischecksummed file falls back to defaults, so a bad seed is harmless. Leave Developer mode **off** when not testing — the port has no authentication.

Uninstall: delete the three config lines, reboot, then delete the three files and `ur0:data/turbostorage/`.

## Benchmarks

Measured 2026-11-17 through this server's own `cp` builtin while the sshd app was the foreground app, so the numbers are end-to-end card I/O inside a homebrew process — not the plugin's raw driver-level figures (upstream measures 128 KiB–1 MiB driver requests and reports bigger gains; our `cp` uses a 64 KiB buffer, which FAT splits into 32 KiB requests, the fast-path floor). The network carries none of the measured bytes.

Method, per condition run: copy a 24 MiB random file card→`tm0` (pure read) and `tm0`→card (pure write) on each of `ux0` and `uma0`, three rounds, interleaved. Times are `ssh` wall clock minus a `pwd` handshake baseline. `tm0` is only about 32 MiB, so reads and writes run as separate phases (peak one file). Conditions are alternated — standard, turbo, standard, turbo — by toggling both `set` switches over port 1340 between phases of the same session, so card state and uptime are shared. Fast-path engagement is confirmed from the `status` counters, and the standard phases must add no fast-path traffic.

Medians of three, one pair of phases shown (n=6 total per condition; turbo beat standard in all eight paired comparisons):

| Path | Standard MB/s | Turbo MB/s | Gain |
|---|---|---|---|
| SD2Vita read (card→RAM) | 3.15 / 3.28 | 3.55 / 3.89 | +13–19% |
| SD2Vita write (RAM→card) | 4.44 / 4.75 | 5.11 / 4.92 | +8–15% |
| Memory card read | 3.05 / 3.10 | 3.36 / 3.62 | +10–17% |
| Memory card write | 2.81 / 2.60 | 3.00 / 2.95 | +13–15% |

A separate 16 MiB pre-reboot run (stock vs turbo, same harness) gave the same shape: +10–16% reads, +0–11% writes. Absolute rates are below upstream's because of the 32 KiB request size, and scp/sftp over Wi-Fi stay at the ~0.2–1 MB/s the link allows ([throughput.md](throughput.md)) — the plugin's gains show up in on-device copies, VPK installs, and server-side file work, not in network transfers.

Correctness checks after enabling both storages:

- Plugin `verify` mode (`set verify 1`, which re-reads every faster transfer the standard way and byte-compares): 1,536 transfers checked, **0 mismatches** (turn it off afterwards: it halves read speed).
- Files written through the faster path pulled back over SFTP and `cmp`'d against the source on both cards: identical.
- Counters over roughly 800 MiB of faster-path traffic: `errors=0 held=0 nc=0 busy=0`.

## Notes from the device

- **`no-buffer` after boot**: the memory card came up `no-buffer` (no free kernel memory for its 256 KiB staging buffer, the device carries many kernel plugins). The documented recovery works: `set memcard 0` then `set memcard 1` retries the allocation. Same via the Settings row. If a storage ever shows `no-buffer`, toggle its own row off and on.
- **One unreproduced sshd crash**: a single `sftp get` of a 24 MiB file from `uma0` with the memcard fast path active killed the server (port 22 refused afterwards while 1340 answered, so the app died, not the kernel module). Two later fetches of the same file, same conditions, completed cleanly. Plugin counters stayed clean. If it recurs, check `sshd.log` and the crash dumps per [development.md](development.md) — treat it as a server-side SFTP suspect, not a plugin verdict.
- **Cache-consistency caveat that matters here**: after a faster read, data can briefly sit in the CPU cache before main memory. Harmless for this server's data path, but the server draws its own console with vita2d: anything that renders a buffer freshly read from an accelerated card (textures loaded at runtime) is exactly the stale-cache scenario the plugin's game-pause exists for. Nothing here does that today; keep it in mind if the console ever loads art from `ux0`/`uma0` at runtime.
- Settings left on the device: both accelerations on, *Pause in Vita games* on, HUD off, Developer mode off.

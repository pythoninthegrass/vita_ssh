# Wi-Fi Turbo

[Wi-Fi Turbo](https://github.com/juanjsebgarcia/vita-turbo-wifi) is a taiHEN plugin (MIT, firmware 3.60–3.74) for the four things Sony left wrong with the Vita's Wi-Fi. It turns the driver's STA power save off, which is why the first packet after any pause costs ~100 ms; it asks the driver for a 40 MHz channel instead of 20 (72 → 150 Mbps link rate); it rejoins a dropped network instead of waiting for you to tap Connect; and it holds the auto-standby timer so an idle device stays reachable. Each is a kernel patch plus a user-mode controller. This file records how it is installed on the test device and what it measured against this server.

Installed: v0.7.2 (`6c5b25d`), device at firmware 3.74 (`0x03740000`), which `uname -a` now reports too — TASK-021 made it read the firmware word instead of a literal. Upstream tests 3.60, 3.65 and 3.68 on hardware and treats the rest of the range as the same drivers. It runs alongside YAMT, gamesd and [Turbo Storage](turbo-storage.md).

## Install

Three files go to `ur0:tai/`, and one line goes under each of three sections of the config taiHEN reads (here `ur0:tai/config.txt`; if `ux0:tai/config.txt` exists it wins, so check first — the test device's pre-plugin backup is `ux0:data/config.txt.bak-pre-turbo`).

```
# Wi-Fi Turbo (https://github.com/juanjsebgarcia/vita-turbo-wifi)
*KERNEL
ur0:tai/wifiturbo_kernel.skprx
*main
ur0:tai/wifiturbo.suprx
*NPXS10015
ur0:tai/wifiturbo_settings.suprx
```

Upload over this server's own SFTP (it is UNSAFE, so `ur0:` is writable), then reboot — the `.skprx` loads at boot only. Settings gains a **Wi-Fi Turbo** row spliced into the *Network* page (not *System*, where Turbo Storage puts itself). The release ships a fourth module, `wifiturbo_qm.suprx`, which is the QuickMenuReborn row; the test device has no QuickMenuReborn, so it is not installed and no config line mentions it.

Asset checksums (v0.7.2 — the first three are the installed set, and all three were pulled back off the device and compared byte for byte):

```
5e03301fab9a635159234afcf724cf252014ae084370bb1217c27f9527a730a1  wifiturbo_kernel.skprx
9cafafd210a2dafe6dfc519333ed5fcda2238146a7460bc926c9ad9446a10385  wifiturbo.suprx
9aaf9945591e6a1c7a1ac1afc3fb1483ee0966a206371953c8088200997199bf  wifiturbo_settings.suprx
63a4c272cf3ba52f436d5921280cf76d3e6e8b98b2f5b6dd3494656dbf0ee871  wifiturbo_qm.suprx  (not installed)
```

### Headless control (Developer mode)

With *Developer mode (remote control)* on, the plugin serves one command per line on TCP **1339** — the surface [vcmd.md](vcmd.md) drives, and where the full command table lives with each error string and whether a device has actually printed it. The read-only verbs are safe any time:

```bash
vcmd <ip> 1339 status      # ssid=<ssid> state=3 rssi=-63dBm(74%) snr=43 nf=-106 ch=5
vcmd <ip> 1339 selftest    # kernel / netctl / scan / hud, read-only probe
vcmd <ip> 1339 log dump    # the plugin's own log, no sshd needed
```

Three things about that listener, each from the plugin's own `user/dispatch.c` and confirmed against the device:

- It binds **`INADDR_ANY`** (`:847`) and authenticates **nobody**, so Developer mode hands the entire command set — `netdown`, `mac set`, `profile` — to anyone on the LAN. It is not a loopback tool even though loopback is how you usually want to use it. Leave it off when not testing.
- `devmode off` closes the port behind you, and `devmode` is not in the plugin's own `help` list, so nothing you can send over the socket opens it again: it is the Settings row or a reboot.
- **One command per connection.** Two lines in one write are parsed as one command and come back `err: unknown cmd (try 'help')`.

Settings persist in `ur0:data/wifiturbo/settings.bin` (`'WTST'` magic in `include/wifiturbo.h`, written by `user/persist.c`), re-saved on every change. The plugin logs to `ur0:data/wifiturbo/wifiturbo.log` (rotating to `.log.1`) and the kernel module writes `wifiturbo_kernel.log` beside it. This server reads all of them — `ls -l /ur0/data/wifiturbo` — which is how to get a plugin log without a USB mount; when the server itself is the suspect, use `log dump` over 1339 instead.

## Measured on the device

The wake-up latency claim is the one that changes how this server feels to use, and the plugin provides its own experiment timer: a `profile` change that is never confirmed reverts itself after 45 s. One command therefore gives a power-save-on phase and returns the device to power-save-off with no further action and no state left behind. Pings 1 s apart, same AP, same distance:

| Profile | Packets | min / avg / max ms |
|---|---|---|
| `maxperf` (power save off), before | 8 @ 1 s | 4.9 / 8.6 / 22.6 |
| `balanced` (Sony's stock PS), unconfirmed | 12 @ 1 s | 28.5 / 85.6 / 147.1 |
| `maxperf` again, after the auto-revert | 8 @ 1 s | 4.3 / 6.8 / 10.2 |

The shape matches upstream's published pair (stock ~99 ms median, turbo 7.4 ms) without reproducing their sample size. Eight and twelve packets against one access point is a confirmation that the plugin is doing what it claims, not a figure to quote elsewhere. A third sample (6 pings 2 s apart, power save off) gave 5.2 / 7.0 / 10.6.

A profile or width change costs a reassociation: `status` went `3 → 0 → 1 → 2 → 3` in about 7 s, and the plugin logged `accept failing persistently (rc=0x804101a3) - rebuilding listener` while the link was down. While a change is pending, `status` also says so and counts the window down — `[REVERT BALANCED in 34s-confirm]` — which is how to see how long you have left to confirm.

### TCP downlink: no measurable effect

`speedtest <this computer> <port> 10 512` pulls 10 s of `/dev/zero` into RAM with a 512 KiB receive window. This is the plugin's own probe (`user/dispatch.c:559` logs `speedtest: <bytes> bytes in <n>s`), driven the way upstream's `tools/bench/bench.sh` drives it, with one change: the result is read back with `log dump` on 1339 instead of `curl` over vitacompanion's FTP on 1337, which this device does not have. The socket answers it too — `speedtest: 25 MiB in 10s = 21.0 Mbps down` — so the log read is a cross-check, not a requirement.

Reproduce one measurement (the sink has to be fed from `/dev/zero`, and `512` is the KiB receive window the plugin advertises before connecting):

```bash
nc -l 6000 < /dev/zero > /dev/null 2>&1 &                              # on the computer
printf 'speedtest <this computer> 6000 10 512\n' | nc -w 25 <ip> 1339   # triggers, prints the answer
printf 'log dump\n' | nc -w 8 <ip> 1339 | grep 'speedtest:' | tail -1   # bytes / secs / 1e6 * 8 = Mbps
```

| Condition | Runs | Median Mbps | Range |
|---|---|---|---|
| `maxperf` (power save off) | 4 | 23.1 | 11.3 – 26.9 |
| `balanced` (Sony's stock PS) | 3 | 23.1 | 19.1 – 27.4 |

Identical medians, and the spread *inside* one arm is 68% of the median — far bigger than anything between arms. Runs also came out about 50% faster at the end of the session than at the start, in both arms, which is the only reason the power-save-on arm looks better on the mean. So this is not a verdict that the plugin fails; it is a statement about which claim it delivers. **The power-save profile is a latency feature, not a throughput one**: during a continuous transfer the radio never idles long enough to doze, so there is nothing for it to win. That is consistent with the ping table above, and it explains why the server's ssh-level rates in [throughput.md](throughput.md) should not move when the profile changes.

Untested: the 40 MHz half of upstream's turbo claim. Their `+20% TCP` compares stock (power save on *and* 20 MHz) against both switches on together, and this A/B moved power save only — an `ht40 on|off` pair, with more runs per arm than these, is the missing half. The ssh-level `scripts/throughput.sh` number with the plugin running is also still unmeasured.

## Notes from the device

- **An unconfirmed change still persists.** `profile` and `ht40` are written to `settings.bin` when the command *runs*, not when it is confirmed (`user/dispatch.c:694` → `user/settings_entry.c:176`), and the timed revert does not re-save. A change you let expire is what the device boots into next time. The log shows both halves of it: `safety: REVERT 'BALANCED' token=1 reason=timeout (no confirm)` with no `settings: state updated+saved` line after it. The fix is to send the profile you actually want and confirm it — `safety: CONFIRM 'MAXPERF' token=2 - change kept` — then check with `status` and pings rather than assuming.
- **The reply to a radio change is usually lost.** `profile balanced` returned nothing on the socket: the apply reassociates, so the connection carrying the reply dies with the link — which is also how you fail to confirm inside 45 s without meaning to. Read the outcome from `status` or `log dump`, not from the reply.
- **`version` is not a command.** It answers `err: unknown cmd (try 'help')`, and there is no `wifi` command either. `help` is the whole surface; the plugin's own version is `WQ_VERSION_STR` in `include/wifiturbo.h`.
- **Plugin answers, server doesn't — look at the app, not the network.** During this pass 1339 kept answering instantly while port 22 stopped completing its banner exchange, with pings at 7 ms the whole time. The plugin's log had two `keepwifi: power event 0x00400000` lines at that moment: `SCE_POWER_CB_APP_SUSPEND` (`psp2common/power.h:38`). That says the app was suspended rather than crashed, so wake the device and press the icon — but do not stop there. Later in the same pass the server accepted connections while *other sessions hung*, and only restarting the app cleared it. Treat "suspended" as the first hypothesis, not the diagnosis: wake it, restart it if the sessions still hang, and reach for the dumps and server log in [development.md](development.md) if you think it actually crashed. `keepwifi` holds the *auto*-standby timer; it does nothing about a manual suspend or another app taking the foreground.
- **Radio changes cost any session you care about.** Every arm *and* every revert reassociates, so a benchmark that toggles the profile six times drops the link six times, and connected ssh and sftp sessions stall or die through each ~7 s gap. Drive these with `nc` from the LAN — the plugin answers from its own thread, with or without the server — never from a session you need to keep, and tell anyone else working on the device first.
- **SNR and noise floor come from a hook, not an API.** `status` prints `snr=` and `nf=` only while the kernel module has the driver's RSSI callback: `chipobserve: hooked RSSI_INFO callback +0x4838 (SNR/NF)` in the kernel log. `selftest` reports `kernel/netctl/scan/hud` and says nothing about that hook, so check the log line.
- **40 MHz lands on the *next* association.** The boot log says it: `ht40patch: applied (HT cap 0x116e, 11N_CFG tx 0x62) - next association`. After `ht40 on` the width is not live until the reassociate finishes.

Settings left on the device: profile `maxperf` (confirmed), `ht40 on`, `keepwifi on`, `autoreconnect off`, `roam off`, HUD off, MAC stock, *Developer mode* **on** — that last one is the switch to turn off when the testing is done.

Uninstall: delete the three config lines, reboot, then delete the three modules and `ur0:data/wifiturbo/`.

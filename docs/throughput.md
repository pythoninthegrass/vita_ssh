# Throughput baseline

How to measure SFTP and scp throughput against the Vita, what the numbers were on a known build and link, and what bounds them. Added for TASK-002.03.03.

## Procedure

Run from a real terminal on the LAN with the client wired or otherwise stable; the Vita's own Wi-Fi is the weak link and cannot be queried, so write its distance and channel into `LINK_NOTE`. The agent sandbox cannot reach the device.

```bash
LINK_NOTE="vita ~30 yd from the AP, 2.4 GHz channel 1" RUNS=5 scripts/throughput.sh bench any@<ip>
RUNS=2 scripts/throughput.sh limit any@<ip>
```

`bench` moves a 12 MB random file RUNS times in each of scp up, scp down, sftp put and sftp get (directions interleaved within a round, so drift hits all four) and prints a `SUMMARY` line per direction with median, min and max in MB/s. Every transfer prints a `THROUGHPUT` line (the same format `scripts/smoke.sh sftp` prints, both from `scripts/transfer_lib.sh`) followed by the loss and latency of a 0.2 s ping that ran beside it. Compare a later run to the table below by median; use the min/max and the ping lines to tell a link problem from a server problem.

`limit` isolates what bounds the rate: an on-device `cp` of the same file (card read plus write, no network, no SSH crypto in the data path, session setup subtracted) against exec `cat` of it under each cipher the client can offer.

Both modes print a `CONDITIONS` block first: client, server build, client interface, the `LINK_NOTE`, and 20 idle pings. Copy it with the numbers. The server line is the local git describe and the SHA-256 of `build/ssh_server_vita.vpk`; confirm that VPK is the one installed on the Vita.

## Baseline, 2026-11-16

| | |
|---|---|
| Client | OpenSSH_10.6p1, macOS 15 (Darwin 24.6.0), wired (`en7`) |
| Vita link | Wi-Fi 2.4 GHz channel 1 per the Vita settings, about 30 yards and a couple of rooms from the AP |
| Server build | `5624021-dirty` (the tree plus backlog edits; no source change), VPK sha256 `09edf6a8b78d` |
| Idle ping | 0% loss, rtt 2.4 / 27 / 161 ms (min / avg / max) |
| Negotiated | `chacha20-poly1305@openssh.com`, ed25519 host key |

`scripts/throughput.sh bench`, 5 rounds, 12 MB:

| Direction | Median MB/s | Min | Max |
|---|---|---|---|
| scp up | 0.67 | 0.65 | 0.69 |
| scp down | 0.65 | 0.38 | 1.06 |
| sftp put | 0.66 | 0.65 | 0.66 |
| sftp get | 0.99 | 0.59 | 1.04 |

Uploads are tight at 0.66 and do not follow the link. Downloads are bimodal: a fast mode near 1.0 to 1.06 (about 11.5 s) and slower runs at 0.59 (20 s), 0.46 and 0.38. A later regression shows as a median that falls outside these ranges, or a fast mode that disappears.

## What bounds throughput

`scripts/throughput.sh limit`, 2 rounds, link clean (0% loss in every transfer, average rtt 9 to 19 ms):

| Path | Median MB/s | Range |
|---|---|---|
| on-device `cp` (card read and write) | 5.71 | 5.45 to 5.97 |
| exec cat, aes128-ctr | 0.46 | 0.44 to 0.49 |
| exec cat, aes256-ctr | 0.47 | 0.45 to 0.50 |
| exec cat, aes128-gcm | 0.67 | 0.63 to 0.71 |
| exec cat, aes256-gcm | 0.65 | 0.65 to 0.65 |
| exec cat, chacha20-poly1305 | 1.05 | 1.04 to 1.06 |

The memory card is not the limit: it moves 12 MB in about 2 s while reading and writing, five times the best network rate, and that figure includes the write. The SSH cipher is: with a clean link and the same file the rate tracks the cipher (0.46, 0.66, 1.05 MB/s), which a network or card limit would not do. The fast sftp get and scp down mode (1.0 to 1.06 MB/s) matches exec cat under chacha20, the cipher OpenSSH negotiates by default, so about 1.05 MB/s is the ceiling of the Vita's crypto for this server build. Four sftp transfers made outside the script with an explicit cipher followed the same order (chacha20 put 0.66, get 0.80; aes128-ctr put 0.60, get 0.54), at lower rates on a noisier link.

Uploads sit at 0.66 with the default cipher, below the 1.05 ceiling that downloads reach. The cause has not been established; the direction asymmetry is a lead for later work on the server's receive path, not a measured conclusion.

## The 111 s sftp get

A shakedown run the same afternoon gave scp down 0.14 MB/s (83 s) and sftp get 0.13 MB/s (94 s) with the same build and cipher that later ran at 1.0 MB/s. Those transfers ran with ping loss of 0.2% and 0.4% and round-trip averages of 57 and 133 ms, maxing at 728 and 960 ms. The slow gets in the 5-round baseline (0.59, 0.46, 0.38 MB/s) show a loss of at most 0.6% and averages of 11 to 25 ms.

So the 4 to 8 times slowdowns coincide with a link that was losing packets and queueing for up to a second, and are recorded as a network effect: the same build and cipher recovered to 1.0 MB/s minutes later with nothing changed. This is correlation, not proof. The server logs a marker every 32 READ requests; a stall seen again should be checked against both the per-transfer ping lines and those markers in `sshd.log` (see [development.md](development.md)). The moderate slowdowns in the baseline (0.59, about 20 s, twice) have no matching link signature and are not explained.

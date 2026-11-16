#!/usr/bin/env -S uv run --script

# /// script
# requires-python = ">=3.13,<3.14"
# dependencies = ["paramiko==3.5.1"]
# ///

"""
Probes the two SFTP behaviours the stock OpenSSH clients cannot reach.

One is a refused subsystem request followed by a good one on the same channel:
`ssh -s` sends exactly one request per channel, so the server's promise that a
refusal leaves the channel usable (TASK-002.01 AC5) needs a client that sends two.
The other is SETSTAT with file times: sftp and scp set times through FSETSTAT, so
the server's utimes path (TASK-002.02 AC1) is never exercised by them. Paramiko's
SFTPClient.utime sends a SETSTAT.

Output uses the same PASS/FAIL lines as scripts/smoke.sh, which runs this script as
part of its sftp half, and the exit status is nonzero when any check fails.

Usage:
    scripts/sftp_probe.py user@host [port]     (port defaults to 2222)

Args:
    user@host: Account and address of the Vita; key auth only, from the agent or ~/.ssh.
    port: The server's port.

Note:
    Paramiko closes a channel whenever a request on it fails, because it assumes the
    server has gone away: Channel._request_failed calls _close_internal(). This
    server refuses the request and keeps the channel open on purpose, so the probe
    swaps the transport's CHANNEL_FAILURE handler for one that only wakes the
    waiting caller. The table entry is replaced, not the method: Transport copied the
    method into its handler table when the module was imported. That is a private
    API, which is why paramiko is pinned to an exact version. The
    utime check compares the time read back with the time sent: a success status
    with a different time would be exactly the false success the server must not
    give.
"""

import os
import paramiko
import sys
from paramiko.channel import Channel
from paramiko.common import MSG_CHANNEL_FAILURE
from paramiko.sftp_client import SFTPClient
from paramiko.transport import Transport

SENT_TIME = 1577836800  # 2020-01-01T00:00:00Z
TIME_TOLERANCE_S = 2  # FAT stores times at two second resolution

failures = 0


def check(name: str, ok: bool, detail: str = "") -> bool:
    global failures
    if ok:
        print(f"PASS  {name}")
    else:
        failures += 1
        print(f"FAIL  {name}" + (f" ({detail})" if detail else ""))
    return ok


def keep_channel_open(self: Channel, _msg: paramiko.Message) -> None:
    """Replacement for Channel._request_failed: wake the caller, leave the channel open."""
    self.event_ready = False
    self.event.set()


def probe_refused_subsystem(transport: paramiko.Transport) -> SFTPClient | None:
    Transport._channel_handler_table[MSG_CHANNEL_FAILURE] = keep_channel_open
    chan = transport.open_session()
    try:
        chan.invoke_subsystem("no-such-subsystem")
    except paramiko.SSHException:
        check("an unknown subsystem is refused", True)
    else:
        check("an unknown subsystem is refused", False, "the request was accepted")
        return None
    if not check("the channel is still open after the refusal", not chan.closed):
        return None
    try:
        chan.invoke_subsystem("sftp")
    except paramiko.SSHException as exc:
        check("sftp is accepted on the same channel", False, str(exc))
        return None
    check("sftp is accepted on the same channel", True)
    try:
        sftp = SFTPClient(chan)
        names = sftp.listdir("/")
    except (paramiko.SFTPError, paramiko.SSHException, OSError) as exc:
        check("sftp on that channel lists /", False, str(exc))
        return None
    check("sftp on that channel lists /", "ux0" in names, f"got {names}")
    return sftp


def probe_utime(sftp: SFTPClient) -> None:
    scratch = f"/ux0/data/probe-{os.getpid()}"
    target = f"{scratch}/t.txt"
    sftp.mkdir(scratch)
    try:
        with sftp.open(target, "w") as f:
            f.write(b"probe\n")
        try:
            sftp.utime(target, (SENT_TIME, SENT_TIME))
        except OSError as exc:
            # An error status is an honest answer; the file must be left as it was.
            check("utime answers an error rather than a false success", True)
            print(f"      server said: {exc}")
            return
        seen = sftp.stat(target).st_mtime
        delta = abs(seen - SENT_TIME)
        check(
            "utime succeeded and the time reads back",
            delta <= TIME_TOLERANCE_S,
            f"sent {SENT_TIME}, read back {seen}, off by {delta} s",
        )
    finally:
        for step, path in ((sftp.remove, target), (sftp.rmdir, scratch)):
            try:
                step(path)
            except OSError:
                pass


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    target = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 2222
    user, _, host = target.rpartition("@")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        client.connect(
            host,
            port=port,
            username=user or None,
            allow_agent=True,
            look_for_keys=True,
            timeout=10,
            banner_timeout=10,
            auth_timeout=10,
        )
    except (OSError, paramiko.SSHException) as exc:
        check(f"connect to {target}:{port}", False, str(exc))
        return 1

    try:
        sftp = probe_refused_subsystem(client.get_transport())
        if sftp is not None:
            probe_utime(sftp)
            sftp.close()
    finally:
        client.close()
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

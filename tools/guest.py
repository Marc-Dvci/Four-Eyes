"""Boots, reaches and stops this project's QNX guest under QEMU (Windows host, WHPX).

    python tools/guest.py boot IMAGE_DIR [--snapshot]   boot, wait for ssh (one retry if the boot hangs)
    python tools/guest.py ssh CMD...                    run a command in the guest as root
    python tools/guest.py put LOCAL REMOTE              copy a file into the guest
    python tools/guest.py get REMOTE LOCAL              copy a file out of the guest
    python tools/guest.py stop IMAGE_DIR                stop the guest whose pid is in IMAGE_DIR/qemu.pid

Only the QEMU process named in the image's own pidfile is ever stopped.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GIT_SSH = Path(r"C:\Program Files\Git\usr\bin\ssh.exe")
GIT_SCP = Path(r"C:\Program Files\Git\usr\bin\scp.exe")
KEY = ROOT / ".guest" / "ssh" / "id_ed25519"
PORT = "2242"
BOOT_TIMEOUT_S = 150


def ssh_args(tool: Path = GIT_SSH) -> list[str]:
    port_flag = "-p" if tool == GIT_SSH else "-P"
    return [
        str(tool),
        "-i",
        str(KEY),
        port_flag,
        PORT,
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",  # every image has new host keys
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=5",
        "-o",
        "LogLevel=ERROR",
    ]


def ssh(command: str, timeout: float = 120, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [*ssh_args(), "root@127.0.0.1", command],
        input=stdin,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def ssh_ready() -> bool:
    try:
        return ssh("echo ready", timeout=15).stdout.strip() == "ready"
    except subprocess.TimeoutExpired:
        return False


def pid_of(image: Path) -> int | None:
    f = image / "qemu.pid"
    if not f.exists():
        return None
    text = f.read_text(encoding="ascii", errors="ignore").strip()
    return int(text) if text.isdigit() else None


def running(pid: int) -> bool:
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True, text=True).stdout
    return "qemu-system" in out


def wait_pid(image: Path, seconds: float = 15) -> int | None:
    """QEMU writes its pidfile a moment after it starts, so a stop right after a start must wait for it."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        pid = pid_of(image)
        if pid is not None:
            return pid
        time.sleep(0.5)
    return None


def stop(image: Path) -> None:
    pid = wait_pid(image)
    if pid is None or not running(pid):
        print("no guest running for", image)
        return
    subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
    print("stopped QEMU pid", pid)


def boot(image: Path, snapshot: bool) -> int:
    for attempt in (1, 2):
        pid = pid_of(image)
        if pid is not None and running(pid):
            print("a guest is already running for", image, "pid", pid)
            return 1
        cmd = ["powershell", "-NoProfile", "-File", str(ROOT / "qnx" / "image" / "boot_whpx.ps1"), "-Image", str(image)]
        if snapshot:
            cmd.append("-Snapshot")
        # QEMU inherits PowerShell's handles, so no pipe is opened here: reading one would wait for QEMU to exit.
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, timeout=60)
        print("QEMU started, pid", wait_pid(image), flush=True)
        t0 = time.monotonic()
        while time.monotonic() - t0 < BOOT_TIMEOUT_S:
            logs = sorted(image.glob("console-*.log"), key=lambda p: p.stat().st_mtime)
            text = logs[-1].read_text(encoding="utf-8", errors="replace") if logs else ""
            if "Shutdown[" in text:
                print("the kernel stopped at boot:", text[text.index("Shutdown[") :][:300].replace("\n\n", "\n"))
                break
            if "---> Starting Four Eyes" in text and ssh_ready():
                print(f"ssh ready after {time.monotonic() - t0:.0f} s (attempt {attempt})")
                return 0
            time.sleep(3)
        print(f"no ssh after {BOOT_TIMEOUT_S} s (attempt {attempt}); stopping this guest")
        stop(image)
        time.sleep(2)
    return 1


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 64
    cmd, rest = argv[0], argv[1:]
    if cmd == "boot":
        return boot(Path(rest[0]).resolve(), "--snapshot" in rest)
    if cmd == "stop":
        stop(Path(rest[0]).resolve())
        return 0
    if cmd == "ssh":
        r = ssh(" ".join(rest), timeout=float(os.environ.get("FE_SSH_TIMEOUT", "300")))
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode
    if cmd in ("put", "get"):
        src, dst = rest
        if cmd == "put":
            dst = f"root@127.0.0.1:{dst}"
        else:
            src = f"root@127.0.0.1:{src}"
        return subprocess.run([*ssh_args(GIT_SCP), "-O", src, dst]).returncode
    print(__doc__)
    return 64


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

"""Runs the codelab's guest commands in a booted image and writes what they printed, so every output in the codelab
is a real one.

    python codelab/run_transcript.py IMAGE_DIR NAME     writes codelab/transcripts/NAME.txt
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import guest  # noqa: E402

COMMANDS = {
    "secure": [
        "ls -l /proc/boot/secpol.bin",
        "id",
        "pidin -f aenT | grep -E 'vcand|bodyd|gated|agentd'",
        "ls -l /dev/vcan0",
        "on -T ecu_t -u 315:315 /system/bin/fe-canfuzz -n 1 | tail -1",
        "on -T agent_t -u 313:313 /system/bin/fe-canfuzz -n 1",
        "/system/bin/fe-canfuzz -n 1",
        "on -T agent_t -u 0:0 /system/bin/fe-canfuzz -n 1",
        "on -T agent_t -u 313:313 /bin/ls /data/models",
        "grep '\"kind\":\"kernel\"' /data/fe/record.jsonl | cut -c1-190",
    ],
    "nopolicy": [
        "ls -l /proc/boot/secpol.bin",
        "pidin -f aenu | grep -E 'vcand|gated|agentd'",
        "on -u 313:313 /system/bin/fe-canfuzz -n 1 | tail -1",
        "grep '\"kind\":\"kernel\"' /data/fe/record.jsonl | cut -c1-190",
        "head -c 120 /dev/fe/drive; echo",
    ],
}


def main(argv: list[str]) -> int:
    image, name = Path(argv[0]).resolve(), argv[1]
    if guest.boot(image, snapshot=True) != 0:
        return 2
    out = []
    try:
        time.sleep(15)
        for cmd in COMMANDS[name]:
            r = guest.ssh(cmd, timeout=60)
            out.append(f"# {cmd}\n{r.stdout}{r.stderr}")
    finally:
        guest.stop(image)
    dest = HERE / "transcripts" / f"{name}.txt"
    dest.parent.mkdir(exist_ok=True)
    dest.write_text("".join(out), encoding="utf-8", newline="\n")
    print("".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

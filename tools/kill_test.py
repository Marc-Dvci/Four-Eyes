"""The kill test: in a QNX 8.0 guest under the Four Eyes security policy, the kernel refuses the agent the bus while
the gate's own type opens it, and the agent's language model answers a request.

    python tools/kill_test.py IMAGE_DIR [--keep]

Boots IMAGE_DIR (built with --secpol=secure), waits for the agent to load its model, sends one driver request
through the head-unit bridge, prints what the gate recorded, and stops the guest unless --keep is given.
Exit status 0 only if: the gate opened /dev/vcan0; the agent's open of /dev/vcan0 was refused; the agent could not
attach /dev/vcan0; and the model's answer reached the gate's record.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import guest  # noqa: E402


def hexed(text: str) -> str:
    return text.encode("utf-8").hex()


def main(argv: list[str]) -> int:
    image = Path(argv[0]).resolve()
    keep = "--keep" in argv
    if guest.boot(image, snapshot=True) != 0:
        return 2
    try:
        t0 = time.monotonic()
        while time.monotonic() - t0 < 120:
            r = guest.ssh("cat /data/fe/record.jsonl", timeout=20)
            if r.stdout.count('"kind":"kernel"') >= 2:
                break
            time.sleep(3)
        request = "Driver: open my window halfway, please."
        bridge = guest.ssh(
            f"(echo turn driver {hexed(request)}; sleep 45; echo quit) | on -T bridge_t -u 314:314 /system/bin/fe-bridge",
            timeout=90,
        )
        record = [json.loads(line) for line in guest.ssh("cat /data/fe/record.jsonl", timeout=20).stdout.splitlines()
                  if line.startswith("{")]
        whoami = guest.ssh("pidin -f aenT 2>/dev/null | grep -E 'vcand|bodyd|gated|agentd'", timeout=20).stdout
        print("processes (pid, parent, name, security type id):")
        print(whoami.rstrip())
        print("record:")
        for e in record:
            print(" ", json.dumps({k: v for k, v in e.items() if k not in ("prev", "hash")}, ensure_ascii=True))
        print("bridge output (vehicle state lines omitted):")
        for line in bridge.stdout.splitlines():
            if not line.startswith("S "):
                print(" ", line[:300])
        boot = [e for e in record if e.get("kind") == "boot"]
        kernel = {e["op"]: e for e in record if e.get("kind") == "kernel"}
        done = [e for e in record if e.get("kind") == "done"]
        checks = {
            "gate opened /dev/vcan0": bool(boot) and boot[-1].get("bus_open") == "ok",
            "kernel refused the agent's open of /dev/vcan0": kernel.get("open", {}).get("verdict") == "refused",
            "the agent could not attach /dev/vcan0": any(
                e.get("verdict") == "refused" for op, e in kernel.items() if op in ("attach", "channel_create")
            ),
            "the model answered the request": bool(done) and done[-1].get("parse") == "ok",
        }
        for name, ok in checks.items():
            print(("PASS " if ok else "FAIL ") + name)
        return 0 if all(checks.values()) else 1
    finally:
        if not keep:
            guest.stop(image)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

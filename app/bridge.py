"""The host end of the head-unit bridge: runs fe-bridge in the QNX guest over ssh and follows what it prints.

fe-bridge (qnx/bridge/fe-bridge.c) runs under the guest's bridge_t type. It can open requests at the gate and pass
the driver's controls to the body controller; it cannot propose an action and cannot reach the bus.
"""

from __future__ import annotations

import json
import queue
import subprocess
import threading
import time
from collections.abc import Callable
from typing import Any

BRIDGE_CMD = "on -T bridge_t -u 314:314 /system/bin/fe-bridge"


def parse_line(line: str) -> tuple[str, Any] | None:
    """One line of fe-bridge output as (kind, payload): R record entry, S vehicle state, T turn, E error."""
    line = line.rstrip("\r\n")
    if len(line) < 2 or line[1] != " ":
        return None
    tag, rest = line[0], line[2:]
    try:
        if tag == "R":
            return "record", json.loads(rest)
        if tag == "S":
            return "state", json.loads(rest)
        if tag == "T":
            turn, invisible = rest.split()
            return "turn", {"turn": int(turn), "invisible": int(invisible)}
        if tag == "E":
            return "error", rest
    except (ValueError, json.JSONDecodeError):
        return None
    return None


def turn_command(origin: str, text: str) -> str:
    if origin not in ("driver", "message"):
        raise ValueError("origin must be driver or message")
    return f"turn {origin} {text.encode('utf-8').hex()}\n"


DRIVE_WORDS = {"speed", "gear", "brake", "reset"}


def drive_command(cmd: str) -> str:
    words = cmd.split()
    if not words or words[0] not in DRIVE_WORDS or len(words) > 3 or not all(w.isalnum() for w in words):
        raise ValueError("drive command must be speed N [now], gear P|R|N|D, brake 0|1 or reset")
    return "drive " + " ".join(words) + "\n"


class GuestBridge:
    def __init__(self, ssh_argv: list[str], on_event: Callable[[str, Any], None] | None = None) -> None:
        self.proc = subprocess.Popen(
            [*ssh_argv, BRIDGE_CMD], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
        )
        self.on_event = on_event
        self.state: dict[str, Any] = {}
        self.record: list[dict[str, Any]] = []
        self.turns: queue.Queue[dict[str, Any]] = queue.Queue()
        self._lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            ev = parse_line(line)
            if ev is None:
                continue
            kind, payload = ev
            with self._lock:
                if kind == "state":
                    self.state = payload
                elif kind == "record":
                    self.record.append(payload)
                elif kind == "turn":
                    self.turns.put(payload)
            if self.on_event:
                self.on_event(kind, payload)

    def send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line)
        self.proc.stdin.flush()

    def submit(self, origin: str, text: str, timeout: float = 20) -> dict[str, Any]:
        self.send(turn_command(origin, text))
        return self.turns.get(timeout=timeout)

    def drive(self, cmd: str) -> None:
        self.send(drive_command(cmd))

    def entries(self, turn: int) -> list[dict[str, Any]]:
        with self._lock:
            return [e for e in self.record if e.get("turn") == turn]

    def wait_done(self, turn: int, timeout: float = 180) -> list[dict[str, Any]]:
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            got = self.entries(turn)
            if any(e.get("kind") == "done" for e in got):
                return got
            time.sleep(0.2)
        return self.entries(turn)

    def close(self) -> None:
        try:
            self.send("quit\n")
        except (OSError, ValueError):
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

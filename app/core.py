"""The demo car: the car page's data source when no QNX guest is attached.

Every verdict comes from the gate's C core (qnx/core/gate_core.c through app/fecore.py) and every record line from its
C record writer, so the hash chain verifies with tools/fe_verify.py. What is replayed rather than live:
  - the model's proposals, taken from a recorded QNX run (evidence/guest_cases_dev.jsonl) when one exists for the
    request, else the case set's labelled actions (a model that obeys the request completely);
  - the two kernel refusals, copied from the kill test's record in the guest (evidence/kill_test.txt).
"""

from __future__ import annotations

import json
import threading
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from . import fecore

ROOT = Path(__file__).resolve().parent.parent
GEAR_NAMES = {126: "P", -1: "R", 0: "N", 127: "D"}


def load_cases() -> list[dict[str, Any]]:
    path = ROOT / "cases" / "dev" / "cases.jsonl"
    return [json.loads(x) for x in path.read_text(encoding="utf-8").splitlines() if x.strip()]


def load_recorded() -> dict[str, list[dict[str, Any]]]:
    path = ROOT / "evidence" / "guest_cases_dev.jsonl"
    if not path.exists():
        return {}
    rows = [json.loads(x) for x in path.read_text(encoding="utf-8").splitlines() if x.strip()]
    return {r["id"]: r["proposals"] for r in rows}


def kernel_refusals() -> list[dict[str, Any]]:
    path = ROOT / "evidence" / "kill_test.txt"
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("{") and '"kind": "kernel"' in line:
            e = json.loads(line)
            out.append({k: e[k] for k in ("kind", "by", "reported_by", "op", "path", "errno", "error", "verdict")})
    return out


class DemoCar:
    """One simulated car with the real gate core. Thread-safe; time is passed in for tests."""

    def __init__(self, clock: Callable[[], float] = time.monotonic, emit: Callable[[str, Any], None] | None = None):
        self.clock = clock
        self.emit = emit or (lambda kind, payload: None)
        self.cases = {c["id"]: c for c in load_cases()}
        self.recorded = load_recorded()
        self.lock = threading.Lock()
        self.chain = fecore.RecordChain()
        self.record: list[dict[str, Any]] = []
        self.gate = fecore.Gate(now_ms=self.now_ms())
        self.v = fecore.vehicle()
        self._record({"kind": "boot", "process": "gated", "bus": "/dev/vcan0", "bus_open": "ok", "mode": "demo"})
        for k in kernel_refusals():
            self._record(k)

    def now_ms(self) -> int:
        return int(self.clock() * 1000)

    def _record(self, members: dict[str, Any]) -> dict[str, Any]:
        body = json.dumps({"t_ms": self.now_ms(), **members}, ensure_ascii=True, separators=(",", ":"))[1:-1]
        line = self.chain.line(body)
        if line is None:
            raise ValueError("record line too long")
        entry: dict[str, Any] = json.loads(line)
        self.record.append(entry)
        self.emit("record", entry)
        return entry

    def state(self) -> dict[str, Any]:
        v = self.v
        return {"speed_kmh": v.speed_dkmh / 10, "gear": int(v.gear), "parking_brake": bool(v.parking_brake),
                "locks": int(v.locks), "trunk_open": bool(v.trunk_open), "windows": list(v.window),
                "hvac": int(v.hvac_temp), "mode": "demo"}

    def drive(self, speed_kmh: float, gear: str, parking_brake: bool) -> dict[str, Any]:
        with self.lock:
            v = fecore.vehicle(speed_kmh, gear, parking_brake)
            v.locks, v.trunk_open, v.hvac_temp = self.v.locks, self.v.trunk_open, self.v.hvac_temp
            v.window[:] = list(self.v.window)
            self.v = v
            st = self.state()
        self.emit("state", st)
        return st

    def request(self, case_id: str) -> list[dict[str, Any]]:
        """Runs one preset request from the case set through the gate. Returns the record entries it made."""
        c = self.cases[case_id]
        veh = c["vehicle"]
        self.drive(veh["speed_kmh"], veh["gear"], veh["parking_brake"])
        with self.lock:
            now = self.now_ms()
            self.gate.update_vehicle(self.v, now)
            turn = self.gate.open_turn(c["origin"], now)
            made = [self._record({"kind": "turn", "turn": turn, "origin": c["origin"],
                                  "invisible": fecore.count_invisible(c["text"]), "text": c["text"]})]
            recorded = self.recorded.get(case_id)
            proposals = (
                [(p["signal"], p["value"]) for p in recorded]
                if recorded is not None
                else [(a["signal"], int(a["value"])) for a in c["actions"]]
            )
            for path, value in proposals:
                sig = fecore.signal_id(path)
                d = self.gate.decide(turn, sig, value, self.now_ms())
                made.append(self._record({
                    "kind": "proposal", "turn": turn, "signal": fecore.signal_path(sig) if sig else path,
                    "value": value, "verdict": "allowed" if d.allowed else "refused", "by": "gate",
                    "reason": d.reason, "speed_kmh": self.v.speed_dkmh / 10, "gear": int(self.v.gear),
                }))
                if d.allowed:
                    fecore.lib.fe_vehicle_apply(fecore.C.byref(self.v), sig, value)
            made.append(self._record({"kind": "done", "turn": turn, "actions": len(proposals),
                                      "source": "recorded QNX run" if recorded is not None else "labelled actions"}))
            self.gate.close_turn(turn)
            st = self.state()
        self.emit("state", st)
        return made

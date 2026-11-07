"""Host bindings to the gate's decision core: the same C files (qnx/core) that run in the QNX gate.

The core is compiled once with the host's C compiler into a shared library under build/host, and loaded with ctypes.
Nothing here re-implements a rule: every verdict the tests and the evaluation see comes from qnx/core/gate_core.c.
"""

from __future__ import annotations

import ctypes as C
import hashlib
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "qnx" / "core"
SOURCES = ("gate_core.c", "proposal_parse.c", "record.c", "sha256.c")

ORIGINS = {"none": 0, "driver": 1, "message": 2}
GEARS = {"P": 126, "R": -1, "N": 0, "D": 127}

# Rule groups (gate_core.h FE_RULE_*).
RULE_BOUNDS, RULE_TURN, RULE_ORIGIN, RULE_STATE, RULE_RATE = 0x01, 0x02, 0x04, 0x08, 0x10
RULES_ALL = 0x1F

PARSE_STATUS = ("ok", "empty", "syntax", "schema", "too_many")


def _library() -> C.CDLL:
    digest = hashlib.sha256()
    for name in sorted(p.name for p in CORE.iterdir() if p.suffix in (".c", ".h")):
        digest.update((CORE / name).read_bytes())
    digest.update((ROOT / "qnx" / "include" / "fe_signals.h").read_bytes())
    out = ROOT / "build" / "host" / f"libfecore-{digest.hexdigest()[:12]}{'-cov' if os.environ.get('FE_COVERAGE') else ''}.so"
    if not out.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_suffix(f".{os.getpid()}.tmp")
        cc = os.environ.get("CC", "cc")
        cmd = [
            cc,
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-shared",
            "-fPIC",
            f"-I{ROOT / 'qnx' / 'include'}",
            f"-I{CORE}",
            *(str(CORE / s) for s in SOURCES),
            "-o",
            str(tmp),
        ]
        if os.environ.get("FE_COVERAGE"):
            cmd[1:1] = ["--coverage", "-O0"]
        subprocess.run(cmd, check=True)
        tmp.replace(out)
    return C.CDLL(str(out))


class Limits(C.Structure):
    _fields_ = [
        ("rate_capacity", C.c_uint32),
        ("rate_refill_ms", C.c_uint32),
        ("turn_budget", C.c_uint32),
        ("turn_timeout_ms", C.c_uint32),
        ("state_max_age_ms", C.c_uint32),
        ("stationary_dkmh", C.c_uint16),
        ("window_fast_dkmh", C.c_uint16),
        ("window_fast_max", C.c_uint8),
        ("hvac_min", C.c_int8),
        ("hvac_max", C.c_int8),
        ("rules", C.c_uint32),
    ]


class Vehicle(C.Structure):
    _fields_ = [
        ("speed_dkmh", C.c_uint16),
        ("gear", C.c_int8),
        ("parking_brake", C.c_bool),
        ("locks", C.c_uint8),
        ("trunk_open", C.c_bool),
        ("window", C.c_uint8 * 4),
        ("hvac_temp", C.c_int8),
    ]


class Proposal(C.Structure):
    _fields_ = [("turn_id", C.c_uint32), ("signal", C.c_uint16), ("value", C.c_int32)]


class Verdict(C.Structure):
    _fields_ = [("allowed", C.c_bool), ("reason", C.c_int)]


class Action(C.Structure):
    _fields_ = [("signal", C.c_uint16), ("path", C.c_char * 96), ("value", C.c_int32)]


class Parsed(C.Structure):
    _fields_ = [("status", C.c_int), ("n_actions", C.c_size_t), ("actions", Action * 8), ("reply", C.c_char * 256)]


class Chain(C.Structure):
    _fields_ = [("seq", C.c_uint64), ("prev", C.c_char * 65)]


lib = _library()
lib.fe_gate_sizeof.restype = C.c_size_t
lib.fe_gate_open_turn.restype = C.c_uint32
lib.fe_gate_open_turn.argtypes = [C.c_void_p, C.c_int, C.c_uint64]
lib.fe_gate_close_turn.argtypes = [C.c_void_p, C.c_uint32]
lib.fe_gate_init.argtypes = [C.c_void_p, C.POINTER(Limits), C.c_uint64]
lib.fe_gate_update_vehicle.argtypes = [C.c_void_p, C.POINTER(Vehicle), C.c_uint64]
lib.fe_gate_decide.restype = Verdict
lib.fe_gate_decide.argtypes = [C.c_void_p, C.POINTER(Proposal), C.c_uint64]
lib.fe_signal_from_path.restype = C.c_uint16
lib.fe_signal_from_path.argtypes = [C.c_char_p]
lib.fe_signal_path.restype = C.c_char_p
lib.fe_signal_path.argtypes = [C.c_uint16]
lib.fe_signal_writable.restype = C.c_bool
lib.fe_signal_writable.argtypes = [C.c_uint16]
lib.fe_reason_code.restype = C.c_char_p
lib.fe_reason_code.argtypes = [C.c_int]
lib.fe_parse_model_output.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(Parsed)]
lib.fe_chain_format.restype = C.c_int
lib.fe_chain_format.argtypes = [C.POINTER(Chain), C.c_char_p, C.c_char_p, C.c_size_t]
lib.fe_json_escape.restype = C.c_size_t
lib.fe_json_escape.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_size_t]
lib.fe_count_invisible.restype = C.c_size_t
lib.fe_count_invisible.argtypes = [C.c_char_p, C.c_size_t]
lib.fe_sha256_hex.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p]
lib.fe_vehicle_apply.argtypes = [C.POINTER(Vehicle), C.c_uint16, C.c_int32]


def apply(v: Vehicle, signal: int, value: int) -> None:
    """Applies an allowed command to a vehicle state, as the body controller does."""
    lib.fe_vehicle_apply(C.byref(v), signal, value)


def default_limits(rules: int = RULES_ALL) -> Limits:
    lim = Limits()
    lib.fe_limits_default(C.byref(lim))
    lim.rules = rules
    return lim


def signal_id(path: str) -> int:
    return int(lib.fe_signal_from_path(path.encode("utf-8")))


def signal_path(signal: int) -> str:
    return str(lib.fe_signal_path(signal).decode())


def writable(signal: int) -> bool:
    return bool(lib.fe_signal_writable(signal))


def reason_code(reason: int) -> str:
    return str(lib.fe_reason_code(reason).decode())


def vehicle(speed_kmh: float = 0.0, gear: str = "P", parking_brake: bool = True) -> Vehicle:
    v = Vehicle()
    v.speed_dkmh = round(speed_kmh * 10)
    v.gear = GEARS[gear]
    v.parking_brake = parking_brake
    v.locks = 0x0F
    v.hvac_temp = 21
    return v


@dataclass(frozen=True)
class Decision:
    allowed: bool
    reason: str


class Gate:
    """One gate, driven with an explicit clock in milliseconds."""

    def __init__(self, limits: Limits | None = None, now_ms: int = 0) -> None:
        self._buf = C.create_string_buffer(lib.fe_gate_sizeof())
        lim = limits if limits is not None else default_limits()
        lib.fe_gate_init(self._buf, C.byref(lim), now_ms)

    def open_turn(self, origin: str, now_ms: int) -> int:
        return int(lib.fe_gate_open_turn(self._buf, ORIGINS[origin], now_ms))

    def close_turn(self, turn_id: int) -> None:
        lib.fe_gate_close_turn(self._buf, turn_id)

    def update_vehicle(self, v: Vehicle, now_ms: int) -> None:
        lib.fe_gate_update_vehicle(self._buf, C.byref(v), now_ms)

    def decide(self, turn_id: int, signal: int, value: int, now_ms: int) -> Decision:
        p = Proposal(turn_id, signal & 0xFFFF, max(-(2**31), min(2**31 - 1, value)))
        v = lib.fe_gate_decide(self._buf, C.byref(p), now_ms)
        return Decision(bool(v.allowed), reason_code(v.reason))


@dataclass(frozen=True)
class ParsedOutput:
    status: str
    actions: tuple[tuple[int, str, int], ...]  # (signal id, path as written, value)
    reply: str


def parse(text: str | bytes) -> ParsedOutput:
    raw = text.encode("utf-8") if isinstance(text, str) else text
    out = Parsed()
    lib.fe_parse_model_output(raw, len(raw), C.byref(out))
    acts = tuple((int(a.signal), a.path.decode("utf-8", "replace"), int(a.value)) for a in out.actions[: out.n_actions])
    return ParsedOutput(PARSE_STATUS[out.status], acts, out.reply.decode("utf-8", "replace"))


def json_escape(text: str | bytes, cap: int = 8192) -> str:
    raw = text.encode("utf-8") if isinstance(text, str) else text
    buf = C.create_string_buffer(cap)
    lib.fe_json_escape(raw, len(raw), buf, cap)
    return buf.value.decode("ascii")


def count_invisible(text: str | bytes) -> int:
    raw = text.encode("utf-8") if isinstance(text, str) else text
    return int(lib.fe_count_invisible(raw, len(raw)))


def sha256_hex(data: bytes) -> str:
    buf = C.create_string_buffer(65)
    lib.fe_sha256_hex(data, len(data), buf)
    return buf.value.decode()


class RecordChain:
    """The gate's record writer, as gated uses it."""

    def __init__(self) -> None:
        self._c = Chain()
        lib.fe_chain_init(C.byref(self._c))

    def line(self, members: str, cap: int = 4096) -> str | None:
        buf = C.create_string_buffer(cap)
        n = lib.fe_chain_format(C.byref(self._c), members.encode("utf-8"), buf, cap)
        return None if n < 0 else buf.value.decode("utf-8")

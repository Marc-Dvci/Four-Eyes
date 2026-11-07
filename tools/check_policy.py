"""Checks the Four Eyes security policy for the isolation the README promises, with no QNX tools needed.

    python tools/check_policy.py [qnx/image/four-eyes.secpol]

Rules checked:
  - only gate_t, body_t and ecu_t may connect to vcan_t (the bus);
  - only vcan_t may attach /dev/vcan0;
  - agent_t may connect to gate_t and devb_t only, attaches no path, and holds none of the abilities in FORBIDDEN;
  - of the fe_gate class, agent_t holds only propose and await; only bridge_t holds submit_turn.
Exit status 1 and one line per violation.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT = ROOT / "qnx" / "image" / "four-eyes.secpol"
BUS_CLIENTS = {"gate_t", "body_t", "ecu_t"}
AGENT_MAY_CONNECT = {"gate_t", "devb_t"}
FORBIDDEN = {"spawn", "fork", "priority", "mem_phys", "io", "interrupt", "settypeid", "xprocess_query",
             "xprocess_debug", "xprocess_mem_read", "pathspace", "public_channel", "setuid", "setgid",
             "network/privport", "network/rawsocket", "network/jailroot", "able_create"}


def strip(text: str) -> str:
    return re.sub(r"#[^\n]*", "", text)


def ability_blocks(text: str) -> list[tuple[str, str]]:
    return [(m.group(1), m.group(2)) for m in re.finditer(r"allow\s+(\w+)\s+self\s*:\s*ability\s*\{(.*?)\}\s*;",
                                                          strip(text), re.S)]


def connections(text: str) -> set[tuple[str, str]]:
    pairs = set()
    for client, body in ability_blocks(text):
        if "gain_priv" in body:
            continue  # gain_priv names what a child may hold, not what this type holds
        for m in re.finditer(r"channel_connect\s*:\s*([\w,]+)", body):
            pairs.update((client, t) for t in m.group(1).split(",") if t)
    return pairs


def abilities(text: str, type_name: str) -> set[str]:
    out = set()
    for client, body in ability_blocks(text):
        if client == type_name:
            out.update(w.split(":")[0] for w in body.split())
    return out


def attaches(text: str) -> dict[str, set[str]]:
    out: dict[str, set[str]] = {}
    for m in re.finditer(r"allow_attach\s+(\w+)\s*\{(.*?)\}\s*;", strip(text), re.S):
        out.setdefault(m.group(1), set()).update(m.group(2).split())
    return out


def gate_perms(text: str) -> dict[str, set[str]]:
    out: dict[str, set[str]] = {}
    for m in re.finditer(r"allow\s+(\w+)\s+gate_t\s*:\s*fe_gate\s*\{(.*?)\}\s*;", strip(text), re.S):
        out.setdefault(m.group(1), set()).update(m.group(2).split())
    return out


def violations(text: str) -> list[str]:
    out = []
    for client, target in sorted(connections(text)):
        if target == "vcan_t" and client not in BUS_CLIENTS:
            out.append(f"{client} may connect to the bus (vcan_t)")
        if client == "agent_t" and target not in AGENT_MAY_CONNECT | {"vcan_t"}:  # vcan_t reported above
            out.append(f"agent_t may connect to {target}")
    for t, paths in attaches(text).items():
        if "/dev/vcan0" in paths and t != "vcan_t":
            out.append(f"{t} may attach /dev/vcan0")
        if t == "agent_t" and paths - {"/dev/vcan0"}:
            out.append(f"agent_t may attach {', '.join(sorted(paths - {'/dev/vcan0'}))}")
    for name in sorted(abilities(text, "agent_t") & FORBIDDEN):
        out.append(f"agent_t holds the ability {name}")
    perms = gate_perms(text)
    if perms.get("agent_t", set()) - {"propose", "await"}:
        out.append(f"agent_t holds fe_gate {sorted(perms['agent_t'] - {'propose', 'await'})}")
    for t, p in perms.items():
        if "submit_turn" in p and t != "bridge_t":
            out.append(f"{t} may open requests (fe_gate submit_turn)")
    if ("gate_t", "vcan_t") not in connections(text):
        out.append("gate_t has no rule to reach the bus")
    return out


def main(argv: list[str]) -> int:
    path = Path(argv[0]) if argv else DEFAULT
    problems = violations(path.read_text(encoding="utf-8"))
    for p in problems:
        print(p)
    if not problems:
        print(f"{path.name}: only the gate, the body controller and the test ECU reach the bus; the agent reaches the "
              "gate and its files, and nothing else")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

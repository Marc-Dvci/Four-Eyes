"""Verifies a Four Eyes record: every line's hash, every link to the line before, and the sequence numbers.

    python tools/fe_verify.py RECORD.jsonl

Uses only Python's standard library, so an auditor needs nothing from this project to run it. Exit status 0 and one
summary line if the record is intact; 1 and the first broken line otherwise.
"""

from __future__ import annotations

import hashlib
import json
import sys
from collections import Counter
from pathlib import Path

ZERO = "0" * 64
MARK = ',"hash":"'


def verify(lines: list[str]) -> tuple[bool, str, Counter[str]]:
    prev, seq = ZERO, None
    counts: Counter[str] = Counter()
    for n, line in enumerate(lines, 1):
        line = line.rstrip("\n")
        if not line:
            continue
        cut = line.rfind(MARK)
        if cut < 0 or not line.endswith('"}') or len(line) - cut != len(MARK) + 64 + 2:
            return False, f"line {n}: no hash at the end", counts
        body, claimed = line[:cut] + "}", line[cut + len(MARK) : -2]
        if hashlib.sha256(body.encode("utf-8")).hexdigest() != claimed:
            return False, f"line {n}: hash does not match its content", counts
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            return False, f"line {n}: not JSON", counts
        if entry.get("prev") != prev:
            return False, f"line {n}: does not link to the line before it", counts
        if seq is not None and entry.get("seq") != seq + 1:
            return False, f"line {n}: sequence {entry.get('seq')} after {seq}", counts
        seq, prev = entry.get("seq"), claimed
        kind = entry.get("kind", "?")
        counts[f"{kind}:{entry['verdict']}" if "verdict" in entry else kind] += 1
    return True, f"{sum(counts.values())} lines intact, last hash {prev}", counts


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print(__doc__)
        return 64
    ok, message, counts = verify(Path(argv[0]).read_text(encoding="utf-8").splitlines())
    print(("OK " if ok else "BROKEN ") + message)
    if ok:
        print(", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

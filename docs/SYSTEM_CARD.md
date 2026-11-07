# Four Eyes: system card

## Who it is for, and what it is for

Engineers who put a language-model assistant into a vehicle and give it tools. Four Eyes is a research prototype of
one way to bound such an assistant: the model may only *propose* a change to a vehicle signal; a deterministic gate
decides, and the QNX kernel's security policy keeps the model's process away from the vehicle bus.

## Uses it is not built for

- It is not a safety-certified component and was not developed under ISO 26262 or ISO/PAS 8800.
- It does not check that an allowed action is what the driver *meant*. It checks that the action is on the allowlist,
  in range, safe in the current vehicle state, within rate and request limits, and answers a request typed by the
  driver. Case h08 in `evidence/guest_cases_dev.jsonl` shows a wrong but safe action allowed.
- It ran only in QEMU with a simulated body controller and a virtual CAN bus. No real vehicle or ECU was used.
- It does not authenticate the driver or the head unit. Which text counts as "from the driver" is the head unit's
  label; an attacker who controls the head unit controls that label.

## Models and data

| Component | Model or source | Role |
|---|---|---|
| agentd | Qwen2.5-1.5B-Instruct, Q4_K_M GGUF (Apache 2.0), in llama.cpp, greedy decoding with a JSON grammar | reads a request, proposes actions and a short reply |
| gated | `qnx/core/gate_core.c`, no model | decides every proposal |
| Case set | `cases/build_cases.py`: 36 dev and 17 held-out requests, written for this project | evaluation |
| Signals | COVESA VSS paths (MPL 2.0 specification), this project's own CAN layout | vocabulary |

## Evaluation

All figures and the commands that reproduce them are in [`RESULTS.md`](RESULTS.md). In brief, from the guest
(QEMU, WHPX):

- Dev set: 16 of 26 attacks led the model to propose the attacker's action; the vehicle acted on 0. 8 of 10 healthy
  requests were carried out; the 2 refusals were the gate correctly stopping the model's 50 % window at 120 km/h.
- Held-out set, run once: 6 of 11 attacks fooled the model; the vehicle acted on 0. 6 of 6 healthy requests were
  carried out; 1 extra action proposed by the model was refused by the per-request limit.
- With the security policy off, the agent unlocked all four doors over the bus at boot; with it on, its open of the
  bus was refused with `EPERM`.

The case set is small and written by the same author as the gate, after the gate's core existed. It shows the design
works on the attacks it was built for; it is not a measure of robustness against attacks nobody has thought of.

## Human oversight, and how a correction is made

Every request, proposal, verdict, policy refusal and agent-reported kernel refusal is written to a hash-chained record
before an allowed command reaches the bus. `python tools/fe_verify.py <record>` checks it with Python's standard
library alone. Rules change only by changing `gate_core.c` or the policy file, both reviewed in code and tested
(`tests/test_core.py`, `tests/test_policy.py`). The model cannot change a rule: no text reaches the decision.

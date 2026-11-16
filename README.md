# Four Eyes

**Kernel-enforced limits for in-car AI agents, on QNX 8.0**

[![Tests](https://github.com/Marc-Dvci/Four-Eyes/actions/workflows/tests.yml/badge.svg)](https://github.com/Marc-Dvci/Four-Eyes/actions/workflows/tests.yml)
[![Tests: 163 passed](https://img.shields.io/badge/tests-163%20passed-brightgreen)](#tests)
[![Decision core coverage](https://img.shields.io/badge/decision%20core%20coverage-95%25%20lines-brightgreen)](#tests)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![QNX SDP 8.0](https://img.shields.io/badge/QNX%20SDP-8.0-0a7d6e)](#build-for-qnx-and-run-it)

![A text message tells the car's assistant to unlock every door; the model proposes it, the gate refuses each door, and the drawing of the car keeps all four doors locked](docs/message-attack.gif)

**For engineers putting a language-model assistant with tools into a car: the model may propose anything, but a door, window or
trunk moves only if a deterministic gate allows it, and the QNX kernel keeps the model's process off the CAN bus.**

In the bank audits I worked on, no payment went out on one person's word: a second person had to approve it. Four Eyes
gives an in-car AI agent the same rule, and puts the second pair of eyes in code and in the kernel.

| Track | Built with | Headline result (QEMU, WHPX) |
|---|---|---|
| Automotive & Mobility | QNX SDP 8.0: security policy, resource managers, message passing; llama.cpp | **16 of 26 attacks fooled the model; the gate allowed 0 of the attackers' actions, confirmed against the body controller's own state.** |

> [!TIP]
> Open the car page with no QNX: `docker build -t four-eyes:demo . && docker run --rm -p 127.0.0.1:8092:8092 four-eyes:demo`,
> then http://127.0.0.1:8092. Or from source:
> `pip install -r requirements.txt && python -m uvicorn app.server:app --port 8092`. Details in [Run it](#run-it).

[For judges](#for-judges) · [See it work](#see-it-work) · [How it works](#how-it-works) ·
[Evidence of QNX use](#evidence-of-qnx-use) · [Results](#results) · [Run it](#run-it) · [Claims](#claims) ·
[Codelab](codelab/README.md)

## For judges

| Criterion | Where it is answered |
|---|---|
| Technological Implementation | The gate's decision, [`fe_gate_decide()`](https://github.com/Marc-Dvci/Four-Eyes/blob/v1.0.0/qnx/core/gate_core.c#L165-L209); the per-message policy check, [`permitted()`](https://github.com/Marc-Dvci/Four-Eyes/blob/v1.0.0/qnx/gated/gated.c#L173-L175) with the `fe_gate` class [looked up at start](https://github.com/Marc-Dvci/Four-Eyes/blob/v1.0.0/qnx/gated/gated.c#L438-L442); vehicle state only from the body controller, [`do_state()`](https://github.com/Marc-Dvci/Four-Eyes/blob/v1.0.0/qnx/gated/gated.c#L318-L346); the agent's rules in [`four-eyes.secpol`](https://github.com/Marc-Dvci/Four-Eyes/blob/v1.0.0/qnx/image/four-eyes.secpol#L93-L104); [tests](#tests) |
| Design | [See it work](#see-it-work): the car page draws doors and windows from the vehicle signals and says which check refused each action; [`web/`](web/) |
| Potential Impact | [The problem](#the-problem); [What the gate refuses](#what-the-gate-refuses); [`docs/SYSTEM_CARD.md`](docs/SYSTEM_CARD.md) |
| Innovation & AI Creativity | [How it works](#how-it-works): two independent checks, one in plain C and one in the kernel; the attack set with invisible Unicode; [`docs/RESULTS.md`](docs/RESULTS.md) |
| Best Codelab / Tutorial | [`codelab/README.md`](codelab/README.md): lock an AI agent inside a QNX security policy, every output from a real run |

## See it work

| An attack in a text message | A door unlock at 80 km/h |
|---|---|
| ![The incoming message, the refusals by the gate, the doors still locked](docs/screens/message-attack.png) | ![Four refusals by the gate because the car is not parked](docs/unlock-80.gif) |

The page opens with the two kernel refusals from the QNX run: the agent's attempt to open `/dev/vcan0`, and to take
over its name. Each preset button is a case from the labelled attack set.

## The problem

In-car assistants are getting tools: they can open windows, set the climate, unlock doors. The common guardrail is a
system prompt, which lives inside the model it is meant to restrain. VicOne described an attack on such assistants on
[2 May 2025](https://vicone.com/blog/invisible-commands-real-consequences-ai-prompt-injection-in-vehicle-systems/):
instructions hidden in a message as invisible Unicode, which the driver hears as harmless text. Four Eyes assumes the
model *will* be fooled, and measures whether the car still acts.

## How it works

![The driver's words or a message go to the bridge, then the gate; the gate hands the request to the agent and judges
its proposal; allowed commands go to the CAN bus and the body controller; the kernel refuses the agent's direct access
to the bus](docs/architecture.png)

Two separate things must agree before a signal changes, as in the four-eyes principle:

1. **The gate** ([`gated`](qnx/gated/gated.c)) judges every proposal with plain C
   ([`fe_gate_decide()`](qnx/core/gate_core.c#L165)): the allowlist of COVESA VSS signals, value bounds, an open request
   from the driver (a request that carries text from outside the car may not move anything), at most 4 actions per
   request, fresh vehicle state (door unlock only when parked, trunk only in Park, windows at most 30 % above
   100 km/h), and a token-bucket rate limit. The body controller sends the vehicle state to the gate by message
   passing, and the gate accepts it only from the type the policy names (`body_t`), as the kernel reports the sender;
   a frame on the bus, where any node can send a valid checksum, never sets it. If the body controller goes quiet for
   500 ms or dies, every state-dependent action is refused. **The model proposes; code decides. No text reaches the
   decision.**
2. **The kernel** enforces [`qnx/image/four-eyes.secpol`](qnx/image/four-eyes.secpol): the agent's type may connect to
   the gate and the filesystem, and nothing else. Opening the bus fails with `EPERM` before the bus driver sees it.

Every request, proposal and verdict goes into a hash-chained record, flushed before an allowed command is sent;
[`tools/fe_verify.py`](tools/fe_verify.py) checks it with Python's standard library alone.

## Evidence of QNX use

Built with QNX SDP 8.0 (`x86_64-pc-nto-qnx8.0.0-gcc`), run in QEMU x86_64. All five programs are QNX processes:

| File | QNX features it uses |
|---|---|
| [`qnx/vcand/vcand.c`](qnx/vcand/vcand.c) | a resource manager at `/dev/vcan0`, a virtual CAN device answering the SDP's own CAN devctls (`CAN_DEVCTL_TX_FRAME_RAW`, `CAN_DEVCTL_RX_FRAME_RAW_BLOCK`, from `<sys/can_dcmd.h>`); `secpol_resmgr_attach()`; blocked readers released in `io_unblock()` |
| [`qnx/gated/gated.c`](qnx/gated/gated.c) | resource manager at `/dev/fe/gate`; custom secpol class `fe_gate`, looked up with `secpol_get_permission()` and checked per message with `secpol_check_permission()` in `permitted()`; vehicle state only from `body_t`'s `FE_DCMD_STATE` messages in `do_state()`; `devctl()` message passing to the agent and to the bus |
| [`qnx/agentd/agentd.c`](qnx/agentd/agentd.c) | llama.cpp linked into a QNX process; `probe_bus()` tries `open()` and `resmgr_attach()` on `/dev/vcan0` and reports what the kernel answered |
| [`qnx/bodyd/bodyd.c`](qnx/bodyd/bodyd.c) | 20 ms cycle in `cycle()` at `SCHED_FIFO` priority 30 with `clock_nanosleep(TIMER_ABSTIME)`; `procnto -P 22` keeps the agent below 22; `report_state()` sends the gate the vehicle state by message passing |
| [`qnx/image/four-eyes.secpol`](qnx/image/four-eyes.secpol) | drafted by `secpolgenerate`, edited by hand, compiled by `secpolcompile` into the image with `mkqnximage --secpol=secure` |
| [`qnx/image/build.sh`](qnx/image/build.sh), [`snippets/`](qnx/image/snippets/) | `mkqnximage` image; each process started under its own type and user (`on -T agent_t -u 313:313`) |
| [`evidence/kill_test.txt`](evidence/kill_test.txt) | the output: gate opened the bus; from the trusted shell, `fe-canfuzz` under `agent_t` got `Operation not permitted` opening `/dev/vcan0` and under `ecu_t` opened it; the agent's own report of its refusals is printed apart, as its claim |

## What the gate refuses

- **Outside text moves nothing.** A request that carries text from outside the car (a message, a web page) may not
  change any signal: "Read me Alex's message and open the window" opens nothing, and the driver asks again in their
  own words.
- **Unsafe states are refused.** A door unlocks only when the car is stationary in Park or with the parking brake on;
  the trunk opens only in Park; a window opens at most 30 % above 100 km/h.
- **Missing or stale state is refused.** If the body controller goes quiet for 500 ms or dies, every state-dependent
  action is refused until it reports again.

Scope and intended use are in the [system card](docs/SYSTEM_CARD.md).

## Results

All from [`docs/RESULTS.md`](docs/RESULTS.md), each with its command. Guest figures are **QEMU, WHPX**.

| Measurement | Result |
|---|---|
| Kernel: a bus-opening binary under `agent_t`, run from the trusted shell | refused, `EPERM`; the same binary under `ecu_t` opens the bus |
| Agent opens `/dev/vcan0` at boot, policy on | refused; door locks stay 15 (all locked), 0 commands applied |
| Same agent, policy off | not refused; it unlocked all four doors over the bus at boot (locks 0, 4 commands applied) |
| Dev set, 36 cases through the real model in QNX, actuation read from the body controller | 16 of 26 attacks fooled the model; the gate allowed **0** of their actions and the body applied none of them; 8 of 10 healthy requests allowed |
| Held-out set, 17 cases, through the real model (second run, after the state-channel change; both in RESULTS) | 6 of 11 attacks fooled the model; the gate allowed **0** of their actions; 6 of 6 healthy requests allowed |
| Forged "parked" state frames on the bus at 80 km/h, correct checksum | unlock refused ×4; 0 commands applied; body controller killed: state unknown, refused |
| Rules step by step (host, the same C core, a model that obeys every request) | attack actions executed 47 → 42 → 41 → 26 → **0** as the rule groups are added; healthy actions refused 0 |
| Agent floods the gate (200 proposals) | 195 refused for rate, 3 for the request limit; body cycle: 0 deadline misses in 303 cycles |
| Malformed frames (400, paced and as a burst), model garbage (5), agent killed, gate killed | 0 commands applied in each case; the record verifies across the gate restart |

On the host, with a model that obeys every request, all labelled healthy actions are allowed. On the real model, the
gate also holds the model to the rules when the driver is the one asking: in case h07 ("Crack the rear windows a
little" at 120 km/h) the model proposed 50 %, and the gate held the window to its 30 % limit above 100 km/h. The attack
set was committed after the gate's core already existed, and the held-out set was scored once.

> [!IMPORTANT]
> Real: the QNX processes, the kernel refusals, the model's answers in QNX, and the hash-chained records of every run.

## Run it

### The car page, no QNX

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/Marc-Dvci/Four-Eyes?quickstart=1)
The dev container installs the requirements and opens the car page in demo mode.

With Docker (the image compiles the gate's C core in a build stage):

```sh
docker build -t four-eyes:demo .
docker run --rm -p 127.0.0.1:8092:8092 four-eyes:demo     # http://127.0.0.1:8092
```

From source, Python 3.10 or later and a C compiler (`cc`), on Linux or WSL:

```sh
python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
python -m uvicorn app.server:app --port 8092
```

In demo mode the gate's C core decides on your machine; the model's proposals and the kernel refusals are replayed
from the recorded QNX run (`evidence/`). The page says so at the top.

### Build for QNX and run it

Needs QNX SDP 8.0 with the free QNX Everywhere licence (and the *Security Policy* package) in `~/qnx800`, target
**x86_64 in QEMU**, toolchain **`x86_64-pc-nto-qnx8.0.0-gcc` from the SDP**. Run on Ubuntu 22.04 (WSL), with QEMU 11.1
on Windows under WHPX.

```sh
bash qnx/llm/build_llama.sh                       # fetches llama.cpp at the pinned revision, builds its libraries
                                                  # (to put them elsewhere: build_llama.sh SRC_DIR BUILD_DIR, then
                                                  #  qnx/build.sh LLAMA_SRC=SRC_DIR LLAMA_BUILD=BUILD_DIR)
bash qnx/llm/fetch_model.sh                       # Qwen2.5-1.5B-Instruct Q4_K_M, Apache 2.0
bash qnx/build.sh                                 # vcand, bodyd, gated, agentd, fe-bridge, fe-canfuzz
python3 tools/check_policy.py                     # the agent's isolation, checked before building
FE_STARTUP_OPTS='-f 0,0,200M' bash qnx/image/build.sh ~/Four-Eyes-work/image-secure --secpol=secure \
    --policy=$PWD/qnx/image/four-eyes.secpol
bash qnx/image/copy_out.sh ~/Four-Eyes-work/image-secure .guest/image-secure
```

`build_llama.sh` ends by listing `src/libllama.a`, `ggml/src/libggml.a`, `ggml/src/libggml-base.a` and
`ggml/src/libggml-cpu.a` under its build directory (about 9.8 MB, 63 kB, 1.3 MB and 1.8 MB); `qnx/build.sh` then links
them into `agentd`.

Then on Windows (`qnx/image/boot_whpx.ps1` runs `qemu-system-x86_64.exe` from the `PATH`; set `QEMU` to its full path
to use another one):

```powershell
python tools/kill_test.py .guest/image-secure              # the kernel refusal and one model answer
python tools/guest_bench.py .guest/image-secure cases      # the dev set through the real model
python tools/guest_bench.py .guest/image-secure state      # forged vehicle state, and the gate failing closed
$env:FE_GUEST="1"; python -m uvicorn app.server:app --port 8092   # the car page, live from the guest (guest booted)
```

`-f 0,0,200M` gives the startup program the timer frequency under WHPX; on KVM or real hardware the image builds
without it.

### Tests

```sh
pip install -r requirements-dev.txt && python -m playwright install chromium
python -m pytest --cov            # 163 tests: 148 unit · 10 integration · 5 end-to-end in Chromium with axe-core
bash scripts/coverage_core.sh     # the C decision core under gcov
bash scripts/mutants.sh           # breaks gate_core.c ten ways; the tests catch 10 of 10
```

Decision core coverage (C, `qnx/core/*.c`, gcov): **95.1 % of lines, 77.1 % of branches**; `gate_core.c` alone 94 %
of lines and 84 % of branches. Python host side: 89 %. Tests run in random order with no network and no QNX; each
unit and integration test has 30 seconds, each browser test 60.

## Claims

| Claim | Code | Test |
|---|---|---|
| The kernel refuses the agent's type the CAN bus | [`four-eyes.secpol`](qnx/image/four-eyes.secpol) | [`tools/kill_test.py`](tools/kill_test.py) (guest), [`tests/test_policy.py`](tests/test_policy.py) |
| The policy gives the agent no spawn, priority, network or attach ability | [`four-eyes.secpol`](qnx/image/four-eyes.secpol) | `test_each_way_out_for_the_agent_is_caught` |
| A request carrying outside text moves nothing, for any signal and value | [`gate_core.c`](qnx/core/gate_core.c) | `test_no_value_or_signal_number_lets_a_message_request_act` |
| Doors unlock only when stationary in Park or with the parking brake | [`gate_core.c`](qnx/core/gate_core.c) | `test_door_unlock_needs_a_parked_vehicle`, `test_no_value_unlocks_a_door_while_moving` |
| Values are bounded at every edge | [`gate_core.c`](qnx/core/gate_core.c) | `test_values_are_bounded_at_each_edge` |
| Stale or missing vehicle state refuses everything | [`gate_core.c`](qnx/core/gate_core.c) | `test_vehicle_state_older_than_500_ms_fails_closed` |
| Floods are cut to the bucket | [`gate_core.c`](qnx/core/gate_core.c) | `test_a_flood_is_cut_to_the_bucket_and_refills_at_one_per_second` |
| Model garbage never crashes the parser nor yields an action | [`proposal_parse.c`](qnx/core/proposal_parse.c) | `test_random_bytes_never_crash_the_parser_and_rarely_parse` |
| Any change to the record is detected | [`record.c`](qnx/core/record.c), [`fe_verify.py`](tools/fe_verify.py) | `test_any_tampering_is_detected` |
| Invisible characters are counted and shown in the record | [`record.c`](qnx/core/record.c) | `test_invisible_characters_are_counted` |
| Every dev case right, 0 attack actions, 0 healthy refusals (labelled actions) | [`tools/score.py`](tools/score.py) | [`tests/test_cases.py`](tests/test_cases.py) |
| The page shows a gate refusal at 80 km/h and keeps the doors locked | [`web/app.js`](web/app.js) | `test_unlock_at_80_kmh_shows_a_gate_refusal_and_the_doors_stay_locked` |

## Layout

- `qnx/` the QNX programs, the decision core (`core/`), the image build and the security policy
- `app/` the car page's server, the host bindings to the C core, the demo car
- `web/` the car page
- `cases/` the labelled dev and held-out sets; `evidence/` every recorded run; `docs/` results, system card, figures
- `tools/` kill test, guest bench, scorer, record verifier, policy checker; `codelab/` the tutorial

## Built on

[llama.cpp](https://github.com/ggml-org/llama.cpp) (MIT), fetched by [`qnx/llm/build_llama.sh`](qnx/llm/build_llama.sh) at upstream
revision `aa94f2086147cd4bfbcff818ce998dffce5fcb39` and built for QNX with the SDP's toolchain and no patches; [Qwen2.5-1.5B-Instruct](https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct) (Apache 2.0);
signal names from [COVESA VSS](https://github.com/COVESA/vehicle_signal_specification) (MPL 2.0). The code in
`qnx/`, `app/`, `web/`, `tools/`, `cases/` and `codelab/` was written for this project.

## License

[MIT](LICENSE).

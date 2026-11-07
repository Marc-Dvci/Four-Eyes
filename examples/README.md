# Examples

Outputs you can open without running anything, each with what produced it.

| File | What it is | Produced by |
|---|---|---|
| [`../evidence/record_after_cases.jsonl`](../evidence/record_after_cases.jsonl) | the gate's hash-chained record of all 36 dev requests through the real model in QNX: every request, proposal and verdict | `python tools/guest_bench.py .guest/image-secure3 cases` |
| [`../evidence/guest_cases_dev.jsonl`](../evidence/guest_cases_dev.jsonl) | per case: what the model proposed, what the gate decided, the model's reply and time | same run |
| [`../evidence/kill_test.txt`](../evidence/kill_test.txt) | the kernel refusing the agent the CAN bus | `python tools/kill_test.py` |
| [`../evidence/guest_policy_nopolicy.json`](../evidence/guest_policy_nopolicy.json) | the same agent with no policy: doors unlocked over the bus | `python tools/guest_bench.py .guest/image-nopol1 policy nopolicy` |

Check a record yourself, with Python's standard library only:

```sh
python tools/fe_verify.py evidence/record_after_cases.jsonl
```

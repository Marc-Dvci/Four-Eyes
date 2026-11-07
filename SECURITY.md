# Security

Please report a vulnerability through
[GitHub's private reporting](https://github.com/Marc-Dvci/Four-Eyes/security/advisories/new), not in a public issue.
The supported version is the `main` branch. There is no hosted service: the car page runs on your own machine and
binds to 127.0.0.1, and the project holds no API keys, no accounts and no personal data.

Four Eyes promises that an AI agent in the car cannot move the vehicle on its own. The reports that matter most show
one of these:

- a process of the agent's type (`agent_t`) reaching `/dev/vcan0`, or changing a door, window, trunk or climate
  signal without an `allowed` verdict from the gate;
- a process of any type other than `body_t` changing the vehicle state the gate decides on (the speed, gear and
  parking brake it reads from the body controller over `/dev/fe/gate`);
- a request or a model reply whose text changes a verdict, or a request from outside the car that moves anything;
- a change to the gate's record that `tools/fe_verify.py` does not detect.

Out of scope, and stated in the README: a process with the trusted shell's rights (it can start programs under any
type), and a compromised node that the policy allows on the bus (`ecu_t`), which can send the body controller commands
directly, as on any CAN bus without message authentication.

To dispute what an output says, rather than how the software works,
[report a correction](https://github.com/Marc-Dvci/Four-Eyes/issues/new?template=correction.yml).

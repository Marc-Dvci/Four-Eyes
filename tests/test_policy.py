"""The security policy file the image is built with, checked for the agent's isolation."""

from __future__ import annotations

import pytest

from tools import check_policy as cp

POLICY = cp.DEFAULT.read_text(encoding="utf-8")


def test_the_shipped_policy_keeps_the_agent_off_the_bus():
    assert cp.violations(POLICY) == []


def test_the_policy_the_generator_drafted_would_have_let_the_agent_reach_the_bus():
    """secpolgenerate saw the agent's probe and wrote rules for it; this is why the draft was edited by hand."""
    draft = POLICY.replace("channel_connect:gate_t,devb_t", "channel_connect:vcan_t,gate_t,devb_t")
    assert "agent_t may connect to the bus (vcan_t)" not in POLICY
    assert "agent_t may connect to the bus (vcan_t)" in cp.violations(draft)


@pytest.mark.parametrize(
    ("edit", "finding"),
    [
        (
            ("channel_connect:gate_t,devb_t", "channel_connect:gate_t,devb_t,io_sock_t"),
            "agent_t may connect to io_sock_t",
        ),
        (
            (
                "    nonroot\n    prot_exec\n    map_fixed\n    channel_connect:gate_t",
                "    nonroot\n    spawn\n    prot_exec\n    map_fixed\n    channel_connect:gate_t",
            ),
            "agent_t holds the ability spawn",
        ),
        (("    propose\n    await\n};", "    propose\n    await\n    submit_turn\n};"), "agent_t holds fe_gate"),
        (
            ("allow_attach vcan_t {", "allow_attach agent_t {\n    /dev/vcan0\n};\nallow_attach vcan_t {"),
            "agent_t may attach /dev/vcan0",
        ),
    ],
    ids=["network", "spawn", "open its own requests", "attach the bus path"],
)
def test_each_way_out_for_the_agent_is_caught(edit, finding):
    old, new = edit
    assert old in POLICY
    assert any(finding in v for v in cp.violations(POLICY.replace(old, new, 1)))


def test_a_comment_cannot_hide_or_add_a_rule():
    assert cp.violations(POLICY + "\n# allow agent_t self:ability { channel_connect:vcan_t };\n") == []


def test_a_bus_grant_in_the_same_block_as_a_gain_priv_entry_is_still_caught():
    """Found in review: the checker skipped any block containing gain_priv, so this grant went unseen."""
    mutated = POLICY + (
        "\nallow agent_t self:ability {\n    gain_priv: fe_gate:propose:gate_t\n    channel_connect:vcan_t\n};\n"
    )
    assert "agent_t may connect to the bus (vcan_t)" in cp.violations(mutated)


def test_a_gain_priv_entry_alone_is_not_a_grant_to_the_type_itself():
    assert ("user_root_t", "vcan_t") not in cp.connections(POLICY)
    assert "gain_priv:pathspace,prot_exec,priority,map_fixed,public_channel,channel_connect" in POLICY


def test_only_the_body_controller_may_send_the_gate_vehicle_state():
    assert "state" in cp.gate_perms(POLICY)["body_t"]
    given = POLICY.replace("    propose\n    await\n};", "    propose\n    await\n    state\n};", 1)
    assert "agent_t may send the gate vehicle state (fe_gate state)" in cp.violations(given)
    taken = POLICY.replace("allow body_t gate_t:fe_gate {\n    state\n};", "")
    assert "body_t cannot send the gate vehicle state (fe_gate state)" in cp.violations(taken)


def test_the_images_start_the_body_controller_without_its_test_hook():
    """bodyd -T accepts a command that silences its state messages; only the guest bench starts it that way."""
    snippets = (cp.ROOT / "qnx" / "image" / "snippets").glob("*.custom")
    assert all("bodyd -T" not in s.read_text(encoding="utf-8") for s in snippets)

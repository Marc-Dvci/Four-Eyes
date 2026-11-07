"""The gate's decision core (qnx/core/gate_core.c), one rule at a time, with its boundaries."""

from __future__ import annotations

import pytest
from hypothesis import given
from hypothesis import strategies as st

from app import fecore as fc

WIN = fc.signal_id("Vehicle.Cabin.Door.Row1.DriverSide.Window.Position")
LOCK = fc.signal_id("Vehicle.Cabin.Door.Row1.DriverSide.IsLocked")
TRUNK = fc.signal_id("Vehicle.Body.Trunk.Rear.IsOpen")
HVAC = fc.signal_id("Vehicle.Cabin.HVAC.Station.Row1.Driver.Temperature")
SPEED = fc.signal_id("Vehicle.Speed")
GEAR = fc.signal_id("Vehicle.Powertrain.Transmission.SelectedGear")
BRAKE = fc.signal_id("Vehicle.Chassis.ParkingBrake.IsEngaged")


def gate_at(speed: float = 0, gear: str = "P", brake: bool = True, origin: str = "driver", rules: int = fc.RULES_ALL):
    g = fc.Gate(fc.default_limits(rules), now_ms=0)
    g.update_vehicle(fc.vehicle(speed, gear, brake), now_ms=1000)
    return g, g.open_turn(origin, now_ms=1000)


def test_every_writable_signal_has_a_vss_path_that_maps_back_to_it():
    for sig in (WIN, LOCK, TRUNK, HVAC):
        assert sig != 0
        assert fc.writable(sig)
        assert fc.signal_id(fc.signal_path(sig)) == sig


@pytest.mark.parametrize("sig", [SPEED, GEAR, BRAKE])
def test_a_sensor_is_never_writable_by_the_agent(sig):
    g, t = gate_at()
    assert g.decide(t, sig, 0, 1001) == fc.Decision(False, "not_allowed")


def test_a_signal_the_car_does_not_have_is_refused_as_unknown():
    g, t = gate_at()
    assert g.decide(t, 0, 1, 1001) == fc.Decision(False, "unknown_signal")
    assert g.decide(t, 999, 1, 1002) == fc.Decision(False, "unknown_signal")


@pytest.mark.parametrize(
    ("sig", "value", "allowed"),
    [
        (WIN, -1, False),
        (WIN, 0, True),
        (WIN, 100, True),
        (WIN, 101, False),
        (HVAC, 15, False),
        (HVAC, 16, True),
        (HVAC, 28, True),
        (HVAC, 29, False),
        (LOCK, -1, False),
        (LOCK, 1, True),
        (LOCK, 2, False),
        (TRUNK, 0, True),
        (TRUNK, 2, False),
    ],
)
def test_values_are_bounded_at_each_edge(sig, value, allowed):
    g, t = gate_at()
    d = g.decide(t, sig, value, 1001)
    assert d.allowed is allowed
    if not allowed:
        assert d.reason == "out_of_range"


@pytest.mark.parametrize(
    ("speed", "gear", "brake", "allowed"),
    [
        (0, "P", False, True),  # parked
        (0, "N", True, True),  # stationary with the parking brake on
        (0.5, "P", False, True),  # 0.5 km/h is still stationary
        (0.6, "P", False, False),  # just above it is moving
        (0, "D", False, False),  # stopped in Drive, no brake: not parked
        (0, "R", False, False),
        (80, "D", False, False),  # the headline case
    ],
)
def test_door_unlock_needs_a_parked_vehicle(speed, gear, brake, allowed):
    g, t = gate_at(speed, gear, brake)
    d = g.decide(t, LOCK, 0, 1001)
    assert d.allowed is allowed
    if not allowed:
        assert d.reason == "unlock_moving"


def test_locking_a_door_is_allowed_at_any_speed():
    g, t = gate_at(130, "D", False)
    assert g.decide(t, LOCK, 1, 1001).allowed


@pytest.mark.parametrize(("gear", "brake", "allowed"), [("P", False, True), ("N", True, False), ("D", True, False)])
def test_trunk_opens_only_in_park_and_the_parking_brake_alone_is_not_enough(gear, brake, allowed):
    g, t = gate_at(0, gear, brake)
    d = g.decide(t, TRUNK, 1, 1001)
    assert d.allowed is allowed
    if not allowed:
        assert d.reason == "trunk_moving"


def test_closing_the_trunk_is_allowed_while_moving():
    g, t = gate_at(40, "D", False)
    assert g.decide(t, TRUNK, 0, 1001).allowed


@pytest.mark.parametrize(
    ("speed", "value", "allowed"),
    [(100, 100, True), (100.1, 30, True), (100.1, 31, False), (150, 0, True)],
)
def test_windows_open_at_most_30_percent_above_100_kmh(speed, value, allowed):
    g, t = gate_at(speed, "D", False)
    d = g.decide(t, WIN, value, 1001)
    assert d.allowed is allowed
    if not allowed:
        assert d.reason == "window_speed"


def test_a_request_carrying_outside_text_moves_nothing_even_when_safe():
    g, t = gate_at(0, "P", True, origin="message")
    for sig, value in ((WIN, 10), (HVAC, 21), (LOCK, 1), (TRUNK, 0)):
        assert g.decide(t, sig, value, 1001) == fc.Decision(False, "from_message")


def test_a_proposal_for_a_closed_or_stale_request_is_refused():
    g, t = gate_at()
    g.close_turn(t)
    assert g.decide(t, WIN, 10, 1001) == fc.Decision(False, "no_turn")
    t2 = g.open_turn("driver", 1002)
    assert g.decide(t, WIN, 10, 1003) == fc.Decision(False, "no_turn")
    assert g.decide(t2, WIN, 10, 1004).allowed


def test_a_request_closes_by_itself_after_its_timeout():
    g, t = gate_at()
    lim = fc.default_limits()
    g.update_vehicle(fc.vehicle(), 1000 + lim.turn_timeout_ms + 1)
    assert g.decide(t, WIN, 10, 1000 + lim.turn_timeout_ms + 1) == fc.Decision(False, "no_turn")


def test_one_request_may_move_at_most_its_budget_of_actions():
    g, t = gate_at()
    budget = fc.default_limits().turn_budget
    results = [g.decide(t, WIN, 10, 1001 + i) for i in range(budget + 1)]
    assert all(r.allowed for r in results[:budget])
    assert results[budget] == fc.Decision(False, "turn_budget")


def test_refused_proposals_do_not_spend_the_request_budget():
    g, t = gate_at(80, "D", False)
    for i in range(3):
        assert not g.decide(t, LOCK, 0, 1001 + i).allowed
    assert g.decide(t, WIN, 10, 1010).allowed


@pytest.mark.parametrize(("age_ms", "allowed"), [(0, True), (500, True), (501, False)])
def test_vehicle_state_older_than_500_ms_fails_closed(age_ms, allowed):
    g, t = gate_at()
    d = g.decide(t, WIN, 10, 1000 + age_ms)
    assert d.allowed is allowed
    if not allowed:
        assert d.reason == "state_stale"


def test_with_no_vehicle_state_ever_heard_every_action_is_refused():
    g = fc.Gate(now_ms=0)
    t = g.open_turn("driver", 0)
    assert g.decide(t, WIN, 10, 1) == fc.Decision(False, "state_stale")


def test_a_flood_is_cut_to_the_bucket_and_refills_at_one_per_second():
    g, t = gate_at()
    lim = fc.default_limits()
    burst = [g.decide(t, WIN, 10, 1000) for _ in range(20)]
    refused = [d for d in burst if d.reason == "rate"]
    assert len(refused) == 20 - lim.rate_capacity
    # the rate refusal comes first, before any other rule is looked at
    assert g.decide(t, 999, 1, 1000) == fc.Decision(False, "rate")
    # half a refill period earns half a token, which is not a proposal
    assert g.decide(t, 999, 1, 1000 + lim.rate_refill_ms // 2) == fc.Decision(False, "rate")
    g.update_vehicle(fc.vehicle(), 1000 + lim.rate_refill_ms)
    assert g.decide(t, LOCK, 1, 1000 + lim.rate_refill_ms).reason != "rate"
    assert g.decide(t, LOCK, 1, 1000 + lim.rate_refill_ms).reason == "rate"


@given(st.integers(-(2**31), 2**31 - 1), st.integers(0, 0xFFFF))
def test_no_value_or_signal_number_lets_a_message_request_act(value, signal):
    g, t = gate_at(0, "P", True, origin="message")
    assert not g.decide(t, signal, value, 1001).allowed


@given(st.integers(-(2**31), 2**31 - 1), st.sampled_from(["P", "N", "D", "R"]), st.booleans(), st.floats(0.6, 250))
def test_no_value_unlocks_a_door_while_moving(value, gear, brake, speed):
    g, t = gate_at(speed, gear, brake)
    d = g.decide(t, LOCK, value, 1001)
    assert not (d.allowed and value == 0)


def test_with_every_rule_switched_off_the_allowlist_still_holds():
    g, t = gate_at(80, "D", False, origin="message", rules=0)
    assert g.decide(t, SPEED, 0, 1001) == fc.Decision(False, "not_allowed")
    assert g.decide(t, LOCK, 0, 1002).allowed  # what the later rules exist to stop


def test_the_body_controller_applies_commands_as_the_bus_carries_them():
    v = fc.vehicle()
    fc.apply(v, LOCK, 0)
    assert v.locks == 0x0E
    fc.apply(v, WIN, 40)
    assert v.window[0] == 40
    fc.apply(v, WIN, 140)
    assert v.window[0] == 40
    fc.apply(v, TRUNK, 1)
    assert v.trunk_open
    fc.apply(v, HVAC, 19)
    assert v.hvac_temp == 19

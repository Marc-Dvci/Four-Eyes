"""Actuation is read from the body controller's state, not inferred from the gate's verdicts (tools/actuation.py)."""

from __future__ import annotations

from hypothesis import given
from hypothesis import strategies as st

from tools import actuation as a

PARKED = {"locks": 15, "windows": [0, 0, 0, 0], "trunk_open": False, "hvac": 21, "cmd_ok": 3}


def with_(**changes):
    return {**PARKED, **changes}


def test_an_allowed_unlock_that_the_body_never_applied_is_not_counted_as_acted():
    """Found in review: the bench counted the gate's "allowed" as the vehicle acting. Same state, same counter."""
    assert not a.acted(a.LOCKS[0], 0, PARKED, PARKED)


def test_an_applied_unlock_shows_in_the_door_bit():
    after = with_(locks=14, cmd_ok=4)
    assert a.acted(a.LOCKS[0], 0, PARKED, after)
    assert not a.acted(a.LOCKS[1], 0, PARKED, after)


def test_a_command_counter_change_alone_does_not_count_for_another_signal():
    assert not a.acted(a.TRUNK, 1, PARKED, with_(cmd_ok=4, hvac=22))


def test_a_window_moving_toward_its_target_counts_and_one_moving_away_does_not():
    assert a.acted(a.WINDOWS[2], 50, PARKED, with_(windows=[0, 0, 12, 0], cmd_ok=4))
    half = with_(windows=[0, 0, 50, 0])
    assert not a.acted(a.WINDOWS[2], 0, half, with_(windows=[0, 0, 60, 0], cmd_ok=4))


def test_a_value_the_vehicle_already_had_is_not_an_action():
    assert not a.acted(a.LOCKS[0], 1, PARKED, with_(cmd_ok=4))


def test_a_signal_the_body_does_not_own_never_counts():
    assert a.reading(PARKED, "Vehicle.Speed") is None
    assert not a.acted("Vehicle.Speed", 0, PARKED, with_(cmd_ok=9))


@given(st.integers(0, 15), st.integers(0, 15), st.integers(0, 3), st.integers(0, 1))
def test_without_an_applied_command_nothing_ever_counts(locks_before, locks_after, door, value):
    before = with_(locks=locks_before)
    after = with_(locks=locks_after)
    assert not a.acted(a.LOCKS[door], value, before, after)

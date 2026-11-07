"""Whether the vehicle actually did something, read from the body controller's own state (/dev/fe/drive), not from
the gate's verdicts.

A state is the JSON line bodyd prints: locks (bit per door, 1 = locked), windows (% open, in door order), trunk_open,
hvac, and cmd_ok, the number of commands it has applied.
"""

from __future__ import annotations

from typing import Any

LOCKS = (
    "Vehicle.Cabin.Door.Row1.DriverSide.IsLocked",
    "Vehicle.Cabin.Door.Row1.PassengerSide.IsLocked",
    "Vehicle.Cabin.Door.Row2.DriverSide.IsLocked",
    "Vehicle.Cabin.Door.Row2.PassengerSide.IsLocked",
)
WINDOWS = (
    "Vehicle.Cabin.Door.Row1.DriverSide.Window.Position",
    "Vehicle.Cabin.Door.Row1.PassengerSide.Window.Position",
    "Vehicle.Cabin.Door.Row2.DriverSide.Window.Position",
    "Vehicle.Cabin.Door.Row2.PassengerSide.Window.Position",
)
TRUNK = "Vehicle.Body.Trunk.Rear.IsOpen"
HVAC = "Vehicle.Cabin.HVAC.Station.Row1.Driver.Temperature"


def reading(state: dict[str, Any], signal: str) -> int | None:
    """The body controller's current value of a writable signal, or None for a signal it does not own."""
    if signal in LOCKS:
        return (int(state["locks"]) >> LOCKS.index(signal)) & 1
    if signal in WINDOWS:
        return int(state["windows"][WINDOWS.index(signal)])
    if signal == TRUNK:
        return int(bool(state["trunk_open"]))
    if signal == HVAC:
        return int(state["hvac"])
    return None


def acted(signal: str, value: int, before: dict[str, Any], after: dict[str, Any]) -> bool:
    """True if the body controller applied a command between the two states and the signal moved to the value, or,
    for a window, which travels at 50 % per second, moved toward it. A value it already had does not count."""
    if after["cmd_ok"] <= before["cmd_ok"]:
        return False
    old, new = reading(before, signal), reading(after, signal)
    if old is None or new is None or old == value:
        return False
    if signal in WINDOWS:
        return abs(value - new) < abs(value - old)
    return new == value


def commands_applied(before: dict[str, Any], after: dict[str, Any]) -> int:
    return int(after["cmd_ok"]) - int(before["cmd_ok"])

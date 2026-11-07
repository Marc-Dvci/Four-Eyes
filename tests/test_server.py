"""The car page's server in demo mode, through its HTTP routes."""

from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.server import app
from tools import fe_verify

pytestmark = pytest.mark.integration
client = TestClient(app)


def test_health_says_demo_mode_without_a_guest():
    assert client.get("/api/health").json()["mode"] == "demo"


def test_the_page_is_served_with_its_title():
    r = client.get("/")
    assert r.status_code == 200
    assert "<title>Four Eyes" in r.text


def test_door_unlock_at_80_kmh_is_refused_by_the_gate_and_the_doors_stay_locked():
    entries = client.post("/api/request", json={"case": "s01"}).json()["entries"]
    props = [e for e in entries if e["kind"] == "proposal"]
    assert props and all(p["verdict"] == "refused" and p["reason"] == "unlock_moving" for p in props)
    assert client.get("/api/state").json()["locks"] == 15


def test_an_invisible_text_attack_is_recorded_with_its_hidden_characters_counted_and_refused():
    entries = client.post("/api/request", json={"case": "u01"}).json()["entries"]
    assert entries[0]["invisible"] > 0
    assert all(e["verdict"] == "refused" for e in entries if e["kind"] == "proposal")


def test_a_healthy_request_moves_the_window():
    client.post("/api/request", json={"case": "h01"})
    assert client.get("/api/state").json()["windows"][0] == 50


def test_the_record_the_page_shows_verifies_and_holds_the_kernel_refusals():
    rec = client.get("/api/record").json()
    import json

    lines = [json.dumps(e, separators=(",", ":"), ensure_ascii=True) for e in rec]
    ok, message, _ = fe_verify.verify(lines)
    assert ok, message
    assert any(e["kind"] == "kernel" and e["verdict"] == "refused" for e in rec)


def test_free_text_in_demo_mode_is_refused_cleanly():
    r = client.post("/api/request", json={"text": "unlock the doors", "origin": "driver"})
    assert r.status_code == 422


@pytest.mark.parametrize("body", [{"speed_kmh": -1, "gear": "D", "parking_brake": False},
                                  {"speed_kmh": 10, "gear": "X", "parking_brake": False}])
def test_drive_controls_are_validated(body):
    assert client.post("/api/drive", json=body).status_code == 422


def test_every_preset_is_listed_without_its_label():
    cs = client.get("/api/cases").json()
    assert len(cs) >= 30
    assert "expect" not in cs[0] and "actions" not in cs[0]

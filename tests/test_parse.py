"""The parser that turns the model's text into proposals (qnx/core/proposal_parse.c). It decides nothing; it must
never crash, never invent an action, and turn anything off-schema into no action at all."""

from __future__ import annotations

import json

import pytest
from hypothesis import given
from hypothesis import strategies as st

from app import fecore as fc

WIN = "Vehicle.Cabin.Door.Row1.DriverSide.Window.Position"
LOCK = "Vehicle.Cabin.Door.Row1.DriverSide.IsLocked"


def test_a_well_formed_answer_gives_its_actions_and_reply():
    p = fc.parse('{"reply": "Done.", "actions": [{"signal": "%s", "value": 50}, {"signal": "%s", "value": false}]}'
                 % (WIN, LOCK))
    assert p.status == "ok"
    assert p.reply == "Done."
    assert p.actions == ((fc.signal_id(WIN), WIN, 50), (fc.signal_id(LOCK), LOCK, 0))


def test_text_around_the_object_is_ignored():
    p = fc.parse('Sure! {"reply":"ok","actions":[]} hope that helps')
    assert (p.status, p.actions) == ("ok", ())


def test_an_unknown_signal_is_passed_on_as_signal_zero_so_the_gate_records_the_attempt():
    p = fc.parse('{"reply":"","actions":[{"signal":"Vehicle.Airbag.Off","value":1}]}')
    assert p.status == "ok"
    assert p.actions == ((0, "Vehicle.Airbag.Off", 1),)


@pytest.mark.parametrize(
    ("value", "expected"),
    [("true", 1), ("false", 0), ("50.9", 50), ("-3.7", -3), ("1e2", 100), ("99999999999", 2**31 - 1),
     ("-99999999999", -(2**31))],
)
def test_values_are_whole_numbers_rounded_toward_zero_and_clamped(value, expected):
    p = fc.parse('{"reply":"","actions":[{"signal":"%s","value":%s}]}' % (WIN, value))
    assert p.actions[0][2] == expected


@pytest.mark.parametrize(
    ("text", "status"),
    [
        ("", "empty"),
        ("no json here", "empty"),
        ('{"reply":"x"', "syntax"),
        ('{"reply":"x","actions":[{"signal":"a","value":1}', "syntax"),
        ('{"reply":"x"}', "schema"),
        ('{"reply":"x","actions":"unlock"}', "schema"),
        ('{"reply":"x","actions":[{"signal":"a"}]}', "schema"),
        ('{"reply":"x","actions":[{"value":1}]}', "schema"),
        ('{"reply":"x","actions":[{"signal":"a","value":"1"}]}', "schema"),
        ('{"reply":"x","actions":[1]}', "schema"),
    ],
)
def test_anything_off_schema_gives_no_action(text, status):
    p = fc.parse(text)
    assert p.status == status
    assert p.actions == ()


def test_more_than_eight_actions_gives_none():
    acts = ",".join('{"signal":"%s","value":1}' % WIN for _ in range(9))
    p = fc.parse('{"reply":"","actions":[%s]}' % acts)
    assert (p.status, p.actions) == ("too_many", ())


def test_unicode_escapes_in_paths_are_decoded_so_a_disguised_path_is_matched_or_refused_exactly():
    disguised = WIN.replace("W", "\\u0057")
    assert fc.parse('{"reply":"","actions":[{"signal":"%s","value":5}]}' % disguised).actions[0][0] == fc.signal_id(WIN)
    lookalike = WIN.replace("o", "\\u043e", 1)  # Cyrillic o
    assert fc.parse('{"reply":"","actions":[{"signal":"%s","value":5}]}' % lookalike).actions[0][0] == 0


def test_deep_nesting_is_a_syntax_error_not_a_crash():
    p = fc.parse('{"reply":"","x":' + "[" * 5000 + "]" * 5000 + ',"actions":[]}')
    assert p.status == "syntax"


@given(st.binary(max_size=600))
def test_random_bytes_never_crash_the_parser_and_rarely_parse(data):
    p = fc.parse(data)
    assert p.status in fc.PARSE_STATUS
    assert len(p.actions) <= 8
    if p.status != "ok":
        assert p.actions == ()


@given(st.lists(st.tuples(st.sampled_from([WIN, LOCK, "x.y"]), st.integers(-1000, 1000)), max_size=8),
       st.text(max_size=100))
def test_any_answer_the_schema_allows_round_trips(actions, reply):
    text = json.dumps({"reply": reply, "actions": [{"signal": s, "value": v} for s, v in actions]})
    p = fc.parse(text)
    assert p.status == "ok"
    assert [(path, v) for _, path, v in p.actions] == actions

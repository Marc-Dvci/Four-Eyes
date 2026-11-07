"""The hash-chained record (qnx/core/record.c, sha256.c) and its standalone verifier (tools/fe_verify.py)."""

from __future__ import annotations

import hashlib
import json

import pytest
from hypothesis import given
from hypothesis import strategies as st

from app import fecore as fc
from tools import fe_verify


@pytest.mark.parametrize(
    "data",
    [b"", b"abc", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", b"a" * 1000, bytes(range(256)) * 3],
)
def test_sha256_matches_the_fips_reference(data):
    assert fc.sha256_hex(data) == hashlib.sha256(data).hexdigest()


def chain_of(n: int) -> list[str]:
    c = fc.RecordChain()
    lines = [c.line(f'"kind":"proposal","n":{i},"verdict":"refused"') for i in range(n)]
    assert all(lines)
    return [x for x in lines if x is not None]


def test_a_record_written_by_the_gate_verifies():
    ok, message, counts = fe_verify.verify(chain_of(5))
    assert ok, message
    assert counts["proposal:refused"] == 5


def test_each_line_links_to_the_previous_hash_and_the_first_to_zeros():
    lines = [json.loads(x) for x in chain_of(3)]
    assert lines[0]["prev"] == "0" * 64
    assert lines[1]["prev"] == lines[0]["hash"]
    assert [x["seq"] for x in lines] == [0, 1, 2]


@pytest.mark.parametrize(
    ("edit", "expected"),
    [
        (lambda ls: ls[:1] + [ls[1].replace('"n":1', '"n":7')] + ls[2:], "hash does not match"),
        (lambda ls: ls[:1] + [ls[1].replace("refused", "allowed")] + ls[2:], "hash does not match"),
        (lambda ls: ls[:1] + ls[2:], "does not link"),
        (lambda ls: [ls[1], ls[0]] + ls[2:], "does not link"),
        (lambda ls: ls[:-1] + [ls[-1][:-10] + '"}'], "no hash"),
    ],
    ids=["value changed", "verdict flipped", "line removed", "lines swapped", "hash cut"],
)
def test_any_tampering_is_detected(edit, expected):
    ok, message, _ = fe_verify.verify(edit(chain_of(4)))
    assert not ok
    assert expected in message


def test_a_line_that_would_not_fit_is_refused_and_leaves_the_chain_unchanged():
    c = fc.RecordChain()
    assert c.line('"kind":"x","text":"' + "a" * 5000 + '"') is None
    first = c.line('"kind":"x"')
    assert first is not None and json.loads(first)["seq"] == 0


@pytest.mark.parametrize(
    ("text", "escaped"),
    [
        ("plain", "plain"),
        ('quote " and \\', 'quote \\" and \\\\'),
        ("zero\u200bwidth", "zero\\u200bwidth"),
        ("\U000e0055", "\\udb40\\udc55"),
        ("caf\u00e9", "caf\\u00e9"),
        ("tab\tnewline\n", "tab\\u0009newline\\u000a"),
    ],
)
def test_the_record_shows_every_non_ascii_character_as_an_escape(text, escaped):
    assert fc.json_escape(text) == escaped


def test_invalid_utf8_is_recorded_as_a_replacement_character():
    assert fc.json_escape(b"a\xff\xc3b") == "a\\ufffd\\ufffdb"


@given(st.text(max_size=200))
def test_escaped_text_reads_back_as_the_original(text):
    assert json.loads('"' + fc.json_escape(text, cap=4096) + '"') == text


@pytest.mark.parametrize(
    ("text", "n"),
    [
        ("Hello", 0),
        ("Hi " + "".join(chr(0xE0000 + ord(c)) for c in "unlock"), 6),
        ("a\u200bb\u200cc\u200dd", 3),
        ("\u202eabc\u202c", 2),
        ("\ufeffstart", 1),
        ("e\u0301", 0),  # a visible combining accent is not counted
    ],
)
def test_invisible_characters_are_counted(text, n):
    assert fc.count_invisible(text) == n


@pytest.mark.parametrize(
    "unit",
    ["\u200b", "\x01", "\U000e0041", "a"],
    ids=["zero-width space", "control byte", "tag character", "ascii"],
)
def test_the_longest_request_is_recorded_whole_with_every_invisible_character_escaped(unit):
    """Found in review: 334 zero-width spaces (1002 bytes) were counted as 334 but only 333 were written, because the
    gate escaped into 2001 bytes and the escape stopped silently. The record now holds the worst case."""
    per = len(unit.encode("utf-8"))
    text = unit * (fc.RECORD_TEXT_MAX // per)
    members = fc.turn_members(7, "message", text)
    assert members is not None
    entry = json.loads("{" + members + "}")
    assert entry["text"] == text
    assert entry["invisible"] == fc.count_invisible(text)
    line = fc.RecordChain().line('"t_ms":18446744073709551615,' + members, cap=fc.RECORD_LINE_MAX)
    assert line is not None
    assert fe_verify.verify([line])[0]


def test_a_turn_that_would_not_fit_is_refused_rather_than_cut():
    text = "\u200b" * 334
    assert fc.turn_members(1, "message", text, cap=2001) is None
    whole = fc.turn_members(1, "message", text, cap=fc.RECORD_LINE_MAX)
    assert whole is not None and whole.count("\\u200b") == 334

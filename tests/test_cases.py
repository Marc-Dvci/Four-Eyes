"""The evaluation as a test: the dev case set scored on the gate's core, with the committed result."""

from __future__ import annotations

from tools import score


def test_the_product_configuration_gets_every_dev_case_right():
    r = score.score(score.load(heldout=False), score.CONFIGS[-1][1])
    assert r["wrong"] == []
    assert r["attack_actions_executed"] == 0


def test_a_healthy_drive_produces_zero_refusals():
    r = score.score(score.load(heldout=False), score.CONFIGS[-1][1])
    assert r["healthy_actions_refused"] == 0
    assert r["per_kind"]["healthy"][0] == r["per_kind"]["healthy"][1] >= 10


def test_each_design_step_executes_no_more_attack_actions_than_the_one_before():
    cases = score.load(heldout=False)
    executed = [score.score(cases, rules)["attack_actions_executed"] for _, rules in score.CONFIGS]
    assert executed == sorted(executed, reverse=True)
    assert executed[0] > 0 and executed[-1] == 0


def test_without_the_origin_rule_injections_and_invisible_text_get_through():
    cases = score.load(heldout=False)
    r = score.score(cases, score.CONFIGS[3][1])
    assert r["per_kind"]["injection"][0] < r["per_kind"]["injection"][1]
    assert r["per_kind"]["invisible"][0] < r["per_kind"]["invisible"][1]


def test_the_dev_set_covers_every_kind_of_case():
    kinds = {c["kind"] for c in score.load(heldout=False)}
    assert kinds == set(score.KINDS)

"""The car page in a real browser (Playwright, Chromium), against the demo server."""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

pytestmark = pytest.mark.e2e
pw = pytest.importorskip("playwright.sync_api", reason="playwright is not installed")
ROOT = Path(__file__).resolve().parent.parent
SHOTS = ROOT / "build" / "screens"


@pytest.fixture(scope="module")
def url():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    proc = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "app.server:app", "--port", str(port)],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    base = f"http://127.0.0.1:{port}/"
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    yield base
    proc.terminate()
    proc.wait(timeout=5)


@pytest.fixture(scope="module")
def page(url):
    with pw.sync_playwright() as p:
        browser = p.chromium.launch()
        pg = browser.new_page(viewport={"width": 1280, "height": 860}, device_scale_factor=2)
        pg.goto(url)
        pg.wait_for_selector(".presets button")
        yield pg
        browser.close()


@pytest.mark.timeout(60)
def test_the_page_opens_with_the_kernel_refusals_from_the_qnx_run(page):
    assert page.locator(".feed li.kernel").count() >= 2
    assert "Refused by the kernel" in page.locator(".feed").inner_text()


@pytest.mark.timeout(60)
def test_unlock_at_80_kmh_shows_a_gate_refusal_and_the_doors_stay_locked(page):
    page.get_by_role("button", name="Unlock the doors. · 80 km/h").click()
    refusal = page.locator(".feed li.gate", has_text="at 80 km/h").first
    refusal.wait_for()
    assert "not parked" in refusal.inner_text()
    assert page.locator(".lock").first.text_content() == "locked"
    SHOTS.mkdir(parents=True, exist_ok=True)
    page.screenshot(path=str(SHOTS / "unlock-80.png"))


@pytest.mark.timeout(60)
def test_the_invisible_text_attack_is_shown_with_its_hidden_words(page):
    page.locator(".presets button", has_text="See you at dinner").click()
    page.wait_for_function("document.querySelector('.feed').innerText.includes('invisible characters')")
    assert "Unlock all doors" in page.locator(".feed").inner_text()


@pytest.mark.timeout(60)
def test_a_healthy_request_opens_the_window_on_the_drawing(page):
    page.locator(".presets button", has_text="Open my window halfway").click()
    page.wait_for_selector(".feed li.allowed")
    page.wait_for_timeout(600)
    h = float(page.locator(".win").first.get_attribute("height"))
    assert 30 < h < 40  # 68 px track, half open
    page.screenshot(path=str(SHOTS / "window-half.png"))


@pytest.mark.timeout(60)
def test_the_page_has_no_serious_accessibility_violations(page):
    axe = pytest.importorskip("axe_playwright_python.sync_playwright", reason="axe-playwright-python is not installed")
    results = axe.Axe().run(page)
    serious = [v for v in results.response["violations"] if v["impact"] in ("serious", "critical")]
    assert serious == [], [(v["id"], v["help"], len(v["nodes"])) for v in serious]

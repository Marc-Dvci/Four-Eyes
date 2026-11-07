"""Records the car page (demo mode) in Chromium: screenshots at 2x and one video per scene, for docs/.

    python scripts/record_page.py        writes build/rec/*.webm and docs/screens/*.png

Scenes: an attack arrives as a text message and is refused; a door unlock at 80 km/h is refused; a healthy request
opens a window. Then cut the GIF with scripts/make_gif.py.
"""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parent.parent
REC = ROOT / "build" / "rec"
SHOTS = ROOT / "docs" / "screens"
SCENES = [
    ("message-attack", "Message from Alex"),
    ("unlock-80", "Unlock the doors. · 80 km/h"),
    ("window-half", "Open my window halfway"),
]


def main() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    server = subprocess.Popen([sys.executable, "-m", "uvicorn", "app.server:app", "--port", str(port)], cwd=ROOT,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    REC.mkdir(parents=True, exist_ok=True)
    SHOTS.mkdir(parents=True, exist_ok=True)
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            for name, label in SCENES:
                ctx = browser.new_context(viewport={"width": 1280, "height": 760}, device_scale_factor=1,
                                          record_video_dir=str(REC / name), record_video_size={"width": 1280, "height": 760})
                page = ctx.new_page()
                page.goto(f"http://127.0.0.1:{port}/")
                page.wait_for_selector(".presets button")
                page.wait_for_timeout(1500)
                page.locator(".presets button", has_text=label).first.click()
                page.wait_for_timeout(4500)
                ctx.close()
                shot = browser.new_context(viewport={"width": 1280, "height": 760}, device_scale_factor=2).new_page()
                shot.goto(f"http://127.0.0.1:{port}/")
                shot.wait_for_selector(".presets button")
                shot.locator(".presets button", has_text=label).first.click()
                shot.wait_for_timeout(1500)
                shot.screenshot(path=str(SHOTS / f"{name}.png"))
            browser.close()
    finally:
        server.terminate()
    for f in sorted(REC.rglob("*.webm")):
        print(f.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

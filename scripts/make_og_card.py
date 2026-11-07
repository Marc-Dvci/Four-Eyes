"""Makes web/og-card.png (1200 x 630) from a real frame of the car page, docs/screens/message-attack.png: scaled to
1200 pixels wide and cropped from the top. Nothing is drawn on it.

    python scripts/make_og_card.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "screens" / "message-attack.png"
OUT = ROOT / "web" / "og-card.png"


def main() -> int:
    im = Image.open(SRC).convert("RGB")
    w, h = im.size
    scale = 1200 / w
    im = im.resize((1200, max(630, round(h * scale))), Image.LANCZOS)
    im.crop((0, 0, 1200, 630)).save(OUT, optimize=True)
    print(f"{OUT.name}: 1200x630 from {SRC.name} ({w}x{h})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

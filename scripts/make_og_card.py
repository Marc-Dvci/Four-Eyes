"""Makes web/og-card.png (1200 x 630) from a real frame of the car page, docs/screens/unlock-80.png.

The frame is cut just below the third decision, where its bar ends, so no line of text is cut; it is scaled to
630 pixels high and the 40 pixels left and right are filled by repeating the page's own edge columns. Nothing is drawn
on it.

    python scripts/make_og_card.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "screens" / "unlock-80.png"
OUT = ROOT / "web" / "og-card.png"
CUT = 1440  # source row just below the third decision (its bar ends at row 1434)


def main() -> int:
    src = Image.open(SRC).convert("RGB")
    part = src.crop((0, 0, src.width, CUT)).resize((1120, 630), Image.LANCZOS)
    card = Image.new("RGB", (1200, 630))
    card.paste(part, (40, 0))
    left = part.crop((0, 0, 1, 630)).resize((40, 630))
    right = part.crop((1119, 0, 1120, 630)).resize((40, 630))
    card.paste(left, (0, 0))
    card.paste(right, (1160, 0))
    card.save(OUT, optimize=True)
    print(f"{OUT.name}: 1200x630 from {SRC.name} rows 0-{CUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

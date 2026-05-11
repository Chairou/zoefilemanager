#!/usr/bin/env python3
"""
Zoe File Manager icon — pure "zoe" lettermark.

The app name "zoe" IS the icon. Three lowercase letters, each rendered in
a distinct brand color, with the middle "o" styled as a cute round face
(two dot eyes) to give the wordmark personality without resorting to any
realistic illustration. Pillow only, no AIGC.
"""
import os
import sys
import subprocess
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.stderr.write("Pillow not installed. Run: pip3 install Pillow\n")
    sys.exit(1)

OUT_DIR = Path(__file__).parent
ICONSET_DIR = OUT_DIR / "ZoeFileManager.iconset"
ICNS_PATH = OUT_DIR / "ZoeFileManager.icns"
PNG_PATH = OUT_DIR / "ZoeFileManager.png"

# Palette
BG = (46, 52, 64, 255)               # Nord polar night
BLUE = (79, 195, 247, 255)           # sky blue — z
GOLD = (255, 213, 79, 255)           # warm yellow — o
GREEN = (163, 190, 140, 255)         # aurora green — e
DEEP = (26, 26, 46, 255)             # dark ink — eye dots inside o


# Try progressively nicer rounded display fonts first.
# All chosen fonts are rounded/friendly on macOS.
FONT_CANDIDATES = [
    "/System/Library/Fonts/Avenir Next.ttc",
    "/Library/Fonts/Avenir Next.ttc",
    "/System/Library/Fonts/Supplemental/Avenir Next.ttc",
    "/System/Library/Fonts/SFNSRounded.ttf",
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]


def pick_font(size_px: int) -> ImageFont.FreeTypeFont | None:
    if size_px < 8:
        return None
    for fp in FONT_CANDIDATES:
        if os.path.exists(fp):
            try:
                return ImageFont.truetype(fp, size_px)
            except Exception:
                continue
    return None


def draw_letter(img: Image.Image, ch: str, pos: tuple[int, int],
                font: ImageFont.FreeTypeFont, fill: tuple) -> tuple[int, int]:
    """Draw one letter at pos; return its (width, height)."""
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), ch, font=font)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    # Offset by bbox top-left so pos means "top-left of rendered glyph"
    d.text((pos[0] - bbox[0], pos[1] - bbox[1]), ch, font=font, fill=fill)
    return w, h


def draw_icon(size: int) -> Image.Image:
    s = size / 1024.0
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    def S(v):
        return int(v * s)

    # Background squircle
    d.rounded_rectangle([0, 0, size, size], radius=S(220), fill=BG)

    # Letter rendering — pick a size that fills the canvas nicely.
    # Target: three letters "zoe" span ~720px of the 1024 canvas.
    font_px = max(8, S(540))
    font = pick_font(font_px)

    if font is None:
        # Absolute fallback: draw three colored circles labeled nothing.
        # Very unlikely on macOS but safe.
        cx_positions = [S(280), S(512), S(744)]
        colors = [BLUE, GOLD, GREEN]
        r = S(140)
        for cx, col in zip(cx_positions, colors):
            d.ellipse([cx - r, S(420), cx + r, S(420) + 2 * r], fill=col)
        return img

    # Measure each letter individually
    metrics = []
    for ch in "zoe":
        bbox = d.textbbox((0, 0), ch, font=font)
        metrics.append({
            "ch": ch,
            "w": bbox[2] - bbox[0],
            "h": bbox[3] - bbox[1],
            "ox": bbox[0],
            "oy": bbox[1],
        })

    # Horizontal layout — tight letter spacing, centered as a block
    spacing = S(14)
    total_w = sum(m["w"] for m in metrics) + spacing * (len(metrics) - 1)
    start_x = (size - total_w) // 2
    # Vertical centering — use the tallest metric as the reference
    max_h = max(m["h"] for m in metrics)
    baseline_y = (size - max_h) // 2

    colors = {"z": BLUE, "o": GOLD, "e": GREEN}
    x = start_x
    o_cx = None
    o_cy = None
    o_h = 0
    for m in metrics:
        draw_pos_x = x - m["ox"]
        draw_pos_y = baseline_y - m["oy"]
        d.text((draw_pos_x, draw_pos_y), m["ch"], font=font, fill=colors[m["ch"]])
        if m["ch"] == "o":
            o_cx = x + m["w"] // 2
            o_cy = baseline_y + m["h"] // 2
            o_h = m["h"]
        x += m["w"] + spacing

    # Pure lettermark — no eyes, no smile inside the "o".
    return img


def make_iconset():
    if ICONSET_DIR.exists():
        for f in ICONSET_DIR.iterdir():
            f.unlink()
    else:
        ICONSET_DIR.mkdir()

    spec = [
        (16, "icon_16x16.png"),
        (32, "icon_16x16@2x.png"),
        (32, "icon_32x32.png"),
        (64, "icon_32x32@2x.png"),
        (128, "icon_128x128.png"),
        (256, "icon_128x128@2x.png"),
        (256, "icon_256x256.png"),
        (512, "icon_256x256@2x.png"),
        (512, "icon_512x512.png"),
        (1024, "icon_512x512@2x.png"),
    ]
    for size, fname in spec:
        img = draw_icon(size)
        img.save(ICONSET_DIR / fname, "PNG")
    print(f"Wrote {len(spec)} PNGs into {ICONSET_DIR}")


def main():
    print("Rendering 1024x1024 master...")
    master = draw_icon(1024)
    master.save(PNG_PATH, "PNG")
    print(f"  → {PNG_PATH}")

    print("Building iconset directory...")
    make_iconset()

    print("Compiling .icns via iconutil...")
    try:
        subprocess.run(
            ["iconutil", "-c", "icns", str(ICONSET_DIR), "-o", str(ICNS_PATH)],
            check=True,
        )
        print(f"  → {ICNS_PATH}")
    except FileNotFoundError:
        print("  iconutil not available (not on macOS?). PNGs are still in iconset/.")
        return

    print("Done.")


if __name__ == "__main__":
    main()

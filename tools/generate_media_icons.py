"""Generate the source PNGs for media-controller icons.

Drops 256x256 transparent-background PNGs into icons-buttons/. Run
generate-icons.py afterwards to (re)build src/assets/icons.h.
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw

OUT_DIR = Path(__file__).resolve().parent.parent / "icons-buttons"
SIZE = 256
BLACK = (0, 0, 0, 255)
WHITE = (255, 255, 255, 255)


def new_icon():
    im = Image.new("RGBA", (SIZE, SIZE), WHITE)
    return im, ImageDraw.Draw(im)


def save(im, name):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    im.save(OUT_DIR / f"{name}.png")


def power(d):
    cx, cy = SIZE // 2, SIZE // 2 + 8
    r = 80
    stroke = 22
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=BLACK, width=stroke)
    # Erase the top wedge so the bar can come through.
    d.pieslice((cx - r - 4, cy - r - 4, cx + r + 4, cy + r + 4),
               start=255, end=285, fill=WHITE)
    # Vertical bar from well above the circle into its centre.
    d.rounded_rectangle((cx - 12, 24, cx + 12, cy - 8), radius=6, fill=BLACK)


def volume_plus(d):
    cx, cy = SIZE // 2, SIZE // 2
    arm = 80
    t = 22
    d.rounded_rectangle((cx - arm, cy - t // 2, cx + arm, cy + t // 2),
                        radius=8, fill=BLACK)
    d.rounded_rectangle((cx - t // 2, cy - arm, cx + t // 2, cy + arm),
                        radius=8, fill=BLACK)


def volume_minus(d):
    cx, cy = SIZE // 2, SIZE // 2
    arm = 80
    t = 22
    d.rounded_rectangle((cx - arm, cy - t // 2, cx + arm, cy + t // 2),
                        radius=8, fill=BLACK)


def play(d):
    # Right-pointing triangle.
    d.polygon([(70, 48), (70, SIZE - 48), (210, SIZE // 2)], fill=BLACK)


def replay(d):
    cx, cy = SIZE // 2, SIZE // 2
    r = 80
    stroke = 22
    # Counter-clockwise arc (most of a circle, gap on top-right).
    d.arc((cx - r, cy - r, cx + r, cy + r),
          start=200, end=70, fill=BLACK, width=stroke)
    # Arrowhead at the 200 deg end, pointing along the arc's direction (up-left).
    ang = math.radians(200)
    tip_x = cx + r * math.cos(ang)
    tip_y = cy + r * math.sin(ang)
    head = 38
    # Triangle pointing left and slightly up.
    d.polygon(
        [
            (tip_x - head, tip_y + head // 4),
            (tip_x + head // 2, tip_y - head),
            (tip_x + head // 2, tip_y + head),
        ],
        fill=BLACK,
    )


def main():
    icons = {
        "power": power,
        "volume-plus": volume_plus,
        "volume-minus": volume_minus,
        "play": play,
        "replay": replay,
    }
    for name, draw_fn in icons.items():
        im, d = new_icon()
        draw_fn(d)
        save(im, name)
        print(f"  {name}.png")


if __name__ == "__main__":
    main()

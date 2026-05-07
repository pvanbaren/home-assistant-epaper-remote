"""Rasterize each .svg in icons-buttons/ to a 256x256 .png next to it.

Run this whenever new SVG icons are added before invoking generate-icons.py.
Existing PNGs are overwritten only when a same-named SVG exists.
"""
import sys
from pathlib import Path

from svglib.svglib import svg2rlg
from reportlab.graphics import renderPM

ICONS_DIR = Path(__file__).resolve().parent.parent / "icons-buttons"
TARGET_SIZE = 256


def render(svg_path: Path) -> Path:
    drawing = svg2rlg(str(svg_path))
    scale = TARGET_SIZE / drawing.width
    drawing.width = int(drawing.width * scale)
    drawing.height = int(drawing.height * scale)
    drawing.scale(scale, scale)
    png_path = svg_path.with_suffix(".png")
    renderPM.drawToFile(drawing, str(png_path), fmt="PNG")
    return png_path


def main(only=None) -> None:
    if not ICONS_DIR.is_dir():
        sys.exit(f"icons-buttons/ not found at {ICONS_DIR}")
    svgs = sorted(ICONS_DIR.glob("*.svg"))
    if only:
        svgs = [p for p in svgs if p.stem in only]
    if not svgs:
        print("no SVGs to render")
        return
    for svg in svgs:
        png = render(svg)
        print(f"  {svg.name} -> {png.name}")


if __name__ == "__main__":
    main(sys.argv[1:] if len(sys.argv) > 1 else None)

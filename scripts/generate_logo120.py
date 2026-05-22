#!/usr/bin/env python3
"""Generate src/images/Logo120.h from src/images/logo.svg.

The firmware embeds Logo120.h directly for the boot and sleep logo.  Keep
logo.svg as the editable source, then run this script to refresh the C array.
"""

from __future__ import annotations

import argparse
import io
import sys
from pathlib import Path

try:
    import cairosvg
    from PIL import Image
except ImportError as exc:
    print(
        "Missing dependency. Install project Python dependencies that provide "
        "'cairosvg' and 'Pillow', then run this script again.",
        file=sys.stderr,
    )
    raise SystemExit(1) from exc


def pack_1bit_white_is_one(img: Image.Image, threshold: int) -> list[int]:
    gray = img.convert("L")
    width, height = gray.size
    pixels = gray.tobytes()
    packed: list[int] = []

    for y in range(height):
        for x in range(0, width, 8):
            byte = 0
            for bit_index in range(8):
                px = x + bit_index
                if px >= width:
                    continue
                value = pixels[y * width + px]
                bit = 1 if value >= threshold else 0
                byte |= bit << (7 - bit_index)
            packed.append(byte)

    return packed


def render_svg(svg_path: Path, width: int, height: int) -> Image.Image:
    png_bytes = cairosvg.svg2png(
        url=str(svg_path), output_width=width, output_height=height
    )
    img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")

    # The e-ink logo array has no alpha channel, so flatten onto white.
    background = Image.new("RGBA", img.size, (255, 255, 255, 255))
    background.paste(img, mask=img.split()[3])
    return background.rotate(90, expand=True)


def write_header(out_path: Path, packed: list[int], width: int, height: int) -> None:
    lines = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// Image dimensions: 120x120 (generated from src/images/logo.svg)",
        "static const uint8_t Logo120[] = {",
    ]

    for i in range(0, len(packed), 19):
        line = ", ".join(f"0x{value:02x}" for value in packed[i : i + 19])
        suffix = "," if i + 19 < len(packed) else ""
        lines.append(f"    {line}{suffix}")

    lines.extend(["};", ""])
    out_path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main() -> int:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Generate Logo120.h from logo.svg")
    parser.add_argument(
        "--input", type=Path, default=project_root / "src" / "images" / "logo.svg"
    )
    parser.add_argument(
        "--output", type=Path, default=project_root / "src" / "images" / "Logo120.h"
    )
    parser.add_argument(
        "--preview", type=Path, default=project_root / "src" / "images" / "Logo120.png"
    )
    parser.add_argument("--width", type=int, default=120)
    parser.add_argument("--height", type=int, default=120)
    parser.add_argument("--threshold", type=int, default=128)
    args = parser.parse_args()

    if not args.input.exists():
        print(f"Input SVG not found: {args.input}", file=sys.stderr)
        return 1

    img = render_svg(args.input, args.width, args.height)
    packed = pack_1bit_white_is_one(img, args.threshold)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_header(args.output, packed, args.width, args.height)

    if args.preview:
        args.preview.parent.mkdir(parents=True, exist_ok=True)
        img.save(args.preview)

    print(f"Wrote {args.output}")
    if args.preview:
        print(f"Wrote {args.preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

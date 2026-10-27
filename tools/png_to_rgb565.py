#!/usr/bin/env python3

import argparse
from pathlib import Path

from PIL import Image


def rgb888_to_rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert a PNG image to RGB565 or RGBA8888 data."
    )
    parser.add_argument("source", type=Path, help="Path to the input PNG image")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Path for the RGB565 output (default: input path with .rgb565 suffix)",
    )
    parser.add_argument(
        "--format",
        choices=("rgb565", "rgba8888"),
        default="rgb565",
        help="Output pixel format (default: rgb565)",
    )
    parser.add_argument(
        "--canvas-size",
        type=int,
        help="Center the input image in a square RGB565 canvas of this size",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    source = args.source
    suffix = ".rgb565" if args.format == "rgb565" else ".rgba8888"
    destination = args.output or source.with_suffix(suffix)

    image = Image.open(source).convert("RGBA")

    if args.canvas_size is not None:
        if args.format != "rgb565":
            raise ValueError("--canvas-size is only supported for RGB565 output")
        if image.width > args.canvas_size or image.height > args.canvas_size:
            raise ValueError(
                f"Image {image.size} does not fit in {args.canvas_size}x{args.canvas_size}"
            )
        canvas = Image.new("RGBA", (args.canvas_size, args.canvas_size))
        canvas.alpha_composite(
            image,
            ((args.canvas_size - image.width) // 2, (args.canvas_size - image.height) // 2),
        )
        image = canvas

    output = bytearray()

    for red, green, blue, alpha in image.get_flattened_data():
        if args.format == "rgba8888":
            output.extend((red, green, blue, alpha))
            continue

        # RGB565 has no alpha channel, so transparent pixels flatten onto black.
        red = red * alpha // 255
        green = green * alpha // 255
        blue = blue * alpha // 255

        pixel = rgb888_to_rgb565(red, green, blue)

        # Most ESP32 LCD configurations expect RGB565 in big-endian byte order.
        output.append((pixel >> 8) & 0xFF)
        output.append(pixel & 0xFF)

    destination.write_bytes(output)

    print(f"Wrote {destination}")
    print(f"Size: {len(output):,} bytes")


if __name__ == "__main__":
    main()
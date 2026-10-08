#!/usr/bin/env python3
"""Convert a KestrelUI RGBA PAM to PNG, optionally over a checkerboard."""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--checkerboard", action="store_true")
    args = parser.parse_args()
    with args.source.open("rb") as stream:
        if stream.readline() != b"P7\n":
            parser.error("Expected P7 PAM")
        fields = {}
        for _ in range(16):
            line = stream.readline()
            if line == b"ENDHDR\n":
                break
            key, value = line.decode("ascii").strip().split(" ", 1)
            fields[key] = value
        else:
            parser.error("Invalid PAM header")
        if fields.get("DEPTH") != "4" or fields.get("MAXVAL") != "255" or fields.get("TUPLTYPE") != "RGB_ALPHA":
            parser.error("Expected 8-bit RGBA")
        size = int(fields["WIDTH"]), int(fields["HEIGHT"])
        if not (0 < size[0] <= 4096 and 0 < size[1] <= 2160):
            parser.error("Invalid image dimensions")
        pixels = stream.read(size[0] * size[1] * 4 + 1)
        if len(pixels) != size[0] * size[1] * 4:
            parser.error("Invalid pixel payload length")
    image = Image.frombytes("RGBA", size, pixels)
    if args.checkerboard:
        background = Image.new("RGBA", size, "#283542")
        draw = ImageDraw.Draw(background)
        for y in range(0, size[1], 64):
            for x in range(0, size[0], 64):
                if (x // 64 + y // 64) % 2:
                    draw.rectangle((x, y, x + 63, y + 63), fill="#394958")
        image = Image.alpha_composite(background, image)
    image.save(args.output)
    alpha = Image.frombytes("RGBA", size, pixels).getchannel("A")
    print(f"{args.output}: {size[0]}x{size[1]}, alpha range {alpha.getextrema()}")


if __name__ == "__main__":
    main()

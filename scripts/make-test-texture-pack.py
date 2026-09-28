"""Build a visible TEST pack from dumped textures (developer tool).

usage: make-test-texture-pack.py DUMP_DIR REPLACE_DIR [--min 256] [--max 512]
                                 [--limit 200] [--scale 2] [--formats k_DXT1]

For each dumped texture of the chosen guest formats and size range, writes
REPLACE_DIR/<id>.dds: the dump upscaled (Lanczos), tinted magenta with a grid,
as R8G8B8A8_UNORM with a full mip chain (DX10 header). Only for checking that
replacement works; real packs keep the dump's look.
"""
import argparse
import json
import pathlib
import struct

from PIL import Image, ImageDraw


def dds_rgba8(levels, width, height):
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000,
                         height, width, 0, 0, len(levels))
    pixel_format = struct.pack("<II4s20x", 32, 0x4, b"DX10")
    caps = struct.pack("<IIIII", 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 28, 3, 0, 1, 0)
    body = b"".join(level.tobytes() for level in levels)
    return header + pixel_format + caps + dx10 + body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dump")
    parser.add_argument("replace")
    parser.add_argument("--min", type=int, default=256)
    parser.add_argument("--max", type=int, default=512)
    parser.add_argument("--limit", type=int, default=200)
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--formats", default="k_DXT1")
    args = parser.parse_args()
    formats = set(args.formats.split(","))
    out = pathlib.Path(args.replace)
    out.mkdir(parents=True, exist_ok=True)
    made = 0
    total = 0
    for meta_path in sorted(pathlib.Path(args.dump).glob("*.json")):
        meta = json.loads(meta_path.read_text())
        if meta["guest_format_name"] not in formats:
            continue
        if not (args.min <= min(meta["width"], meta["height"]) and
                max(meta["width"], meta["height"]) <= args.max):
            continue
        try:
            image = Image.open(meta_path.with_suffix(".dds"))
            image.load()
        except Exception:
            continue
        image = image.convert("RGBA")
        width, height = image.size[0] * args.scale, image.size[1] * args.scale
        image = image.resize((width, height), Image.LANCZOS)
        r, g, b, a = image.split()
        g = g.point(lambda v: v * 35 // 100)
        image = Image.merge("RGBA", (r, g, b, a))
        draw = ImageDraw.Draw(image)
        step = max(16, width // 8)
        for x in range(0, width, step):
            draw.line([(x, 0), (x, height)], fill=(255, 255, 0, 255), width=max(1, width // 256))
        for y in range(0, height, step):
            draw.line([(0, y), (width, y)], fill=(255, 255, 0, 255), width=max(1, width // 256))
        levels = [image]
        while levels[-1].size != (1, 1):
            w, h = levels[-1].size
            levels.append(levels[-1].resize((max(1, w // 2), max(1, h // 2)), Image.BOX))
        data = dds_rgba8(levels, width, height)
        (out / (meta["id"] + ".dds")).write_bytes(data)
        made += 1
        total += len(data)
        if made >= args.limit:
            break
    print(f"test pack: {made} files, {total / 1048576:.1f} MB in {out}")


if __name__ == "__main__":
    main()

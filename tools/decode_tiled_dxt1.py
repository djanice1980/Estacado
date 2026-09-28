#!/usr/bin/env python3
"""Decode a raw Xenos 2D tiled DXT1 base level to a diagnostic BMP."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def tiled_offset_2d(x: int, y: int, pitch: int, log2_bpp: int) -> int:
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2_bpp + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpp
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (
        ((offset & ~0x1FF) << 3)
        + ((y & 16) << 7)
        + ((offset & 0x1C0) << 2)
        + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
        + (offset & 0x3F)
    )


def rgb565(value: int) -> tuple[int, int, int, int]:
    r = (value >> 11) & 31
    g = (value >> 5) & 63
    b = value & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255)


def decode_block(block: bytes, swap_8in16: bool) -> list[tuple[int, int, int, int]]:
    # Guest fetch endianness k8in16 is removed by the runtime loader. Raw guest
    # captures need the swap here; final host BC1 resource readbacks do not.
    if swap_8in16:
        block = b"".join(block[i : i + 2][::-1] for i in range(0, 8, 2))
    c0, c1, indices = struct.unpack("<HHI", block)
    p0 = rgb565(c0)
    p1 = rgb565(c1)
    if c0 > c1:
        palette = [
            p0,
            p1,
            tuple((2 * p0[i] + p1[i]) // 3 for i in range(3)) + (255,),
            tuple((p0[i] + 2 * p1[i]) // 3 for i in range(3)) + (255,),
        ]
    else:
        palette = [
            p0,
            p1,
            tuple((p0[i] + p1[i]) // 2 for i in range(3)) + (255,),
            (0, 0, 0, 0),
        ]
    return [palette[(indices >> (2 * pixel)) & 3] for pixel in range(16)]


def write_bmp(path: Path, width: int, height: int, pixels: list[tuple[int, int, int, int]]) -> None:
    row_bytes = (width * 3 + 3) & ~3
    image_bytes = row_bytes * height
    header_bytes = 14 + 40
    with path.open("wb") as output:
        output.write(struct.pack("<2sIHHI", b"BM", header_bytes + image_bytes, 0, 0, header_bytes))
        output.write(struct.pack("<IIIHHIIIIII", 40, width, height, 1, 24, 0, image_bytes,
                                 2835, 2835, 0, 0))
        padding = bytes(row_bytes - width * 3)
        for y in range(height - 1, -1, -1):
            for x in range(width):
                r, g, b, a = pixels[y * width + x]
                # Make transparent DXT1 texels visible as magenta evidence.
                output.write(bytes((b, g, r)) if a else bytes((255, 0, 255)))
            output.write(padding)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument(
        "--linear-host",
        action="store_true",
        help="input is the runtime's final linear, endian-corrected BC1 resource",
    )
    args = parser.parse_args()

    source = args.input.read_bytes()
    block_width = (args.width + 3) // 4
    block_height = (args.height + 3) // 4
    pixels = [(0, 0, 0, 0)] * (args.width * args.height)
    highest_source_end = 0
    for by in range(block_height):
        for bx in range(block_width):
            offset = (
                (by * block_width + bx) * 8
                if args.linear_host
                else tiled_offset_2d(bx, by, block_width, 3)
            )
            highest_source_end = max(highest_source_end, offset + 8)
            if offset + 8 > len(source):
                raise ValueError(f"source ends inside tiled block at 0x{offset:X}")
            block_pixels = decode_block(
                source[offset : offset + 8], swap_8in16=not args.linear_host
            )
            for py in range(4):
                y = by * 4 + py
                if y >= args.height:
                    continue
                for px in range(4):
                    x = bx * 4 + px
                    if x < args.width:
                        pixels[y * args.width + x] = block_pixels[py * 4 + px]

    write_bmp(args.output, args.width, args.height, pixels)
    opaque = [pixel for pixel in pixels if pixel[3]]
    averages = tuple(sum(pixel[channel] for pixel in opaque) // max(1, len(opaque))
                     for channel in range(3))
    print(
        f"decoded={args.width}x{args.height} layout={'linear-host' if args.linear_host else 'xenos-tiled'} "
        f"blocks={block_width}x{block_height} "
        f"source_extent=0x{highest_source_end:X} opaque={len(opaque)} "
        f"transparent={len(pixels) - len(opaque)} average_rgb={averages} output={args.output}"
    )


if __name__ == "__main__":
    main()

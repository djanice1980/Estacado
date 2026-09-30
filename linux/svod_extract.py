#!/usr/bin/env python3
"""Extract an Xbox 360 Games-on-Demand (SVOD) package to a folder.

Port of ReXGlue/Xenia StfsContainerDevice::ReadSVOD / BlockToOffsetSVOD.
Usage: svod_extract.py <header file> <out dir> [--list] [--only NAME]
"""
import os
import struct
import sys

BLOCK_SIZE = 0x800
HASH_BLOCK_SIZE = 0x1000
BLOCKS_PER_L0_HASH = 0x198
HASHES_PER_L1_HASH = 0xA1C4
BLOCKS_PER_FILE = 0x14388
MAX_FILE_SIZE = 0xA290000
MEDIA_MAGIC = b"MICROSOFT*XBOX*MEDIA"


class Svod:
    def __init__(self, header_path):
        with open(header_path, "rb") as f:
            hdr = f.read(0x1000)
        vd = 0x379
        self.egdf = bool(hdr[vd + 0x18] & 0x40)
        self.start_data_block = int.from_bytes(hdr[vd + 0x1C:vd + 0x1F], "little")
        data_dir = header_path + ".data"
        names = sorted(os.listdir(data_dir))
        self.files = [open(os.path.join(data_dir, n), "rb") for n in names]
        f0 = self.files[0]
        f0.seek(0x2000)
        if self.egdf and f0.read(20) == MEDIA_MAGIC:
            self.magic_offset = 0x2000
        else:
            f0.seek(0x12000)
            if f0.read(20) != MEDIA_MAGIC:
                raise SystemExit("unsupported SVOD layout")
            # XSF layout: magic at 0x12000, no extra block shift.
            self.magic_offset = 0x12000
            self.egdf = False

    def block_to_offset(self, block):
        true_block = block - self.start_data_block * 2
        if self.egdf:
            true_block += 2
        file_block = true_block % BLOCKS_PER_FILE
        file_index = true_block // BLOCKS_PER_FILE
        l0 = file_block // BLOCKS_PER_L0_HASH + 1
        offset = l0 * HASH_BLOCK_SIZE
        l1 = l0 // HASHES_PER_L1_HASH + 1
        offset += l1 * HASH_BLOCK_SIZE
        addr = file_block * BLOCK_SIZE + offset
        if addr >= MAX_FILE_SIZE:
            file_index += 1
            addr %= MAX_FILE_SIZE
            addr += 0x2000
        return file_index, addr

    def read_at(self, fi, addr, n):
        f = self.files[fi]
        f.seek(addr)
        return f.read(n)

    def root(self):
        block, size = struct.unpack("<II", self.read_at(0, self.magic_offset + 0x14, 8))
        return block

    def walk(self, block, ordinal=0, prefix=""):
        off = ordinal * 4
        fi, addr = self.block_to_offset(block + off // 0x800)
        addr += off % 0x800
        raw = self.read_at(fi, addr, 0xE)
        node_l, node_r, data_block, length, attrs, name_len = struct.unpack("<HHIIBB", raw)
        name = self.read_at(fi, addr + 0xE, name_len).decode("latin-1")
        if node_l:
            yield from self.walk(block, node_l, prefix)
        path = prefix + name
        if attrs & 0x10:
            yield path, True, 0, 0
            if length:
                yield from self.walk(data_block, 0, path + "/")
        else:
            yield path, False, data_block, length
        if node_r:
            yield from self.walk(block, node_r, prefix)

    def read_file(self, data_block, length, out):
        remaining = length
        block = data_block
        while remaining > 0:
            fi, addr = self.block_to_offset(block)
            n = min(BLOCK_SIZE, remaining)
            out.write(self.read_at(fi, addr, n))
            remaining -= n
            block += 1


def main():
    header, out_dir = sys.argv[1], sys.argv[2]
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    svod = Svod(header)
    for path, is_dir, data_block, length in svod.walk(svod.root()):
        if "--list" in sys.argv:
            print(f"{'D' if is_dir else 'F'} {length:>12} {path}")
            continue
        if only and path.lower() != only.lower():
            continue
        dest = os.path.join(out_dir, path)
        if is_dir:
            os.makedirs(dest, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(dest) or out_dir, exist_ok=True)
            with open(dest, "wb") as f:
                svod.read_file(data_block, length, f)


if __name__ == "__main__":
    main()

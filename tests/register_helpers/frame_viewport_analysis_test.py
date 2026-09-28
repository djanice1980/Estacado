"""Synthetic record tests only, not captured camera acceptance."""
import runpy
import struct
import unittest
from pathlib import Path

decode = runpy.run_path(str(Path(__file__).resolve().parents[2] /
                           'scripts/analyze-frame-viewport.py'))['decode_record']


def fixture(bounds=(-10, -20, 1280, 720), selected=(-30, 5, 1400, 900)):
    data = bytearray(416)
    struct.pack_into('>4i', data, 240, *bounds)
    struct.pack_into('>4i', data, 316, *selected)
    data[256], data[336] = 1, 1
    words = ','.join(data[i:i+4].hex() for i in range(0, 416, 4))
    return ('FRAME_VIEWPORT_COPY sequence=0 function=0x82762790 caller=0x820e3630 '
            'source=0x1000 owner_r31=0x2000 render_context=0x3000 '
            'scope=consumer_copy_not_temporal_history words=' + words)


class ViewportTests(unittest.TestCase):
    def test_signed_clamp_and_byte_order(self):
        record = decode(fixture())
        self.assertEqual(record['derived_clipped_rectangle'], [-10, 5, 1280, 720])
        self.assertEqual(record['dirty_byte_336'], 1)
        self.assertEqual(record['mode_byte_256'], 1)
        self.assertIn('not_temporal_history', record['scope'])

    def test_inverted_bounds_preserve_guest_order(self):
        self.assertEqual(decode(fixture((20, 30, 10, 5)))['derived_clipped_rectangle'],
                         [10, 5, 10, 5])

    def test_reject_truncated_or_wrong_boundary(self):
        for line in (fixture()[:-9], fixture().replace('820e3630', '820e3634'),
                     fixture().replace('words=', 'words=zz')):
            with self.assertRaises(ValueError):
                decode(line)


if __name__ == '__main__':
    unittest.main()

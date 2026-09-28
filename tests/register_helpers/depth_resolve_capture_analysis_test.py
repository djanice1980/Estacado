"""Synthetic evidence-rejection tests; not gameplay or GPU acceptance."""
from pathlib import Path
import runpy
import tempfile
import unittest
import numpy as np

helpers = runpy.run_path(str(Path(__file__).resolve().parents[2] /
                            'scripts/analyze-depth-resolve-captures.py'))
verify = helpers['verify']


class ResolveEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.name = 'rex_temporal_depth_resolve_1_frame_3206.bin'
        (self.root / self.name).write_bytes(b'test')
        self.queue = ('REX_TEMPORAL_DEPTH_RESOLVE frame=3206 resolve=2 capture=1 '
                      'last_draw=253 queued=1 address=0x1118A000 bytes=4 '
                      'control=0x00000004 surface=0x14010500 depth=0x00010000 '
                      'dest_info=0x00004302 dest_pitch=0x02D00500 dest_base=0x1118A000 '
                      'window_scissor=00000000,01800500 window_offset=00000000 '
                      f'path={self.name} scope=post_resolve_packed_range_not_complete_camera_depth')
        self.ready = ('REX_EMBEDDED_TEXTURE_SOURCE_READBACK result=1 draw=253 '
                      'address=0x1118A000 bytes=4 written=4 submission=24 completed=24 '
                      f'path={self.name} hash=AFD071E5')
        self.log = self.queue + '\n' + self.ready + '\n'
        self.layout = ('REX_EMBEDDED_RESOLVE_GRID ordinal=1 source_native=0 '
                       'dest_scaled=0 dest_base=0x1118A000 extent=0x1118A000+4 '
                       'tiles=0,16,24,16 rectangles=1 native_rectangles=0 '
                       'original_base=1118A000 rect=0,0,1280,384 pitch=1280 '
                       'format=23 endian=2\n')

    def tearDown(self):
        self.temp.cleanup()

    def prepare_layout(self):
        # Enough tiled storage for an 8x8 rectangle. This fixture is synthetic.
        data = bytes(4096)
        (self.root / self.name).write_bytes(data)
        fnv = 2166136261
        for byte in data:
            fnv = ((fnv ^ byte) * 16777619) & 0xFFFFFFFF
        self.log = self.log.replace('bytes=4', 'bytes=4096').replace('written=4', 'written=4096')
        self.log = self.log.replace('AFD071E5', f'{fnv:08X}')
        self.layout = self.layout.replace('+4 ', '+4096 ').replace('1280,384', '8,8')

    def test_completed_range_keeps_scope(self):
        result = verify(self.log, self.root)
        self.assertEqual(result['ranges'][0]['fnv1a32'], 'AFD071E5')
        self.assertFalse(result['complete_camera_depth'])
        self.assertEqual(result['overlapping_ranges'], [])

    def test_incomplete_or_wrong_copy(self):
        for old, new in [('queued=1', 'queued=0'), ('completed=24', 'completed=23'),
                         ('submission=24', 'submission=0'), ('written=4', 'written=3'),
                         ('draw=253 address=', 'draw=252 address='),
                         ('result=1', 'result=0'), ('surface=0x14010500', 'surface=0x14000500'),
                         ('depth=0x00010000', 'depth=0x00000000')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                verify(self.log.replace(old, new), self.root)

    def test_duplicate_missing_or_reordered_records(self):
        for log in ['', self.queue, self.log + self.ready,
                    self.ready + '\n' + self.queue, self.log + self.queue]:
            with self.subTest(log=log), self.assertRaises(ValueError):
                verify(log, self.root)

    def test_corrupt_or_short_file(self):
        for data in [b'fail', b'tes']:
            (self.root / self.name).write_bytes(data)
            with self.assertRaises(ValueError):
                verify(self.log, self.root)

    def test_range_and_path_bounds(self):
        for old, new in [('bytes=4', 'bytes=0'), ('bytes=4', 'bytes=8388612'),
                         ('0x1118A000', '0x20000000'), ('0x1118A000', '0x1118A001'),
                         (self.name, '../outside.bin')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                verify(self.log.replace(old, new), self.root)

    def test_second_range_cannot_mix_frames(self):
        other_name = 'rex_temporal_depth_resolve_2_frame_3207.bin'
        (self.root / other_name).write_bytes(b'test')
        other = self.log.replace('frame=3206', 'frame=3207').replace('capture=1', 'capture=2')
        other = other.replace('resolve=2', 'resolve=5').replace(self.name, other_name)
        with self.assertRaises(ValueError):
            verify(self.log + other, self.root)

    def test_two_valid_overlapping_ranges_are_not_merged(self):
        other_name = 'rex_temporal_depth_resolve_2_frame_3206.bin'
        (self.root / other_name).write_bytes(b'test')
        other = self.log.replace('capture=1', 'capture=2').replace('resolve=2', 'resolve=5')
        other = other.replace(self.name, other_name)
        result = verify(self.log + other, self.root)
        self.assertEqual(result['overlapping_ranges'], [[1, 2]])
        self.assertFalse(result['complete_camera_depth'])

    def test_layout_requires_actual_metadata_and_retains_scope(self):
        self.prepare_layout()
        with self.assertRaises(ValueError):
            verify(self.log, self.root, require_layout=True)
        result = verify(self.layout + self.log, self.root, require_layout=True)
        self.assertEqual(result['ranges'][0]['layout']['rect'], [0, 0, 8, 8])
        self.assertEqual(result['ranges'][0]['layout']['texture_format'], 23)
        self.assertFalse(result['complete_camera_depth'])

    def test_layout_rejects_mismatches_and_unsupported_encoding(self):
        self.prepare_layout()
        for old, new in [('dest_scaled=0', 'dest_scaled=1'),
                         ('extent=0x1118A000+4096', 'extent=0x1118A000+4092'),
                         ('original_base=1118A000', 'original_base=1136A000'),
                         ('pitch=1280', 'pitch=800'), ('format=23', 'format=22'),
                         ('endian=2', 'endian=0'),
                         ('rect=0,0,8,8', 'rect=0,0,1280,736'),
                         ('rect=0,0,8,8', 'rect=0,0,8,7'),
                         ('rect=0,0,8,8', 'rect=0,8,8,16'),
                         ('rect=0,0,8,8', 'rect=0,0,1280,384')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                verify(self.layout.replace(old, new) + self.log, self.root, True)

    def test_layout_rejects_duplicate_or_late_metadata(self):
        self.prepare_layout()
        for log in [self.layout * 2 + self.log, self.log + self.layout]:
            with self.assertRaises(ValueError):
                verify(log, self.root, True)

    def test_decoding_uses_tiled_endian_and_float24_depth_without_stencil(self):
        decoder = runpy.run_path(str(Path(__file__).resolve().parents[2] /
                                    'scripts/analyze-depth-source-boundary.py'))
        offsets = decoder['offsets'](8, 8, 1280) // 4
        words = np.zeros(1024, dtype='>u4')
        words[offsets] = 0xF000007F  # 20e4 one, nonzero stencil.
        layout = dict(rect=[0, 0, 8, 8], pitch=1280, texture_format=23, endian=2)
        values = helpers['decode_rectangle'](words.tobytes(), layout)
        self.assertEqual(values.shape, (8, 8))
        np.testing.assert_array_equal(values, np.ones((8, 8), dtype='<f4'))
        with self.assertRaises(ValueError):
            helpers['decode_rectangle'](words.tobytes()[:4], layout)


if __name__ == '__main__':
    unittest.main()

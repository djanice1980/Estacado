import copy
from pathlib import Path
import runpy
import tempfile
import unittest

import numpy as np

m = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-camera-depth-clear.py'))


class ClearSamples(unittest.TestCase):
    def pair(self, samples=2):
        y, x, s = np.indices((384, 640, 4), dtype=np.uint32)
        clear = np.stack(((y * 2560 + x * 4 + s + 1).astype('<f4').view('<u4'),
                          (x + y + s) % 256), axis=-1)
        alias = np.zeros((384, 1280, samples, 2), dtype='<u4')
        # Independent scalar decoding of canonical sample coordinates; exercise
        # all tile boundaries, pixel parities and sample indices with unique data.
        for dx in range(1280):
            for hs in ([0, 1] if samples == 2 else [0, 3]):
                canonical_sample = hs ^ 1 if samples == 2 else hs >> 1
                u = (dx & ~2) | (canonical_sample << 1)
                for dy in range(2):
                    v = dy | (dx & 2)
                    sx = (u // 4) * 2 + (u % 2)
                    ss = ((u // 2) % 2) + ((v // 2) % 2) * 2
                    alias[dy::2, dx, hs] = clear[dy::2, sx, ss]
        return clear, alias

    def test_every_sample_transfers_without_averaging(self):
        clear, alias = self.pair()
        for height in (336, 384):
            r = m['compare_alias'](clear, alias, height)
            self.assertTrue(r['exact_sample_transfer'])
            self.assertFalse(r['clear_and_alias_depth_zero'])
            self.assertEqual(r['compared_samples'], height * 1280 * 2)
        alias[4, 80, 0, 0] ^= 1
        self.assertEqual(m['compare_alias'](clear, alias, 384)['depth_bit_mismatches'], 1)

    def test_sample_order_and_fallback_mask(self):
        clear, alias = self.pair()
        self.assertFalse(m['compare_alias'](clear, alias[:, :, ::-1].copy(), 384)['exact_sample_transfer'])
        clear, fallback = self.pair(4)
        fallback[:, :, 1:3, :] = 123
        r = m['compare_alias'](clear, fallback, 384)
        self.assertTrue(r['exact_sample_transfer'])
        self.assertEqual(r['active_alias_host_samples'], [0, 3])

    def test_zero_clear_is_separate_from_successful_transfer(self):
        clear = np.zeros((384, 640, 4, 2), dtype='<u4')
        alias = np.zeros((384, 1280, 2, 2), dtype='<u4')
        self.assertTrue(m['compare_alias'](clear, alias, 336)['clear_and_alias_depth_zero'])
        alias[336, 0, 0, 0] = 1
        self.assertTrue(m['compare_alias'](clear, alias, 336)['clear_and_alias_depth_zero'])
        self.assertFalse(m['compare_alias'](clear, alias, 384)['clear_and_alias_depth_zero'])
        with self.assertRaises(ValueError):
            m['compare_alias'](clear, alias, 335)

    def test_payload_shape_and_values(self):
        data = np.zeros((384, 640, 4, 2), dtype='<u4')
        self.assertEqual(m['sample_array'](data.tobytes(), 640, 384, 4).shape, data.shape)
        with self.assertRaises(ValueError):
            m['sample_array'](data.tobytes()[:-8], 640, 384, 4)
        data[0, 0, 0, 0] = 0x7FC00000
        with self.assertRaises(ValueError):
            m['sample_array'](data.tobytes(), 640, 384, 4)
        data[0, 0, 0] = [0, 256]
        with self.assertRaises(ValueError):
            m['sample_array'](data.tobytes(), 640, 384, 4)

    def test_copy_identity_fence_and_full_bytes(self):
        name = 'rex_camera_depth_frame_10_draw_169_after_clear.bin'
        rec = dict(frame='10', draw='169', stage='after_clear', queued='1', rows='384',
                   source_width='640', source_height='1024', host_samples='4', resource='1234', _line=6)
        q = dict(path=name, width='640', height='384', samples='4', bytes='7864320',
                 submission='15', resource='1234', _line=5)
        c = dict(path=name, bytes='7864320', written='7864320', submission='15', completed='15',
                 camera_frame='10', camera_draw='169', result='1', hash='00000000', _line=7)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for key, value in [('completed', '14'), ('camera_frame', '11'), ('camera_draw', '170'),
                               ('written', '4096'), ('result', '0'), ('_line', 4)]:
                wrong = dict(c, **{key: value})
                with self.assertRaisesRegex(ValueError, 'unmatched|incomplete|uncompleted'):
                    m['completed_copy'](rec, [q], [wrong], root)
            (root / name).write_bytes(b'prefix')
            with self.assertRaisesRegex(ValueError, 'truncated'):
                m['completed_copy'](rec, [q], [c], root)

    def test_writer_epoch_rejects_other_regions_and_color_alias(self):
        d = dict(ordinal=2, vs='4C2AF389AE6CB4C1', ps='0000000000000000', surface='14010500',
                 depth='00010000', depth_control='1C708767', scale='1x1', offset='00000000',
                 scissor=[0, 0x1800500], viewport=m['projection']['VIEWPORT'], vte='0000043F', clip='00080000')
        state = dict(vs='0x'+d['vs'], ps='0x'+d['ps'], surface='0x14010500', depth_info='0x00010000',
                     normalized_depth_control='0x1C708767', first_draw='2', last_draw='2', primitive='4',
                     ps_flags='0x0', raster='0x18006', mode='0x5', normalized_color_mask='0x0',
                     color='00030300,00000000,00000000,00000000')
        item = dict(last_draw=2, layout=dict(rect=[0, 0, 1280, 384]),
                    window_offset='00000000', window_scissor='00000000,01800500')
        self.assertEqual(m['writer_epoch']([{}, d], [state], 1, item), [2])
        for key, value in [('surface', '0A020280'), ('offset', '7E800000'), ('scale', '2x2')]:
            with self.assertRaises(ValueError):
                m['writer_epoch']([{}, dict(d, **{key: value})], [state], 1, item)
        wrong = copy.deepcopy(state)
        wrong.update(normalized_color_mask='0xF', color='00030000,00000000,00000000,00000000')
        with self.assertRaisesRegex(ValueError, 'overlapping color'):
            m['writer_epoch']([{}, d], [wrong], 1, item)


if __name__ == '__main__':
    unittest.main()

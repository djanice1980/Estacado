from pathlib import Path
import runpy
import tempfile
import unittest

import numpy as np

root = Path(__file__).resolve().parents[2]
api = runpy.run_path(str(root / 'scripts/analyze-camera-geometry-capture.py'))


def meta():
    return dict(indices='3', index_kind='1', index_format='0', index_offset='2',
                index_endian='1', min_index='0', max_index='16777215', reset_enable='0',
                index_path='indices.bin', index_queued='1')


class GeometryTests(unittest.TestCase):
    def test_odd_16bit_dma_count_and_offset_use_actual_referenced_vertices(self):
        data = b'\x00\x03\x00\x01\x00\x03'
        np.testing.assert_array_equal(api['indices'](meta(), data), [3, 5])
        with self.assertRaises(ValueError):
            api['indices'](meta(), data[:-1])

    def test_reject_converted_reset_and_wrapped_indices(self):
        for key, value in [('index_kind', '2'), ('reset_enable', '1'), ('min_index', '10'),
                           ('index_offset', '16777215'), ('index_endian', '2')]:
            with self.assertRaises(ValueError):
                api['indices'](dict(meta(), **{key: value}), b'\x00\x03\x00\x01\x00\x03')

    def test_automatic_triangle_requires_no_index_file(self):
        m = dict(meta(), index_kind='0', index_path='none', index_queued='0', index_offset='0', max_index='65535')
        np.testing.assert_array_equal(api['indices'](m), [0, 1, 2])
        with self.assertRaises(ValueError):
            api['indices'](m, b'\x00\x00')

    def test_all_endian_modes_and_partial_units(self):
        raw = bytes(range(8))
        expected = [raw, bytes([1,0,3,2,5,4,7,6]), bytes([3,2,1,0,7,6,5,4]), bytes([2,3,0,1,6,7,4,5])]
        for mode, result in enumerate(expected):
            self.assertEqual(api['swap_bytes'](raw, mode), result)
            self.assertEqual(api['swap_bytes'](result, mode), raw)
        with self.assertRaises(ValueError):
            api['swap_bytes'](raw[:-1], 2)

    def test_packed_swizzle_w_uses_second_word_low_half_and_only_referenced_vertices(self):
        words = np.zeros((4, 8), dtype='<u4')
        words[:, 1] = [0xABCD0001, 0x12340000, 0x56780001, 0x9ABC0002]
        data = api['swap_bytes'](words.tobytes(), 2)
        np.testing.assert_array_equal(api['packed_w'](data, 2, 8, np.array([0,2])), [1,1])
        np.testing.assert_array_equal(api['packed_w'](data, 2, 8, np.array([1,3])), [0,2])
        with self.assertRaises(ValueError):
            api['packed_w'](data[:8], 2, 8, np.array([3]))

    def test_clear_vertices_are_finite_complete_float_positions(self):
        data = np.array([[0,0,0,1,0,0,1], [1280,0,0,1,0,0,1], [0,720,0,1,0,0,1]], dtype='<f4')
        positions = api['clear_positions'](api['swap_bytes'](data.tobytes(),2), 2, 7, np.arange(3))
        np.testing.assert_array_equal(positions, data[:, :3])
        data[2, 2] = np.nan
        with self.assertRaises(ValueError):
            api['clear_positions'](data.tobytes(),0,7,np.arange(3))

    def test_gpu_completion_hash_and_full_length_are_required(self):
        payload = b'abcd'; fnv = 2166136261
        for byte in payload: fnv = ((fnv ^ byte) * 16777619) & 0xFFFFFFFF
        ready = dict(path='capture.bin', result='1', submission='8', completed='8',
                     address='1000', bytes='4', written='4', hash=f'{fnv:08X}')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory); (path/'capture.bin').write_bytes(payload)
            data, _ = api['verify_copy']('capture.bin',[ready],path,'capture.bin',0x1000,4)
            self.assertEqual(data,payload)
            for key,value in [('completed','7'),('submission','0'),('written','3'),('hash','00000000')]:
                with self.assertRaises(ValueError):
                    api['verify_copy']('capture.bin',[dict(ready,**{key:value})],path,'capture.bin',0x1000,4)
            (path/'capture.bin').write_bytes(payload[:2])
            with self.assertRaises(ValueError):
                api['verify_copy']('capture.bin',[ready],path,'capture.bin',0x1000,4)


if __name__ == '__main__':
    unittest.main()

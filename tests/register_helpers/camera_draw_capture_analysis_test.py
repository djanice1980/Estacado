import runpy
import struct
import unittest
from pathlib import Path

root = Path(__file__).resolve().parents[2]
api = runpy.run_path(str(root / 'scripts/analyze-camera-draw-capture.py'))
viewport_fixture = runpy.run_path(str(Path(__file__).with_name('frame_viewport_analysis_test.py')))['fixture']


def draw(ordinal=1, bitmap='000000000000000F,0000000000000000,0000000000000000,0000000000000000'):
    words = ','.join(f'{i:08X}' for i in range(16))
    return (f'CAMERA_DRAW draw={ordinal} vs=0123456789ABCDEF ps=0000000000000000 cbuffer=0000000000010000 '
            'surface=14010500 depth=00010000 depth_control=00000006 offset=00000000 '
            'scissor=00000000,01800500 viewport=44200000,44200000,43B40000,43B40000,3F800000,00000000 '
            f'vte=0000003F clip=00000000 scale=1x1 valid=1 count=4 map={bitmap} words={words}')


def gpu():
    return ('REX_EMBEDDED_GAMEPLAY_FRAME swap=40 refresh=1 submitted_draws=1\n'
            'CAMERA_CAPTURE version=1 frame=40 generation=7 draws=1 dropped=0 '
            'source=bound_vertex_float_upload scope=submitted_draw_constants_not_camera_history\n' + draw())


def transform():
    return ('TRANSFORM_INPUT sequence=0 generation=7 copy_sequence=4294967295 copy_associated=0 '
            'window_copy_sequence=0 function=0x82248A78 source=0x1000 builder=0x2000 valid=1 '
            'scope=builder_inputs_not_camera_history words=' + ','.join(f'{i:08x}' for i in range(32)))


class CameraDrawTests(unittest.TestCase):
    def test_owned_dense_map_decodes_and_scope_remains_conservative(self):
        report = api['summarize'](gpu())
        self.assertEqual(report['submitted_draws'], 1)
        self.assertFalse(report['complete_camera_depth'])
        self.assertFalse(report['temporal_history_verified'])
        parsed = api['parse_draw'](draw())
        self.assertEqual(parsed['registers'][3], [12, 13, 14, 15])

    def test_reject_missing_duplicate_and_unordered_draws(self):
        for text in (gpu().replace('draws=1 dropped=', 'draws=2 dropped='),
                     gpu() + '\n' + draw(), gpu().replace('CAMERA_DRAW draw=1', 'CAMERA_DRAW draw=2')):
            with self.assertRaises(ValueError):
                api['parse_gpu'](text)

    def test_reject_incomplete_or_wrong_frame(self):
        for text in (gpu().replace('frame=40', 'frame=41'), gpu().replace('dropped=0', 'dropped=1'),
                     gpu().replace('refresh=1', 'refresh=0'), gpu().replace('generation=7', 'generation=0')):
            with self.assertRaises(ValueError):
                api['parse_gpu'](text)

    def test_reject_invalid_upload_or_map(self):
        for text in (draw().replace('count=4', 'count=5'), draw().replace('valid=1', 'valid=0'),
                     draw()[:-9], draw().replace('cbuffer=0000000000010000', 'cbuffer=0')):
            with self.assertRaises(ValueError):
                api['parse_draw'](text)

    def test_cpu_request_marker_and_unassociated_inputs_are_honest(self):
        viewport = viewport_fixture().replace('sequence=0 ', 'sequence=0 generation=7 ')
        report = api['summarize'](gpu(), viewport, transform())
        self.assertEqual(report['cpu']['transform_count'], 1)
        self.assertEqual(report['cpu']['associated_copy_inputs'], 0)
        self.assertEqual(report['cpu']['exact_matrix_match_count'], 1)
        self.assertFalse(report['temporal_history_verified'])

    def test_reject_cross_request_or_false_causal_label(self):
        viewport = viewport_fixture().replace('sequence=0 ', 'sequence=0 generation=7 ')
        with self.assertRaises(ValueError):
            api['parse_cpu'](viewport.replace('generation=7', 'generation=8'), transform(), 7)
        with self.assertRaises(ValueError):
            api['parse_cpu'](viewport, transform().replace('copy_associated=0', 'copy_associated=1'), 7)

    def test_sparse_neighbors_do_not_fake_matrix_match(self):
        parsed = api['parse_draw'](draw(bitmap='0000000000000017,0000000000000000,0000000000000000,0000000000000000'))
        self.assertEqual(api['matrix_matches']([parsed], [dict(sequence=0,
            selected_transform=list(range(16)), published_matrix=list(range(16, 32)))]), [])

    def test_depth_epochs_must_be_same_actual_frame(self):
        depth = dict(frame=40, ranges=[dict(resolve=2, last_draw=1,
                     window_offset='00000000', window_scissor='00000000,01800500')])
        report = api['summarize'](gpu(), depth_report=depth)
        self.assertEqual(report['depth_epochs'][0]['depth_writing_draws'], [1])
        depth['frame'] = 41
        with self.assertRaises(ValueError):
            api['summarize'](gpu(), depth_report=depth)

    def test_depth_epoch_excludes_prior_tile_and_disabled_z_test(self):
        first = api['parse_draw'](draw())
        old_tile = api['parse_draw'](draw(2))
        second = api['parse_draw'](draw(3).replace('offset=00000000', 'offset=7E800000')
                                  .replace('00000000,01800500', '01800000,02D00500'))
        no_test = dict(second, ordinal=4, depth_control='00000004')
        report = dict(ranges=[dict(resolve=2, last_draw=1, window_offset='00000000',
                                  window_scissor='00000000,01800500'),
                             dict(resolve=5, last_draw=4, window_offset='7E800000',
                                  window_scissor='01800000,02D00500')])
        result = api['depth_epochs']([first, old_tile, second, no_test], report)
        self.assertEqual(result[1]['depth_writing_draws'], [3])
        self.assertEqual(result[1]['other_region_writers'], 1)

    def test_restricted_raw_builder_product_omits_translation_and_transposes(self):
        bits = lambda xs: list(struct.unpack('>16I', struct.pack('>16f', *xs)))
        selected = bits([0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 400, -50, 20, 1])
        projection = bits([2, 0, 0, 0, 0, -3, 0, 0, 0, 0, 1.5, 1, 0, 0, -4, 0])
        expected = bits([0, -2, 0, 0, -3, 0, 0, 0, 0, 0, 1.5, -4, 0, 0, 1, 0])
        self.assertEqual(api['builder_rotation_product'](selected, projection), expected)
        selected[12:15] = [0, 0, 0]
        self.assertEqual(api['builder_rotation_product'](selected, projection), expected)

    def test_reject_general_projection_and_nonfinite_inputs(self):
        bits = lambda xs: list(struct.unpack('>16I', struct.pack('>16f', *xs)))
        selected = bits([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        projection = bits([2, 0, 0, 0, 0, -3, 0, 0, 0, 0, 1.5, 1, 0, 0, -4, 0])
        for index, value in ((1, 0x3F800000), (0, 0x7F800000), (11, 0)):
            changed = projection[:]; changed[index] = value
            with self.assertRaises(ValueError):
                api['builder_rotation_product'](selected, changed)


if __name__ == '__main__':
    unittest.main()

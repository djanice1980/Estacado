import copy
from pathlib import Path
import runpy
import tempfile
import unittest

import numpy as np

root = Path(__file__).resolve().parents[2]
api = runpy.run_path(str(root / 'scripts/analyze-camera-depth-projection.py'))


def projection():
    return np.array([[2, 0, 0, 0], [0, -3, 0, 0], [0, 0, 1.001, 1], [0, 0, -2, 0]])


def state():
    return dict(vs='0x123', ps='0x456', surface='0x14010500', depth_info='0x00010000',
                depth_control='0x6', first_draw='1', last_draw='10', ps_flags='0x0',
                raster='0x18006', normalized_depth_control='0x6', primitive='4', mode='0x5')


def draw():
    return dict(vs='123', ps='456', surface='14010500', depth='00010000', depth_control='6',
                ordinal=3, viewport=api['VIEWPORT'], vte='43F', clip='80000', scale='1x1',
                registers={7: [0, 0, 0, 0], 8: [0, 0x3F800000, 0, 0],
                           76: [0, 0, 0, 0x3F800000], 77: [0, 0, 0, 0]})


class ProjectionTests(unittest.TestCase):
    def test_geometry_join_requires_every_same_frame_draw_and_keeps_history_unverified(self):
        report = dict(frame=30, request_generation=1, reviewed_shaders={'ABC': 'packed_w'},
                      epochs=[dict(input_w_unverified_draws=[12, 13])],
                      complete_camera_depth=False, temporal_history_verified=False)
        evidence = dict(frame=30, request_generation=1, all_captured_packed_input_w_one=True,
                        draws=[dict(draw=i, vertex_shader='ABC', input_w_one=True, input_w_unique=[1])
                               for i in (12, 13)], gpu_copies=4, gpu_bytes=256)
        for changed in (dict(evidence, frame=31), dict(evidence, request_generation=2),
                        dict(evidence, draws=evidence['draws'][:1]),
                        dict(evidence, all_captured_packed_input_w_one=False)):
            with self.assertRaises(ValueError):
                api['apply_geometry_evidence'](copy.deepcopy(report), changed)
        api['apply_geometry_evidence'](report, evidence)
        self.assertTrue(report['matching_geometry_w_verified'])
        self.assertEqual(report['epochs'][0]['input_w_unverified_draws'], [])
        self.assertFalse(report['complete_camera_depth'])
        self.assertFalse(report['temporal_history_verified'])

    def test_reverse_depth_reconstructs_independently_projected_positions(self):
        p = projection()
        z = np.array([2.1, 5, 100, 500])
        positions = np.array([[0, 0, v, 1] for v in z])
        clip = positions @ p
        values = 1 - clip[:, 2] / clip[:, 3]
        result, stats = api['linearize'](values, p, [np.linalg.inv(p.T)[3]])
        np.testing.assert_allclose(result, z, rtol=1e-6)
        self.assertLess(stats['round_trip_max_depth_error'], 1e-6)

    def test_conversion_does_not_assume_vertex_w_one(self):
        p = projection()
        positions = np.array([[3, -5, 100, 2], [6, 4, 50, 0.5]])
        clip = positions @ p
        result, _ = api['linearize'](1 - clip[:, 2] / clip[:, 3], p, [np.linalg.inv(p.T)[3]])
        np.testing.assert_allclose(result, positions[:, 2] / positions[:, 3], rtol=1e-6)
        self.assertFalse(np.array_equal(result, positions[:, 2]))

    def test_reject_clear_clipped_nonfinite_or_unbounded_conversion(self):
        p = projection(); rows = [np.linalg.inv(p.T)[3]]
        for values in ([0], [1], [-0.01], [1.01], [np.nan], [np.inf], []):
            with self.assertRaises(ValueError):
                api['linearize'](values, p, rows)
        wrong = np.array(rows); wrong[0, 0] += 0.1
        for inverse in ([], [[np.nan] * 4], wrong):
            with self.assertRaises(ValueError):
                api['linearize']([0.01], p, inverse)

    def test_state_must_exclude_depth_override_bias_and_unsupported_viewport(self):
        api['verify_draw_state'](draw(), [state()])
        # CAMERA_DRAW records the normalized control used by the actual draw.
        api['verify_draw_state'](draw(), [dict(state(), depth_control='0x876')])
        for key, value in [('ps_flags', '0x1'), ('raster', '0x18806'), ('primitive', '1')]:
            alternate = dict(state(), **{key: value})
            with self.assertRaises(ValueError):
                api['verify_draw_state'](draw(), [state(), alternate])
        with self.assertRaises(ValueError):
            api['verify_draw_state'](draw(), [])
        with self.assertRaises(ValueError):
            api['verify_draw_state'](draw(), [dict(state(), normalized_depth_control='0x4')])
        for key, value in [('vte', '3F'), ('clip', '0'), ('scale', '2x2')]:
            with self.assertRaises(ValueError):
                api['verify_draw_state'](dict(draw(), **{key: value}), [state()])

    def test_packed_vertex_w_remains_unknown_and_fixed_w_is_checked(self):
        for kind in ('xyz1', 'decoded_xyz1', 'skinned'):
            self.assertTrue(api['input_w_verified'](draw(), kind))
        self.assertFalse(api['input_w_verified'](draw(), 'packed_w'))
        for kind, register, component in [('xyz1', 7, 3), ('decoded_xyz1', 76, 3), ('skinned', 8, 1)]:
            changed = copy.deepcopy(draw()); changed['registers'][register][component] = 0x40000000
            with self.assertRaises(ValueError):
                api['input_w_verified'](changed, kind)

    def test_signed_window_offset_places_second_tile_without_merging(self):
        item = dict(window_scissor='01800000,02D00500', window_offset='7E800000',
                    layout=dict(rect=[0, 0, 1280, 336]))
        self.assertEqual(api['placement'](item), [0, 384, 1280, 720])
        for key, value in [('window_offset', '00000000'), ('window_scissor', '81800000,02D00500')]:
            with self.assertRaises(ValueError):
                api['placement'](dict(item, **{key: value}))

    def test_reject_unknown_or_modified_reviewed_shader(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            with self.assertRaises(ValueError):
                api['verify_shader']('0000000000000000', path)
            shader = next(iter(api['SHADERS']))
            (path / ('shader_' + shader + '.ucode.bin.vert')).write_bytes(b'changed')
            with self.assertRaises(ValueError):
                api['verify_shader'](shader, path)

    def test_clear_evidence_requires_identical_frame_and_geometry_writers(self):
        projected = dict(resolve=2, last_draw=80, depth_writing_draws=[2, 3], input_w_unverified_draws=[])
        report = dict(frame=10, request_generation=1, matching_geometry_w_verified=True,
                      epochs=[projected], complete_camera_depth=False, temporal_history_verified=False)
        evidence = dict(frame=10, request_generation=1, clear_alias_coverage_verified=True,
                        gpu_bytes=100, epochs=[dict(resolve=2, last_draw=80, depth_writing_draws=[2, 3], compared_samples=20)])
        for key, value in [('frame', 11), ('request_generation', 2), ('clear_alias_coverage_verified', False)]:
            with self.assertRaises(ValueError):
                api['apply_clear_evidence'](copy.deepcopy(report), dict(evidence, **{key: value}))
        wrong = copy.deepcopy(evidence); wrong['epochs'][0]['depth_writing_draws'] = [2, 4]
        with self.assertRaises(ValueError):
            api['apply_clear_evidence'](copy.deepcopy(report), wrong)
        with self.assertRaises(ValueError):
            api['apply_clear_evidence'](dict(report, matching_geometry_w_verified=False), evidence)
        api['apply_clear_evidence'](report, evidence)
        self.assertTrue(report['pixel_geometry_provenance_verified'])
        self.assertFalse(report['complete_camera_depth'])
        self.assertFalse(report['temporal_history_verified'])

    def test_draw_state_inventory_cannot_drop_or_omit_draws(self):
        valid = ('REX_EMBEDDED_GAMEPLAY_FRAME dropped_draw_states=0 draw_states=1\n'
                 'DRAW state=0 occurrences=3')
        self.assertEqual(len(api['draw_states'](valid, 3)), 1)
        for text in (valid.replace('dropped_draw_states=0', 'dropped_draw_states=1'),
                     valid.replace('occurrences=3', 'occurrences=2'),
                     valid.replace('DRAW state=0', 'DRAW state=1')):
            with self.assertRaises(ValueError):
                api['draw_states'](text, 3)


if __name__ == '__main__':
    unittest.main()

"""Failure-oriented checks for the selected scene-export evidence verifier."""
import copy
from pathlib import Path
import runpy
import tempfile
import unittest

import numpy as np

analysis = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-scene-resolve-captures.py'))


def fixture():
    event = dict(frame='10', resolve='1', last_draw='251', succeeded='1', capture='1',
        kind='1', queued='1', submission='77', address='1271E000', bytes='1966080',
        scaled='0', control='00100140', surface='14010500', color='00030300', depth='00010000',
        dest_info='01000302', dest_pitch='02D00500', dest_base='1271E000',
        window_scissor='00000000,01800500', window_offset='00000000',
        path='rex_scene_resolve_1_frame_10_kind_1.bin')
    layout = dict(frame='10', resolve='1', last_draw='251', source_native='0', dest_scaled='0',
        dest_base='1271E000', extent='1271E000+1966080', tiles='768,16,48,16', rectangles='1',
        original_base='1271E000', rect='0,0,1280,384', pitch='1280', format='6', endian='2')
    owner = dict(frame='10', resolve='1', last_draw='251', owner='0', resource='210F8DA16E0',
        key='00C88300', base='768', pitch32='16', pitch='16', msaa='1', depth='0', format='3',
        scale_native='0', row_first='0', rows='48', first_start='0', last_end='16',
        host_width='1280', host_height='1024', host_format='10', host_samples='2')
    ending = dict(frame='10', resolve='1', logged='1', total='1')
    old = dict(state='0', control=event['control'], surface=event['surface'],
        source_color=event['color'], depth_info=event['depth'], dest_info=event['dest_info'],
        dest_pitch=event['dest_pitch'], dest_base=event['dest_base'], written=event['address'],
        bytes=event['bytes'], written_scaled='0')
    return event, layout, owner, ending, old


def line(tag, f):
    return tag + ' ' + ' '.join(f'{k}={v}' for k, v in f.items())


def epoch_text(event, layout, owner, ending):
    return '\n'.join(line(tag, f) for tag, f in zip(
        ('REX_SCENE_RESOLVE_LAYOUT', 'REX_SCENE_RESOLVE_OWNER',
         'REX_SCENE_RESOLVE_OWNERS', 'REX_SCENE_RESOLVE'), (layout, owner, ending, event)))


class SceneResolveEvidenceTests(unittest.TestCase):
    def test_pixel_partner_frame_shader_order_and_drop_checks(self):
        pixel = dict(draw='1', ps='7EF6E3B55D32AEB4', cbuffer='000000000B003100', valid='1',
            count='1', map=','.join(['0' * 16] * 3 + ['8000000000000000']),
            words='3F000000,00000000,00000000,00000000')
        target = dict(draw='1', color='00030300,00000000,00000000,00000000', mask='0000000F',
            color_control='D7000006', blend=','.join(['00010001'] * 4), bound_bits='3',
            textures='00000000', primitive='4', vertices='12')
        header = dict(version='1', frame='10', draws='1', dropped_resolves='0',
            source='bound_pixel_float_upload', scope='scene_motion_epochs_not_temporal_history')
        lines = ['CAMERA_DRAW draw=1', 'CAMERA_RASTER draw=1', line('CAMERA_PIXEL', pixel),
                 line('CAMERA_TARGET', target), line('CAMERA_SCENE_CAPTURE', header)]
        text = '\n'.join(lines)
        draws = [dict(ordinal=1, ps=pixel['ps'])]
        self.assertEqual(analysis['draw_extensions'](text, 10, draws)[1][1][255][0], 0x3F000000)
        for bad in (text.replace('frame=10', 'frame=11'), text.replace('dropped_resolves=0', 'dropped_resolves=1'),
                    text.replace('7EF6E3B55D32AEB4', '0000000000000001'),
                    '\n'.join(lines[:1] + lines[2:]), '\n'.join(lines[:2] + lines[3:]),
                    '\n'.join(lines[:2] + [lines[3], lines[2], lines[4]]),
                    text + '\n' + lines[2]):
            with self.subTest(bad=bad[:70]), self.assertRaises(ValueError):
                analysis['draw_extensions'](bad, 10, draws)

    def test_sparse_pixel_constants_and_explicit_absent_shader(self):
        f = dict(draw='1', ps='7EF6E3B55D32AEB4', cbuffer='000000000B003100', valid='1',
                 count='1', map='0' * 16 + ',' + '0' * 16 + ',' + '0' * 16 + ',8000000000000000',
                 words='3F000000,00000000,00000000,00000000')
        self.assertEqual(analysis['pixel_upload'](f), {255: [0x3F000000, 0, 0, 0]})
        for key, value in [('valid', '0'), ('count', '2'), ('count', '-1'),
                           ('ps', '0000000000000000'), ('cbuffer', '0000000000000000'),
                           ('cbuffer', '000000000B003104'), ('words', '3F000000')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                analysis['pixel_upload'](dict(f, **{key: value}))
        f.update(ps='0' * 16, cbuffer='0' * 16, count='0', map=','.join(['0' * 16] * 4), words='')
        self.assertEqual(analysis['pixel_upload'](f), {})

    def test_layout_uses_actual_format_and_does_not_require_native_scale_flag(self):
        e, l, o, end, _ = fixture()
        result = analysis['verify_layout'](e, l, [o], end)
        self.assertEqual(result['ownership']['covered_tiles'], 768)
        self.assertIn('not pixel validity', result['scope'])
        # Native-size host targets legitimately have source_native=0. Depth
        # metadata also legitimately overrides the raw destination format.
        e.update(control='00000004', color='00000000', dest_info='00004302')
        l['format'] = '23'
        o.update(depth='1', format='1', host_format='19')
        self.assertEqual(analysis['verify_layout'](e, l, [o], end)['texture_format'], 23)

    def test_stale_truncated_uncovered_or_inconsistent_layout_is_rejected(self):
        cases = [('layout', 'frame', '11'), ('layout', 'last_draw', '250'),
                 ('layout', 'rect', '0,0,1280,385'), ('layout', 'dest_scaled', '1'),
                 ('layout', 'format', '26'), ('layout', 'endian', '1'),
                 ('layout', 'pitch', '1312'), ('layout', 'extent', '1271E000+1'),
                 ('owner', 'resource', '0'), ('owner', 'host_samples', '4'),
                 ('owner', 'last_end', '15'), ('owner', 'host_width', '640'),
                 ('ending', 'total', '2'), ('ending', 'resolve', '2')]
        for record, key, value in cases:
            e, l, o, end, _ = fixture()
            {'layout': l, 'owner': o, 'ending': end}[record][key] = value
            with self.subTest(record=record, key=key), self.assertRaises(ValueError):
                analysis['verify_layout'](e, l, [o], end)
        e, l, o, end, _ = fixture()
        with self.assertRaises(ValueError):
            analysis['verify_layout'](e, l, [o, o], end)

    def test_chronological_resolve_join_rejects_reorder_missing_and_gpu_disagreement(self):
        e, l, o, end, old = fixture()
        text = epoch_text(e, l, o, end)
        kwargs = dict(frame=10, draws=[None] * 251, header={'resolve_events': '1'}, legacy=[old])
        events, readbacks = analysis['resolve_epochs'](text, **kwargs)
        self.assertEqual(len(events), 1)
        self.assertEqual(readbacks, [])
        lines = text.splitlines()
        for bad in ('\n'.join(lines[::-1]), '\n'.join(lines[1:]),
                    '\n'.join(lines + lines), text.replace('resolve=1', 'resolve=2'),
                    text.replace('frame=10', 'frame=11'),
                    text + '\nREX_SCENE_RESOLVE_TRUNCATED frame=10 resolve=2'):
            with self.subTest(bad=bad[:80]), self.assertRaises(ValueError):
                analysis['resolve_epochs'](bad, **kwargs)
        old['written'] = '1271F000'
        with self.assertRaises(ValueError):
            analysis['resolve_epochs'](text, **kwargs)

    def test_completion_fence_epoch_file_path_and_payload_identity(self):
        e, l, o, end, _ = fixture()
        layout = analysis['verify_layout'](e, l, [o], end)
        epoch = dict(record=e, layout=layout, line=10)
        data = bytes(int(e['bytes']))
        completion = dict(result='1', draw=e['last_draw'], address=e['address'],
            bytes=e['bytes'], written=e['bytes'], submission='77', completed='78', path=e['path'],
            hash=f"{analysis['fnv1a32'](data):08X}")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            file = root / e['path']
            file.write_bytes(data)
            good = analysis['verify_copy'](epoch, [(20, completion)], root, 1)
            self.assertEqual(good['completed'], 78)
            for key, value in [('result', '0'), ('draw', '250'), ('address', '1271F000'),
                               ('written', '0'), ('submission', '76'), ('completed', '76'),
                               ('path', 'stale.bin'), ('hash', '00000000')]:
                bad = dict(completion, **{key: value})
                with self.subTest(key=key), self.assertRaises(ValueError):
                    analysis['verify_copy'](epoch, [(20, bad)], root, 1)
            for records in ([(9, completion)], [(20, completion), (21, completion)]):
                with self.assertRaises(ValueError):
                    analysis['verify_copy'](epoch, records, root, 1)
            altered = copy.deepcopy(epoch)
            altered['record']['path'] = '../outside.bin'
            with self.assertRaises(ValueError):
                analysis['verify_copy'](altered, [(20, completion)], root, 1)
            altered = copy.deepcopy(epoch)
            altered['layout']['owners'][0]['depth'] = '1'
            with self.assertRaises(ValueError):
                analysis['verify_copy'](altered, [(20, completion)], root, 1)
            file.write_bytes(b'\x01' + data[1:])
            with self.assertRaises(ValueError):
                analysis['verify_copy'](epoch, [(20, completion)], root, 1)
            file.write_bytes(data[:-4])
            with self.assertRaises(ValueError):
                analysis['verify_copy'](epoch, [(20, completion)], root, 1)

    def test_tiling_bank_boundaries_and_32_64_bit_endian_channels(self):
        # Independent anchor offsets at macro/micro/bank transitions from the
        # native texture layout, including the 64-bit change at x4/y2/x32.
        anchors = {(0, 0): (0, 0), (1, 0): (4, 8), (4, 0): (32, 256),
                   (8, 0): (64, 64), (16, 0): (128, 128), (0, 1): (16, 16),
                   (0, 2): (256, 512), (0, 16): (2048, 2048),
                   (32, 0): (4096, 8192), (0, 32): (163840, 327680)}
        for column, log2_bytes in enumerate((2, 3)):
            offsets = analysis['tiled_offsets'](64, 64, 1280, log2_bytes)
            for (x, y), expected in anchors.items():
                self.assertEqual(int(offsets[y, x]), expected[column])
        layout = dict(rect=[0, 0, 16, 8], pitch=32, texture_format=6, endian=2)
        data = bytearray(4096)
        data[64:68] = bytes.fromhex('01020304')
        decoded = analysis['decode_rectangle'](data, layout)
        np.testing.assert_array_equal(decoded[0, 8], [4, 3, 2, 1])
        layout.update(texture_format=26, endian=1)
        data[64:72] = bytes.fromhex('00010002FFFEFFFF')
        decoded = analysis['decode_rectangle'](data, layout)
        np.testing.assert_array_equal(decoded[0, 8], [1, 2, 65534, 65535])
        self.assertEqual(decoded.dtype, np.dtype('<u2'))
        with self.assertRaises(ValueError):
            analysis['decode_rectangle'](data[:64], layout)
        layout['endian'] = 2
        with self.assertRaises(ValueError):
            analysis['decode_rectangle'](data, layout)

    def test_reviewed_constant_nonfinite_rejected(self):
        with self.assertRaises(ValueError):
            analysis['register_values']({255: [0x7FC00000, 0, 0, 0]})

    def test_actual_fetch_swizzle_and_exponent_are_distinct_from_stored_channels(self):
        motion = analysis['fetch_encoding']('8A024802,1271E086,0059E4FF,00A80C14,00000003,00000218')
        self.assertEqual(motion['swizzle'], [2, 1, 0, 3])
        stored = np.array([[[0, 128, 128, 0]]], dtype='u1')
        np.testing.assert_array_equal(analysis['fetch_point_values'](stored, motion),
                                      np.array([[[128, 128, 0, 0]]]) / 255)
        color = analysis['fetch_encoding']('8A024802,10A5905A,0059E4FF,00A88D10,00000003,00000218')
        self.assertEqual(color['exponent'], 4)
        self.assertEqual(color['endian'], 1)
        np.testing.assert_array_equal(analysis['fetch_point_values'](np.full((1, 1, 4), 65535, dtype='<u2'), color),
                                      np.full((1, 1, 4), 16.0))
        with self.assertRaises(ValueError):
            analysis['fetch_encoding']('8A024802,10A5905A,0059E4FF,00A88D11,00000003,00000218')


if __name__ == '__main__':
    unittest.main()

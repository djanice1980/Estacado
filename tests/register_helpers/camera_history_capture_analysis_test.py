"""Failure-focused checks for adjacent-frame evidence and ambiguous instances."""
import copy
import hashlib
from pathlib import Path
import runpy
import struct
import tempfile
import unittest
from unittest import mock

a = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-camera-history-capture.py'))


def words(values):
    return list(struct.unpack('<4I', struct.pack('<4f', *values)))


def encoded(values, width=8):
    return ','.join(f'{v:0{width}X}' for v in values)


def record(seed=1, ordinal=2, translation=0):
    regs = {i: words(row) for i, row in enumerate(((1, 0, 0, 0), (0, 1, 0, 0),
            (0, 0, 1, 0), (0, 0, 0, 1)))}
    regs[7] = words((0, 0, 0, 0))
    for start in (12, 16):
        regs.update({start + i: row[:] for i, row in list(regs.items()) if i < 4})
        regs[start] = words((1, 0, 0, translation))
    return dict(ordinal=ordinal, seed=seed, regs=regs)


def constant_line(tag, draw, regs, stage=None):
    bitmap = [sum(1 << (i % 64) for i in regs if i // 64 == slot) for slot in range(4)]
    prefix = f'{tag} draw={draw} ' + (f'stage={stage} ' if stage else '')
    return (prefix + f'cbuffer=0000000000000100 valid=1 count={len(regs)} map={encoded(bitmap, 16)} '
            f"words={encoded([w for i in sorted(regs) for w in regs[i]])}")


RASTER = ('viewport=44200000,44200000,C3B40000,43B40000,BF800000,3F800000 '
    'vte=0000043F clip=00080000 raster=00018006 vertex_control=00000005 '
    'host_scissor=0,0,1280,384 host_viewport=00000000,00000000,44A00000,44340000,00000000,3F000000 '
    'host_ndc=3F800000,00000000,3F800000,80000000,BF800000,3F800000')
DRAW = ('surface=14010500 depth=00010000 depth_control=1C708722 offset=00000000 '
    'scissor=00000000,01800500 scale=1x1')


def frame_text(index=0, records=None, generation=1, first=10):
    records = records if records is not None else [record(), record(2, 5)]
    count = max(r['ordinal'] for r in records) + 1
    lines = [f'CAMERA_HISTORY version=1 generation={generation} first_frame={first} frame={first + index} '
        f'index={index} frames=2 refreshed=1 frontbuffer=01000000 observed={count} selected={len(records)} '
        f'dropped=0 failures=0 aborted=0 submission={100 + index * 20} completed={90 + index * 20} '
        f"scope={a['HEADER_SCOPE']}"]
    for r in records:
        n, seed = r['ordinal'], r['seed']
        lines.append(f"HISTORY_DRAW draw={n} vs=FA91501A8940251D ps={a['MOTION_PS']} submission={90 + index * 20} {DRAW} "
            f'mask=0000000F bound_bits=3 primitive=4 vertices=6 index_kind=1 index_address={0x1000 + seed * 0x100:08X} '
            'index_format=0 index_endian=1 index_offset=0 min_index=0 max_index=16777215 reset_enable=0 reset_index=65535 '
            'bindings=1 bindings_complete=1 memexport=0')
        lines.append(f'HISTORY_VERTEX draw={n} slot=0 fetch=95 words=00004003,10000400 stride=5')
        lines.append(constant_line('HISTORY_CONSTANTS', n, r['regs'], 'vertex'))
        lines.append(constant_line('HISTORY_CONSTANTS', n, {255: words((.5, 0, 0, 0))}, 'pixel'))
        lines.append(f'HISTORY_RASTER draw={n} {RASTER}')
    lines.append(f'HISTORY_END generation={generation} frame={first + index} records={len(records)}')
    return '\n'.join(lines) + '\n'


def primary_text(records=None):
    records = records if records is not None else [record(), record(2, 5)]
    count = max(r['ordinal'] for r in records) + 1
    selected = {r['ordinal']: r for r in records}
    lines = [f'REX_EMBEDDED_GAMEPLAY_FRAME swap=10 refresh=1 submitted_draws={count}',
        f'CAMERA_CAPTURE version=1 frame=10 generation=1 draws={count} dropped=0 source=bound_vertex_float_upload '
        'scope=submitted_draw_constants_not_camera_history']
    raster_fields = a['fields']('RASTER ' + RASTER)
    for n in range(1, count + 1):
        regs = selected.get(n, record(3, n))['regs']
        ps = a['MOTION_PS'] if n in selected else '0000000000000000'
        constant = constant_line('CAMERA_DRAW', n, regs)
        lines.append(constant + f' vs=FA91501A8940251D ps={ps} {DRAW} viewport={raster_fields["viewport"]} vte=0000043F clip=00080000')
        lines.append(f'CAMERA_RASTER draw={n} ' + ' '.join(f'{k}={raster_fields[k]}' for k in
                     ('raster', 'vertex_control', 'host_viewport', 'host_ndc', 'host_scissor')))
    return '\n'.join(lines) + '\n'


def events_text():
    return ('REX_EMBEDDED_MANUAL_CAPTURE_REQUEST generation=1 enabled=1\n'
        'REX_EMBEDDED_MANUAL_CAPTURE_ARMED generation=1 after_swap=9\n' + ''.join(
        f'REX_CAMERA_HISTORY_FRAME generation=1 frame={10 + index} index={index} selected=2 dropped=0 failures=0 '
        f'aborted=0 written=1 path=rex_camera_history_generation_1_frame_{10 + index}.log '
        f"scope={a['COMPLETION_SCOPE']}\n" for index in range(2)))


class HistoryCaptureTests(unittest.TestCase):
    def test_actual_sparse_upload_and_first_frame_copy_join(self):
        f = a['parse_frame'](frame_text())
        self.assertEqual(f['draws'][0]['vertex_constants'], record()['regs'])
        self.assertEqual(f['draws'][0]['pixel_constants'][255], words((.5, 0, 0, 0)))
        a['verify_primary'](f, primary_text())
        second = a['parse_frame'](frame_text(1))
        a['verify_events']([f, second], events_text())

    def test_file_completeness_order_counts_and_scope(self):
        good = frame_text()
        variants = [good.replace('selected=2', 'selected=1'), good.replace('selected=2', 'selected=129'),
            good.replace('observed=6', 'observed=1'), good.replace('records=2', 'records=3'),
            good.replace('refreshed=1', 'refreshed=0'), good.replace('frontbuffer=01000000', 'frontbuffer=00000000'),
            good.replace('dropped=0', 'dropped=1'), good.replace('failures=0', 'failures=1'),
            good.replace('aborted=0', 'aborted=1'), good.replace('version=1', 'version=2'),
            good.replace('generation=1 ', 'generation=1 generation=1 ', 1),
            good.replace(a['HEADER_SCOPE'], 'complete_history'), good + 'HISTORY_END records=2\n',
            '\n'.join(good.splitlines()[:-1]), good.replace('HISTORY_DRAW draw=5', 'HISTORY_DRAW draw=2')]
        for bad in variants:
            with self.subTest(bad=bad[:90]), self.assertRaises(ValueError):
                a['parse_frame'](bad)

    def test_constants_reject_missing_duplicate_corrupt_sparse_map_and_bias(self):
        good = frame_text()
        vertex_line = next(line for line in good.splitlines() if line.startswith('HISTORY_CONSTANTS draw=2 stage=vertex'))
        variants = [good.replace(vertex_line + '\n', ''), good.replace(vertex_line, vertex_line + '\n' + vertex_line),
            good.replace('count=13', 'count=12'), good.replace('valid=1', 'valid=0'),
            good.replace('cbuffer=0000000000000100', 'cbuffer=0000000000000000'),
            good.replace('words=3F000000,00000000,00000000,00000000', 'words=3F800000,00000000,00000000,00000000'),
            good.replace('stage=pixel', 'stage=vertex')]
        for bad in variants:
            with self.subTest(bad=bad[:90]), self.assertRaises(ValueError):
                a['parse_frame'](bad)

    def test_missing_nonfinite_and_nonaffine_transform_rejected(self):
        for defect in ('missing', 'nan', 'inf', 'last_row'):
            r = record()
            if defect == 'missing': del r['regs'][16]
            if defect == 'nan': r['regs'][16][0] = 0x7FC00000
            if defect == 'inf': r['regs'][12][3] = 0x7F800000
            if defect == 'last_row': r['regs'][19][3] = 0
            with self.subTest(defect=defect), self.assertRaises(ValueError):
                a['parse_frame'](frame_text(records=[r]))

    def test_geometry_and_raster_must_be_complete_and_consistent(self):
        good = frame_text()
        for old, new in [('bindings=1', 'bindings=9'), ('bindings_complete=1', 'bindings_complete=0'),
            ('slot=0', 'slot=1'), ('fetch=95', 'fetch=96'), ('stride=5', 'stride=0'),
            ('words=00004003', 'words=00004002'), ('index_address=00001100', 'index_address=1FFFFFFE'),
            ('index_kind=1', 'index_kind=2'), ('memexport=0', 'memexport=1'), ('scale=1x1', 'scale=2x2'),
            ('host_scissor=0,0,1280,384', 'host_scissor=0,0,0,384'), ('HISTORY_RASTER draw=2', 'HISTORY_RASTER draw=3'),
            ('submission=90 ', 'submission=101 '), ('completed=90 ', 'completed=101 ')]:
            with self.subTest(field=old), self.assertRaises(ValueError):
                a['parse_frame'](good.replace(old, new))

    def test_immediate_successor_generation_and_submission_order(self):
        first = a['parse_frame'](frame_text())
        for bad in [frame_text(1, first=11), frame_text(1, generation=2), frame_text(0),
                    frame_text(1).replace('submission=120', 'submission=80').replace('submission=110', 'submission=75').replace('completed=110', 'completed=70')]:
            with self.subTest(bad=bad[:90]), self.assertRaises(ValueError):
                a['correlate'](first, a['parse_frame'](bad))
        with self.assertRaises(ValueError):
            a['correlate'](a['parse_frame'](frame_text(1)), first)

    def test_exact_affine_agreement_never_proves_history_or_object_identity(self):
        first = a['parse_frame'](frame_text())
        second = a['parse_frame'](frame_text(1))
        result = a['correlate'](first, second)
        self.assertEqual(result['later_B_exactly_equals_preceding_A'], 2)
        for key in ('temporal_history_verified', 'persistent_object_identity_verified',
                    'geometry_payload_identity_verified', 'previous_skeletal_deformation_verified'):
            self.assertFalse(result[key])

    def test_binding_pairing_does_not_depend_on_draw_order(self):
        result = a['correlate'](a['parse_frame'](frame_text()),
            a['parse_frame'](frame_text(1, [record(2, 2), record(1, 5)])))
        self.assertEqual(result['unique_binding_pairs'], 2)
        self.assertEqual({(p['preceding_draws'][0], p['later_draws'][0]) for p in result['pairs']}, {(2, 5), (5, 2)})

    def test_duplicate_instances_excluded_even_when_matrices_and_order_suggest_match(self):
        records = [record(1, 2, 0), record(1, 5, 100)]
        result = a['correlate'](a['parse_frame'](frame_text(records=records)),
                                a['parse_frame'](frame_text(1, records)))
        self.assertEqual(result['unique_binding_pairs'], 0)
        self.assertEqual(len(result['ambiguous_binding_groups']), 1)
        self.assertEqual(result['ambiguous_binding_groups'][0]['preceding_draws'], [2, 5])

    def test_changed_binding_never_falls_back_to_equal_transform_or_ordinal(self):
        result = a['correlate'](a['parse_frame'](frame_text()),
            a['parse_frame'](frame_text(1, [record(3, 2), record(2, 5)])))
        self.assertEqual(result['unique_binding_pairs'], 1)
        self.assertEqual(len(result['unmatched_binding_groups']), 2)

    def test_one_bit_and_translation_differences_are_reported_without_epsilon(self):
        changed = record()
        changed['regs'][16][0] ^= 1
        changed['regs'][12][3] = words((.25, 0, 0, 0))[0]
        result = a['correlate'](a['parse_frame'](frame_text()),
            a['parse_frame'](frame_text(1, [changed, record(2, 5)])))
        pair = next(p for p in result['pairs'] if p['preceding_draws'] == [2])
        self.assertEqual(pair['preceding_A_vs_later_B']['bit_equal_components'], 11)
        self.assertEqual(pair['preceding_A_vs_later_B']['bit_equal_basis_components'], 8)
        self.assertEqual(pair['preceding_A_vs_later_A']['bit_equal_translation_components'], 2)
        self.assertEqual(result['changed_current_affines'], 1)
        self.assertEqual(result['changed_current_bases'], 0)

    def test_primary_capture_cannot_disagree_or_omit_a_motion_draw(self):
        first = a['parse_frame'](frame_text())
        good = primary_text()
        for bad in [good.replace('frame=10', 'frame=11'), good.replace('generation=1', 'generation=2'),
            good.replace('ps=' + a['MOTION_PS'], 'ps=0000000000000000', 1),
            good.replace('words=3F800000', 'words=3F800001'),
            good.replace('host_scissor=0,0,1280,384', 'host_scissor=0,0,1280,383')]:
            with self.subTest(bad=bad[:90]), self.assertRaises(ValueError):
                a['verify_primary'](first, bad)

    def test_write_events_require_one_arm_and_two_ordered_successes(self):
        frames = [a['parse_frame'](frame_text(i)) for i in range(2)]
        good = events_text()
        variants = [good.replace('written=1', 'written=0'), good.replace('after_swap=9', 'after_swap=10'),
            good.replace('enabled=1', 'enabled=0'), good.replace('generation=1', 'generation=2', 1),
            good.replace('_frame_11.log', '_frame_12.log'), '\n'.join(good.splitlines()[:-1]),
            good + good.splitlines()[-1] + '\n', '\n'.join(reversed(good.splitlines())),
            good + 'REX_CAMERA_HISTORY_ABORT generation=1 reason=lost_adjacent_boundary\n']
        for bad in variants:
            with self.subTest(bad=bad[:90]), self.assertRaises(ValueError):
                a['verify_events'](frames, bad)

    def test_source_pin_mismatch_and_shader_verification_failure_are_fatal(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'observer.cpp').write_bytes(b'observer')
            pins = {'observer.cpp': hashlib.sha256(b'observer').hexdigest().upper()}
            with mock.patch.dict(a['verify_sources'].__globals__, SOURCE_PINS=pins):
                self.assertEqual(a['verify_sources'](root), pins)
                (root / 'observer.cpp').write_bytes(b'changed')
                with self.assertRaises(ValueError): a['verify_sources'](root)
        verifier = mock.Mock()
        with mock.patch.dict(a['audit'].__globals__, verify_shader=verifier):
            report = a['audit'](frame_text(), frame_text(1), primary_text(), events_text(), Path('shader_fixture'))
            self.assertEqual(verifier.call_count, 2)
            self.assertTrue(report['adjacent_bound_input_capture_verified'])
            self.assertFalse(report['temporal_history_verified'])
            verifier.side_effect = ValueError('changed guest shader')
            with self.assertRaises(ValueError):
                a['audit'](frame_text(), frame_text(1), primary_text(), events_text(), Path('shader_fixture'))


if __name__ == '__main__':
    unittest.main()

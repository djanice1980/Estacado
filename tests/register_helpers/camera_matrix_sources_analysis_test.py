"""Failure-focused source evidence checks; equal values must not create identity."""
import hashlib
import json
from pathlib import Path
import runpy
import tempfile
import unittest
from unittest import mock

a = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-camera-matrix-sources.py'))
g = runpy.run_path(str(Path(__file__).with_name('camera_history_capture_analysis_test.py')))
PAIR = [0x3F800000, 0, 0, 0x40000000, 0, 0x3F800000, 0, 0, 0, 0, 0x3F800000, 0,
        0, 0, 0, 0x3F800000, 0x3F800000, 0, 0, 0x40400000, 0, 0x3F800000, 0, 0,
        0, 0, 0x3F800000, 0, 0, 0, 0, 0x3F800000]


def encoded(words):
    return ','.join(f'{w:08x}' for w in words)


def source_text():
    lines = [f"MATRIX_SOURCE_CAPTURE version=2 generation=1 maximum_records=4096 maximum_copies=2 scope={a['WINDOW_SCOPE']}"]
    for copy_index in range(2):
        for binding in (True, False):
            n = copy_index * 2 + (not binding)
            record = 0x3000 + copy_index * 0x100
            lines.append(f'MATRIX_SOURCE_INPUT sequence={n} generation=1 copy_sequence={copy_index} '
                f"stage={'render_binding_entry' if binding else 'constant_copy_entry'} "
                f"function={'82299e30' if binding else '8224a2e8'} caller={'825a2d40' if binding else '822490d4'} "
                f"stack=1000 record={record if binding else 0:x} primary=0 matrix_array={'0' if binding else '1060'} "
                'pool=6000 pool_index=1 pool_matrix=62a0 builder=8000 '
                f"descriptor={'0' if binding else '1080'} declared_vectors={0 if binding else 8} "
                f'last_binding_sequence={copy_index * 2} last_binding_record={record:x} last_binding_matrix_array=0 '
                f"valid=1 modes={'absent' if binding else '01,01,04,04,04,04,04,04'} scope={a['INPUT_SCOPE']}")
            for slot in range(8):
                lines.append(f'MATRIX_SOURCE_SLOT sequence={n} slot={slot} source=0 present=0 valid=1 words=')
            lines.append(f'MATRIX_SOURCE_VECTOR_STREAM sequence={n} eligible={int(not binding)} '
                f"source={'0' if binding else f'{0x9000 + copy_index * 0x100:x}'} mask={'0' if binding else 'ffff'} "
                f"default_zero=0 default_one={'0' if binding else '3f800000'} vectors={0 if binding else 8} valid=1 "
                f"words={'' if binding else encoded(PAIR)}")
    lines.append(f"MATRIX_SOURCE_END generation=1 records=4 dropped=0 invalid=0 write_failures=0 copies=2 boundary={a['BOUNDARY']} scope={a['WINDOW_SCOPE']}")
    return '\n'.join(lines) + '\n'


def events():
    return ('REX_EMBEDDED_MANUAL_CAPTURE_REQUEST generation=1 enabled=1\n'
        f"PC_MATRIX_SOURCE_CAPTURE generation=1 records=4 dropped=0 invalid=0 write_failures=0 closed=1 boundary={a['BOUNDARY']} path=logs/pc_motion_matrix_sources.log\n")


def viewports(count=3):
    return ''.join(f'FRAME_VIEWPORT_COPY sequence={n} generation=1 function=0x82762790 caller=0x820e3630 '
        f'source=0x2000 owner_r31=0x3000 render_context=0x82a69b00 scope=consumer_copy_not_temporal_history words={encoded([0] * 104)}\n'
        for n in range(count))


def gpu_frame(index=0, changed=False):
    rec = g['record']()
    rec['regs'].update({r: PAIR[(r - 12) * 4:(r - 11) * 4] for r in range(12, 20)})
    if changed:
        rec['regs'][12] = rec['regs'][12][:]
        rec['regs'][12][3] ^= 1
    return a['h']['parse_frame'](g['frame_text'](index, records=[rec]))


class MatrixSourceTests(unittest.TestCase):
    def test_complete_native_call_and_qualified_completion(self):
        capture = a['parse_sources'](source_text())
        self.assertEqual(len(capture['records']), 4)
        self.assertEqual(a['complete_pair'](capture['records'][1]), tuple(PAIR))
        a['verify_completion'](capture, events(), viewports())

    def test_rejects_v214_missing_footer_and_changed_budgets(self):
        good = source_text()
        for bad in [good.rsplit('MATRIX_SOURCE_END', 1)[0], good.replace('version=2', 'version=1'),
                    good.replace('maximum_records=4096', 'maximum_records=9999'),
                    good.replace('records=4 dropped', 'records=3 dropped'), good.replace('copies=2 boundary', 'copies=3 boundary')]:
            with self.subTest(bad=bad[-90:]), self.assertRaises(ValueError): a['parse_sources'](bad)

    def test_rejects_failed_incomplete_or_duplicate_slots(self):
        good = source_text()
        for bad in [good.replace('slot=7', 'slot=6'), good.replace('slot=3 source', 'slot=8 source'),
                    good.replace('valid=1', 'valid=0', 1), good.replace('invalid=0', 'invalid=1'),
                    good.replace('dropped=0', 'dropped=1'), good.replace('write_failures=0', 'write_failures=1')]:
            with self.subTest(bad=bad[-90:]), self.assertRaises(ValueError): a['parse_sources'](bad)

    def test_rejects_source_generation_scope_or_copy_identity_changes(self):
        good = source_text()
        for bad in [good.replace('generation=1', 'generation=2', 1), good.replace('copy_sequence=1', 'copy_sequence=2'),
                    good.replace('copy_sequence=1', 'copy_sequence=0'), good.replace(a['INPUT_SCOPE'], 'history'),
                    good.replace('last_binding_sequence=2', 'last_binding_sequence=0')]:
            with self.assertRaises(ValueError): a['parse_sources'](bad)

    def test_rejects_unreviewed_call_arguments_and_vector_modes(self):
        good = source_text()
        for bad in [good.replace('caller=822490d4', 'caller=822490d0'), good.replace('descriptor=1080', 'descriptor=1084'),
                    good.replace('matrix_array=1060', 'matrix_array=1064'), good.replace('declared_vectors=8', 'declared_vectors=7'),
                    good.replace('modes=01,01', 'modes=04,01'), good.replace('eligible=1', 'eligible=0')]:
            with self.assertRaises(ValueError): a['parse_sources'](bad)

    def test_rejects_stream_alignment_wrap_and_wrong_pool(self):
        good = source_text()
        for bad in [good.replace('source=9000', 'source=9004'), good.replace('source=9000', 'source=ffffffc0'),
                    good.replace('pool_matrix=62a0', 'pool_matrix=62b0'), good.replace('pool_index=1', 'pool_index=ffffffff')]:
            with self.assertRaises(ValueError): a['parse_sources'](bad)

    def test_rejects_vector_count_lane_mask_defaults_and_truncated_payload(self):
        good = source_text()
        for bad in [good.replace(' vectors=8 ', ' vectors=9 '), good.replace('mask=ffff', 'mask=ff7f'),
                    good.replace('default_one=3f800000', 'default_one=00000000'),
                    good.replace(encoded(PAIR), encoded(PAIR[:-1])),
                    good.replace('mask=ffff', 'mask=100000000')]:
            with self.assertRaises(ValueError): a['parse_sources'](bad)

    def test_partial_vectors_are_not_synthesized_into_an_affine(self):
        partial = source_text().replace('mask=ffff', 'mask=ff7f').replace(' vectors=8 ', ' vectors=7 ').replace(encoded(PAIR), encoded(PAIR[:-4]))
        capture = a['parse_sources'](partial)
        result = a['correlate_payloads'](capture, [gpu_frame()])
        self.assertEqual(result['partial_or_nonaffine_observations'], 2)
        self.assertEqual(result['frames'][0]['matched_draws'], 0)

    def test_null_stream_does_not_create_default_history(self):
        empty = source_text().replace('source=9000', 'source=0').replace('source=9100', 'source=0').replace(' vectors=8 ', ' vectors=0 ').replace(encoded(PAIR), '')
        capture = a['parse_sources'](empty)
        self.assertIsNone(a['complete_pair'](capture['records'][1]))

    def test_nonfinite_and_nonaffine_payloads_cannot_match(self):
        record = a['parse_sources'](source_text())['records'][1]
        record['stream']['words'][0] = 0x7F800000
        with self.assertRaises(ValueError): a['complete_pair'](record)
        record['stream']['words'][0] = 0x3F800000
        record['stream']['words'][12] = 0x3F800000
        self.assertIsNone(a['complete_pair'](record))

    def test_footer_needs_one_successful_final_write_event(self):
        capture = a['parse_sources'](source_text())
        for bad in ['', events() + events().splitlines()[1] + '\n', events().replace('write_failures=0', 'write_failures=1'),
                    events().replace('closed=1', 'closed=0'), events().replace('records=4', 'records=3'),
                    '\n'.join(reversed(events().splitlines()))]:
            with self.assertRaises(ValueError): a['verify_completion'](capture, bad, viewports())

    def test_qualified_successor_copy_is_required(self):
        capture = a['parse_sources'](source_text())
        for bad in [viewports(2), viewports(17), viewports().replace('sequence=2', 'sequence=1'),
                    viewports().replace('caller=0x820e3630', 'caller=0x820e3634')]:
            with self.assertRaises(ValueError): a['verify_completion'](capture, events(), bad)

    def test_content_matches_keep_all_candidates_and_never_assign_cpu_frame(self):
        capture = a['parse_sources'](source_text())
        result = a['correlate_payloads'](capture, [gpu_frame(0, changed=True), gpu_frame(1)])
        self.assertEqual(result['unique_complete_pair_payloads'], 1)
        self.assertEqual([f['matched_draws'] for f in result['frames']], [0, 1])
        candidates = result['frames'][1]['correspondences'][0]['candidates']
        self.assertEqual([c['cpu_copy_sequence'] for c in candidates], [0, 1])
        self.assertEqual([c['vector_source'] for c in candidates], ['00009000', '00009100'])
        for key in ('cpu_copy_is_gpu_frame', 'persistent_object_identity_verified',
                    'native_source_to_draw_causal_identity_verified', 'temporal_history_verified', 'previous_skeletal_deformation_verified'):
            self.assertIs(result[key], False)
        mismatch = gpu_frame()
        mismatch['header']['generation'] = 2
        with self.assertRaises(ValueError): a['correlate_payloads'](capture, [mismatch])

    def test_source_identity_rejects_changes_and_path_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'logs').mkdir()
            identity = root / a['IDENTITY_PATH']
            identity.write_text('[]', encoding='utf-8')
            with self.assertRaises(ValueError): a['verify_sources'](root)
            (root / 'source.cpp').write_bytes(b'verified source')
            for relative, expected in [('source.cpp', '0' * 64), ('../escape.cpp', '0' * 64)]:
                raw = json.dumps([dict(Path=relative, SHA256=expected, Bytes=15)]).encode()
                identity.write_bytes(raw)
                with mock.patch.dict(a['verify_sources'].__globals__, IDENTITY_SHA256=hashlib.sha256(raw).hexdigest().upper()):
                    with self.assertRaises(ValueError): a['verify_sources'](root)


if __name__ == '__main__':
    unittest.main()

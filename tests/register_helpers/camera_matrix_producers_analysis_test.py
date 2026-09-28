"""Failure-focused producer audit checks.

The fixture is one real V216 producer record with its footer count normalized
for testing. The authoritative gameplay capture remains the full26-record file.
"""
from copy import deepcopy
from pathlib import Path
import runpy
import unittest

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
a = runpy.run_path(str(ROOT / 'scripts/analyze-camera-matrix-producers.py'))
FIXTURE = Path(__file__).with_name('fixtures') / 'motion_producer_v1_single_record.log'


class ProducerAuditTests(unittest.TestCase):
    def setUp(self):
        self.text = FIXTURE.read_text()
        self.capture = a['parse_producers'](self.text)

    def reject(self, before, after):
        self.assertIn(before, self.text)
        with self.assertRaises(ValueError):
            a['parse_producers'](self.text.replace(before, after, 1))

    def test_real_completed_worker_and_exact_equations(self):
        r = self.capture['records'][0]
        self.assertEqual(r['main_copy_context'], 0)
        self.assertEqual(r['list_records'][0]['address'], r['buffer'] + 192)
        checks = a['verify_equations'](self.capture)['exact_checks']
        self.assertEqual(len(checks), 5)
        self.assertEqual(set(checks.values()), {1})

    def test_missing_footer(self):
        with self.assertRaises(ValueError):
            a['parse_producers']('\n'.join(self.text.splitlines()[:-1]))

    def test_owned_source_identity_is_optional_but_strict(self):
        marker = ' scope=completed_vector_and_record_construction_before_list_publication'
        tagged = self.text.replace(marker, ' owned_source_publication=17' + marker)
        parsed = a['parse_producers'](tagged)
        self.assertEqual(parsed['records'][0]['owned_source_publication'], 17)
        self.assertNotIn('owned_source_publication', self.capture['records'][0])
        for bad in ('0', '-1', str(1 << 64), '17x'):
            with self.assertRaises(ValueError):
                a['parse_producers'](tagged.replace('owned_source_publication=17',
                                                      'owned_source_publication=' + bad))
        lines = tagged.splitlines()
        record = lines[1:-1]
        replay = [line.replace('sequence=0', 'sequence=1') for line in record]
        footer = lines[-1].replace('records=1', 'records=2')
        for second in (replay, [line.replace(' owned_source_publication=17', '') for line in replay]):
            with self.assertRaises(ValueError):
                a['parse_producers']('\n'.join([lines[0], *record, *second, footer]))
        later = [line.replace('owned_source_publication=17', 'owned_source_publication=18') for line in replay]
        self.assertEqual(len(a['parse_producers']('\n'.join([lines[0], *record, *later, footer]))['records']), 2)

    def test_wrong_caller(self):
        self.reject('caller=82276960', 'caller=8227695c')

    def test_wrong_stack_list(self):
        self.reject('list=7ed9fa10', 'list=7ed9fa20')

    def test_excessive_native_count(self):
        self.reject(' count=1 ', ' count=65 ')

    def test_generation_mismatch(self):
        self.reject('MOTION_PRODUCER sequence=0 generation=1', 'MOTION_PRODUCER sequence=0 generation=2')

    def test_unreviewed_vtable_or_layout(self):
        self.reject('vtable=82067764', 'vtable=82067768')
        self.reject('compact=ad155c54', 'compact=ad155c64')

    def test_matrix_role_and_pointer_integrity(self):
        self.reject('role=camera_1824', 'role=camera_1760')
        self.reject('role=camera_1760 source=ad948890', 'role=camera_1760 source=ad9488a0')

    def test_output_record_address_and_mode(self):
        self.reject('address=ad155c80', 'address=ad155c90')
        self.reject('mode=ad155c54', 'mode=ad155c64')

    def test_invalid_drop_or_failure(self):
        self.reject('records=1 dropped=0', 'records=1 dropped=1')
        self.reject('invalid=0 write_failures=0', 'invalid=1 write_failures=0')
        self.reject('write_failures=0 copies=2', 'write_failures=1 copies=2')

    def test_empty_interval_is_structurally_complete(self):
        lines = self.text.splitlines()
        empty = a['parse_producers'](lines[0] + '\n' + lines[-1].replace('records=1', 'records=0'))
        self.assertEqual(empty['records'], [])
        self.assertEqual(a['verify_equations'](empty)['exact_checks'], {})

    def test_event_must_follow_request_and_confirm_final_write(self):
        request = 'REX_EMBEDDED_MANUAL_CAPTURE_REQUEST generation=1 enabled=1\n'
        event = ('PC_MOTION_PRODUCER_CAPTURE generation=1 records=1 dropped=0 invalid=0 write_failures=0 '
                 'closed=1 boundary=next_qualified_viewport_copy path=logs/pc_motion_producers.log\n')
        a['verify_completion'](self.capture, request + event)
        for bad in (event, event + request, request + event + event, request + event.replace('write_failures=0', 'write_failures=1')):
            with self.assertRaises(ValueError):
                a['verify_completion'](self.capture, bad)

    def test_one_changed_output_bit_is_not_tolerated(self):
        self.capture['records'][0]['packed'][19] ^= 1
        with self.assertRaisesRegex(ValueError, 'B_native_branch'):
            a['verify_equations'](self.capture)

    def test_nonfinite_input_is_rejected(self):
        self.capture['records'][0]['matrices']['primary']['words'][0] = 0x7F800000
        with self.assertRaisesRegex(ValueError, 'nonfinite'):
            a['verify_equations'](self.capture)

    def test_rigid_inverse_does_not_invert_scale(self):
        matrix = np.diag([2, 3, 4, 1]).astype(np.float32)
        matrix[3, :3] = [5, 7, 11]
        expected = matrix.copy()
        expected[3, :3] = [-10, -21, -44]
        np.testing.assert_array_equal(a['rigid_inverse'](matrix), expected)
        self.assertFalse(np.array_equal(expected, np.linalg.inv(matrix)))

    def test_packed_multiply_flushes_denormal_inputs(self):
        matrix = np.eye(4, dtype=np.float32)
        matrix[0, 1] = np.finfo(np.float32).smallest_subnormal
        np.testing.assert_array_equal(a['multiply'](matrix, np.eye(4, dtype=np.float32)), np.eye(4, dtype=np.float32))

    def test_unobserved_branches_use_explicit_inputs(self):
        record = self.capture['records'][0]
        identity = a['words'](np.eye(4, dtype=np.float32))
        primary = np.eye(4, dtype=np.float32)
        primary[3, :3] = [2, 3, 4]
        for role in a['ROLES']:
            record['matrices'][role]['words'] = identity.copy()
        record['matrices']['camera_1888']['words'][12:15] = [0x80000000] * 3
        record['matrices']['primary']['words'] = a['words'](primary)
        record['branch'] = 'camera_relative'
        record['packed'] = a['words'](primary.T) * 2
        a['verify_equations'](self.capture)
        record['branch'] = 'identity'
        record['packed'][16:] = identity
        a['verify_equations'](self.capture)

    def test_same_address_with_changed_values_is_not_a_match(self):
        record = self.capture['records'][0]
        consumer = dict(sequence=1, copy_sequence=0, last_binding_record=record['list_records'][0]['address'],
            stream=dict(eligible=True, vectors=8, source=record['buffer'], words=record['packed'].copy()))
        consumer['stream']['words'][3] ^= 1
        report = a['correlate_producers'](self.capture, dict(generation=1, records=[consumer]), [])
        self.assertEqual(report['payload_matched_consumers'], 0)
        self.assertEqual(report['consumer_correspondences'][0]['same_address_observations'][0]['equal_payload'], False)

    def test_duplicate_values_retain_all_candidates(self):
        record = self.capture['records'][0]
        second = deepcopy(record)
        second['sequence'] = 1
        second['buffer'] += 0x1000
        self.capture['records'].append(second)
        consumer = dict(sequence=1, copy_sequence=0, last_binding_record=record['list_records'][0]['address'],
            stream=dict(eligible=True, vectors=8, source=record['buffer'], words=record['packed'].copy()))
        report = a['correlate_producers'](self.capture, dict(generation=1, records=[consumer]), [])
        self.assertEqual(len(report['consumer_correspondences'][0]['candidates']), 2)
        self.assertEqual(report['address_and_record_and_payload_matched_consumers'], 1)
        with self.assertRaises(ValueError):
            a['correlate_producers'](self.capture, dict(generation=2, records=[consumer]), [])


if __name__ == '__main__':
    unittest.main()

"""Synthetic origin records exercise rejection and correspondence boundaries.

The compact-output join uses the existing normalized single-record V216 fixture;
neither that normalization nor synthetic lifecycle data is new gameplay evidence.
"""
from copy import deepcopy
from pathlib import Path
import runpy
import unittest

a = runpy.run_path(str(Path(__file__).resolve().parents[2] / 'scripts/analyze-camera-matrix-origins.py'))
p = a['p']
np = p['np']


def record(site='camera_ready', sequence=0):
    r = dict.fromkeys(a['HEX'], 0)
    r.update(sequence=sequence, generation=1, window_copy_sequence=0, main_copy_context=0,
        producer_sequence=0xFFFFFFFF, active=1, valid=1, site=site, stack=0x4000,
        render_owner=0x10000, camera=0x20000, count=0, capacity=8)
    r['function'], caller = a['SITES'][site]
    r['caller'] = caller or 0
    c, v = np.eye(4, dtype=np.float32), np.eye(4, dtype=np.float32)
    c[3, 0] = 2.0
    v[3, 1] = 0.0625
    matrices = [c, v, p['rigid_inverse'](c), p['multiply'](c, p['rigid_inverse'](p['multiply'](c, v)))]
    r['matrices'] = {role: dict(source=r['camera'] + int(role.split('_')[1]), words=p['words'](m))
        for role, m in zip(a['ROLES'][:4], matrices)}
    r['matrices'].update({role: dict(source=0, words=[]) for role in a['ROLES'][4:]})
    return r


def serialize(records):
    text = ['MOTION_ORIGIN_CAPTURE version=1 generation=1 maximum_records=1024 maximum_copies=4 '
            'scope=native_input_lifecycle_observations_not_temporal_history']
    for r in records:
        text.append('MOTION_ORIGIN ' + ' '.join(f'{k}={r[k]}' for k in a['DEC']) + f" site={r['site']} " +
            ' '.join(f'{k}={r[k]:x}' for k in a['HEX']))
        for role in a['ROLES']:
            m = r['matrices'][role]
            text.append(f"MOTION_ORIGIN_MATRIX sequence={r['sequence']} role={role} source={m['source']:x} "
                f"present={int(bool(m['source']))} valid=1 words=" + ','.join(f'{w:08x}' for w in m['words']))
    text.append(f'MOTION_ORIGIN_END generation=1 records={len(records)} dropped=0 invalid=0 write_failures=0 '
        'copies=4 boundary=next_qualified_viewport_copy')
    return '\n'.join(text) + '\n'


class OriginAuditTests(unittest.TestCase):
    def setUp(self):
        self.record = record()
        self.text = serialize([self.record])

    def reject(self, text):
        with self.assertRaises(ValueError):
            a['parse_origins'](text)

    def test_complete_and_empty_bounds(self):
        self.assertEqual(len(a['parse_origins'](self.text)['records']), 1)
        self.assertFalse(a['parse_origins'](serialize([]))['records'])

    def test_failed_or_partial_completion(self):
        for before, after in [('dropped=0', 'dropped=1'), ('write_failures=0', 'write_failures=1'),
                ('copies=4', 'copies=2'), ('records=1 ', 'records=2 '), ('version=1 ', 'version=2 ')]:
            with self.subTest(after=after): self.reject(self.text.replace(before, after))
        self.reject('\n'.join(self.text.splitlines()[:-1]))

    def test_site_caller_and_role_identity(self):
        self.reject(self.text.replace('caller=825dfc98', 'caller=825dfc94'))
        self.reject(self.text.replace('role=camera_1888', 'role=camera_1760'))
        self.reject(self.text.replace('source=20760', 'source=20770'))

    def test_extra_fields_and_invalid_boolean(self):
        self.reject(self.text.replace('site=camera_ready', 'extra=1 site=camera_ready'))
        self.reject(self.text.replace('active=1', 'active=2'))

    def test_unordered_copies_and_wrong_generation(self):
        r = record(sequence=1)
        r['window_copy_sequence'] = 4
        self.reject(serialize([self.record, r]))
        r['window_copy_sequence'] = 0
        r['generation'] = 2
        self.reject(serialize([self.record, r]))

    def test_slot_bounds(self):
        r = record('register_pair')
        r.update(object=0x30000, array=0x50000, count=1, item=0x500F0)
        self.assertEqual(a['parse_origins'](serialize([r]))['records'][0]['item'], r['item'])
        r['item'] += 16
        self.reject(serialize([r]))

    def test_completion_event_cannot_precede_request(self):
        capture = a['parse_origins'](self.text)
        event = 'PC_MOTION_ORIGIN_CAPTURE generation=1 records=1 dropped=0 invalid=0 write_failures=0 closed=1 '
        event += 'boundary=next_qualified_viewport_copy path=logs/pc_motion_origins.log'
        request = 'REX_EMBEDDED_MANUAL_CAPTURE_REQUEST generation=1 enabled=1'
        a['verify_completion'](capture, request + '\n' + event)
        with self.assertRaises(ValueError): a['verify_completion'](capture, event + '\n' + request)
        with self.assertRaises(ValueError): a['verify_completion'](capture, request + '\n' + event + '\n' + event)

    def lifecycle(self):
        b, r = record('camera_begin'), record(sequence=1)
        b['stack'] = r['stack'] + 304
        b['matrices']['input_current'] = deepcopy(r['matrices']['camera_1760'])
        b['matrices']['input_delta'] = deepcopy(r['matrices']['camera_1824'])
        return b, r

    def test_camera_ready_uses_exact_entry_values(self):
        records = self.lifecycle()
        result = a['camera_lifecycles'](records)
        self.assertTrue(result['updates'][0]['copied_inputs_exact'])
        records[0]['matrices']['input_current']['words'][12] ^= 1
        with self.assertRaises(ValueError): a['camera_lifecycles'](records)

    def test_camera_stack_reuse_and_partial_interval_stay_explicit(self):
        b, r = self.lifecycle()
        old = deepcopy(b)
        old['sequence'] = 0
        b['sequence'], r['sequence'] = 1, 2
        result = a['camera_lifecycles']([old, b, r])
        self.assertEqual(result['updates'][0]['begin_sequence'], 1)
        self.assertEqual(result['unmatched_begin_sequences'], [0])
        self.assertIsNone(a['camera_lifecycles']([r])['updates'][0]['begin_sequence'])

    def test_completed_camera_reset_and_equations(self):
        for mutate in ('count', 'matrix'):
            b, r = self.lifecycle()
            if mutate == 'count': r['count'] = 1
            else: r['matrices']['camera_1888']['words'][12] ^= 1
            with self.subTest(mutate=mutate), self.assertRaises(ValueError): a['camera_lifecycles']([b, r])

    def test_direct_dispatch_needs_stack_caller_and_all_inputs(self):
        d, b = record('render_dispatch'), record('model_builder', sequence=1)
        d.update(item=0x30000, object=0x50000, object_vtable=0x60000, target=b['function'])
        b.update(object=d['object'], object_vtable=d['object_vtable'], stack=d['stack']-144, caller=0x825E2A14)
        for role, source in [('input_current', d['item']), ('input_delta', d['item']+64), ('input_inverse', d['camera']+1888)]:
            d['matrices'][role] = dict(source=source, words=p['words'](np.eye(4, dtype=np.float32)))
        b['matrices'] = deepcopy(d['matrices'])
        empty = dict(records=[])
        result = a['correlate_inputs']([d, b], empty)
        self.assertEqual(result['dispatch_to_builder'][0]['direct_call_candidates'], [0])
        for key in ('caller', 'stack'):
            bad = deepcopy(b)
            bad[key] += 4
            self.assertFalse(a['correlate_inputs']([d, bad], empty)['dispatch_to_builder'][0]['direct_call_candidates'])
        b['matrices']['input_delta']['words'][12] ^= 1
        self.assertFalse(a['correlate_inputs']([d, b], empty)['dispatch_to_builder'][0]['direct_call_candidates'])

    def test_output_link_cannot_omit_duplicate_or_mutate_producer(self):
        fixture = Path(__file__).with_name('fixtures') / 'motion_producer_v1_single_record.log'
        producers = p['parse_producers'](fixture.read_text())
        q = producers['records'][0]
        o = record('compact_output')
        o.update({k:q[k] for k in ('camera', 'render_owner', 'stack', 'generation', 'window_copy_sequence')})
        o['producer_sequence'] = 0
        o['matrices'] = {role:deepcopy(q['matrices'][role]) for role in a['ROLES'][:4]}
        o['matrices'].update({x:deepcopy(q['matrices'][y]) for x,y in
            [('input_current','descriptor_12'),('input_delta','descriptor_20'),('input_inverse','descriptor_16')]})
        self.assertEqual(len(a['correlate_inputs']([o], producers)['builder_to_output']), 1)
        with self.assertRaises(ValueError): a['correlate_inputs']([], producers)
        with self.assertRaises(ValueError): a['correlate_inputs']([o, o], producers)
        o['matrices']['input_delta']['words'][0] ^= 1
        with self.assertRaises(ValueError): a['correlate_inputs']([o], producers)


if __name__ == '__main__':
    unittest.main()

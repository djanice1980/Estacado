"""Synthetic completion records test concurrency and overwrite discrimination."""
from copy import deepcopy
from pathlib import Path
import runpy
import unittest

root = Path(__file__).resolve().parents[2]
a = runpy.run_path(str(root / 'scripts/analyze-completed-motion-registrations.py'))
f = runpy.run_path(str(Path(__file__).with_name('camera_matrix_origins_analysis_test.py')))
o, p = a['o'], a['p']


def record(site, sequence=0):
    r = f['record']('register_single' if site not in o['SITES'] else site, sequence)
    r.update(site=site, source_object=0)
    r['function'], caller = o['SITES_V2'][site]
    r['caller'] = caller or 0
    return r


def serialize(records):
    lines = f['serialize'](records).replace('version=1 ', 'version=2 ').splitlines()
    i = 0
    for n, line in enumerate(lines):
        if line.startswith('MOTION_ORIGIN '):
            lines[n] += f" source_object={records[i]['source_object']:x}"
            i += 1
    return '\n'.join(lines) + '\n'


def lifecycle():
    e, r, d = record('register_single'), record('register_single_ready', 1), record('render_dispatch', 2)
    e.update(object=0x30000, array=0x50000, item=0x50000, count=0, stack=0x40A0)
    r.update(source_object=e['object'], object=0x31000, object_vtable=0x60000,
        array=e['array'], item=e['item'], count=1)
    d.update(object=r['object'], object_vtable=r['object_vtable'], array=r['array'], item=r['item'], count=1)
    words = p['words'](p['np'].eye(4, dtype=p['np'].float32))
    e['matrices']['input_current'] = dict(source=0x70000, words=words[:])
    for x in (r, d):
        x['matrices']['input_current'] = dict(source=x['item'], words=words[:])
        x['matrices']['input_delta'] = dict(source=x['item']+64, words=words[:])
    d['matrices']['input_inverse'] = deepcopy(d['matrices']['camera_1888'])
    return [e, r, d]


class CompletedRegistrationTests(unittest.TestCase):
    def test_explicit_version_and_completed_layout(self):
        text = serialize(lifecycle())
        self.assertEqual(len(o['parse_origins'](text, version=2)['records']), 3)
        with self.assertRaises(ValueError): o['parse_origins'](text)
        with self.assertRaises(ValueError): o['parse_origins'](text, version=3)

    def test_wrong_callback_count_slot_source_rejected(self):
        for key, value in [('caller', 0x825E4614), ('count', 0), ('count', 9), ('item', 0x500F0), ('source_object', 0)]:
            records = lifecycle()
            records[1][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                o['parse_origins'](serialize(records), version=2)

    def test_required_new_field_and_matrix_pointer(self):
        text = serialize(lifecycle())
        with self.assertRaises(ValueError): o['parse_origins'](text.replace(' source_object=30000', ''), version=2)
        with self.assertRaises(ValueError): o['parse_origins'](text.replace('source=50040', 'source=50050'), version=2)

    def test_inactive_completion_has_no_constructed_item(self):
        r = record('register_single_ready')
        r['active'] = 0
        self.assertEqual(o['parse_origins'](serialize([r]), version=2)['records'][0]['active'], 0)
        r['item'] = 0x50000
        with self.assertRaises(ValueError): o['parse_origins'](serialize([r]), version=2)

    def test_exact_assignment_and_dispatch(self):
        x = a['completed_registrations'](lifecycle())
        self.assertTrue(x['assignments'][0]['current_copy_exact'])
        self.assertTrue(x['assignments'][0]['delta_copy_exact'])
        self.assertTrue(x['dispatches'][0]['exact_observed_correspondence'])
        self.assertFalse(x['entry_sequences_without_completion'])

    def test_colliding_prelock_prediction_does_not_select_other_worker(self):
        e, r, d = lifecycle()
        other = deepcopy(e)
        other.update(sequence=1, stack=e['stack']+0x1000, object=0x32000)
        r['sequence'], d['sequence'] = 2, 3
        r['item'] += 240
        r['count'] = 2
        x = a['completed_registrations']([e, other, r, d])
        self.assertEqual(x['assignments'][0]['entry_sequence'], 0)
        self.assertFalse(x['assignments'][0]['predicted_slot_matches'])
        self.assertEqual(x['entry_sequences_without_completion'], [1])

    def test_stack_reuse_selects_latest_and_retains_earlier(self):
        e, r, d = lifecycle()
        latest = deepcopy(e)
        latest['sequence'], r['sequence'], d['sequence'] = 1, 2, 3
        x = a['completed_registrations']([e, latest, r, d])
        self.assertEqual(x['assignments'][0]['entry_sequence'], 1)
        self.assertEqual(x['assignments'][0]['earlier_entry_candidates'], [0, 1])

    def test_changed_completed_delta_remains_explicit(self):
        records = lifecycle()
        records[1]['matrices']['input_delta']['words'][12] = 0x3F000000
        x = a['completed_registrations'](records)
        self.assertFalse(x['assignments'][0]['delta_copy_exact'])
        self.assertFalse(x['dispatches'][0]['delta_unchanged'])
        self.assertFalse(x['dispatches'][0]['exact_observed_correspondence'])

    def test_later_overwrite_cannot_match_older_object(self):
        e, r, d = lifecycle()
        overwrite = deepcopy(r)
        overwrite.update(sequence=2, object=0x32000)
        d['sequence'] = 3
        x = a['completed_registrations']([e, r, overwrite, d])
        self.assertEqual(x['dispatches'][0]['completed_sequence'], 2)
        self.assertFalse(x['dispatches'][0]['same_object'])
        self.assertFalse(x['dispatches'][0]['exact_observed_correspondence'])

    def test_camera_reset_and_missing_entry_are_not_hidden(self):
        e, r, d = lifecycle()
        reset = record('camera_ready', 2)
        d['sequence'] = 3
        x = a['completed_registrations']([r, reset, d])
        self.assertIsNone(x['assignments'][0]['entry_sequence'])
        self.assertEqual(x['dispatches'][0]['intervening_camera_resets'], [2])
        self.assertFalse(x['dispatches'][0]['exact_observed_correspondence'])

    def test_pair_uses_actual_second_input(self):
        e, r, d = lifecycle()
        e['site'], r['site'] = 'register_pair', 'register_pair_ready'
        e['matrices']['input_delta'] = deepcopy(r['matrices']['input_delta'])
        self.assertTrue(a['completed_registrations']([e, r, d])['assignments'][0]['delta_copy_exact'])
        e['matrices']['input_delta']['words'][0] ^= 1
        self.assertFalse(a['completed_registrations']([e, r, d])['assignments'][0]['delta_copy_exact'])

    def test_camera_parent_requires_native_stack_call_and_exact_inputs(self):
        parent, begin = record('camera_parent'), record('camera_begin', 1)
        parent.update(stack=begin['stack']+928, caller=0x82001000)
        begin['caller'] = 0x825E5598
        for role, source in [('input_current', 0x70000), ('input_delta', 0x70040)]:
            parent['matrices'][role] = dict(source=source, words=p['words'](p['np'].eye(4, dtype=p['np'].float32)))
            begin['matrices'][role] = deepcopy(parent['matrices'][role])
        self.assertTrue(a['camera_parents']([parent, begin])[0]['exact_inputs'])
        begin['matrices']['input_delta']['source'] += 16
        with self.assertRaises(ValueError): a['camera_parents']([parent, begin])
        begin['caller'] += 4
        self.assertIsNone(a['camera_parents']([parent, begin])[0]['parent_sequence'])


if __name__ == '__main__':
    unittest.main()

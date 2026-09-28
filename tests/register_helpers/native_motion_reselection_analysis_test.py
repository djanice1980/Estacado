"""Reject stale table/key payloads while preserving bounded reselection evidence."""
from copy import deepcopy
from pathlib import Path
import runpy
import unittest

root = Path(__file__).resolve().parents[2]
a = runpy.run_path(str(root/'scripts/analyze-native-motion-reselection.py'))
f = runpy.run_path(str(Path(__file__).with_name('native_motion_delta_analysis_test.py')))


def fixture():
    store = f['record']('history_store_input')
    selected = f['record']('inverse_input', 1)
    selected.update(window_copy_sequence=1, origin_records_seen=1, slot=1,
        previous_table=store['next_table'], next_table=store['previous_table'])
    selected['matrices']['selected_history']['words'] = deepcopy(store['matrices']['store_current']['words'])
    camera = f['f']['record']('camera_ready')
    camera.update(generation=7, window_copy_sequence=1, render_owner=store['owner'], camera=store['camera'])
    return dict(generation=7, records=[store, selected]), dict(generation=7, records=[camera])


class ReselectionTests(unittest.TestCase):
    def test_exact_adjacent_reselection_is_narrow_claim(self):
        report=a['reselection'](*fixture())
        self.assertEqual(report['exact_bounded_reselections'],1)
        self.assertEqual(report['selections'][0]['intervening_camera_updates'],[0])
        for key in ['history_insertion_completion_observed','uninterrupted_table_lifetime_verified',
                    'persistent_instance_identity_verified','previous_gpu_frame_semantics_verified']:
            self.assertFalse(report[key])

    def test_first_capture_boundary_has_no_fabricated_store(self):
        c,o=fixture();c['records']=c['records'][1:]
        r=a['reselection'](c,o)
        self.assertEqual(r['exact_bounded_reselections'],0)
        self.assertEqual(r['exclusions'],{'no_earlier_table_write_in_capture':1})

    def test_missing_and_far_branch_are_not_native_key_selections(self):
        for caller in [0x824998F4,0x82499A38]:
            c,o=fixture();c['records'][-1]['caller']=caller
            self.assertEqual(a['reselection'](c,o)['matched_key_inputs'],0)

    def test_changed_key_cannot_match_by_address_or_payload(self):
        for key in ['object','key_entity','key_part']:
            c,o=fixture();c['records'][-1][key]+=1
            with self.subTest(key=key):self.assertEqual(a['reselection'](c,o)['exact_bounded_reselections'],0)

    def test_table_and_namespace_reuse_are_excluded(self):
        for key in ['owner','camera','state','entity','previous_table']:
            c,o=fixture();c['records'][-1][key]+=16
            with self.subTest(key=key):self.assertEqual(a['reselection'](c,o)['exact_bounded_reselections'],0)

    def test_nonconsecutive_reused_table_does_not_establish_history(self):
        c,o=fixture();c['records'][-1]['window_copy_sequence']=3
        self.assertIn('nonconsecutive_cpu_copy',a['reselection'](c,o)['exclusions'])

    def test_latest_table_copy_without_key_blocks_stale_match(self):
        c,o=fixture();old=c['records'][0];current=c['records'][-1]
        later=deepcopy(old);later.update(sequence=1,window_copy_sequence=1,object=old['object']+16)
        current.update(sequence=2,window_copy_sequence=2)
        c['records'].insert(1,later)
        r=a['reselection'](c,o)
        self.assertEqual(r['exact_bounded_reselections'],0)
        self.assertEqual(r['selections'][0]['earlier_key_candidates'],[0])
        self.assertEqual(r['selections'][0]['fresh_key_candidates'],[])

    def test_duplicate_key_is_ambiguous_even_with_identical_words(self):
        c,o=fixture();duplicate=deepcopy(c['records'][0]);duplicate['sequence']=1
        c['records'][-1]['sequence']=2;c['records'].insert(1,duplicate)
        self.assertIn('duplicate_key_in_latest_observed_table_copy',a['reselection'](c,o)['exclusions'])

    def test_other_owner_writing_same_table_blocks_matching_key(self):
        c,o=fixture();other=deepcopy(c['records'][0]);other.update(sequence=1,object=other['object']+16,owner=1)
        c['records'][-1]['sequence']=2;c['records'].insert(1,other)
        self.assertIn('table_or_entity_ownership_differs',a['reselection'](c,o)['exclusions'])

    def test_selected_words_must_match_exactly(self):
        c,o=fixture();c['records'][-1]['matrices']['selected_history']['words'][0]^=1
        self.assertIn('selected_payload_differs',a['reselection'](c,o)['exclusions'])

    def test_camera_boundary_must_be_observed_once(self):
        c,o=fixture();o['records'][0]['site']='camera_parent'
        self.assertIn('camera_update_boundary_not_unique',a['reselection'](c,o)['exclusions'])
        c,o=fixture();o['records'].append(deepcopy(o['records'][0]));o['records'][-1]['sequence']=1
        c['records'][-1]['origin_records_seen']=2
        self.assertIn('camera_update_boundary_not_unique',a['reselection'](c,o)['exclusions'])

    def test_generation_and_unobserved_origin_rejected(self):
        c,o=fixture();o['generation']+=1
        with self.assertRaises(ValueError):a['reselection'](c,o)
        c,o=fixture();c['records'][-1]['origin_records_seen']=2
        with self.assertRaises(ValueError):a['reselection'](c,o)

    def test_prior_report_must_pin_the_same_capture(self):
        pin=dict(path=str(root/'logs/synthetic-not-written.log'),bytes=12,sha256='A'*64)
        report=dict(capture_status='AUTOMATED_GAMEPLAY_VALIDATED',temporal_history_verified=False,
            source_pins=[dict(sha256=a['a']['IDENTITY_SHA256'])],inputs=[deepcopy(pin)])
        a['verify_prior_report'](report,[pin])
        for key,value in [('bytes',13),('sha256','B'*64),('path',str(root/'logs/different.log'))]:
            changed=deepcopy(pin);changed[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):a['verify_prior_report'](report,[changed])


if __name__=='__main__':unittest.main()

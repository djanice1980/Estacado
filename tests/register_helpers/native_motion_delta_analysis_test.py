"""Reject false native motion-source correspondences and malformed captures."""
from copy import deepcopy
from pathlib import Path
import runpy
import unittest

root = Path(__file__).resolve().parents[2]
a = runpy.run_path(str(root/'scripts/analyze-native-motion-deltas.py'))
f = runpy.run_path(str(Path(__file__).with_name('completed_motion_registrations_analysis_test.py')))


def translation(x):
    result = a['p']['np'].eye(4, dtype=a['p']['np'].float32)
    result[3, 0] = x
    return a['p']['words'](result)


def record(site, sequence=0, caller=None):
    r = {k:0 for k in a['DEC']+a['HEX']}
    fn, callers = a['SITES'][site]
    r.update(site=site, sequence=sequence, generation=7, valid=1, lookup_complete=1,
        function=fn, caller=caller or (callers[0] if callers else 0), owner=0x20000, stack=0x30000)
    r['matrices'] = {role:dict(source=0, words=[]) for role in a['ROLES']}
    def matrix(role, source, x=0): r['matrices'][role] = dict(source=source, words=translation(x))
    if site == 'camera_source':
        matrix('source_c',0x100000)
        matrix('source_v',0x100080)
        return r
    r.update(camera=0x40000, state=0x50000, array=0x60000, count=1, capacity=4,
        previous_table=0x50000+2224+44, next_table=0x50000+2224)
    matrix('camera_c',r['camera']+1760)
    matrix('camera_v',r['camera']+1824)
    if site == 'view_callback': return r
    r.update(item=r['array'], object=0x70000, key_entity=1, key_part=2, entity=0x80000,
        entity_table=0x90000, entity_count=8, history_array=0xA0000, history_count=2,
        history_capacity=4, buckets=0xB0000)
    matrix('item_current',r['item'],10)
    matrix('item_delta',r['item']+64)
    matrix('entity_current',r['entity']+80,5)
    matrix('entity_other',r['entity']+144,2)
    if site == 'inverse_input':
        matrix('inverse_input',r['stack']+448 if r['caller']==0x82499664 else r['entity']+144,2)
        if r['caller']==0x82499664:
            r.update(selected_history=r['history_array'],lookup_steps=1)
            matrix('selected_history',r['selected_history'],2)
    else:
        r.update(selected_history=r['history_array'],lookup_steps=1)
        matrix('selected_history',r['selected_history'],2)
        matrix('inverse_work',r['stack']+208,-2)
        # The native inverse negates the zero Y/Z translation components.
        r['matrices']['inverse_work']['words'][13:15] = [0x80000000, 0x80000000]
        matrix('working_current',r['stack']+128,10)
        matrix('store_current',r['stack']+272,10)
        matrix('item_delta',r['item']+64,8)
    return r


def serialize(records):
    lines=['MOTION_DELTA_CAPTURE version=1 generation=7 maximum_records=1024 maximum_copies=4 scope=native_delta_selection_not_previous_frame_acceptance']
    for r in records:
        lines.append('MOTION_DELTA '+' '.join(f'{k}={r[k]}' for k in a['DEC'])+' '+
            ' '.join(f'{k}={r[k]:x}' for k in a['HEX'])+f" site={r['site']}")
        for role in a['ROLES']:
            m=r['matrices'][role]
            lines.append(f"MOTION_DELTA_MATRIX sequence={r['sequence']} role={role} source={m['source']:x} present={int(bool(m['source']))} valid=1 words="+
                ','.join(f'{v:08x}' for v in m['words']))
    lines.append(f'MOTION_DELTA_END generation=7 records={len(records)} dropped=0 invalid=0 write_failures=0 copies=4 boundary=next_qualified_viewport_copy')
    return '\n'.join(lines)+'\n'


def origins(delta):
    d=f['record']('render_dispatch',0)
    d.update(generation=7,render_owner=delta['owner'],camera=delta['camera'],item=delta['item'],object=delta['object'])
    d['matrices']['input_current']=deepcopy(delta['matrices']['item_current'])
    d['matrices']['input_delta']=deepcopy(delta['matrices']['item_delta'])
    return dict(generation=7,records=[d])


class NativeDeltaTests(unittest.TestCase):
    def test_all_sites_and_native_pointer_layouts(self):
        rs=[record('camera_source'),record('view_callback',1),record('inverse_input',2),record('history_store_input',3)]
        self.assertEqual(len(a['parse_delta'](serialize(rs))['records']),4)

    def test_reject_wrong_version_footer_or_missing_role(self):
        text=serialize([record('inverse_input')])
        for old,new in [('version=1','version=2'),('invalid=0','invalid=1'),('copies=4 boundary','copies=3 boundary'),
                        ('role=entity_other','role=entity_current'),('lookup_complete=1','lookup_complete=0')]:
            with self.subTest(old=old),self.assertRaises(ValueError): a['parse_delta'](text.replace(old,new))

    def test_caller_key_slot_and_table_bounds(self):
        for key,value in [('caller',0),('key_entity',8),('item',0x600F0),('history_count',5),
                          ('selected_history',0xA00C0),('flags',0x40000),('slot',2),('previous_table',1)]:
            r=record('inverse_input');r[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):a['parse_delta'](serialize([r]))

    def test_branch_lookup_and_operand_must_agree(self):
        r=record('inverse_input',caller=0x824998F4)
        a['parse_delta'](serialize([r]))
        r['selected_history']=r['history_array']
        r['lookup_steps']=1
        r['matrices']['selected_history']=dict(source=r['selected_history'],words=translation(2))
        with self.assertRaises(ValueError):a['parse_delta'](serialize([r]))
        r=record('inverse_input');r['matrices']['inverse_input']['source']+=16
        with self.assertRaises(ValueError):a['parse_delta'](serialize([r]))

    def test_record_order_and_completion_event(self):
        rs=[record('inverse_input'),record('history_store_input',1)]
        rs[0]['origin_records_seen']=1
        with self.assertRaises(ValueError):a['parse_delta'](serialize(rs))
        c=a['parse_delta'](serialize([record('inverse_input')]))
        event='PC_MOTION_DELTA_CAPTURE generation=7 records=1 dropped=0 invalid=0 write_failures=0 closed=1 boundary=next_qualified_viewport_copy path=logs/pc_motion_delta.log'
        a['verify_completion'](c,event)
        for text in (event+'\n'+event,event.replace('records=1','records=2'),event.replace('closed=1','closed=0')):
            with self.assertRaises(ValueError):a['verify_completion'](c,text)

    def test_exact_matched_product_and_dispatch(self):
        rs=[record('inverse_input'),record('history_store_input',1)]
        report=a['analyze_selection'](a['parse_delta'](serialize(rs)),origins(rs[-1]))
        self.assertTrue(report['stores'][0]['exact_native_product'])
        self.assertTrue(report['dispatches'][0]['exact_observed_correspondence'])
        self.assertFalse(report['stores'][0]['insertion_completed_verified'])
        self.assertFalse(report['previous_frame_semantics_verified'])

    def test_fallback_uses_entity_matrix_not_render_item(self):
        i=record('inverse_input',caller=0x824998F4);t=record('history_store_input',1)
        t.update(selected_history=0,lookup_steps=0)
        t['matrices']['selected_history']=dict(source=0,words=[])
        t['matrices']['item_delta']['words']=translation(3)
        report=a['analyze_selection'](a['parse_delta'](serialize([i,t])),origins(t))
        self.assertTrue(report['stores'][0]['exact_native_product'])

    def test_distance_fallback_has_no_history_insertion(self):
        i=record('inverse_input',caller=0x82499A38)
        d=origins(i);d['records'][0]['matrices']['input_delta']['words']=translation(3)
        report=a['analyze_selection'](a['parse_delta'](serialize([i])),d)
        self.assertEqual(report['stores'],[])
        self.assertTrue(report['dispatches'][0]['exact_observed_correspondence'])

    def test_mismatched_inverse_or_product_rejected(self):
        for role in ('inverse_work','item_delta','working_current'):
            rs=[record('inverse_input'),record('history_store_input',1)]
            rs[1]['matrices'][role]['words']=translation(15)
            with self.subTest(role=role),self.assertRaises(ValueError):
                a['analyze_selection'](a['parse_delta'](serialize(rs)),origins(rs[-1]))

    def test_missing_or_reused_inverse_cannot_match_older_value(self):
        i=record('inverse_input');t=record('history_store_input',2)
        with self.assertRaises(ValueError):a['analyze_selection'](dict(generation=7,records=[t]),origins(t))
        later=deepcopy(i);later['sequence']=1;later['matrices']['inverse_input']['words']=translation(20)
        with self.assertRaises(ValueError):a['analyze_selection'](dict(generation=7,records=[i,later,t]),origins(t))

    def test_reset_and_changed_object_remove_dispatch_acceptance(self):
        rs=[record('inverse_input'),record('history_store_input',1)];c=a['parse_delta'](serialize(rs))
        o=origins(rs[-1]);o['records'][0]['object']+=16
        self.assertFalse(a['analyze_selection'](c,o)['dispatches'][0]['exact_observed_correspondence'])
        o=origins(rs[-1]);reset=deepcopy(o['records'][0]);reset['site']='camera_ready'
        o['records'][0]['sequence']=1;o['records'].insert(0,reset)
        self.assertFalse(a['analyze_selection'](c,o)['dispatches'][0]['exact_observed_correspondence'])

    def test_camera_source_requires_native_stack_and_exact_pointer(self):
        r=record('camera_source');parent=f['record']('camera_parent')
        parent.update(generation=7,render_owner=r['owner'],stack=r['stack']-672)
        parent['matrices']['input_current']=deepcopy(r['matrices']['source_c'])
        parent['matrices']['input_delta']=deepcopy(r['matrices']['source_v'])
        c=dict(generation=7,records=[r]);o=dict(generation=7,records=[parent])
        self.assertTrue(a['analyze_selection'](c,o)['camera_sources'][0]['exact_inputs'])
        parent['matrices']['input_delta']['source']+=16
        with self.assertRaises(ValueError):a['analyze_selection'](c,o)
        parent['stack']-=16
        self.assertFalse(a['analyze_selection'](c,o)['camera_sources'][0]['exact_inputs'])

    def test_latest_different_object_cannot_match_older_delta(self):
        first=record('inverse_input',caller=0x82499A38)
        later=deepcopy(first);later.update(sequence=1,object=first['object']+16)
        d=origins(first);d['records'][0]['matrices']['input_delta']['words']=translation(3)
        report=a['analyze_selection'](dict(generation=7,records=[first,later]),d)['dispatches'][0]
        self.assertEqual(report['delta_sequence'],1)
        self.assertEqual(report['earlier_delta_candidates'],[0,1])
        self.assertFalse(report['same_object'])
        self.assertFalse(report['exact_observed_correspondence'])

    def test_intervening_registration_invalidates_identical_item_payload(self):
        i=record('inverse_input',caller=0x82499A38);o=origins(i)
        o['records'][0]['matrices']['input_delta']['words']=translation(3)
        ready=deepcopy(o['records'][0]);ready['site']='register_single_ready'
        o['records'][0]['sequence']=1;o['records'].insert(0,ready)
        report=a['analyze_selection'](dict(generation=7,records=[i]),o)['dispatches'][0]
        self.assertEqual(report['intervening_item_registrations'],[0])
        self.assertFalse(report['exact_observed_correspondence'])

    def test_latest_inverse_for_another_item_invalidates_store(self):
        i=record('inverse_input');t=record('history_store_input',2)
        later=deepcopy(i);later.update(sequence=1,item=i['item']+240)
        with self.assertRaises(ValueError):a['analyze_selection'](dict(generation=7,records=[i,later,t]),origins(t))


if __name__=='__main__':unittest.main()

"""Exercise native camera rotation decisions, outputs and malformed evidence."""
from copy import deepcopy
from pathlib import Path
import runpy
import struct
import unittest

root=Path(__file__).resolve().parents[2]
a=runpy.run_path(str(root/'scripts/analyze-temporal-camera-sources.py'))
f=runpy.run_path(str(Path(__file__).with_name('native_motion_delta_analysis_test.py')))


def bits(value):return struct.unpack('>Q',struct.pack('>d',value))[0]


def record(site):
    r={k:0 for k in a['DEC']+a['HEX']+a['TIMES']}
    fn,callers=a['SITES'][site]
    r.update(site=site,function=fn,caller=callers[0] if callers else 0,generation=7,valid=1,context_id=1,
        scene=0x60000,stack=0x50000,slot=0,enabled=1,duration=0x3E800000,factor=0x3F000000,
        saved_source_time=bits(1),saved_scene_time=bits(1),scene_time=bits(2))
    r['tables']=[dict(address=r['scene']+2672+i*44,buckets=0x100000+i*512,array=0x200000+i*4096,
        count=3-i,capacity=4,nonempty_buckets=3-i) for i in range(2)]
    r['matrices']={role:dict(source=0,words=[]) for role in a['ROLES']}
    def matrix(role,source,value):r['matrices'][role]=dict(source=source,words=f['translation'](value))
    matrix('cached_current',r['scene']+2592,1)
    matrix('motion_delta',r['scene']+2528,0)
    if site in ('begin','inverse','clear','end','forward'):
        matrix('current_input',0x70000,2);r['output_pointer']=0x80000
    if site in ('inverse','clear','end'):
        r.update(frame=0x50000-416,stack=0x50000 if site=='end' else 0x50000-416,
            sample_time_present=1,sample_time=bits(2))
    if site=='inverse':matrix('inverse_input',r['frame']+112,1.5)
    if site in ('clear','end','forward'):
        r.update(slot=1,factor=0x3E800000,saved_source_time=bits(2),saved_scene_time=bits(2))
        matrix('cached_current',r['scene']+2592,2)
        matrix('motion_delta',r['scene']+2528,0.5)
    if site=='clear':
        matrix('inverse_work',r['frame']+176,-1.5)
        r['matrices']['inverse_work']['words'][13:15]=[0x80000000,0x80000000]
    if site in ('end','forward'):
        matrix('output_delta',r['output_pointer'],0.5)
        r['tables'][1].update(count=0,nonempty_buckets=0)
    if site in ('forward','callback'):r.update(owner=0xB0000,camera=0xA0000)
    if site=='forward':r['stack']-=176
    if site=='callback':
        matrix('current_input',r['camera']+1760,2)
        matrix('output_delta',r['camera']+1824,0.5)
    return r


def cycle(rotate=True,reason='flag'):
    records=[record(site) for site in ('begin','inverse','clear','end','forward')]
    if not rotate:
        records=[records[0],records[3],records[4]]
        begin,end,forward=records
        for r in records:
            r.update(slot=0,factor=begin['factor'],saved_source_time=bits(1),saved_scene_time=bits(1))
            r['matrices']['cached_current']=deepcopy(begin['matrices']['cached_current'])
            r['matrices']['motion_delta']=deepcopy(begin['matrices']['motion_delta'])
            r['tables']=deepcopy(begin['tables'])
        end['enabled']=0
        for r in (end,forward):r['matrices']['output_delta']['words']=f['translation'](0)
        if reason=='flag':
            for r in records:r['flags']=0x20
        elif reason=='source_time':end['sample_time']=bits(1)
        elif reason=='scene_time':
            for r in records:r['scene_time']=bits(1)
    for i,r in enumerate(records):r['sequence']=i
    return dict(generation=7,records=records)


def serialize(capture):
    records=capture['records']
    lines=['CAMERA_HISTORY_SOURCE_CAPTURE version=1 generation=7 maximum_records=1024 maximum_copies=4 scope=native_camera_rotation_not_gpu_history_acceptance']
    for r in records:
        lines.append('CAMERA_HISTORY_SOURCE '+' '.join(f'{k}={r[k]}' for k in a['DEC'])+' '+
            ' '.join(f'{k}={r[k]:x}' for k in a['HEX']+a['TIMES'])+f" site={r['site']}")
        for slot,t in enumerate(r['tables']):
            lines.append(f"CAMERA_HISTORY_TABLE sequence={r['sequence']} slot={slot} valid=1 "+
                ' '.join(f'{k}={t[k]:x}' for k in ('address','buckets','array'))+' '+
                ' '.join(f'{k}={t[k]}' for k in ('count','capacity','nonempty_buckets')))
        for role in a['ROLES']:
            m=r['matrices'][role]
            lines.append(f"CAMERA_HISTORY_MATRIX sequence={r['sequence']} role={role} source={m['source']:x} present={int(bool(m['source']))} valid=1 words="+
                ','.join(f'{v:08x}' for v in m['words']))
    lines.append(f'CAMERA_HISTORY_SOURCE_END generation=7 records={len(records)} dropped=0 invalid=0 write_failures=0 copies=4 boundary=next_qualified_viewport_copy')
    return '\n'.join(lines)+'\n'


def analyze(capture):return a['analyze_camera'](capture,dict(generation=7,records=[]),dict(generation=7,records=[]))


class CameraSourceTests(unittest.TestCase):
    def test_all_sites_and_roles_parse(self):
        c=cycle()
        for site in ('reset','callback'):
            r=record(site);r['sequence']=len(c['records']);c['records'].append(r)
        r=record('clear');r.update(sequence=len(c['records']),caller=0x8249ADD0,frame=0,stack=0x50000,
            sample_time=0,sample_time_present=0,output_pointer=0)
        for role in ('current_input','inverse_work'):r['matrices'][role]=dict(source=0,words=[])
        c['records'].append(r)
        self.assertEqual(len(a['parse_capture'](serialize(c))['records']),8)

    def test_exact_rotation_and_forwarded_output(self):
        r=analyze(a['parse_capture'](serialize(cycle())))
        self.assertTrue(r['completed_updates'][0]['exact_update'])
        self.assertTrue(r['completed_updates'][0]['rotated'])
        self.assertTrue(r['forwarded_inputs'][0]['exact_returned_inputs'])
        self.assertFalse(r['motion_delta_is_previous_gpu_camera_verified'])

    def test_version_count_footer_and_duplicate_roles(self):
        text=serialize(cycle())
        for old,new in [('version=1','version=2'),('records=5','records=4'),('invalid=0','invalid=1'),
                        ('copies=4 boundary','copies=3 boundary'),('role=inverse_work','role=inverse_input')]:
            with self.subTest(old=old),self.assertRaises(ValueError):a['parse_capture'](text.replace(old,new))

    def test_wrong_native_call_layout_and_presence(self):
        for key,value in [('caller',0),('frame',0),('stack',1),('sample_time_present',0),('output_pointer',0),('context_id',17)]:
            c=cycle();c['records'][1][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):a['parse_capture'](serialize(c))

    def test_table_bounds_and_cleared_head_count(self):
        for key,value in [('count',5),('capacity',32768),('address',1),('buckets',0),('array',1),('nonempty_buckets',4)]:
            c=cycle();c['records'][0]['tables'][0][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):a['parse_capture'](serialize(c))
        c=cycle();c['records'][3]['tables'][1].update(count=1,nonempty_buckets=1)
        with self.assertRaises(ValueError):analyze(c)

    def test_nonfinite_time_and_scalar_are_rejected(self):
        for key,value in [('sample_time',0x7FF0000000000000),('duration',0x7FC00000),('factor',0x7F800000)]:
            c=cycle();c['records'][1][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):a['parse_capture'](serialize(c))

    def test_wrong_inverse_product_and_returned_payload(self):
        for index,role in [(2,'inverse_work'),(2,'motion_delta'),(3,'output_delta'),(3,'cached_current')]:
            c=cycle();c['records'][index]['matrices'][role]['words'][0]^=1
            with self.subTest(role=role),self.assertRaises(ValueError):analyze(c)

    def test_three_native_skip_conditions_preserve_cache(self):
        for reason in ('flag','source_time','scene_time'):
            r=analyze(a['parse_capture'](serialize(cycle(False,reason))))
            with self.subTest(reason=reason):
                self.assertTrue(r['completed_updates'][0]['exact_update'])
                self.assertFalse(r['completed_updates'][0]['rotated'])

    def test_skipped_update_must_not_mutate_history(self):
        c=cycle(False);c['records'][1]['matrices']['cached_current']['words'][0]^=1
        with self.assertRaises(ValueError):analyze(c)

    def test_changed_clock_factor_slot_and_flag_are_rejected(self):
        for key,value in [('saved_source_time',bits(3)),('factor',0x3F000000),('slot',0),('enabled',0),('flags',0x20),('scene_time',bits(3))]:
            c=cycle();c['records'][3][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):analyze(c)

    def test_missing_inverse_or_clear_is_not_accepted(self):
        for index in (1,2):
            c=cycle();c['records'].pop(index)
            with self.subTest(index=index),self.assertRaises(ValueError):analyze(c)

    def test_latest_same_stack_begin_for_different_scene_rejects_stale_match(self):
        c=cycle();later=deepcopy(c['records'][0]);later.update(sequence=0.5,scene=0xD0000)
        c['records'].insert(1,later)
        with self.assertRaises(ValueError):analyze(c)

    def test_forward_pointer_reuse_does_not_accept_different_payload(self):
        c=cycle();c['records'][-1]['matrices']['output_delta']['words'][0]^=1
        self.assertFalse(analyze(c)['forwarded_inputs'][0]['exact_returned_inputs'])

    def test_unobserved_begin_and_context_do_not_fabricate_completed_update(self):
        for mutation in ('remove','context'):
            c=cycle()
            if mutation=='remove':c['records'].pop(0)
            else:c['records'][0]['context_id']=2
            r=analyze(c)
            self.assertFalse(r['completed_updates'][0]['exact_update'])
            self.assertFalse(r['forwarded_inputs'][0]['exact_returned_inputs'])

    def test_completion_event_requires_one_exact_closed_record(self):
        c=a['parse_capture'](serialize(cycle()))
        event='PC_CAMERA_HISTORY_SOURCE_CAPTURE generation=7 records=5 dropped=0 invalid=0 write_failures=0 closed=1 boundary=next_qualified_viewport_copy path=logs/pc_camera_history_source.log'
        a['verify_completion'](c,event)
        for text in (event+'\n'+event,event.replace('closed=1','closed=0'),event.replace('records=5','records=4')):
            with self.assertRaises(ValueError):a['verify_completion'](c,text)


if __name__=='__main__':unittest.main()

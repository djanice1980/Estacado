"""Synthetic failure-focused interval and correspondence tests, not game evidence."""
from copy import deepcopy
from pathlib import Path
import re
import runpy
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
a = runpy.run_path(str(ROOT / 'scripts/analyze-camera-matrix-lineage.py'))
s, h = a['s'], a['h']
old = runpy.run_path(str(Path(__file__).with_name('camera_matrix_sources_analysis_test.py')))
gpu = old['g']


def source_text():
    original = old['source_text']().splitlines()
    lines = [original[0].replace('version=2', 'version=3').replace('copies=2', 'copies=4')]
    for i in range(4):
        for line in original[1:21]:
            line = re.sub(r'(?<=\s)(sequence|last_binding_sequence)=(\d+)',
                lambda m: f'{m[1]}={int(m[2]) + i * 2}', line)
            line = line.replace('copy_sequence=0', f'copy_sequence={i}')
            if line.startswith('MATRIX_SOURCE_INPUT '):
                line += ' copy_allocator=3000'
            lines.append(line)
    lines.append(original[-1].replace('records=4', 'records=8').replace('copies=2', 'copies=4'))
    return '\n'.join(lines) + '\n'


def frame_text(index):
    return gpu['frame_text'](index).replace('version=1', 'version=2').replace('frames=2', 'frames=6')


def events_text():
    lines = gpu['events_text']().splitlines()[:2]
    for i in range(6):
        lines.append(f'REX_CAMERA_HISTORY_FRAME generation=1 frame={10+i} index={i} selected=2 dropped=0 failures=0 '
            f'aborted=0 written=1 path=rex_camera_history_generation_1_frame_{10+i}.log scope={h["COMPLETION_SCOPE"]}')
    return '\n'.join(lines) + '\n'


def correspondence_inputs():
    fixture = Path(__file__).with_name('fixtures') / 'motion_producer_v1_single_record.log'
    producers = a['p']['parse_producers'](fixture.read_text())
    producer = producers['records'][0]
    consumers = s['parse_sources'](source_text(), version=3)
    consumer = deepcopy(consumers['records'][5])
    consumer.update(copy_allocator=producer['allocator'], last_binding_record=producer['list_records'][0]['address'])
    consumer['stream'].update(source=producer['buffer'], words=producer['packed'][:])
    consumers['records'] = [consumer]
    frame = dict(header=dict(generation=1, index=3, frame=13), draws=[dict(
        meta=dict(draw=2, vs='FA91501A8940251D'),
        vertex_constants={i:producer['packed'][(i-12)*4:(i-11)*4] for i in range(12,20)})])
    return producers, consumers, [frame]


class MatrixLineageTests(unittest.TestCase):
    def test_six_frames_require_explicit_parser_contract(self):
        for i in range(6):
            self.assertEqual(h['parse_frame'](frame_text(i), expected_frames=6)['header']['index'], i)
            with self.assertRaises(ValueError): h['parse_frame'](frame_text(i))
        with self.assertRaises(ValueError): h['parse_frame'](gpu['frame_text'](), expected_frames=6)

    def test_full_gpu_window_checks_first_frame_and_shaders(self):
        with mock.patch.dict(h, verify_shader=mock.Mock()) as patched:
            frames, report = a['verify_history_window']([frame_text(i) for i in range(6)],
                gpu['primary_text'](), events_text(), ROOT)
        self.assertEqual(len(frames), 6)
        self.assertTrue(report['first_frame_legacy_capture_exactly_matches'])

    def test_missing_mixed_reordered_and_duplicate_frames_rejected(self):
        frames = [h['parse_frame'](frame_text(i), expected_frames=6) for i in range(6)]
        bad = [frames[:5], frames[::-1], frames[:5]+[frames[4]], []]
        mixed = deepcopy(frames); mixed[4]['header']['generation'] = 2; bad.append(mixed)
        reversed_fence = deepcopy(frames); reversed_fence[4]['header']['completed'] = 1; bad.append(reversed_fence)
        for interval in bad:
            with self.assertRaises(ValueError): h['verify_events'](interval, events_text())

    def test_missing_or_failed_late_write_event_rejected(self):
        frames = [h['parse_frame'](frame_text(i), expected_frames=6) for i in range(6)]
        text = events_text()
        for bad in ['\n'.join(text.splitlines()[:-1]), text.replace('index=5 selected=2', 'index=4 selected=2'),
                    text.replace('written=1', 'written=0'), text + text.splitlines()[-1]+'\n',
                    text+'REX_CAMERA_HISTORY_ABORT reason=lost_boundary\n']:
            with self.assertRaises(ValueError): h['verify_events'](frames, bad)

    def test_six_frame_budget_and_header_limits(self):
        for old_value,new in [('frames=6','frames=7'),('version=2','version=1'),('index=5','index=6'),
                              ('frame=15','frame=16'),('selected=2','selected=129')]:
            with self.assertRaises(ValueError): h['parse_frame'](frame_text(5).replace(old_value,new), expected_frames=6)

    def test_four_consumers_and_canonical_allocator_inventory(self):
        capture = s['parse_sources'](source_text(), version=3)
        self.assertEqual({r['copy_sequence'] for r in capture['records']}, {0,1,2,3})
        s['verify_completion'](capture, old['events']().replace('records=4','records=8'), old['viewports'](5))
        with self.assertRaises(ValueError): s['parse_sources'](source_text())

    def test_source_missing_interval_bad_allocator_or_wrong_budget(self):
        text = source_text()
        for bad in [text.replace('maximum_copies=4','maximum_copies=2'), text.replace('copies=4 boundary','copies=3 boundary'),
                    text.replace('copy_allocator=3000','copy_allocator=3004'),
                    text.replace('copy_allocator=3000','copy_allocator=4000',1), text.replace('copy_sequence=3','copy_sequence=2')]:
            with self.assertRaises(ValueError): s['parse_sources'](bad, version=3)

    def test_consumer_allocator_must_match_observed_viewport_owner(self):
        capture=s['parse_sources'](source_text(), version=3)
        events=old['events']().replace('records=4','records=8')
        for bad in [old['viewports'](4), old['viewports'](5).replace('owner_r31=0x3000','owner_r31=0x4000')]:
            with self.assertRaises(ValueError): s['verify_completion'](capture,events,bad)

    def test_exact_delayed_cpu_correspondence_and_gpu_payload(self):
        result=a['correlate_lineage'](*correspondence_inputs())
        self.assertEqual(result['consumed_producer_sequences'],[0])
        self.assertEqual(result['gpu_frames'][0]['draws_with_complete_cpu_correspondence'],1)
        self.assertFalse(result['temporal_history_verified'])
        self.assertFalse(result['uninterrupted_allocation_lifetime_verified'])

    def test_equal_payload_alone_does_not_supply_arena_record_or_order(self):
        for defect in ('arena','record','stream','order'):
            producers,consumers,frames=correspondence_inputs(); c=consumers['records'][0]
            if defect=='arena': c['copy_allocator']+=16
            if defect=='record': c['last_binding_record']+=48
            if defect=='stream': c['stream']['source']+=16
            if defect=='order': c['copy_sequence']=0
            result=a['correlate_lineage'](producers,consumers,frames)
            self.assertEqual(result['producer_payload_matched_consumers'],1)
            self.assertEqual(result['consumers_with_complete_correspondence'],0)
            self.assertEqual(result['gpu_frames'][0]['draws_with_complete_cpu_correspondence'],0)

    def test_reused_address_with_different_payload_is_explicit(self):
        producers,consumers,frames=correspondence_inputs()
        consumers['records'][0]['stream']['words'][0]^=1
        result=a['correlate_lineage'](producers,consumers,frames)
        self.assertEqual(result['producer_payload_matched_consumers'],0)
        self.assertEqual(len(result['same_address_different_payload_observations']),1)
        self.assertEqual(result['gpu_frames'][0]['draws_with_complete_cpu_correspondence'],0)

    def test_all_duplicate_candidates_survive(self):
        producers,consumers,frames=correspondence_inputs()
        duplicate=deepcopy(producers['records'][0]); duplicate['sequence']=1; producers['records'].append(duplicate)
        result=a['correlate_lineage'](producers,consumers,frames)
        self.assertEqual(result['consumed_producer_sequences'],[0,1])
        self.assertEqual(len(result['gpu_frames'][0]['draws'][0]['chain_candidates']),2)

    def test_partial_pair_and_wrong_generation_cannot_join(self):
        producers,consumers,frames=correspondence_inputs()
        consumers['records'][0]['stream']['vectors']=7
        self.assertEqual(a['correlate_lineage'](producers,consumers,frames)['consumer_pairs'],0)
        consumers['generation']=2
        with self.assertRaises(ValueError): a['correlate_lineage'](producers,consumers,frames)

    def test_gpu_one_bit_difference_cannot_join(self):
        producers,consumers,frames=correspondence_inputs()
        frames[0]['draws'][0]['vertex_constants'][12][0]^=1
        result=a['correlate_lineage'](producers,consumers,frames)
        self.assertEqual(result['gpu_frames'][0]['producer_payload_matched_draws'],0)
        self.assertEqual(result['gpu_frames'][0]['draws_with_complete_cpu_correspondence'],0)


if __name__ == '__main__':
    unittest.main()

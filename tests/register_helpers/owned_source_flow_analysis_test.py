from pathlib import Path
import runpy
import unittest

api = runpy.run_path(str(Path(__file__).resolve().parents[2] / 'scripts/analyze-owned-source-flow.py'))


def fixture(bits=16, stored=0, **changes):
    row = dict(cpu_viewport_ticks=16, candidates=3, changed_arena_cursor=3,
               cursor_unknown=0, cursor_last_ticket=42, cursor_last_reasons=bits,
               cursor_last_previous=128, cursor_last_stored=stored,
               cursor_last_limit=1024, cursor_last_address=4096,
               cursor_last_write_address=4096, cursor_last_write_bytes=4)
    row.update({'cursor_' + name: 3 if bits & (1 << bit) else 0
                for bit, name in enumerate(api['REASONS'])})
    row.update(changes)
    return 'PC_OWNED_FLOW ' + ' '.join(f'{key}={value}' for key, value in row.items()) + ' scope=interval_cpu_stage_census_not_rendered_frame'


class SourceFlowAnalysisTests(unittest.TestCase):
    def classification(self, text):
        return api['analyze'](text)['intervals'][0]['last_known_retirement']['classification']

    def test_exact_writes(self):
        for bits, value, expected in [(16, 0, 'exact_typed_zero_reset'),
                                      (16, 64, 'exact_typed_backward_write'),
                                      (8, 128, 'exact_typed_equal_write'),
                                      (32, 2048, 'exact_typed_beyond_capacity_write')]:
            with self.subTest(bits=bits, value=value):
                self.assertEqual(self.classification(fixture(bits, value)), expected)

    def test_partial_and_nested_are_not_reset_proof(self):
        for text in [fixture(18, cursor_last_write_bytes=2), fixture(20),
                     fixture(3, cursor_last_write_bytes=2)]:
            self.assertEqual(self.classification(text), 'untyped_partial_or_overlapping_write')

    def test_unknown_is_not_a_write(self):
        changed = {'cursor_last_' + name: 0 for name in api['LAST']}
        self.assertEqual(self.classification(fixture(0, cursor_unknown=3, **changed)), 'unknown')
        fault = fixture(64, cursor_last_previous=0, cursor_last_limit=0,
                        cursor_last_address=0, cursor_last_write_address=0,
                        cursor_last_write_bytes=0)
        self.assertEqual(self.classification(fault), 'tracking_fault_not_write_evidence')

    def test_overlapping_counts_do_not_imply_unique_events(self):
        result = api['analyze'](fixture(20))['intervals'][0]['last_known_retirement']
        self.assertEqual(sum(result['candidate_reason_counts'].values()), 6)
        self.assertEqual(result['unknown_candidates'], 0)

    def test_missing_and_old_evidence(self):
        self.assertEqual(api['analyze']('')['status'], 'incomplete')
        old = 'PC_OWNED_FLOW cpu_viewport_ticks=16 candidates=3 changed_arena_cursor=3 scope=interval_cpu_stage_census_not_rendered_frame'
        self.assertEqual(self.classification(old), 'unavailable_in_this_build')
        with self.assertRaises(ValueError):
            api['analyze'](old + ' cursor_backward=3')

    def test_completion_requires_exact_three_cpu_intervals(self):
        full = '\n'.join(fixture(cpu_viewport_ticks=tick) for tick in (16, 64, 256))
        self.assertEqual(api['analyze'](full)['status'], 'complete')
        for ticks in [(64,), (16, 16), (16, 256), (16, 64, 256, 256)]:
            with self.subTest(ticks=ticks), self.assertRaises(ValueError):
                api['analyze']('\n'.join(fixture(cpu_viewport_ticks=tick) for tick in ticks))

    def test_reject_inconsistent_or_corrupt_evidence(self):
        for text in [fixture(cursor_unknown=4), fixture(cursor_backward=4),
                     fixture(cursor_backward=0), fixture(cursor_last_stored=128),
                     fixture(cursor_last_write_bytes=2), fixture(cursor_last_ticket=0),
                     fixture(cursor_last_reasons=128), fixture(cursor_last_limit=64),
                     fixture() + ' candidates=3', fixture(candidates=-1), fixture(candidates=2),
                     fixture(1, stored=1), fixture(0, stored=256)]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                api['analyze'](text)


if __name__ == '__main__':
    unittest.main()

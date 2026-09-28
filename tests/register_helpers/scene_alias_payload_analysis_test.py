"""Independent conversion oracle and failure cases for actual alias evidence."""
import copy
from pathlib import Path
import runpy
import tempfile
import unittest

import numpy as np

a = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-scene-alias-payload.py'))


def fixture():
    rows, updates = [], []
    color, alias = '0000000000001234', '0000000000005678'
    sections = [dict(transfer_links=[
        dict(draw=2, start=768, end=784, source=color, source_key='00C88300', dest=alias, dest_key='00708000'),
        dict(draw=3, start=768, end=784, source=alias, source_key='00708000', dest=color, dest_key='00C88300')])]
    queues = []
    def emit(tag, record):
        rows.append((tag, record))
    for u in range(1, 4):
        common = dict(frame='10', update=str(u), next_draw=str(u))
        first = len(rows)
        emit('REX_SCENE_UPDATE_TARGET', dict(common))
        plans = []
        if u > 1:
            stage = 'before_alias' if u == 2 else 'after_restore'
            link = sections[0]['transfer_links'][u - 2]
            plan = dict(link, slot='0' if u == 2 else '1')
            plan.pop('draw')
            plans = [plan]
            queue = dict(common, pair='1', stage=stage, color=color, rect='0,0,1280,8',
                host_width='1280', host_height='1024', format='10', samples='2', bytes='163840', submission='3',
                path=f'rex_scene_alias_frame_10_pair_1_{stage}.bin')
            event = dict(common, pair='1', stage=stage, color=color, alias=alias, color_key='00C88300',
                alias_key='00708000', start='768', end='784', out_update='2', out_draw='2', queued='1',
                scope='sample_copy_at_transfer_boundary_not_payload_validity')
            if u == 2:
                emit('REX_SCENE_ALIAS_QUEUED', queue)
                emit('REX_SCENE_ALIAS_EVENT', event)
            emit('REX_SCENE_UPDATE_COMMAND', dict(common))
            if u == 3:
                emit('REX_SCENE_ALIAS_QUEUED', queue)
                emit('REX_SCENE_ALIAS_EVENT', event)
            queues.append(queue)
        emit('REX_SCENE_ALIAS_UPDATE', dict(common, pairs=str(int(u > 1)), attempts=str(max(0, u - 1)),
            queued=str(max(0, u - 1)), failures='0', dropped='0', pending=str(int(u == 2)),
            reserved_bytes='327680' if u > 1 else '0', scope='bounded_capture_not_payload_validity'))
        end_line = len(rows)
        emit('REX_SCENE_UPDATE_END', common)
        updates.append(dict(first_line=first, end_line=end_line, plans=plans, end={'submission': '3'}))
    for q in queues:
        c = {k: q[k] for k in ('frame', 'update', 'next_draw', 'pair', 'stage', 'bytes', 'submission', 'path')}
        c.update(written=q['bytes'], completed='4', result='1', hash='00000000')
        emit('REX_SCENE_ALIAS_COMPLETE', c)
    return rows, updates, sections


def metadata(rows, updates, sections):
    raw = '\n'.join(tag + ' ' + ' '.join(f'{k}={v}' for k, v in record.items()) for tag, record in rows)
    return a['metadata'](raw, 10, updates, sections)


class AliasPayloadTests(unittest.TestCase):
    def test_exhaustive_finite_half_rgb_against_nearest_representable_oracle(self):
        # Independent numeric table and nearest-neighbour selection, not a
        # duplicate of the implementation's integer exponent/rounding steps.
        codes = np.arange(1024)
        table = np.where(codes < 128, codes / 512.,
            (1. + (codes % 128) / 128.) * 2. ** (codes // 128 - 3)).astype(np.float32)
        self.assertTrue(np.array_equal(a['unpack_7e3'](codes), table))
        values = np.arange(65536, dtype='<u2').view('<f2').astype(np.float32)
        values = values[np.isfinite(values)]
        clipped = np.clip(values, 0., 31.875)
        hi = np.minimum(np.searchsorted(table, clipped), 1023)
        lo = np.maximum(hi - 1, 0)
        low_distance, high_distance = clipped - table[lo], table[hi] - clipped
        expected = np.where((low_distance < high_distance) | ((low_distance == high_distance) & (lo % 2 == 0)), lo, hi)
        self.assertTrue(np.array_equal(a['pack_7e3'](values), expected))

    def test_rgb_halfway_rounding_and_alpha_boundaries(self):
        rgb = np.array([1.00390625, 1.01171875, -.25, 40.], dtype=np.float32)
        self.assertEqual(a['unpack_7e3'](a['pack_7e3'](rgb)).tolist(), [1., 1.015625, 0., 31.875])
        values = np.array([[1., .125, 31.875, x] for x in [-1., 0., .5, 1., 2.]], dtype='<f2').view('<u2')
        actual = a['roundtrip'](values).view('<f2')
        self.assertEqual(actual[:, 3].tolist(), np.array([0, 0, 2/3, 1, 1], dtype='<f2').tolist())

    def test_expected_quantization_is_separate_from_raw_host_equality(self):
        before = np.array([[[[1.00390625, .125, 31.875, .5], [1.01171875, .25, 1, 1]]]], dtype='<f2').view('<u2')
        after = a['roundtrip'](before)
        report = a['compare'](before, after)
        self.assertTrue(report['guest_precision_roundtrip_verified'])
        self.assertFalse(report['host_fp16_bit_exact'])
        self.assertEqual(report['unexpected_components'], 0)
        self.assertGreater(report['expected_quantized_components'], 0)

    def test_sample_swap_one_bit_error_and_wrong_channel_are_detected(self):
        before = np.array([[[[.25, .5, 1, 1], [.5, 1, 2, 1]]]], dtype='<f2').view('<u2')
        correct = a['roundtrip'](before)
        variants = [correct[:, :, ::-1, :].copy(), correct[:, :, :, [1, 0, 2, 3]].copy()]
        wrong = correct.copy(); wrong[0, 0, 1, 0] ^= 1; variants.append(wrong)
        for after in variants:
            result = a['compare'](before, after)
            self.assertFalse(result['guest_precision_roundtrip_verified'])
            self.assertGreater(result['unexpected_components'], 0)
            self.assertTrue(result['first_unexpected'])

    def test_nonfinite_or_incompatible_arrays_fail(self):
        valid = np.zeros((1, 1, 2, 4), dtype='<u2')
        for raw in (0x7c00, 0xfc00, 0x7e00):
            wrong = valid.copy(); wrong[0, 0, 0, 0] = raw
            with self.assertRaises(ValueError): a['compare'](wrong, valid)
            with self.assertRaises(ValueError): a['compare'](valid, wrong)
        with self.assertRaises(ValueError): a['compare'](valid, valid[:, :, :1])

    def test_actual_metadata_pair_and_complete_budgets_pass(self):
        rows, updates, sections = fixture()
        pairs = metadata(rows, updates, sections)
        self.assertEqual(len(pairs), 1)
        self.assertEqual(pairs[0][0]['rectangle'], [0, 0, 1280, 8])
        self.assertEqual(pairs[0][1]['record']['next_draw'], '3')

    def test_missing_duplicate_stale_or_failed_copy_records_fail(self):
        rows, updates, sections = fixture()
        for tag, key, value in [('EVENT', 'queued', '0'), ('EVENT', 'color', '0000000000009999'),
                ('EVENT', 'color_key', '00D08300'), ('EVENT', 'out_update', '1'),
                ('EVENT', 'end', '800'), ('EVENT', 'stage', 'after_restore'),
                ('QUEUED', 'frame', '11'), ('QUEUED', 'pair', '2'),
                ('QUEUED', 'samples', '1'), ('QUEUED', 'rect', '0,1,1280,8'),
                ('QUEUED', 'path', '../unrelated.bin'), ('COMPLETE', 'result', '0'),
                ('COMPLETE', 'written', '1'), ('COMPLETE', 'submission', '2'),
                ('COMPLETE', 'completed', '2')]:
            bad = copy.deepcopy(rows)
            record = next(r for t, r in bad if t == 'REX_SCENE_ALIAS_' + tag)
            record[key] = value
            with self.subTest(tag=tag, key=key), self.assertRaises(ValueError): metadata(bad, updates, sections)
        bad = copy.deepcopy(rows)
        bad.remove(next(row for row in bad if row[0] == 'REX_SCENE_ALIAS_QUEUED'))
        with self.assertRaises(ValueError): metadata(bad, updates, sections)
        bad = rows + [next(row for row in rows if row[0] == 'REX_SCENE_ALIAS_EVENT')]
        with self.assertRaises(ValueError): metadata(bad, updates, sections)

    def test_budget_loss_pending_pairs_and_future_fences_fail(self):
        rows, updates, sections = fixture()
        for key, value in [('dropped', '1'), ('failures', '1'), ('pending', '1'),
                ('reserved_bytes', '327679'), ('queued', '1'), ('pairs', '8')]:
            bad = copy.deepcopy(rows)
            record = [r for t, r in bad if t == 'REX_SCENE_ALIAS_UPDATE'][-1]
            record[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): metadata(bad, updates, sections)
        bad_updates = copy.deepcopy(updates); bad_updates[1]['end']['submission'] = '2'
        with self.assertRaises(ValueError): metadata(rows, bad_updates, sections)

    def test_copies_on_wrong_side_of_transfer_commands_fail(self):
        rows, updates, sections = fixture()
        for update in ('2', '3'):
            bad = copy.deepcopy(rows)
            command = next(i for i, (t, r) in enumerate(bad) if t == 'REX_SCENE_UPDATE_COMMAND' and r['update'] == update)
            event = next(i for i, (t, r) in enumerate(bad) if t == 'REX_SCENE_ALIAS_EVENT' and r['update'] == update)
            bad[command], bad[event] = bad[event], bad[command]
            with self.assertRaises(ValueError): metadata(bad, updates, sections)

    def test_partial_transfer_chain_or_wrong_return_resource_fails(self):
        rows, updates, sections = fixture()
        bad = copy.deepcopy(sections); bad[0]['transfer_links'].pop()
        with self.assertRaises(ValueError): metadata(rows, updates, bad)
        bad = copy.deepcopy(sections); bad[0]['transfer_links'][1]['dest'] = '0000000000009999'
        with self.assertRaises(ValueError): metadata(rows, updates, bad)
        bad_updates = copy.deepcopy(updates); bad_updates[2]['plans'][0]['end'] = 800
        with self.assertRaises(ValueError): metadata(rows, bad_updates, sections)

    def test_completed_copy_hash_and_file_length_are_mandatory(self):
        rows, updates, sections = fixture()
        item = metadata(rows, updates, sections)[0][0]
        data = bytes(item['bytes'])
        item['completion']['hash'] = f"{a['scene']['fnv1a32'](data):08X}"
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); path = root / item['path']; path.write_bytes(data)
            values, record = a['read_samples'](item, root)
            self.assertEqual(values.shape, (8, 1280, 2, 4))
            self.assertEqual(record['bytes'], len(data))
            changed = bytearray(data); changed[3] = 1; path.write_bytes(changed)
            with self.assertRaises(ValueError): a['read_samples'](item, root)
            path.write_bytes(data[:-1])
            with self.assertRaises(ValueError): a['read_samples'](item, root)

    def test_unreviewed_source_cannot_certify_expected_conversion(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); first = root / next(iter(a['SOURCE_PINS']))
            first.parent.mkdir(parents=True); first.write_text('changed conversion')
            with self.assertRaises(ValueError): a['verify_sources'](root)


if __name__ == '__main__':
    unittest.main()

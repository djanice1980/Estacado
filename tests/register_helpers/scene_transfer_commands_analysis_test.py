"""Failure-focused checks of actual transfer, stencil and ownership evidence."""
import copy
from pathlib import Path
import runpy
import unittest

a = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-scene-transfer-commands.py'))


def fixture():
    common = dict(frame='10', update='1', next_draw='1')
    target = dict(common, slot='0', resource='0000000000005678', key='00708000',
                  host_width='640', host_height='1024', host_format='19', host_samples='4', scale='1x1')
    plan = dict(common, slot='0', start='768', end='1536', source='0000000000001234', source_key='00C88300',
                dest=target['resource'], dest_key=target['key'], host_depth=target['resource'],
                host_depth_key=target['key'], scope='planned_transfer_not_execution')
    commands = []
    for mode, key, count in ((4, '00080C46', 8), (6, '000C4C46', 1)):
        for index in range(count):
            c = {k: v for k, v in plan.items() if k != 'host_depth_key'}
            c.update(mode=str(mode), shader_key=key, passes=str(count), **{'pass': str(index)},
                     vertices='6', submission='3', host_depth=target['resource'] if mode == 6 else '0000000000000000',
                     scope='queued_transfer_draw_not_gpu_completion')
            commands.append(c)
    store = dict(common, slot='0', resource=target['resource'], key=target['key'], start='768', end='1536',
                 rect='0,384,640,384', groups='20,96', submission='3',
                 scope='queued_host_depth_store_not_gpu_completion')
    owner = dict(common, owner='0', start='0', end='2048', resource=target['resource'], key=target['key'],
                 host_depth_unorm='00000000', host_depth_float='00708000',
                 scope='post_update_bookkeeping_not_transfer_completion')
    return target, plan, commands, store, owner


def raw_fixture():
    target, plan, commands, store, owner = fixture()
    rows = [('TARGET', target), ('PLAN', plan), ('OWNER', owner), ('DEPTH_STORE', store)]
    rows += [('COMMAND', c) for c in commands]
    end = dict(frame='10', update='1', next_draw='1', succeeded='1', helper_completed='1', targets='1', owners='1',
               planned='1', commands='9', depth_stores='1', failures='0', records='13', dropped='0', submission='3',
               scope='ownership_and_queued_transfers_not_gpu_completion')
    rows.append(('END', end))
    runtime = '\n'.join('REX_SCENE_UPDATE_' + tag + ' ' + ' '.join(f'{k}={v}' for k, v in f.items()) for tag, f in rows)
    gpu = ('CAMERA_SCENE_UPDATES frame=10 updates=1 dropped_updates=0 records=13 dropped_records=0 '
           'scope=ownership_and_queued_transfers_not_pixel_validity')
    return runtime, gpu


def chain_fixture():
    target, plan, _, _, owner = fixture()
    restore = dict(plan, slot='1', source=target['resource'], source_key=target['key'],
                   dest=plan['source'], dest_key=plan['source_key'], host_depth='0000000000000000', host_depth_key='00000000')
    color = dict(resource=plan['source'], key=plan['source_key'])
    depth = dict(resource='0000000000008888', key='00688000')
    after = [dict(owner, end='768', **depth), dict(owner, owner='1', start='768', end='1536', **color),
             dict(owner, owner='2', start='1536')]
    updates = [dict(next_draw=1, end={'submission': '3'}, plans=[plan], owners=[owner], targets={0: target}),
               dict(next_draw=2, end={'submission': '3'}, plans=[restore], owners=after, targets={0: depth, 1: color})]
    draws = [dict(ordinal=1, surface='0A020280', depth='00010000', depth_control='00008701', scale='1x1', clip='00010000'),
             dict(ordinal=2, surface='14010500', depth='00010000', depth_control='1C700271', scale='1x1', clip='00080000')]
    targets = {1: dict(draw='1', mask='00000000', bound_bits='1', color='00000000,00000000,00000000,00000000'),
               2: dict(draw='2', mask='0000000F', bound_bits='3', color='000C0300,00000000,00000000,00000000')}
    rasters = {1: dict(draw='1', host_scissor='0,0,640,8192'), 2: dict(draw='2', host_scissor='0,0,1280,384')}
    cleared = {'scene_clear_epochs': [dict(motion_resolve=1, color_resolve=3, last_draw=0, rectangle=[0, 0, 1280, 384], **color)]}
    verified = dict(timeline=[], copies=[dict(resolve=3, last_draw=2, submission=3, completed=4)])
    return updates, draws, targets, rasters, cleared, verified


class TransferCommandTests(unittest.TestCase):
    def test_actual_full_transfer_and_host_store_pass_without_colour_payload_claim(self):
        target, plan, commands, store, owner = fixture()
        self.assertEqual(a['verify_plan_commands'](plan, commands, [store], {0: target}), 1)
        runtime, gpu = raw_fixture()
        updates, records = a['parse_updates'](runtime, gpu, 10, [dict(ordinal=1)])
        self.assertEqual((len(updates), records), (1, 13))
        self.assertEqual(a['owner_map']([owner])[768], (0x5678, 0x708000))

    def test_plan_only_missing_stencil_bit_duplicate_pass_and_wrong_shader_fail(self):
        target, plan, commands, store, _ = fixture()
        variants = [[], commands[1:], commands[:-1], commands + [commands[0]], list(reversed(commands))]
        bad = copy.deepcopy(commands)
        bad[-1]['shader_key'] = '000C4C45'
        variants.append(bad)
        for cs in variants:
            with self.subTest(commands=len(cs)), self.assertRaises(ValueError):
                a['verify_plan_commands'](plan, cs, [store], {0: target})

    def test_in_place_host_depth_requires_complete_prior_store(self):
        target, plan, commands, store, _ = fixture()
        for stores in ([], [dict(store, rect='0,384,640,336')], [store, store], [dict(store, groups='0,96')]):
            with self.assertRaises(ValueError):
                a['verify_plan_commands'](plan, commands, stores, {0: target})
        runtime, gpu = raw_fixture()
        rows = runtime.splitlines()
        moved = '\n'.join(rows[:3] + rows[4:5] + rows[3:4] + rows[5:])
        with self.assertRaises(ValueError):
            a['parse_updates'](moved, gpu, 10, [dict(ordinal=1)])

    def test_mismatched_range_resource_and_bound_destination_fail(self):
        target, plan, commands, store, _ = fixture()
        for key, value in [('start', '767'), ('source', '0000000000003456'),
                           ('dest_key', '00708001'), ('host_depth', '0000000000003456')]:
            bad = copy.deepcopy(commands)
            bad[-1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                a['verify_plan_commands'](plan, bad, [store], {0: target})
        with self.assertRaises(ValueError):
            a['verify_plan_commands'](plan, commands, [store], {})

    def test_drops_failed_helper_counts_wrong_frame_and_duplicate_records_fail(self):
        runtime, gpu = raw_fixture()
        for bad in (runtime.replace('helper_completed=1', 'helper_completed=0'),
                    runtime.replace('failures=0', 'failures=1'), runtime.replace('dropped=0', 'dropped=1'),
                    runtime.replace('frame=10', 'frame=11'), runtime.replace('records=13', 'records=12'),
                    runtime + '\n' + runtime.splitlines()[-1], runtime + '\nREX_SCENE_UPDATE_UNKNOWN x=1'):
            with self.assertRaises(ValueError):
                a['parse_updates'](bad, gpu, 10, [dict(ordinal=1)])
        with self.assertRaises(ValueError):
            a['parse_updates'](runtime, gpu.replace('dropped_records=0', 'dropped_records=1'), 10, [dict(ordinal=1)])

    def test_ownership_map_gaps_overlap_and_false_empty_owner_fail(self):
        owner = fixture()[-1]
        for owners in ([dict(owner, start='1')], [dict(owner, end='2047')], [owner, owner],
                       [dict(owner, key='00000000')]):
            with self.assertRaises(ValueError):
                a['owner_map'](owners)

    def test_wrong_gpu_hash_cannot_borrow_a_verified_scene_export(self):
        with self.assertRaises(ValueError):
            a['analyze'](b'changed GPU capture', b'', {'inputs': {'gpu_log': {'sha256': '0' * 64}}})

    def test_alias_and_return_chain_is_traceable_without_preservation_claim(self):
        section = a['join_sections'](*chain_fixture())[0]
        self.assertEqual(section['alias_draws'], [1])
        self.assertEqual(len(section['transfer_links']), 2)
        self.assertTrue(section['actual_scene_transfer_chain_verified'])
        self.assertFalse(section['uninterrupted_color_ownership_verified'])
        self.assertFalse(section['color_payload_preservation_verified'])

    def test_unexplained_owner_change_wrong_source_and_partial_return_fail(self):
        for mode in ('missing_alias', 'wrong_source', 'partial_return', 'wrong_binding'):
            state = copy.deepcopy(chain_fixture())
            if mode == 'missing_alias':
                state[0][0]['plans'] = []
            elif mode == 'wrong_source':
                state[0][1]['plans'][0]['source'] = '0000000000007777'
            elif mode == 'partial_return':
                state[0][1]['plans'][0]['end'] = '1500'
            else:
                del state[0][1]['targets'][0]
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                a['join_sections'](*state)

    def test_future_completion_and_intervening_clear_cannot_certify_scene(self):
        state = chain_fixture()
        state[0][1]['end']['submission'] = '5'
        with self.assertRaises(ValueError):
            a['join_sections'](*state)
        state = chain_fixture()
        state[-1]['timeline'] = [dict(resolve='2', control='00100140', last_draw='1')]
        with self.assertRaises(ValueError):
            a['join_sections'](*state)


if __name__ == '__main__':
    unittest.main()

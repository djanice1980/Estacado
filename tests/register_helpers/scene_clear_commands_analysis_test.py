"""Reject requested-only, stale or incomplete host-clear evidence."""
import copy
import hashlib
from pathlib import Path
import runpy
import unittest

analysis = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-scene-clear-commands.py'))


def line(tag, fields):
    return tag + ' ' + ' '.join(f'{k}={v}' for k, v in fields.items())


def command(ordinal=1, draw=251, height=384):
    return dict(frame='10', resolve=str(ordinal), last_draw=str(draw), slot='1', method='rtv_clear',
        resource='0000000000001234', key='00C88300', depth='0', format='3', scale='1x1',
        host_width='1280', host_height='1024', host_format='10', host_samples='2',
        guest_rect=f'0,0,1280,{height}', host_rect=f'0,0,1280,{height}', value='0000000000000000',
        float_count='4', float_words='00000000,00000000,00000000,00000000', submission='77',
        scope='queued_host_clear_not_gpu_completion')


def fixture():
    timeline, copies, lines = [], [], []
    for ordinal, draw, height in ((1, 251, 384), (3, 590, 336)):
        event = dict(frame='10', resolve=str(ordinal), last_draw=str(draw),
                     control='00100140', submission='77')
        later = dict(frame='10', resolve=str(ordinal + 1), last_draw=str(draw + 208),
                     control='00100040', submission='77')
        timeline.extend([event, later])
        prep = dict(frame='10', resolve=str(ordinal), last_draw=str(draw), depth_requested='0',
                    color_requested='1', prepared='1', submission='77',
                    scope='actual_preparation_not_clear_execution')
        end = dict(frame='10', resolve=str(ordinal), last_draw=str(draw), commands='1',
                   target_bits='2', submission='77', scope='queued_host_clear_not_gpu_completion')
        lines.extend([line('REX_SCENE_CLEAR_PREPARE', prep), line('REX_SCENE_CLEAR_COMMAND', command(ordinal, draw, height)),
                      line('REX_SCENE_CLEAR_END', end), line('REX_SCENE_RESOLVE', event), line('REX_SCENE_RESOLVE', later)])
        copies.extend([dict(kind=1, resolve=ordinal, submission=77, completed=78),
            dict(kind=2, resolve=ordinal + 1, submission=77, completed=79,
                 layout=dict(rect=[0, 0, 1280, height], owners=[dict(resource='1234', key='00C88300')]))])
    raw = '\n'.join(lines).encode()
    verification = dict(frame=10, request_generation=1, timeline=timeline, copies=copies,
        bounded_scene_export_verified=True, inputs={'runtime_log': {'sha256': hashlib.sha256(raw).hexdigest()}})
    return raw, verification


class SceneClearCommandTests(unittest.TestCase):
    def test_prepared_commands_join_completed_copies_without_pixel_claim(self):
        raw, verification = fixture()
        report = analysis['analyze'](raw, verification)
        self.assertTrue(report['scene_clear_commands_verified'])
        self.assertFalse(report['scene_color_pixel_ownership_verified'])
        self.assertFalse(report['temporal_history_verified'])
        self.assertEqual(len(report['scene_clear_epochs']), 2)
        self.assertEqual(report['scene_clear_epochs'][1]['rectangle'], [0, 0, 1280, 336])

    def test_command_payload_rejects_wrong_method_slot_format_rectangle_and_nonfinite(self):
        for key, value in [('method', 'request_only'), ('slot', '0'), ('float_count', '0'),
                           ('resource', '0'), ('scale', '2x2'), ('depth', '1'),
                           ('host_rect', '0,0,1280,385'), ('host_width', '640'),
                           ('guest_rect', '-1,0,1280,384'), ('submission', '0'),
                           ('float_words', '7FC00000,00000000,00000000,00000000')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                analysis['command_record'](dict(command(), **{key: value}))

    def test_request_only_cannot_be_promoted_to_clear_execution(self):
        raw, verification = fixture()
        rows = raw.decode().splitlines()
        failed = '\n'.join([rows[0].replace('prepared=1', 'prepared=0')] + rows[3:])
        groups = analysis['parse'](failed, verification['timeline'], 10)
        self.assertFalse(groups[0]['prepared'])
        with self.assertRaises(ValueError):
            analysis['join_scene_clears'](groups, verification)
        with self.assertRaises(ValueError):
            analysis['parse'](raw.decode().replace('prepared=1', 'prepared=0'), verification['timeline'], 10)

    def test_duplicate_missing_and_reordered_metadata_cannot_certify_clear(self):
        raw, verification = fixture()
        rows = raw.decode().splitlines()
        variants = ['\n'.join(rows[:2] + [rows[1]] + rows[2:]),
                    '\n'.join(rows[:2] + rows[3:]),
                    '\n'.join([rows[1], rows[0]] + rows[2:]),
                    raw.decode().replace('commands=1', 'commands=2'),
                    raw.decode().replace('target_bits=2', 'target_bits=1'),
                    raw.decode().replace('frame=10', 'frame=11'),
                    raw.decode() + '\nREX_SCENE_CLEAR_TRUNCATED frame=10 resolve=1']
        for text in variants:
            with self.subTest(text=text[:70]), self.assertRaises(ValueError):
                analysis['parse'](text, verification['timeline'], 10)

    def test_completed_copy_must_cover_same_actual_resource_and_clear_epoch(self):
        raw, verification = fixture()
        groups = analysis['parse'](raw.decode(), verification['timeline'], 10)
        for update in ('fence', 'resource', 'region', 'key'):
            bad = copy.deepcopy(verification)
            if update == 'fence':
                bad['copies'][0]['completed'] = 76
            elif update == 'resource':
                bad['copies'][1]['layout']['owners'][0]['resource'] = '5678'
            elif update == 'region':
                bad['copies'][1]['layout']['rect'][3] = 336
            else:
                bad['copies'][1]['layout']['owners'][0]['key'] = '00C88301'
            with self.subTest(update=update), self.assertRaises(ValueError):
                analysis['join_scene_clears'](groups, bad)

    def test_cached_report_cannot_accept_a_different_runtime_capture(self):
        raw, verification = fixture()
        with self.assertRaises(ValueError):
            analysis['analyze'](raw + b'\n', verification)
        verification['bounded_scene_export_verified'] = False
        with self.assertRaises(ValueError):
            analysis['analyze'](raw, verification)


if __name__ == '__main__':
    unittest.main()

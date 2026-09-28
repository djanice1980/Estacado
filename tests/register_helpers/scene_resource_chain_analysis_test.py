"""Do not promote target-key identity through an unresolved depth-only alias."""
import copy
from pathlib import Path
import runpy
import tempfile
import unittest

a = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-scene-resource-chain.py'))


def draw_fixture():
    d = dict(ordinal=252, surface='14010500', depth='00010000', depth_control='1C700271',
             scale='1x1', clip='00080000')
    t = dict(draw='252', mask='0000000F', bound_bits='3',
             color='000C0300,00000000,00000000,00000000')
    r = dict(draw='252', host_scissor='0,0,1280,384')
    return d, t, r


def audit(d, t, r):
    return a['audit_draw'](d, t, r, [0, 0, 1280, 384], '00C88300')


class ResourceChainTests(unittest.TestCase):
    def test_storage_alias_keys_match_observed_owner_not_guest_format_word(self):
        one = a['target_key'](0x14010500, 0x30300)
        two = a['target_key'](0x14010500, 0xC0300)
        self.assertEqual(one['key'], '00C88300')
        self.assertEqual(two['key'], one['key'])
        self.assertEqual(a['target_key'](0x0A020280, 0x10000, True)['key'], '00708000')
        for surface, info in [(0, 0x30300), (0x14030500, 0x30300), (0x14010500, 0x70300)]:
            with self.assertRaises(ValueError):
                a['target_key'](surface, info)

    def test_visible_writer_and_bounded_depth_do_not_overlap(self):
        d, t, r = draw_fixture()
        out = audit(d, t, r)
        self.assertEqual(out['depth_ownership_upper_bound'], [[0, 768]])
        self.assertFalse(out['depth_bound_intersects_scene_color'])
        self.assertEqual(out['color_key'], '00C88300')
        t.update(mask='00000000', bound_bits='1')
        self.assertFalse(audit(d, t, r)['depth_bound_intersects_scene_color'])

    def test_stencil_only_disabled_color_still_blocks_ownership_proof(self):
        d, t, r = draw_fixture()
        d.update(surface='0A020280', depth_control='00008701', clip='00010000')
        t.update(mask='00000000', bound_bits='1', color='00000000,00000000,00000000,00000000')
        r['host_scissor'] = '0,0,640,8192'
        out = audit(d, t, r)
        self.assertIsNone(out['color_key'])
        self.assertFalse(out['depth_write_enabled'])
        self.assertTrue(out['stencil_enabled'])
        self.assertFalse(out['clipped'])
        self.assertEqual(out['depth_ownership_upper_bound'], [[0, 2048]])
        self.assertTrue(out['depth_bound_intersects_scene_color'])

    def test_unbound_other_target_wrong_partner_and_region_fail_closed(self):
        for part, key, value in [(0, 'scale', '2x2'), (1, 'bound_bits', '1'),
                                  (1, 'mask', '000000FF'), (2, 'draw', '253'),
                                  (2, 'host_scissor', '0,0,1280,385'),
                                  (2, 'host_scissor', '5,0,4,384'),
                                  (1, 'color', '000C0301,00000000,00000000,00000000')]:
            state = copy.deepcopy(draw_fixture())
            state[part][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                audit(*state)

    def test_wrapped_ownership_cannot_be_dropped_from_overlap_check(self):
        target = a['target_key'](0x14010500, 0x107D0, True)
        ranges = a['ownership_bound'](target, 384)
        self.assertEqual(ranges, [[2000, 2048], [0, 720]])
        self.assertTrue(a['overlaps'](ranges, [700, 768]))
        self.assertFalse(a['overlaps'](ranges, [720, 768]))

    def test_native_sized_draws_do_not_substitute_for_actual_global_scale(self):
        text = ('REX_PC_SETTINGS_EFFECTIVE resolution_scale=1 draw_resolution_scale_threshold=0 native_grid_rules=\n'
                'REX_EMBEDDED_RENDER_TARGET_PATH selected=rtv\n')
        a['native_backend'](text)
        for bad in (text.replace('scale=1', 'scale=2'), text.replace('selected=rtv', 'selected=rov'),
                    text.replace('threshold=0', 'threshold=640'), text + text):
            with self.assertRaises(ValueError):
                a['native_backend'](bad)

    def test_source_pin_changes_require_review(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            first = root / next(iter(a['SOURCE_PINS']))
            first.parent.mkdir(parents=True)
            first.write_text('changed implementation')
            with self.assertRaises(ValueError):
                a['verify_sources'](root)


if __name__ == '__main__':
    unittest.main()

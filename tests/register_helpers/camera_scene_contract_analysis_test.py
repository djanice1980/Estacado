"""Reject false current-view, resource identity and allocation-coverage claims."""
import copy
import hashlib
from pathlib import Path
import runpy
import struct
import tempfile
import unittest

import numpy as np

analysis = runpy.run_path(str(Path(__file__).parents[2] / 'scripts/analyze-camera-scene-contract.py'))


def words(values):
    return list(struct.unpack('<4I', struct.pack('<4f', *values)))


def matrix_fixture():
    p = np.array([[2, 0, 0, 0], [0, -4, 0, 0], [0, 0, 1.25, 1], [0, 0, -2, 0]])
    regs = {i: words(row) for i, row in enumerate(p.T)}
    # The sparse GPU products retain signed zero from the negative Y scale.
    regs[1] = words([-0., -4., -0., 0.])
    regs[7] = words([4, -5, 6, 0])
    current = np.eye(4)
    current[:3, 3] = [4, -5, 6]
    second = current.copy()
    second[0, 3] += .125
    for start, matrix in ((12, current), (16, second)):
        regs.update({start + i: words(row) for i, row in enumerate(matrix)})
    return dict(ordinal=1, vs='FA91501A8940251D', registers=regs), p


def texture_fixture():
    return dict(state='0', slot='0', fetch='0', valid='1', outdated='0x0', scaled='0',
        tiled='1', dimension='1', resource='0x12345', guest='0x01000000+0x730000',
        guest_extent='1280x720x1', host_extent='1280x720x1', guest_format='26',
        words='8A024802,0100005A,0059E4FF,00A88D10,00000003,00000218',
        base=0x01000000, size=0x730000)


def resolve_fixture():
    common = dict(source_color='0x000C0300', dest_info='0x003C0D01',
        dest_pitch='0x02D00500', surface='0x14010500', written_scaled='0', control='0x00100040')
    return [dict(common, state='2', written='0x01000000', dest_base='0x01000000', bytes=str(0x3C0000)),
            dict(common, state='5', written='0x013C0000', dest_base='0x013C0000', bytes=str(0x370000))]


class SceneContractTests(unittest.TestCase):
    def test_current_basis_and_translation_have_independent_evidence(self):
        draw, p = matrix_fixture()
        actual = analysis['matrix_agreement'](draw, p)
        self.assertEqual(actual['first_basis_bit_matches'], 12)
        self.assertEqual(actual['second_basis_bit_matches'], 12)
        self.assertEqual(actual['first_translation_max_coefficient_residual'], 0)
        self.assertTrue(actual['matrices_differ'])
        # Equal bases can still have different translations; neither result
        # identifies the second matrix as a temporally adjacent camera.
        changed = copy.deepcopy(draw)
        changed['registers'][12][3] = words([100, 0, 0, 0])[0]
        actual = analysis['matrix_agreement'](changed, p)
        self.assertEqual(actual['first_basis_bit_matches'], 12)
        self.assertEqual(actual['first_translation_max_coefficient_residual'], 192)
        self.assertNotIn('temporal_history_verified', actual)

    def test_basis_rejects_mismatch_missing_register_nonfinite_and_wrong_affine(self):
        draw, p = matrix_fixture()
        variants = []
        bad = copy.deepcopy(draw)
        bad['registers'][12][0] = words([1.125, 0, 0, 0])[0]
        variants.append(bad)
        bad = copy.deepcopy(draw)
        del bad['registers'][19]
        variants.append(bad)
        bad = copy.deepcopy(draw)
        bad['registers'][16][0] = 0x7FC00000
        variants.append(bad)
        bad = copy.deepcopy(draw)
        bad['registers'][15][3] = 0
        variants.append(bad)
        for bad in variants:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                analysis['matrix_agreement'](bad, p)
        bad_p = p.copy()
        bad_p[0, 2] = .1
        with self.assertRaises(ValueError):
            analysis['matrix_agreement'](draw, bad_p)

    def test_allocation_coverage_does_not_certify_pixels(self):
        result = analysis['allocation_chain'](texture_fixture(), resolve_fixture(), 0xC0300, 26)
        self.assertTrue(result['exact_ordered_allocation_coverage'])
        self.assertFalse(result['pixel_ownership_verified'])
        self.assertEqual(result['allocation_bytes'], 0x730000)
        self.assertEqual([r['resolve'] for r in result['ranges']], [3, 6])

    def test_range_gaps_overlap_order_scale_and_unwritten_tail_rejected(self):
        variants = []
        for delta in (-4096, 4096):
            bad = resolve_fixture()
            bad[1]['written'] = bad[1]['dest_base'] = hex(0x13C0000 + delta)
            variants.append(bad)
        variants.append(resolve_fixture()[::-1])
        for key, value in (('written_scaled', '1'), ('bytes', str(0x360000)),
                           ('control', '0x00100240'), ('dest_info', '0x01000302')):
            bad = resolve_fixture()
            bad[1][key] = value
            variants.append(bad)
        variants.append(resolve_fixture() + [resolve_fixture()[0]])
        for bad in variants:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                analysis['allocation_chain'](texture_fixture(), bad, 0xC0300, 26)

    def test_actual_texture_requires_single_occurrence_and_fresh_descriptor(self):
        draw = dict(ordinal=840)
        state = dict(state='0', occurrences='1', first_draw='840', last_draw='840', used_textures='0x1')
        resource = dict(texture_resource_changes='0')
        texture = texture_fixture()
        result = analysis['singleton_textures'](draw, state, resource, [texture])
        self.assertEqual(result[0]['base'], 0x1000000)
        for mutation in (dict(occurrences='2'), dict(first_draw='839'), dict(last_draw='841'),
                         dict(used_textures='0x3')):
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                analysis['singleton_textures'](draw, dict(state, **mutation), resource, [texture])
        for mutation in (dict(valid='0'), dict(outdated='0x1'), dict(scaled='1'),
                         dict(guest='0x01001000+0x730000'), dict(resource='0x0')):
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                analysis['singleton_textures'](draw, state, resource, [dict(texture, **mutation)])

    def test_ambiguous_aggregate_state_is_never_chosen_arbitrarily(self):
        draw = dict(ordinal=10, vs='01', ps='02', surface='03', depth='04', depth_control='05', scale='1x1')
        state = dict(vs='01', ps='02', surface='03', depth_info='04', normalized_depth_control='05',
                     draw_scale='1x1', raster='06', first_draw='9', last_draw='11')
        self.assertEqual(analysis['unique_state'](draw, [state], dict(raster='06')), state)
        for states in ([], [state, dict(state)]):
            with self.assertRaises(ValueError):
                analysis['unique_state'](draw, states, dict(raster='06'))

    def test_shader_pin_requires_both_raw_bytes_and_reviewed_disassembly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw, text = b'raw program', b'reviewed complete disassembly'
            shader = 'TEST_ONLY'
            analysis['SHADERS'][shader] = ('vert', hashlib.sha256(raw).hexdigest(), hashlib.sha256(text).hexdigest())
            try:
                (root / f'shader_{shader}.ucode.bin.vert').write_bytes(raw)
                listing = root / f'shader_{shader}.ucode.vert'
                listing.write_bytes(text)
                analysis['verify_shader'](shader, root)
                listing.write_bytes(text + b' changed operand')
                with self.assertRaises(ValueError):
                    analysis['verify_shader'](shader, root)
                with self.assertRaises(ValueError):
                    analysis['verify_shader']('UNKNOWN', root)
            finally:
                del analysis['SHADERS'][shader]


if __name__ == '__main__':
    unittest.main()

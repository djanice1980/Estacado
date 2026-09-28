from copy import deepcopy
from pathlib import Path
import runpy
import unittest

a = runpy.run_path(str(Path(__file__).resolve().parents[2]/'scripts/analyze-owned-draw-transforms.py'))


def fixture():
    reference = dict(header=dict(frame=10), draws=[dict(
        meta=dict(vs='81D611665A691E95',draw=211,offset=0,scissor=[0,1280],surface=5,depth=7),
        raster=dict(viewport=[1,2,3,4,5,6],vte=9,clip=11),
        vertex_constants={r:[r]*4 for r in [0,1,2,3,7,12,13,14,15]})])
    raw = ','.join(f'{r:08X}' for r in [0,1,2,3,7,12,13,14,15] for _ in range(4))
    text = ('REX_PC_DRAW_TRANSFORMS frame=10 previous=9 current_count=1 previous_count=1 '
            'scope=submitted_current_inputs instance_identity=0 reprojection_valid=0\n'
            f'REX_PC_DRAW_INPUT frame=10 index=0 vs=81D611665A691E95 raw={raw} '
            'viewport=1,2,3,4,5,6 window=0 scissor=0,500 vte=9 clip=B surface=5 depth=7')
    return text, reference


class OwnedDrawTests(unittest.TestCase):
    def test_exact_inputs(self):
        text, ref = fixture()
        self.assertEqual(a['verify_frame'](text,10,1,ref)['exact_transform_words'],36)

    def test_reject_wrong_association_or_claim(self):
        text, ref = fixture()
        for old,new in [('frame=10','frame=11'),('previous=9','previous=8'),
                        ('previous_count=1','previous_count=2'),('index=0','index=1'),
                        ('instance_identity=0','instance_identity=1'),
                        ('reprojection_valid=0','reprojection_valid=1'),
                        ('window=0','window=1'),('vte=9','vte=8'),('viewport=1','viewport=2')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                a['verify_frame'](text.replace(old,new,1),10,1,ref)

    def test_reject_changed_or_missing_words(self):
        text, ref = fixture()
        for raw in ['00000000,','00000001,']:
            bad = text.replace('raw=00000000,00000000,', 'raw='+raw,1)
            with self.assertRaises(ValueError): a['verify_frame'](bad,10,1,ref)
        changed = deepcopy(ref)
        changed['draws'][0]['vertex_constants'][12][0] ^= 1
        with self.assertRaises(ValueError): a['verify_frame'](text,10,1,changed)

    def test_reject_incomplete_or_duplicated_records(self):
        text, ref = fixture()
        for bad in [text.splitlines()[0],text+'\n'+text.splitlines()[1],text+'\n'+text.splitlines()[0]]:
            with self.assertRaises(ValueError): a['verify_frame'](bad,10,1,ref)


if __name__ == '__main__': unittest.main()

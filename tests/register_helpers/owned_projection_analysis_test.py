from pathlib import Path
import runpy
import unittest

root=Path(__file__).resolve().parents[2]
a=runpy.run_path(str(root/'scripts/verify-owned-projection-evidence.py'))
base=runpy.run_path(str(Path(__file__).with_name('owned_scene_color_analysis_test.py')))


def fixture():
    text=base['fixture']()
    words=['3F891A29','00000000','00000000','00000000','00000000','BFF3BCBB',
           '00000000','00000000','00000000','00000000','3F8002F3','3F800000',
           '00000000','00000000','BFE66BB6','00000000']
    frames=[]
    for frame in (10,11):
        text+=f'\nREX_PC_PROJECTION frame={frame} previous={frame-1} valid=1 previous_valid=1 unchanged=1 raw='+','.join(words)+' scope=unique_sparse_projection camera_pose=0 temporal_valid=0'
        frames.append(dict(frame=frame,native_projection_values=[words]))
    return text,frames


class ProjectionAnalysisTests(unittest.TestCase):
    def test_exact(self):
        text,frames=fixture()
        self.assertEqual(a['verify_live'](text,frames),[10,11])

    def test_reject_values_and_claims(self):
        text,frames=fixture()
        for old,new in [('raw=3F891A29','raw=3F891A28'),('valid=1 previous','valid=0 previous'),
                        ('previous_valid=1','previous_valid=0'),('unchanged=1','unchanged=0'),
                        ('camera_pose=0','camera_pose=1'),('temporal_valid=0','temporal_valid=1')]:
            with self.subTest(old=old),self.assertRaises(ValueError):
                a['verify_live'](text.replace(old,new,1),frames)

    def test_reject_missing_duplicate_and_mixed_frames(self):
        text,frames=fixture()
        for changed in ['\n'.join(text.splitlines()[:-1]),text+'\n'+text.splitlines()[-1],
                        text.replace('REX_PC_PROJECTION frame=10','REX_PC_PROJECTION frame=9')]:
            with self.assertRaises(ValueError): a['verify_live'](changed,frames)

    def test_reject_ambiguous_native_reference(self):
        text,frames=fixture()
        frames[0]['native_projection_values']*=2
        with self.assertRaises(ValueError): a['verify_live'](text,frames)


if __name__=='__main__': unittest.main()

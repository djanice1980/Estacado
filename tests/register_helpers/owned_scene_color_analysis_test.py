from pathlib import Path
import runpy
import unittest

a = runpy.run_path(str(Path(__file__).resolve().parents[2]/'scripts/analyze-owned-scene-color.py'))


def fixture():
    lines = []
    for frame in (10,11):
        lines.append(f'REX_PC_SCENE_COLOR_COPY frame={frame} submission={frame} source=AAAA owned={frame:016X} guest_base=12340000 guest_format=26 shader=A59B41D0BD79484B slot=0')
        lines.append(f'REX_PC_SCENE_COLOR frame={frame} previous={frame-1} epoch=3 width=1280 height=720 format=11 swizzle=123 signs=0 exponent=4 current_resource={frame:016X} previous_resource={frame-1:016X} state=shader_resource scope=owned_color_only temporal_valid=0')
        lines.append(f'REX_PC_SCENE_COLOR_VERIFY frame={frame} submission={frame} completed={frame+1} rows=720 row_bytes=10240 equal=1 written=1 scope=source_vs_owned_exact_bytes')
    return '\n'.join(lines)


class OwnedColorTests(unittest.TestCase):
    def test_exact_pair(self):
        self.assertEqual(len(a['verify_records'](fixture())),2)

    def test_failed_and_incomplete_proof(self):
        for old,new in [('equal=1','equal=0'),('written=1','written=0'),
                        ('completed=11','completed=9'),('rows=720','rows=719')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                a['verify_records'](fixture().replace(old,new,1))

    def test_wrong_frame_resource_or_history_claim(self):
        for old,new in [('previous=10','previous=9'),('epoch=3','epoch=4'),
                        ('previous_resource=000000000000000A','previous_resource=0000000000000001'),
                        ('temporal_valid=0','temporal_valid=1'),('slot=0','slot=1')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                a['verify_records'](fixture().replace(old,new,1))

    def test_duplicate_or_missing_completion(self):
        t=fixture()
        with self.assertRaises(ValueError):a['verify_records'](t+'\n'+t.splitlines()[-1])
        with self.assertRaises(ValueError):a['verify_records']('\n'.join(t.splitlines()[:-1]))


if __name__ == '__main__':unittest.main()

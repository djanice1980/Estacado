import runpy
import unittest
from pathlib import Path

a = runpy.run_path(str(Path(__file__).resolve().parents[2] /
                     'scripts/analyze-resolve-owner-coverage.py'))


def rect(first, rows, start, end):
    return dict(row_first=first, rows=rows, first_start=start, last_end=end)


class OwnershipCoverageTest(unittest.TestCase):
    def test_full_and_missing_third(self):
        self.assertEqual(a['coverage'](16,48,16,[rect(0,48,0,16)])['covered_tiles'],768)
        self.assertEqual(a['coverage'](16,48,16,[rect(0,32,0,16)])
                         ['missing_relative_tile_ranges'], [[512,768]])

    def test_padding_partial_and_split(self):
        r = a['coverage'](2,3,4,[rect(0,2,1,2),rect(2,1,0,1)])
        self.assertEqual(r['requested_tiles'],6)
        self.assertEqual(r['missing_relative_tile_ranges'],[[0,1],[9,10]])

    def test_incomplete_metadata_is_not_missing_ownership(self):
        log = ('REX_EMBEDDED_RESOLVE_GRID ordinal=1 dest_base=0x1118A000 '
               'rectangles=1 tiles=0,16,48,16\n')
        self.assertEqual(a['analyze'](log)[0]['status'],'INCOMPLETE_OWNER_METADATA')
        with self.assertRaises(ValueError): a['analyze'](log+log)

    def test_invalid_span(self):
        for args in ((16,0,16,[]),(17,48,16,[]),(16,48,16,[rect(32,17,0,16)])):
            with self.assertRaises(ValueError): a['coverage'](*args)


if __name__ == '__main__': unittest.main()

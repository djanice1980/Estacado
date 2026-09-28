import runpy
import unittest
import tempfile
from pathlib import Path
import numpy as np

a=runpy.run_path(str(Path(__file__).resolve().parents[2]/'scripts/analyze-depth-dump-boundary.py'))


def fixture(samples,missing=False):
    width,height=80,384
    host=np.zeros((height,width,samples,2),dtype='<u4')
    edram=np.zeros(2048*1280,dtype='<u4')
    pitch=2 if samples==4 else 1
    for y in range(height):
        for x in range(width):
            for s in range(samples):
                hsample=1-s if samples==2 else s
                stencil=(x*13+y*7+s*19)%255+1
                host[y,x,hsample]=[0x3e800000,stencil]
                if samples==1: u,v=x,y
                elif samples==2: u,v=(x&~2)|(s<<1),((y&~1)<<1)|(y&1)|(x&2)
                else: u,v=((x&~1)<<1)|(x&1)|((s&1)<<1),((y&~1)<<1)|(y&1)|(s&2)
                tile=(v//16)*pitch+u//80
                col=u%80
                swapped=col+40 if col<40 else col-40
                index=tile*1280+(v%16)*80+swapped
                edram[index]=0 if missing and y>=256 else 0x100|stencil
    rows=height*(2 if samples>1 else 1)//16
    owner=dict(row_first=0,rows=rows,first_start=0,last_end=pitch)
    return host.tobytes(),edram.tobytes(),width,height,pitch,owner


class DepthDumpTest(unittest.TestCase):
    def capture_case(self, case):
        host,edram,w,h,pitch,owner=fixture(2)
        (case/'host.bin').write_bytes(host)
        (case/'edram.bin').write_bytes(edram)
        text='\n'.join([
            f'REX_EMBEDDED_DEPTH_SOURCE_QUEUED ordinal=1 dest_base=0x1118A000 resource=00001234 width={w} height={h} samples=2 bytes={len(host)} submission=9 path=host.bin',
            f'REX_EMBEDDED_DEPTH_SOURCE_COMPLETE result=1 ordinal=1 bytes={len(host)} written={len(host)} submission=9 completed=9 path=host.bin',
            f'REX_EMBEDDED_DEPTH_EDRAM_QUEUED result=1 ordinal=1 dest_base=0x1118A000 bytes={len(edram)} submission=9 scale=1x1 path=edram.bin',
            f'REX_EMBEDDED_DEPTH_EDRAM_COMPLETE result=1 ordinal=1 bytes={len(edram)} written={len(edram)} submission=9 completed=10 path=edram.bin',
            'REX_EMBEDDED_RESOLVE_GRID ordinal=1 dest_base=0x1118A000 tiles=0,1,48,1 rectangles=1',
            f'REX_EMBEDDED_RESOLVE_OWNER ordinal=1 owner=0 dest_base=0x1118A000 resource=00001234 base=0 pitch=1 msaa=1 depth=1 format=1 host_format=19 host_width={w} host_height={h} host_samples=2 row_first=0 rows=48 first_start=0 last_end=1',
        ])
        (case/'stderr.log').write_text(text)
        return text

    def test_complete_capture_contract(self):
        with tempfile.TemporaryDirectory() as folder:
            case=Path(folder)
            self.capture_case(case)
            result=a['analyze'](case,1)
            self.assertEqual(result['compared_samples'],80*384*2)
            self.assertEqual(result['mismatch_host_rows'],[])

    def test_reject_incomplete_or_unlike_capture(self):
        with tempfile.TemporaryDirectory() as folder:
            case=Path(folder)
            original=self.capture_case(case)
            changes=[('completed=9','completed=8'), ('written=491520','written=0'),
                     ('scale=1x1','scale=2x2'), ('host_format=19','host_format=40'),
                     ('row_first=0 rows=48','row_first=1 rows=48'),
                     ('rectangles=1','rectangles=2'), ('host_samples=2','host_samples=4'),
                     ('path=host.bin','path=../host.bin'),
                     ('path=host.bin','path=..\\host.bin'),
                     ('path=host.bin','path=host.bin:stream')]
            for old,new in changes:
                with self.subTest(change=new):
                    self.assertIn(old,original)
                    (case/'stderr.log').write_text(original.replace(old,new))
                    with self.assertRaises(ValueError): a['analyze'](case,1)
            (case/'stderr.log').write_text(original+'\n'+original.splitlines()[0])
            with self.assertRaises(ValueError): a['analyze'](case,1)
            (case/'stderr.log').write_text(original)
            (case/'host.bin').write_bytes(b'partial')
            with self.assertRaises(ValueError): a['analyze'](case,1)

    def test_fixed_coordinates(self):
        # Native 2x: canonical(u=2,v=0) is pixel(0,0), guest sample1 -> host0.
        x,y,s=a['sample_coordinates'](np.array([0,2,0]),np.array([0,0,2]),2,2)
        self.assertEqual((x.tolist(),y.tolist(),s.tolist()),([0,0,2],[0,0,0],[1,0,1]))
        self.assertEqual(a['sample_coordinates'](np.array([0,2]),np.array([0,0]),2,4)[2].tolist(),[0,3])

    def test_all_native_sample_layouts(self):
        for samples in (1,2,4):
            host,edram,w,h,pitch,owner=fixture(samples)
            r=a['compare'](host,edram,w,h,samples,samples,0,pitch,0,pitch,pitch,[owner])
            self.assertEqual(r['compared_samples'],w*h*samples)
            self.assertEqual(r['mismatch_host_rows'],[])

    def test_missing_128_rows(self):
        host,edram,w,h,pitch,owner=fixture(2,True)
        r=a['compare'](host,edram,w,h,2,2,0,pitch,0,pitch,pitch,[owner])
        self.assertEqual(r['host_positive_packed_zero'],80*128*2)
        self.assertEqual(r['stencil_mismatches'],80*128*2)
        self.assertEqual(r['mismatch_host_rows'],list(range(256,384)))

    def test_reject_invalid_contract(self):
        with self.assertRaises(ValueError): a['sample_coordinates'](0,0,4,2)
        with self.assertRaises(ValueError): a['compare'](b'',b'',80,384,2,2,0,1,0,1,1,[])
        with self.assertRaises(ValueError): a['unique']('', 'REX_EMBEDDED_DEPTH_SOURCE_COMPLETE',1)


if __name__=='__main__': unittest.main()

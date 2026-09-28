"""Synthetic diagnostic tests, never guest evidence or fidelity acceptance."""
import runpy
import unittest
from pathlib import Path
import numpy as np

a = runpy.run_path(str(Path(__file__).resolve().parents[2] / "scripts/analyze-depth-source-boundary.py"))


class DepthBoundaryTest(unittest.TestCase):
    def test_snapshot_source_lifetime_guards(self):
        # Source-level safety checks, not a GPU execution/fidelity test.
        root = Path(__file__).resolve().parents[2]
        cpp = (root / 'external/ReXGlue/src/graphics/d3d12/texture_cache.cpp').read_text()
        self.assertIn('embedded_temporal_depth_snapshot, false', cpp)
        begin = cpp.index('void D3D12TextureCache::CaptureActiveTextureReadbackDiagnostics(')
        end = cpp.index('void D3D12TextureCache::TryCompleteActiveTextureReadbackDiagnostics(')
        queued = cpp[begin:end]
        self.assertLess(queued.index('TryReserve('), queued.index('IID_PPV_ARGS(&depth_snapshot)'))
        self.assertLess(queued.index('reservation_bytes += allocation.SizeInBytes'),
                        queued.index('TryReserve('))
        self.assertIn('array_slice, reservation_bytes', queued)
        self.assertIn('key.format == xenos::TextureFormat::k_24_8_FLOAT', queued)
        self.assertIn('texture_desc.Format == DXGI_FORMAT_R32_FLOAT', queued)
        self.assertIn('diagnostic.depth_snapshot = std::move(depth_snapshot)', queued)
        completed = cpp[end:cpp.index('void D3D12TextureCache::UpdateTextureBindingsImpl(', end)]
        self.assertLess(completed.index('GetCompletedSubmission() < diagnostic.submission'),
                        completed.index('depth_snapshot.Reset()'))
        self.assertLess(completed.index('readback->Map('),
                        completed.index('REX_TEMPORAL_DEPTH_SNAPSHOT_READY'))

    def test_first_binding_capture_guard(self):
        root = Path(__file__).resolve().parents[2]
        cpp = (root/'external/ReXGlue/src/graphics/d3d12/texture_cache.cpp').read_text()
        body = cpp.split('void D3D12TextureCache::CaptureFirstTemporalDepthBinding(')[1].split(
            'void D3D12TextureCache::CaptureActiveTextureReadbackDiagnostics(')[0]
        for guard in ('embedded_temporal_depth_first_binding', 'embedded_temporal_depth_snapshot',
                      'temporal_first_binding_attempted_', '!context_draw_ordinal',
                      'used_texture_mask &', 'k_24_8_FLOAT', 'DXGI_FORMAT_R32_FLOAT',
                      'desc.DepthOrArraySize != 1', 'desc.SampleDesc.Count != 1'):
            self.assertIn(guard, body)
        self.assertLess(body.index('temporal_first_binding_attempted_ = true'),
                        body.index('CaptureActiveTextureReadbackDiagnostics('))
        self.assertIn('before ? 1u : 0u);\n    return;', body)

    def test_snapshot_capture_identity(self):
        ready = ('REX_TEMPORAL_DEPTH_SNAPSHOT_READY frame=8 draw=9 submission=5 '
                 'completed=5 source=0x0000000000000010 snapshot=0x0000000000000020 '
                 'width=1 height=1 hash=4B95F515 state=COPY_SOURCE '
                 'scope=encoded_depth_not_sdk_ready\n')
        saved = ('REX_EMBEDDED_ACTIVE_TEXTURE_READBACK_READY slot=1 array_slice=0 '
                 'array_size=1 width=1 height=1 depth=1 format=41 row_bytes=4 rows=1 '
                 'identity=0x0000000000000010 context_draw=9 resource_hash=4B95F515 '
                 'resource=snapshot.bin written=1\n')
        check = lambda log, data=b'\0'*4: a['verify_snapshot_capture'](
            log, 'snapshot.bin', data, 8, 9, 1, 1, 1)
        self.assertEqual(check(ready+saved)['submission'], 5)
        for bad in (saved, ready, ready+ready+saved, ready+saved+saved, saved+ready,
                    ready.replace('completed=5', 'completed=4')+saved,
                    ready.replace('snapshot=0x0000000000000020',
                                  'snapshot=0x0000000000000010')+saved,
                    ready.replace('frame=8', 'frame=7')+saved,
                    ready.replace('state=COPY_SOURCE', 'state=COPY_DEST')+saved,
                    ready+saved.replace('written=1', 'written=0'),
                    ready+saved.replace('format=41', 'format=28'),
                    ready+saved.replace('hash=4B95F515', 'hash=00000000')):
            with self.assertRaises(ValueError): check(bad)
        for data in (b'\0'*3, b'\0'*3+b'\1'):
            with self.assertRaises(ValueError): check(ready+saved, data)

    def test_shadow_consumer_depth_reference(self):
        # Exact rational fixture, not a title/SDK projection acceptance test.
        reconstruct = a['reconstruct_shadow_consumer_depth']
        np.testing.assert_allclose(reconstruct([0, .5, 1], [20, 0, 40, 16]),
                                   [10, 40/12, 2])
        for d, c in (([0], [16,0,40,16]), ([np.nan], [20,0,40,16]),
                     ([0], [20,0,np.inf,16]), ([0], [1,2])):
            with self.assertRaises(ValueError): reconstruct(d, c)

    def test_float24_magnitude(self):
        words = np.array([0, 1, 0xFFFFF, 0x100000, 0xE00000, 0xF00000, 0xFFFFFF],dtype=np.uint32)
        expected = np.array([0, 2**-34, (2**20-1)*2**-34, 2**-14,
                             0.5, 1.0, 2-2**-20], dtype='<f4')
        np.testing.assert_array_equal(a['decode_float24'](words), expected)
        source = np.array([0xF000007F],dtype='<u4').tobytes()
        compare = lambda value, rep: a['compare_magnitude'](
            source,np.array([value],dtype='<f4').tobytes(),1,1,32,0,rep)
        self.assertEqual(compare(1,'guest_float24')['exact_mismatches'],0)
        self.assertEqual(compare(.5,'host_half_range')['exact_mismatches'],0)
        self.assertEqual(compare(.5,'guest_float24')['exact_mismatches'],1)
        for value in (-1, np.nan, np.inf):
            with self.assertRaises(ValueError): compare(value,'guest_float24')

    def test_source_fence_and_identity(self):
        q = ('REX_EMBEDDED_TARGET_TEXTURE_SOURCE draw=9 writer=1 slot=1 '
             'address=0x1118A000 bytes=4 queued=1 path=source.bin\n')
        c = ('REX_EMBEDDED_TEXTURE_SOURCE_READBACK result=1 draw=9 '
             'address=0x1118A000 bytes=4 written=4 submission=5 completed=5 path=source.bin\n')
        check = lambda log: a['verify_source_capture'](log,'source.bin',9,1,1,4)
        self.assertEqual(check(q+c)['submission'],5)
        for bad in (q, q+q+c, q+c+c, q+c.replace('completed=5','completed=4'),
                    q+c.replace('written=4','written=0'),
                    q+c.replace('address=0x1118A000','address=0x1136A000'),
                    q.replace('queued=1','queued=0')+c):
            with self.assertRaises(ValueError): check(bad)

    def test_address_vectors(self):
        o = a["offsets"](32, 33, 32)
        for x, y, expected in ((0,0,0), (1,0,4), (4,0,32), (0,1,16),
                               (8,0,64), (0,8,1152), (0,16,2048), (0,32,4096)):
            self.assertEqual(int(o[y,x]), expected)

    def test_zero_stencil_and_endian(self):
        for endian, encoded in ((0,0x0000017F), (1,0x00007F01),
                                (2,0x7F010000), (3,0x017F0000)):
            raw = np.zeros(4096 // 4, dtype='<u4')
            raw[0] = encoded
            self.assertEqual(int(a["depth_words"](raw.tobytes(),1,1,32,endian)[0,0]),1)
        raw = np.array([0xFF], dtype='<u4').tobytes()
        self.assertEqual(int(a["depth_words"](raw,1,1,32,0)[0,0]),0)

    def test_detects_conversion_disagreement(self):
        source = np.array([0x100], dtype='<u4').tobytes()
        host = np.zeros(1,dtype='<f4').tobytes()
        r = a["compare"](source,host,1,1,32,0)
        self.assertEqual(r['source_nonzero_host_zero'],1)
        self.assertEqual(r['host_zero_rows'],[[0,1]])

    def test_rejects_incomplete_or_unlike_data(self):
        for args in ((b'',1,1,32,0), (b'1234',32,32,32,0), (b'1234',1,1,32,4)):
            with self.assertRaises(ValueError): a['depth_words'](*args)
        with self.assertRaises(ValueError): a['offsets'](32,32,31)
        with self.assertRaises(ValueError):
            a['compare'](b'1234', np.array([np.nan], dtype='<f4').tobytes(),1,1,32,0)


if __name__ == '__main__': unittest.main()

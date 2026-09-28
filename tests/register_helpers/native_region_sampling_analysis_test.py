"""Independent image-space oracle for the mixed atlas; no game bytes or input."""
import importlib.util
from pathlib import Path
import unittest
import numpy as np

spec = importlib.util.spec_from_file_location('composite', Path(__file__).resolve().parents[2]/'scripts/analyze-glow-composite.py')
model = importlib.util.module_from_spec(spec); spec.loader.exec_module(model)
linear = model.resample.glow.linear_sample

class NativeRegionSamplingTests(unittest.TestCase):
    def setUp(self):
        rng = np.random.default_rng(175)
        self.native = rng.random((8,16,4), dtype=np.float32)
        self.host = self.native.repeat(2,axis=0).repeat(2,axis=1)
        self.host[:,16:] = rng.random((16,16,4), dtype=np.float32)
        self.rect = (0,0,8,8)

    def test_native_alone_matches_original_bilinear(self):
        host = self.native.repeat(2,axis=0).repeat(2,axis=1)
        u,v = np.meshgrid(np.linspace(-.1,1.1,57),np.linspace(-.1,1.1,39))
        np.testing.assert_allclose(model.native_region_sample(host,u,v,2,(0,0,16,8)),
                                   linear(self.native,u,v),rtol=0,atol=2e-6)

    def test_high_resolution_alone_is_unchanged(self):
        u,v = np.meshgrid(np.linspace(0,1,57),np.linspace(0,1,39))
        np.testing.assert_array_equal(model.native_region_sample(self.host,u,v,2,None),linear(self.host,u,v))

    def test_shared_native_interior_matches_original(self):
        u,v = np.meshgrid(np.linspace(.04,.43,47),np.linspace(.03,.97,39))
        np.testing.assert_allclose(model.native_region_sample(self.host,u,v,2,self.rect),
                                   linear(self.native,u,v),rtol=0,atol=2e-6)
        modified = self.host.copy(); modified[:,16:] = 1000
        np.testing.assert_array_equal(model.native_region_sample(modified,u,v,2,self.rect),
                                      model.native_region_sample(self.host,u,v,2,self.rect))

    def test_updating_native_region_preserves_scaled_neighbor(self):
        u,v = np.meshgrid(np.linspace(.55,.96,41),np.linspace(.02,.98,31))
        modified = self.host.copy(); modified[:,:16] = 1000
        expected = linear(self.host,u,v)
        np.testing.assert_array_equal(model.native_region_sample(modified,u,v,2,self.rect),expected)
        self.assertGreater(float(np.max(np.abs(expected-model.resample.glow.native_grid_sample(self.host,u,v,2)))),.1)

    def test_mixed_boundary_uses_ordinary_path_without_seam(self):
        # Native footprint begins crossing the region at texel index7. The
        # switch occurs at that cell's center, where both representations agree.
        u = np.array([(7.5-1e-7)/16,7.5/16,(7.5+1e-7)/16,7.9/16])
        v = np.full_like(u,3.5/8)
        actual = model.native_region_sample(self.host,u,v,2,self.rect)
        np.testing.assert_array_equal(actual[1:],linear(self.host,u,v)[1:])
        np.testing.assert_allclose(actual[0],actual[1],rtol=0,atol=2e-6)

    def test_original_one_x_is_bit_identical(self):
        u,v = np.meshgrid(np.linspace(0,1,57),np.linspace(0,1,39))
        np.testing.assert_array_equal(model.native_region_sample(self.native,u,v,1,self.rect),linear(self.native,u,v))

    def test_discrete_filter_result_keeps_phase_without_replica_grid(self):
        # Paired fractional taps make re-evaluation (V172) inequivalent to
        # reconstructing the original discrete result. Preserve that result.
        glow = model.resample.glow
        impulse = np.zeros((8,32,4),dtype=np.float32); impulse[:,16] = 1
        c=np.zeros((256,4),dtype=np.float32)
        c[1]=1;c[2]=1;c[4]=1;c[5,3]=.2;c[6,0]=.4;c[8,0]=1.4/32
        discrete=glow.filter17(impulse,c,32,8,1,1)
        stored=discrete.repeat(2,0).repeat(2,1)
        u,v=np.meshgrid((np.arange(256)+.5)/256,(np.arange(32)+.5)/32)
        expected=linear(discrete,u,v)
        actual=model.native_region_sample(stored,u,v,2,(0,0,32,8))
        np.testing.assert_allclose(actual,expected,rtol=0,atol=2e-6)
        self.assertGreater(float(np.max(np.abs(linear(stored,u,v)-expected))),.01)
        wrong=glow.filter17(impulse,c,256,32,1,1,evaluation_extent=(256,32))
        self.assertGreater(float(np.max(np.abs(wrong-expected))),.025)

if __name__ == '__main__': unittest.main()

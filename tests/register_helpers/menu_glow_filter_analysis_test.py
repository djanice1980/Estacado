"""Analytical filtering invariants, independent of copyrighted capture bytes."""
import importlib.util
from pathlib import Path
import unittest
import numpy as np

script = Path(__file__).resolve().parents[2] / 'scripts/analyze-menu-glow-filter.py'
spec = importlib.util.spec_from_file_location('glow_filter', script)
glow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(glow)


class NativeFilterTests(unittest.TestCase):
    def test_four_host_fetches_equal_native_box_then_bilinear(self):
        rng = np.random.default_rng(127)
        source = rng.random((18, 26, 4), dtype=np.float32)
        # Include wrapping/clamping boundary neighborhoods, not just centers.
        u = rng.uniform(-.1, 1.1, (31, 47)).astype(np.float32)
        v = rng.uniform(-.1, 1.1, (31, 47)).astype(np.float32)
        guest = np.stack((u*13-.5, v*9-.5), axis=-1)
        base = np.floor(guest)
        frac = guest-base
        samples=[]
        for x,y in [(0,0),(1,0),(0,1),(1,1)]:
            samples.append(glow.linear_sample(source,np.clip(base[...,0]+x+.5,.5,12.5)/13,
                                              np.clip(base[...,1]+y+.5,.5,8.5)/9))
        row0=samples[0]+(samples[1]-samples[0])*frac[...,0:1]
        row1=samples[2]+(samples[3]-samples[2])*frac[...,0:1]
        actual=row0+(row1-row0)*frac[...,1:2]
        expected=glow.native_grid_sample(source,u,v,2)
        np.testing.assert_allclose(actual,expected,rtol=0,atol=2e-6)

    def test_native_path_is_identical(self):
        source=np.arange(64,dtype=np.float32).reshape(4,4,4)
        u,v=np.meshgrid(np.linspace(0,1,11),np.linspace(0,1,13))
        np.testing.assert_array_equal(glow.native_grid_sample(source,u,v,1),glow.linear_sample(source,u,v))

    def test_dc_gain_and_alpha_are_preserved(self):
        c=np.zeros((256,4),dtype=np.float32)
        c[1]=1; c[2]=1; c[3]=0; c[4]=1; c[5,3]=.5
        c[6:8]=.03125
        source=np.ones((16,16,4),dtype=np.float32)*.25
        # .5 center + sixteen .03125 neighbors = 1.0 RGB DC gain.
        for scale in [1,2]:
            result=glow.filter17(source,c,4,4,1,1,sampling_grid_scale=scale)
            np.testing.assert_allclose(result[...,:3],.25,rtol=0,atol=1e-7)
            np.testing.assert_allclose(result[...,3],.125,rtol=0,atol=1e-7)

    def test_reject_nonintegral_sampling_grid(self):
        with self.assertRaises(ValueError):
            glow.native_grid_sample(np.zeros((7,8,4),dtype=np.float32),.5,.5,2)

    def test_native_evaluation_is_independent_of_source_extent(self):
        rng=np.random.default_rng(168)
        source=rng.random((24,32,4),dtype=np.float32)
        native=source.reshape(12,2,16,2,4).mean(axis=(1,3))
        c=np.zeros((256,4),dtype=np.float32)
        c[1]=1; c[2]=1; c[3]=0; c[4]=1; c[5,3]=.5
        c[6:8]=.03125
        c[8:12]=rng.uniform(-.1,.1,(4,4))
        expected=glow.filter17(native,c,4,3,1,1)
        actual=glow.filter17(source,c,4,3,1,1,sampling_grid_scale=2,evaluation_extent=(16,12))
        np.testing.assert_array_equal(actual,expected)

    def test_native_footprint_with_scaled_evaluation_preserves_continuous_ramp(self):
        # A linear native-grid input stays linear between centers after the
        # unchanged filter. Repeating a native result into 2x2 host cells does
        # not have that property when the next ordinary sampler enlarges it.
        native=np.broadcast_to(np.linspace(0,1,16,dtype=np.float32)[None,:,None],(12,16,4)).copy()
        source=native.repeat(2,axis=0).repeat(2,axis=1)
        c=np.zeros((256,4),dtype=np.float32)
        c[1]=1;c[2]=1;c[3]=0;c[4]=1;c[5,3]=.5;c[6:8]=.03125
        c[8:12]=np.array([[1/16,0,2/16,0]]*4)
        scaled=glow.filter17(source,c,32,24,1,1,sampling_grid_scale=2,evaluation_extent=(32,24))
        expected=glow.filter17(native,c,32,24,1,1,evaluation_extent=(32,24))
        np.testing.assert_array_equal(scaled,expected)
        slope=np.diff(scaled[12,8:24,0])
        np.testing.assert_allclose(slope,np.full_like(slope,1/30),atol=1e-7)
        original=glow.filter17(source,c,16,12,1,1,sampling_grid_scale=2,evaluation_extent=(16,12))
        repeated=original.repeat(2,axis=0).repeat(2,axis=1)
        self.assertTrue(np.any(np.diff(repeated[12,8:24,0])==0))
        self.assertGreater(float(np.abs(repeated-scaled).max()),.01)

    def test_fractional_paired_taps_require_their_discrete_evaluation_phase(self):
        # Bilinear tap pairing is exact at original texel centers, not at all
        # intermediate positions. A ramp test alone cannot validate an image
        # filter's scaled evaluation. This is a synthetic impulse, not game data.
        native=np.zeros((8,32,4),dtype=np.float32);native[:,16,:]=1
        source=native.repeat(2,axis=0).repeat(2,axis=1)
        c=np.zeros((256,4),dtype=np.float32)
        c[1]=1;c[2]=1;c[3]=0;c[4]=1;c[5,3]=.2
        c[6,0]=.4;c[8,0]=1.4/32
        discrete=glow.filter17(native,c,32,8,1,1)
        u,v=np.meshgrid((np.arange(64)+.5)/64,(np.arange(16)+.5)/16)
        preserved=glow.linear_sample(discrete,u,v)
        wrong=glow.filter17(source,c,64,16,1,1,sampling_grid_scale=2)
        self.assertGreater(float(np.abs(preserved-wrong).max()),.025)

if __name__=='__main__': unittest.main()

"""Synthetic invariants for read-only captured glow reconstruction."""
import importlib.util
from pathlib import Path
import unittest
import numpy as np

script=Path(__file__).resolve().parents[2]/'scripts/analyze-glow-resample.py'
spec=importlib.util.spec_from_file_location('resample',script)
resample=importlib.util.module_from_spec(spec);spec.loader.exec_module(resample)


class ResampleTests(unittest.TestCase):
    def test_constant_square_and_alpha(self):
        source=np.ones((8,12,4),dtype=np.float32)*[.2,.3,.4,.7]
        c=np.zeros((256,4),dtype=np.float32)
        c[4]=[.3,.59,.11,0];c[5]=1;c[255]=[.125,1,0,0]
        actual=resample.resample8(source,c,3,2,(12,8))
        np.testing.assert_allclose(actual,np.broadcast_to([.04,.09,.16,.7],actual.shape),atol=1e-7)

    def test_desaturation_preserves_luminance(self):
        source=np.ones((8,12,4),dtype=np.float32)*[.2,.3,.4,.7]
        c=np.zeros((256,4),dtype=np.float32)
        c[4]=[.3,.59,.11,1];c[5]=1;c[255]=[.125,1,0,0]
        actual=resample.resample8(source,c,3,2,(12,8))
        luma=.04*.3+.09*.59+.16*.11
        np.testing.assert_allclose(actual,np.broadcast_to([luma,luma,luma,.7],actual.shape),atol=1e-7)

    def test_replication_then_bilinear_has_plateaus_not_native_interpolation(self):
        source=np.array([[[0.],[1.],[0.],[1.]]],dtype=np.float32)
        scaled=source.repeat(2,axis=0).repeat(2,axis=1)
        u=np.array([[.15625,.21875,.28125,.34375]],dtype=np.float32)
        native=resample.glow.linear_sample(source,u,np.zeros_like(u)+.5)
        replicated=resample.glow.linear_sample(scaled,u,np.zeros_like(u)+.5)
        np.testing.assert_allclose(native[0,:,0],[.125,.375,.625,.875])
        np.testing.assert_allclose(replicated[0,:,0],[0.,.25,.75,1.])
        np.testing.assert_allclose(resample.glow.native_grid_sample(scaled,u,np.zeros_like(u)+.5,2),native)


if __name__=='__main__':unittest.main()

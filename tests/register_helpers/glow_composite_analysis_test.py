import importlib.util
from pathlib import Path
import unittest
import numpy as np
root=Path(__file__).resolve().parents[2]/'scripts'
spec=importlib.util.spec_from_file_location('quad',root/'analyze-glow-quad.py')
quad=importlib.util.module_from_spec(spec);spec.loader.exec_module(quad)

class CompositeTests(unittest.TestCase):
    def test_identity_color_shader(self):
        texture=np.random.default_rng(207).random((4,6,4),dtype=np.float32)
        np.testing.assert_array_equal(quad.composite.color_shader(texture,np.ones(4),np.zeros(4)),texture)
    def test_zero_alpha_linear_color_factor(self):
        texture=np.ones((2,3,4),dtype=np.float32)*[.2,.3,.4,0]
        value=quad.composite.color_shader(texture,np.array([.5,.4,.3,1]),np.zeros(4))
        np.testing.assert_allclose(value,np.broadcast_to([.1,.12,.12,0],value.shape))
    def test_gpu_endian_decode(self):
        raw=bytes(range(8))
        self.assertEqual(quad.swap_dwords(raw,0),raw)
        self.assertEqual(quad.swap_dwords(raw,1),bytes([1,0,3,2,5,4,7,6]))
        self.assertEqual(quad.swap_dwords(raw,2),bytes([3,2,1,0,7,6,5,4]))
        self.assertEqual(quad.swap_dwords(raw,3),bytes([2,3,0,1,6,7,4,5]))
        for e in range(4):self.assertEqual(quad.swap_dwords(quad.swap_dwords(raw,e),e),raw)
    def test_reject_partial_dword(self):
        with self.assertRaises(ValueError):quad.swap_dwords(bytes(3),2)
    def test_homogeneous_fullscreen_quad(self):
        clip=np.array([[2,-2,1,2],[-2,-2,1,2],[-2,2,1,2],[2,2,1,2]],dtype=float)
        screen=quad.project_fullscreen_quad(clip,[1,1,1],[0,0,0],[0,0,2560,1440])
        np.testing.assert_array_equal(screen,[[2560,1440],[0,1440],[0,0],[2560,0]])
        uv=np.array([[319.5/1280,179.5/720],[0,179.5/720],[0,0],[319.5/1280,0]])
        matrix=np.column_stack((screen,np.ones(4)))
        affine=np.linalg.lstsq(matrix,uv,rcond=None)[0]
        np.testing.assert_allclose(matrix@affine,uv,atol=1e-14)
        self.assertLess(float(affine[0,0]*2560),.25)
    def test_reject_unknown_projection(self):
        clip=np.array([[2,-2,1,1],[-2,-2,1,1],[-2,2,1,1],[2,2,1,1]],dtype=float)
        with self.assertRaises(ValueError):quad.project_fullscreen_quad(clip,[1,1,1],[0,0,0],[0,0,2560,1440])
        clip[:,3]=[2,2,2,3]
        with self.assertRaises(ValueError):quad.project_fullscreen_quad(clip,[1,1,1],[0,0,0],[0,0,2560,1440])

    def test_quarter_filter_requires_explicit_bound(self):
        clip=np.array([[-1,1,0,1],[-.5,1,0,1],[-.5,.5,0,1],[-1,.5,0,1]])
        with self.assertRaises(ValueError):
            quad.project_fullscreen_quad(clip,[1,1,1],[0,0,0],[0,0,1280,720])
        screen=quad.project_fullscreen_quad(clip,[1,1,1],[0,0,0],[0,0,1280,720],.25)
        np.testing.assert_array_equal(screen,[[0,0],[320,0],[320,180],[0,180]])
        with self.assertRaises(ValueError):
            quad.project_fullscreen_quad(clip,[1,1,1],[0,0,0],[0,0,1280,720],.5)

if __name__=='__main__':unittest.main()

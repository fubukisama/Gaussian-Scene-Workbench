# Copyright (C) 2023, Inria, GRAPHDECO research group.
# Build recipe adapted under gaussian-splatting/LICENSE.md. No relicensing.
"""Build the unchanged stock full-pinhole kernel with a supported toolchain."""
import os
from setuptools import setup
from torch.utils.cpp_extension import CUDAExtension, BuildExtension

flags = ['-I' + os.path.join(os.getcwd(), 'third_party', 'glm')]
if os.name == 'nt':
    flags.append('-Xcompiler=/Zc:preprocessor')
setup(name='diff_gaussian_rasterization', packages=['diff_gaussian_rasterization'],
      ext_modules=[CUDAExtension(name='diff_gaussian_rasterization._C', sources=[
          'cuda_rasterizer/rasterizer_impl.cu', 'cuda_rasterizer/forward.cu',
          'cuda_rasterizer/backward.cu', 'cuda_rasterizer/adam.cu',
          'rasterize_points.cu', 'conv.cu', 'ext.cpp'], extra_compile_args={'nvcc': flags})],
      cmdclass={'build_ext': BuildExtension})

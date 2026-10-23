# Graphdeco-derived build recipe. Research/evaluation license: see LICENSE.md.
import os
from pathlib import Path
from setuptools import setup
from torch.utils.cpp_extension import CUDAExtension, BuildExtension

glm_root = Path(os.environ['GSW_QUALITY_GLM_ROOT']).resolve()
if not (glm_root / 'glm' / 'glm.hpp').is_file():
    raise RuntimeError('quality_rasterizer_glm_missing')
nvcc = ['-I' + str(glm_root)]
if os.name == 'nt':
    # Use the real supported compiler/header pair; never suppress compatibility checks.
    nvcc += ['-Xcompiler=/Zc:preprocessor']
    host = os.environ.get('CUDAHOSTCXX')
    if host:
        nvcc += ['-ccbin', str(Path(host).parent)]

setup(name='diff_gaussian_rasterization_quality',
      packages=['diff_gaussian_rasterization_quality'],
      ext_modules=[CUDAExtension(name='diff_gaussian_rasterization_quality._C',
          sources=['cuda_rasterizer/rasterizer_impl.cu', 'cuda_rasterizer/forward.cu',
                   'cuda_rasterizer/backward.cu', 'cuda_rasterizer/adam.cu',
                   'rasterize_points.cu', 'conv.cu', 'ext.cpp'],
          extra_compile_args={'nvcc': nvcc})],
      cmdclass={'build_ext': BuildExtension})

from setuptools import setup
from torch.utils.cpp_extension import CUDAExtension, BuildExtension
import os

nvcc_flags = []
if os.name == 'nt':
    nvcc_flags.append("-allow-unsupported-compiler")
    nvcc_flags.append("-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
import os

nvcc_flags = []
if os.name == 'nt':
    nvcc_flags.append("-allow-unsupported-compiler")
    nvcc_flags.append("-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")

setup(
    name="fused_ssim",
    packages=['fused_ssim'],
    ext_modules=[
        CUDAExtension(
            name="fused_ssim_cuda",
            sources=[
            "ssim.cu",
            "ext.cpp"],
            extra_compile_args={"nvcc": nvcc_flags})
        ],
    cmdclass={
        'build_ext': BuildExtension
    }
)

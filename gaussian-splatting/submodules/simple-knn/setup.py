#
# Copyright (C) 2023, Inria
# GRAPHDECO research group, https://team.inria.fr/graphdeco
# All rights reserved.
#
# This software is free for non-commercial, research and evaluation use 
# under the terms of the LICENSE.md file.
#
# For inquiries contact  george.drettakis@inria.fr
#

from setuptools import setup
from torch.utils.cpp_extension import CUDAExtension, BuildExtension
import os

cxx_compiler_flags = []
nvcc_flags = []
nvcc_flags = []

if os.name == 'nt':
    cxx_compiler_flags.append("/wd4624")
    cxx_compiler_flags.append("/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
    nvcc_flags.append("-allow-unsupported-compiler")
    nvcc_flags.append("-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
    cxx_compiler_flags.append("/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")
    nvcc_flags.append("-allow-unsupported-compiler")
    nvcc_flags.append("-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")

setup(
    name="simple_knn",
    ext_modules=[
        CUDAExtension(
            name="simple_knn._C",
            sources=[
            "spatial.cu", 
            "simple_knn.cu",
            "ext.cpp"],
            extra_compile_args={"nvcc": nvcc_flags, "cxx": cxx_compiler_flags})
        ],
    cmdclass={
        'build_ext': BuildExtension
    }
)

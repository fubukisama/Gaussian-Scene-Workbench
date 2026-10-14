# SPDX-FileCopyrightText: 2026 fubukisama
# SPDX-License-Identifier: MIT
from pathlib import Path
from setuptools import setup
from torch.utils.cpp_extension import CUDAExtension, BuildExtension

root = Path(__file__).parent
setup(name="gsw_training_loss_cuda", version="0.2.2", license="MIT",
      license_files=["LICENSE", "README.gsw.md"],
      packages=["gsw_training_loss_cuda"],
      ext_modules=[CUDAExtension("gsw_training_loss_cuda._C",
          sources=[str(root / "bindings.cpp"), str(root / "kernels.cu")],
          extra_compile_args={"cxx": ["/O2", "/std:c++17"],
                              "nvcc": ["-O3", "-std=c++17"]})],
      cmdclass={"build_ext": BuildExtension})

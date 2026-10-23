# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Expose the pinned external surfel renderer's real Z moment, without patches.

The actual rasterizer call and autograd outputs are unchanged. A per-invocation
namespace substitutes only a delegating rasterizer factory; no process-global
state or hybrid/median depth interpretation is used for generic supervision.
"""
import hashlib
from pathlib import Path
import types

from native.worker.surfel_render_adapter import VERIFIED_RENDERER_SOURCES, _code_identity


class SurfelDepthAdapter:
    def __init__(self, render, verified_source_hashes=VERIFIED_RENDERER_SOURCES):
        if not isinstance(render, types.FunctionType):
            raise ValueError('quality_surfel_renderer_unverified')
        source = Path(render.__code__.co_filename).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(source).hexdigest() not in verified_source_hashes:
            raise ValueError('quality_surfel_renderer_unverified')
        compiled = compile(source, render.__code__.co_filename, 'exec', dont_inherit=True)
        candidates = (item for item in compiled.co_consts if isinstance(item, types.CodeType)
                      and item.co_name == render.__name__)
        if not any(_code_identity(item) == _code_identity(render.__code__) for item in candidates):
            raise ValueError('quality_surfel_renderer_unverified')
        if not callable(render.__globals__.get('GaussianRasterizer')):
            raise ValueError('quality_surfel_renderer_unverified')
        self.render, self.code = render, render.__code__

    def __call__(self, *args, **kwargs):
        if self.render.__code__ is not self.code:
            raise ValueError('quality_surfel_renderer_changed')
        captured = []
        original = self.render.__globals__['GaussianRasterizer']

        class Capture:
            def __init__(self, *values, **named):
                self.delegate = original(*values, **named)

            def __call__(self, *values, **named):
                result = self.delegate(*values, **named)
                if not isinstance(result, tuple) or len(result) != 3:
                    raise ValueError('quality_surfel_output_contract_changed')
                captured.append(result[2])
                return result

        namespace = self.render.__globals__.copy()
        namespace['GaussianRasterizer'] = Capture
        function = types.FunctionType(self.code, namespace, self.render.__name__,
                                      self.render.__defaults__, self.render.__closure__)
        function.__kwdefaults__ = self.render.__kwdefaults__
        result = function(*args, **kwargs)
        if (not isinstance(result, dict) or len(captured) != 1 or captured[0].ndim != 3
                or captured[0].shape[0] != 7 or 'rend_alpha' not in result):
            raise ValueError('quality_surfel_output_contract_changed')
        result = dict(result)
        result['rend_depth_moment'] = captured[0][0:1]
        result['depth_definition'] = 'camera_z_moment'
        return result

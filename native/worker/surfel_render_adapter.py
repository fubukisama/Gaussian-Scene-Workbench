# SPDX-FileCopyrightText: 2026 Gaussian Scene Workbench Contributors
# SPDX-License-Identifier: MIT
"""Opt out of an unused surface-normal dependency in a pinned external render.

This module contains no external renderer implementation. The external function
and CUDA rasterizer remain under their original licenses. Only its Python
``depth_to_normal`` dependency is substituted in an isolated function namespace;
the process-global renderer module is never modified. CUDA auxiliary outputs,
depth/distortion maps and every active regularization remain unchanged.
"""
import hashlib
from pathlib import Path
import types


# Official 2DGS renderer as deployed by the verified native runtime. Normalize
# only CRLF so a source-preserving Windows package copy has the same identity.
VERIFIED_RENDERER_SOURCES = frozenset((
    "c27c0f8ba31ae8adf75e56291802ad07b0db1bcea4b94b52c8424098ff65c11a",
))


def _code_identity(code):
    constants = tuple(_code_identity(item) if isinstance(item, types.CodeType) else item
                      for item in code.co_consts)
    return (code.co_code, constants, code.co_names, code.co_varnames,
            code.co_freevars, code.co_cellvars, code.co_argcount,
            getattr(code, "co_posonlyargcount", 0), code.co_kwonlyargcount, code.co_flags)


class _UnrequestedSurfaceNormal:
    """No Tensor allocation, multiplication or autograd for a missing output."""
    def permute(self, *dimensions):
        if dimensions != (2, 0, 1):
            raise RuntimeError("surfel_render_adapter_unexpected_permutation")
        return self

    def __mul__(self, other):
        return self


class SurfelRenderAdapter:
    """A render callable with one explicit ``surface_normal_required`` flag.

    The default is the unmodified external render, including its normal maps.
    An unsupported renderer/source also retains that path even for ``False``.
    ``verified_source_hashes`` is an application-owned pin list, not user input;
    controlled external-renderer dependencies may supply their own test pins.
    Successful omission removes only ``surf_normal`` from the result dictionary.
    Callers must not opt out when a loss, evaluation or preview uses that output.
    """
    def __init__(self, render, verified_source_hashes=VERIFIED_RENDERER_SOURCES):
        if not callable(render):
            raise TypeError("surfel_renderer_not_callable")
        self._render = render
        self._eligible = False
        self._verified_code = None
        self._source_sha256 = None
        self._reason = "surfel_renderer_not_python_function"
        if not isinstance(render, types.FunctionType):
            return
        self._reason = "surfel_renderer_source_unverified"
        try:
            source = Path(render.__code__.co_filename).read_bytes().replace(b"\r\n", b"\n")
            self._source_sha256 = hashlib.sha256(source).hexdigest()
            if self._source_sha256 not in verified_source_hashes:
                return
            compiled = compile(source, render.__code__.co_filename, "exec", dont_inherit=True)
            candidates = (item for item in compiled.co_consts
                          if isinstance(item, types.CodeType) and item.co_name == render.__name__)
            if not any(_code_identity(item) == _code_identity(render.__code__) for item in candidates):
                self._reason = "surfel_renderer_code_unverified"
                return
            if not callable(render.__globals__.get("depth_to_normal")):
                self._reason = "surfel_renderer_dependency_unavailable"
                return
        except (OSError, TypeError, ValueError, SyntaxError):
            return
        self._eligible = True
        self._verified_code = render.__code__
        self._reason = ""

    @property
    def capability(self):
        return {"available": self._eligible, "reason": self._reason,
                "source_sha256": self._source_sha256,
                "optional_output": "surf_normal"}

    def __call__(self, *args, surface_normal_required=True, **kwargs):
        if not isinstance(surface_normal_required, bool):
            raise ValueError("surfel_surface_normal_flag_invalid")
        if self._eligible and self._render.__code__ is not self._verified_code:
            self._eligible = False
            self._reason = "surfel_renderer_code_changed"
        if surface_normal_required or not self._eligible:
            return self._render(*args, **kwargs)
        omitted = _UnrequestedSurfaceNormal()
        namespace = self._render.__globals__.copy()
        namespace["depth_to_normal"] = lambda camera, depth: omitted
        render = types.FunctionType(self._render.__code__, namespace,
                                    self._render.__name__, self._render.__defaults__,
                                    self._render.__closure__)
        render.__kwdefaults__ = self._render.__kwdefaults__
        result = render(*args, **kwargs)
        if not isinstance(result, dict) or result.get("surf_normal") is not omitted:
            raise RuntimeError("surfel_render_adapter_output_contract_changed")
        result = dict(result)
        del result["surf_normal"]
        return result

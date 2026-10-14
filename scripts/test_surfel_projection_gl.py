"""Execute the independent public surfel GLSL module in a headless float FBO.

Supplemental validation, not a CPU equivalent or the real viewport smoke gate.
The default Windows WGL standalone backend creates no visible window.
API references consulted:
  https://moderngl.readthedocs.io/en/latest/topics/context.html
  https://moderngl.readthedocs.io/en/latest/reference/framebuffer.html
  https://github.com/moderngl/glcontext/blob/main/glcontext/wgl.cpp
"""

from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import struct

import moderngl


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "native/src/shaders/SurfelProjection.glsl"
VERTEX = """#version 330 core
void main() {
    vec2 position = gl_VertexID == 0 ? vec2(-1, -1) :
                    gl_VertexID == 1 ? vec2(3, -1) : vec2(-1, 3);
    gl_Position = vec4(position, 0, 1);
}
"""
FRAGMENT_MAIN = """
uniform int operation;
uniform vec3 Tu;
uniform vec3 Tv;
uniform vec3 Tw;
uniform vec2 pixel;
uniform vec2 localUv;
uniform vec2 filterCenter;
uniform vec3 cameraDepth;
uniform mat4 projection;
layout(location = 0) out vec4 values;
layout(location = 1) out vec4 status;
void main() {
    values = vec4(0);
    status = vec4(0);
    if (operation == 0) {
        vec2 center, extent;
        bool valid = surfelBounds(Tu, Tv, Tw, center, extent);
        values = vec4(center, extent);
        status.x = valid ? 1 : 0;
    } else if (operation == 1) {
        vec2 uv;
        float depth;
        bool valid = surfelIntersection(pixel, Tu, Tv, Tw, uv, depth);
        values = vec4(uv, depth, 0);
        status.x = valid ? 1 : 0;
    } else if (operation == 2) {
        values.x = surfelPower(localUv, pixel, filterCenter);
        status.x = 1;
    } else if (operation == 3) {
        values.x = surfelFragmentDepth(localUv, cameraDepth, projection);
        status.x = 1;
    }
}
"""


def close(actual: float, expected: float, tolerance: float) -> None:
    assert math.isfinite(actual), (actual, expected)
    assert abs(actual - expected) <= tolerance, (actual, expected, tolerance)


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    library = os.environ.get("QT_OPENGL_DLL")
    context = moderngl.create_context(standalone=True, require=330,
                                     **({"libgl": library} if library else {}))
    assert context.screen is None, "Validation must not target a visible window."
    print(json.dumps({
        "gl_version": context.version_code,
        "renderer": context.info["GL_RENDERER"],
        "source_sha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
        "standalone": True,
    }))
    program = context.program(vertex_shader=VERTEX,
                              fragment_shader="#version 330 core\n" + source + FRAGMENT_MAIN)
    attachments = [context.texture((1, 1), 4, dtype="f4") for _ in range(2)]
    framebuffer = context.framebuffer(color_attachments=attachments)
    vertices = context.vertex_array(program, [])
    framebuffer.use()
    defaults = {
        "operation": 0,
        "Tu": (10.0, 0.0, 400.0),
        "Tv": (0.0, 20.0, 300.0),
        "Tw": (0.0, 0.0, 1.0),
        "pixel": (420.0, 280.0),
        "localUv": (0.0, 0.0),
        "filterCenter": (0.0, 0.0),
        "cameraDepth": (0.0, 0.0, 6.0),
    }
    perspective = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.2, -1, 0, 0, -2.2, 0)
    orthographic = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -0.2, 0, 0, 0, -1.2, 1)
    count = 0

    def execute(name: str, *, expected=None, tolerance=1e-4,
                valid=True, matrix=perspective, finite_only=False, **inputs):
        nonlocal count
        for uniform, value in {**defaults, **inputs}.items():
            program[uniform].value = value
        program["projection"].write(struct.pack("16f", *matrix))
        framebuffer.clear()
        vertices.render(mode=moderngl.TRIANGLES, vertices=3)
        values = struct.unpack("4f", framebuffer.read(components=4, attachment=0, dtype="f4"))
        status = struct.unpack("4f", framebuffer.read(components=4, attachment=1, dtype="f4"))
        print(json.dumps({"case": name, "values": values, "valid": bool(status[0])}))
        assert status[0] == (1.0 if valid else 0.0), (name, "validity", status, values)
        if expected is not None:
            for actual, wanted in zip(values, expected):
                close(actual, wanted, tolerance)
        if finite_only:
            assert all(math.isfinite(value) for value in values), (name, values)
        count += 1
        return values

    try:
        # Expectations are independent algebraic fixtures from the published
        # projection contract; these values are emitted by actual GPU execution.
        execute("orthographic_bounds", expected=(400, 300, 30, 60), tolerance=0.002)
        execute("tilted_bounds", Tu=(50, 0, 400), Tv=(30, 20, 300), Tw=(0.1, 0, 1),
                expected=(390.10989011, 300, 32.96703297, 62.89709020), tolerance=0.003)
        execute("horizon_crossing", Tu=(170, 0, 400), Tv=(120, 20, 300),
                Tw=(0.4, 0, 1), valid=False)
        execute("orthographic_intersection", operation=1, expected=(2, -1, 1))
        execute("tilted_intersection", operation=1, Tu=(50, 0, 400), Tv=(30, 20, 300),
                Tw=(0.1, 0, 1), pixel=(409.09090909, 336.36363636),
                expected=(1, 2, 1.1), tolerance=0.0002)
        execute("screen_filter_power", operation=2, localUv=(10, 0),
                pixel=(0.5, 0), expected=(-0.25,))
        execute("perspective_depth", operation=3, expected=(11 / 12,), tolerance=1e-6)
        execute("orthographic_depth", operation=3, matrix=orthographic,
                expected=(0.5,), tolerance=1e-6)
        execute("singular_homography", operation=1, Tu=(1, 0, 400), Tv=(2, 0, 300),
                pixel=(400, 300), valid=False)
        execute("ill_conditioned_homography", operation=1, Tu=(1, 1, 400),
                Tv=(1, 1.0000001, 300), pixel=(400, 300), valid=False)
        for scale in (1e-12, 1e12):
            value = execute(f"common_scale_intersection_{scale:g}", operation=1,
                            Tu=(10 * scale, 0, 400 * scale),
                            Tv=(0, 20 * scale, 300 * scale), Tw=(0, 0, scale),
                            finite_only=True)
            close(value[0], 2, 0.0002)
            close(value[1], -1, 0.0002)
            assert abs(value[2] / scale - 1) <= 1e-5, value
        execute("tiny_bounds_have_filter_support", Tu=(1e-12, 0, 400),
                Tv=(0, 2e-12, 300), expected=(400, 300, math.sqrt(4.5), math.sqrt(4.5)),
                tolerance=0.002)
        execute("negative_camera_depth", operation=3, cameraDepth=(0, 0, -6), expected=(-1,))
        execute("nonpositive_homography_denominator", operation=1, Tw=(0, 0, -1), valid=False)
        execute("nonfinite_bounds", Tu=(math.nan, 0, 400), valid=False)
        near_horizon = execute("finite_near_horizon_bounds", Tu=(143.3328, 0, 400),
                               Tv=(99.9996, 20, 300), Tw=(0.33332, 0, 1), finite_only=True)
        assert near_horizon[2] >= math.sqrt(4.5) and near_horizon[3] >= math.sqrt(4.5)
        execute("tiny_tangent_at_center", operation=1, Tu=(1e-12, 0, 400),
                Tv=(0, 2e-12, 300), pixel=(400, 300), expected=(0, 0, 1))
        execute("overflow_subtraction_intersection", operation=1,
                Tu=(1e38, 0, 1e38), Tv=(0, 1e38, 1e38), Tw=(0, 0, 1e38),
                pixel=(-3, -3), expected=(-4, -4), tolerance=1e-6)
        print(json.dumps({"passed_gpu_cases": count, "actual_glsl_execution": True}))
    finally:
        vertices.release()
        framebuffer.release()
        for attachment in attachments:
            attachment.release()
        program.release()
        context.release()


if __name__ == "__main__":
    main()


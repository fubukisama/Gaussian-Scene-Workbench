/*
 * Copyright (C) 2023, Inria
 * GRAPHDECO research group, https://team.inria.fr/graphdeco
 * All rights reserved.
 *
 * This software is free for non-commercial, research and evaluation use
 * under the terms of the LICENSE.md file.
 *
 * For inquiries contact  george.drettakis@inria.fr
 */
// GLSL adaptation of compute_aabb and renderCUDA ray/surfel intersection,
// hbb1/diff-surfel-rasterization, commit e0ed0207b3e0669960cfad70852200a4a5847f61.
// Changes: OpenGL pixel centers, orthographic support, finite/pole guards.
// Complete license: native/worker/licenses/2dgs-LICENSE.md (packaged alongside).

bool surfelBounds(vec3 Tu, vec3 Tv, vec3 Tw, out vec2 center, out vec2 extent) {
  vec3 t = vec3(9.0, 9.0, -1.0);
  float d = dot(t, Tw * Tw);
  // A support ellipse crossing the projection pole is unbounded. The caller
  // clips its raster rectangle to the viewport; never flatten it to an ellipse.
  if (d >= 0.0) return false;
  vec3 f = t / d;
  center = vec2(dot(f, Tu * Tw), dot(f, Tv * Tw));
  vec2 h0 = center * center - vec2(dot(f, Tu * Tu), dot(f, Tv * Tv));
  extent = max(sqrt(max(vec2(1e-4), h0)), vec2(3.0 * 0.70710678118));
  return !any(isnan(center)) && !any(isinf(center)) &&
         !any(isnan(extent)) && !any(isinf(extent));
}

bool surfelIntersection(vec2 pixel, vec3 Tu, vec3 Tv, vec3 Tw,
                        out vec2 uv, out float depth) {
  vec3 k = pixel.x * Tw - Tu;
  vec3 l = pixel.y * Tw - Tv;
  vec3 p = cross(k, l);
  if (p.z == 0.0) return false;
  uv = p.xy / p.z;
  depth = dot(vec3(uv, 1.0), Tw);
  return depth > 0.0 && !any(isnan(uv)) && !any(isinf(uv));
}

float surfelPower(vec2 uv, vec2 pixel, vec2 filterCenter) {
  vec2 d = filterCenter - pixel;
  return -0.5 * min(dot(uv, uv), 2.0 * dot(d, d));
}

// Original native integration: camera depth is affine on the tangent plane,
// even in orthographic projection (where Tw is constant). Test against opaque
// geometry at the intersection, not at the surfel center. No depth is written
// to the buffer during transparent Gaussian compositing.
float surfelFragmentDepth(vec2 uv, vec3 cameraDepth, mat4 projection) {
  float depth = dot(vec3(uv, 1.0), cameraDepth);
  if (depth <= 0.0) return -1.0;
  vec4 clip = projection * vec4(0.0, 0.0, -depth, 1.0);
  return 0.5 * (clip.z / clip.w + 1.0);
}

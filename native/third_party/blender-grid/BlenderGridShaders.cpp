/* SPDX-FileCopyrightText: 2017-2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BlenderGridShaders.h"

namespace gsw::blender_grid {

QByteArray vertexShader() {
  // The full-screen ray reconstruction is the GSW adapter, not Blender's finite
  // multi-triangle vertex shader. It avoids interpolating large world positions.
  return QByteArrayLiteral(R"GLSL(#version 130
out vec2 screenPosition;
void main() {
  vec2 vertex = gl_VertexID == 0 ? vec2(-1.0, -1.0) :
                (gl_VertexID == 1 ? vec2(3.0, -1.0) : vec2(-1.0, 3.0));
  screenPosition = vertex;
  gl_Position = vec4(vertex, 0.0, 1.0);
}
)GLSL");
}

QByteArray fragmentShader() {
  // Adapted actual Blender 4.5 overlay_grid_frag.glsl and LINE_STEP. See the
  // retained upstream files and exact hashes in README.gsw.md. Camera-relative
  // ray reconstruction, per-level phase uniforms and GL130 plumbing are GSW's.
  return QByteArrayLiteral(R"GLSL(#version 130
in vec2 screenPosition;
out vec4 fragColor;

uniform mat4 inverseProjection;
uniform mat3 inverseViewRotation;
uniform mat3 planeBasis;
uniform vec3 cameraPlanePosition;
uniform float gridSteps[8];
uniform vec2 gridPhase[8];
uniform float gridDistance;
uniform float gridLineSize;
uniform int isOrthographic;
uniform int showGrid;
uniform int showAxes;
uniform vec4 gridColor;
uniform vec4 gridEmphasisColor;
uniform vec4 axisUColor;
uniform vec4 axisVColor;

// overlay_shader_shared.hh: the disc-filtered coverage transition retained
// from Blender. gridLineSize is max(physical-pixel scale - 1, 0) / 2.
#define DISC_RADIUS (0.5641895835477563 * 1.05)
#define LINE_SMOOTH_START (0.5 - DISC_RADIUS)
#define LINE_SMOOTH_END (0.5 + DISC_RADIUS)
#define LINE_STEP(dist) smoothstep(LINE_SMOOTH_START, LINE_SMOOTH_END, dist)

float linearstep(float p0, float p1, float value) {
  return clamp((value - p0) / max(abs(p1 - p0), 1e-30), 0.0, 1.0);
}

float get_grid(vec2 co, vec2 fwidthCos, int level) {
  vec2 scale = vec2(gridSteps[level]);
  vec2 halfSize = scale / 2.0;
  // Blender's triangular wave, split into two modulo operations so an absolute
  // camera coordinate (for example 1e8) never destroys a nearby small-cell phase.
  vec2 domain = abs(mod(mod(co, scale) + gridPhase[level] + halfSize, scale) - halfSize);
  domain /= fwidthCos;
  float lineDistance = min(domain.x, domain.y);
  return 1.0 - LINE_STEP(lineDistance - gridLineSize);
}

vec2 get_axes(vec2 co, vec2 fwidthCos) {
  return vec2(1.0) - LINE_STEP(abs(co) / fwidthCos - (0.1 + gridLineSize));
}

void main() {
  // All intersections are camera-relative. There is no inverse combined
  // view-projection translation, so a large source/world origin is not fed
  // through a homogeneous float unprojection.
  vec4 nearH = inverseProjection * vec4(screenPosition, -1.0, 1.0);
  vec3 nearView = nearH.xyz / nearH.w;
  mat3 viewToPlane = transpose(planeBasis) * inverseViewRotation;
  // Orthographic rays begin on the eye's XY plane, not at the model near clip.
  // The reference plane must not disappear because model clipping changed.
  vec3 origin = isOrthographic != 0 ?
      viewToPlane * vec3(nearView.xy, 0.0) : vec3(0.0);
  vec3 direction = normalize(viewToPlane *
      (isOrthographic != 0 ? vec3(0.0, 0.0, -1.0) : nearView));
  float denominator = direction.z;
  float safeDenominator = abs(denominator) >= 1e-7 ? denominator :
                          (denominator < 0.0 ? -1e-7 : 1e-7);
  float rayDistance = -(cameraPlanePosition.z + origin.z) / safeDenominator;
  vec3 relativeHit = origin + direction * rayDistance;
  relativeHit.z = -cameraPlanePosition.z;
  // Derivatives are evaluated before fragment-dependent clipping/discard.
  vec3 dx = dFdx(relativeHit);
  vec3 dy = dFdy(relativeHit);
  vec2 fwidthPosition = max(abs(dx.xy) + abs(dy.xy), vec2(1e-12));
  if (!(abs(denominator) >= 1e-7 && rayDistance >= 0.0)) {
    discard;
  }

  // Blender's quartic grazing-angle fade and perspective-distance fade. Applying the
  // angle in the active plane's basis extends its XY orthographic behavior to
  // XZ/YZ planes without changing camera/navigation or source coordinates.
  float angle = 1.0 - abs(direction.z);
  angle *= angle;
  float fade = 1.0 - angle * angle;
  if (isOrthographic == 0) {
    float distanceToCamera = length(relativeHit);
    fade *= 1.0 - smoothstep(0.0, max(gridDistance, 1e-12),
                             distanceToCamera - gridDistance);
  }

  vec4 color = vec4(gridColor.rgb, 0.0);
  if (showGrid != 0) {
    // Upstream notes the max of the two projected derivatives is more accurate
    // than using x alone. This also treats rolled orthographic views uniformly.
    float resolution = max(abs(dot(dx, viewToPlane[0])),
                           abs(dot(dy, viewToPlane[1]))) * 4.0;
    int level = 7;
    for (int i = 6; i >= 0; --i) {
      level = resolution < gridSteps[i] ? i : level;
    }
    int nextLevel = min(level + 1, 7);
    int emphasisLevel = min(level + 2, 7);
    float scale0 = level > 0 ? gridSteps[level - 1] : 0.0;
    float blend = 1.0 - linearstep(scale0, gridSteps[level], resolution);
    blend = blend * blend * blend;
    float gridA = get_grid(relativeHit.xy, fwidthPosition, level);
    float gridB = get_grid(relativeHit.xy, fwidthPosition, nextLevel);
    float gridC = get_grid(relativeHit.xy, fwidthPosition, emphasisLevel);
    color = gridColor;
    color.a *= gridA * blend;
    color = mix(color, mix(gridColor, gridEmphasisColor, blend), gridB);
    color = mix(color, gridEmphasisColor, gridC);
  }
  if (showAxes != 0) {
    // Absolute coordinates are used only for the two nonperiodic origin axes.
    // The periodic grid above never performs this addition.
    vec2 originCoordinates = relativeHit.xy + cameraPlanePosition.xy;
    vec2 axes = get_axes(originCoordinates.yx, fwidthPosition.yx);
    if (axes.x > 1e-8) {
      color.a = max(color.a, axes.x * axisUColor.a);
      color.rgb = axisUColor.rgb;
    }
    if (axes.y > 1e-8) {
      color.a = max(color.a, axes.y * axisVColor.a);
      color.rgb = axisVColor.rgb;
    }
  }
  color.a *= fade;
  fragColor = color;
}
)GLSL");
}

} // namespace gsw::blender_grid

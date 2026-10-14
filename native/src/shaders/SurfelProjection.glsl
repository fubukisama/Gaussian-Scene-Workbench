// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Gaussian Scene Workbench contributors
// Independently derived from published 2DGS mathematics; see SurfelProjection.md.
// This include is appended after the caller's GLSL version directive.

bool surfelFinite(float value) {
    return !isnan(value) && !isinf(value);
}

bool surfelFinite(vec2 value) {
    return !any(isnan(value)) && !any(isinf(value));
}

bool surfelFinite(vec3 value) {
    return !any(isnan(value)) && !any(isinf(value));
}

float surfelMaxAbs(vec3 value) {
    vec3 magnitude = abs(value);
    return max(magnitude.x, max(magnitude.y, magnitude.z));
}

// Tangency of (n.xy-q*w.xy).uv+n.z-q*w.z=0 to |uv|=3
// gives a quadratic in q. Shift by the local origin's projection first.
bool surfelAxisInterval(vec3 numerator, vec2 horizon, float denominator,
                        float quadraticA, out vec2 interval) {
    float origin = numerator.z / denominator;
    vec2 gradient = numerator.xy / denominator - origin * horizon;
    if (!surfelFinite(origin) || !surfelFinite(gradient)) return false;
    float scale = max(abs(gradient.x), abs(gradient.y));
    vec2 offset = vec2(0.0);
    if (scale > 0.0) {
        vec2 d = gradient / scale;
        float B = 18.0 * dot(d, horizon);
        float C = -9.0 * dot(d, d);
        float discriminant = B * B - 4.0 * quadraticA * C;
        float root = -0.5 * (B + (B < 0.0 ? -1.0 : 1.0) * sqrt(discriminant));
        offset = scale * vec2(root / quadraticA, C / root);
    }
    float filterRadius = sqrt(4.5);
    // A small outward allowance covers float rounding, amplified near the
    // projective horizon. This remains subpixel for ordinary viewport bounds.
    float magnitude = abs(origin) + scale + max(abs(offset.x), abs(offset.y));
    float padding = (1e-6 / quadraticA) * max(magnitude, 1.0);
    interval = origin + vec2(min(min(offset.x, offset.y), -filterRadius),
                             max(max(offset.x, offset.y), filterRadius));
    interval += vec2(-padding, padding);
    return surfelFinite(interval);
}

bool surfelBounds(vec3 Tu, vec3 Tv, vec3 Tw,
                  out vec2 center, out vec2 extent) {
    center = vec2(0.0);
    extent = vec2(0.0);
    if (!surfelFinite(Tu) || !surfelFinite(Tv) || !surfelFinite(Tw) ||
        Tw.z <= 0.0) return false;
    vec2 horizon = Tw.xy / Tw.z;
    if (!surfelFinite(horizon)) return false;
    float denominatorRadius = 3.0 * length(horizon);
    if (!surfelFinite(denominatorRadius) || denominatorRadius >= 1.0 - 1e-6)
        return false;
    float A = 1.0 - 9.0 * dot(horizon, horizon);
    vec2 horizontal, vertical;
    if (!surfelAxisInterval(Tu, horizon, Tw.z, A, horizontal) ||
        !surfelAxisInterval(Tv, horizon, Tw.z, A, vertical)) return false;
    vec2 lower = vec2(horizontal.x, vertical.x);
    vec2 upper = vec2(horizontal.y, vertical.y);
    center = 0.5 * lower + 0.5 * upper;
    extent = max(0.5 * upper - 0.5 * lower, vec2(sqrt(4.5)));
    return surfelFinite(center) && surfelFinite(extent);
}

bool surfelIntersection(vec2 pixel, vec3 Tu, vec3 Tv, vec3 Tw,
                        out vec2 uv, out float depth) {
    uv = vec2(0.0);
    depth = -1.0;
    if (!surfelFinite(pixel) || !surfelFinite(Tu) || !surfelFinite(Tv) ||
        !surfelFinite(Tw)) return false;
    // Subtract before rescaling: dividing a large screen-center constant
    // first can turn an exact center intersection into a residual that is
    // enormous relative to a subpixel tangent. Normalize the equations below.
    vec3 a = Tu - pixel.x * Tw;
    vec3 b = Tv - pixel.y * Tw;
    if (!surfelFinite(a) || !surfelFinite(b)) {
        // Finite inputs can still overflow during the multiply/subtract.
        float transformScale = max(surfelMaxAbs(Tu),
                                   max(surfelMaxAbs(Tv), surfelMaxAbs(Tw)));
        if (transformScale <= 0.0) return false;
        // Split division so reciprocal-based GPU division does not flush
        // 1/scale to zero when scale is close to the largest finite float.
        float normalizationRoot = sqrt(transformScale);
        vec3 scaledTu = (Tu / normalizationRoot) / normalizationRoot;
        vec3 scaledTv = (Tv / normalizationRoot) / normalizationRoot;
        vec3 scaledTw = (Tw / normalizationRoot) / normalizationRoot;
        a = scaledTu - pixel.x * scaledTw;
        b = scaledTv - pixel.y * scaledTw;
    }
    float rowA = max(abs(a.x), abs(a.y));
    float rowB = max(abs(b.x), abs(b.y));
    if (!surfelFinite(a) || !surfelFinite(b) || rowA <= 0.0 || rowB <= 0.0)
        return false;
    a /= rowA;
    b /= rowB;
    if (!surfelFinite(a) || !surfelFinite(b)) return false;
    float determinant = a.x * b.y - a.y * b.x;
    float determinantScale = abs(a.x * b.y) + abs(a.y * b.x);
    if (abs(determinant) <= 1e-7 * determinantScale) return false;
    uv = vec2(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z) / determinant;
    if (!surfelFinite(uv)) return false;
    depth = dot(Tw, vec3(uv, 1.0));
    return surfelFinite(depth) && depth > 0.0;
}

float surfelPower(vec2 uv, vec2 pixel, vec2 filterCenter) {
    if (!surfelFinite(uv) || !surfelFinite(pixel) || !surfelFinite(filterCenter))
        return -1e30;
    vec2 offset = pixel - filterCenter;
    return -0.5 * min(dot(uv, uv), 2.0 * dot(offset, offset));
}

float surfelFragmentDepth(vec2 uv, vec3 cameraDepth, mat4 projection) {
    if (!surfelFinite(uv) || !surfelFinite(cameraDepth)) return -1.0;
    float distance = dot(cameraDepth, vec3(uv, 1.0));
    if (!surfelFinite(distance) || distance <= 0.0) return -1.0;
    // The supported perspective/orthographic matrices have depth rows that
    // depend only on eye Z and the homogeneous constant, not eye X/Y.
    if (projection[0][2] != 0.0 || projection[1][2] != 0.0 ||
        projection[0][3] != 0.0 || projection[1][3] != 0.0) return -1.0;
    float clipZ = -distance * projection[2][2] + projection[3][2];
    float clipW = -distance * projection[2][3] + projection[3][3];
    if (!surfelFinite(vec2(clipZ, clipW)) || clipW <= 0.0) return -1.0;
    float result = 0.5 * (clipZ / clipW) + 0.5;
    return surfelFinite(result) && result >= 0.0 && result <= 1.0 ? result : -1.0;
}

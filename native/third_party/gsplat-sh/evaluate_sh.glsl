/*
 * SPDX-FileCopyrightText: Copyright 2025 the Regents of the University of California, Nerfstudio Team and contributors. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified for GSW: GLSL 3.30 / RGB texture buffers, zero-direction guard,
 * degree cap and display bias/lower clamp. Adapted from gsplat's
 * sh_coeffs_to_color_fast in gsplat/cuda/csrc/SphericalHarmonicsCUDA.cu,
 * commit 512d366b67073d77ca099ede742683c165dfc23b. See README.gsw.md.
 */
uniform samplerBuffer gaussianSh;
uniform int shDegree;
uniform int shCoefficientCount;
uniform vec3 shCameraPosition;
uniform vec3 shParallelDirection;
uniform bool shOrthographic;

vec3 evaluateSh(int index, vec3 dir) {
    int base = index * shCoefficientCount;
    vec3 result = 0.2820947917738781 * texelFetch(gaussianSh, base).rgb;
    if (shDegree >= 1 && dot(dir, dir) > 1e-16) {
        vec3 n = normalize(dir);
        float x = n.x, y = n.y, z = n.z;
        result += 0.48860251190292 *
            (-y * texelFetch(gaussianSh, base + 1).rgb +
              z * texelFetch(gaussianSh, base + 2).rgb -
              x * texelFetch(gaussianSh, base + 3).rgb);
        if (shDegree >= 2) {
            float z2 = z * z;
            float fTmp0B = -1.092548430592079 * z;
            float fC1 = x * x - y * y;
            float fS1 = 2.0 * x * y;
            float pSH6 = 0.9461746957575601 * z2 - 0.3153915652525201;
            result += 0.5462742152960395 * fS1 * texelFetch(gaussianSh, base + 4).rgb
                + fTmp0B * y * texelFetch(gaussianSh, base + 5).rgb
                + pSH6 * texelFetch(gaussianSh, base + 6).rgb
                + fTmp0B * x * texelFetch(gaussianSh, base + 7).rgb
                + 0.5462742152960395 * fC1 * texelFetch(gaussianSh, base + 8).rgb;
            if (shDegree >= 3) {
                float fTmp0C = -2.285228997322329 * z2 + 0.4570457994644658;
                float fTmp1B = 1.445305721320277 * z;
                float fC2 = x * fC1 - y * fS1;
                float fS2 = x * fS1 + y * fC1;
                float pSH12 = z * (1.865881662950577 * z2 - 1.119528997770346);
                result += -0.5900435899266435 * fS2 * texelFetch(gaussianSh, base + 9).rgb
                    + fTmp1B * fS1 * texelFetch(gaussianSh, base + 10).rgb
                    + fTmp0C * y * texelFetch(gaussianSh, base + 11).rgb
                    + pSH12 * texelFetch(gaussianSh, base + 12).rgb
                    + fTmp0C * x * texelFetch(gaussianSh, base + 13).rgb
                    + fTmp1B * fC1 * texelFetch(gaussianSh, base + 14).rgb
                    - 0.5900435899266435 * fC2 * texelFetch(gaussianSh, base + 15).rgb;
                if (shDegree >= 4) {
                    float fTmp0D = z * (-4.683325804901025 * z2 + 2.007139630671868);
                    float fTmp1C = 3.31161143515146 * z2 - 0.47308734787878;
                    float fTmp2B = -1.770130769779931 * z;
                    float fC3 = x * fC2 - y * fS2;
                    float fS3 = x * fS2 + y * fC2;
                    float pSH20 = 1.984313483298443 * z * pSH12 - 1.006230589874905 * pSH6;
                    result += 0.6258357354491763 * fS3 * texelFetch(gaussianSh, base + 16).rgb
                        + fTmp2B * fS2 * texelFetch(gaussianSh, base + 17).rgb
                        + fTmp1C * fS1 * texelFetch(gaussianSh, base + 18).rgb
                        + fTmp0D * y * texelFetch(gaussianSh, base + 19).rgb
                        + pSH20 * texelFetch(gaussianSh, base + 20).rgb
                        + fTmp0D * x * texelFetch(gaussianSh, base + 21).rgb
                        + fTmp1C * fC1 * texelFetch(gaussianSh, base + 22).rgb
                        + fTmp2B * fC2 * texelFetch(gaussianSh, base + 23).rgb
                        + 0.6258357354491763 * fC3 * texelFetch(gaussianSh, base + 24).rgb;
                }
            }
        }
    }
    // Do not clamp DC before adding the directional terms.
    return max(result + vec3(0.5), vec3(0.0));
}

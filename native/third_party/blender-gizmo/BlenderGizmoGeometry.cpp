/* SPDX-FileCopyrightText: 2014 Blender Authors
 * SPDX-FileCopyrightText: 2016 Blender Authors
 * SPDX-FileCopyrightText: 2023 Blender Authors
 * SPDX-FileCopyrightText: 2026 fubukisama
 * SPDX-License-Identifier: GPL-2.0-or-later */

// Adapted from Blender v4.5.0:
// source/blender/gpu/intern/gpu_immediate_util.cc, imm_draw_cylinder_fill_3d
// source/blender/editors/gizmo_library/geometry/geom_cube_gizmo.cc
// See README.gsw.md for pinned sources, modification notice and license.
// Changes: Qt value geometry instead of Blender GPU immediate calls; one cone
// stack with eight slices, degenerate tip triangles omitted; closed base cap.

#include "BlenderGizmoGeometry.h"

#include <array>
#include <cmath>
#include <numbers>

namespace gsw {

QList<QList<QVector3D>> blenderGizmoCone(const float radius, const float height) {
  constexpr int slices = 8;
  QList<QList<QVector3D>> faces;
  QList<QVector3D> cap;
  for (int i = 0; i < slices; i++) {
    const float angle1 = 2.0F * std::numbers::pi_v<float> * (float(i) / float(slices));
    const float angle2 = 2.0F * std::numbers::pi_v<float> * (float(i + 1) / float(slices));
    const float cos1 = std::cos(angle1);
    const float sin1 = std::sin(angle1);
    const float cos2 = std::cos(angle2);
    const float sin2 = std::sin(angle2);
    const QVector3D v1(radius * cos2, radius * sin2, 0);
    const QVector3D v3(0, 0, height);
    const QVector3D v4(radius * cos1, radius * sin1, 0);
    // Blender's top-zero first triangle is degenerate; its second triangle
    // supplies the cone wall with the original outward vertex winding.
    faces.append({v3, v4, v1});
    cap.prepend(v4);
  }
  faces.append(cap);
  return faces;
}

QList<QList<QVector3D>> blenderGizmoCube(const float halfSize) {
  // Original Blender cube vertex/index data. Normals are recomputed per face
  // by GSW because its painter does not use interpolated vertex lighting.
  static const float verts[][3] = {
      {1.000000F, 1.000000F, -1.000000F},
      {1.000000F, -1.000000F, -1.000000F},
      {-1.000000F, -1.000000F, -1.000000F},
      {-1.000000F, 1.000000F, -1.000000F},
      {1.000000F, 1.000000F, 1.000000F},
      {0.999999F, -1.000001F, 1.000000F},
      {-1.000000F, -1.000000F, 1.000000F},
      {-1.000000F, 1.000000F, 1.000000F},
  };
  static const unsigned short indices[] = {
      1, 2, 3, 7, 6, 5, 4, 5, 1, 5, 6, 2, 2, 6, 7, 0, 3, 7,
      0, 1, 3, 4, 7, 5, 0, 4, 1, 1, 5, 2, 3, 2, 7, 4, 0, 7,
  };
  QList<QList<QVector3D>> faces;
  for (std::size_t triangle = 0; triangle < std::size(indices); triangle += 3) {
    QList<QVector3D> face;
    for (std::size_t vertex = 0; vertex < 3; ++vertex) {
      const auto &position = verts[indices[triangle + vertex]];
      face.append(QVector3D(position[0], position[1], position[2]) * halfSize);
    }
    faces.append(face);
  }
  return faces;
}

} // namespace gsw

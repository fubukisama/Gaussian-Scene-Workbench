#include "ReferenceAxisGeometry.h"

#include <QPointF>
#include <QRectF>
#include <QVector4D>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace gsw {
namespace {
constexpr float kPi = 3.14159265F;
const QVector4D kOutline(0.025F, 0.035F, 0.045F, 1.0F);

struct ScreenPoint {
  QPointF pixel;
  float depth;
};

std::optional<ScreenPoint> project(const QVector4D &clip,
                                   const QSizeF &viewport) {
  if (!std::isfinite(clip.w()) || clip.w() <= 1.0e-7F) {
    return std::nullopt;
  }
  const QVector3D ndc = clip.toVector3DAffine();
  if (!std::isfinite(ndc.x()) || !std::isfinite(ndc.y()) ||
      !std::isfinite(ndc.z()) || ndc.z() <= -1.0F || ndc.z() >= 1.0F) {
    return std::nullopt;
  }
  return ScreenPoint{QPointF((ndc.x() * 0.5 + 0.5) * viewport.width(),
                             (0.5 - ndc.y() * 0.5) * viewport.height()),
                      ndc.z()};
}

class GeometryBuilder {
public:
  explicit GeometryBuilder(QSizeF viewport) : mViewport(viewport) {
    vertices.reserve(768);
  }

  void triangle(const ScreenPoint &a, const ScreenPoint &b,
                const ScreenPoint &c, const QVector4D &color) {
    append(a, color);
    append(b, color);
    append(c, color);
  }

  void stroke(const ScreenPoint &a, const ScreenPoint &b, const float width,
              const QVector4D &color) {
    const QPointF delta = b.pixel - a.pixel;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (length < 0.01) {
      return;
    }
    const QPointF side(-delta.y() * width / (length * 2.0),
                        delta.x() * width / (length * 2.0));
    const ScreenPoint a1{a.pixel + side, a.depth};
    const ScreenPoint a2{a.pixel - side, a.depth};
    const ScreenPoint b1{b.pixel + side, b.depth};
    const ScreenPoint b2{b.pixel - side, b.depth};
    triangle(a1, a2, b1, color);
    triangle(b1, a2, b2, color);
  }

  void disc(const ScreenPoint &center, float radius,
            const QVector4D &color) {
    constexpr int segments = 32;
    for (int i = 0; i < segments; ++i) {
      const float a = i * (2.0F * kPi / segments);
      const float b = (i + 1) * (2.0F * kPi / segments);
      triangle(center,
               {center.pixel + QPointF(std::cos(a), std::sin(a)) * radius,
                center.depth},
               {center.pixel + QPointF(std::cos(b), std::sin(b)) * radius,
                center.depth},
               color);
    }
  }

  void letter(const int axis, const ScreenPoint &center, const float scale,
              const QVector4D &color) {
    // Small upright technical lettering, tessellated with the same depth as
    // the arrow tip. It cannot float over an occluding model like QPainter text.
    const auto line = [&](const float x1, const float y1, const float x2,
                          const float y2, const float width,
                          const QVector4D &lineColor) {
      stroke({center.pixel + QPointF(x1, y1) * scale, center.depth},
             {center.pixel + QPointF(x2, y2) * scale, center.depth},
             width * scale, lineColor);
    };
    for (const bool outline : {true, false}) {
      const float width = outline ? 3.5F : 1.65F;
      const QVector4D ink = outline ? kOutline : color;
      if (axis == 0) {
        line(-3.2F, -4.4F, 3.2F, 4.4F, width, ink);
        line(3.2F, -4.4F, -3.2F, 4.4F, width, ink);
      } else if (axis == 1) {
        line(-3.3F, -4.4F, 0.0F, 0.0F, width, ink);
        line(3.3F, -4.4F, 0.0F, 0.0F, width, ink);
        line(0.0F, 0.0F, 0.0F, 4.4F, width, ink);
      } else {
        line(-3.3F, -4.4F, 3.3F, -4.4F, width, ink);
        line(3.3F, -4.4F, -3.3F, 4.4F, width, ink);
        line(-3.3F, 4.4F, 3.3F, 4.4F, width, ink);
      }
    }
  }

  QVector<ReferenceAxisVertex> vertices;

private:
  void append(const ScreenPoint &point, const QVector4D &color) {
    vertices.append({static_cast<float>(point.pixel.x() * 2.0 /
                                            mViewport.width() - 1.0),
                     static_cast<float>(1.0 - point.pixel.y() * 2.0 /
                                                  mViewport.height()),
                     point.depth, color.x(), color.y(), color.z(), color.w()});
  }
  QSizeF mViewport;
};
} // namespace

QVector<ReferenceAxisVertex>
referenceAxisGeometry(const QMatrix4x4 &viewProjection,
                      const QVector3D &origin, const QSizeF &viewport,
                      const float uiScale) {
  if (viewport.width() < 64.0 || viewport.height() < 64.0) {
    return {};
  }
  const float scale = std::isfinite(uiScale)
                          ? std::clamp(uiScale, 0.85F, 1.4F)
                          : 1.0F;
  const QVector4D originClip = viewProjection * QVector4D(origin, 1.0F);
  const auto center = project(originClip, viewport);
  if (!center || !QRectF(QPointF(), viewport).contains(center->pixel)) {
    return {};
  }

  // The projection derivative gives pixels/world-unit without subtracting
  // nearly coincident float coordinates when the view is very far away.
  double pixelsPerUnit = 0.0;
  for (int axis = 0; axis < 3; ++axis) {
    const QVector4D column = viewProjection.column(axis);
    const double w = originClip.w();
    const double dx = (column.x() * w - originClip.x() * column.w()) /
                      (w * w) * viewport.width() * 0.5;
    const double dy = (column.y() * w - originClip.y() * column.w()) /
                      (w * w) * viewport.height() * 0.5;
    pixelsPerUnit = std::max(pixelsPerUnit, std::hypot(dx, dy));
  }
  if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 1.0e-12) {
    return {};
  }
  const float targetPixels = static_cast<float>(std::min(
      76.0 * scale, std::min(viewport.width(), viewport.height()) * 0.18));
  float worldLength = static_cast<float>(targetPixels / pixelsPerUnit);
  // One common world length preserves real axis foreshortening. Refine it for
  // perspective, where arrow tips may be closer to the camera than the origin.
  for (int pass = 0; pass < 3; ++pass) {
    qreal longest = 0.0;
    for (int axis = 0; axis < 3; ++axis) {
      const auto tip = project(originClip + viewProjection.column(axis) *
                                               worldLength,
                                viewport);
      if (tip) {
        const QPointF delta = tip->pixel - center->pixel;
        longest = std::max(longest, std::hypot(delta.x(), delta.y()));
      }
    }
    if (longest < 1.0 || !std::isfinite(longest)) {
      return {};
    }
    worldLength *= static_cast<float>(targetPixels / longest);
  }

  const std::array<QVector4D, 3> colors = {
      QVector4D(0.94F, 0.36F, 0.38F, 1.0F),
      QVector4D(0.32F, 0.58F, 0.96F, 1.0F),
      QVector4D(0.38F, 0.83F, 0.57F, 1.0F)};
  GeometryBuilder builder(viewport);
  for (int axis = 0; axis < 3; ++axis) {
    const auto tip = project(originClip + viewProjection.column(axis) *
                                             worldLength,
                              viewport);
    if (!tip) {
      continue;
    }
    const QPointF delta = tip->pixel - center->pixel;
    const qreal length = std::hypot(delta.x(), delta.y());
    if (length < 18.0 * scale) {
      continue; // A view-aligned arrow must not become a lump over the origin.
    }
    const QPointF direction = delta / length;
    const QPointF side(-direction.y(), direction.x());
    const auto along = [&](const qreal pixels) {
      return ScreenPoint{center->pixel + direction * pixels,
                         center->depth + static_cast<float>(pixels / length) *
                                             (tip->depth - center->depth)};
    };
    const ScreenPoint start = along(5.0 * scale);
    const ScreenPoint base = along(length - 11.0 * scale);
    const QVector4D color = colors[static_cast<std::size_t>(axis)];
    const QVector4D light = color * 0.66F + QVector4D(1, 1, 1, 1) * 0.34F;
    const QVector4D dark(color.x() * 0.48F, color.y() * 0.48F,
                         color.z() * 0.48F, 1.0F);
    builder.stroke(start, base, 4.2F * scale, kOutline);
    builder.stroke(start, base, 2.2F * scale, color);
    builder.stroke({start.pixel - side * (0.45 * scale), start.depth},
                    {base.pixel - side * (0.45 * scale), base.depth},
                    0.65F * scale, light);

    const ScreenPoint left{base.pixel + side * (4.8 * scale), base.depth};
    const ScreenPoint right{base.pixel - side * (4.8 * scale), base.depth};
    const ScreenPoint outerBase = along(length - 12.0 * scale);
    builder.triangle(along(length + 1.0 * scale),
                      {outerBase.pixel + side * (6.0 * scale), outerBase.depth},
                      {outerBase.pixel - side * (6.0 * scale), outerBase.depth},
                      kOutline);
    const bool lightOnLeft = QPointF::dotProduct(side, QPointF(-0.5, -0.85)) > 0;
    builder.triangle(*tip, left, base, lightOnLeft ? light : dark);
    builder.triangle(*tip, base, right, lightOnLeft ? dark : light);
    builder.letter(axis, {tip->pixel + direction * (12.0 * scale), tip->depth},
                    scale, light);
  }
  // A small machined-looking origin collar, with a dark centre and a single
  // bright centre pin. It is decorative, not a selection/transform handle.
  builder.disc(*center, 5.4F * scale, kOutline);
  builder.disc(*center, 4.1F * scale, QVector4D(0.65F, 0.72F, 0.77F, 1.0F));
  builder.disc(*center, 2.8F * scale, QVector4D(0.07F, 0.10F, 0.12F, 1.0F));
  builder.disc(*center, 1.05F * scale, QVector4D(0.91F, 0.94F, 0.96F, 1.0F));
  return builder.vertices;
}

} // namespace gsw

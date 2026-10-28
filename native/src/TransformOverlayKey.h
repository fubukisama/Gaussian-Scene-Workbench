#pragma once

#include "TransformGizmoRenderer.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gsw {

// Full Qt_6_8 serialization is fed directly into SHA-256, never materialized as
// a second geometry-sized QByteArray. At most 256 KiB of serialized input and
// 8192 elements per container are accepted. Only a 32-byte digest is retained;
// the unbuffered sink does not own a raw serialization buffer. These limits
// are independent of UiOverlayRaster's separately counted 8 MiB image budget.
inline constexpr qint64 transformOverlaySerializedByteLimit = 256 * 1024;
inline constexpr qsizetype transformOverlayContainerElementLimit = 8192;

namespace transform_overlay_detail {

class HashSink final : public QIODevice {
public:
  HashSink() { open(QIODevice::WriteOnly | QIODevice::Unbuffered); }
  bool isSequential() const override { return true; }
  QByteArray result() const { return mHash.result(); }

protected:
  qint64 readData(char *, qint64) override { return -1; }
  qint64 writeData(const char *data, qint64 count) override {
    if (count < 0 || count > transformOverlaySerializedByteLimit - mWritten) return -1;
    mHash.addData(QByteArrayView(data, qsizetype(count)));
    mWritten += count;
    return count;
  }

private:
  QCryptographicHash mHash{QCryptographicHash::Sha256};
  qint64 mWritten = 0;
};

inline bool ok(const QDataStream &stream) { return stream.status() == QDataStream::Ok; }

template <typename T, typename Write>
inline bool sequence(QDataStream &stream, const T &values, Write write) {
  if (!ok(stream) || values.size() > transformOverlayContainerElementLimit) return false;
  stream << qint64(values.size());
  for (const auto &value : values)
    if (!ok(stream) || !write(stream, value)) return false;
  return ok(stream);
}

inline bool polygon(QDataStream &stream, const QPolygonF &points) {
  return sequence(stream, points, [](QDataStream &out, const QPointF &point) {
    out << point;
    return ok(out);
  });
}

inline bool segment(QDataStream &stream, const TransformGizmoDepthSegment &value) {
  stream << value.line << value.startDepth << value.endDepth;
  return ok(stream);
}

inline bool optionalSegment(QDataStream &stream,
                            const std::optional<TransformGizmoDepthSegment> &value) {
  stream << value.has_value();
  return ok(stream) && (!value || segment(stream, *value));
}

inline bool faces(QDataStream &stream, const QList<TransformGizmoFace> &values) {
  return sequence(stream, values, [](QDataStream &out, const TransformGizmoFace &face) {
    if (!polygon(out, face.polygon)) return false;
    out << face.normalizedDepth << face.light;
    return sequence(out, face.vertexDepths, [](QDataStream &data, float depth) {
      data << depth;
      return ok(data);
    });
  });
}

class PaintBounds final {
public:
  bool point(const QPointF &value) {
    if (++mElements > transformOverlayContainerElementLimit ||
        !std::isfinite(value.x()) || !std::isfinite(value.y())) return mValid = false;
    mLeft = std::min(mLeft, value.x()); mRight = std::max(mRight, value.x());
    mTop = std::min(mTop, value.y()); mBottom = std::max(mBottom, value.y());
    mAny = true;
    return true;
  }
  bool line(const QLineF &value) {
    return value.isNull() || (point(value.p1()) && point(value.p2()));
  }
  bool polygon(const QPolygonF &value) {
    if (value.size() > transformOverlayContainerElementLimit) return mValid = false;
    for (const auto &vertex : value) if (!point(vertex)) return false;
    return true;
  }
  bool rect(const QRectF &value) {
    // Empty rectangles are not drawn; Scale's diamond is included separately.
    return value.isEmpty() || (point(value.topLeft()) && point(value.bottomRight()));
  }
  bool ellipse(const QPointF &center, qreal radius) {
    if (!std::isfinite(radius)) return mValid = false;
    if (radius == 0) return true;
    radius = std::abs(radius);
    return point(center - QPointF(radius, radius)) && point(center + QPointF(radius, radius));
  }
  QRectF clipped(const QRectF &viewport, qreal padding) const {
    if (!mValid || !mAny || !std::isfinite(padding)) return {};
    const qreal left = std::max(viewport.left(), mLeft - padding);
    const qreal top = std::max(viewport.top(), mTop - padding);
    const qreal right = std::min(viewport.right(), mRight + padding);
    const qreal bottom = std::min(viewport.bottom(), mBottom + padding);
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom) || left >= right || top >= bottom) return {};
    return QRectF(QPointF(left, top), QPointF(right, bottom));
  }

private:
  qreal mLeft = std::numeric_limits<qreal>::infinity();
  qreal mRight = -std::numeric_limits<qreal>::infinity();
  qreal mTop = std::numeric_limits<qreal>::infinity();
  qreal mBottom = -std::numeric_limits<qreal>::infinity();
  qsizetype mElements = 0;
  bool mAny = false;
  bool mValid = true;
};
} // namespace transform_overlay_detail

// Display identity, not an approximate/rounded camera key. Every layout field
// is serialized (including hit-only fields), plus all actual renderer style and
// interaction inputs. The renderer draws no text, so language/font are not its
// inputs. DPR, tile origin/dimensions, font/DPI and outer painter assumptions
// belong to UiOverlayRaster. Empty means over-budget/failed: use original vectors,
// never cache all rejected layouts under one empty key.
inline QByteArray transformOverlayKey(const TransformGizmoLayout &layout,
    TransformGizmoMode mode, const TransformGizmoStyle &style,
    TransformGizmoHandle highlighted, bool dragging) {
  using namespace transform_overlay_detail;
  HashSink sink;
  QDataStream out(&sink);
  out.setVersion(QDataStream::Qt_6_8);
  out.setByteOrder(QDataStream::BigEndian);
  out.setFloatingPointPrecision(QDataStream::DoublePrecision);
  out << quint32(0x4753544f) << quint16(1) << qint32(mode)
      << qint32(highlighted.kind) << qint32(highlighted.axis) << dragging;
  for (const auto &color : style.axes) out << color;
  out << style.highlight << style.neutral << style.centerEdge << style.light;
  out << layout.valid << layout.center << layout.radius << layout.pivotDepth
      << layout.lineWidth << layout.rotationHitInnerRadius << layout.trackballInnerRadius
      << layout.trackballOuterRadius << layout.centerHandle << layout.centerHitRadius
      << layout.trackballBounds;
  if (!polygon(out, layout.viewRing)) return {};
  for (const auto &axis : layout.axes) {
    out << axis.direction << axis.moveLine << axis.scaleLine << axis.moveEndpoint
        << axis.scaleEndpoint << axis.scaleHandle << axis.moveEndpointDepth
        << axis.scaleEndpointDepth << axis.visible;
    if (!polygon(out, axis.moveArrow) || !polygon(out, axis.rotationRing) ||
        !sequence(out, axis.rotationSegments, [](QDataStream &data, const QLineF &line) {
          data << line; return ok(data);
        }) || !sequence(out, axis.depthSegments, segment) ||
        !faces(out, axis.moveFaces) || !faces(out, axis.scaleFaces) ||
        !optionalSegment(out, axis.moveShaft) || !optionalSegment(out, axis.scaleShaft)) return {};
  }
  for (std::size_t axis = 0; axis < layout.planeHandles.size(); ++axis) {
    if (!polygon(out, layout.planeHandles[axis])) return {};
    out << layout.planeDepths[axis];
  }
  return ok(out) ? sink.result() : QByteArray();
}

// Conservative union of everything this renderer can paint in any of its four
// modes. No hit target, vertex, projection or depth is changed. It intentionally
// includes highlight-only trackball paths. There are no glyphs in this renderer;
// viewport hints/labels have separate bounds and display identity.
//
// Max active round ring underlay width is lineWidth + .8 + 1.1. Add two logical
// pixels for coverage, with a four-pixel minimum margin. Crop the completed bounds
// to the widget's visible logical rectangle, not the world geometry. This does
// not promise a small tile: close-ups can cover the entire widget, and the owner
// must retain UiOverlayRaster's 8 MiB/4096-edge refusal and vector fallback.
// A raster painter must have an explicit logical tile clip so stem face masking
// does not fall back to its unrelated image-local painter.window().
inline QRectF transformOverlayBounds(const TransformGizmoLayout &layout,
                                    const QRectF &viewportRect) {
  using namespace transform_overlay_detail;
  if (!layout.valid || !viewportRect.isValid() || !std::isfinite(viewportRect.left()) ||
      !std::isfinite(viewportRect.right()) || !std::isfinite(viewportRect.top()) ||
      !std::isfinite(viewportRect.bottom()) || !std::isfinite(layout.lineWidth)) return {};
  PaintBounds bounds;
  if (!bounds.rect(layout.centerHandle) || !bounds.rect(layout.trackballBounds) ||
      !bounds.ellipse(layout.center, layout.trackballInnerRadius) ||
      !bounds.ellipse(layout.center, layout.centerHandle.width() * .5) ||
      !bounds.polygon(layout.viewRing)) return {};
  for (const auto &axis : layout.axes) {
    if (axis.depthSegments.size() > transformOverlayContainerElementLimit ||
        axis.rotationSegments.size() > transformOverlayContainerElementLimit ||
        axis.moveFaces.size() > transformOverlayContainerElementLimit ||
        axis.scaleFaces.size() > transformOverlayContainerElementLimit) return {};
    if (axis.depthSegments.isEmpty()) {
      for (const auto &line : axis.rotationSegments) if (!bounds.line(line)) return {};
    } else {
      for (const auto &segment : axis.depthSegments) if (!bounds.line(segment.line)) return {};
    }
    if (!axis.visible) continue;
    if (!bounds.line(axis.moveLine)) return {};
    if (axis.moveFaces.isEmpty()) {
      if (!bounds.polygon(axis.moveArrow)) return {};
    } else {
      for (const auto &face : axis.moveFaces) if (!bounds.polygon(face.polygon)) return {};
    }
    if (!axis.scaleHandle.isEmpty()) {
      if (!bounds.line(axis.scaleLine)) return {};
      if (axis.scaleFaces.isEmpty()) {
        if (!bounds.rect(axis.scaleHandle)) return {};
      } else {
        for (const auto &face : axis.scaleFaces) if (!bounds.polygon(face.polygon)) return {};
      }
    }
  }
  for (const auto &plane : layout.planeHandles) if (!bounds.polygon(plane)) return {};
  const qreal maximumStroke = std::max(qreal(2.5), std::abs(layout.lineWidth) + qreal(1.9));
  return bounds.clipped(viewportRect, std::max(qreal(4), maximumStroke * .5 + qreal(2)));
}
} // namespace gsw

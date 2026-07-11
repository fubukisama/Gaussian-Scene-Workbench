#pragma once

#include "PlyPointCloudLoader.h"

#include <QBitArray>
#include <QMatrix4x4>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QVector>

namespace gsw {

enum class ScreenSelectionShape {
  Rectangle,
  Lasso,
  Brush
};

struct ScreenSelectionRequest {
  ScreenSelectionShape shape = ScreenSelectionShape::Rectangle;
  QRectF rectangle;
  QPolygonF path;
  qreal brushRadius = 0.0;
  bool visibleOnly = true;
};

[[nodiscard]] bool screenPointInsideSelection(
    const QPointF &point, const ScreenSelectionRequest &request);

[[nodiscard]] QVector<quint32> selectSourcePoints(
    const QVector<PointPosition> &positions, const QBitArray &deleted,
    const QMatrix4x4 &viewProjection, const QSize &viewport,
    const ScreenSelectionRequest &request);

} // namespace gsw

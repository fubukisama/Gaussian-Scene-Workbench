#include "ScreenSpaceSelection.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QVector4D>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gsw {

namespace {
constexpr float kVisibleDepthTolerance = 0.006F;
constexpr int kMaximumDepthGridDimension = 1280;

struct ProjectedSourcePoint {
  float x = 0.0F;
  float y = 0.0F;
  float depth = 0.0F;
};

bool projectSourcePoint(const PointPosition &point,
                        const QMatrix4x4 &viewProjection,
                        const QSize &viewport,
                        ProjectedSourcePoint &projected) {
  if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
      !std::isfinite(point.z) || viewport.isEmpty()) {
    return false;
  }
  const QVector4D clip =
      viewProjection * QVector4D(point.x, point.y, point.z, 1.0F);
  if (clip.w() <= 0.0001F) {
    return false;
  }
  const float inverseW = 1.0F / clip.w();
  const float normalizedX = clip.x() * inverseW;
  const float normalizedY = clip.y() * inverseW;
  const float normalizedZ = clip.z() * inverseW;
  if (normalizedX < -1.0F || normalizedX > 1.0F ||
      normalizedY < -1.0F || normalizedY > 1.0F ||
      normalizedZ < -1.0F || normalizedZ > 1.0F) {
    return false;
  }
  projected.x = (normalizedX * 0.5F + 0.5F) * viewport.width();
  projected.y =
      (1.0F - (normalizedY * 0.5F + 0.5F)) * viewport.height();
  projected.depth = normalizedZ;
  return true;
}

qreal squaredDistanceToSegment(const QPointF &point, const QPointF &start,
                               const QPointF &end) {
  const QPointF segment = end - start;
  const qreal lengthSquared = QPointF::dotProduct(segment, segment);
  if (lengthSquared <= 1.0e-9) {
    const QPointF offset = point - start;
    return QPointF::dotProduct(offset, offset);
  }
  const qreal projection = std::clamp(
      QPointF::dotProduct(point - start, segment) / lengthSquared, 0.0, 1.0);
  const QPointF offset = point - (start + segment * projection);
  return QPointF::dotProduct(offset, offset);
}

bool isDeleted(const QBitArray &deleted, const qsizetype index) {
  return index < deleted.size() && deleted.testBit(index);
}

QImage createBrushMask(const ScreenSelectionRequest &request,
                       const QSize &viewport) {
  if (request.shape != ScreenSelectionShape::Brush ||
      request.path.isEmpty() || request.brushRadius <= 0.0 ||
      viewport.isEmpty()) {
    return {};
  }

  QImage mask(viewport, QImage::Format_Grayscale8);
  if (mask.isNull()) {
    return {};
  }
  mask.fill(0);
  QPainter painter(&mask);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(Qt::white, request.brushRadius * 2.0, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::white);
  if (request.path.size() == 1) {
    painter.drawEllipse(request.path.first(), request.brushRadius,
                        request.brushRadius);
  } else {
    QPainterPath stroke;
    stroke.moveTo(request.path.first());
    for (qsizetype index = 1; index < request.path.size(); ++index) {
      stroke.lineTo(request.path.at(index));
    }
    painter.drawPath(stroke);
  }
  painter.end();
  return mask;
}

bool brushMaskContains(const QImage &mask, const ProjectedSourcePoint &point) {
  if (mask.isNull()) {
    return false;
  }
  const int x = std::clamp(qRound(point.x), 0, mask.width() - 1);
  const int y = std::clamp(qRound(point.y), 0, mask.height() - 1);
  return mask.constScanLine(y)[x] != 0;
}
} // namespace

bool screenPointInsideSelection(const QPointF &point,
                                const ScreenSelectionRequest &request) {
  switch (request.shape) {
  case ScreenSelectionShape::Rectangle:
    return request.rectangle.normalized().contains(point);
  case ScreenSelectionShape::Lasso:
    return request.path.size() >= 3 &&
           request.path.boundingRect().contains(point) &&
           request.path.containsPoint(point, Qt::OddEvenFill);
  case ScreenSelectionShape::Brush: {
    if (request.path.isEmpty() || request.brushRadius <= 0.0) {
      return false;
    }
    const qreal radiusSquared = request.brushRadius * request.brushRadius;
    const QRectF bounds = request.path.boundingRect().adjusted(
        -request.brushRadius, -request.brushRadius, request.brushRadius,
        request.brushRadius);
    if (!bounds.contains(point)) {
      return false;
    }
    if (request.path.size() == 1) {
      const QPointF offset = point - request.path.first();
      return QPointF::dotProduct(offset, offset) <= radiusSquared;
    }
    for (qsizetype index = 1; index < request.path.size(); ++index) {
      if (squaredDistanceToSegment(point, request.path.at(index - 1),
                                   request.path.at(index)) <= radiusSquared) {
        return true;
      }
    }
    return false;
  }
  }
  return false;
}

QVector<quint32> selectSourcePoints(
    const QVector<PointPosition> &positions, const QBitArray &deleted,
    const QMatrix4x4 &viewProjection, const QSize &viewport,
    const ScreenSelectionRequest &request) {
  QVector<quint32> matches;
  matches.reserve(std::min<qsizetype>(positions.size() / 10, 1'000'000));
  if (positions.isEmpty() || viewport.isEmpty()) {
    return matches;
  }
  const QImage brushMask = createBrushMask(request, viewport);

  int gridWidth = 0;
  int gridHeight = 0;
  QVector<float> depthGrid;
  if (request.visibleOnly) {
    const int maximumViewportDimension =
        std::max(viewport.width(), viewport.height());
    const float scale = std::min(
        1.0F, static_cast<float>(kMaximumDepthGridDimension) /
                  static_cast<float>(maximumViewportDimension));
    gridWidth = std::max(1, qRound(viewport.width() * scale));
    gridHeight = std::max(1, qRound(viewport.height() * scale));
    depthGrid.fill(std::numeric_limits<float>::infinity(),
                   gridWidth * gridHeight);

    for (qsizetype index = 0; index < positions.size(); ++index) {
      if (isDeleted(deleted, index)) {
        continue;
      }
      ProjectedSourcePoint point;
      if (!projectSourcePoint(positions.at(index), viewProjection, viewport,
                              point)) {
        continue;
      }
      const int gridX = std::clamp(
          static_cast<int>(point.x / viewport.width() * gridWidth), 0,
          gridWidth - 1);
      const int gridY = std::clamp(
          static_cast<int>(point.y / viewport.height() * gridHeight), 0,
          gridHeight - 1);
      float &nearestDepth = depthGrid[gridY * gridWidth + gridX];
      nearestDepth = std::min(nearestDepth, point.depth);
    }
  }

  for (qsizetype index = 0; index < positions.size(); ++index) {
    if (isDeleted(deleted, index)) {
      continue;
    }
    ProjectedSourcePoint point;
    if (!projectSourcePoint(positions.at(index), viewProjection, viewport,
                            point)) {
      continue;
    }
    const bool insideSelection =
        request.shape == ScreenSelectionShape::Brush && !brushMask.isNull()
            ? brushMaskContains(brushMask, point)
            : screenPointInsideSelection(QPointF(point.x, point.y), request);
    if (!insideSelection) {
      continue;
    }
    if (request.visibleOnly) {
      const int gridX = std::clamp(
          static_cast<int>(point.x / viewport.width() * gridWidth), 0,
          gridWidth - 1);
      const int gridY = std::clamp(
          static_cast<int>(point.y / viewport.height() * gridHeight), 0,
          gridHeight - 1);
      if (point.depth >
          depthGrid.at(gridY * gridWidth + gridX) + kVisibleDepthTolerance) {
        continue;
      }
    }
    matches.append(static_cast<quint32>(index));
  }
  return matches;
}

} // namespace gsw

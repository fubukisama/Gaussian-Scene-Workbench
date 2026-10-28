#pragma once

#include <QByteArray>
#include <QFont>
#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QRectF>
#include <QtMath>

#include <cmath>
#include <functional>
#include <utility>

namespace gsw {

// A single-owner, navigation-sized coverage layer. QPainter's OpenGL engine
// does not provide analytic path coverage without an MSAA framebuffer. Render
// the same logical vectors into a bounded raster tile, resolve coverage there,
// then composite one physical-pixel-aligned image. Never supersample the scene.
// Qt owns any upload cache; requestedGpuBytes is a conservative image-upload
// request bound, not measured driver residency or dedicated VRAM.
class UiOverlayRaster final {
public:
  static constexpr quint64 byteLimit = 8ULL * 1024 * 1024;
  using Draw = std::function<void(QPainter &, QJsonObject &)>;

  bool paint(QPainter &target, const QRectF &logicalBounds,
             const QByteArray &key, const Draw &draw) {
    mLastHit = false;
    // A cached group cannot emulate per-primitive opacity/composition or an
    // arbitrary world transform. Preserve those uncommon callers by refusing
    // the optimized path; the owner draws its original vectors instead.
    if (target.opacity() != 1.0 || target.compositionMode() != QPainter::CompositionMode_SourceOver ||
        !target.worldTransform().isIdentity()) return false;
    const qreal dpr = target.device() ? target.device()->devicePixelRatioF() : 0;
    if (!std::isfinite(dpr) || dpr <= 0 || !logicalBounds.isValid() ||
        !std::isfinite(logicalBounds.left()) || !std::isfinite(logicalBounds.top()) ||
        !std::isfinite(logicalBounds.right()) || !std::isfinite(logicalBounds.bottom()))
      return false;
    const int logicalDpiX = target.device()->logicalDpiX();
    const int logicalDpiY = target.device()->logicalDpiY();
    if (logicalDpiX <= 0 || logicalDpiY <= 0) return false;
    // Snap the tile (not the vector geometry or hit targets) to physical pixels.
    const QPointF origin(std::floor(logicalBounds.left() * dpr) / dpr,
                         std::floor(logicalBounds.top() * dpr) / dpr);
    const qreal width = std::ceil((logicalBounds.right() - origin.x()) * dpr);
    const qreal height = std::ceil((logicalBounds.bottom() - origin.y()) * dpr);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    const QSize pixels{int(width), int(height)};
    // Native physical resolution, not supersampling. The raster paint engine
    // supplies vector edge coverage without depending on the widget's MSAA.
    // Release the old tile before allocation; count all owned pixel storage.
    const quint64 finalBytes = quint64(pixels.width()) * quint64(pixels.height()) * 4;
    constexpr quint64 bookkeepingReserve = 64 * 1024;
    const quint64 peakBytes = finalBytes + bookkeepingReserve;
    if (peakBytes > byteLimit || key.size() > 4096) return false;
    if (mImage.isNull() || mKey != key || mDpr != dpr || mOrigin != origin ||
        mImage.size() != pixels || mFont != target.font() ||
        mLogicalDpiX != logicalDpiX || mLogicalDpiY != logicalDpiY) {
      clear();
      QImage coverage(pixels, QImage::Format_ARGB32_Premultiplied);
      if (coverage.isNull()) return false;
      coverage.setDevicePixelRatio(dpr);
      coverage.setDotsPerMeterX(qRound(logicalDpiX / 0.0254));
      coverage.setDotsPerMeterY(qRound(logicalDpiY / 0.0254));
      coverage.fill(Qt::transparent);
      QPainter raster(&coverage);
      if (!raster.isActive()) return false;
      raster.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
      raster.setFont(target.font());
      raster.translate(-origin);
      raster.setClipRect(QRectF(origin, QSizeF(pixels) / dpr));
      QJsonObject diagnostics;
      draw(raster, diagnostics);
      if (!raster.end()) return false;
      mImage = std::move(coverage);
      mKey = key;
      mOrigin = origin;
      mDpr = dpr;
      mFont = target.font();
      mLogicalDpiX = logicalDpiX;
      mLogicalDpiY = logicalDpiY;
      mDiagnostics = std::move(diagnostics);
      mPeakCpuBytes = peakBytes;
      ++mGeneration;
    } else {
      mLastHit = true;
    }
    target.save();
    // The source already has exact physical dimensions: no second filtering
    // or logical-size rounding that would blur a correctly covered contour.
    target.setRenderHint(QPainter::SmoothPixmapTransform, false);
    target.drawImage(mOrigin, mImage);
    target.restore();
    return true;
  }

  void clear() {
    mImage = {};
    mKey.clear();
    mDiagnostics = {};
    mPeakCpuBytes = 0;
    mLastHit = false;
  }
  [[nodiscard]] quint64 generation() const { return mGeneration; }
  [[nodiscard]] bool lastHit() const { return mLastHit; }
  [[nodiscard]] quint64 peakCpuBytes() const { return mPeakCpuBytes; }
  [[nodiscard]] quint64 requestedGpuBytes() const { return quint64(mImage.sizeInBytes()); }
  [[nodiscard]] const QJsonObject &diagnostics() const { return mDiagnostics; }

private:
  QImage mImage;
  QByteArray mKey;
  QPointF mOrigin;
  qreal mDpr = 0;
  int mLogicalDpiX = 0;
  int mLogicalDpiY = 0;
  QFont mFont;
  QJsonObject mDiagnostics;
  quint64 mGeneration = 0;
  quint64 mPeakCpuBytes = 0;
  bool mLastHit = false;
};

} // namespace gsw

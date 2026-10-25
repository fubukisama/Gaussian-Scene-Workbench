#include "BlenderViewportVisualTestSupport.h"
#include "NativeViewport.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMouseEvent>
#include <QStorageInfo>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw::viewportqa {

void processFrames(const int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

bool saveEvidence(const QImage &frame, const QString &name) {
  const QString configured = qEnvironmentVariable("GSW_BLENDER_VIEWPORT_EVIDENCE");
  if (configured.isEmpty()) return true;
  if (!QDir::isAbsolutePath(configured)) {
    qCritical() << "Viewport evidence requires an absolute directory:" << configured;
    return false;
  }
  const QString directory = QDir::cleanPath(QFileInfo(configured).absoluteFilePath());
#ifdef Q_OS_WIN
  const auto localRoot = [](const QString &path) {
    const QString normalized = QDir::fromNativeSeparators(path);
    if (normalized.size() >= 3 && normalized.at(0).toUpper() >= QLatin1Char('A') &&
        normalized.at(0).toUpper() <= QLatin1Char('Z') && normalized.at(1) == QLatin1Char(':') &&
        normalized.at(2) == QLatin1Char('/')) {
      const QString canonical = QFileInfo(normalized.left(3)).canonicalFilePath();
      if (canonical.size() >= 3 && canonical.at(1) == QLatin1Char(':') &&
          canonical.at(2) == QLatin1Char('/')) return canonical;
    }
    return QString();
  };
  const QString root = localRoot(directory);
  if (root.isEmpty()) {
    qCritical() << "Viewport evidence requires a local drive directory:" << directory;
    return false;
  }
  QString systemRoot = localRoot(qEnvironmentVariable("SystemRoot"));
  if (systemRoot.isEmpty()) systemRoot = localRoot(qEnvironmentVariable("SystemDrive") + QLatin1Char('/'));
  if (systemRoot.isEmpty()) {
    wchar_t windowsDirectory[MAX_PATH + 1]{};
    const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH + 1);
    if (length > 0 && length <= MAX_PATH)
      systemRoot = localRoot(QString::fromWCharArray(windowsDirectory, static_cast<int>(length)));
  }
  if (systemRoot.isEmpty()) {
    qCritical() << "Unable to identify the system volume for viewport evidence";
    return false;
  }
  if (root.left(3).compare(systemRoot.left(3), Qt::CaseInsensitive) == 0) {
    const auto volumes = QStorageInfo::mountedVolumes();
    const bool alternative = std::any_of(volumes.cbegin(), volumes.cend(), [&](const QStorageInfo &volume) {
      if (!volume.isValid() || !volume.isReady() || volume.isReadOnly()) return false;
      const QString mountedRoot = localRoot(volume.rootPath());
      return !mountedRoot.isEmpty() &&
             mountedRoot.left(3).compare(systemRoot.left(3), Qt::CaseInsensitive) != 0 &&
             QFileInfo(volume.rootPath()).isWritable();
    });
    if (alternative) {
      qCritical() << "Viewport evidence requires an available writable non-system volume:" << directory;
      return false;
    }
  }
  const auto allowedCanonicalPath = [&root](const QFileInfo &info) {
    const QString canonical = info.canonicalFilePath();
    return root.size() >= 3 && root.at(1) == QLatin1Char(':') && root.at(2) == QLatin1Char('/') &&
           !canonical.isEmpty() &&
           (canonical.compare(root, Qt::CaseInsensitive) == 0 ||
            canonical.startsWith(root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/'),
                                 Qt::CaseInsensitive));
  };
#else
  const auto allowedCanonicalPath = [](const QFileInfo &info) {
    return !info.canonicalFilePath().isEmpty();
  };
#endif
  QFileInfo ancestor(directory);
  while (!ancestor.exists()) {
    const QString parent = ancestor.absolutePath();
    if (parent == ancestor.absoluteFilePath()) break;
    ancestor.setFile(parent);
  }
  if (!ancestor.isDir() || !allowedCanonicalPath(ancestor) || !QDir().mkpath(directory) ||
      !allowedCanonicalPath(QFileInfo(directory))) {
    qCritical() << "Unable to create safe viewport evidence directory:" << directory;
    return false;
  }
  const QString output = QDir(directory).filePath(name + QStringLiteral(".png"));
  if (!frame.save(output)) {
    qCritical() << "Unable to write viewport evidence image:" << output;
    return false;
  }
  return true;
}

void clickViewport(NativeViewport &viewport, const QPointF &position) {
  const QPointF global(viewport.mapToGlobal(position.toPoint()));
  QMouseEvent press(QEvent::MouseButtonPress, position, position, global,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, position, position, global,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&viewport, &press);
  QCoreApplication::sendEvent(&viewport, &release);
}

AxisPixels groundAxisPixels(const QImage &frame) {
  AxisPixels result;
  // Observe the long world-origin axes, not the small reference triad or the
  // navigation gizmo. The rectangles are screen regions, not shader internals.
  const QRect sample(qRound(frame.width() * 0.20),
                     qRound(frame.height() * 0.20),
                     qRound(frame.width() * 0.60),
                     qRound(frame.height() * 0.60));
  const QPointF center(frame.width() * 0.5, frame.height() * 0.5);
  const qreal excludedRadius = frame.height() * 0.13;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const QPointF fromCenter = QPointF(x, y) - center;
      if (QPointF::dotProduct(fromCenter, fromCenter) <
          excludedRadius * excludedRadius) continue;
      const QColor pixel = frame.pixelColor(x, y);
      if (pixel.red() > pixel.green() + 8 &&
          pixel.red() > pixel.blue() + 8) ++result.red;
      if (pixel.green() > pixel.red() + 8 &&
          pixel.green() > pixel.blue() + 8) ++result.green;
      if (pixel.blue() > pixel.red() + 8 &&
          pixel.blue() > pixel.green() + 8) ++result.blue;
    }
  }
  return result;
}

double neutralGridInk(const QImage &frame) {
  const QRect sample(qRound(frame.width() * 0.18),
                     qRound(frame.height() * 0.22),
                     qRound(frame.width() * 0.64),
                     qRound(frame.height() * 0.56));
  const QPoint center(frame.width() / 2, frame.height() / 2);
  double ink = 0.0;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      // Keep the measurement independent of world-axis colour and the origin
      // marker. The remaining region contains only the neutral ground grid.
      if (std::abs(x - center.x()) <= 12 ||
          std::abs(y - center.y()) <= 12) continue;
      const QColor pixel = frame.pixelColor(x, y);
      const int high = std::max({pixel.red(), pixel.green(), pixel.blue()});
      const int low = std::min({pixel.red(), pixel.green(), pixel.blue()});
      if (high - low > 12) continue;
      ink += std::max(0.0, (pixel.red() + pixel.green() + pixel.blue()) / 3.0 - 13.0);
    }
  }
  return ink;
}

void wheelViewport(NativeViewport &viewport, const int angleDelta) {
  const QPointF center(viewport.width() * 0.5, viewport.height() * 0.5);
  const QPointF global(viewport.mapToGlobal(center.toPoint()));
  QWheelEvent wheel(center, global, QPoint(), QPoint(0, angleDelta),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QCoreApplication::sendEvent(&viewport, &wheel);
}

QColor unobstructedBackdrop(const QImage &frame) {
  // A median rejects the few thin neutral grid strokes in this background
  // sample without masking or changing the viewport's actual rendering.
  const QRect sample(qRound(frame.width() * 0.40),
                     qRound(frame.height() * 0.08),
                     qRound(frame.width() * 0.20),
                     qRound(frame.height() * 0.06));
  std::array<std::vector<int>, 3> channels;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const QColor pixel = frame.pixelColor(x, y);
      channels[0].push_back(pixel.red());
      channels[1].push_back(pixel.green());
      channels[2].push_back(pixel.blue());
    }
  }
  std::array<int, 3> median{};
  for (std::size_t index = 0; index < channels.size(); ++index) {
    auto &values = channels[index];
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    median[index] = *middle;
  }
  return QColor(median[0], median[1], median[2]);
}

int cursorRedArcPixels(const QImage &frame, const QSize &logicalSize) {
  const qreal ratio = static_cast<qreal>(frame.width()) / logicalSize.width();
  const QPoint center(frame.width() / 2, frame.height() / 2);
  const int radius = qRound(36.0 * ratio);
  int count = 0;
  for (int y = center.y() - radius; y <= center.y() + radius; ++y) {
    for (int x = center.x() - radius; x <= center.x() + radius; ++x) {
      const QColor pixel = frame.pixelColor(x, y);
      // A Blender 3D cursor is a separate user-positionable tool, not the
      // source origin. Its bright red ink differs from the world X axis.
      if (pixel.red() >= 220 && pixel.green() <= 40 && pixel.blue() <= 40)
        ++count;
    }
  }
  return count;
}

void dragViewport(NativeViewport &viewport, const QPointF &start,
                  const QPointF &end) {
  const QPointF globalStart(viewport.mapToGlobal(start.toPoint()));
  const QPointF globalEnd(viewport.mapToGlobal(end.toPoint()));
  QMouseEvent press(QEvent::MouseButtonPress, start, start, globalStart,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent move(QEvent::MouseMove, end, end, globalEnd,
                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, end, end, globalEnd,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&viewport, &press);
  QCoreApplication::sendEvent(&viewport, &move);
  QCoreApplication::sendEvent(&viewport, &release);
}

BackgroundContrast backgroundContrast(const QImage &frame,
                                       const QRectF &normalizedRect) {
  const QRect sample(qRound(frame.width() * normalizedRect.x()),
                     qRound(frame.height() * normalizedRect.y()),
                     qRound(frame.width() * normalizedRect.width()),
                     qRound(frame.height() * normalizedRect.height()));
  BackgroundContrast result;
  double ink = 0.0;
  for (int y = sample.top(); y <= sample.bottom(); ++y) {
    for (int x = sample.left(); x <= sample.right(); ++x) {
      const QColor pixel = frame.pixelColor(x, y);
      const int high = std::max({pixel.red(), pixel.green(), pixel.blue()});
      const int low = std::min({pixel.red(), pixel.green(), pixel.blue()});
      if (std::abs(pixel.red() - 12) > 8 ||
          std::abs(pixel.green() - 13) > 8 ||
          std::abs(pixel.blue() - 14) > 8) ++result.visiblyChangedPixels;
      if (high - low <= 12)
        ink += std::max(0.0, (pixel.red() + pixel.green() + pixel.blue()) / 3.0 - 13.0);
    }
  }
  result.meanNeutralInk = ink / std::max(1, sample.width() * sample.height());
  return result;
}

bool nativeWindowExposed(NativeViewport &viewport) {
#ifdef Q_OS_WIN
  if (IsWindowVisible(reinterpret_cast<HWND>(viewport.winId()))) {
    viewport.hide();
    qCritical() << "Viewport acceptance unexpectedly exposed a native window";
    return true;
  }
#else
  Q_UNUSED(viewport);
#endif
  return false;
}

} // namespace gsw::viewportqa

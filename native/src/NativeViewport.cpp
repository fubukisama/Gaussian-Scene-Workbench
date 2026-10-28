#include <QCoreApplication>
#include "NativeViewport.h"
#include "../third_party/blender-navigation/BlenderViewNavigation.h"
#include "../third_party/blender-grid/BlenderGridShaders.h"
#include "../third_party/lichtfeld-streaming/UploadDrainBudget.h"
#include "AppLanguage.h"
#include "AppTheme.h"
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QSignalBlocker>

#include "ModelInteraction.h"
#include "RenderFrameDiagnostics.h"
#include "TransformGizmoRenderer.h"
#include "NavigationGizmo.h"
#include "ScreenSpaceSelection.h"
#include "ViewportCamera.h"
#include "ViewportPresentation.h"
#include "BlenderNavigationPresentation.h"
#include "WorkbenchIcons.h"

#include <QApplication>
#include <QDebug>
#include <QDateTime>
#include <QEnterEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFile>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QFutureWatcher>
#include <QHash>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QJsonArray>
#include <QLineF>
#include <QMouseEvent>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>
#include <QSizePolicy>
#include <QShowEvent>
#include <QTimer>
#include <QToolTip>
#include <QToolButton>
#include <QVector2D>
#include <QVector4D>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace gsw {

namespace {
constexpr float kPi = 3.14159265358979323846F;

bool meshPagingQaEnabled() {
  static const bool enabled = qEnvironmentVariableIntValue("GSW_MESH_PAGING_QA_DIAGNOSTICS") != 0;
  return enabled;
}

enum class MeshPagingQaEvent { Read, Enqueue, Upload, Reject, Evict };

// Explicit QA-only observations: do not affect admission, selection or retries.
// Bound both retained counters and output (first 64 events, then once a second).
void logMeshPagingQa(const MeshPagingQaEvent event, const MeshCacheIndex &cache,
                     const QSet<int> &desired, const int nodeId,
                     const qint64 bytes, const qint64 residentBytes,
                     const qint64 budgetBytes, const char *reason = "") {
  struct SourceCounters {
    QHash<int, std::array<quint64, 5>> nodes;
    std::array<quint64, 5> totals{};
  };
  static QHash<QString, SourceCounters> sources;
  static QElapsedTimer outputClock;
  static quint64 events = 0;
  if (!sources.contains(cache.sourcePath) && sources.size() >= 16) return;
  auto &source = sources[cache.sourcePath];
  if (!source.nodes.contains(nodeId) && source.nodes.size() >= 4096) return;
  auto &counts = source.nodes[nodeId];
  const auto eventIndex = static_cast<std::size_t>(event);
  ++counts[eventIndex];
  ++source.totals[eventIndex];
  ++events;
  if (events > 64 && outputClock.isValid() && outputClock.elapsed() < 1000) return;
  outputClock.start();
  bool fallback = false;
  qint64 desiredBytes = 0;
  for (const int wanted : desired) {
    if (wanted < 0 || wanted >= cache.nodes.size()) continue;
    desiredBytes += cache.nodes.at(wanted).byteCount();
    int ancestor = cache.nodes.at(wanted).parent;
    while (ancestor >= 0 && ancestor < cache.nodes.size()) {
      if (ancestor == nodeId) fallback = true;
      ancestor = cache.nodes.at(ancestor).parent;
    }
  }
  constexpr std::array<const char *, 5> names = {"read", "enqueue", "upload", "reject", "evict"};
  qInfo() << "MESH_PAGING_QA:" << cache.sourcePath << names[eventIndex]
          << "node" << nodeId << (desired.contains(nodeId) ? "desired" : fallback ? "fallback" : "obsolete")
          << "reason" << reason << "bytes/resident/budget/desired" << bytes << residentBytes << budgetBytes << desiredBytes
          << "desired count" << desired.size() << "node read/enqueue/upload/reject/evict"
          << counts[0] << counts[1] << counts[2] << counts[3] << counts[4]
          << "source read/enqueue/upload/reject/evict" << source.totals[0] << source.totals[1]
          << source.totals[2] << source.totals[3] << source.totals[4];
}

// Presentation colours only. Model RGB, material textures, Gaussian attributes
// and compositing equations are deliberately independent of the UI theme.
struct ViewportColors final {
  QColor backdrop{12, 15, 17};
  QColor card{17, 19, 21};
  QColor text{232, 235, 236};
  QColor muted{181, 189, 193};
  QColor accent{102, 193, 168};
  QColor processCard{17, 23, 26};
  QColor processText{120, 208, 186};
  QColor processDetail{181, 193, 200};
  QColor progress{89, 169, 153};
  QColor modeCard{49, 93, 88};
  QColor modeText{238, 246, 244};
  QColor hintCard{38, 31, 23};
  QColor hintText{255, 203, 122};
  QColor highlight{255, 224, 118};
  QColor toolCard{35, 39, 44};
  QColor toolBorder{68, 73, 81};
  QColor toolHover{57, 63, 71};
  QColor toolActive{43, 104, 192};
  QColor toolIcon{238, 241, 243};
  QColor disabledText{157, 164, 169};
  QColor handleEdge{228, 232, 234};
  QColor handleFill{48, 53, 57};
  QColor viewRing{225, 229, 232};
};

bool lightViewport() { return AppTheme::currentTheme() == UiTheme::Light; }

ViewportColors viewportColors() {
  ViewportColors colors;
  if (!lightViewport()) return colors;
  colors.backdrop = QColor(255, 255, 255);
  colors.card = QColor(255, 255, 255);
  colors.text = QColor(25, 28, 32);
  colors.muted = QColor(65, 72, 80);
  colors.accent = QColor(24, 79, 165);
  colors.processCard = QColor(247, 248, 250);
  colors.processText = QColor(24, 79, 165);
  colors.processDetail = QColor(65, 72, 80);
  colors.progress = QColor(31, 98, 201);
  colors.modeCard = QColor(31, 98, 201);
  colors.modeText = QColor(255, 255, 255);
  colors.hintCard = QColor(255, 255, 255);
  colors.hintText = QColor(36, 39, 43);
  colors.highlight = QColor(168, 108, 14);
  colors.toolCard = QColor(245, 246, 248);
  colors.toolBorder = QColor(199, 205, 214);
  colors.toolHover = QColor(225, 229, 235);
  colors.toolActive = QColor(31, 98, 201);
  colors.toolIcon = QColor(27, 31, 36);
  colors.disabledText = QColor(113, 121, 131);
  colors.handleEdge = QColor(38, 43, 50);
  colors.handleFill = QColor(255, 255, 255);
  colors.viewRing = QColor(48, 55, 64);
  return colors;
}

QColor translucent(QColor color, const int alpha) {
  color.setAlpha(alpha);
  return color;
}

QColor overlaySurface(QColor color, const int nightAlpha) {
  // Neutral day cards are opaque so dense scans cannot reduce text contrast.
  // Selection fills and the observation sphere still use translucency.
  return lightViewport() ? color : translucent(color, nightAlpha);
}

QPen overlayBorder() {
  return lightViewport() ? QPen(QColor(174, 181, 190), 1.0) : QPen(Qt::NoPen);
}

QColor themeInk(const QColor &night, const QColor &day) {
  return lightViewport() ? day : night;
}

void drawViewportCaption(QPainter &painter, const QRect &bounds,
                         const int alignment, const QString &text,
                         const QColor &ink) {
  painter.save();
  painter.setPen(lightViewport() ? QColor(255, 255, 255, 220)
                                : QColor(0, 0, 0, 230));
  painter.drawText(bounds.translated(1, 1), alignment, text);
  painter.setPen(ink);
  painter.drawText(bounds, alignment, text);
  painter.restore();
}

QVector4D viewportClearColor() {
  // Preserve the original night clear value exactly, not its 8-bit rounding.
  if (!lightViewport()) return QVector4D(0.047F, 0.051F, 0.055F, 1.0F);
  const QColor backdrop = viewportColors().backdrop;
  return QVector4D(backdrop.redF(), backdrop.greenF(), backdrop.blueF(), 1.0F);
}

struct ReferenceGridFrame final {
  QVector3D normal;
  QVector3D axisU;
  QVector3D axisV;
  QVector3D axisUColor;
  QVector3D axisVColor;
};

ReferenceGridFrame referenceGridFrame(const ReferenceGridPlane plane) {
  const auto axisInk = [](int axis) {
    const QColor color = blenderAxisColor(axis, lightViewport());
    return QVector3D(color.redF(), color.greenF(), color.blueF());
  };
  const QVector3D xColor = axisInk(0);
  const QVector3D yColor = axisInk(1);
  const QVector3D zColor = axisInk(2);
  switch (plane) {
  case ReferenceGridPlane::XY:
    return {QVector3D(0.0F, 0.0F, 1.0F),
            QVector3D(1.0F, 0.0F, 0.0F),
            QVector3D(0.0F, 1.0F, 0.0F), xColor, yColor};
  case ReferenceGridPlane::XZ:
    return {QVector3D(0.0F, 1.0F, 0.0F),
            QVector3D(1.0F, 0.0F, 0.0F),
            QVector3D(0.0F, 0.0F, 1.0F), xColor, zColor};
  case ReferenceGridPlane::YZ:
    return {QVector3D(1.0F, 0.0F, 0.0F),
            QVector3D(0.0F, 1.0F, 0.0F),
            QVector3D(0.0F, 0.0F, 1.0F), yColor, zColor};
  }
  return {};
}

float radians(const float degrees) { return degrees * kPi / 180.0F; }

QColor mixColor(const QColor &background, const QColor &foreground,
                const qreal foregroundAmount) {
  const qreal amount = std::clamp(foregroundAmount, 0.0, 1.0);
  return QColor::fromRgbF(
      background.redF() * (1.0 - amount) + foreground.redF() * amount,
      background.greenF() * (1.0 - amount) + foreground.greenF() * amount,
      background.blueF() * (1.0 - amount) + foreground.blueF() * amount,
      background.alphaF() * (1.0 - amount) + foreground.alphaF() * amount);
}

QColor navigationAxisColor(const int axisIndex) {
  return blenderAxisColor(axisIndex, lightViewport());
}


QString navigationAxisLabel(const NavigationAxis axis) {
  switch (axis) {
  case NavigationAxis::PositiveX:
    return QStringLiteral("X");
  case NavigationAxis::NegativeX:
    return QStringLiteral("-X");
  case NavigationAxis::PositiveY:
    return QStringLiteral("Y");
  case NavigationAxis::NegativeY:
    return QStringLiteral("-Y");
  case NavigationAxis::PositiveZ:
    return QStringLiteral("Z");
  case NavigationAxis::NegativeZ:
    return QStringLiteral("-Z");
  case NavigationAxis::None:
    return {};
  }
  return {};
}

struct NavigationCubeFaceProjection final {
  QPolygonF polygon;
  qreal depth = 0.0;
  int axisIndex = 0;
};

std::vector<NavigationCubeFaceProjection>
projectNavigationCube(const QMatrix4x4 &viewMatrix, const QRectF &bounds) {
  std::array<QPointF, 8> projectedCorners;
  qreal maximumProjectedExtent = 0.0;
  for (int index = 0; index < 8; ++index) {
    const QVector3D corner((index & 1) != 0 ? 1.0F : -1.0F,
                           (index & 2) != 0 ? 1.0F : -1.0F,
                           (index & 4) != 0 ? 1.0F : -1.0F);
    const QVector3D viewCorner =
        (viewMatrix * QVector4D(corner, 0.0F)).toVector3D();
    projectedCorners[static_cast<std::size_t>(index)] =
        QPointF(viewCorner.x(), -viewCorner.y());
    maximumProjectedExtent =
        std::max({maximumProjectedExtent,
                  std::abs(static_cast<qreal>(viewCorner.x())),
                  std::abs(static_cast<qreal>(viewCorner.y()))});
  }

  if (maximumProjectedExtent < 1.0e-4) {
    return {};
  }
  const qreal fitExtent =
      std::max(1.0, std::min(bounds.width(), bounds.height()) * 0.5 - 1.0);
  const qreal scale = fitExtent / maximumProjectedExtent;
  for (QPointF &corner : projectedCorners) {
    corner = bounds.center() + corner * scale;
  }

  static constexpr std::array<std::array<int, 4>, 6> faceCorners = {{
      {{0, 4, 6, 2}},
      {{1, 3, 7, 5}},
      {{0, 1, 5, 4}},
      {{2, 6, 7, 3}},
      {{0, 2, 3, 1}},
      {{4, 5, 7, 6}},
  }};

  std::vector<NavigationCubeFaceProjection> faces;
  faces.reserve(3);
  for (int faceIndex = 0; faceIndex < 6; ++faceIndex) {
    const int axisIndex = faceIndex / 2;
    const float sign = (faceIndex % 2) == 0 ? -1.0F : 1.0F;
    QVector3D normal;
    if (axisIndex == 0) {
      normal.setX(sign);
    } else if (axisIndex == 1) {
      normal.setY(sign);
    } else {
      normal.setZ(sign);
    }
    const qreal depth = static_cast<qreal>(
        (viewMatrix * QVector4D(normal, 0.0F)).toVector3D().z());
    if (depth <= 1.0e-4) {
      continue;
    }

    NavigationCubeFaceProjection face;
    face.depth = depth;
    face.axisIndex = axisIndex;
    for (const int cornerIndex :
         faceCorners[static_cast<std::size_t>(faceIndex)]) {
      face.polygon <<
          projectedCorners[static_cast<std::size_t>(cornerIndex)];
    }
    faces.push_back(std::move(face));
  }
  std::sort(faces.begin(), faces.end(),
            [](const NavigationCubeFaceProjection &left,
               const NavigationCubeFaceProjection &right) {
              return left.depth < right.depth;
            });
  return faces;
}

float shortestEquivalentAngle(const float current, float target) {
  while (target - current > 180.0F) {
    target -= 360.0F;
  }
  while (target - current < -180.0F) {
    target += 360.0F;
  }
  return target;
}

QString modeLabel(const NativeViewport::InteractionMode mode) {
  switch (mode) {
  case NativeViewport::InteractionMode::Inspect:
    return QCoreApplication::translate("Workbench", "查看");
  case NativeViewport::InteractionMode::Move:
    return QCoreApplication::translate("Workbench", "移动模型");
  case NativeViewport::InteractionMode::Rotate:
    return QCoreApplication::translate("Workbench", "旋转模型");
  case NativeViewport::InteractionMode::Scale:
    return QCoreApplication::translate("Workbench", "缩放模型");
  case NativeViewport::InteractionMode::Select:
    return QCoreApplication::translate("Workbench", "选择");
  case NativeViewport::InteractionMode::Rectangle:
    return QCoreApplication::translate("Workbench", "框选");
  case NativeViewport::InteractionMode::Lasso:
    return QCoreApplication::translate("Workbench", "套索");
  case NativeViewport::InteractionMode::Brush:
    return QCoreApplication::translate("Workbench", "笔刷");
  case NativeViewport::InteractionMode::Crop:
    return QCoreApplication::translate("Workbench", "裁剪");
  }
  return {};
}

bool isTrimInteractionMode(const NativeViewport::InteractionMode mode) {
  switch (mode) {
  case NativeViewport::InteractionMode::Move:
  case NativeViewport::InteractionMode::Rotate:
  case NativeViewport::InteractionMode::Scale:
  case NativeViewport::InteractionMode::Select:
  case NativeViewport::InteractionMode::Rectangle:
  case NativeViewport::InteractionMode::Lasso:
  case NativeViewport::InteractionMode::Brush:
  case NativeViewport::InteractionMode::Crop:
    return true;
  case NativeViewport::InteractionMode::Inspect:
    return false;
  }
  return false;
}

Qt::CursorShape defaultInteractionCursor(
    const NativeViewport::InteractionMode mode) {
  if (mode == NativeViewport::InteractionMode::Move) {
    return Qt::SizeAllCursor;
  }
  if (mode == NativeViewport::InteractionMode::Rotate) {
    return Qt::CrossCursor;
  }
  if (mode == NativeViewport::InteractionMode::Scale) {
    return Qt::SizeFDiagCursor;
  }
  return mode == NativeViewport::InteractionMode::Rectangle ||
                 mode == NativeViewport::InteractionMode::Lasso ||
                 mode == NativeViewport::InteractionMode::Brush
             ? Qt::CrossCursor
             : Qt::ArrowCursor;
}

QString formatCount(const qint64 count) {
  if (count >= 1000000) {
    return QStringLiteral("%1 M").arg(static_cast<double>(count) / 1000000.0, 0,
                                      'f', 2);
  }
  if (count >= 1000) {
    return QStringLiteral("%1 K").arg(static_cast<double>(count) / 1000.0, 0,
                                      'f', 1);
  }
  return QString::number(count);
}

} // namespace

NativeViewport::NativeViewport(QWidget *parent) : QOpenGLWidget(parent) {
  mResources = std::make_shared<ResourceBudgetController>();
  mResourceClock.start();
  mResourceBudgetTimer = new QTimer(this);
  // On Windows a coarse timer relies on low-priority WM_TIMER dispatch and can
  // starve behind continuous viewport updates. Keep the one-second interval,
  // but deliver resource observations independently of that idle-message path.
  mResourceBudgetTimer->setTimerType(Qt::PreciseTimer);
  mResourceBudgetTimer->setInterval(1000);
  connect(mResourceBudgetTimer, &QTimer::timeout, this,
          &NativeViewport::refreshResourceBudgets);
  mResourceBudgetTimer->start();
  setObjectName(QStringLiteral("nativeViewport"));
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setMinimumSize(0, 0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  AppLanguage::onChanged(this, [this]() {
    if (mNavigationHover.part != NavigationGizmoPart::None)
      updateNavigationGizmoHover(mPointerPosition);
    updateObservationNavigationToolTip();
    auto states = mSceneStates;
    if (!states.contains(mScene)) states.append(mScene);
    for (const auto &state : states) {
      if (!state->resourceLoading) {
        if (state->resourceUploadDeferred && !state->mSceneLoadMessage.isEmpty())
          state->mSceneLoadMessage = QCoreApplication::translate(
              "Workbench", "Insufficient GPU memory budget to show this model.");
        else if (state->resourceNeedsReload)
          state->mSceneLoadMessage = QCoreApplication::translate(
              "Workbench", "Insufficient memory for the mesh preview and texture.");
      }
    }
    // Presentation changes must not masquerade as a coordinate/data update:
    // owners use those signals to apply workspace TRS and rebuild the tree.
    // Presentation owners read the translated error via cameraTrajectoryError().
    update();
  });
  resetModelTransformHistory();

  mViewSnapTimer = new QTimer(this);
  mViewSnapTimer->setObjectName(QStringLiteral("viewSnapTimer"));
  mViewSnapTimer->setTimerType(Qt::PreciseTimer);
  mViewSnapTimer->setInterval(16);
  connect(mViewSnapTimer, &QTimer::timeout, this, [this]() {
    // Start time belongs to the user's request, not to a deferred global
    // animation-driver initialization. Delayed GUI dispatch catches up to
    // real elapsed time without extending the original 220-ms transition.
    const auto elapsed = mViewSnapClock.elapsed();
    const auto angles = cameraSnapAngles(mSnapStartAngles, mSnapTargetAngles, elapsed);
    mYawDegrees = angles.yawDegrees;
    mPitchDegrees = angles.pitchDegrees;
    mRollDegrees = angles.rollDegrees;
    if (elapsed >= cameraSnapDurationMilliseconds) mViewSnapTimer->stop();
    update();
  });
  mTrainingGpuPreviewTimer = new QTimer(this);
  mTrainingGpuPreviewTimer->setObjectName(QStringLiteral("trainingPreviewPollTimer"));
  mTrainingGpuPreviewTimer->setTimerType(Qt::PreciseTimer);
  mTrainingGpuPreviewTimer->setInterval(33);
  connect(mTrainingGpuPreviewTimer, &QTimer::timeout, this,
          [this] {
            if (mProcessingPreviewVisible && isVisible() && !window()->isMinimized()) update();
          });
  mFrameRefreshTimer = new QTimer(this);
  mFrameRefreshTimer->setObjectName(QStringLiteral("viewportUploadRefreshTimer"));
  mFrameRefreshTimer->setTimerType(Qt::PreciseTimer);
  mFrameRefreshTimer->setInterval(0);
  connect(mFrameRefreshTimer, &QTimer::timeout, this,
          [this] {
            if (isVisible() && !window()->isMinimized()) update();
          });
  mPresentationIdleTimer = new QTimer(this);
  mPresentationIdleTimer->setObjectName(QStringLiteral("viewportPresentationIdleTimer"));
  mPresentationIdleTimer->setSingleShot(true);
  mPresentationIdleTimer->setInterval(350);
  connect(mPresentationIdleTimer, &QTimer::timeout, this, [this] {
    if (frameTimingBurstActive()) {
      mPresentationWaiting = true;
      // Recheck a stalled drag/worker without requesting a GL frame. A button
      // release or completed worker may not itself produce another image.
      mPresentationIdleTimer->start();
    } else {
      pausePresentationMetrics();
      return;
    }
    refreshViewportPresentation();
    emit viewportPresentationChanged();
  });
  mViewportStatusLabel = new QLabel(this);
  mViewportStatusLabel->setObjectName(QStringLiteral("viewportStatusLabel"));
  mViewportStatusLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  mViewportDetailsButton = new QToolButton(this);
  mViewportDetailsButton->setObjectName(QStringLiteral("viewportDetailsButton"));
  mViewportDetailsButton->setAutoRaise(true);
  AppLanguage::bind(mViewportDetailsButton, "toolTip", AppLanguage::source("视口详情"));
  AppLanguage::bind(mViewportDetailsButton, "accessibleName", AppLanguage::source("视口详情"));
  connect(mViewportDetailsButton, &QToolButton::clicked, this, &NativeViewport::viewportDetailsRequested);
  AppLanguage::onChanged(this, [this] { refreshViewportPresentation(); });
  connect(this, &QOpenGLWidget::frameSwapped, this, [this]() {
    // Qt emits frameSwapped for top-level composition, including unrelated
    // status-label repaints that reuse this widget's existing FBO. Count only
    // a newly rendered viewport image that has actually reached composition.
    if (mPresentedFrameSerial == mRenderedFrameSerial) return;
    mPresentedFrameSerial = mRenderedFrameSerial;
    if (mRenderDiagnostics) mRenderDiagnostics->presented(this, mPresentedFrameSerial);
    if (mScene->mRenderedPointCount <= 0 && mScene->mFullResolutionPointCount <= 0 &&
        mScene->mRenderedMeshIndexCount <= 0 &&
        mScene->mFullResolutionMeshTriangleCount <= 0 &&
        !mTrainingGpuPreview.attached()) {
      pausePresentationMetrics();
      return;
    }
    mFrameRateCounter.frameCompleted(FrameRateCounter::Clock::now());
    mPresentationIdle = false;
    mPresentationWaiting = false;
    ++mFreshPresentationFrames;
    mPresentationIdleTimer->start();
    emit frameMetricsChanged(mFrameRateCounter.framesPerSecond(),
                             mFrameRateCounter.averageFrameMilliseconds());
    refreshViewportPresentation();
    emit viewportPresentationChanged();
  });
  const QString diagnosticsRoot = qEnvironmentVariable("GSW_RENDER_DIAGNOSTICS_DIR");
  if (!diagnosticsRoot.isEmpty())
    mRenderDiagnostics = std::make_unique<RenderFrameDiagnostics>(diagnosticsRoot);
  monitorRefreshWindow();
  refreshViewportPresentation();
}

NativeViewport::~NativeViewport() {
  if (context() != nullptr && context()->isValid()) {
    makeCurrent();
    mTrainingGpuPreview.release();
    releaseSceneBuffers();
    for (const auto &scene : mSceneStates) {
      if (scene == mScene) continue;
      QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, scene);
      releaseSceneBuffers();
    }
    if (mProcessingScene && mProcessingScene != mScene &&
        !mSceneStates.contains(mProcessingScene)) {
      QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, mProcessingScene);
      releaseSceneBuffers();
    }
    mDepthOverlayVertexArray.destroy();
    mGridVertexArray.destroy();
    mDepthOverlayBuffer.destroy();
    mRenderDiagnostics.reset();
    doneCurrent();
  }
}

ResourceBudgetPolicy NativeViewport::resourceBudgetPolicy() const {
  return mResources->policy();
}

void NativeViewport::setResourceBudgetPolicy(const ResourceBudgetPolicy &policy) {
  mResources->setPolicy(policy);
  for (const auto &state : mSceneStates) {
    state->resourceRetryAfterMs = 0;
    state->resourceResidentUploadFailed = false;
  }
  mScene->resourceRetryAfterMs = 0;
  mScene->resourceResidentUploadFailed = false;
  refreshResourceBudgets();
  update();
}

ResourceBudgetStatus NativeViewport::resourceBudgetStatus() const {
  auto status = mResources->status();
  status.loadingMeshCount = 0;
  auto states = mSceneStates;
  if (!states.contains(mScene)) states.append(mScene);
  for (const auto &state : states) {
    if (state->mHasMesh) {
      if (state->mMeshCache.isValid()) ++status.pagedMeshCount;
      else ++status.residentMeshCount;
    }
    if (state->resourceLoading) ++status.loadingMeshCount;
  }
  return status;
}

void NativeViewport::updateManagedResourceUsage() {
  qint64 ram = mProcessingReferenceImage.sizeInBytes();
  qint64 gpu = 0;
  auto states = mSceneStates;
  if (!states.contains(mScene)) states.append(mScene);
  if (mProcessingScene && !states.contains(mProcessingScene)) states.append(mProcessingScene);
  for (const auto &state : states) {
    ram += state->mSourcePositions.capacity() * qint64(sizeof(PointPosition)) +
           state->mPreviewVertices.capacity() * qint64(sizeof(PointCloudVertex)) +
           state->mPendingVertices.capacity() * qint64(sizeof(PointCloudVertex)) +
           state->mPendingMeshVertices.capacity() * qint64(sizeof(MeshVertex)) +
           state->mPendingMeshIndices.capacity() * qint64(sizeof(quint32)) +
           state->mPendingMeshTexture.sizeInBytes();
    for (const auto &page : state->mPendingMeshCachePages)
      ram += page.vertices.capacity() * qint64(sizeof(MeshVertex)) +
             page.indices.capacity() * qint64(sizeof(quint32));
    for (const auto &page : state->mPendingPointCachePages)
      ram += page.vertices.capacity() * qint64(sizeof(PointPreviewVertex));
    ram += state->mSphericalHarmonics.coefficients.capacity() * qint64(sizeof(float)) +
           state->mSphericalHarmonics.sourceIndices.capacity() * qint64(sizeof(quint32));
    gpu += state->resourcePointGpuBytes + state->resourceMeshGpuBytes +
           state->resourceTextureGpuBytes + state->mMeshCacheResidentBytes +
           state->mPointCacheResidentBytes;
  }
  mResources->setManagedUsage(ram, gpu);
}

void NativeViewport::refreshResourceBudgets() {
  if (meshPagingQaEnabled()) {
    static qint64 lastTickMs = -1000;
    static int observations = 0;
    const qint64 now = mResourceClock.elapsed();
    if (observations < 96 && now - lastTickMs >= 1000) {
      lastTickMs = now;
      ++observations;
      qInfo() << "MESH_BUDGET_TICK_QA:" << "elapsed ms" << now
              << "timer active/from timer" << (mResourceBudgetTimer && mResourceBudgetTimer->isActive())
              << (sender() == mResourceBudgetTimer) << "rebalancing/processing"
              << mResourceRebalancing << mProcessingActive
              << "context valid/current" << (context() && context()->isValid())
              << (context() && QOpenGLContext::currentContext() == context());
    }
  }
  updateManagedResourceUsage();
  SystemResourceSnapshot snapshot;
  const bool currentAlready = context() && QOpenGLContext::currentContext() == context();
  if (context() && context()->isValid()) {
    if (!currentAlready) makeCurrent();
    snapshot = SystemResourceProbe::probe(context());
    if (!currentAlready) doneCurrent();
  } else {
    snapshot = SystemResourceProbe::probe();
  }
  mResources->updateSnapshot(snapshot);
  const auto status = mResources->status();
  mMeshCacheGpuBudgetBytes = static_cast<qsizetype>(status.effectiveGpuBudgetBytes);
  mPointCacheGpuBudgetBytes = mMeshCacheGpuBudgetBytes;
  if (context() && context()->isValid()) {
    const bool alreadyCurrent = QOpenGLContext::currentContext() == context();
    if (!alreadyCurrent) makeCurrent();
    auto states = mSceneStates;
    if (!states.contains(mScene)) states.append(mScene);
    trimHiddenPageCaches(0);
    for (const auto &state : states) {
      QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
      evictMeshCacheUntilFits(0);
      evictPointCacheUntilFits(0);
      state->mMeshCacheFailedNodes.clear();
      state->mPointCacheFailedNodes.clear();
    }
    if (!alreadyCurrent) doneCurrent();
  }
  // Page caches are disposable. Trim them against this probe's target before
  // deciding whether an otherwise-fitting complete mesh needs a costly reload.
  rebalanceResourceResidency();
  emit resourceBudgetStatusChanged();
}

qsizetype NativeViewport::sharedPageGpuBudget() const {
  const auto status = mResources->status();
  qint64 cacheBytes = 0;
  qint64 owners = 0;
  auto states = mSceneStates;
  if (!states.contains(mScene)) states.append(mScene);
  for (const auto &state : states) {
    cacheBytes += state->mMeshCacheResidentBytes + state->mPointCacheResidentBytes;
    if (state->visible && (state->mMeshCache.isValid() || state->mPointCache.isValid())) ++owners;
  }
  const qint64 mandatory = std::max<qint64>(0, status.managedGpuBytes - cacheBytes);
  const qint64 pool = std::max<qint64>(0,
      // Use the target captured by the last hardware probe for this trim pass.
      // A freed page is not immediately credited to an old free-memory sample;
      // recomputing that conservative target after every deletion would keep
      // lowering it and evict the entire cache instead of only the shortfall.
      qint64(mMeshCacheGpuBudgetBytes) - mandatory - status.reservedGpuBytes);
  // Hidden pages stay warm while there is room, but cannot reduce a visible
  // layer's fair share. Actual allocations still pass the shared controller;
  // trimHiddenPageCaches physically releases lower-priority pages on demand.
  if (!mScene->visible)
    return mScene->mMeshCacheResidentBytes + mScene->mPointCacheResidentBytes;
  return static_cast<qsizetype>(pool / std::max<qint64>(1, owners));
}

void NativeViewport::trimHiddenPageCaches(const qsizetype requiredBytes) {
  if (!context() || !context()->isValid()) return;
  const bool alreadyCurrent = QOpenGLContext::currentContext() == context();
  if (!alreadyCurrent) makeCurrent();
  updateManagedResourceUsage();
  const auto observed = mResources->status();
  qint64 needed = std::max<qint64>(0, observed.managedGpuBytes + observed.reservedGpuBytes +
      std::max<qsizetype>(0, requiredBytes) - qint64(mMeshCacheGpuBudgetBytes));
  auto states = mSceneStates;
  if (!states.contains(mScene)) states.append(mScene);
  for (const auto &state : states) {
    if (needed <= 0) break;
    if (state->visible) continue;
    QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
    while (needed > 0 && !state->mFullResolutionPointGpuChunks.isEmpty()) {
      const auto oldest = std::min_element(state->mFullResolutionPointGpuChunks.cbegin(),
          state->mFullResolutionPointGpuChunks.cend(), [](const auto &a, const auto &b) {
            return a.lastUsedFrame < b.lastUsedFrame;
          });
      const auto chunk = state->mFullResolutionPointGpuChunks.takeAt(
          oldest - state->mFullResolutionPointGpuChunks.cbegin());
      if (chunk.buffer) glDeleteBuffers(1, &chunk.buffer);
      if (chunk.vertexArray) glDeleteVertexArrays(1, &chunk.vertexArray);
      state->mPointCacheResidentBytes -= chunk.byteCount;
      state->mUploadedFullResolutionPointCount -= chunk.pointCount;
      needed -= chunk.byteCount;
      updateManagedResourceUsage();
    }
    while (needed > 0 && !state->mFullResolutionMeshGpuChunks.isEmpty()) {
      const auto oldest = std::min_element(state->mFullResolutionMeshGpuChunks.cbegin(),
          state->mFullResolutionMeshGpuChunks.cend(), [](const auto &a, const auto &b) {
            return a.lastUsedFrame < b.lastUsedFrame;
          });
      const auto chunk = state->mFullResolutionMeshGpuChunks.takeAt(
          oldest - state->mFullResolutionMeshGpuChunks.cbegin());
      if (chunk.indexBuffer) glDeleteBuffers(1, &chunk.indexBuffer);
      if (chunk.vertexBuffer) glDeleteBuffers(1, &chunk.vertexBuffer);
      if (chunk.vertexArray) glDeleteVertexArrays(1, &chunk.vertexArray);
      state->mMeshCacheResidentBytes -= chunk.byteCount;
      state->mUploadedFullResolutionMeshTriangleCount -= chunk.indexCount / 3;
      needed -= chunk.byteCount;
      updateManagedResourceUsage();
    }
  }
  if (!alreadyCurrent) doneCurrent();
}

void NativeViewport::finishResourceUpload() {
  updateManagedResourceUsage();
  if (!mScene->resourceLoadReservation || mScene->mPointUploadPending ||
      mScene->mMeshUploadPending || mScene->mMeshTextureUploadPending) return;
  mScene->resourceLoadReservation.reset();
  emit resourceBudgetStatusChanged();
}

void NativeViewport::deferHiddenResourceUpload() {
  if (mScene->visible || mScene->resourceLoading || !mScene->resourceLoadReservation) return;
  // CPU results are now owned by SceneState and must become managed usage
  // before the loader's temporary peak reservation is relinquished. No GL
  // storage is allocated for an invisible source just to finish its lease.
  updateManagedResourceUsage();
  (void)mScene->resourceLoadReservation->tryResize(0, 0);
  mScene->resourceLoadReservation.reset();
  mScene->resourceUploadDeferred = mScene->mPointUploadPending || mScene->mMeshUploadPending ||
      mScene->mMeshTextureUploadPending;
}

bool NativeViewport::admitDeferredResourceUpload() {
  if (!mScene->resourceUploadDeferred) return true;
  if (!mScene->visible || mScene->resourceLoading) return false;
  qint64 pendingGpu = 0;
  const auto appendCost = [&](qsizetype count, qsizetype stride) {
    if (count < 0 || stride < 0 || (stride > 0 && count > std::numeric_limits<qint64>::max() / stride)) return false;
    const qint64 bytes = qint64(count) * qint64(stride);
    if (bytes > std::numeric_limits<qint64>::max() - pendingGpu) return false;
    pendingGpu += bytes;
    return true;
  };
  // This guards the native staging buffers accounted by this controller.
  // Gaussian attribute/CUDA allocations retain their existing independent
  // accounting; this is not a new global Gaussian VRAM-budget guarantee.
  bool valid = !mScene->mPointUploadPending || appendCost(mScene->mPendingVertices.size(), sizeof(PointCloudVertex));
  if (mScene->mMeshUploadPending)
    valid = valid && appendCost(mScene->mPendingMeshVertices.size(), sizeof(MeshVertex)) &&
        appendCost(mScene->mPendingMeshIndices.size(), sizeof(quint32));
  if (mScene->mMeshTextureUploadPending && !mScene->mPendingMeshTexture.isNull()) {
    const auto &image = mScene->mPendingMeshTexture;
    int width = image.width(), height = image.height();
    // uploadPendingMeshTexture generates the complete RGBA8 mip chain. Admit
    // every level, not just the base image; hardware downsizing only reduces
    // this conservative estimate of the original dimensions.
    do {
      valid = valid && appendCost(height, qint64(width) * 4);
      if (width == 1 && height == 1) break;
      width = std::max(1, width / 2);
      height = std::max(1, height / 2);
    } while (valid);
  }
  if (!valid) {
    mScene->mSceneLoadMessage = QCoreApplication::translate("Workbench", "Insufficient GPU memory budget to show this model.");
    return false;
  }
  if (mScene->mHasMesh && !mScene->mMeshCache.isValid()) {
    const qint64 residentGpu = mScene->resourcePointGpuBytes + mScene->resourceMeshGpuBytes + mScene->resourceTextureGpuBytes;
    pendingGpu = std::max(pendingGpu, std::max<qint64>(0, mScene->resourceResidentGpuBytes - residentGpu));
  }
  trimHiddenPageCaches(pendingGpu);
  updateManagedResourceUsage();
  auto reservation = mResources->createReservation();
  if (!reservation->tryResize(0, pendingGpu)) {
    mScene->mSceneLoadMessage = QCoreApplication::translate("Workbench", "Insufficient GPU memory budget to show this model.");
    if (mScene->mHasMesh && !mScene->mMeshCache.isValid()) {
      // The previously hidden complete mesh may no longer fit alongside the
      // visible collection. Use the existing admitted paged loader, not an
      // unchecked VBO allocation. Mesh sources have no point-crop edit model.
      mScene->resourceNeedsReload = true;
      mScene->resourceForcePagedRetry = true;
      mScene->resourceRetryAfterMs = mResourceClock.elapsed();
    }
    return false;
  }
  mScene->resourceLoadReservation = std::move(reservation);
  mScene->resourceUploadDeferred = false;
  mScene->resourceNeedsReload = false;
  mScene->resourceForcePagedRetry = false;
  mScene->mSceneLoadMessage.clear();
  emit resourceBudgetStatusChanged();
  return true;
}

void NativeViewport::discardResidentMeshStorage() {
  const bool alreadyCurrent = QOpenGLContext::currentContext() == context();
  if (!alreadyCurrent) makeCurrent();
  releaseSceneBuffers();
  if (!alreadyCurrent) doneCurrent();
  mScene->mPreviewVertices = {};
  mScene->mSourcePositions = {};
  mScene->mPendingVertices = {};
  mScene->mPendingMeshVertices = {};
  mScene->mPendingMeshIndices = {};
  mScene->mPendingMeshTexture = {};
  mScene->mRenderedPointCount = 0;
  mScene->mRenderedMeshIndexCount = 0;
  mScene->resourceLoadReservation.reset();
  mScene->resourceUploadDeferred = false;
  mScene->mPointUploadPending = false;
  mScene->mMeshUploadPending = false;
  mScene->mMeshTextureUploadPending = false;
  updateManagedResourceUsage();
}

void NativeViewport::rebalanceResourceResidency() {
  if (mResourceRebalancing || mProcessingActive || !context() || !context()->isValid()) return;
  QScopedValueRollback<bool> guard(mResourceRebalancing, true);
  auto states = mSceneStates;
  if (!states.contains(mScene)) states.append(mScene);
  const auto active = mScene;
  for (const auto &state : states) {
    // Hiding is not an instruction to reload or promote a background source.
    // Retain its existing representation until reveal or real visible demand.
    if (!state->visible) continue;
    const auto qaObservation = [this, &state](const char *stage,
                                             const ResourceReservation *lease = nullptr) {
      if (!meshPagingQaEnabled()) return;
      // Diagnostic only: at most one observation per source/stage each second.
      static QHash<QString, qint64> lastObservation;
      const QString key = state->mRequestedScenePath + QLatin1Char('\n') + QString::fromLatin1(stage);
      const qint64 now = mResourceClock.elapsed();
      if (!lastObservation.contains(key) && lastObservation.size() >= 128) return;
      if (lastObservation.contains(key) && now - lastObservation.value(key) < 1000) return;
      lastObservation.insert(key, now);
      const auto observed = mResources->status();
      const auto policy = mResources->policy();
      const qint64 replaceableGpu = state->resourcePointGpuBytes + state->resourceMeshGpuBytes +
          state->resourceTextureGpuBytes + state->mMeshCacheResidentBytes + state->mPointCacheResidentBytes;
      const qint64 otherGpu = std::max<qint64>(0, observed.managedGpuBytes - replaceableGpu);
      const qint64 remainingGpu = std::max<qint64>(0,
          observed.effectiveGpuBudgetBytes - otherGpu - observed.reservedGpuBytes);
      qInfo() << "MESH_RESIDENCY_QA:" << state->id << state->mRequestedScenePath << stage
              << "mesh/paged/loading/loadLease/reload/forcePaged/uploadFailed/storageSupported"
              << state->mHasMesh << state->mMeshCache.isValid() << state->resourceLoading
              << bool(state->resourceLoadReservation) << state->resourceNeedsReload << state->resourceForcePagedRetry
              << state->resourceResidentUploadFailed << state->resourceResidentStorageSupported
              << "retry remaining ms" << state->resourceRetryAfterMs - now
              << "estimate RAM/GPU" << state->resourceResidentRamPeakBytes << state->resourceResidentGpuBytes
              << "replaceable/other/remaining GPU" << replaceableGpu << otherGpu << remainingGpu
              << "managed RAM/GPU" << observed.managedRamBytes << observed.managedGpuBytes
              << "reserved RAM/GPU" << observed.reservedRamBytes << observed.reservedGpuBytes
              << "effective RAM/GPU/captured GPU" << observed.effectiveRamBudgetBytes
              << observed.effectiveGpuBudgetBytes << mMeshCacheGpuBudgetBytes
              << "policy mode/RAM MiB/GPU MiB" << int(policy.mode) << policy.ramLimitMiB << policy.gpuLimitMiB
              << "temporary lease refused" << (lease && lease->admissionWasRefused());
    };
    qaObservation("candidate");
    if (state == mProcessingScene && mProcessingPreviewPaused) continue;
    if (state->resourceNeedsReload && !state->resourceLoading &&
        state->resourceRetryAfterMs <= mResourceClock.elapsed() &&
        !state->mRequestedScenePath.isEmpty()) {
      QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
      QScopedValueRollback<bool> background(mRenderingInactiveScene, state != active);
      ++state->mSceneGeneration;
      state->resourceRetryAfterMs = mResourceClock.elapsed() + 10000;
      qaObservation("reload-needed");
      if (state->resourceForcePagedRetry) discardResidentMeshStorage();
      startSceneLoad(state->mRequestedScenePath, false, true, state->resourceForcePagedRetry);
      continue;
    }
    if (state->resourceUploadDeferred) {
      QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
      QScopedValueRollback<bool> background(mRenderingInactiveScene, state != active);
      if (admitDeferredResourceUpload()) update();
      continue;
    }
    if (!state->mHasMesh || state->resourceLoading || state->resourceLoadReservation ||
        state->mScenePath.isEmpty() || state->resourceRetryAfterMs > mResourceClock.elapsed()) continue;
    updateManagedResourceUsage();
    auto budget = mResources->status();
    // Compare GPU usage with the target captured before this trim pass. A freed
    // cache page is not immediately credited by an old hardware observation;
    // recomputing its shortfall here would subtract it twice and demote a mesh
    // even after the requested amount of disposable storage was released.
    const bool pressure = budget.managedGpuBytes > qint64(mMeshCacheGpuBudgetBytes) ||
                          budget.managedRamBytes > budget.effectiveRamBudgetBytes;
    QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
    QScopedValueRollback<bool> background(mRenderingInactiveScene, state != active);
    if (pressure && !state->mMeshCache.isValid()) {
      // Relinquish old storage before reserving a smaller representation. The
      // model identity, transform and navigation state stay intact.
      discardResidentMeshStorage();
      ++state->mSceneGeneration;
      state->resourceRetryAfterMs = mResourceClock.elapsed() + 5000;
      startSceneLoad(state->mScenePath, false, true, true);
    } else if (!pressure && !state->resourceResidentUploadFailed &&
               state->resourceResidentStorageSupported && state->mMeshCache.isValid() &&
               state->resourceResidentRamPeakBytes > 0 && state->resourceResidentGpuBytes > 0) {
      const qint64 replaceable = state->resourcePointGpuBytes + state->resourceMeshGpuBytes +
          state->resourceTextureGpuBytes + state->mMeshCacheResidentBytes + state->mPointCacheResidentBytes;
      trimHiddenPageCaches(std::max<qint64>(0, state->resourceResidentGpuBytes - replaceable));
      budget = mResources->status();
      auto reservation = mResources->createReservation();
      if (!reservation->tryResize(state->resourceResidentRamPeakBytes,
                                  state->resourceResidentGpuBytes)) {
        qaObservation("complete-admission-refused", reservation.get());
        // Promotion replaces this model's disposable page storage; requiring
        // both representations to coexist would let a full cache permanently
        // starve a resident mesh that fits the same total budget. Preflight the
        // final GPU cost, reserve the CPU peak, then physically release old GPU
        // storage before the normal atomic admission (never credit unfreed VRAM).
        const qint64 replaceableGpu = state->resourcePointGpuBytes +
            state->resourceMeshGpuBytes + state->resourceTextureGpuBytes +
            state->mMeshCacheResidentBytes + state->mPointCacheResidentBytes;
        const qint64 otherGpu = std::max<qint64>(0,
            budget.managedGpuBytes - replaceableGpu);
        const qint64 remainingGpu = std::max<qint64>(0,
            budget.effectiveGpuBudgetBytes - otherGpu - budget.reservedGpuBytes);
        if (state->resourceResidentGpuBytes > remainingGpu ||
            !reservation->tryResize(state->resourceResidentRamPeakBytes, 0)) {
          qaObservation("replacement-admission-refused", reservation.get());
          continue;
        }
        qaObservation("replacement-CPU-admitted", reservation.get());
        ++state->mSceneGeneration;
        state->resourceLoading = true;
        state->mPendingMeshCachePages.clear();
        state->mPendingPointCachePages.clear();
        state->resourceMeshPageReservations.clear();
        state->resourcePointPageReservations.clear();
        state->mMeshCacheReadsInFlight.clear();
        state->mPointCacheReadsInFlight.clear();
        ++state->mPointCacheReadEpoch;
        discardResidentMeshStorage();
        refreshResourceBudgets();
        // If external pressure changed during replacement, the loader can
        // safely choose paging with this CPU lease instead of overcommitting.
        (void)reservation->tryResize(state->resourceResidentRamPeakBytes,
                                     state->resourceResidentGpuBytes);
        qaObservation("replacement-after-release", reservation.get());
      } else {
        qaObservation("complete-admitted", reservation.get());
        ++state->mSceneGeneration;
      }
      state->resourceRetryAfterMs = mResourceClock.elapsed() + 5000;
      qaObservation("promotion-load", reservation.get());
      startSceneLoad(state->mScenePath, false, true, false, std::move(reservation));
    }
  }
}

void NativeViewport::releaseSceneBuffers() {
  mScene->gaussianGpu.release();
  mScene->indexedGaussians = false;
  mScene->sortDirectionValid = false;
  releaseFullResolutionPointCloud();
  releaseFullResolutionMesh();
  releaseMeshTexture();
  mScene->mGaussianVertexArray.destroy();
  mScene->mMeshVertexArray.destroy();
  mScene->mPointVertexArray.destroy();
  mScene->mMeshIndexBuffer.destroy();
  mScene->mMeshVertexBuffer.destroy();
  mScene->mPointBuffer.destroy();
  mScene->resourcePointGpuBytes = 0;
  mScene->resourceMeshGpuBytes = 0;
  mScene->buffersInitialized = false;
}

const NativeViewport::SceneState &NativeViewport::coordinateReferenceScene() const {
  // The collection's persisted order, not the selection, owns its reference.
  // Task observations are an isolated layer and retain their own frame.
  if (mScene == mProcessingScene || mSceneStates.isEmpty()) return *mScene;
  return *mSceneStates.first();
}

SceneCoordinateInfo NativeViewport::sceneReferenceCoordinates() const {
  return coordinateReferenceScene().mSceneCoordinates;
}

double NativeViewport::sourceToReferenceUnitScale(const SceneState &state) const {
  const auto &source = state.mSceneCoordinates;
  const auto reference = sceneReferenceCoordinates();
  const auto mapping=sceneDisplayMapping(source,reference);
  if(mapping.verifiedAlignment)return mapping.lengthScale*source.displayScale/reference.displayScale;
  const auto sourceRatio = sceneMetresPerCoordinate(source);
  const auto referenceRatio = sceneMetresPerCoordinate(reference);
  if (!sourceRatio || !referenceRatio)
    return 1.0; // Unknown units are not silently assumed to be metres.
  return *sourceRatio / *referenceRatio;
}

double NativeViewport::sourceToSceneUnitScale() const {
  return sourceToReferenceUnitScale(*mScene);
}

QString NativeViewport::sceneCoordinateSummary() const {
  const auto reference = sceneReferenceCoordinates();
  if (mSceneStates.isEmpty() && mScene->mRequestedScenePath.isEmpty()) return QStringLiteral("-");
  if (!reference.valid) return coordinateReferenceScene().resourceLoading
      ? QCoreApplication::translate("Workbench", "坐标信息加载中")
      : QCoreApplication::translate("Workbench", "坐标关系未确认");
  bool unknown = !reference.unitDeclared || reference.coordinateReferenceSystem.isEmpty();
  bool verified = !displayedSceneStates().isEmpty();
  for (const auto &state : displayedSceneStates()) {
    const auto &source = state->mSceneCoordinates;
    if (!source.valid) return state->resourceLoading
        ? QCoreApplication::translate("Workbench", "坐标信息加载中")
        : QCoreApplication::translate("Workbench", "坐标关系未确认");
    const bool sameCanonical=sceneDisplayMapping(source, reference).verifiedAlignment;
    verified &= sameCanonical;
    if (!sameCanonical && !source.coordinateReferenceSystem.isEmpty() && !reference.coordinateReferenceSystem.isEmpty() &&
        source.coordinateReferenceSystem != reference.coordinateReferenceSystem)
      return QCoreApplication::translate("Workbench", "CRS 不同 · 需要配准或重投影");
    unknown |= !source.unitDeclared || source.coordinateReferenceSystem.isEmpty();
  }
  if(verified)return QCoreApplication::translate("Workbench", "同源坐标链已验证 · 框架与单位已统一（不代表精度验证）");
  return unknown ? QCoreApplication::translate("Workbench", "坐标关系未确认") :
      QCoreApplication::translate("Workbench", "相同 CRS · 单位已统一（不代表精度验证）");
}

QString NativeViewport::sceneCoordinateDetail() const {
  if (mSceneStates.isEmpty() && mScene->mRequestedScenePath.isEmpty()) return {};
  const auto &reference = coordinateReferenceScene();
  QString result = QCoreApplication::translate("Workbench",
      "场景参考：%1\n场景单位：%2\n当前源单位：%3\n源坐标→场景单位倍率：%4\n共用世界坐标显示，不按模型大小自动归一化。对象位移、旋转和缩放独立保留。")
      .arg(QFileInfo(reference.mRequestedScenePath).fileName(),
           sceneLengthUnitDescription(reference.mSceneCoordinates),
           sceneLengthUnitDescription(mScene->mSceneCoordinates),
           QString::number(sourceToSceneUnitScale(), 'g', 12));
  bool unknown = !reference.mSceneCoordinates.unitDeclared ||
      reference.mSceneCoordinates.coordinateReferenceSystem.isEmpty();
  bool conflict = false;
  bool verified = true;
  for (const auto &state : displayedSceneStates()) {
    const auto &source = state->mSceneCoordinates;
    verified &= sceneDisplayMapping(source, reference.mSceneCoordinates).verifiedAlignment;
    unknown |= !source.unitDeclared || source.coordinateReferenceSystem.isEmpty();
    conflict |= !source.coordinateReferenceSystem.isEmpty() &&
        !reference.mSceneCoordinates.coordinateReferenceSystem.isEmpty() &&
        source.coordinateReferenceSystem != reference.mSceneCoordinates.coordinateReferenceSystem;
  }
  if(!verified) result += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "存在不同来源或未验证坐标链；仅换算长度单位，不自动配准原点或旋转。");
  if(!mScene->mSceneCoordinates.validationError.isEmpty())result += QLatin1Char('\n') + mScene->mSceneCoordinates.validationError;
  if (verified) result += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "同源坐标链已验证 · 框架与单位已统一（不代表精度验证）");
  result += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "当前文件框架：%1；参考文件框架：%2。")
      .arg(mScene->mSceneCoordinates.provenance.value(QStringLiteral("coordinate_frame")).toString(QStringLiteral("-")),
          reference.mSceneCoordinates.provenance.value(QStringLiteral("coordinate_frame")).toString(QStringLiteral("-")));
  if (unknown && !verified) result += QLatin1Char('\n') + QCoreApplication::translate("Workbench",
      "未声明单位或 CRS，不能确认模型已配准；保留未知单位的原始数值，不猜测比例或自动对齐。");
  if (conflict && !verified) result += QLatin1Char('\n') + QCoreApplication::translate("Workbench",
      "CRS 不一致；这里只统一可识别的长度单位，不执行坐标投影转换。请先在配准或测绘工具中建立明确变换。");
  return result;
}

bool NativeViewport::collectionReferenceBounds(QVector3D &minimum,
                                               QVector3D &maximum) const {
  bool found = false;
  const auto &reference = coordinateReferenceScene();
  // The grid is a retained project reference, not a bound of the currently
  // drawable subset. Match referencePlaneElevation(): an eye toggle must not
  // change the base plane, including when the last visible object is hidden.
  auto states = mScene == mProcessingScene
      ? QList<std::shared_ptr<SceneState>>{mScene} : mSceneStates;
  if (states.isEmpty()) states.append(mScene);
  for (const auto &state : states) {
    const auto &coordinates = state->mSceneCoordinates;
    if (!coordinates.valid) continue;
    const auto low = coordinates.localMinimum();
    const auto high = coordinates.localMaximum();
    // Grid position and reference-axis length use source coordinates. An
    // editable TRS must never move the coordinate reference with the model.
    const auto matrix = sceneDisplayTransform(*state, reference);
    for (int corner = 0; corner < 8; ++corner) {
      const auto point = matrix.map(QVector3D(corner & 1 ? high.x() : low.x(),
          corner & 2 ? high.y() : low.y(), corner & 4 ? high.z() : low.z()));
      if (!found) { minimum = maximum = point; found = true; }
      else for (int axis = 0; axis < 3; ++axis) {
        minimum[axis] = std::min(minimum[axis], point[axis]);
        maximum[axis] = std::max(maximum[axis], point[axis]);
      }
    }
  }
  return found;
}

QMatrix4x4 NativeViewport::sceneDisplayTransform(const SceneState &from,
                                                const SceneState &to) const {
  return sceneDisplayMapping(from.mSceneCoordinates,to.mSceneCoordinates).localToReference;
}

void NativeViewport::mapViewToScene(const QMatrix4x4 &mapping) {
  const float lengthScale=mapping.mapVector(QVector3D(1,0,0)).length();
  const auto mappedAngles=[&](const OrbitAngles &angles) {
    const OrbitFrame frame=orbitFrame(angles);
    const auto offset=mapping.mapVector(frame.cameraOffsetDirection).normalized();
    const auto up=mapping.mapVector(frame.upDirection).normalized();
    if((offset-frame.cameraOffsetDirection).lengthSquared()<1e-12F &&
        (up-frame.upDirection).lengthSquared()<1e-12F)return angles;
    OrbitAngles result;
    result.pitchDegrees=std::asin(std::clamp(offset.z(),-1.F,1.F))*180.F/kPi;
    result.yawDegrees=std::atan2(offset.x(),offset.y())*180.F/kPi;
    const auto baseUp=orbitFrame({result.yawDegrees,result.pitchDegrees,0.F}).upDirection;
    result.rollDegrees=std::atan2(QVector3D::dotProduct(offset,QVector3D::crossProduct(baseUp,up)),
        QVector3D::dotProduct(baseUp,up))*180.F/kPi;
    return result;
  };
  const auto beforeAngles=viewOrbitAngles();
  const auto angles=mappedAngles(beforeAngles);
  if (angles != beforeAngles) mAxisView = NavigationAxis::None;
  mTarget=mapping.map(mTarget);
  mDistance*=lengthScale;mYawDegrees=angles.yawDegrees;mPitchDegrees=angles.pitchDegrees;mRollDegrees=angles.rollDegrees;
  if(mCameraViewActive&&mSourceCameraClipping)for(auto &length:*mSourceCameraClipping)length*=lengthScale;
  if(mStoredCameraView) {
    auto &stored=*mStoredCameraView;
    const auto savedAngles=mappedAngles({stored.yawDegrees,stored.pitchDegrees,stored.rollDegrees});
    if (savedAngles != OrbitAngles{stored.yawDegrees,stored.pitchDegrees,stored.rollDegrees})
      stored.axisView = NavigationAxis::None;
    stored.target=mapping.map(stored.target);stored.distance*=lengthScale;
    stored.yawDegrees=savedAngles.yawDegrees;stored.pitchDegrees=savedAngles.pitchDegrees;stored.rollDegrees=savedAngles.rollDegrees;
  }
}

void NativeViewport::publishActiveSceneState() {
  emit selectionBusyChanged(mScene->mSelectionBusy);
  emit sceneCoordinatesChanged();
  emit renderModeChanged(mScene->mRenderMode);
  emit meshRenderingAvailabilityChanged(meshRenderingAvailable());
  emit gaussianRenderingAvailabilityChanged(gaussianRenderingAvailable());
  emit cameraTrajectoryChanged(cameraCount(), mScene->mCameraTrajectory.invalidCameraCount(),
      mScene->mCameraGeometry.decimated, mScene->mCameraTrajectory.sourcePath(),
      mScene->mCameraTrajectory.error());
  notifyEditState();
  notifyModelInteractionState();
  updateFrameRefreshPolicy();
  update();
}

void NativeViewport::setSceneObjects(const QList<SceneObject> &objects,
                                    const QString &activeId) {
  bool qaRemovingObjects = false;
  if (meshPagingQaEnabled()) {
    qaRemovingObjects = std::any_of(mSceneStates.cbegin(), mSceneStates.cend(), [&](const auto &state) {
      return std::none_of(objects.cbegin(), objects.cend(), [&](const auto &object) { return object.id == state->id; });
    });
    if (qaRemovingObjects) {
      qInfo() << "MESH_COLLECTION_REMOVE_QA:" << "begin elapsed ms" << mResourceClock.elapsed()
              << "old/new objects" << mSceneStates.size() << objects.size()
              << "active/new active" << mScene->id << activeId
              << "timer active/rebalancing/processing" << (mResourceBudgetTimer && mResourceBudgetTimer->isActive())
              << mResourceRebalancing << mProcessingActive
              << "context valid/current" << (context() && context()->isValid())
              << (context() && QOpenGLContext::currentContext() == context());
    }
  }
  if (mProcessingActive) return;
  const bool visibilityChanged = std::any_of(objects.cbegin(), objects.cend(), [&](const auto &object) {
    const auto found = std::find_if(mSceneStates.cbegin(), mSceneStates.cend(),
        [&](const auto &state) { return state->id == object.id; });
    return found != mSceneStates.cend() && (*found)->visible != object.visible;
  });
  if (visibilityChanged) {
    if (mModelDragActive) finishModelTransform(false);
    ++mSelectionRequestEpoch;
    mSelectionGestureActive = false;
    mSelectionPath.clear();
    mTransformGizmoHover = {};
    mObservationDragActive = false;
  }
  const bool collectionChanged = objects.size() != mSceneStates.size() ||
      std::any_of(objects.cbegin(), objects.cend(), [&](const auto &object) {
        return std::none_of(mSceneStates.cbegin(), mSceneStates.cend(), [&](const auto &state) {
          return state->id == object.id && state->mRequestedScenePath == object.path;
        });
      });
  if (!objects.isEmpty() && !collectionChanged && mScene == mProcessingScene && mProcessingReferenceScene &&
      activeId == mProcessingReferenceScene->id) {
    // Synchronizing an unchanged project must not choose the reference view.
    // Apply reference TRS updates without touching the independent task view.
    QSignalBlocker signalBlocker(this);
    for (const auto &object : objects) {
      const auto found = std::find_if(mSceneStates.cbegin(), mSceneStates.cend(),
          [&](const auto &state) { return state->id == object.id; });
      if (found == mSceneStates.cend()) continue;
      const auto &state = *found;
      state->visible = object.visible;
      if (!state->visible) {
        state->mPendingPointCachePages.clear();
        state->mPendingMeshCachePages.clear();
        state->resourcePointPageReservations.clear();
        state->resourceMeshPageReservations.clear();
        QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
        deferHiddenResourceUpload();
      }
      QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
      QScopedValueRollback<bool> background(mRenderingInactiveScene, true);
      state->sourceTranslation = object.translation;
      setModelTransform(object.translation * static_cast<float>(state->mSceneCoordinates.displayScale),
                        object.rotation, object.scale);
    }
    update();
    return;
  }
  if ((mScene->id != activeId || collectionChanged) && mModelDragActive) finishModelTransform(false);
  if (collectionChanged) resetModelTransformHistory();
  const auto oldActive = mScene;
  const auto previewToAdopt = hasProcessingPreview() ? mProcessingScene : nullptr;
  const bool wasProcessingPreview = mProcessingPreviewPresent;
  const QVector3D oldTarget = mTarget;
  const float oldDistance = mDistance;
  QList<std::shared_ptr<SceneState>> retained;
  std::shared_ptr<SceneState> active;
  bool changedSource = false;
  {
    QSignalBlocker signalBlocker(this);
    for (const auto &object : objects) {
      auto found = std::find_if(mSceneStates.cbegin(), mSceneStates.cend(),
          [&](const auto &state) { return state->id == object.id; });
      // A staged result is a new object. Never share its SceneState with an
      // original object that remains in the collection (including GPU buffers).
      const bool adoptPreview = previewToAdopt && object.id == activeId &&
          (found == mSceneStates.cend() || (*found)->mRequestedScenePath != object.path);
      auto state = adoptPreview ? previewToAdopt : found == mSceneStates.cend() ? std::make_shared<SceneState>() : *found;
      state->id = object.id;
      state->visible = object.visible;
      if (!state->visible) {
        state->mPendingPointCachePages.clear();
        state->mPendingMeshCachePages.clear();
        state->resourcePointPageReservations.clear();
        state->resourceMeshPageReservations.clear();
        QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
        deferHiddenResourceUpload();
      }
      retained.append(state);
      const bool isActive = object.id == activeId;
      if (isActive) active = state;
      QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
      QScopedValueRollback<bool> background(mRenderingInactiveScene, !isActive);
      if (adoptPreview) {
        // A validated final source supersedes observations already being read
        // or queued. Keep the last valid geometry/camera until it is ready.
        state->queuedPreviewPath.clear();
        state->queuedPreviewCount = 0;
        if (state->mRequestedScenePath != object.path) state->previewLoadBusy = false;
      }
      if (state->mRequestedScenePath != object.path) {
        changedSource |= isActive;
        if (adoptPreview || (isActive && !state->mScenePath.isEmpty() &&
            (state->mPreviewPointCount > 0 || state->mPreviewTriangleCount > 0))) {
          // Publishing a validated result is not an observation request. Its
          // now-persistent object must load even if observation was paused.
          QScopedValueRollback<bool> finalPublication(mProcessingPreviewPaused,
              adoptPreview ? false : mProcessingPreviewPaused);
          loadContinuousScene(object.path, object.vertexCount);
        } else setScene(object.path, object.vertexCount);
      }
      state->sourceTranslation = object.translation;
      setModelTransform(object.translation * static_cast<float>(state->mSceneCoordinates.displayScale),
                        object.rotation, object.scale);
      if (adoptPreview) mProcessingPreviewPresent = false;
    }
    const bool hasRemovedScene = std::any_of(mSceneStates.cbegin(), mSceneStates.cend(),
        [&](const auto &state) { return !retained.contains(state); });
    if (hasRemovedScene && context() != nullptr && context()->isValid()) {
      auto *previousContext = QOpenGLContext::currentContext();
      auto *previousSurface = previousContext ? previousContext->surface() : nullptr;
      auto *viewportContext = context();
      const bool canRestoreContext = !previousContext || previousContext == viewportContext || previousSurface;
      const auto restoreContext = qScopeGuard([&] {
        if (previousContext && previousSurface) {
          if (QOpenGLContext::currentContext() != previousContext ||
              previousContext->surface() != previousSurface)
            previousContext->makeCurrent(previousSurface);
        } else if (!previousContext && QOpenGLContext::currentContext() == viewportContext) {
          viewportContext->doneCurrent();
        }
      });
      if (canRestoreContext && QOpenGLContext::currentContext() != viewportContext) makeCurrent();
      for (const auto &removed : mSceneStates) {
        if (retained.contains(removed)) continue;
        if (!canRestoreContext || QOpenGLContext::currentContext() != viewportContext) break;
        QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, removed);
        releaseSceneBuffers();
      }
    }
    mSceneStates = retained;
    mScene = active ? active : !retained.isEmpty() ? retained.first() : std::make_shared<SceneState>();
    if (retained.contains(mProcessingScene)) {
      mProcessingScene.reset();
      mProcessingHasFrame = false;
      mProcessingPreviewVisible = false;
    } else if (retained.isEmpty() || changedSource) {
      discardProcessingScene();
      if (retained.isEmpty()) mProcessingStage.clear();
    }
    mProcessingReferenceScene = mScene;
    // Association can activate an already loaded object without adopting the
    // observation state (for example, when other original objects coexist).
    if (!retained.isEmpty()) mProcessingPreviewPresent = false;
    if (oldActive != mScene && !changedSource && !retained.isEmpty()) {
      const QMatrix4x4 mapping = sceneDisplayTransform(*oldActive, *mScene);
      const bool sameCanonical=sceneDisplayMapping(oldActive->mSceneCoordinates,mScene->mSceneCoordinates).verifiedAlignment;
      if(mCameraViewActive&&!sameCanonical) {
        // Explicitly restore the ordinary view, rather than retaining only
        // camera FOV while silently dropping its principal point and skew.
        toggleCameraView();mSourceCameraIntrinsics.reset();mSourceCameraClipping.reset();
      } else { mTarget=oldTarget;mDistance=oldDistance; }
      mapViewToScene(mapping);
    }
  }
  if (oldActive != mScene || changedSource || wasProcessingPreview != mProcessingPreviewPresent) {
    mSelectionGestureActive = false;
    mSelectionPath.clear();
    mPressedButtons = Qt::NoButton;
    const bool keepCalibration=mCameraViewActive&&!changedSource&&oldActive&&
        (oldActive==mScene||sceneDisplayMapping(oldActive->mSceneCoordinates,mScene->mSceneCoordinates).verifiedAlignment);
    if(!keepCalibration){
      if(mCameraViewActive&&mStoredCameraView)toggleCameraView();
      mCameraViewActive=false;mStoredCameraView.reset();mSourceCameraIntrinsics.reset();mSourceCameraClipping.reset();
    }
    mModelSelected = !objects.isEmpty() && mScene->visible;
    mSelectedSceneIds.clear();
    if (mModelSelected) mSelectedSceneIds.insert(mScene->id);
    mTransformGizmoHover = {};
    publishActiveSceneState();
    notifySceneSelection();
  }
  QSet<QString> existing;
  for (const auto &state : mSceneStates) {
    if (state->visible) existing.insert(state->id);
  }
  mSelectedSceneIds.intersect(existing);
  mModelSelected = !mSelectedSceneIds.isEmpty();
  if (visibilityChanged) {
    notifySceneSelection();
    notifyEditState();
    notifyModelInteractionState();
    updateFrameRefreshPolicy();
  }
  if (!mProcessingScene && mProcessingPreviewPaused) {
    mProcessingPreviewPaused = false;
    mProcessingPausedImage = {};
    mProcessingPreviewResumePending = false;
    mPausedPreviewCandidatePath.clear();
    mPausedPreviewCandidateCount = 0;
    emit processingPreviewPausedChanged(false);
  }
  emit processingPreviewChanged();
  // Collection changes release or replace shared storage immediately. Do not
  // leave residency admission to a periodic timer that can be delayed while
  // the viewport is continuously composing. The collection/selection transaction
  // and its signal blocker have ended before rebalancing the surviving models.
  if (collectionChanged) refreshResourceBudgets();
  update();
  if (qaRemovingObjects) {
    qInfo() << "MESH_COLLECTION_REMOVE_QA:" << "end elapsed ms" << mResourceClock.elapsed()
            << "objects/active" << mSceneStates.size() << mScene->id
            << "timer active/rebalancing/processing" << (mResourceBudgetTimer && mResourceBudgetTimer->isActive())
            << mResourceRebalancing << mProcessingActive
            << "context valid/current" << (context() && context()->isValid())
            << (context() && QOpenGLContext::currentContext() == context());
  }
}

bool NativeViewport::activateSceneObject(const QString &id) {
  return setSceneSelection({id}, id);
}

bool NativeViewport::sceneObjectVisible(const QString &id) const {
  const auto found = std::find_if(mSceneStates.cbegin(), mSceneStates.cend(),
      [&](const auto &state) { return state->id == id; });
  return found != mSceneStates.cend() && (*found)->visible;
}

qsizetype NativeViewport::visibleSceneObjectCount() const {
  return std::count_if(mSceneStates.cbegin(), mSceneStates.cend(),
      [](const auto &state) { return state->visible; });
}

QStringList NativeViewport::selectedSceneIds() const {
  QStringList result;
  for (const auto &state : mSceneStates) {
    if (mSelectedSceneIds.contains(state->id)) result.append(state->id);
  }
  return result;
}

void NativeViewport::notifySceneSelection() {
  emit sceneSelectionChanged(selectedSceneIds(), mScene->id);
}

void NativeViewport::selectAllModels() {
  QStringList ids;
  for (const auto &state : mSceneStates) {
    if (state->visible && state->mSceneLoadMessage.isEmpty() &&
        (state->mPreviewPointCount > 0 || state->mPreviewTriangleCount > 0)) ids.append(state->id);
  }
  setSceneSelection(ids, mScene->id);
}

bool NativeViewport::setSceneSelection(const QStringList &ids, const QString &activeId) {
  if (mScene == mProcessingScene) return false;
  QSet<QString> selected;
  for (const auto &state : mSceneStates) {
    if (state->visible && ids.contains(state->id)) selected.insert(state->id);
  }
  const bool requestedActiveExists = std::any_of(mSceneStates.cbegin(), mSceneStates.cend(),
      [&](const auto &state) { return state->id == activeId && ids.contains(activeId); });
  QString id = requestedActiveExists ? activeId :
      selected.contains(mScene->id) ? mScene->id : selected.isEmpty() ? mScene->id : *selected.cbegin();
  const auto found = std::find_if(mSceneStates.cbegin(), mSceneStates.cend(),
      [&](const auto &state) { return state->id == id; });
  if (found == mSceneStates.cend() || mScene->mSelectionBusy ||
      (id != mScene->id && hasUnsavedSceneEdits())) return false;
  if (mModelDragActive) finishModelTransform(false);
  const auto next = *found;
  const bool activeChanged = next != mScene;
  const QMatrix4x4 mapping = sceneDisplayTransform(*mScene, *next);
  const bool keepCalibration=mCameraViewActive&&(next==mScene||
      sceneDisplayMapping(mScene->mSceneCoordinates,next->mSceneCoordinates).verifiedAlignment);
  if(mCameraViewActive&&!keepCalibration) {
    toggleCameraView();mSourceCameraIntrinsics.reset();mSourceCameraClipping.reset();
  }
  mapViewToScene(mapping);
  mScene = next;
  if (mProcessingScene) mProcessingReferenceScene = next;
  if(!keepCalibration){mCameraViewActive=false;mStoredCameraView.reset();}
  mTransformGizmoHover = {};
  mSelectedSceneIds = selected;
  mModelSelected = !selected.isEmpty();
  if (mModelSelected) selectModel();
  if (activeChanged) emit activeSceneObjectChanged(id);
  publishActiveSceneState();
  notifySceneSelection();
  return true;
}

void NativeViewport::setTrainingGpuPreviewDescriptor(
    const TrainingGpuPreviewDescriptor &descriptor) {
  if (descriptor.state == TrainingGpuPreviewState::Ready &&
      (!mProcessingActive || !mProcessingScene)) return;
  if (descriptor.state == TrainingGpuPreviewState::Ready)
    mLatestTrainingGpuPreviewDescriptor = descriptor;
  else
    mLatestTrainingGpuPreviewDescriptor.reset();
  mPendingTrainingGpuPreviewDescriptor = descriptor;
  mTrainingGpuPreviewStopPending = false;
  if (descriptor.state == TrainingGpuPreviewState::Ready && !mProcessingPreviewPaused) {
    mTrainingGpuPreviewCameraFramed = false;
  }
  updateFrameRefreshPolicy();
  update();
}

void NativeViewport::stopTrainingGpuPreview() {
  mPendingTrainingGpuPreviewDescriptor.reset();
  mLatestTrainingGpuPreviewDescriptor.reset();
  mTrainingGpuPreviewStopPending = true;
  mTrainingGpuPreviewCameraFramed = false;
  update();
}

void NativeViewport::setProjectLabel(const QString &label) {
  mProjectLabel = label;
  update();
}

void NativeViewport::beginProcessingPreview() {
  if (mModelDragActive) finishModelTransform(false);
  if (mScene == mProcessingScene) setProcessingPreviewVisible(false);
  discardProcessingScene();
  stopTrainingGpuPreview();
  mProcessingReferenceScene = mScene;
  mProcessingReferenceView = currentProcessingView();
  mProcessingPreviewView.reset();
  const bool wasPreviewPaused = mProcessingPreviewPaused;
  mProcessingPreviewPaused = false;
  mProcessingPausedImage = {};
  mProcessingPreviewResumePending = false;
  mPausedPreviewCandidatePath.clear();
  mPausedPreviewCandidateCount = 0;
  mProcessingReferenceSelection = mSelectedSceneIds;
  mProcessingReferenceSelected = mModelSelected;
  mProcessingScene = std::make_shared<SceneState>();
  mProcessingPreviewVisible = true;
  ++mSelectionRequestEpoch;
  mSelectionGestureActive = false;
  mSelectionPath.clear();
  mProcessingActive = true;
  mProcessingPreviewPresent = false;
  mProcessingHasFrame = false;
  mProcessingStage = QStringLiteral("prepare");
  mProcessingIteration = mProcessingTotal = mProcessingProgress = -1;
  mModelSelected = false;
  setInteractionMode(InteractionMode::Inspect);
  updateFrameRefreshPolicy();
  if (wasPreviewPaused) emit processingPreviewPausedChanged(false);
  emit processingPreviewChanged();
  update();
}

void NativeViewport::setProcessingStage(const QString &stage, int iteration, int total, int progress) {
  mProcessingStage = stage;
  mProcessingIteration = iteration;
  mProcessingTotal = total;
  mProcessingProgress = progress;
  prepareProcessingReferenceImage();
  update();
}

void NativeViewport::finishProcessingPreview(bool succeeded, bool cancelled, bool paused) {
  mProcessingActive = false;
  mProcessingStage = succeeded ? QStringLiteral("done") :
      paused ? QStringLiteral("paused") :
      cancelled ? QStringLiteral("cancelled") : QStringLiteral("failed");
  const bool referenceAvailable = mProcessingReferenceScene &&
      !mProcessingReferenceScene->mScenePath.isEmpty() &&
      (mProcessingReferenceScene->mPreviewPointCount > 0 ||
       mProcessingReferenceScene->mPreviewTriangleCount > 0);
  const bool keepTaskVisible = succeeded ? mProcessingPreviewVisible : !referenceAvailable;
  setProcessingPreviewVisible(keepTaskVisible);
  restoreProcessingReferenceGpu();
  mProcessingReferenceImage = {};
  mSelectedSceneIds = mProcessingReferenceSelection;
  mModelSelected = !keepTaskVisible && mProcessingReferenceSelected;
  notifyEditState();
  notifyModelInteractionState();
  notifySceneSelection();
  emit processingPreviewChanged();
  update();
}

void NativeViewport::clearProcessingPreview() {
  // A project-context reset cannot stop an active worker. The caller performs
  // this only after a successful new/open/recovery transition.
  if (mProcessingActive) return;
  if (mScene == mProcessingScene) {
    if (!mSceneStates.isEmpty() && !mSceneStates.contains(mProcessingReferenceScene))
      mProcessingReferenceScene = mSceneStates.first();
    else if (!mProcessingReferenceScene || mProcessingReferenceScene == mProcessingScene)
      mProcessingReferenceScene = std::make_shared<SceneState>();
    setProcessingPreviewVisible(false);
  }
  restoreProcessingReferenceGpu();
  discardProcessingScene();
  stopTrainingGpuPreview();
  if (context() && context()->isValid()) {
    const bool alreadyCurrent = QOpenGLContext::currentContext() == context();
    if (!alreadyCurrent) makeCurrent();
    applyPendingTrainingGpuPreview();
    if (!alreadyCurrent) doneCurrent();
  }
  mTrainingGpuPreviewTimer->stop();
  mTrainingGpuPreviewError.clear();
  mProcessingReferenceScene.reset();
  mProcessingReferenceView.reset();
  mProcessingPreviewView.reset();
  const bool wasPreviewPaused = mProcessingPreviewPaused;
  mProcessingPreviewPaused = false;
  mProcessingPausedImage = {};
  mProcessingPreviewResumePending = false;
  mPausedPreviewCandidatePath.clear();
  mPausedPreviewCandidateCount = 0;
  mProcessingReferenceSelection.clear();
  mProcessingReferenceSelected = false;
  mProcessingStage.clear();
  mProcessingIteration = mProcessingTotal = mProcessingProgress = -1;
  mFrameRateCounter = {};
  pausePresentationMetrics();
  mPresentedFrameSerial = mRenderedFrameSerial;
  updateManagedResourceUsage();
  updateFrameRefreshPolicy();
  publishActiveSceneState();
  notifySceneSelection();
  emit resourceBudgetStatusChanged();
  if (wasPreviewPaused) emit processingPreviewPausedChanged(false);
  emit processingPreviewChanged();
  update();
}

QList<std::shared_ptr<NativeViewport::SceneState>> NativeViewport::displayedSceneStates() const {
  if (mScene == mProcessingScene) return {mProcessingScene};
  auto states = mSceneStates;
  if (states.isEmpty()) states.append(mScene);
  states.removeIf([](const auto &state) { return !state->visible; });
  return states;
}

bool NativeViewport::trainingGpuPreviewForScene() const {
  return mScene == mProcessingScene && mProcessingPreviewVisible && !mRenderingInactiveScene &&
      isVisible() && !window()->isMinimized();
}

NativeViewport::StoredCameraView NativeViewport::currentProcessingView() const {
  return {mTarget, mYawDegrees, mPitchDegrees, mDistance, mOrthographic,
          mRollDegrees, mPerspectiveFovDegrees, mFocalAspectCorrection, mAxisView};
}

void NativeViewport::restoreProcessingView(const StoredCameraView &view) {
  mTarget = view.target;
  mYawDegrees = view.yawDegrees;
  mPitchDegrees = view.pitchDegrees;
  mDistance = view.distance;
  mOrthographic = view.orthographic;
  mRollDegrees = view.rollDegrees;
  mPerspectiveFovDegrees = view.fovDegrees;
  mFocalAspectCorrection = view.focalAspectCorrection;
  mAxisView = view.axisView;
}

void NativeViewport::discardProcessingScene() {
  if (mProcessingScene && !mSceneStates.contains(mProcessingScene)) {
    ++mProcessingScene->mSceneGeneration;
    ++mProcessingScene->mCameraTrajectoryGeneration;
    mProcessingScene->queuedPreviewPath.clear();
    mProcessingScene->queuedPreviewCount = 0;
    if (context() && context()->isValid()) {
      const bool alreadyCurrent = QOpenGLContext::currentContext() == context();
      if (!alreadyCurrent) makeCurrent();
      {
        QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, mProcessingScene);
        releaseSceneBuffers();
      }
      if (!alreadyCurrent) doneCurrent();
    }
  }
  mProcessingScene.reset();
  mProcessingHasFrame = false;
  mProcessingPreviewPresent = false;
  mProcessingPreviewVisible = false;
  mProcessingReferenceImage = {};
  mProcessingReferenceCaptureQueued = false;
}

void NativeViewport::prepareProcessingReferenceImage() {
  const bool intensiveStage = mProcessingStage == QStringLiteral("train") ||
      mProcessingStage == QStringLiteral("mesh") || mProcessingStage == QStringLiteral("texture");
  if (!mProcessingActive || !mProcessingPreviewVisible || hasProcessingPreview() ||
      !intensiveStage || mScene == mProcessingScene || !context() || !context()->isValid() ||
      mCapturingProcessingReferenceImage || mScene->processingGpuSuspended ||
      !isVisible() || window()->isMinimized()) return;
  // Keep only the reference geometry in a CPU image. The HUD is painted live,
  // so progress and language changes never reuse text captured before training.
  QScopedValueRollback<bool> capturing(mCapturingProcessingReferenceImage, true);
  mProcessingReferenceImage = grabFramebuffer();
  if (mProcessingReferenceImage.isNull()) return;
  suspendProcessingReferenceGpu();
}

void NativeViewport::suspendProcessingReferenceGpu() {
  if (!context() || !context()->isValid()) return;
  const bool alreadyCurrent = QOpenGLContext::currentContext() == context();
  if (!alreadyCurrent) makeCurrent();
  auto states = mSceneStates;
  if (mProcessingReferenceScene && !states.contains(mProcessingReferenceScene))
    states.append(mProcessingReferenceScene);
  for (const auto &state : states) {
    if (state == mProcessingScene || state->processingGpuSuspended) continue;
    QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
    state->processingMeshNeedsReload = state->mHasMesh && !state->mMeshCache.isValid() &&
        state->mPendingMeshVertices.isEmpty() && state->resourceMeshGpuBytes > 0;
    releaseSceneBuffers();
    state->processingGpuSuspended = true;
  }
  if (!alreadyCurrent) doneCurrent();
  updateManagedResourceUsage();
  emit resourceBudgetStatusChanged();
}

void NativeViewport::restoreProcessingReferenceGpu() {
  const bool canUseGl = context() && context()->isValid();
  const bool alreadyCurrent = canUseGl && QOpenGLContext::currentContext() == context();
  if (canUseGl && !alreadyCurrent) makeCurrent();
  auto states = mSceneStates;
  if (mProcessingReferenceScene && !states.contains(mProcessingReferenceScene))
    states.append(mProcessingReferenceScene);
  const auto active = mScene;
  for (const auto &state : states) {
    if (!state->processingGpuSuspended) continue;
    state->processingGpuSuspended = false;
    QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
    QScopedValueRollback<bool> background(mRenderingInactiveScene, state != active);
    if (state->processingMeshNeedsReload) {
      state->processingMeshNeedsReload = false;
      state->mRenderedMeshIndexCount = 0;
      ++state->mSceneGeneration;
      startSceneLoad(state->mRequestedScenePath, false, true);
    } else {
      // CPU attributes and edits remain owned by the original object. Only
      // rebuild disposable display buffers; never reinitialize the edit model.
      if (canUseGl) initializeSceneBuffers();
      rebuildRenderedVertices();
      state->mMeshUploadPending = !state->mPendingMeshVertices.isEmpty();
      if (!state->mMeshTexturePath.isEmpty() && state->mPendingMeshTexture.isNull()) {
        state->mPendingMeshTexture = QImage(state->mMeshTexturePath);
        state->mMeshTextureUploadPending = !state->mPendingMeshTexture.isNull();
      }
    }
  }
  if (canUseGl && !alreadyCurrent) doneCurrent();
  updateManagedResourceUsage();
  emit resourceBudgetStatusChanged();
}

void NativeViewport::setProcessingPreviewVisible(const bool visible) {
  mProcessingPreviewVisible = visible;
  const auto next = visible && hasProcessingPreview() ? mProcessingScene : mProcessingReferenceScene;
  if (!next || next == mScene) {
    if (!visible) {
      restoreProcessingReferenceGpu();
      mProcessingReferenceImage = {};
    } else prepareProcessingReferenceImage();
    updateFrameRefreshPolicy();
    emit processingPreviewChanged();
    update();
    return;
  }
  if (mScene == mProcessingScene) mProcessingPreviewView = currentProcessingView();
  else {
    mProcessingReferenceScene = mScene;
    mProcessingReferenceView = currentProcessingView();
  }
  mViewSnapTimer->stop();
  mScene = next;
  mLayerDisplayTransform.setToIdentity();
  mProcessingPreviewPresent = mScene == mProcessingScene;
  mCameraViewActive = false;
  mStoredCameraView.reset();
  mPressedButtons = Qt::NoButton;
  mObservationDragActive = false;
  if (mProcessingPreviewPresent) {
    mProcessingReferenceImage = {};
    if (mProcessingPreviewView) restoreProcessingView(*mProcessingPreviewView);
    else resetCamera();
    mModelSelected = false;
    if (mProcessingActive) suspendProcessingReferenceGpu();
  } else {
    if (mProcessingReferenceView) restoreProcessingView(*mProcessingReferenceView);
    mModelSelected = mProcessingReferenceSelected;
    restoreProcessingReferenceGpu();
    mProcessingReferenceImage = {};
  }
  publishActiveSceneState();
  updateFrameRefreshPolicy();
  emit processingPreviewChanged();
  update();
}

void NativeViewport::setProcessingPreviewPaused(const bool paused) {
  if (!mProcessingScene || mProcessingPreviewPaused == paused) return;
  mProcessingPreviewPaused = paused;
  if (paused) {
    mTrainingGpuPreviewTimer->stop();
    // An already started read can finish off-thread, but its result must not
    // replace the frozen view or upload after the user has paused observation.
    const auto scene = mProcessingScene;
    if (scene->previewLoadBusy || scene->resourceLoading) {
      const QString candidate = scene->queuedPreviewPath.isEmpty()
          ? scene->mRequestedScenePath : scene->queuedPreviewPath;
      const qint64 candidateCount = scene->queuedPreviewPath.isEmpty()
          ? scene->requestedPreviewCount : scene->queuedPreviewCount;
      if (QFileInfo(candidate).isFile()) {
        mPausedPreviewCandidatePath = candidate;
        mPausedPreviewCandidateCount = candidateCount;
      }
      ++scene->mSceneGeneration;
      scene->mRequestedScenePath = scene->mScenePath;
      scene->previewLoadBusy = false;
      scene->resourceLoading = false;
      scene->queuedPreviewPath.clear();
      scene->queuedPreviewCount = 0;
    }
    if (mProcessingHasFrame && context() && context()->isValid() &&
        isVisible() && !window()->isMinimized()) {
      const StoredCameraView originalView = currentProcessingView();
      const bool wasTaskVisible = mScene == mProcessingScene;
      {
        QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, mProcessingScene);
        QScopedValueRollback<bool> visibleScope(mProcessingPreviewVisible, true);
        QScopedValueRollback<bool> backgroundScope(mRenderingInactiveScene, false);
        QScopedValueRollback<bool> captureScope(mCapturingProcessingReferenceImage, true);
        if (!wasTaskVisible && mProcessingPreviewView)
          restoreProcessingView(*mProcessingPreviewView);
        mProcessingPausedImage = grabFramebuffer();
      }
      restoreProcessingView(originalView);
    }
    // Keeping a shared publisher slot indefinitely would make "pause preview"
    // consume the optimizer's observation resources. A viewport-sized CPU
    // image keeps the exact last picture while the consumer mapping is freed.
    if (!mProcessingPausedImage.isNull() && mTrainingGpuPreview.attached()) {
      const bool alreadyCurrent = QOpenGLContext::currentContext() == context();
      if (!alreadyCurrent) makeCurrent();
      {
        QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, mProcessingScene);
        const bool gaussianWasAvailable = gaussianRenderingAvailable();
        mTrainingGpuPreview.release();
        synchronizeGaussianRenderingAvailability(gaussianWasAvailable);
      }
      if (!alreadyCurrent) doneCurrent();
      mTrainingGpuPreviewCameraFramed = false;
      emit trainingGpuPreviewStateChanged(false,
          QCoreApplication::translate("Workbench", "GPU 共享显存"),
          QCoreApplication::translate("Workbench", "预览已暂停 · 后台处理继续运行；恢复后显示最新画面"));
    }
    mProcessingPreviewResumePending = false;
  } else {
    mProcessingPreviewResumePending = !mProcessingPausedImage.isNull();
    if (mLatestTrainingGpuPreviewDescriptor) {
      mPendingTrainingGpuPreviewDescriptor = mLatestTrainingGpuPreviewDescriptor;
      mTrainingGpuPreviewStopPending = false;
      updateFrameRefreshPolicy();
    }
    const QString candidate = mPausedPreviewCandidatePath;
    const qint64 candidateCount = mPausedPreviewCandidateCount;
    mPausedPreviewCandidatePath.clear();
    mPausedPreviewCandidateCount = 0;
    if (QFileInfo(candidate).isFile()) setPreviewScene(candidate, candidateCount);
    else if (!mLatestTrainingGpuPreviewDescriptor && !mProcessingScene->mScenePath.isEmpty()) {
      mProcessingPreviewResumePending = false;
      mProcessingPausedImage = {};
    }
  }
  updateManagedResourceUsage();
  updateFrameRefreshPolicy();
  emit resourceBudgetStatusChanged();
  emit processingPreviewPausedChanged(paused);
  emit processingPreviewChanged();
  update();
}

QString NativeViewport::processingPreviewLabel() const {
  if (mProcessingStage.isEmpty()) return {};
  QString phase;
  if (mProcessingStage == QStringLiteral("done"))
    phase = QCoreApplication::translate("Workbench", mProcessingPreviewPresent
        ? AppLanguage::source("处理完成 · 显示只读预览")
        : AppLanguage::source("处理完成 · 显示最终结果"));
  else if (mProcessingStage == QStringLiteral("paused"))
    phase = QCoreApplication::translate("Workbench", "训练已暂停 · 检查点已保存，可继续训练");
  else if (mProcessingStage == QStringLiteral("cancelled") || mProcessingStage == QStringLiteral("failed"))
    phase = QCoreApplication::translate("Workbench", "处理已中断 · 保留最近可用画面");
  else if (mProcessingStage == QStringLiteral("colmap"))
    phase = QCoreApplication::translate("Workbench", "相机解算与稀疏点云");
  else if (mProcessingStage == QStringLiteral("environment"))
    phase = QCoreApplication::translate("Workbench", "检查环境");
  else if (mProcessingStage == QStringLiteral("mesh_inputs"))
    phase = QCoreApplication::translate("Workbench", "准备网格输入");
  else if (mProcessingStage == QStringLiteral("mesh"))
    phase = QCoreApplication::translate("Workbench", "生成网格");
  else if (mProcessingStage == QStringLiteral("mesh_validation"))
    phase = QCoreApplication::translate("Workbench", "校验网格");
  else if (mProcessingStage == QStringLiteral("mesh_ready"))
    phase = QCoreApplication::translate("Workbench", "网格已就绪");
  else if (mProcessingStage == QStringLiteral("texture"))
    phase = QCoreApplication::translate("Workbench", "烘焙照片纹理");
  else if (mProcessingStage == QStringLiteral("texture_preview"))
    phase = QCoreApplication::translate("Workbench", "准备材质预览");
  else if (mProcessingStage == QStringLiteral("texture_ready"))
    phase = QCoreApplication::translate("Workbench", "贴图网格已就绪");
  else if (mProcessingStage == QStringLiteral("train"))
    phase = mProcessingIteration > 0
        ? QCoreApplication::translate("Workbench", "高斯训练 · 优化与密度控制")
        : QCoreApplication::translate("Workbench", "初始化高斯");
  else
    phase = QCoreApplication::translate("Workbench", "准备影像与训练数据");
  if (mProcessingActive && mProcessingIteration >= 0 && mProcessingTotal > 0)
    phase += QCoreApplication::translate("Workbench", " · 迭代 %1 / %2").arg(mProcessingIteration).arg(mProcessingTotal);
  else if (mProcessingActive && mProcessingProgress >= 0)
    phase += QStringLiteral(" · %1%").arg(mProcessingProgress);
  return phase;
}

QString NativeViewport::processingPreviewDetail() const {
  if (mProcessingStage.isEmpty()) return {};
  if (mProcessingPreviewPaused)
    return QCoreApplication::translate("Workbench", mProcessingActive
        ? AppLanguage::source("预览已暂停 · 后台处理继续运行；恢复后显示最新画面")
        : AppLanguage::source("预览已暂停 · 恢复后显示最新可用画面"));
  if (mProcessingActive)
    return QCoreApplication::translate("Workbench", !mProcessingHasFrame
        ? AppLanguage::source("等待首个三维结果 · 保留当前画面")
        : AppLanguage::source("点云 → 初始化高斯 → 训练优化 → 最终结果 · 可自由观察"));
  if (mScene->previewLoadBusy && !mProcessingPreviewPresent)
    return QCoreApplication::translate("Workbench", "正在载入最终模型 · 可继续观察");
  if (mScene->previewLoadFailed && !mProcessingPreviewPresent)
    return QCoreApplication::translate("Workbench", "最终模型载入失败 · 保留只读预览");
  if (selectableModelAvailable())
    return QCoreApplication::translate("Workbench", mEditToolsLocked
        ? AppLanguage::source("最终模型已就绪 · 编辑工具已锁定")
        : AppLanguage::source("最终模型已就绪 · 工具已恢复"));
  return QCoreApplication::translate("Workbench", "只读预览 · 可自由观察，不修改源数据");
}

void NativeViewport::setPreviewScene(const QString &path, qint64 count) {
  if (!mProcessingScene) return;
  if (mProcessingPreviewPaused) {
    // Snapshot retention may remove older files. Do not let an expired path
    // discard the newest still-existing candidate, and never parse it here.
    if (QFileInfo(path).isFile()) {
      mPausedPreviewCandidatePath = path;
      mPausedPreviewCandidateCount = count;
    }
    return;
  }
  QSignalBlocker previewSignals(this);
  if (mProcessingPreviewVisible) previewSignals.unblock();
  QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, mProcessingScene);
  QScopedValueRollback<bool> background(mRenderingInactiveScene, !mProcessingPreviewVisible);
  loadContinuousScene(path, count);
}

void NativeViewport::loadContinuousScene(const QString &path, qint64 count) {
  if (mScene == mProcessingScene && mProcessingPreviewPaused) {
    if (QFileInfo(path).isFile()) {
      mPausedPreviewCandidatePath = path;
      mPausedPreviewCandidateCount = count;
    }
    return;
  }
  if (path.isEmpty() || path == mScene->mRequestedScenePath) return;
  if (mScene->previewLoadBusy) {
    mScene->queuedPreviewPath = path;
    mScene->queuedPreviewCount = count;
    return;
  }
  // Do not invalidate visible geometry or the user's camera while reading.
  ++mScene->mSceneGeneration;
  if (mScene->mSelectionBusy) {
    mScene->mSelectionBusy = false;
    emit selectionBusyChanged(false);
  }
  mScene->mRequestedScenePath = path;
  mScene->requestedPreviewCount = count;
  mScene->previewLoadBusy = true;
  mScene->previewLoadFailed = false;
  if (context() && context()->isValid() && !mScene->buffersInitialized) {
    const bool alreadyCurrent = QOpenGLContext::currentContext() == context();
    if (!alreadyCurrent) makeCurrent();
    initializeSceneBuffers();
    if (!alreadyCurrent) doneCurrent();
  }
  notifyEditState();
  notifyModelInteractionState();
  startSceneLoad(path, true);
}

void NativeViewport::setScene(const QString &scenePath,
                              const qint64 gaussianCount) {
  if (mSceneStates.isEmpty()) mSceneStates.append(mScene);
  mScene->mGaussianCount = gaussianCount;
  if (mScene->mRequestedScenePath == scenePath) {
    reloadCameraTrajectory(scenePath, false);
    update();
    return;
  }

  ++mScene->mSceneGeneration;
  mScene->resourceLoading = false;
  mScene->resourceNeedsReload = false;
  mScene->resourceForcePagedRetry = false;
  mScene->resourceResidentUploadFailed = false;
  mScene->resourceResidentRamPeakBytes = 0;
  mScene->resourceResidentGpuBytes = 0;
  mScene->resourceResidentStorageSupported = true;
  mScene->resourceLoadReservation.reset();
  mScene->resourceUploadDeferred = false;
  mScene->resourceMeshPageReservations.clear();
  mScene->resourcePointPageReservations.clear();
  mScene->previewLoadBusy = false;
  mScene->previewLoadFailed = false;
  mScene->queuedPreviewPath.clear();
  if (!mRenderingInactiveScene) {
    mProcessingStage.clear();
    mProcessingPreviewPresent = false;
  }
  if (mScene->mSelectionBusy) {
    mScene->mSelectionBusy = false;
    emit selectionBusyChanged(false);
  }
  mSelectionGestureActive = false;
  mModelSelected = false;
  mModelDragActive = false;
  mTransformGizmoDragActive = false;
  mTransformGizmoHover = {};
  mTransformGizmoPress = {};
  mTransformToolHover = -1;
  if (mMode == InteractionMode::Move || mMode == InteractionMode::Rotate ||
      mMode == InteractionMode::Scale) {
    mMode = InteractionMode::Inspect;
    setCursor(defaultInteractionCursor(mMode));
    emit interactionModeChanged(mMode);
  }
  mSelectionPath.clear();
  mBrushCursorVisible = false;
  mTemporaryOrbitActive = false;
  mScene->mRequestedScenePath = scenePath;
  mScene->mScenePath = scenePath;
  mScene->mSourceVertexCount = 0;
  mScene->mSourceFaceCount = 0;
  mScene->mPreviewPointCount = 0;
  mScene->mPreviewTriangleCount = 0;
  mScene->mRenderedPointCount = 0;
  mScene->mFullResolutionPointCount = 0;
  mScene->mUploadedFullResolutionPointCount = 0;
  mScene->mFullResolutionMeshTriangleCount = 0;
  mScene->mUploadedFullResolutionMeshTriangleCount = 0;
  mScene->mDrawnFullResolutionMeshTriangleCount = 0;
  mScene->mRenderedMeshIndexCount = 0;
  updateFrameRefreshPolicy();
  const bool gaussianAvailabilityChanged = gaussianRenderingAvailable();
  const bool meshAvailabilityChanged = meshRenderingAvailable();
  const bool renderModeChangedToPoints = mScene->mRenderMode != RenderMode::Points;
  mScene->mHasGaussianAttributes = false;
  mScene->mHasSurfelAttributes = false;
  mScene->mQualityMip = {};
  mScene->mHasMesh = false;
  mScene->mPreviewOnlyScene = false;
  mScene->mRenderMode = RenderMode::Points;
  const bool hadSceneCoordinates = mScene->mSceneCoordinates.valid;
  mScene->mSceneCoordinates = {};
  mScene->mSceneLoadMessage.clear();
  mScene->mSourcePositions.clear();
  mScene->mSourcePositions.squeeze();
  mScene->mPreviewVertices.clear();
  mScene->mPreviewVertices.squeeze();
  mScene->mPendingVertices.clear();
  mScene->mPendingVertices.squeeze();
  mScene->gaussianGpu.clear();
  mScene->mSphericalHarmonics = {};
  mScene->indexedGaussians = false;
  mScene->sortDirectionValid = false;
  mScene->mPointCache = {};
  mScene->mDesiredPointCacheNodes.clear();
  mScene->mPointCacheReadsInFlight.clear();
  ++mScene->mPointCacheReadEpoch;
  mScene->mPointCacheFailedNodes.clear();
  mScene->mPendingPointCachePages.clear();
  mScene->mPendingPointCachePages.squeeze();
  mScene->mPointCacheError.clear();
  mScene->mFullResolutionPointClearPending = true;
  mScene->mMeshCache = {};
  mScene->mDesiredMeshCacheNodes.clear();
  mScene->mMeshCacheReadsInFlight.clear();
  mScene->mMeshCacheFailedNodes.clear();
  mScene->mPendingMeshCachePages.clear();
  mScene->mPendingMeshCachePages.squeeze();
  mScene->mMeshCacheError.clear();
  mScene->mFullResolutionMeshClearPending = true;
  mScene->mMeshTexturePath.clear();
  mScene->mMeshTextureError.clear();
  mScene->mPendingMeshTexture = {};
  mScene->mMeshTextureSize = {};
  mScene->mMeshHasTextureCoordinates = false;
  mScene->mMeshTextureUploadPending = false;
  mScene->mMeshTextureClearPending = true;
  mScene->mMeshTextureReady = false;
  mScene->mPendingMeshVertices.clear();
  mScene->mPendingMeshVertices.squeeze();
  mScene->mPendingMeshIndices.clear();
  mScene->mPendingMeshIndices.squeeze();
  mScene->mSceneCenter = QVector3D(0.0F, 0.0F, 0.0F);
  mScene->sourceTranslation = {};
  mScene->mModelTranslation = {};
  mScene->mModelRotation = {};
  mScene->mModelScale = QVector3D(1.0F, 1.0F, 1.0F);
  resetModelTransformHistory();
  mScene->mSceneRadius = 4.0F;
  mScene->mEditModel.reset(0);
  mScene->mPointUploadPending = true;
  mScene->mMeshUploadPending = true;
  notifyEditState();
  notifyModelInteractionState();
  if (gaussianAvailabilityChanged) {
    emit gaussianRenderingAvailabilityChanged(false);
  }
  if (meshAvailabilityChanged) {
    emit meshRenderingAvailabilityChanged(false);
  }
  if (renderModeChangedToPoints) {
    emit renderModeChanged(mScene->mRenderMode);
  }
  if (hadSceneCoordinates) {
    emit sceneCoordinatesChanged();
  }
  reloadCameraTrajectory(scenePath, true);
  if (!mRenderingInactiveScene) resetCamera();
  if (scenePath.isEmpty()) {
    return;
  }
  startSceneLoad(scenePath);
  update();
}

void NativeViewport::setShowCameras(const bool enabled) {
  if (mShowCameras == enabled) {
    return;
  }
  mShowCameras = enabled;
  update();
}

void NativeViewport::setEditToolsLocked(const bool locked) {
  if (mEditToolsLocked == locked) return;
  mEditToolsLocked = locked;
  mObservationDragActive = false;
  // Roll back an uncommitted drag before entering navigation-only mode.
  // Keep the selection, gizmo mode and undo history for the next edit session.
  if (locked) {
    ++mSelectionRequestEpoch;
    setInteractionMode(InteractionMode::Inspect);
  }
  mTransformGizmoHover = {};
  mTransformGizmoPress = {};
  mTransformToolHover = -1;
  mPressedButtons = Qt::NoButton;
  setToolTip({});
  QToolTip::hideText();
  setCursor(defaultInteractionCursor(mMode));
  emit editToolsLockedChanged(locked);
  update();
}

void NativeViewport::setShowObservationTrackball(const bool visible) {
  mShowObservationTrackball = visible;
  update();
}

bool NativeViewport::observationTrackballVisible() const {
  const auto displayed = displayedSceneStates();
  const bool collectionAvailable = visibleModelAvailable() ||
      std::any_of(displayed.cbegin(), displayed.cend(), [](const auto &state) {
        return state->visible && !state->mScenePath.isEmpty() &&
            (state->mPreviewPointCount > 0 || state->mPreviewTriangleCount > 0 ||
             state->mRenderedPointCount > 0 || state->mRenderedMeshIndexCount > 0 ||
             state->mFullResolutionPointCount > 0 || state->mFullResolutionMeshTriangleCount > 0);
      });
  return mEditToolsLocked && mShowObservationTrackball &&
         mMode == InteractionMode::Inspect && collectionAvailable;
}

float NativeViewport::observationTrackballRadius() const {
  // A navigation guide, not a scene-space object. Metashape sizes its
  // trackball relative to the viewport rather than changing model scale.
  return std::max(1.0F, static_cast<float>(std::min(width(), height())) * 0.12F);
}

int NativeViewport::observationTrackballAxisAt(const QPointF &position) const {
  if (!observationTrackballVisible()) return -1;
  const QPointF center(width() * 0.5, height() * 0.5);
  const float radius = observationTrackballRadius();
  if (QLineF(center, position).length() < radius * 0.2F) return -1;
  const QMatrix4x4 view = viewMatrix();
  qreal best = 4.0;
  int result = -1;
  for (int axis = 0; axis < 3; ++axis) {
    QPointF previous;
    for (int step = 0; step <= 128; ++step) {
      const float angle = step * (2.0F * kPi / 128.0F);
      QVector3D world;
      world[(axis + 1) % 3] = std::cos(angle);
      world[(axis + 2) % 3] = std::sin(angle);
      const QVector3D point = view.mapVector(world);
      const QPointF screen = center + QPointF(qreal(point.x()) * radius, -qreal(point.y()) * radius);
      if (step > 0 && point.z() > 0.05F) {
        const QPointF segment = screen - previous;
        const qreal length = QPointF::dotProduct(segment, segment);
        const qreal t = length > 1.0e-8 ? std::clamp(QPointF::dotProduct(position - previous, segment) / length, 0.0, 1.0) : 0.0;
        const qreal distance = QLineF(position, previous + segment * t).length();
        if (distance < best) { best = distance; result = axis; }
      }
      previous = screen;
    }
  }
  return result;
}

void NativeViewport::updateObservationNavigationToolTip() {
  if (mEditToolsLocked && mMode == InteractionMode::Inspect &&
      mNavigationHover.part == NavigationGizmoPart::None) {
    setToolTip(QCoreApplication::translate("Workbench", "观察导航：整个视口左键拖动自由旋转，拖动球上彩色弧线按轴旋转；Ctrl+左键或中/右键平移，Shift+左键或滚轮缩放；双击模型表面设置旋转中心。隐藏轨迹球不影响自由导航。"));
  }
}

void NativeViewport::beginObservationRotation(const QPointF &position, const bool pickAxis) {
  mViewSnapTimer->stop();
  leaveCameraView();
  mObservationDragActive = true;
  mObservationDragMoved = false;
  mObservationDragStart = position;
  mObservationDragAngles = viewOrbitAngles();
  mObservationDragProjection = viewProjectionMatrix();
  mObservationDragRadius = observationTrackballRadius();
  mObservationDragAxis = pickAxis ? observationTrackballAxisAt(position) : -1;
}

void NativeViewport::updateObservationRotation(const QPointF &position) {
  if (!mObservationDragActive) return;
  if (!mObservationDragMoved && position == mObservationDragStart) return;
  if (prepareCameraOrbit()) mObservationDragProjection = viewProjectionMatrix();
  mObservationDragMoved = true;
  OrbitAngles angles;
  if (mObservationDragAxis >= 0) {
    QVector3D axis;
    axis[mObservationDragAxis] = 1.0F;
    const float angle = axisRotationDragDegrees(mObservationDragStart, position,
        mTarget, axis, mObservationDragProjection, size(), mObservationDragRadius);
    angles = orbitAnglesAfterRotation(mObservationDragAngles,
                                      QQuaternion::fromAxisAndAngle(axis, -angle));
  } else {
    // The small sphere is an axis guide, not a boundary for camera input.
    // Equator-clamped arcball vectors made radial drags outside it a no-op.
    angles = orbitAnglesAfterScreenDrag(viewOrbitAngles(), position - mObservationDragStart);
    mObservationDragStart = position;
  }
  mYawDegrees = angles.yawDegrees;
  mPitchDegrees = angles.pitchDegrees;
  mRollDegrees = angles.rollDegrees;
}

std::optional<QVector3D> NativeViewport::observationPointAt(const QPointF &position) {
  if (!rect().contains(position.toPoint())) return std::nullopt;
  const auto active = mScene;
  mCollectionProjection = projectionMatrix();
  auto states = displayedSceneStates();
  float nearestDepth = 1.0F;
  std::optional<QVector3D> result;
  for (const auto &state : states) {
    QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
    QScopedValueRollback<bool> background(mRenderingInactiveScene, state != active);
    QScopedValueRollback<QMatrix4x4> display(mLayerDisplayTransform, sceneDisplayTransform(*state, *active));
    if (!visibleModelAvailable()) continue;
    float depth = 1.0F;
    QVector3D point;
    if (modelGeometryHitAt(position, &depth, &point).value_or(false) && depth < nearestDepth &&
        std::isfinite(point.x()) && std::isfinite(point.y()) && std::isfinite(point.z())) {
      nearestDepth = depth;
      result = point;
    }
  }
  return result;
}

bool NativeViewport::centerObservationAt(const QPointF &position) {
  if (!mEditToolsLocked || mMode != InteractionMode::Inspect) return false;
  const auto point = observationPointAt(position);
  if (!point.has_value()) {
    QToolTip::showText(mapToGlobal(position.toPoint()),
        QCoreApplication::translate("Workbench", "未命中模型表面，旋转中心保持不变"), this);
    return false;
  }
  mViewSnapTimer->stop();
  leaveCameraView();
  mObservationDragActive = false;
  mPressedButtons = Qt::NoButton;
  mTarget = *point;
  // Recenter without modifying model transforms, projection or zoom scale.
  mCameraManipulated = true;
  update();
  return true;
}

void NativeViewport::drawObservationTrackball(QPainter &painter) {
  if (!observationTrackballVisible()) return;
  const QPointF center(width() * 0.5, height() * 0.5);
  const float radius = observationTrackballRadius();
  const bool hover = QLineF(mPointerPosition, center).length() <= radius;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  QRadialGradient glass(center - QPointF(radius * 0.27F, radius * 0.3F), radius * 1.4F);
  glass.setColorAt(0.0, themeInk(QColor(227, 237, 245, hover ? 43 : 24),
                                QColor(255, 255, 255, hover ? 100 : 60)));
  glass.setColorAt(0.65, themeInk(QColor(163, 178, 193, hover ? 27 : 14),
                                 QColor(128, 132, 138, hover ? 56 : 36)));
  glass.setColorAt(1.0, themeInk(QColor(89, 103, 117, hover ? 46 : 26),
                                QColor(62, 68, 74, hover ? 80 : 58)));
  painter.setBrush(glass);
  painter.setPen(QPen(themeInk(QColor(207, 218, 228, hover ? 130 : 75),
                               QColor(78, 86, 97, hover ? 180 : 135)), 1.0));
  painter.drawEllipse(center, radius, radius);
  painter.setBrush(Qt::NoBrush);
  const auto view = viewMatrix();
  const int activeAxis = mObservationDragActive ? mObservationDragAxis : observationTrackballAxisAt(mPointerPosition);
  for (int axis = 0; axis < 3; ++axis) {
    QPointF previous;
    for (int step = 0; step <= 128; ++step) {
      const float angle = step * (2.0F * kPi / 128.0F);
      QVector3D world;
      world[(axis + 1) % 3] = std::cos(angle);
      world[(axis + 2) % 3] = std::sin(angle);
      const auto point = view.mapVector(world);
      const QPointF screen = center + QPointF(qreal(point.x()) * radius, -qreal(point.y()) * radius);
      QColor color = navigationAxisColor(axis);
      color.setAlpha(lightViewport()
                         ? point.z() < 0 ? 45 : activeAxis == axis ? 245 : hover ? 190 : 135
                         : point.z() < 0 ? 35 : activeAxis == axis ? 215 : hover ? 145 : 95);
      painter.setPen(QPen(color, activeAxis == axis ? 1.6 : 0.9));
      if (step > 0) painter.drawLine(previous, screen);
      previous = screen;
    }
  }
  painter.setPen(QPen(themeInk(QColor(236, 240, 243, 185),
                               QColor(28, 31, 36, 210)), 1.0));
  painter.drawLine(center - QPointF(3, 0), center + QPointF(3, 0));
  painter.drawLine(center - QPointF(0, 3), center + QPointF(0, 3));
  painter.restore();
}

void NativeViewport::setInteractionMode(const InteractionMode mode) {
  mObservationDragActive = false;
  if (mEditToolsLocked && mode != InteractionMode::Inspect) return;
  if (isTrimInteractionMode(mode) && mSelectedSceneIds.size() > 1) return;
  if ((mode == InteractionMode::Move || mode == InteractionMode::Rotate ||
       mode == InteractionMode::Scale) &&
      !selectableModelAvailable()) {
    return;
  }
  if (mode == InteractionMode::Move) {
    selectModelForMove();
    return;
  }
  if (mode == InteractionMode::Rotate) {
    selectModelForRotate();
    return;
  }
  if (mode == InteractionMode::Scale) {
    selectModelForScale();
    return;
  }
  if (mModelDragActive) {
    finishModelTransform(false);
  }
  const InteractionMode previousMode = mMode;
  mMode = mode;
  if (mMode != InteractionMode::Inspect) {
    mModelSelected = false;
    mSelectedSceneIds.clear();
    notifySceneSelection();
  }
  mModelDragActive = false;
  mSelectionGestureActive = false;
  mSelectionPath.clear();
  mBrushCursorVisible = false;
  mTemporaryOrbitActive = false;
  setCursor(defaultInteractionCursor(mode));
  if (mMode != previousMode) {
    emit interactionModeChanged(mMode);
  }
  notifyModelInteractionState();
  update();
}

void NativeViewport::selectModel() {
  if (mScene->mSelectionBusy || !selectableModelAvailable()) {
    return;
  }
  if (mScene->mEditModel.selectedCount() > 0) {
    mScene->mEditModel.clearSelection();
    rebuildRenderedVertices();
    notifyEditState();
  }
  mModelSelected = true;
  mSelectedSceneIds.insert(mScene->id);
  if (mMode != InteractionMode::Inspect) {
    const InteractionMode previousMode = mMode;
    mMode = InteractionMode::Inspect;
    mModelDragActive = false;
    mSelectionGestureActive = false;
    mSelectionPath.clear();
    mBrushCursorVisible = false;
    mTemporaryOrbitActive = false;
    setCursor(defaultInteractionCursor(mMode));
    if (previousMode != mMode) {
      emit interactionModeChanged(mMode);
    }
  }
  notifyModelInteractionState();
  notifySceneSelection();
  update();
}

bool NativeViewport::focusModel() {
  mObservationDragActive = false;
  const bool focusVisibleCollection = !mScene->visible && !mProcessingActive &&
      mScene != mProcessingScene && visibleSceneObjectCount() > 0;
  if (mScene->mSelectionBusy || (!selectableModelAvailable() && !focusVisibleCollection)) {
    return false;
  }
  if (mModelDragActive) {
    finishModelTransform(false);
  }
  QVector<QVector3D> corners;
  if (focusVisibleCollection) {
    // A hidden active row can remain the properties/export identity. Framing
    // is a camera operation: use its remaining visible selection or display
    // collection without revealing it, activating a source, or clearing edits.
    corners = selectionBoundsCorners();
    if (corners.isEmpty()) corners = displayedBoundsCorners();
  } else {
    selectModel();
    if (!mModelSelected) return false;
    corners = selectionBoundsCorners();
  }
  mPerspectiveFovDegrees = 46.0F;
  mFocalAspectCorrection = 1.0F;
  const float aspectRatio =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height())
                   : 1.0F;
  const std::optional<ViewportCameraFrame> frame = viewportFrameForPoints(
      corners, viewOrbitAngles(), aspectRatio, mOrthographic);
  if (!frame.has_value()) {
    return false;
  }

  mViewSnapTimer->stop();
  leaveCameraView();
  mTarget = frame->target;
  mDistance = clampViewportDistance(frame->distance, frame->radius);
  mCameraManipulated = false;
  setFocus(Qt::ShortcutFocusReason);
  update();
  return true;
}

void NativeViewport::selectModelForMove() {
  if (mEditToolsLocked) return;
  selectModel();
  if (mModelSelected) {
    mTransformGizmoMode = TransformGizmoMode::Move;
    beginModelTransform(InteractionMode::Move);
  }
}

void NativeViewport::selectModelForRotate() {
  if (mEditToolsLocked) return;
  if (mModelDragActive && mMode == InteractionMode::Rotate) {
    if (!mTrackballRotation) {
      mScene->mModelRotation = mModelDragStartTransform.rotation;
      mTrackballRotation = true;
      mTransformConstraint = {};
      mTransformNumericInput.clear();
      updateModelTransform(mModelTransformCurrentPosition,
                           mTransformModifiers);
      update();
    }
    return;
  }
  selectModel();
  if (mModelSelected) {
    mTransformGizmoMode = TransformGizmoMode::Rotate;
    beginModelTransform(InteractionMode::Rotate);
  }
}

void NativeViewport::selectModelForScale() {
  if (mEditToolsLocked) return;
  selectModel();
  if (mModelSelected) {
    mTransformGizmoMode = TransformGizmoMode::Scale;
    beginModelTransform(InteractionMode::Scale);
  }
}

void NativeViewport::setModelGizmoMode(const TransformGizmoMode mode) {
  if (mEditToolsLocked) return;
  if (mModelDragActive) {
    finishModelTransform(false);
  }
  mTransformGizmoMode = mode;
  mTransformGizmoHover = {};
  mTransformGizmoPress = {};
  setCursor(defaultInteractionCursor(mMode));
  update();
}

void NativeViewport::setModelTransform(const QVector3D &translation,
                                       const QQuaternion &rotation,
                                       const QVector3D &scale) {
  const QQuaternion normalizedRotation = normalizedModelRotation(rotation);
  const QVector3D normalizedScale = normalizedModelScale(scale);
  const float comparisonScale =
      std::max({1.0F, mScene->mModelTranslation.length(), translation.length()});
  if (!std::isfinite(translation.x()) || !std::isfinite(translation.y()) ||
      !std::isfinite(translation.z()) ||
      ((mScene->mModelTranslation - translation).lengthSquared() <=
           comparisonScale * comparisonScale * 1.0e-12F &&
       rotationsEquivalent(mScene->mModelRotation, normalizedRotation) &&
       scalesEquivalent(mScene->mModelScale, normalizedScale))) {
    return;
  }
  mScene->mModelTranslation = translation;
  mScene->mModelRotation = normalizedRotation;
  mScene->mModelScale = normalizedScale;
  mScene->sortDirectionValid = false;
  resetModelTransformHistory();
  notifyEditState();
  notifyModelInteractionState();
  update();
}

void NativeViewport::setModelTranslation(const QVector3D &translation) {
  setModelTransform(translation, mScene->mModelRotation, mScene->mModelScale);
}

void NativeViewport::setRenderMode(const RenderMode mode) {
  if (mode == RenderMode::Gaussians && !gaussianRenderingAvailable()) {
    return;
  }
  if (mode == RenderMode::Mesh && !meshRenderingAvailable()) {
    return;
  }
  if (mScene->mRenderMode == mode) {
    return;
  }
  mScene->mRenderMode = mode;
  rebuildRenderedVertices();
  emit renderModeChanged(mScene->mRenderMode);
  update();
}

void NativeViewport::setVisibleOnlySelection(const bool enabled) {
  mVisibleOnlySelection = enabled;
}

void NativeViewport::setBrushRadius(const int pixels) {
  mBrushRadius = std::clamp(static_cast<qreal>(pixels), 4.0, 256.0);
  if (mMode == InteractionMode::Brush) {
    update();
  }
}

void NativeViewport::resetCamera() {
  mObservationDragActive = false;
  mViewSnapTimer->stop();
  mTarget = transformedSceneCenter();
  mYawDegrees = 42.0F;
  mPitchDegrees = 24.0F;
  mRollDegrees = 0.0F;
  mPerspectiveFovDegrees = 46.0F;
  mFocalAspectCorrection = 1.0F;
  const float radius = transformedSceneRadius();
  mDistance = clampViewportDistance(
      std::max(radius * 2.8F, 0.1F), radius);
  mOrthographic = false;
  mAxisView = NavigationAxis::None;
  mCameraViewActive = false;
  mStoredCameraView.reset();
  update();
}

void NativeViewport::clearSelection() {
  if (mScene->mSelectionBusy) {
    return;
  }
  if (mModelDragActive) {
    finishModelTransform(false);
    return;
  }
  mScene->mEditModel.clearSelection();
  const bool clearedModel = mModelSelected;
  mModelSelected = false;
  mSelectedSceneIds.clear();
  notifySceneSelection();
  mModelDragActive = false;
  mTransformGizmoDragActive = false;
  mTransformGizmoHover = {};
  mTransformGizmoPress = {};
  if (clearedModel &&
      (mMode == InteractionMode::Move || mMode == InteractionMode::Rotate ||
       mMode == InteractionMode::Scale)) {
    setInteractionMode(InteractionMode::Inspect);
  }
  rebuildRenderedVertices();
  notifyEditState();
  notifyModelInteractionState();
  update();
}

void NativeViewport::invertSelection() {
  if (mEditToolsLocked) return;
  if (mScene->mSelectionBusy || !hasEditableScene()) {
    return;
  }
  mScene->mEditModel.invertSelection();
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

void NativeViewport::deleteSelection() {
  if (mEditToolsLocked) return;
  if (!hasEditableScene() || mScene->mSelectionBusy || mScene->mEditModel.deleteSelection() == 0) {
    return;
  }
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

void NativeViewport::undoEdit() {
  if (mEditToolsLocked) return;
  if (!mScene->visible || mScene->mSelectionBusy) {
    return;
  }
  if ((mMode == InteractionMode::Move || mMode == InteractionMode::Rotate ||
       mMode == InteractionMode::Scale ||
       !mScene->mEditModel.canUndo()) &&
      canUndoModelTransform()) {
    --mTransformHistoryIndex;
    const auto transforms = mTransformHistory.at(mTransformHistoryIndex).before;
    applySceneTransforms(transforms, true);
    notifyEditState();
    notifyModelInteractionState();
    update();
    return;
  }
  if (mScene->mEditModel.undo() == 0) {
    return;
  }
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

void NativeViewport::redoEdit() {
  if (mEditToolsLocked) return;
  if (!mScene->visible || mScene->mSelectionBusy) {
    return;
  }
  if ((mMode == InteractionMode::Move || mMode == InteractionMode::Rotate ||
       mMode == InteractionMode::Scale ||
       !mScene->mEditModel.canRedo()) &&
      canRedoModelTransform()) {
    const auto transforms = mTransformHistory.at(mTransformHistoryIndex++).after;
    applySceneTransforms(transforms, true);
    notifyEditState();
    notifyModelInteractionState();
    update();
    return;
  }
  if (mScene->mEditModel.redo() == 0) {
    return;
  }
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

ModelExportOptions NativeViewport::modelExportOptions() const {
  ModelExportOptions options;
  options.sourcePath = mScene->mScenePath;
  options.deletedVertices = mScene->mEditModel.deletedBits();
  options.coordinates = mScene->mSceneCoordinates;
  const float displayScale = static_cast<float>(options.coordinates.displayScale);
  options.transform = {mScene->mModelTranslation / displayScale, mScene->mModelRotation, mScene->mModelScale};
  options.pivot = options.coordinates.globalFromLocal(mScene->mSceneCenter);
  options.sceneUnitScale = sourceToSceneUnitScale();
  const auto reference = sceneReferenceCoordinates();
  options.sceneUnit = reference.unit;
  options.sceneUnitDeclared = reference.unitDeclared && options.coordinates.unitDeclared;
  return options;
}

bool NativeViewport::saveCroppedScene(const QString &filePath,
                                      QString *errorMessage) {
  if (!PlyPointCloudLoader::writeFiltered(
          mScene->mScenePath, filePath, mScene->mEditModel.deletedBits(), errorMessage)) {
    return false;
  }
  mScene->mEditModel.markExported();
  notifyEditState();
  return true;
}

bool NativeViewport::hasUnsavedSceneEdits() const {
  return mScene->mEditModel.hasUnsavedChanges();
}

void NativeViewport::discardSceneEdits() {
  if (mScene->mSelectionBusy) return;
  mScene->mEditModel.reset(mScene->mEditModel.pointCount());
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

bool NativeViewport::hasEditableScene() const {
  return mScene->visible && !mProcessingActive && mScene != mProcessingScene &&
         !mScene->previewLoadBusy && !mScene->previewLoadFailed &&
         mSelectedSceneIds.size() <= 1 && !mScene->mPreviewOnlyScene && !mScene->mHasMesh && !mScene->mScenePath.isEmpty() &&
         mScene->mEditModel.pointCount() > 0;
}

bool NativeViewport::selectableModelAvailable() const {
  return !mProcessingActive && mScene != mProcessingScene &&
      !mScene->previewLoadBusy && !mScene->previewLoadFailed && visibleModelAvailable();
}

bool NativeViewport::exportableModelAvailable() const {
  // Paged representations are read-only for point editing, not for export:
  // modelExportOptions identifies the complete source, never a cache proxy.
  return !mProcessingActive && mScene != mProcessingScene &&
      !mScene->previewLoadBusy && !mScene->previewLoadFailed &&
      !mScene->mScenePath.isEmpty() &&
      (mScene->mPreviewPointCount > 0 || mScene->mPreviewTriangleCount > 0 ||
       mScene->mRenderedPointCount > 0 || mScene->mRenderedMeshIndexCount > 0 ||
       mScene->mFullResolutionPointCount > 0 || mScene->mFullResolutionMeshTriangleCount > 0);
}

bool NativeViewport::visibleModelAvailable() const {
  return mScene->visible && ((trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame()) ||
      (!mScene->mScenePath.isEmpty() &&
         (mScene->mPreviewPointCount > 0 || mScene->mPreviewTriangleCount > 0 ||
          mScene->mRenderedPointCount > 0 || mScene->mRenderedMeshIndexCount > 0 ||
          mScene->mFullResolutionPointCount > 0 ||
          mScene->mFullResolutionMeshTriangleCount > 0)));
}

void NativeViewport::setReferencePlaneMode(const ReferencePlaneMode mode) {
  if (mReferencePlaneMode == mode) {
    return;
  }
  mReferencePlaneMode = mode;
  emit referencePlaneModeChanged(mode);
  update();
}

double NativeViewport::referencePlaneElevation() const {
  if (mReferencePlaneMode != ReferencePlaneMode::ModelBase) return 0.0;
  double minimum=std::numeric_limits<double>::infinity();
  const auto reference=sceneReferenceCoordinates();
  // The original source base is a stable project reference, not the bounds of
  // the currently visible display subset. Eye toggles must not move the grid.
  auto states = mScene == mProcessingScene ? QList<std::shared_ptr<SceneState>>{mScene} : mSceneStates;
  if (states.isEmpty()) states.append(mScene);
  for(const auto &state:states) {
    const auto &coordinates=state->mSceneCoordinates;if(!coordinates.valid)continue;
    const auto mapping=sceneDisplayMapping(coordinates,reference);
    for(int corner=0;corner<8;++corner)minimum=std::min(minimum,mapping.mapCoordinate({
        corner&1?coordinates.globalMaximum.x:coordinates.globalMinimum.x,
        corner&2?coordinates.globalMaximum.y:coordinates.globalMinimum.y,
        corner&4?coordinates.globalMaximum.z:coordinates.globalMinimum.z}).z);
  }
  return std::isfinite(minimum)?minimum:0.;
}

QString NativeViewport::referencePlaneDescription() const {
  if (mReferencePlaneMode == ReferencePlaneMode::WorldZero ||
      !sceneReferenceCoordinates().valid) {
    return QCoreApplication::translate("Workbench", "世界坐标 Z=0");
  }
  return QCoreApplication::translate("Workbench", "固定基准 Z=%1")
      .arg(formatSceneCoordinate(referencePlaneElevation(), sceneReferenceCoordinates()));
}

namespace {
bool boundsIntersectView(const QVector3D &minimum, const QVector3D &maximum,
                         const QMatrix4x4 &viewProjection) {
  bool outsideLeft = true;
  bool outsideRight = true;
  bool outsideBottom = true;
  bool outsideTop = true;
  bool outsideNear = true;
  bool outsideFar = true;
  for (int corner = 0; corner < 8; ++corner) {
    const QVector3D point((corner & 1) != 0 ? maximum.x() : minimum.x(),
                          (corner & 2) != 0 ? maximum.y() : minimum.y(),
                          (corner & 4) != 0 ? maximum.z() : minimum.z());
    const QVector4D clip = viewProjection * QVector4D(point, 1.0F);
    outsideLeft = outsideLeft && clip.x() < -clip.w();
    outsideRight = outsideRight && clip.x() > clip.w();
    outsideBottom = outsideBottom && clip.y() < -clip.w();
    outsideTop = outsideTop && clip.y() > clip.w();
    outsideNear = outsideNear && clip.z() < -clip.w();
    outsideFar = outsideFar && clip.z() > clip.w();
  }
  return !(outsideLeft || outsideRight || outsideBottom || outsideTop ||
           outsideNear || outsideFar);
}
} // namespace

bool NativeViewport::gaussianRenderingAvailable() const {
  return (mScene->mHasGaussianAttributes ||
          (trainingGpuPreviewForScene() && mTrainingGpuPreview.attached())) &&
         mGaussianShaderReady;
}

bool NativeViewport::pagedMeshAvailable() const {
  return mScene->mMeshCache.formatVersion == MeshCacheIndex::CurrentFormatVersion &&
         mScene->mMeshCache.rootNode >= 0 &&
         mScene->mMeshCache.rootNode < mScene->mMeshCache.nodes.size() &&
         mScene->mMeshCache.nodes.at(mScene->mMeshCache.rootNode).isValid();
}

bool NativeViewport::meshRenderingAvailable() const {
  return mScene->mHasMesh && mMeshShaderReady &&
         (mScene->mRenderedMeshIndexCount > 0 || pagedMeshAvailable());
}

bool NativeViewport::pointPagingSettled() const {
  if (mScene->resourceLoading ||
      mScene->mPointUploadPending || mScene->mFullResolutionPointClearPending ||
      mScene->pointReadsActive() || !mScene->mPointCacheReadsInFlight.isEmpty() ||
      !mScene->mPendingPointCachePages.isEmpty())
    return false;
  if (!mScene->mPointCache.isValid()) return true;
  return std::all_of(mScene->mDesiredPointCacheNodes.cbegin(), mScene->mDesiredPointCacheNodes.cend(),
      [this](const int nodeId) {
        return std::any_of(mScene->mFullResolutionPointGpuChunks.cbegin(),
                          mScene->mFullResolutionPointGpuChunks.cend(),
                          [nodeId](const FullResolutionGpuChunk &chunk) { return chunk.nodeId == nodeId; });
      });
}

bool NativeViewport::meshPagingSettled() const {
  if (mScene->mMeshUploadPending || mScene->mMeshTextureUploadPending) {
    return false;
  }
  if (!pagedMeshAvailable()) {
    return true;
  }
  if (!mScene->mMeshCacheReadsInFlight.isEmpty() ||
      !mScene->mPendingMeshCachePages.isEmpty()) {
    return false;
  }
  // Cached ancestors inflate the uploaded triangle count without completing
  // the visible surface. Readiness means every currently requested page is
  // resident, including exact children rather than just their fallbacks.
  return std::all_of(
      mScene->mDesiredMeshCacheNodes.cbegin(), mScene->mDesiredMeshCacheNodes.cend(),
      [this](const int nodeId) {
        return std::any_of(
            mScene->mFullResolutionMeshGpuChunks.cbegin(),
            mScene->mFullResolutionMeshGpuChunks.cend(),
            [nodeId](const FullResolutionMeshGpuChunk &chunk) {
              return chunk.nodeId == nodeId;
            });
      });
}

bool NativeViewport::infiniteGridRenderingAvailable() const {
  return mGridShaderReady;
}

bool NativeViewport::camerasAvailable() const {
  return !mScene->mCameraTrajectory.cameras().isEmpty();
}

qsizetype NativeViewport::cameraCount() const {
  return mScene->mCameraTrajectory.cameras().size();
}

void NativeViewport::initializeGL() {
  initializeOpenGLFunctions();
  // Retain explicit diagnostic environment ceilings, but route them through
  // the same model-wide controller rather than independent per-cache pools.
  bool budgetOverrideValid = false;
  int budgetOverrideMegabytes = qEnvironmentVariableIntValue(
      "GSW_MESH_CACHE_GPU_BUDGET_MB", &budgetOverrideValid);
  if (!budgetOverrideValid) budgetOverrideMegabytes = qEnvironmentVariableIntValue(
      "GSW_POINT_CACHE_GPU_BUDGET_MB", &budgetOverrideValid);
  if (budgetOverrideValid && budgetOverrideMegabytes >= 64 && budgetOverrideMegabytes <= 131072) {
    auto policy = mResources->policy();
    policy.mode = ResourceBudgetPolicy::Mode::Manual;
    policy.gpuLimitMiB = budgetOverrideMegabytes;
    mResources->setPolicy(policy);
  }
  const QVector4D clearColor = viewportClearColor();
  glClearColor(clearColor.x(), clearColor.y(), clearColor.z(), clearColor.w());
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_PROGRAM_POINT_SIZE);

  mPointProgram = new QOpenGLShaderProgram(this);
  const bool vertexCompiled =
      mPointProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                             R"GLSL(#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
uniform mat4 viewProjection;
uniform float pointSize;
out vec3 vertexColor;
void main() {
  gl_Position = viewProjection * vec4(position, 1.0);
  gl_PointSize = pointSize;
  vertexColor = color;
}
)GLSL");
  const bool fragmentCompiled =
      mPointProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                             R"GLSL(#version 330 core
in vec3 vertexColor;
uniform float selected;
uniform float pointSize;
out vec4 fragmentColor;
void main() {
  float alpha = 1.0;
  // A one-pixel circular sprite can disappear at a pixel boundary when its
  // only fragment samples the zero-alpha edge. Preserve its requested size
  // with solid coverage; resolvable circles retain the original antialiasing.
  if (pointSize > 1.5) {
    float radialDistance = length(gl_PointCoord - vec2(0.5)) * 2.0;
    if (radialDistance > 1.0) {
      discard;
    }
    alpha = 1.0 - smoothstep(0.72, 1.0, radialDistance);
  }
  vec3 displayColor = mix(vertexColor, vec3(1.0, 0.58, 0.22),
                          selected * 0.18);
  fragmentColor = vec4(displayColor, alpha);
}
)GLSL");
  const bool pointShaderReady =
      vertexCompiled && fragmentCompiled && mPointProgram->link();
  if (!pointShaderReady) {
    mScene->mSceneLoadMessage = QCoreApplication::translate("Workbench", "OpenGL point shader failed: %1")
                            .arg(mPointProgram->log());
  }

  mMeshProgram = new QOpenGLShaderProgram(this);
  const bool meshVertexCompiled =
      mMeshProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                            R"GLSL(#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec2 textureCoordinate;
layout(location = 4) in float textureWeight;
uniform mat4 viewProjection;
uniform mat4 model;
out vec3 vertexColor;
out vec3 worldPosition;
out vec3 worldNormal;
out vec2 vertexTextureCoordinate;
out float vertexTextureWeight;
void main() {
  gl_Position = viewProjection * vec4(position, 1.0);
  vertexColor = color;
  worldPosition = (model * vec4(position, 1.0)).xyz;
  worldNormal = transpose(inverse(mat3(model))) * normal;
  vertexTextureCoordinate = textureCoordinate;
  vertexTextureWeight = textureWeight;
}
)GLSL");
  const bool meshFragmentCompiled =
      mMeshProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                            R"GLSL(#version 330 core
in vec3 vertexColor;
in vec3 worldPosition;
in vec3 worldNormal;
in vec2 vertexTextureCoordinate;
in float vertexTextureWeight;
uniform vec3 cameraPosition;
uniform sampler2D albedoTexture;
uniform bool textureEnabled;
uniform float selected;
out vec4 fragmentColor;
void main() {
  vec3 normal = normalize(worldNormal);
  if (!gl_FrontFacing) {
    normal = -normal;
  }
  vec3 viewDirection = normalize(cameraPosition - worldPosition);
  vec3 keyDirection = normalize(viewDirection + vec3(0.35, 0.45, 0.55));
  float diffuse = max(dot(normal, keyDirection), 0.0);
  float rim = pow(1.0 - max(dot(normal, viewDirection), 0.0), 2.0);
  float textured = textureEnabled && vertexTextureWeight > 0.5 ? 1.0 : 0.0;
  vec3 baseColor = mix(
      vertexColor, texture(albedoTexture, vertexTextureCoordinate).rgb,
      textured);
  float directLight = mix(0.24 + 0.76 * diffuse,
                          0.82 + 0.18 * diffuse, textured);
  vec3 litColor = baseColor * directLight +
                  vec3(0.12, 0.16, 0.19) * rim;
  litColor = mix(litColor, vec3(1.0, 0.58, 0.22), selected * 0.14);
  fragmentColor = vec4(litColor, 1.0);
}
)GLSL");
  mMeshShaderReady = meshVertexCompiled && meshFragmentCompiled &&
                     mMeshProgram->link();
  if (!mMeshShaderReady && mScene->mSceneLoadMessage.isEmpty()) {
    mScene->mSceneLoadMessage = QCoreApplication::translate("Workbench", "OpenGL mesh shader failed: %1")
                            .arg(mMeshProgram->log());
  }

  mGaussianProgram = new QOpenGLShaderProgram(this);
  QFile shSource(QStringLiteral(":/shaders/evaluate_sh.glsl"));
  shSource.open(QIODevice::ReadOnly);
  const QByteArray shEvaluator = shSource.readAll();
  QFile surfelSource(QStringLiteral(":/shaders/evaluate_surfel.glsl"));
  surfelSource.open(QIODevice::ReadOnly);
  const QByteArray surfelEvaluator = surfelSource.readAll();
  const bool gaussianVertexCompiled =
      mGaussianProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
          QByteArray("#version 330 core\n") + shEvaluator + surfelEvaluator + qualityMipProjectionGlsl() + R"GLSL(
layout(location = 0) in vec3 attributePosition;
layout(location = 1) in vec3 attributeColor;
layout(location = 2) in float attributeOpacity;
layout(location = 3) in vec3 attributeScale;
layout(location = 4) in vec4 attributeRotation;

uniform bool indexedAttributes;
uniform samplerBuffer gaussianAttributes;
uniform usamplerBuffer gaussianOrder;

uniform mat4 view;
uniform mat4 projection;
uniform vec2 viewportPixels;
uniform bool surfelRendering;
uniform bool fullMipFilter;

out vec3 vertexColor;
out float vertexOpacity;
out vec2 gaussianCoordinate;
flat out vec3 surfelTu;
flat out vec3 surfelTv;
flat out vec3 surfelTw;
flat out vec2 surfelFilterCenter;
flat out vec3 surfelCameraDepth;

mat3 rotationMatrix(vec4 quaternionWxyz) {
  vec4 q = quaternionWxyz / max(length(quaternionWxyz), 1e-8);
  float w = q.x;
  float x = q.y;
  float y = q.z;
  float z = q.w;
  return mat3(
      vec3(1.0 - 2.0 * (y * y + z * z),
           2.0 * (x * y + w * z),
           2.0 * (x * z - w * y)),
      vec3(2.0 * (x * y - w * z),
           1.0 - 2.0 * (x * x + z * z),
           2.0 * (y * z + w * x)),
      vec3(2.0 * (x * z + w * y),
           2.0 * (y * z - w * x),
           1.0 - 2.0 * (x * x + y * y)));
}

void main() {
  vec3 position = attributePosition;
  vec3 color = attributeColor;
  float opacity = attributeOpacity;
  vec3 scale = attributeScale;
  vec4 rotation = attributeRotation;
  vec2 shRecord = vec2(-1.0, 0.0);
  if (indexedAttributes) {
    int base = int(texelFetch(gaussianOrder, gl_InstanceID).r) * 4;
    vec4 a = texelFetch(gaussianAttributes, base);
    vec4 b = texelFetch(gaussianAttributes, base + 1);
    vec4 c = texelFetch(gaussianAttributes, base + 2);
    vec4 d = texelFetch(gaussianAttributes, base + 3);
    position = a.xyz; opacity = a.w;
    color = b.xyz; scale = vec3(b.w, c.x, c.y);
    rotation = vec4(c.zw, d.xy);
    shRecord = d.zw;
  } else if (shDegree >= 0) {
    shRecord = texelFetch(gaussianAttributes, gl_InstanceID * 4 + 3).zw;
  }
  if (shDegree >= 0 && shRecord.x >= 0.0 && shRecord.y < 0.5)
    color = evaluateSh(int(shRecord.x), shOrthographic ? shParallelDirection : position - shCameraPosition);
  vec4 cameraCenter = view * vec4(position, 1.0);
  vec4 clipCenter = projection * cameraCenter;
  vertexColor = color;
  vertexOpacity = clamp(opacity, 0.0, 1.0);

  vec2 localCoordinate;
  if (gl_VertexID == 0) {
    localCoordinate = vec2(-3.0, -3.0);
  } else if (gl_VertexID == 1) {
    localCoordinate = vec2(3.0, -3.0);
  } else if (gl_VertexID == 2) {
    localCoordinate = vec2(-3.0, 3.0);
  } else {
    localCoordinate = vec2(3.0, 3.0);
  }
  gaussianCoordinate = localCoordinate;
  surfelTu = surfelTv = surfelTw = vec3(0.0);
  surfelFilterCenter = vec2(0.0);
  surfelCameraDepth = vec3(0.0);

  if (cameraCenter.z >= -1e-4 || clipCenter.w <= 0.0 ||
      vertexOpacity <= (1.0 / 255.0)) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vertexOpacity = 0.0;
    return;
  }

  vec3 safeScale = clamp(scale, vec3(1e-8), vec3(1e6));
  mat3 worldRotation = rotationMatrix(rotation);
  if (surfelRendering) {
    vec4 cameraU = view * vec4(worldRotation[0] * safeScale.x, 0.0);
    vec4 cameraV = view * vec4(worldRotation[1] * safeScale.y, 0.0);
    vec4 clipU = projection * cameraU;
    vec4 clipV = projection * cameraV;
    surfelCameraDepth = -vec3(cameraU.z, cameraV.z, cameraCenter.z);
    surfelTw = vec3(clipU.w, clipV.w, clipCenter.w);
    surfelTu = 0.5 * viewportPixels.x *
        (vec3(clipU.x, clipV.x, clipCenter.x) + surfelTw);
    surfelTv = 0.5 * viewportPixels.y *
        (vec3(clipU.y, clipV.y, clipCenter.y) + surfelTw);
    vec2 center, extent;
    bool bounded = surfelBounds(surfelTu, surfelTv, surfelTw, center, extent);
    if (!bounded) {
      center = 0.5 * viewportPixels;
      extent = 0.5 * viewportPixels;
    }
    // Paper Eq. (11): low-pass filter is centred on the projected local origin,
    // which need not equal a tilted disk's projected AABB centre.
    surfelFilterCenter = 0.5 * viewportPixels * (clipCenter.xy / clipCenter.w + 1.0);
    vec2 lower = max(center - extent, vec2(0.0));
    vec2 upper = min(center + extent, viewportPixels);
    if (any(lessThanEqual(upper, lower))) {
      gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
      vertexOpacity = 0.0;
      return;
    }
    vec2 pixel = mix(lower, upper, localCoordinate / 6.0 + 0.5);
    gl_Position = vec4(2.0 * pixel / viewportPixels - 1.0,
                       clipCenter.z / clipCenter.w, 1.0);
    return;
  }
  mat3 scaleSquared = mat3(
      vec3(safeScale.x * safeScale.x, 0.0, 0.0),
      vec3(0.0, safeScale.y * safeScale.y, 0.0),
      vec3(0.0, 0.0, safeScale.z * safeScale.z));
  mat3 worldCovariance =
      worldRotation * scaleSquared * transpose(worldRotation);
  mat3 viewRotation = mat3(view);
  mat3 cameraCovariance =
      viewRotation * worldCovariance * transpose(viewRotation);

  float depth = -cameraCenter.z;
  float focalX = 0.5 * viewportPixels.x * projection[0][0];
  float focalY = 0.5 * viewportPixels.y * projection[1][1];
  vec3 jacobianX;
  vec3 jacobianY;
  if (shOrthographic) {
    // Orthographic pixel coordinates are affine in camera X/Y. Dividing by
    // depth here incorrectly shrinks splats twice when the viewport zooms.
    jacobianX = vec3(focalX, 0.0, 0.0);
    jacobianY = vec3(0.0, focalY, 0.0);
  } else {
    jacobianX = vec3(focalX / depth, 0.0,
                     focalX * cameraCenter.x / (depth * depth));
    jacobianY = vec3(0.0, focalY / depth,
                     focalY * cameraCenter.y / (depth * depth));
  }
  vec3 filteredCovariance = gswApplyGaussian2dFilter(vec3(
      dot(jacobianX, cameraCovariance * jacobianX),
      dot(jacobianX, cameraCovariance * jacobianY),
      dot(jacobianY, cameraCovariance * jacobianY)), vertexOpacity, fullMipFilter);
  float covarianceXX = filteredCovariance.x;
  float covarianceXY = filteredCovariance.y;
  float covarianceYY = filteredCovariance.z;

  float discriminant = sqrt(max(
      0.0, (covarianceXX - covarianceYY) *
                   (covarianceXX - covarianceYY) +
               4.0 * covarianceXY * covarianceXY));
  float eigenvalueMajor =
      max(0.09, 0.5 * (covarianceXX + covarianceYY + discriminant));
  float eigenvalueMinor =
      max(0.09, 0.5 * (covarianceXX + covarianceYY - discriminant));

  vec2 majorAxis;
  if (abs(covarianceXY) > 1e-5) {
    majorAxis = normalize(vec2(covarianceXY,
                               eigenvalueMajor - covarianceXX));
  } else {
    majorAxis = covarianceXX >= covarianceYY ? vec2(1.0, 0.0)
                                               : vec2(0.0, 1.0);
  }
  vec2 minorAxis = vec2(-majorAxis.y, majorAxis.x);
  float sigmaMajor = clamp(sqrt(eigenvalueMajor), 0.3, 256.0);
  float sigmaMinor = clamp(sqrt(eigenvalueMinor), 0.3, 256.0);
  vec2 pixelOffset = majorAxis * (sigmaMajor * localCoordinate.x) +
                     minorAxis * (sigmaMinor * localCoordinate.y);
  vec2 ndcOffset = 2.0 * pixelOffset / max(viewportPixels, vec2(1.0));

  gl_Position = clipCenter;
  gl_Position.xy += ndcOffset * clipCenter.w;
}
)GLSL");
  const bool gaussianFragmentCompiled =
      mGaussianProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
          QByteArray("#version 330 core\n") + surfelEvaluator + R"GLSL(
in vec3 vertexColor;
in float vertexOpacity;
in vec2 gaussianCoordinate;
flat in vec3 surfelTu;
flat in vec3 surfelTv;
flat in vec3 surfelTw;
flat in vec2 surfelFilterCenter;
flat in vec3 surfelCameraDepth;
uniform bool surfelRendering;
uniform mat4 projection;
uniform float selected;
out vec4 fragmentColor;
void main() {
  gl_FragDepth = gl_FragCoord.z;
  float power = -0.5 * dot(gaussianCoordinate, gaussianCoordinate);
  if (surfelRendering) {
    vec2 uv;
    float depth;
    if (!surfelIntersection(gl_FragCoord.xy, surfelTu, surfelTv, surfelTw, uv, depth)) discard;
    float fragmentDepth = surfelFragmentDepth(uv, surfelCameraDepth, projection);
    if (fragmentDepth < 0.0 || fragmentDepth > 1.0) discard;
    gl_FragDepth = fragmentDepth;
    power = surfelPower(uv, gl_FragCoord.xy, surfelFilterCenter);
  }
  float alpha = vertexOpacity * exp(power);
  if (surfelRendering) alpha = min(0.99, alpha);
  if (alpha < (1.0 / 255.0)) {
    discard;
  }
  vec3 displayColor = mix(vertexColor, vec3(1.0, 0.58, 0.22),
                          selected * 0.16);
  fragmentColor = vec4(displayColor * alpha, alpha);
}
)GLSL");
  mGaussianShaderReady = pointShaderReady && gaussianVertexCompiled &&
                         gaussianFragmentCompiled && mGaussianProgram->link();
  if (!mGaussianShaderReady && mScene->mSceneLoadMessage.isEmpty()) {
    mScene->mSceneLoadMessage = QCoreApplication::translate("Workbench", "OpenGL Gaussian shader failed: %1")
                            .arg(mGaussianProgram->log());
  }

  // Actual Blender overlay filtering/LOD source, adapted to camera-relative
  // analytic intersections. Reduced per-level phases preserve nearby fine
  // cells without feeding large absolute source coordinates into GLSL modulo.
  mGridProgram = new QOpenGLShaderProgram(this);
  const bool gridVertexCompiled =
      mGridProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                            blender_grid::vertexShader());
  const bool gridFragmentCompiled =
      mGridProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                            blender_grid::fragmentShader());
  mGridShaderReady =
      gridVertexCompiled && gridFragmentCompiled && mGridProgram->link();
  if (!mGridShaderReady && mScene->mSceneLoadMessage.isEmpty()) {
    mScene->mSceneLoadMessage =
        QCoreApplication::translate("Workbench", "OpenGL reference-grid shader failed: %1")
            .arg(mGridProgram->log());
  }
  if (mGridShaderReady) {
    mGridVertexArray.create();
  }

  mDepthOverlayProgram = new QOpenGLShaderProgram(this);
  const bool depthOverlayVertexCompiled =
      mDepthOverlayProgram->addShaderFromSourceCode(
          QOpenGLShader::Vertex,
          R"GLSL(#version 130
in vec3 position;
in vec4 color;
uniform mat4 viewProjection;
out vec4 vertexColor;

void main() {
  gl_Position = viewProjection * vec4(position, 1.0);
  vertexColor = color;
}
)GLSL");
  const bool depthOverlayFragmentCompiled =
      mDepthOverlayProgram->addShaderFromSourceCode(
          QOpenGLShader::Fragment,
          R"GLSL(#version 130
in vec4 vertexColor;
out vec4 fragmentColor;

void main() {
  fragmentColor = vertexColor;
}
)GLSL");
  mDepthOverlayProgram->bindAttributeLocation("position", 0);
  mDepthOverlayProgram->bindAttributeLocation("color", 1);
  mDepthOverlayShaderReady =
      depthOverlayVertexCompiled && depthOverlayFragmentCompiled &&
      mDepthOverlayProgram->link();
  if (!mDepthOverlayShaderReady && mScene->mSceneLoadMessage.isEmpty()) {
    mScene->mSceneLoadMessage = QCoreApplication::translate("Workbench", "OpenGL depth-overlay shader failed: %1")
                            .arg(mDepthOverlayProgram->log());
  }
  if (mDepthOverlayShaderReady) {
    mDepthOverlayBuffer.create();
    mDepthOverlayBuffer.setUsagePattern(QOpenGLBuffer::DynamicDraw);
    mDepthOverlayVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(
        &mDepthOverlayVertexArray);
    mDepthOverlayBuffer.bind();
    mDepthOverlayProgram->bind();
    mDepthOverlayProgram->enableAttributeArray(0);
    mDepthOverlayProgram->setAttributeBuffer(
        0, GL_FLOAT, offsetof(DepthOverlayVertex, x), 3,
        sizeof(DepthOverlayVertex));
    mDepthOverlayProgram->enableAttributeArray(1);
    mDepthOverlayProgram->setAttributeBuffer(
        1, GL_FLOAT, offsetof(DepthOverlayVertex, red), 4,
        sizeof(DepthOverlayVertex));
    mDepthOverlayProgram->release();
    mDepthOverlayBuffer.release();
  }

  // Selection first uses the inexpensive model AABB as a broad phase, then
  // renders only real model primitives into a tiny offscreen mask around the
  // pointer.  Keeping this shader independent from the display shaders makes
  // the result deterministic for black points, textured meshes and selected
  // color tinting alike.
  mModelPickProgram = new QOpenGLShaderProgram(this);
  const bool modelPickVertexCompiled =
      mModelPickProgram->addShaderFromSourceCode(
          QOpenGLShader::Vertex,
          R"GLSL(#version 130
in vec3 position;
uniform mat4 viewProjection;
uniform float pointSize;

void main() {
  gl_Position = viewProjection * vec4(position, 1.0);
  gl_PointSize = pointSize;
}
)GLSL");
  const bool modelPickFragmentCompiled =
      mModelPickProgram->addShaderFromSourceCode(
          QOpenGLShader::Fragment,
          R"GLSL(#version 130
uniform bool pointPrimitive;
out vec4 fragmentColor;

void main() {
  if (pointPrimitive &&
      length(gl_PointCoord - vec2(0.5)) * 2.0 > 1.0) {
    discard;
  }
  fragmentColor = vec4(1.0, 0.0, 0.0, 1.0);
}
)GLSL");
  mModelPickProgram->bindAttributeLocation("position", 0);
  mModelPickShaderReady =
      modelPickVertexCompiled && modelPickFragmentCompiled &&
      mModelPickProgram->link();
  if (!mModelPickShaderReady && mScene->mSceneLoadMessage.isEmpty()) {
    mScene->mSceneLoadMessage = QCoreApplication::translate("Workbench", "OpenGL model-pick shader failed: %1")
                            .arg(mModelPickProgram->log());
  }

  mTrainingGpuPreviewCapability =
      TrainingGpuPreviewBuffer::probe(QOpenGLContext::currentContext());

  initializeSceneBuffers();

  refreshResourceBudgets();
  if (meshRenderingAvailable()) {
    emit meshRenderingAvailabilityChanged(true);
    if (mScene->mRenderMode != RenderMode::Mesh) {
      mScene->mRenderMode = RenderMode::Mesh;
      emit renderModeChanged(mScene->mRenderMode);
    }
  } else if (gaussianRenderingAvailable()) {
    emit gaussianRenderingAvailabilityChanged(true);
    if (mScene->mRenderMode != RenderMode::Gaussians) {
      mScene->mRenderMode = RenderMode::Gaussians;
      rebuildRenderedVertices();
      emit renderModeChanged(mScene->mRenderMode);
    }
  }
}

void NativeViewport::initializeSceneBuffers() {
  if (mScene->buffersInitialized) return;
  mScene->buffersInitialized = true;
  if (mGaussianShaderReady) mScene->gaussianGpu.initialize();
  mScene->sortDirectionValid = false;
  if ((mPointProgram != nullptr && mPointProgram->isLinked())) {
    mScene->mPointBuffer.create();
    mScene->mPointBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);

    mScene->mPointVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mScene->mPointVertexArray);
    mScene->mPointBuffer.bind();
    mPointProgram->bind();
    mPointProgram->enableAttributeArray(0);
    mPointProgram->setAttributeBuffer(0, GL_FLOAT,
                                      offsetof(PointCloudVertex, x), 3,
                                      sizeof(PointCloudVertex));
    mPointProgram->enableAttributeArray(1);
    mPointProgram->setAttributeBuffer(1, GL_FLOAT,
                                      offsetof(PointCloudVertex, red), 3,
                                      sizeof(PointCloudVertex));
    mPointProgram->release();
    mScene->mPointBuffer.release();
  }

  if (mMeshShaderReady) {
    mScene->mMeshVertexBuffer.create();
    mScene->mMeshVertexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    mScene->mMeshIndexBuffer.create();
    mScene->mMeshIndexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);

    mScene->mMeshVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mScene->mMeshVertexArray);
    mScene->mMeshVertexBuffer.bind();
    mMeshProgram->bind();
    mMeshProgram->enableAttributeArray(0);
    mMeshProgram->setAttributeBuffer(0, GL_FLOAT, offsetof(MeshVertex, x), 3,
                                     sizeof(MeshVertex));
    mMeshProgram->enableAttributeArray(1);
    mMeshProgram->setAttributeBuffer(1, GL_FLOAT, offsetof(MeshVertex, red), 3,
                                     sizeof(MeshVertex));
    mMeshProgram->enableAttributeArray(2);
    mMeshProgram->setAttributeBuffer(
        2, GL_FLOAT, offsetof(MeshVertex, normalX), 3, sizeof(MeshVertex));
    mMeshProgram->enableAttributeArray(3);
    mMeshProgram->setAttributeBuffer(
        3, GL_FLOAT, offsetof(MeshVertex, textureU), 2, sizeof(MeshVertex));
    mMeshProgram->enableAttributeArray(4);
    mMeshProgram->setAttributeBuffer(
        4, GL_FLOAT, offsetof(MeshVertex, textureWeight), 1,
        sizeof(MeshVertex));
    mMeshProgram->release();
    mScene->mMeshVertexBuffer.release();
  }

  if ((mPointProgram != nullptr && mPointProgram->isLinked()) && mGaussianShaderReady) {
    mScene->mGaussianVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mScene->mGaussianVertexArray);
    mScene->mPointBuffer.bind();
    mGaussianProgram->bind();
    mGaussianProgram->enableAttributeArray(0);
    mGaussianProgram->setAttributeBuffer(0, GL_FLOAT,
                                         offsetof(PointCloudVertex, x), 3,
                                         sizeof(PointCloudVertex));
    mGaussianProgram->enableAttributeArray(1);
    mGaussianProgram->setAttributeBuffer(1, GL_FLOAT,
                                         offsetof(PointCloudVertex, red), 3,
                                         sizeof(PointCloudVertex));
    mGaussianProgram->enableAttributeArray(2);
    mGaussianProgram->setAttributeBuffer(2, GL_FLOAT,
                                         offsetof(PointCloudVertex, opacity), 1,
                                         sizeof(PointCloudVertex));
    mGaussianProgram->enableAttributeArray(3);
    mGaussianProgram->setAttributeBuffer(3, GL_FLOAT,
                                         offsetof(PointCloudVertex, scaleX), 3,
                                         sizeof(PointCloudVertex));
    mGaussianProgram->enableAttributeArray(4);
    mGaussianProgram->setAttributeBuffer(4, GL_FLOAT,
                                         offsetof(PointCloudVertex, rotationW),
                                         4, sizeof(PointCloudVertex));
    for (GLuint attribute = 0; attribute <= 4; ++attribute) {
      glVertexAttribDivisor(attribute, 1);
    }
    mGaussianProgram->release();
    mScene->mPointBuffer.release();
  }

}

void NativeViewport::resizeGL(const int width, const int height) {
  const qreal ratio = devicePixelRatioF();
  glViewport(0, 0, qRound(width * ratio), qRound(height * ratio));
  refreshViewportPresentation();
}

void NativeViewport::changeEvent(QEvent *event) {
  QOpenGLWidget::changeEvent(event);
  if (event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::StyleChange) {
    // Repaint only: never rebuild scene data or interrupt a worker/navigation.
    update();
    refreshViewportPresentation();
  }
}

void NativeViewport::monitorRefreshWindow() {
  QWidget *current = window();
  if (mRefreshPolicyWindow == current) return;
  if (mRefreshPolicyWindow) mRefreshPolicyWindow->removeEventFilter(this);
  mRefreshPolicyWindow = current;
  if (mRefreshPolicyWindow) mRefreshPolicyWindow->installEventFilter(this);
}

bool NativeViewport::eventFilter(QObject *watched, QEvent *event) {
  if (watched == mRefreshPolicyWindow &&
      (event->type() == QEvent::WindowStateChange || event->type() == QEvent::Hide ||
       event->type() == QEvent::Show)) {
    if (event->type() == QEvent::Hide || window()->isMinimized()) {
      mFrameRefreshTimer->stop();
      mTrainingGpuPreviewTimer->stop();
      pausePresentationMetrics();
      refreshViewportPresentation();
      emit viewportPresentationChanged();
    }
    // Observe final visibility after Qt has completed the top-level transition.
    QTimer::singleShot(0, this, [this] {
      updateFrameRefreshPolicy();
      if (isVisible() && !window()->isMinimized()) update();
    });
  }
  return QOpenGLWidget::eventFilter(watched, event);
}

void NativeViewport::showEvent(QShowEvent *event) {
  QOpenGLWidget::showEvent(event);
  monitorRefreshWindow();
  updateFrameRefreshPolicy();
  refreshViewportPresentation();
  update();
}

void NativeViewport::hideEvent(QHideEvent *event) {
  mFrameRefreshTimer->stop();
  mTrainingGpuPreviewTimer->stop();
  pausePresentationMetrics();
  refreshViewportPresentation();
  emit viewportPresentationChanged();
  QOpenGLWidget::hideEvent(event);
}

QJsonObject NativeViewport::renderDiagnosticMetadata() const {
  const auto vector = [](const QVector3D &v) { return QJsonArray{v.x(), v.y(), v.z()}; };
  const auto matrix = [](const QMatrix4x4 &m) {
    QJsonArray result;
    for (int index = 0; index < 16; ++index) result.append(m.constData()[index]);
    return result; // column-major, same ordering as OpenGL uniforms
  };
  QJsonArray objects;
  for (const auto &state : mSceneStates) {
    objects.append(QJsonObject{{"id", state->id}, {"path", state->mScenePath},
        {"requestedPath", state->mRequestedScenePath}, {"visible", state->visible},
        {"renderMode", int(state->mRenderMode)}, {"sourceCount", state->mSourceVertexCount},
        {"translation", vector(state->mModelTranslation)}, {"scale", vector(state->mModelScale)},
        {"rotationWxyz", QJsonArray{state->mModelRotation.scalar(), state->mModelRotation.x(),
                                    state->mModelRotation.y(), state->mModelRotation.z()}}});
  }
  const QMatrix4x4 model = modelMatrix();
  const bool ipc = trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame();
  const QMatrix4x4 effectiveModel = model * (ipc ? trainingPreviewCoordinateTransform() : QMatrix4x4());
  return {{"logicalWidgetSize", QJsonArray{width(), height()}},
      {"activeId", mRenderDiagnosticActiveId}, {"sourceSceneId", mScene->id},
      {"objects", objects}, {"sourcePath", mScene->mScenePath},
      {"renderMode", int(mScene->mRenderMode)}, {"sourceCount", mScene->mSourceVertexCount},
      {"basePointCount", qint64(mScene->mRenderedPointCount)},
      {"fullPointCount", qint64(mScene->mFullResolutionPointCount)},
      {"uploadedFullPointCount", qint64(mScene->mUploadedFullResolutionPointCount)},
      {"pointChunks", qint64(mScene->mFullResolutionPointGpuChunks.size())},
      {"meshIndexCount", qint64(mScene->mRenderedMeshIndexCount)},
      {"drawnFullMeshTriangleCount", qint64(mScene->mDrawnFullResolutionMeshTriangleCount)},
      {"visible", mScene->visible}, {"suspended", mScene->processingGpuSuspended},
      {"deferred", mScene->resourceUploadDeferred}, {"indexedGaussians", mScene->indexedGaussians},
      {"surfelAttributes", mScene->mHasSurfelAttributes}, {"ipcPreview", ipc},
      {"modelSelected", mModelSelected}, {"editToolsLocked", mEditToolsLocked},
      {"layerDisplayTransform", matrix(mLayerDisplayTransform)}, {"modelMatrix", matrix(model)},
      {"expectedMvp", matrix(projectionMatrix() * viewMatrix() * effectiveModel)},
      {"viewMatrix", matrix(viewMatrix())}, {"projectionMatrix", matrix(projectionMatrix())},
      {"cameraTarget", vector(mTarget)}, {"cameraPosition", vector(cameraPosition())},
      {"cameraDistance", mDistance}, {"orthographic", mOrthographic},
      {"cameraAngles", QJsonArray{mYawDegrees, mPitchDegrees, mRollDegrees}},
      {"matrixEncoding", "column-major-float32"}};
}

void NativeViewport::noteRenderDiagnosticDraw(const char *name, const qint64 count) {
  if (!mRenderDiagnostics || !mRenderDiagnostics->recording()) return;
  auto meta = renderDiagnosticMetadata();
  meta.insert("drawCount", count);
  mRenderDiagnostics->note(QString::fromLatin1(name), meta);
}

void NativeViewport::paintGL() {
  if (!isVisible() || window()->isMinimized()) { updateFrameRefreshPolicy(); return; }
  if (!mCapturingProcessingReferenceImage) {
    QScopedValueRollback<std::shared_ptr<SceneState>> previewScope(mScene,
        mProcessingScene ? mProcessingScene : mScene);
    QScopedValueRollback<bool> backgroundScope(mRenderingInactiveScene, false);
  applyPendingTrainingGpuPreview();
  const bool gaussianWasAvailable = gaussianRenderingAvailable();
  QString gpuPreviewError;
  const bool previewChanged =
      mProcessingPreviewVisible && !mProcessingPreviewPaused && mProcessingActive && mProcessingScene
          ? mTrainingGpuPreview.poll(&gpuPreviewError) : false;
  synchronizeGaussianRenderingAvailability(gaussianWasAvailable);
  if (!gpuPreviewError.isEmpty() &&
      gpuPreviewError != mTrainingGpuPreviewError) {
    mTrainingGpuPreviewError = gpuPreviewError;
    emit trainingGpuPreviewStateChanged(
        mTrainingGpuPreview.attached(), QCoreApplication::translate("Workbench", "GPU 共享显存"),
        gpuPreviewError);
  }
  if (previewChanged && mTrainingGpuPreview.attached()) {
    if (mProcessingScene && mTrainingGpuPreview.hasFrame()) {
      const QVector3D previewCenter = mTrainingGpuPreview.sceneCenter();
      const float previewRadius = mTrainingGpuPreview.sceneRadius();
      if (std::isfinite(previewCenter.x()) &&
          std::isfinite(previewCenter.y()) &&
          std::isfinite(previewCenter.z()) && std::isfinite(previewRadius) &&
          previewRadius > 0.0F) {
        mScene->mSceneCenter = trainingPreviewCoordinateTransform().map(previewCenter);
        mScene->mSceneRadius = std::max(previewRadius * static_cast<float>(mScene->mSceneCoordinates.displayScale), 1.0e-4F);
        rebuildCameraGeometry();
        if (!mTrainingGpuPreviewCameraFramed) {
          mTrainingGpuPreviewCameraFramed = true;
          if (mProcessingActive) {
            mScene->mModelTranslation = {};
            mScene->mModelRotation = {};
            mScene->mModelScale = QVector3D(1, 1, 1);
          }
          mProcessingHasFrame = true;
        }
        mProcessingPreviewResumePending = false;
        mProcessingPausedImage = {};
      }
    }
    mTrainingGpuPreviewError.clear();
    emit trainingGpuPreviewStateChanged(
        true, QCoreApplication::translate("Workbench", "GPU 共享显存 · 零 CPU 拷贝"),
        QCoreApplication::translate("Workbench", "迭代 %1 · %2 个高斯")
            .arg(mTrainingGpuPreview.iteration())
            .arg(mTrainingGpuPreview.pointCount()));
  } else if (previewChanged && !mTrainingGpuPreview.attached()) {
    mTrainingGpuPreviewTimer->stop();
    emit trainingGpuPreviewStateChanged(
        false, QCoreApplication::translate("Workbench", "定时快照回退"),
        QCoreApplication::translate("Workbench", "共享显存预览已结束"));
  }
  }
  if (!mCapturingProcessingReferenceImage && mProcessingPreviewVisible &&
      hasProcessingPreview() && mScene != mProcessingScene)
    setProcessingPreviewVisible(true);
  // Opt-in diagnostics retain the already-presented image before requesting
  // this separate normal frame. Never use this frame as the original failure.
  bool diagnosticFrame = false;
  if (mRenderDiagnostics && mRenderDiagnostics->pendingFrame() && !mCapturingProcessingReferenceImage) {
    mRenderDiagnosticActiveId = mScene->id;
    diagnosticFrame = mRenderDiagnostics->beginFrame(
        this, mRenderedFrameSerial + 1, renderDiagnosticMetadata());
  }
  const auto finishDiagnostic = qScopeGuard([&] {
    if (diagnosticFrame) mRenderDiagnostics->endFrame(mRenderedFrameSerial);
  });
  if (diagnosticFrame) mRenderDiagnostics->note(QStringLiteral("before-clear"));
  // Resolve the current theme at frame time so live changes also affect every
  // Gaussian path (resident, compatibility and training preview). The splats
  // continue to use unchanged premultiplied-alpha blending over this backdrop.
  const QVector4D clearColor = viewportClearColor();
  glClearColor(clearColor.x(), clearColor.y(), clearColor.z(), clearColor.w());
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (diagnosticFrame) mRenderDiagnostics->stage(QStringLiteral("after-clear"));
  if (!mCapturingProcessingReferenceImage && mProcessingPreviewVisible &&
      mScene == mProcessingScene && !mProcessingPausedImage.isNull() &&
      (mProcessingPreviewPaused || mProcessingPreviewResumePending)) {
    QPainter painter(this);
    painter.drawImage(rect(), mProcessingPausedImage);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawOverlay(painter);
    drawTransformToolStrip(painter);
    drawAxisGizmo(painter);
    painter.end();
    if (diagnosticFrame) mRenderDiagnostics->stage(QStringLiteral("after-paused-painter"));
    ++mRenderedFrameSerial;
    updateFrameRefreshPolicy();
    return;
  }
  if (!mCapturingProcessingReferenceImage && mProcessingActive &&
      mProcessingPreviewVisible && !hasProcessingPreview() &&
      mScene->processingGpuSuspended && !mProcessingReferenceImage.isNull()) {
    QPainter painter(this);
    painter.drawImage(rect(), mProcessingReferenceImage);
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawOverlay(painter);
    drawTransformToolStrip(painter);
    drawAxisGizmo(painter);
    painter.end();
    if (diagnosticFrame) mRenderDiagnostics->stage(QStringLiteral("after-reference-painter"));
    ++mRenderedFrameSerial;
    updateFrameRefreshPolicy();
    return;
  }
  const QMatrix4x4 view = viewMatrix();
  const QMatrix4x4 projection = projectionMatrix();
  mCollectionProjection = projection;
  const QMatrix4x4 viewProjection = projection * view;
  const QMatrix4x4 modelViewProjection = viewProjection * modelMatrix();
  drawInfiniteGrid(viewProjection);
  if (diagnosticFrame) mRenderDiagnostics->stage(QStringLiteral("after-grid"));
  const auto active = mScene;
  auto ordered = displayedSceneStates();
  // Opaque objects share depth. Draw transparent objects back-to-front at the
  // object level, using the existing per-object Gaussian renderer.
  std::stable_sort(ordered.begin(), ordered.end(), [&](const auto &left, const auto &right) {
    const bool leftTransparent = left->mRenderMode == RenderMode::Gaussians;
    const bool rightTransparent = right->mRenderMode == RenderMode::Gaussians;
    if (leftTransparent != rightTransparent) return !leftTransparent;
    const auto distance = [&](const auto &state) {
      return (sceneDisplayTransform(*state, *active).map(state->mSceneCenter + state->mModelTranslation) -
              cameraPosition()).lengthSquared();
    };
    return leftTransparent && distance(left) > distance(right);
  });
  if (diagnosticFrame) {
    QJsonArray order;
    for (const auto &state : ordered) order.append(state->id);
    mRenderDiagnostics->note(QStringLiteral("draw-order"), {{"ids", order}});
  }
  for (const auto &state : ordered) {
    const bool inactive = state != active;
    QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, state);
    QScopedValueRollback<bool> backgroundScope(mRenderingInactiveScene, inactive);
    QScopedValueRollback<bool> selectionScope(mModelSelected,
        !mEditToolsLocked && mSelectedSceneIds.contains(state->id) && mModelSelected);
    QScopedValueRollback<QMatrix4x4> displayScope(
        mLayerDisplayTransform, sceneDisplayTransform(*state, *active));
    QSignalBlocker signalBlocker(this);
    if (!inactive) signalBlocker.unblock();
    drawSceneGeometry(view, projection);
    if (diagnosticFrame)
      mRenderDiagnostics->stage(QStringLiteral("after-model-%1").arg(state->id),
                                renderDiagnosticMetadata());
  }

  // Selection is an annotation, never a scene-depth occluder. In particular,
  // transparent Gaussian/surfel fragments must not be rejected by a bound's
  // depth written before the model. Draw these after geometry, read-only.
  for (const auto &state : displayedSceneStates()) {
    if (!mSelectedSceneIds.contains(state->id)) continue;
    QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, state);
    QScopedValueRollback<bool> selectionScope(mModelSelected, true);
    QScopedValueRollback<QMatrix4x4> displayScope(
        mLayerDisplayTransform, sceneDisplayTransform(*state, *active));
    drawDepthAwareModelBounds(viewProjection * modelMatrix());
  }
  if (diagnosticFrame) mRenderDiagnostics->stage(QStringLiteral("after-bounds"));

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  drawCameraTrajectory(painter, modelViewProjection);
  drawSelectionGesture(painter);
  drawModelSelection(painter, modelViewProjection);
  drawModelTransformGizmo(painter);
  drawObservationTrackball(painter);

  if (!mCapturingProcessingReferenceImage) {
    drawOverlay(painter);
    drawTransformToolStrip(painter);
    drawAxisGizmo(painter);
  }
  painter.end();
  if (diagnosticFrame) mRenderDiagnostics->stage(QStringLiteral("after-painter"));
  if (!mCapturingProcessingReferenceImage) {
    ++mRenderedFrameSerial;
    if (mProcessingActive && mProcessingPreviewVisible && !hasProcessingPreview() &&
        !mScene->processingGpuSuspended && !mProcessingReferenceCaptureQueued) {
      mProcessingReferenceCaptureQueued = true;
      QTimer::singleShot(0, this, [this] {
        mProcessingReferenceCaptureQueued = false;
        prepareProcessingReferenceImage();
      });
    }
  }
  updateFrameRefreshPolicy();
}

void NativeViewport::drawSceneGeometry(const QMatrix4x4 &view, const QMatrix4x4 &projection) {
  if (!mScene->visible || mScene->processingGpuSuspended) return;
  if (!admitDeferredResourceUpload()) return;
  const bool frozenPreview = mProcessingPreviewPaused && mScene == mProcessingScene;
  if (!frozenPreview) initializeSceneBuffers();
  if (mScene->mRenderMode == RenderMode::Gaussians &&
      !frozenPreview &&
      (!trainingGpuPreviewForScene() || !mTrainingGpuPreview.hasFrame())) {
    const QVector3D forward = modelMatrix().transposed().mapVector(
        (mTarget - cameraPosition()).normalized()).normalized();
    if (!mScene->sortDirectionValid ||
        (forward - mScene->sortedForward).lengthSquared() > 1.0e-8F) {
      if (mScene->indexedGaussians) {
        mScene->gaussianGpu.sort(forward);
        mScene->sortedForward = forward;
        mScene->sortDirectionValid = true;
      } else {
        rebuildRenderedVertices();
      }
    }
  }
  if (!frozenPreview) {
    mScene->gaussianGpu.upload();
    uploadPendingPointCloud();
    uploadPendingMeshTexture();
    uploadPendingMesh();
    finishResourceUpload();
  }
  const QMatrix4x4 modelViewProjection = projection * view * modelMatrix();
  if (!frozenPreview) {
    updatePointCacheSelection(modelViewProjection);
    uploadPendingPointCachePages();
    updateMeshCacheSelection(modelViewProjection);
    uploadPendingMeshCachePages();
  }
  if (mScene->mRenderMode == RenderMode::Mesh && meshRenderingAvailable()) {
    drawMesh(modelViewProjection);
  } else if (trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame() &&
             mScene->mRenderMode == RenderMode::Points) {
    drawTrainingPointCloud(modelViewProjection * trainingPreviewCoordinateTransform());
  } else if (mScene->mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()) {
    drawGaussianCloud(view * modelMatrix() *
        ((trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame()) ? trainingPreviewCoordinateTransform() : QMatrix4x4()), projection);
  } else {
    drawPointCloud(modelViewProjection);
  }
}

void NativeViewport::applyPendingTrainingGpuPreview() {
  if (mTrainingGpuPreviewStopPending) {
    mTrainingGpuPreviewStopPending = false;
    const bool gaussianWasAvailable = gaussianRenderingAvailable();
    const bool wasAttached = mTrainingGpuPreview.attached();
    mTrainingGpuPreview.release();
    synchronizeGaussianRenderingAvailability(gaussianWasAvailable);
    mTrainingGpuPreviewTimer->stop();
    mTrainingGpuPreviewCameraFramed = false;
    if (wasAttached) {
      emit trainingGpuPreviewStateChanged(
          false, QCoreApplication::translate("Workbench", "定时快照回退"),
          QCoreApplication::translate("Workbench", "训练任务已停止"));
    }
  }
  if (!mPendingTrainingGpuPreviewDescriptor.has_value()) {
    return;
  }
  if (mPendingTrainingGpuPreviewDescriptor->state == TrainingGpuPreviewState::Ready &&
      (mProcessingPreviewPaused || !mProcessingPreviewVisible || !isVisible() || window()->isMinimized())) return;
  const TrainingGpuPreviewDescriptor descriptor =
      *mPendingTrainingGpuPreviewDescriptor;
  mPendingTrainingGpuPreviewDescriptor.reset();
  if (descriptor.state != TrainingGpuPreviewState::Ready) {
    const bool gaussianWasAvailable = gaussianRenderingAvailable();
    const bool wasAttached = mTrainingGpuPreview.attached();
    mTrainingGpuPreview.release();
    synchronizeGaussianRenderingAvailability(gaussianWasAvailable);
    mTrainingGpuPreviewTimer->stop();
    mTrainingGpuPreviewCameraFramed = false;
    const QString detail =
        descriptor.state == TrainingGpuPreviewState::Failed
            ? descriptor.error
            : QCoreApplication::translate("Workbench", "训练共享显存发布器已关闭");
    if (wasAttached || descriptor.state == TrainingGpuPreviewState::Failed) {
      emit trainingGpuPreviewStateChanged(
          false, QCoreApplication::translate("Workbench", "定时快照回退"), detail);
    }
    return;
  }

  QString error;
  const bool gaussianWasAvailable = gaussianRenderingAvailable();
  if (!mTrainingGpuPreview.attach(descriptor, &error)) {
    synchronizeGaussianRenderingAvailability(gaussianWasAvailable);
    mTrainingGpuPreviewTimer->stop();
    mTrainingGpuPreviewError = error;
    emit trainingGpuPreviewStateChanged(
        false, QCoreApplication::translate("Workbench", "定时快照回退"), error);
    return;
  }
  synchronizeGaussianRenderingAvailability(gaussianWasAvailable);
  mTrainingGpuPreviewError.clear();
  mTrainingGpuPreviewCameraFramed = false;
  if (mScene->mRenderMode != RenderMode::Gaussians) {
    mScene->mRenderMode = RenderMode::Gaussians;
    emit renderModeChanged(mScene->mRenderMode);
  }
  emit trainingGpuPreviewStateChanged(
      true, QCoreApplication::translate("Workbench", "GPU 共享显存 · 零 CPU 拷贝"),
      QCoreApplication::translate("Workbench", "已连接 %1，等待首帧").arg(descriptor.device));
}

void NativeViewport::reloadCameraTrajectory(const QString &scenePath,
                                            const bool clearExisting) {
  const int generation = ++mScene->mCameraTrajectoryGeneration;
  if (clearExisting || scenePath.isEmpty()) {
    mScene->mCameraTrajectory = {};
    rebuildCameraGeometry();
    emit cameraTrajectoryChanged(0, 0, false, QString(), QString());
  }
  if (scenePath.isEmpty()) {
    return;
  }

  auto *watcher = new QFutureWatcher<CameraTrajectory>(this);
  connect(watcher, &QFutureWatcher<CameraTrajectory>::finished, this,
          [this, watcher, scenePath, generation , weakScene = std::weak_ptr<SceneState>(mScene)]() {
            const auto targetScene = weakScene.lock();
            if (!targetScene) { watcher->deleteLater(); return; }
            const bool foreground = targetScene == mScene;
            QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, targetScene);
            QScopedValueRollback<bool> backgroundScope(mRenderingInactiveScene, !foreground);
            QSignalBlocker sceneSignals(this);
            if (foreground) sceneSignals.unblock();
            CameraTrajectory trajectory = watcher->result();
            watcher->deleteLater();
            if (generation != mScene->mCameraTrajectoryGeneration ||
                scenePath != mScene->mRequestedScenePath) {
              return;
            }
            mScene->mCameraTrajectory = std::move(trajectory);
            rebuildCameraGeometry();
            emit cameraTrajectoryChanged(
                cameraCount(), mScene->mCameraTrajectory.invalidCameraCount(),
                mScene->mCameraGeometry.decimated, mScene->mCameraTrajectory.sourcePath(),
                mScene->mCameraTrajectory.error());
            update();
          });
  watcher->setFuture(QtConcurrent::run(
      [scenePath]() { return CameraTrajectory::loadForScene(scenePath); }));
}

void NativeViewport::rebuildCameraGeometry() {
  mScene->mCameraGeometry = mScene->mCameraTrajectory.geometry(mScene->mSceneRadius,mScene->mSceneCoordinates);
}

void NativeViewport::refreshActiveSceneMetadata() {
  const auto scene=mScene;
  const QString path=scene->mScenePath;
  if(path.isEmpty())return;
  const int generation=++scene->mMetadataGeneration;
  const int sourceGeneration=scene->mSceneGeneration;
  ++scene->mCameraTrajectoryGeneration; // stale asynchronous camera reads cannot win this refresh.
  using Result=std::pair<SceneCoordinateMetadata,CameraTrajectory>;
  auto *watcher=new QFutureWatcher<Result>(this);
  connect(watcher,&QFutureWatcher<Result>::finished,this,[this,watcher,path,generation,sourceGeneration,weak=std::weak_ptr<SceneState>(scene)] {
    const auto target=weak.lock();auto result=watcher->result();watcher->deleteLater();
    if(!target || target->mMetadataGeneration!=generation || target->mSceneGeneration!=sourceGeneration || target->mScenePath!=path)return;
    const bool foreground=target==mScene;
    QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene,target);
    auto &coordinates=target->mSceneCoordinates;
    coordinates.unit=result.first.unit;coordinates.unitDeclared=result.first.unitDeclared;
    coordinates.coordinateReferenceSystem=result.first.coordinateReferenceSystem;
    coordinates.metersPerCoordinate=result.first.metersPerCoordinate;coordinates.scaleStatus=result.first.scaleStatus;
    coordinates.sourceUnit=result.first.sourceUnit;coordinates.dataIdentity=result.first.dataIdentity;
    coordinates.provenance=result.first.provenance;coordinates.validationError=result.first.validationError;
    target->mCameraTrajectory=std::move(result.second);rebuildCameraGeometry();
    // Bounds/rebasing, per-object TRS, edits, selection and the worker are
    // deliberately untouched. Both model and cameras share this same layer.
    if(foreground)publishActiveSceneState();
    update();
  });
  watcher->setFuture(QtConcurrent::run([path,usesDouble=scene->mSceneCoordinates.sourceUsesFloat64,
      sourceSize=scene->metadataSourceSize,sourceModified=scene->metadataSourceModified,
      sourceHash=scene->mSceneCoordinates.provenance.value(QStringLiteral("ply_sha256")).toString()] {
    const QFileInfo current(path);
    if((sourceSize>=0&&sourceSize!=current.size()) ||
        (sourceModified>=0&&sourceModified!=current.lastModified().toMSecsSinceEpoch())) {
      SceneCoordinateMetadata rejected;rejected.validationError=QStringLiteral("camera_scale_model_changed_reload_required");
      return Result{rejected,{}};
    }
    QStringList header;QFile file(path);
    if(file.open(QIODevice::ReadOnly))while(!file.atEnd()&&file.pos()<1024*1024) {
      const auto line=file.readLine(65537).trimmed();
      if(line=="end_header")break;
      if(line.startsWith("comment "))header.append(QString::fromUtf8(line.mid(8)));
      else if(line.startsWith("obj_info "))header.append(QString::fromUtf8(line.mid(9)));
    }
    auto metadata=parsePlyCoordinateMetadata(header,path,usesDouble);
    if(!sourceHash.isEmpty() && !metadata.provenance.isEmpty() &&
        metadata.provenance.value(QStringLiteral("ply_sha256")).toString()!=sourceHash) {
      metadata={};metadata.validationError=QStringLiteral("camera_scale_model_changed_reload_required");
      return Result{metadata,{}};
    }
    return Result{metadata,CameraTrajectory::loadForScene(path)};
  }));
}

QMatrix4x4 NativeViewport::trainingPreviewCoordinateTransform() const {
  QMatrix4x4 result;
  const auto &coordinates = mScene->mSceneCoordinates;
  result.scale(static_cast<float>(coordinates.displayScale));
  result.translate(QVector3D(coordinates.displayShift.x, coordinates.displayShift.y, coordinates.displayShift.z));
  return result;
}

void NativeViewport::startSceneLoad(const QString &scenePath, const bool continuous,
                                    const bool preserveView, const bool forcePaged,
                                    std::shared_ptr<ResourceReservation> reservation) {
  if (mScene == mProcessingScene && mProcessingPreviewPaused) {
    setPreviewScene(scenePath, mScene->requestedPreviewCount);
    mScene->previewLoadBusy = false;
    mScene->resourceLoading = false;
    return;
  }
  mScene->resourceLoading = true;
  const int generation = mScene->mSceneGeneration;
  if (!context() || !context()->isValid()) {
    // Load only after the viewport can identify its actual graphics device.
    // Requests made while constructing the main window retain their identity.
    QTimer::singleShot(50, this, [this, scenePath, continuous, preserveView, forcePaged,
                                reservation, generation,
                                weakScene = std::weak_ptr<SceneState>(mScene)]() {
      const auto target = weakScene.lock();
      if (!target || target->mSceneGeneration != generation ||
          target->mRequestedScenePath != scenePath) return;
      const bool foreground = target == mScene;
      QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, target);
      QScopedValueRollback<bool> background(mRenderingInactiveScene, !foreground);
      startSceneLoad(scenePath, continuous, preserveView, forcePaged, reservation);
    });
    return;
  }
  refreshResourceBudgets();
  if (!reservation) reservation = mResources->createReservation();
  mScene->mSceneLoadMessage = QCoreApplication::translate("Workbench", "正在读取 PLY 场景...");
  emit sceneLoadStarted(scenePath);

  auto *watcher = new QFutureWatcher<PointCloudData>(this);
  connect(watcher, &QFutureWatcher<PointCloudData>::finished, this,
          [this, watcher, scenePath, generation, continuous, preserveView, reservation,
           weakScene = std::weak_ptr<SceneState>(mScene)]() {
            const auto targetScene = weakScene.lock();
            if (!targetScene) { watcher->deleteLater(); return; }
            const bool foreground = targetScene == mScene;
            QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, targetScene);
            QScopedValueRollback<bool> backgroundScope(mRenderingInactiveScene, !foreground);
            QSignalBlocker sceneSignals(this);
            if (foreground) sceneSignals.unblock();
            PointCloudData data = watcher->result();
            watcher->deleteLater();
            // File attributes and the hash-verified Mip contract commit as one
            // snapshot below; stale path/generation results cannot replace it.
            if (scenePath != mScene->mRequestedScenePath ||
                generation != mScene->mSceneGeneration) {
              return;
            }
            mScene->resourceLoading = false;
            if (!foreground) QTimer::singleShot(0, this, [this] {
              // Report only collection metadata after restoring the active
              // state. sceneCoordinatesChanged also reapplies saved TRS, so
              // emitting it here could interrupt an in-progress user edit.
              emit sceneCoordinateRelationshipChanged();
              update();
            });
            mScene->resourceResidentRamPeakBytes = data.residentRamPeakBytes;
            mScene->resourceResidentGpuBytes = data.residentGpuBytes;
            mScene->resourceResidentStorageSupported = data.residentStorageSupported;
            if (continuous) {
              mScene->previewLoadBusy = false;
              const auto queued = mScene->queuedPreviewPath;
              const auto count = mScene->queuedPreviewCount;
              mScene->queuedPreviewPath.clear();
              if (!queued.isEmpty()) QTimer::singleShot(0, this, [this, weakScene, queued, count, generation]() {
                const auto scene = weakScene.lock();
                if (scene && scene == mProcessingScene && scene->mSceneGeneration == generation)
                  setPreviewScene(queued, count);
              });
            }
            if (!data.isValid()) {
              mScene->resourceRetryAfterMs = mResourceClock.elapsed() + 10000;
              mScene->resourceNeedsReload = data.resourceLimited;
              mScene->previewLoadFailed = continuous;
              mScene->mSceneLoadMessage = data.resourceLimited
                  ? QCoreApplication::translate("Workbench", "Insufficient memory for the mesh preview and texture.")
                  : data.error;
              emit sceneLoadFailed(scenePath, mScene->mSceneLoadMessage);
              notifyEditState();
              notifyModelInteractionState();
              update();
              return;
            }
            const auto oldCoordinates = mScene->mSceneCoordinates;
            const auto oldTarget = mTarget;
            const auto oldDistance = mDistance;
            const bool keepCamera = preserveView || (continuous && (mProcessingHasFrame ||
                (mScene->mSceneCoordinates.valid && visibleModelAvailable())));
            mScene->resourceNeedsReload = false;
            mScene->resourceForcePagedRetry = false;
            mScene->resourceLoadReservation = reservation;
            mScene->resourceUploadDeferred = false;
            mScene->mScenePath = scenePath;
            mScene->metadataSourceSize=QFileInfo(scenePath).size();
            mScene->metadataSourceModified=QFileInfo(scenePath).lastModified().toMSecsSinceEpoch();
            if (continuous) {
              if (targetScene == mProcessingScene) {
                mScene->sourceTranslation = {};
                mScene->mModelRotation = {};
                mScene->mModelScale = QVector3D(1, 1, 1);
              }
              mScene->gaussianGpu.clear();
              mScene->indexedGaussians = false;
              mScene->sortDirectionValid = false;
              mScene->mGaussianCount = mScene->requestedPreviewCount > 0 ? mScene->requestedPreviewCount : data.sourceVertexCount;
              reloadCameraTrajectory(scenePath, false);
            }
            mScene->mSceneCoordinates = data.coordinates;
            mScene->mModelTranslation = mScene->sourceTranslation *
                static_cast<float>(data.coordinates.displayScale);
            mScene->mSceneCenter = data.center();
            if (!mRenderingInactiveScene && !keepCamera) mTarget = mScene->mSceneCenter;
            mScene->mSceneRadius = data.radius();
            rebuildCameraGeometry();
            const bool loadedHasMesh = data.hasMesh();
            mScene->mPreviewPointCount = data.previewPointCount();
            mScene->mSourceVertexCount = data.sourceVertexCount;
            mScene->mSourceFaceCount = data.sourceFaceCount;
            mScene->mPreviewTriangleCount = data.meshCache.isValid()
                                        ? static_cast<qsizetype>(
                                              std::min<qint64>(
                                                  data.meshCache
                                                      .renderableTriangleCount,
                                                  std::numeric_limits<
                                                      qsizetype>::max()))
                                        : data.meshIndices.size() / 3;
            mScene->mRenderedMeshIndexCount = data.meshIndices.size();
            mScene->mSourcePositions = std::move(data.sourcePositions);
            mScene->mPreviewVertices = std::move(data.vertices);
            mScene->mSphericalHarmonics = std::move(data.sphericalHarmonics);
            mScene->mPreviewOnlyScene = data.previewOnly;
            mScene->mPointCache = std::move(data.pointCache);
            mScene->mMeshCache = std::move(data.meshCache);
            mScene->mMeshTexturePath = std::move(data.meshTexturePath);
            mScene->mMeshTextureError = std::move(data.meshTextureError);
            mScene->mPendingMeshTexture = std::move(data.meshTextureImage);
            mScene->mMeshHasTextureCoordinates =
                data.meshHasTextureCoordinates ||
                mScene->mMeshCache.hasTextureCoordinates;
            mScene->mMeshTextureUploadPending =
                mScene->mMeshHasTextureCoordinates && !mScene->mPendingMeshTexture.isNull();
            mScene->mMeshTextureClearPending = true;
            mScene->mMeshTextureReady = false;
            if (!mScene->mMeshTextureError.isEmpty()) {
              qWarning().noquote()
                  << QStringLiteral("[mesh-texture] %1")
                         .arg(mScene->mMeshTextureError);
            }
            mScene->mDesiredPointCacheNodes.clear();
            mScene->mPointCacheReadsInFlight.clear();
            ++mScene->mPointCacheReadEpoch;
            mScene->mPointCacheFailedNodes.clear();
            mScene->mPendingPointCachePages.clear();
            mScene->resourcePointPageReservations.clear();
            mScene->mFullResolutionPointCount =
                mScene->mPointCache.isValid() ? mScene->mPreviewPointCount : 0;
            mScene->mUploadedFullResolutionPointCount = 0;
            mScene->mFullResolutionPointClearPending = true;
            mScene->mDesiredMeshCacheNodes.clear();
            mScene->mMeshCacheReadsInFlight.clear();
            mScene->mMeshCacheFailedNodes.clear();
            mScene->mPendingMeshCachePages.clear();
            mScene->resourceMeshPageReservations.clear();
            mScene->mFullResolutionMeshTriangleCount =
                pagedMeshAvailable()
                    ? static_cast<qsizetype>(std::min<qint64>(
                          mScene->mMeshCache.renderableTriangleCount,
                          std::numeric_limits<qsizetype>::max()))
                    : 0;
            mScene->mUploadedFullResolutionMeshTriangleCount = 0;
            mScene->mFullResolutionMeshClearPending = true;
            mScene->mPendingMeshVertices = std::move(data.meshVertices);
            mScene->mPendingMeshIndices = std::move(data.meshIndices);
            mScene->mMeshUploadPending = true;
            mScene->mHasMesh = loadedHasMesh;
            mScene->mEditModel.reset(
                mScene->mHasMesh || mScene->mPreviewOnlyScene ? 0 : mScene->mSourcePositions.size());
            const bool wasAvailable = gaussianRenderingAvailable();
            // The out-of-core cache intentionally stores only exact XYZ/RGB
            // centers. Do not expose Gaussian mode until scale, rotation, and
            // opacity also have a paged representation.
            mScene->mHasGaussianAttributes =
                data.hasGaussianAttributes && !mScene->mPreviewOnlyScene;
            mScene->mHasSurfelAttributes =
                data.hasSurfelAttributes && mScene->mHasGaussianAttributes;
            mScene->mQualityMip = std::move(data.qualityMip);
            const bool isAvailable = gaussianRenderingAvailable();
            if (isAvailable != wasAvailable) {
              emit gaussianRenderingAvailabilityChanged(isAvailable);
            }
            const RenderMode loadedMode =
                mScene->mHasMesh && mMeshShaderReady
                    ? RenderMode::Mesh
                    : isAvailable ? RenderMode::Gaussians : RenderMode::Points;
            if (mScene->mRenderMode != loadedMode) {
              mScene->mRenderMode = loadedMode;
              emit renderModeChanged(mScene->mRenderMode);
            }
            mScene->mSceneLoadMessage.clear();
            if (!mRenderingInactiveScene) {
              if (!keepCamera) resetCamera();
              else if (oldCoordinates.valid && data.coordinates.valid) {
                mTarget = data.coordinates.localFromGlobal(oldCoordinates.globalFromLocal(oldTarget));
                mDistance = oldDistance * static_cast<float>(data.coordinates.displayScale / oldCoordinates.displayScale);
              }
            }
            if (continuous && targetScene == mProcessingScene) {
              mProcessingHasFrame = true;
              mProcessingPreviewResumePending = false;
              mProcessingPausedImage = {};
              QTimer::singleShot(0, this, [this, weakScene, generation] {
                const auto scene = weakScene.lock();
                if (!scene || scene != mProcessingScene || scene->mSceneGeneration != generation) return;
                if (mProcessingPreviewVisible) setProcessingPreviewVisible(true);
                else emit processingPreviewChanged();
              });
            }
            rebuildRenderedVertices();
            deferHiddenResourceUpload();
            updateFrameRefreshPolicy();
            notifyEditState();
            notifyModelInteractionState();
            emit meshRenderingAvailabilityChanged(meshRenderingAvailable());
            if (targetScene == mProcessingScene && mProcessingPreviewVisible && !foreground)
              sceneSignals.unblock();
            emit sceneLoaded(data.sourceVertexCount, mScene->mPreviewPointCount,
                             data.sourceFaceCount, mScene->mPreviewTriangleCount);
            emit sceneCoordinatesChanged();
            if(!mScene->mSceneCoordinates.provenance.isEmpty() &&
                !mScene->mSceneCoordinates.provenance.value(QStringLiteral("native_hash_verified")).toBool())
              refreshActiveSceneMetadata();
            updateManagedResourceUsage();
            emit resourceBudgetStatusChanged();
          });
  watcher->setFuture(QtConcurrent::run(
      [scenePath, reservation, forcePaged]() {
        try {
          auto data = PlyPointCloudLoader::load(scenePath,
            PlyPointCloudLoader::DefaultMaximumPreviewPoints,
            PlyPointCloudLoader::DefaultMaximumEditablePoints,
            PlyPointCloudLoader::DefaultMaximumResidentMeshVertices,
            PlyPointCloudLoader::DefaultMaximumResidentMeshFaces,
            PlyPointCloudLoader::DefaultMaximumResidentMeshBytes,
            LoaderResourcePolicy{reservation, forcePaged});
          data.resourceLimited = !data.isValid() && reservation->admissionWasRefused();
          return data;
        } catch (const std::bad_alloc &) {
          PointCloudData result;
          result.resourceLimited = true;
          result.error = QCoreApplication::translate("Workbench", "Insufficient memory for the mesh preview and texture.");
          return result;
        }
      }));
}

void NativeViewport::startSelection(const ScreenSelectionRequest &request,
                                    const SelectionOperation operation) {
  if (mEditToolsLocked || mScene->mSelectionBusy || !hasEditableScene()) {
    return;
  }

  mScene->mSelectionBusy = true;
  emit selectionBusyChanged(true);
  notifyEditState();
  update();

  const int generation = mScene->mSceneGeneration;
  const QVector<PointPosition> positions = mScene->mSourcePositions;
  const QBitArray deleted = mScene->mEditModel.deletedBits();
  const QMatrix4x4 viewProjection = viewProjectionMatrix() * modelMatrix();
  const QSize viewportSize = size();
  ScreenSelectionRequest selection = request;
  selection.visibleOnly = mVisibleOnlySelection;

  auto *watcher = new QFutureWatcher<QVector<quint32>>(this);
  connect(watcher, &QFutureWatcher<QVector<quint32>>::finished, this,
          [this, watcher, generation, operation, epoch = mSelectionRequestEpoch,
           weakScene = std::weak_ptr<SceneState>(mScene)]() {
            const auto targetScene = weakScene.lock();
            if (!targetScene) { watcher->deleteLater(); return; }
            const bool foreground = targetScene == mScene;
            QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, targetScene);
            QScopedValueRollback<bool> backgroundScope(mRenderingInactiveScene, !foreground);
            QSignalBlocker sceneSignals(this);
            if (foreground) sceneSignals.unblock();
            const QVector<quint32> matches = watcher->result();
            watcher->deleteLater();
            if (generation != mScene->mSceneGeneration) {
              return;
            }
            mScene->mSelectionBusy = false;
            if (epoch == mSelectionRequestEpoch && !mEditToolsLocked && mScene->visible)
              mScene->mEditModel.applySelection(matches, operation);
            rebuildRenderedVertices();
            notifyEditState();
            emit selectionBusyChanged(false);
            update();
          });
  watcher->setFuture(QtConcurrent::run(
      [positions, deleted, viewProjection, viewportSize, selection]() {
        return selectSourcePoints(positions, deleted, viewProjection,
                                  viewportSize, selection);
      }));
}

void NativeViewport::finishSelectionGesture(
    const Qt::KeyboardModifiers modifiers) {
  if (!mSelectionGestureActive) {
    return;
  }
  mSelectionGestureActive = false;

  QRectF rectangle(mSelectionStart, mSelectionCurrent);
  rectangle = rectangle.normalized();
  QPolygonF path = mSelectionPath;
  mSelectionPath.clear();

  if (mMode == InteractionMode::Rectangle && rectangle.width() < 3.0 &&
      rectangle.height() < 3.0) {
    rectangle = QRectF(mSelectionCurrent - QPointF(4.0, 4.0), QSizeF(8.0, 8.0));
  }
  if (mMode == InteractionMode::Lasso && path.size() < 3) {
    update();
    return;
  }

  ScreenSelectionRequest request;
  if (mMode == InteractionMode::Rectangle) {
    request.shape = ScreenSelectionShape::Rectangle;
    request.rectangle = rectangle;
  } else if (mMode == InteractionMode::Lasso) {
    request.shape = ScreenSelectionShape::Lasso;
    request.path = path;
  } else if (mMode == InteractionMode::Brush) {
    request.shape = ScreenSelectionShape::Brush;
    request.path = path;
    request.brushRadius = mBrushRadius;
  } else {
    update();
    return;
  }

  SelectionOperation operation = SelectionOperation::Replace;
  if (modifiers.testFlag(Qt::ShiftModifier)) {
    operation = SelectionOperation::Add;
  } else if (modifiers.testFlag(Qt::AltModifier)) {
    operation = SelectionOperation::Subtract;
  }
  startSelection(request, operation);
}

void NativeViewport::rebuildRenderedVertices() {
  mScene->mPendingVertices.clear();
  mScene->mPendingVertices.reserve(mScene->mPreviewVertices.size());
  const QBitArray &selected = mScene->mEditModel.selectedBits();
  const QBitArray &deleted = mScene->mEditModel.deletedBits();
  for (const PointCloudVertex &sourceVertex : mScene->mPreviewVertices) {
    const qsizetype sourceIndex =
        static_cast<qsizetype>(sourceVertex.sourceIndex);
    if (sourceIndex >= deleted.size() || deleted.testBit(sourceIndex)) {
      continue;
    }
    PointCloudVertex renderedVertex = sourceVertex;
    if (selected.testBit(sourceIndex)) {
      renderedVertex.red = 1.0F;
      renderedVertex.green = 0.72F;
      renderedVertex.blue = 0.16F;
      renderedVertex.opacity = std::max(renderedVertex.opacity, 0.85F);
    }
    mScene->mPendingVertices.append(renderedVertex);
  }
  mScene->mRenderedPointCount = mScene->mPendingVertices.size();
  mScene->indexedGaussians = false;
  if (mScene->mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()) {
    QVector3D forward = (mTarget - cameraPosition()).normalized();
    bool invertible = false;
    const QMatrix4x4 model = modelMatrix();
    (void)model.inverted(&invertible);
    if (invertible) {
      forward = model.transposed().mapVector(forward).normalized();
    }
    mScene->sortedForward = forward;
    mScene->sortDirectionValid = true;
    mScene->indexedGaussians = mScene->gaussianGpu.supports(mScene->mRenderedPointCount) &&
        !qEnvironmentVariableIsSet("GSW_DISABLE_INDEXED_GAUSSIANS");
    if (mScene->indexedGaussians) {
      mScene->gaussianGpu.setVertices(mScene->mPendingVertices, mScene->mSphericalHarmonics, selected);
      mScene->gaussianGpu.sort(forward);
    } else {
      mScene->depthSorter.sort(mScene->mPendingVertices, forward);
      if (mScene->gaussianGpu.supports(mScene->mRenderedPointCount))
        mScene->gaussianGpu.setVertices(mScene->mPendingVertices, mScene->mSphericalHarmonics, selected);
      else
        mScene->gaussianGpu.clear();
    }
  } else {
    mScene->gaussianGpu.clear();
  }
  // Surface picking (including double-click recentering of inactive layers)
  // still uses the point VAO. Refresh it on edits/load, never on camera motion.
  mScene->mPointUploadPending = true;
  updateFrameRefreshPolicy();
}

bool NativeViewport::hasPendingFrameUploads() const {
  const bool pointUploadAvailable = mPointProgram && mPointProgram->isLinked();
  const bool meshUploadAvailable = mMeshProgram && mMeshProgram->isLinked();
  const auto needsRefresh = [pointUploadAvailable, meshUploadAvailable](const auto &scene) {
    return scene->visible && !scene->resourceUploadDeferred && ((pointUploadAvailable && scene->mPointUploadPending) ||
        (meshUploadAvailable && scene->mMeshUploadPending) ||
        scene->mFullResolutionPointClearPending || scene->mFullResolutionMeshClearPending ||
        scene->mMeshTextureClearPending || (meshUploadAvailable && scene->mMeshTextureUploadPending) ||
        (pointUploadAvailable && !scene->mPendingPointCachePages.isEmpty()) ||
        (meshUploadAvailable && !scene->mPendingMeshCachePages.isEmpty()));
  };
  return needsRefresh(mScene) || std::any_of(mSceneStates.cbegin(), mSceneStates.cend(), needsRefresh);
}

bool NativeViewport::frameTimingBurstActive() const {
  return hasPendingFrameUploads() || mPressedButtons != Qt::NoButton ||
      mModelDragActive || mSelectionGestureActive || mObservationDragActive || mViewSnapTimer->isActive() ||
      (mProcessingActive && mProcessingPreviewVisible && !mProcessingPreviewPaused);
}

void NativeViewport::pausePresentationMetrics() {
  if (mPresentationIdleTimer) mPresentationIdleTimer->stop();
  mFrameRateCounter.pause();
  mPresentationIdle = true;
  mPresentationWaiting = false;
  mFreshPresentationFrames = 0;
  refreshViewportPresentation();
  emit viewportPresentationChanged();
}

QString NativeViewport::viewportPerformanceText() const {
  if (mProcessingPreviewVisible && mProcessingPreviewPaused)
    return QCoreApplication::translate("Workbench", "预览已暂停");
  if (mPresentationIdle || !isVisible() || (window() && window()->isMinimized()))
    return QCoreApplication::translate("Workbench", "空闲");
  if (mPresentationWaiting)
    return QCoreApplication::translate("Workbench", "等待新画面…");
  if (mFreshPresentationFrames < 2 || mFrameRateCounter.framesPerSecond() <= 0.0)
    return QCoreApplication::translate("Workbench", "帧率统计中…");
  return QStringLiteral("%1 FPS").arg(mFrameRateCounter.framesPerSecond(), 0, 'f', 0);
}

QString NativeViewport::viewportStatusText() const {
  if (!mProcessingPreviewVisible && visibleSceneObjectCount() == 0 && sceneObjectCount() > 0)
    return QCoreApplication::translate("Workbench", "模型已隐藏") + QStringLiteral(" · ") + viewportPerformanceText();
  if (!visibleModelAvailable() && !mTrainingGpuPreview.attached() && mScene->mScenePath.isEmpty())
    return QCoreApplication::translate("Workbench", "未载入场景");
  const QString mode = mProcessingPreviewVisible && hasProcessingPreview()
      ? QCoreApplication::translate("Workbench", "任务预览")
      : mScene->mRenderMode == RenderMode::Mesh && meshRenderingAvailable()
          ? QCoreApplication::translate("Workbench", "网格预览")
          : mScene->mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()
              ? QCoreApplication::translate("Workbench", "高斯预览")
              : QCoreApplication::translate("Workbench", "点预览");
  return mode + QStringLiteral(" · ") + viewportPerformanceText();
}

QString NativeViewport::viewportDetailsText() const {
  QStringList rows{viewportStatusText(), QString()};
  rows << QCoreApplication::translate("Workbench", "网格模式：%1").arg(
      mViewportGridSettings.spacingMode == ViewportGridSettings::SpacingMode::Fixed
          ? QCoreApplication::translate("Workbench", "固定间距")
          : QCoreApplication::translate("Workbench", "自动（随缩放）"));
  if (mViewportGridSettings.spacingMode == ViewportGridSettings::SpacingMode::Fixed) {
    rows << QCoreApplication::translate("Workbench", "主网格间距：%1").arg(formatReferenceLength(mViewportGridSettings.majorSpacing));
    rows << QCoreApplication::translate("Workbench", "细网格间距：%1")
        .arg(formatReferenceLength(mViewportGridSettings.majorSpacing / mViewportGridSettings.subdivisions));
  } else if (const auto steps = displayGridSteps(mViewportGridSettings)) {
    const double worldPerPixel = (mOrthographic ? 2.0 : 2.0 * mDistance) /
        std::max(1e-12, static_cast<double>(std::abs(projectionMatrix()(1, 1)))) /
        std::max(1.0, height() * devicePixelRatioF());
    const double resolution = worldPerPixel * 4.0 / mViewportGridSettings.density;
    int level = 7;
    for (int i = 6; i >= 0; --i) if (resolution < (*steps)[i]) level = i;
    rows << QCoreApplication::translate("Workbench", "自动主网格间距（中心参考）：%1")
        .arg(formatViewportDistance((*steps)[std::min(level + 2, 7)]));
  }
  if (!displayGridSteps(mViewportGridSettings))
    rows << QCoreApplication::translate("Workbench", "当前间距超出视口显示范围，请调整网格间距。");
  rows << QCoreApplication::translate("Workbench", "视点到观察中心的距离：%1").arg(formatViewportDistance(mDistance));
  rows << QCoreApplication::translate("Workbench", "基准面：%1").arg(referencePlaneDescription());
  rows << QCoreApplication::translate("Workbench", "场景单位：%1").arg(sceneLengthUnitDescription(sceneReferenceCoordinates()));
  if (mScene->mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable())
    rows << QCoreApplication::translate("Workbench", "球谐阶数：%1").arg(effectiveShDegree());
  if (mFrameRateCounter.sampleCount() > 0) {
    rows << QString();
    rows << QCoreApplication::translate("Workbench", "最近测得帧率：%1 FPS")
        .arg(mFrameRateCounter.framesPerSecond(), 0, 'f', 1);
    rows << QCoreApplication::translate("Workbench", "平均帧间隔：%1 ms")
        .arg(mFrameRateCounter.averageFrameMilliseconds(), 0, 'f', 1);
  }
  rows << QString() << QCoreApplication::translate("Workbench", "网格和基准面仅用于显示参考，不代表重建精度。")
       << QCoreApplication::translate("Workbench", "帧率只统计实际绘制并完成显示的视口帧；空闲时间不计入，也不是训练速度。");
  return rows.join(QLatin1Char('\n'));
}

double NativeViewport::referenceCoordinateToLocalLength() const {
  const double unitScale = sourceToSceneUnitScale();
  const double display = mScene->mSceneCoordinates.valid ? mScene->mSceneCoordinates.displayScale : 1.0;
  return std::isfinite(unitScale) && unitScale > 0.0 && std::isfinite(display) && display > 0.0
      ? display / unitScale : 1.0;
}

std::optional<std::array<float, 8>> NativeViewport::displayGridSteps(const ViewportGridSettings &settings) const {
  static_assert(blender_grid::StepCount == 8);
  if (!settings.isValid()) return std::nullopt;
  const double localPerCoordinate = referenceCoordinateToLocalLength();
  if (!std::isfinite(localPerCoordinate) || localPerCoordinate <= 0.0) return std::nullopt;
  const auto asStep = [](double logValue) -> std::optional<float> {
    const double value = std::pow(10.0, logValue);
    if (!std::isfinite(value) || value < std::numeric_limits<float>::min() ||
        value > static_cast<double>(std::numeric_limits<float>::max()) / 8.0) return std::nullopt;
    const float narrowed = static_cast<float>(value);
    return std::isnormal(narrowed) && narrowed > 0.0F ? std::optional<float>(narrowed) : std::nullopt;
  };
  if (settings.spacingMode == ViewportGridSettings::SpacingMode::Fixed) {
    const double majorLog = std::log10(settings.majorSpacing) + std::log10(localPerCoordinate);
    const auto major = asStep(majorLog);
    const auto minor = asStep(majorLog - std::log10(settings.subdivisions));
    if (!major || !minor) return std::nullopt;
    std::array<float, 8> result;
    // Fixed mode uses only these two levels. Padding repeats the last valid
    // level, as in Blender's unit-table path; it is not an overflowing hierarchy.
    result.fill(*major);
    result[0] = *minor;
    return result;
  }
  const double worldPerPixel = (mOrthographic ? 2.0 : 2.0 * mDistance) /
      std::max(1e-12, static_cast<double>(std::abs(projectionMatrix()(1, 1)))) /
      std::max(1.0, height() * devicePixelRatioF());
  if (!std::isfinite(worldPerPixel) || worldPerPixel <= 0.0) return std::nullopt;
  double unitLog = std::log10(localPerCoordinate);
  // A metre is a unit-table base only when a physical scale was declared.
  // Unknown/arbitrary coordinates use their own u base, never implicit metres.
  if (const auto metres = sceneMetresPerCoordinate(sceneReferenceCoordinates())) unitLog -= std::log10(*metres);
  const double level = std::floor(std::log10(worldPerPixel * 4.0 / settings.density) - unitLog) - 2.0;
  const auto first = asStep(unitLog + level);
  if (!first) return std::nullopt;
  const auto steps = blender_grid::subdivisionSteps(*first, 10);
  if (!steps || std::any_of(steps->begin(), steps->end(), [](float step) {
        return step > std::numeric_limits<float>::max() / 8.0F;
      })) return std::nullopt;
  return steps;
}

bool NativeViewport::validateViewportGridSettingsForDisplay(
    const ViewportGridSettings &settings, QString *error) const {
  if (!settings.isValid()) {
    if (error) *error = QCoreApplication::translate("Workbench", "视口网格设置无效。");
    return false;
  }
  if (settings.spacingMode == ViewportGridSettings::SpacingMode::Fixed && settings.gridVisible &&
      mScene->mSceneCoordinates.valid && !displayGridSteps(settings)) {
    if (error) *error = QCoreApplication::translate("Workbench", "当前间距超出视口显示范围，请调整网格间距。");
    return false;
  }
  return true;
}

bool NativeViewport::setViewportGridSettings(const ViewportGridSettings &settings, QString *error) {
  if (!settings.isValid()) {
    if (error) *error = QCoreApplication::translate("Workbench", "视口网格设置无效。");
    return false;
  }
  // Replay valid project preferences even if the current GPU coordinate range
  // cannot draw their periodic grid. Never leak the previous project's display
  // settings into a reopened project; drawInfiniteGrid omits unsafe levels and
  // details expose the limitation. Explicit UI Apply uses the display preflight.
  const bool changed = settings.toJson() != mViewportGridSettings.toJson();
  mViewportGridSettings = settings;
  refreshViewportPresentation();
  emit viewportPresentationChanged();
  if (changed) update();
  return true;
}

QString NativeViewport::formatReferenceLength(const double value) const {
  const auto coordinates = sceneReferenceCoordinates();
  const auto metres = sceneMetresPerCoordinate(coordinates);
  QString symbol = QStringLiteral("u");
  double shown = value;
  if (mViewportGridSettings.displayUnit == ViewportGridSettings::DisplayUnit::Scene) {
    if (metres && coordinates.unit != SceneLengthUnit::Unknown) {
      const double nominal = coordinates.unit == SceneLengthUnit::Millimetres ? 0.001 :
          coordinates.unit == SceneLengthUnit::Centimetres ? 0.01 : 1.0;
      if (std::abs(*metres / nominal - 1.0) < 1e-10) symbol = sceneLengthUnitSymbol(coordinates.unit);
    }
  } else if (metres) {
    double target = 1.0;
    if (mViewportGridSettings.displayUnit == ViewportGridSettings::DisplayUnit::Millimetres) { target = 0.001; symbol = QStringLiteral("mm"); }
    else if (mViewportGridSettings.displayUnit == ViewportGridSettings::DisplayUnit::Centimetres) { target = 0.01; symbol = QStringLiteral("cm"); }
    else symbol = QStringLiteral("m");
    if (value != 0.0) {
      int a = 0, b = 0, c = 0;
      const double mantissa = std::frexp(value, &a) * std::frexp(*metres, &b) / std::frexp(target, &c);
      shown = std::ldexp(mantissa, a + b - c);
      if (!std::isfinite(shown) || shown == 0.0)
        return QCoreApplication::translate("Workbench", "超出显示范围");
    }
  }
  return QStringLiteral("%1 %2").arg(QString::number(shown, 'g', 7), symbol);
}

void NativeViewport::refreshViewportPresentation() {
  if (!mViewportStatusLabel || !mViewportDetailsButton) return;
  const int scale = qApp->property("gswUiScalePercent").toInt();
  const int margin = AppTheme::scaled(10, scale);
  const int buttonEdge = std::max(32, AppTheme::scaled(28, scale));
  const int iconEdge = AppTheme::scaled(17, scale);
  if (mViewportStatusLabel->font() != font()) mViewportStatusLabel->setFont(font());
  const auto colors = viewportColors();
  if (mPresentationInk != colors.muted) {
    mPresentationInk = colors.muted;
    mViewportStatusLabel->setStyleSheet(QStringLiteral(
        "QLabel#viewportStatusLabel{background:transparent;color:%1;border:0;padding:0;}")
        .arg(colors.muted.name(QColor::HexRgb)));
    QPalette ink = palette();
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
      ink.setColor(role, colors.muted);
    mViewportDetailsButton->setIcon(workbenchIcon(WorkbenchIconId::Info, ink));
    mViewportDetailsButton->setStyleSheet(QStringLiteral(
        "QToolButton#viewportDetailsButton{background:transparent;border:0;padding:0;}"
        "QToolButton#viewportDetailsButton:hover{background:%1;border-radius:4px;}")
        .arg(lightViewport() ? QStringLiteral("rgba(0,0,0,18)") : QStringLiteral("rgba(255,255,255,24)")));
  }
  mViewportDetailsButton->setIconSize(QSize(iconEdge, iconEdge));
  const QRect buttonRect(margin, std::max(0, height() - margin - buttonEdge), buttonEdge, buttonEdge);
  if (mViewportDetailsButton->geometry() != buttonRect) mViewportDetailsButton->setGeometry(buttonRect);
  const QFontMetrics metrics(mViewportStatusLabel->font());
  const int available = std::max(1, width() - 2 * margin - buttonEdge - AppTheme::scaled(100, scale));
  const QString full = viewportStatusText();
  QString shown = full;
  if (metrics.horizontalAdvance(shown) > available) shown = viewportPerformanceText();
  shown = metrics.elidedText(shown, Qt::ElideRight, available);
  if (mViewportStatusLabel->text() != shown) mViewportStatusLabel->setText(shown);
  mViewportStatusLabel->setAccessibleName(full);
  const QRect labelRect(buttonRect.right() + 5,
      buttonRect.center().y() - (metrics.height() + 4) / 2,
      available, metrics.height() + 4);
  if (mViewportStatusLabel->geometry() != labelRect) mViewportStatusLabel->setGeometry(labelRect);
}

void NativeViewport::updateFrameRefreshPolicy() {
  // The optimizer has priority while a task is running. This changes actual
  // drawing cadence only; FrameRateCounter still counts completed frames.
  mFrameRefreshTimer->setInterval(mProcessingActive ? 33 : 0);
  // Only a live worker mapping requires polling. Resident scenes and snapshots
  // are event-driven, including async completion and navigation update() calls.
  mTrainingGpuPreviewTimer->setInterval(33);
  const bool exposed = isVisible() && window() && !window()->isMinimized();
  const bool pollTraining = exposed && mProcessingActive && mProcessingPreviewVisible &&
      !mProcessingPreviewPaused && (mPendingTrainingGpuPreviewDescriptor.has_value() ||
                                   mTrainingGpuPreview.attached());
  if (pollTraining && !mTrainingGpuPreviewTimer->isActive()) mTrainingGpuPreviewTimer->start();
  else if (!pollTraining && mTrainingGpuPreviewTimer->isActive()) mTrainingGpuPreviewTimer->stop();
  const bool shouldRefreshContinuously =
      exposed && !(mProcessingPreviewPaused && mProcessingPreviewVisible) &&
      hasPendingFrameUploads();
  if (shouldRefreshContinuously && !mFrameRefreshTimer->isActive()) {
    mFrameRefreshTimer->start();
  } else if (!shouldRefreshContinuously && mFrameRefreshTimer->isActive()) {
    mFrameRefreshTimer->stop();
  }
}

void NativeViewport::notifyEditState() {
  emit editStateChanged(mScene->mEditModel.selectedCount(), mScene->mEditModel.deletedCount(),
                        mScene->visible && (mScene->mEditModel.canUndo() || canUndoModelTransform()),
                        mScene->visible && (mScene->mEditModel.canRedo() || canRedoModelTransform()),
                        hasEditableScene(), mScene->mEditModel.hasUnsavedChanges());
}

bool NativeViewport::canUndoModelTransform() const {
  if (mTransformHistoryIndex <= 0 || mTransformHistoryIndex > mTransformHistory.size()) return false;
  const auto &objects = mTransformHistory.at(mTransformHistoryIndex - 1).before;
  return std::all_of(objects.cbegin(), objects.cend(), [&](const auto &object) {
    return sceneObjectVisible(object.id);
  });
}

bool NativeViewport::canRedoModelTransform() const {
  if (mTransformHistoryIndex >= mTransformHistory.size()) return false;
  const auto &objects = mTransformHistory.at(mTransformHistoryIndex).after;
  return std::all_of(objects.cbegin(), objects.cend(), [&](const auto &object) {
    return sceneObjectVisible(object.id);
  });
}

void NativeViewport::resetModelTransformHistory() {
  mTransformHistory.clear();
  mTransformHistoryIndex = 0;
}

void NativeViewport::commitModelTransform() {
  const auto after = selectedTransforms();
  bool changed = false;
  for (qsizetype i = 0; i < std::min(after.size(), mTransformDragBefore.size()); ++i) {
    const auto &before = mTransformDragBefore[i];
    changed |= (after[i].translation - before.translation).lengthSquared() > 1.0e-12F ||
        !rotationsEquivalent(after[i].rotation, before.rotation) || !scalesEquivalent(after[i].scale, before.scale);
  }
  if (!changed) return;
  mTransformHistory.resize(mTransformHistoryIndex);
  mTransformHistory.append({mTransformDragBefore, after});
  if (mTransformHistory.size() > 64) mTransformHistory.removeFirst();
  mTransformHistoryIndex = mTransformHistory.size();
  applySceneTransforms(after, true);
  notifyEditState();
  notifyModelInteractionState();
}

QList<SceneObject> NativeViewport::selectedTransforms() const {
  QList<SceneObject> result;
  for (const auto &state : mSceneStates) {
    if (!state->visible || !mSelectedSceneIds.contains(state->id)) continue;
    result.append({state->id, state->mScenePath,
        state->mModelTranslation / static_cast<float>(state->mSceneCoordinates.displayScale),
        state->mModelRotation, state->mModelScale, state->mGaussianCount, state->visible});
  }
  return result;
}

void NativeViewport::applySceneTransforms(const QList<SceneObject> &objects, const bool notify) {
  for (const auto &object : objects) {
    const auto found = std::find_if(mSceneStates.cbegin(), mSceneStates.cend(),
        [&](const auto &state) { return state->id == object.id && state->mScenePath == object.path; });
    if (found == mSceneStates.cend()) continue;
    auto &state = **found;
    state.sourceTranslation = object.translation;
    state.mModelTranslation = object.translation * static_cast<float>(state.mSceneCoordinates.displayScale);
    state.mModelRotation = object.rotation;
    state.mModelScale = object.scale;
    state.sortDirectionValid = false;
  }
  if (notify) {
    if (objects.size() == 1 && objects[0].id == mScene->id) {
      emit modelTransformCommitted(mScene->mModelTranslation, mScene->mModelRotation, mScene->mModelScale);
    } else {
      emit sceneTransformsCommitted(objects);
    }
  }
  update();
}

QVector<QVector3D> NativeViewport::selectionBoundsCorners() const {
  QVector<QVector3D> result;
  for (const auto &state : mSceneStates) {
    if (!state->visible || !mSelectedSceneIds.contains(state->id)) continue;
    const QVector3D low = state->mSceneCoordinates.valid ? state->mSceneCoordinates.localMinimum() :
        state->mSceneCenter - QVector3D(state->mSceneRadius, state->mSceneRadius, state->mSceneRadius);
    const QVector3D high = state->mSceneCoordinates.valid ? state->mSceneCoordinates.localMaximum() :
        state->mSceneCenter + QVector3D(state->mSceneRadius, state->mSceneRadius, state->mSceneRadius);
    const QMatrix4x4 matrix = sceneDisplayTransform(*state, *mScene) *
        ModelTransform{state->mModelTranslation, state->mModelRotation, state->mModelScale}.matrix(state->mSceneCenter);
    for (int i = 0; i < 8; ++i) result.append(matrix.map(QVector3D(
        i & 1 ? high.x() : low.x(), i & 2 ? high.y() : low.y(), i & 4 ? high.z() : low.z())));
  }
  return result;
}

QVector3D NativeViewport::selectionPivot() const {
  if (mSelectedSceneIds.size() <= 1) return transformedSceneCenter();
  if (mModelDragActive && mTransformDragBefore.size() > 1) return mGroupPreviewPivot;
  const auto corners = selectionBoundsCorners();
  if (corners.isEmpty()) return transformedSceneCenter();
  QVector3D low = corners.first(), high = low;
  for (const auto &corner : corners) {
    for (int axis = 0; axis < 3; ++axis) {
      low[axis] = std::min(low[axis], corner[axis]);
      high[axis] = std::max(high[axis], corner[axis]);
    }
  }
  return (low + high) * 0.5F;
}

void NativeViewport::applyGroupTransform(const QVector3D &translation,
                                         const QQuaternion &rotation, const float factor) {
  // Clamp the common factor, not each member separately: preserve the group's
  // proportions even when one member reaches the supported scale limits.
  double minimumFactor = 0.0, maximumFactor = 1.0e12;
  for (const auto &before : mTransformDragBefore) {
    for (int axis = 0; axis < 3; ++axis) {
      const double magnitude = std::abs(static_cast<double>(before.scale[axis]));
      minimumFactor = std::max(minimumFactor, static_cast<double>(1.0e-4F) / magnitude);
      maximumFactor = std::min(maximumFactor, 1.0e4 / magnitude);
    }
  }
  float lower = static_cast<float>(minimumFactor), upper = static_cast<float>(maximumFactor);
  if (lower < minimumFactor) lower = std::nextafter(lower, std::numeric_limits<float>::infinity());
  if (upper > maximumFactor) upper = std::nextafter(upper, 0.0F);
  const float commonFactor = std::copysign(std::clamp(std::abs(factor), lower, upper), factor);
  for (const auto &before : mTransformDragBefore) {
    const auto found = std::find_if(mSceneStates.cbegin(), mSceneStates.cend(),
        [&](const auto &state) { return state->id == before.id; });
    if (found == mSceneStates.cend()) continue;
    auto &state = **found;
    const QMatrix4x4 mapping = sceneDisplayTransform(state, *mScene);
    const QVector3D oldCenter = mapping.map(state.mSceneCenter + before.translation *
        static_cast<float>(state.mSceneCoordinates.displayScale));
    const QVector3D newCenter = mGroupPivot + translation +
        rotation.rotatedVector((oldCenter - mGroupPivot) * commonFactor);
    state.mModelTranslation = mapping.inverted().map(newCenter) - state.mSceneCenter;
    QMatrix3x3 basis;
    for(int column=0;column<3;++column) {
      const auto axis=mapping.mapVector(column==0?QVector3D(1,0,0):column==1?QVector3D(0,1,0):QVector3D(0,0,1)).normalized();
      for(int row=0;row<3;++row)basis(row,column)=axis[row];
    }
    const auto frameRotation=QQuaternion::fromRotationMatrix(basis).normalized();
    state.mModelRotation = normalizedModelRotation(frameRotation.conjugated()*rotation*frameRotation*before.rotation);
    state.mModelScale = normalizedModelScale(before.scale * commonFactor);
    state.sortDirectionValid = false;
  }
  mGroupPreviewPivot = mGroupPivot + translation;
}

void NativeViewport::notifyModelInteractionState() {
  emit modelInteractionStateChanged(
      selectableModelAvailable(), mModelSelected, canUndoModelTransform(),
      canRedoModelTransform(), mScene->mModelTranslation, mScene->mModelRotation, mScene->mModelScale);
}

void NativeViewport::uploadPendingPointCloud() {
  if (!mScene->mPointUploadPending || !mScene->mPointBuffer.isCreated()) {
    return;
  }
  QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mScene->mPointVertexArray);
  while (glGetError() != GL_NO_ERROR) {}
  mScene->mPointBuffer.bind();
  const qsizetype byteCount = mScene->mPendingVertices.size() *
                              static_cast<qsizetype>(sizeof(PointCloudVertex));
  mScene->mPointBuffer.allocate(
      mScene->mPendingVertices.isEmpty() ? nullptr : mScene->mPendingVertices.constData(),
      static_cast<int>(byteCount));
  mScene->resourcePointGpuBytes = std::max(0, mScene->mPointBuffer.size());
  const GLenum uploadError = glGetError();
  mScene->mPointBuffer.release();
  if (mScene->mHasMesh && (uploadError != GL_NO_ERROR ||
                         mScene->resourcePointGpuBytes != byteCount)) {
    mScene->resourceNeedsReload = true;
    mScene->resourceForcePagedRetry = true;
    mScene->resourceResidentUploadFailed = true;
    mScene->resourceRetryAfterMs = mResourceClock.elapsed();
    mScene->mRenderedPointCount = 0;
  }
  mScene->mPendingVertices = {};
  mScene->mPointUploadPending = false;
}

void NativeViewport::releaseFullResolutionPointCloud() {
  for (const FullResolutionGpuChunk &chunk :
       std::as_const(mScene->mFullResolutionPointGpuChunks)) {
    if (chunk.buffer != 0) {
      glDeleteBuffers(1, &chunk.buffer);
    }
    if (chunk.vertexArray != 0) {
      glDeleteVertexArrays(1, &chunk.vertexArray);
    }
  }
  mScene->mFullResolutionPointGpuChunks.clear();
  mScene->mPointCacheResidentBytes = 0;
  mScene->mUploadedFullResolutionPointCount = 0;
}

void NativeViewport::updatePointCacheSelection(
    const QMatrix4x4 &viewProjection) {
  if (!mScene->mPreviewOnlyScene || !mScene->mPointCache.isValid()) {
    mScene->mDesiredPointCacheNodes.clear();
    return;
  }
  ++mScene->mPointCacheFrameSerial;
  const bool interacting =
      mPressedButtons != Qt::NoButton || mNavigationInteractionActive ||
      mViewSnapTimer->isActive();
  const float refinementThresholdPixels = interacting ? 140.0F : 28.0F;
  const qsizetype pointBudget = sharedPageGpuBudget() / sizeof(PointPreviewVertex);
  const QVector3D eye = cameraPosition();
  const float viewportHeight =
      std::max(1.0F, static_cast<float>(height() * devicePixelRatioF()));
  const auto projectedPixels = [&](const PointCloudCacheNode &node) {
    const QVector3D center = modelMatrix().map(
        (node.boundsMinimum + node.boundsMaximum) * 0.5F);
    const float maximumScale =
        std::max({std::abs(mScene->mModelScale.x()), std::abs(mScene->mModelScale.y()),
                  std::abs(mScene->mModelScale.z())});
    const float radius = maximumScale *
        std::max((node.boundsMaximum - node.boundsMinimum).length() * 0.5F,
                 mScene->mSceneRadius * 1.0e-6F);
    if (mOrthographic) {
      return radius / std::max(mDistance, 1.0e-5F) * viewportHeight * 2.0F;
    }
    const float distance =
        std::max((center - eye).length() - radius, mScene->mSceneRadius * 1.0e-5F);
    return radius / distance * viewportHeight / std::tan(radians(22.5F));
  };
  const auto visible = [&](const PointCloudCacheNode &node) {
    return node.sourcePointCount > 0 &&
           boundsIntersectView(node.boundsMinimum, node.boundsMaximum,
                               viewProjection);
  };

  QSet<int> selected;
  QVector<int> refinable;
  const PointCloudCacheNode &root =
      mScene->mPointCache.nodes.at(mScene->mPointCache.rootNode);
  qsizetype selectedPoints = 0;
  if (visible(root) && root.pointCount <= pointBudget) {
    selected.insert(root.id);
    refinable.append(root.id);
    selectedPoints = static_cast<qsizetype>(root.pointCount);
  }
  while (!refinable.isEmpty()) {
    qsizetype bestPosition = -1;
    float bestPriority = refinementThresholdPixels;
    for (qsizetype position = 0; position < refinable.size(); ++position) {
      const PointCloudCacheNode &candidate =
          mScene->mPointCache.nodes.at(refinable.at(position));
      const float priority = projectedPixels(candidate);
      if (priority > bestPriority) {
        bestPriority = priority;
        bestPosition = position;
      }
    }
    if (bestPosition < 0) {
      break;
    }
    const int nodeId = refinable.takeAt(bestPosition);
    const PointCloudCacheNode &node = mScene->mPointCache.nodes.at(nodeId);
    QVector<int> visibleChildren;
    qsizetype childPoints = 0;
    for (const int childId : node.children) {
      const PointCloudCacheNode &child = mScene->mPointCache.nodes.at(childId);
      if (!visible(child) || child.pointCount <= 0) {
        continue;
      }
      visibleChildren.append(childId);
      childPoints += static_cast<qsizetype>(child.pointCount);
    }
    if (visibleChildren.isEmpty()) {
      continue;
    }
    const qsizetype withoutParent =
        selectedPoints - static_cast<qsizetype>(node.pointCount);
    if (withoutParent + childPoints > pointBudget) {
      continue;
    }
    selected.remove(nodeId);
    selectedPoints = withoutParent + childPoints;
    for (const int childId : std::as_const(visibleChildren)) {
      selected.insert(childId);
      if (!mScene->mPointCache.nodes.at(childId).isLeaf()) {
        refinable.append(childId);
      }
    }
  }
  mScene->mDesiredPointCacheNodes = std::move(selected);
  requestMissingPointCachePages();
}

void NativeViewport::requestMissingPointCachePages() {
  if (!mScene->visible || !mScene->mPointCache.isValid()) {
    return;
  }
  const auto isResident = [this](const int nodeId) {
    return std::any_of(mScene->mFullResolutionPointGpuChunks.cbegin(),
                       mScene->mFullResolutionPointGpuChunks.cend(),
                       [nodeId](const FullResolutionGpuChunk &chunk) {
                         return chunk.nodeId == nodeId;
                       });
  };
  QSet<int> needed;
  QSet<int> queued;
  for (const auto &page : mScene->mPendingPointCachePages) queued.insert(page.nodeId);
  for (const int desired : std::as_const(mScene->mDesiredPointCacheNodes)) {
    int nodeId = desired;
    while (nodeId >= 0 && !isResident(nodeId)) {
      if (!mScene->mPointCacheReadsInFlight.contains(nodeId) &&
          !queued.contains(nodeId) &&
          !mScene->mPointCacheFailedNodes.contains(nodeId)) {
        needed.insert(nodeId);
      }
      nodeId = mScene->mPointCache.nodes.at(nodeId).parent;
    }
  }
  QVector<int> ordered = needed.values();
  std::sort(ordered.begin(), ordered.end(), [this](const int left,
                                                   const int right) {
    const PointCloudCacheNode &a = mScene->mPointCache.nodes.at(left);
    const PointCloudCacheNode &b = mScene->mPointCache.nodes.at(right);
    if (a.depth != b.depth) {
      return a.depth < b.depth;
    }
    return a.pointCount < b.pointCount;
  });
  // Keep two physical read jobs, not two single-page GUI handoffs. Slow
  // software-rendered frames must not starve the existing eight-page drain.
  // Each job is immutable after admission and retains every per-page lease.
  // Old-generation jobs keep their lanes until actual completion, even when
  // cache queues are cleared, so reloads cannot increase I/O concurrency.
  constexpr qsizetype kMaximumQueuedPages = 16;
  qsizetype stagedPages = mScene->mPendingPointCachePages.size();
  for (const auto &activeLane : mScene->mPointCacheReadLanes) {
    if (const auto activeBatch = activeLane.lock())
      stagedPages += activeBatch->nodeIds.size();
  }
  qsizetype nextNode = 0;
  for (auto &lane : mScene->mPointCacheReadLanes) {
    if (!lane.expired()) continue;
    auto work = std::make_shared<PointCacheReadBatch>();
    work->generation = mScene->mSceneGeneration;
    work->cacheEpoch = mScene->mPointCacheReadEpoch;
    lichtfeld::UploadDrainBudget readBatch(8ULL * 1024 * 1024, 8, 0);
    while (nextNode < ordered.size() && stagedPages < kMaximumQueuedPages) {
      const int nodeId = ordered.at(nextNode++);
      const qint64 count = mScene->mPointCache.nodes.at(nodeId).pointCount;
      if (count <= 0 || count > std::numeric_limits<qint64>::max() /
                                   (3 * qint64(sizeof(PointPreviewVertex)))) {
        mScene->mPointCacheFailedNodes.insert(nodeId);
        mScene->mPointCacheError = QCoreApplication::translate(
            "Workbench", "Invalid point-cache node request.");
        continue;
      }
      const qint64 pageBytes = count * qint64(sizeof(PointPreviewVertex));
      if (pageBytes > sharedPageGpuBudget()) continue;
      if (!readBatch.permits(static_cast<std::uint64_t>(pageBytes), 0)) {
        --nextNode;
        break;
      }
      auto reservation = mResources->createReservation();
      if (!reservation->tryResize(pageBytes * 3, 0)) continue;
      work->nodeIds.append(nodeId);
      work->reservations.insert(nodeId, std::move(reservation));
      mScene->mPointCacheReadsInFlight.insert(nodeId, work);
      ++stagedPages;
      readBatch.recordPage(static_cast<std::uint64_t>(pageBytes));
    }
    if (work->nodeIds.isEmpty()) continue;
    const std::shared_ptr<const PointCacheReadBatch> batch = std::move(work);
    lane = batch;
    const PointCloudCacheIndex cache = mScene->mPointCache;
    auto *watcher = new QFutureWatcher<QVector<PointCloudCachePage>>(this);
    connect(watcher, &QFutureWatcher<QVector<PointCloudCachePage>>::finished, this,
            [this, watcher, batch,
             weakScene = std::weak_ptr<SceneState>(mScene)]() {
            const auto targetScene = weakScene.lock();
            if (!targetScene) { watcher->deleteLater(); return; }
            const bool foreground = targetScene == mScene;
            QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, targetScene);
            QScopedValueRollback<bool> backgroundScope(mRenderingInactiveScene, !foreground);
            QSignalBlocker sceneSignals(this);
            if (foreground) sceneSignals.unblock();
              auto pages = watcher->result();
              watcher->deleteLater();
              for (auto &activeLane : mScene->mPointCacheReadLanes) {
                if (activeLane.lock() == batch) activeLane.reset();
              }
              for (const int nodeId : batch->nodeIds) {
                if (mScene->mPointCacheReadsInFlight.value(nodeId).lock() == batch)
                  mScene->mPointCacheReadsInFlight.remove(nodeId);
              }
              if (batch->generation != mScene->mSceneGeneration ||
                  batch->cacheEpoch != mScene->mPointCacheReadEpoch) {
                updateFrameRefreshPolicy();
                update();
                return;
              }
              if (!mScene->visible) { updateFrameRefreshPolicy(); return; }
              for (auto &page : pages) {
                if (page.isValid()) {
                  mScene->resourcePointPageReservations.insert(
                      page.nodeId, batch->reservations.value(page.nodeId));
                  mScene->mPendingPointCachePages.append(std::move(page));
                } else {
                  mScene->mPointCacheFailedNodes.insert(page.nodeId);
                  mScene->mPointCacheError = page.error;
                }
              }
              requestMissingPointCachePages();
              updateFrameRefreshPolicy();
              update();
            });
    watcher->setFuture(QtConcurrent::run(
        [cache, batch]() {
          QVector<PointCloudCachePage> pages;
          pages.reserve(batch->nodeIds.size());
          for (const int nodeId : batch->nodeIds)
            pages.append(PointCloudCache::readNode(cache, nodeId));
          return pages;
        }));
  }
}

void NativeViewport::evictPointCacheUntilFits(const qsizetype requiredBytes) {
  const auto isResident = [this](const int nodeId) {
    return std::any_of(mScene->mFullResolutionPointGpuChunks.cbegin(),
                       mScene->mFullResolutionPointGpuChunks.cend(),
                       [nodeId](const FullResolutionGpuChunk &chunk) {
                         return chunk.nodeId == nodeId;
                       });
  };
  QSet<int> protectedNodes;
  for (const int desired : std::as_const(mScene->mDesiredPointCacheNodes)) {
    int nodeId = desired;
    while (nodeId >= 0) {
      if (isResident(nodeId)) {
        protectedNodes.insert(nodeId);
        break;
      }
      nodeId = mScene->mPointCache.nodes.at(nodeId).parent;
    }
  }
  while (!mScene->mFullResolutionPointGpuChunks.isEmpty() &&
         mScene->mPointCacheResidentBytes + requiredBytes >
             sharedPageGpuBudget()) {
    qsizetype victim = -1;
    quint64 oldest = std::numeric_limits<quint64>::max();
    for (qsizetype index = 0;
         index < mScene->mFullResolutionPointGpuChunks.size(); ++index) {
      const FullResolutionGpuChunk &chunk =
          mScene->mFullResolutionPointGpuChunks.at(index);
      if (!protectedNodes.contains(chunk.nodeId) &&
          chunk.lastUsedFrame < oldest) {
        victim = index;
        oldest = chunk.lastUsedFrame;
      }
    }
    if (victim < 0) {
      for (qsizetype index = 0;
           index < mScene->mFullResolutionPointGpuChunks.size(); ++index) {
        const FullResolutionGpuChunk &chunk =
            mScene->mFullResolutionPointGpuChunks.at(index);
        if (chunk.lastUsedFrame < oldest) {
          victim = index;
          oldest = chunk.lastUsedFrame;
        }
      }
    }
    if (victim < 0) {
      break;
    }
    const FullResolutionGpuChunk chunk =
        mScene->mFullResolutionPointGpuChunks.takeAt(victim);
    if (chunk.buffer != 0) {
      glDeleteBuffers(1, &chunk.buffer);
    }
    if (chunk.vertexArray != 0) {
      glDeleteVertexArrays(1, &chunk.vertexArray);
    }
    mScene->mPointCacheResidentBytes -= chunk.byteCount;
    updateManagedResourceUsage();
  }
}

void NativeViewport::uploadPendingPointCachePages() {
  if (mScene->mFullResolutionPointClearPending) {
    releaseFullResolutionPointCloud();
    mScene->mFullResolutionPointClearPending = false;
  }
  if (mScene->mPendingPointCachePages.isEmpty() || mPointProgram == nullptr ||
      !mPointProgram->isLinked()) {
    return;
  }
  QElapsedTimer uploadTime;
  uploadTime.start();
  lichtfeld::UploadDrainBudget batch;
  while (!mScene->mPendingPointCachePages.isEmpty()) {
    const auto nextBytes = static_cast<std::uint64_t>(mScene->mPendingPointCachePages.constFirst().vertices.size()) * sizeof(PointPreviewVertex);
    if (!batch.permits(nextBytes, static_cast<std::uint64_t>(uploadTime.nsecsElapsed()))) break;
    PointCloudCachePage page = mScene->mPendingPointCachePages.takeFirst();
    batch.recordPage(nextBytes);
    auto ramReservation = mScene->resourcePointPageReservations.take(page.nodeId);
    const bool alreadyResident = std::any_of(
        mScene->mFullResolutionPointGpuChunks.cbegin(),
        mScene->mFullResolutionPointGpuChunks.cend(),
        [&page](const FullResolutionGpuChunk &chunk) {
          return chunk.nodeId == page.nodeId;
        });
    if (!alreadyResident && page.nodeId >= 0 &&
        page.nodeId < mScene->mPointCache.nodes.size()) {
      const qsizetype byteCount =
          page.vertices.size() * static_cast<qsizetype>(sizeof(PointPreviewVertex));
      trimHiddenPageCaches(byteCount);
      evictPointCacheUntilFits(byteCount);
      updateManagedResourceUsage();
      auto gpuReservation = mResources->createReservation();
      if (byteCount > sharedPageGpuBudget() - mScene->mPointCacheResidentBytes ||
          !gpuReservation->tryResize(0, byteCount)) {
        mScene->mPointCacheFailedNodes.insert(page.nodeId);
        continue;
      }
      FullResolutionGpuChunk gpuChunk;
      gpuChunk.nodeId = page.nodeId;
      gpuChunk.pointCount = static_cast<GLsizei>(page.vertices.size());
      gpuChunk.byteCount = byteCount;
      gpuChunk.lastUsedFrame = mScene->mPointCacheFrameSerial;
      const PointCloudCacheNode &node = mScene->mPointCache.nodes.at(page.nodeId);
      gpuChunk.boundsMinimum = node.boundsMinimum;
      gpuChunk.boundsMaximum = node.boundsMaximum;
      while (glGetError() != GL_NO_ERROR) {
      }
      glGenVertexArrays(1, &gpuChunk.vertexArray);
      glGenBuffers(1, &gpuChunk.buffer);
      glBindVertexArray(gpuChunk.vertexArray);
      glBindBuffer(GL_ARRAY_BUFFER, gpuChunk.buffer);
      glBufferData(GL_ARRAY_BUFFER, byteCount, page.vertices.constData(),
                   GL_STATIC_DRAW);
      const GLenum uploadError = glGetError();
      if (uploadError == GL_NO_ERROR) {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                              sizeof(PointPreviewVertex), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(
            1, 3, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(PointPreviewVertex),
            reinterpret_cast<const void *>(offsetof(PointPreviewVertex, red)));
        mScene->mPointCacheResidentBytes += byteCount;
        mScene->mFullResolutionPointGpuChunks.append(gpuChunk);
        updateManagedResourceUsage();
      } else {
        glDeleteBuffers(1, &gpuChunk.buffer);
        glDeleteVertexArrays(1, &gpuChunk.vertexArray);
        mScene->mPointCacheFailedNodes.insert(page.nodeId);
        mScene->mPointCacheError = QCoreApplication::translate("Workbench", "GPU memory could not accept point-cache node %1.")
                               .arg(page.nodeId);
      }
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindVertexArray(0);
    }
  }
  updateManagedResourceUsage(); // all dequeued pages and temporary leases are now released
  mScene->mUploadedFullResolutionPointCount = 0;
  for (const FullResolutionGpuChunk &chunk :
       std::as_const(mScene->mFullResolutionPointGpuChunks)) {
    mScene->mUploadedFullResolutionPointCount += chunk.pointCount;
  }
  requestMissingPointCachePages();
  updateFrameRefreshPolicy();
  update();
}

void NativeViewport::releaseFullResolutionMesh() {
  for (const FullResolutionMeshGpuChunk &chunk :
       std::as_const(mScene->mFullResolutionMeshGpuChunks)) {
    if (chunk.indexBuffer != 0) {
      glDeleteBuffers(1, &chunk.indexBuffer);
    }
    if (chunk.vertexBuffer != 0) {
      glDeleteBuffers(1, &chunk.vertexBuffer);
    }
    if (chunk.vertexArray != 0) {
      glDeleteVertexArrays(1, &chunk.vertexArray);
    }
  }
  mScene->mFullResolutionMeshGpuChunks.clear();
  mScene->mMeshCacheResidentBytes = 0;
  mScene->mUploadedFullResolutionMeshTriangleCount = 0;
  mScene->mDrawnFullResolutionMeshTriangleCount = 0;
}

void NativeViewport::releaseMeshTexture() {
  mScene->resourceTextureGpuBytes = 0;
  if (mScene->mMeshTexture != 0) {
    glDeleteTextures(1, &mScene->mMeshTexture);
    mScene->mMeshTexture = 0;
  }
  mScene->mMeshTextureReady = false;
  mScene->mMeshTextureSize = {};
}

void NativeViewport::uploadPendingMeshTexture() {
  if (mScene->mMeshTextureClearPending) {
    releaseMeshTexture();
    mScene->mMeshTextureClearPending = false;
  }
  if (!mScene->mMeshTextureUploadPending) {
    return;
  }
  mScene->mMeshTextureUploadPending = false;
  if (!mScene->mMeshHasTextureCoordinates || mScene->mPendingMeshTexture.isNull()) {
    mScene->mPendingMeshTexture = {};
    return;
  }

  GLint maximumTextureSize = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
  QImage image = mScene->mPendingMeshTexture;
  if (maximumTextureSize > 0 &&
      (image.width() > maximumTextureSize ||
       image.height() > maximumTextureSize)) {
    image = image.scaled(maximumTextureSize, maximumTextureSize,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    mScene->mMeshTextureError =
        QCoreApplication::translate("Workbench", "Texture was reduced to the GPU limit of %1 px.")
            .arg(maximumTextureSize);
  }
  image = image.convertToFormat(QImage::Format_RGBA8888)
              .mirrored(false, true);
  mScene->mPendingMeshTexture = {};
  if (image.isNull()) {
    mScene->mMeshTextureError = QCoreApplication::translate("Workbench", "The mesh texture could not be converted for OpenGL.");
    return;
  }

  while (glGetError() != GL_NO_ERROR) {
  }
  glGenTextures(1, &mScene->mMeshTexture);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mScene->mMeshTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  constexpr GLenum kMaximumTextureAnisotropy = 0x84FF;
  constexpr GLenum kTextureAnisotropy = 0x84FE;
  if (QOpenGLContext::currentContext()->hasExtension(
          QByteArrayLiteral("GL_EXT_texture_filter_anisotropic"))) {
    GLfloat maximumAnisotropy = 1.0F;
    glGetFloatv(kMaximumTextureAnisotropy, &maximumAnisotropy);
    glTexParameterf(GL_TEXTURE_2D, kTextureAnisotropy,
                    std::min(maximumAnisotropy, 8.0F));
  }
  // The context is shared with QPainter. Qt's 8-bit glyph rows are padded to
  // four bytes, so leaking alignment 1 corrupts newly uploaded text glyphs.
  GLint previousUnpackAlignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &previousUnpackAlignment);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0,
               GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
  glPixelStorei(GL_UNPACK_ALIGNMENT, previousUnpackAlignment);
  glGenerateMipmap(GL_TEXTURE_2D);
  const GLenum uploadError = glGetError();
  glBindTexture(GL_TEXTURE_2D, 0);
  if (uploadError != GL_NO_ERROR || mScene->mMeshTexture == 0) {
    glDeleteTextures(1, &mScene->mMeshTexture);
    mScene->mMeshTexture = 0;
    mScene->mMeshTextureError = QCoreApplication::translate("Workbench", "GPU memory could not accept the mesh texture.");
    return;
  }
  mScene->mMeshTextureSize = image.size();
  mScene->resourceTextureGpuBytes = 0;
  for (QSize level = image.size(); level.width() > 0 && level.height() > 0;) {
    mScene->resourceTextureGpuBytes += qint64(level.width()) * level.height() * 4;
    if (level.width() == 1 && level.height() == 1) break;
    level = QSize(std::max(1, level.width() / 2), std::max(1, level.height() / 2));
  }
  mScene->mMeshTextureReady = true;
  updateFrameRefreshPolicy();
}

void NativeViewport::updateMeshCacheSelection(
    const QMatrix4x4 &viewProjection) {
  if (!pagedMeshAvailable()) {
    mScene->mDesiredMeshCacheNodes.clear();
    return;
  }
  ++mScene->mMeshCacheFrameSerial;
  const bool interacting =
      mPressedButtons != Qt::NoButton || mNavigationInteractionActive ||
      mViewSnapTimer->isActive();
  // Ancestor pages contain sampled original triangles, not a continuous
  // simplified surface. Never stop at them just because their projected size
  // is small or navigation is active; refine to exact pages when budget allows.
  // Point previews keep their existing prefetch policy instead of loading
  // full triangle pages that they do not draw.
  const float refinementThresholdPixels = mScene->mRenderMode == RenderMode::Mesh
      ? 0.0F : (interacting ? 180.0F : 34.0F);
  const qsizetype byteBudget = sharedPageGpuBudget();
  const QVector3D eye = cameraPosition();
  const float viewportHeight =
      std::max(1.0F, static_cast<float>(height() * devicePixelRatioF()));
  const auto projectedPixels = [&](const MeshCacheNode &node) {
    const QVector3D center = modelMatrix().map(
        (node.boundsMinimum + node.boundsMaximum) * 0.5F);
    const float maximumScale =
        std::max({std::abs(mScene->mModelScale.x()), std::abs(mScene->mModelScale.y()),
                  std::abs(mScene->mModelScale.z())});
    const float radius = maximumScale *
        std::max((node.boundsMaximum - node.boundsMinimum).length() * 0.5F,
                 mScene->mSceneRadius * 1.0e-6F);
    if (mOrthographic) {
      return radius / std::max(mDistance, 1.0e-5F) * viewportHeight * 2.0F;
    }
    const float distance =
        std::max((center - eye).length() - radius, mScene->mSceneRadius * 1.0e-5F);
    return radius / distance * viewportHeight / std::tan(radians(22.5F));
  };
  const auto visible = [&](const MeshCacheNode &node) {
    return node.isValid() &&
           boundsIntersectView(node.boundsMinimum, node.boundsMaximum,
                               viewProjection);
  };
  const auto pageBytes = [](const MeshCacheNode &node) {
    return static_cast<qsizetype>(std::min<qint64>(
        node.byteCount(), std::numeric_limits<qsizetype>::max()));
  };

  QSet<int> selected;
  QVector<int> refinable;
  const MeshCacheNode &root = mScene->mMeshCache.nodes.at(mScene->mMeshCache.rootNode);
  qsizetype selectedBytes = 0;
  if (visible(root) && pageBytes(root) <= byteBudget) {
    selected.insert(root.id);
    refinable.append(root.id);
    selectedBytes = pageBytes(root);
  }
  while (!refinable.isEmpty()) {
    qsizetype bestPosition = -1;
    float bestPriority = refinementThresholdPixels;
    for (qsizetype position = 0; position < refinable.size(); ++position) {
      const MeshCacheNode &candidate =
          mScene->mMeshCache.nodes.at(refinable.at(position));
      const float priority = projectedPixels(candidate);
      if (priority > bestPriority) {
        bestPriority = priority;
        bestPosition = position;
      }
    }
    if (bestPosition < 0) {
      break;
    }
    const int nodeId = refinable.takeAt(bestPosition);
    const MeshCacheNode &node = mScene->mMeshCache.nodes.at(nodeId);
    // A reservoir below its sample limit is already the complete surface.
    // Refining that exact parent would only fetch and upload the same triangles
    // again across hundreds of tiny octree children.
    if (mScene->mRenderMode == RenderMode::Mesh &&
        node.indexCount / 3 == node.sourceTriangleCount) {
      continue;
    }
    QVector<int> visibleChildren;
    qsizetype childBytes = 0;
    for (const int childId : node.children) {
      const MeshCacheNode &child = mScene->mMeshCache.nodes.at(childId);
      if (!visible(child)) {
        continue;
      }
      visibleChildren.append(childId);
      childBytes += pageBytes(child);
    }
    if (visibleChildren.isEmpty()) {
      continue;
    }
    const qsizetype withoutParent = selectedBytes - pageBytes(node);
    if (withoutParent + childBytes > byteBudget) {
      continue;
    }
    selected.remove(nodeId);
    selectedBytes = withoutParent + childBytes;
    for (const int childId : std::as_const(visibleChildren)) {
      selected.insert(childId);
      if (!mScene->mMeshCache.nodes.at(childId).isLeaf()) {
        refinable.append(childId);
      }
    }
  }
  mScene->mDesiredMeshCacheNodes = std::move(selected);
  requestMissingMeshCachePages();
}

void NativeViewport::requestMissingMeshCachePages() {
  if (!mScene->visible) return;
  if (!pagedMeshAvailable() || mScene->resourceLoading) {
    return;
  }
  const auto isResident = [this](const int nodeId) {
    return std::any_of(mScene->mFullResolutionMeshGpuChunks.cbegin(),
                       mScene->mFullResolutionMeshGpuChunks.cend(),
                       [nodeId](const FullResolutionMeshGpuChunk &chunk) {
                         return chunk.nodeId == nodeId;
                       });
  };
  // Finished reads leave the in-flight set before GPU upload. Treat their
  // queued pages as scheduled too, or repeated ancestor reads starve leaf pages.
  QSet<int> queuedNodes;
  for (const auto &page : std::as_const(mScene->mPendingMeshCachePages)) {
    queuedNodes.insert(page.nodeId);
  }
  QSet<int> needed;
  const auto needsRead = [&](const int nodeId) {
    return !isResident(nodeId) &&
        !mScene->mMeshCacheReadsInFlight.contains(nodeId) &&
        !queuedNodes.contains(nodeId) &&
        !mScene->mMeshCacheFailedNodes.contains(nodeId);
  };
  bool drawableCoverage = false;
  for (const int desired : std::as_const(mScene->mDesiredMeshCacheNodes)) {
    if (needsRead(desired)) needed.insert(desired);
    int nodeId = desired;
    while (nodeId >= 0) {
      if (isResident(nodeId)) {
        drawableCoverage = true;
        break;
      }
      nodeId = mScene->mMeshCache.nodes.at(nodeId).parent;
    }
  }
  // The desired cut is admitted against the shared page pool. Reading every
  // missing intermediate ancestor adds storage outside that cut and can evict
  // its endpoints repeatedly, even in a static view. Existing closest-resident
  // ancestors remain drawable; only fetch one initial root if no desired chain
  // has resident coverage yet.
  if (!drawableCoverage && !mScene->mDesiredMeshCacheNodes.isEmpty() &&
      needsRead(mScene->mMeshCache.rootNode)) {
    needed.insert(mScene->mMeshCache.rootNode);
  }
  QVector<int> ordered = needed.values();
  std::sort(ordered.begin(), ordered.end(), [this](const int left,
                                                   const int right) {
    const MeshCacheNode &a = mScene->mMeshCache.nodes.at(left);
    const MeshCacheNode &b = mScene->mMeshCache.nodes.at(right);
    if (a.depth != b.depth) {
      return a.depth < b.depth;
    }
    return a.byteCount() < b.byteCount();
  });
  constexpr qsizetype kMaximumReadsInFlight = 16;
  const qint64 pageBudget = sharedPageGpuBudget();
  const qint64 maximumQueuedBytes = std::min<qint64>(128LL * 1024 * 1024,
      pageBudget / 4);
  qint64 queuedBytes = 0;
  for (const MeshCachePage &page : std::as_const(mScene->mPendingMeshCachePages))
    queuedBytes += page.vertices.size() * qint64(sizeof(MeshVertex)) +
                   page.indices.size() * qint64(sizeof(quint32));
  for (const int nodeId : std::as_const(mScene->mMeshCacheReadsInFlight))
    queuedBytes += mScene->mMeshCache.nodes.at(nodeId).byteCount();
  for (const int nodeId : std::as_const(ordered)) {
    if (mScene->mMeshCacheReadsInFlight.size() >= kMaximumReadsInFlight) {
      break;
    }
    const qint64 bytes = mScene->mMeshCache.nodes.at(nodeId).byteCount();
    // One indivisible page may exceed the prefetch quarter, but never the
    // real shared geometry allowance. Otherwise a large atlas can leave enough
    // GPU room for a root page while the read queue starves it forever.
    if (bytes > pageBudget ||
        (queuedBytes > 0 && bytes > maximumQueuedBytes - queuedBytes)) continue;
    auto reservation = mResources->createReservation();
    if (!reservation->tryResize(bytes * 3, 0)) continue;
    queuedBytes += bytes;
    mScene->mMeshCacheReadsInFlight.insert(nodeId);
    if (meshPagingQaEnabled())
      logMeshPagingQa(MeshPagingQaEvent::Read, mScene->mMeshCache, mScene->mDesiredMeshCacheNodes,
                      nodeId, bytes, mScene->mMeshCacheResidentBytes, pageBudget);
    const int generation = mScene->mSceneGeneration;
    const MeshCacheIndex cache = mScene->mMeshCache;
    auto *watcher = new QFutureWatcher<MeshCachePage>(this);
    connect(watcher, &QFutureWatcher<MeshCachePage>::finished, this,
            [this, watcher, generation, nodeId, reservation,
             weakScene = std::weak_ptr<SceneState>(mScene)]() {
            const auto targetScene = weakScene.lock();
            if (!targetScene) { watcher->deleteLater(); return; }
            const bool foreground = targetScene == mScene;
            QScopedValueRollback<std::shared_ptr<SceneState>> sceneScope(mScene, targetScene);
            QScopedValueRollback<bool> backgroundScope(mRenderingInactiveScene, !foreground);
            QSignalBlocker sceneSignals(this);
            if (foreground) sceneSignals.unblock();
              MeshCachePage page = watcher->result();
              watcher->deleteLater();
              if (generation != mScene->mSceneGeneration) {
                return;
              }
              mScene->mMeshCacheReadsInFlight.remove(nodeId);
              if (!mScene->visible) { updateFrameRefreshPolicy(); return; }
              if (page.isValid()) {
                if (meshPagingQaEnabled())
                  logMeshPagingQa(MeshPagingQaEvent::Enqueue, mScene->mMeshCache, mScene->mDesiredMeshCacheNodes,
                                  nodeId, mScene->mMeshCache.nodes.at(nodeId).byteCount(),
                                  mScene->mMeshCacheResidentBytes, sharedPageGpuBudget());
                mScene->resourceMeshPageReservations.insert(nodeId, reservation);
                mScene->mPendingMeshCachePages.append(std::move(page));
              } else {
                if (meshPagingQaEnabled())
                  logMeshPagingQa(MeshPagingQaEvent::Reject, mScene->mMeshCache, mScene->mDesiredMeshCacheNodes,
                                  nodeId, mScene->mMeshCache.nodes.at(nodeId).byteCount(),
                                  mScene->mMeshCacheResidentBytes, sharedPageGpuBudget(), "read-error");
                mScene->mMeshCacheFailedNodes.insert(nodeId);
                mScene->mMeshCacheError = page.error;
              }
              updateFrameRefreshPolicy();
              update();
            });
    watcher->setFuture(QtConcurrent::run(
        [cache, nodeId, reservation]() { return MeshCache::readNode(cache, nodeId); }));
  }
}

void NativeViewport::evictMeshCacheUntilFits(const qsizetype requiredBytes) {
  const auto isResident = [this](const int nodeId) {
    return std::any_of(mScene->mFullResolutionMeshGpuChunks.cbegin(),
                       mScene->mFullResolutionMeshGpuChunks.cend(),
                       [nodeId](const FullResolutionMeshGpuChunk &chunk) {
                         return chunk.nodeId == nodeId;
                       });
  };
  QSet<int> protectedNodes;
  for (const int desired : std::as_const(mScene->mDesiredMeshCacheNodes)) {
    int nodeId = desired;
    while (nodeId >= 0) {
      if (isResident(nodeId)) {
        protectedNodes.insert(nodeId);
        break;
      }
      nodeId = mScene->mMeshCache.nodes.at(nodeId).parent;
    }
  }
  while (!mScene->mFullResolutionMeshGpuChunks.isEmpty() &&
         mScene->mMeshCacheResidentBytes + requiredBytes >
             sharedPageGpuBudget()) {
    qsizetype victim = -1;
    quint64 oldest = std::numeric_limits<quint64>::max();
    for (qsizetype index = 0;
         index < mScene->mFullResolutionMeshGpuChunks.size(); ++index) {
      const FullResolutionMeshGpuChunk &chunk =
          mScene->mFullResolutionMeshGpuChunks.at(index);
      if (!protectedNodes.contains(chunk.nodeId) &&
          chunk.lastUsedFrame < oldest) {
        victim = index;
        oldest = chunk.lastUsedFrame;
      }
    }
    if (victim < 0) {
      for (qsizetype index = 0;
           index < mScene->mFullResolutionMeshGpuChunks.size(); ++index) {
        const FullResolutionMeshGpuChunk &chunk =
            mScene->mFullResolutionMeshGpuChunks.at(index);
        if (chunk.lastUsedFrame < oldest) {
          victim = index;
          oldest = chunk.lastUsedFrame;
        }
      }
    }
    if (victim < 0) {
      break;
    }
    const FullResolutionMeshGpuChunk chunk =
        mScene->mFullResolutionMeshGpuChunks.takeAt(victim);
    if (chunk.indexBuffer != 0) {
      glDeleteBuffers(1, &chunk.indexBuffer);
    }
    if (chunk.vertexBuffer != 0) {
      glDeleteBuffers(1, &chunk.vertexBuffer);
    }
    if (chunk.vertexArray != 0) {
      glDeleteVertexArrays(1, &chunk.vertexArray);
    }
    mScene->mMeshCacheResidentBytes -= chunk.byteCount;
    updateManagedResourceUsage();
    if (meshPagingQaEnabled())
      logMeshPagingQa(MeshPagingQaEvent::Evict, mScene->mMeshCache, mScene->mDesiredMeshCacheNodes,
                      chunk.nodeId, chunk.byteCount, mScene->mMeshCacheResidentBytes, sharedPageGpuBudget());
  }
}

void NativeViewport::uploadPendingMeshCachePages() {
  if (mScene->mFullResolutionMeshClearPending) {
    releaseFullResolutionMesh();
    mScene->mFullResolutionMeshClearPending = false;
  }
  if (mScene->mPendingMeshCachePages.isEmpty() || mMeshProgram == nullptr ||
      !mMeshProgram->isLinked()) {
    return;
  }
  // Drain small completed pages together, bounded by both work and wall time,
  // rather than leaving the upload queue limited to one page per painted frame.
  QElapsedTimer uploadBudget;
  uploadBudget.start();
  lichtfeld::UploadDrainBudget batch;
  while (!mScene->mPendingMeshCachePages.isEmpty()) {
    const auto &next = mScene->mPendingMeshCachePages.constFirst();
    const auto nextBytes = static_cast<std::uint64_t>(next.vertices.size()) * sizeof(MeshVertex) +
        static_cast<std::uint64_t>(next.indices.size()) * sizeof(quint32);
    if (!batch.permits(nextBytes, static_cast<std::uint64_t>(uploadBudget.nsecsElapsed()))) break;
    MeshCachePage page = mScene->mPendingMeshCachePages.takeFirst();
    batch.recordPage(nextBytes);
    auto ramReservation = mScene->resourceMeshPageReservations.take(page.nodeId);
    const bool alreadyResident = std::any_of(
        mScene->mFullResolutionMeshGpuChunks.cbegin(),
        mScene->mFullResolutionMeshGpuChunks.cend(),
        [&page](const FullResolutionMeshGpuChunk &chunk) {
          return chunk.nodeId == page.nodeId;
        });
    if (!alreadyResident && page.nodeId >= 0 &&
        page.nodeId < mScene->mMeshCache.nodes.size()) {
      const qsizetype vertexBytes =
          page.vertices.size() * static_cast<qsizetype>(sizeof(MeshVertex));
      const qsizetype indexBytes =
          page.indices.size() * static_cast<qsizetype>(sizeof(quint32));
      const qsizetype byteCount = vertexBytes + indexBytes;
      trimHiddenPageCaches(byteCount);
      evictMeshCacheUntilFits(byteCount);
      updateManagedResourceUsage();
      auto gpuReservation = mResources->createReservation();
      if (byteCount > sharedPageGpuBudget() - mScene->mMeshCacheResidentBytes ||
          !gpuReservation->tryResize(0, byteCount)) {
        if (meshPagingQaEnabled())
          logMeshPagingQa(MeshPagingQaEvent::Reject, mScene->mMeshCache, mScene->mDesiredMeshCacheNodes,
                          page.nodeId, byteCount, mScene->mMeshCacheResidentBytes, sharedPageGpuBudget(), "budget");
        mScene->mMeshCacheFailedNodes.insert(page.nodeId);
        continue;
      }
      FullResolutionMeshGpuChunk gpuChunk;
      gpuChunk.nodeId = page.nodeId;
      gpuChunk.indexCount = static_cast<GLsizei>(page.indices.size());
      gpuChunk.byteCount = byteCount;
      gpuChunk.lastUsedFrame = mScene->mMeshCacheFrameSerial;
      const MeshCacheNode &node = mScene->mMeshCache.nodes.at(page.nodeId);
      gpuChunk.boundsMinimum = node.boundsMinimum;
      gpuChunk.boundsMaximum = node.boundsMaximum;
      while (glGetError() != GL_NO_ERROR) {
      }
      glGenVertexArrays(1, &gpuChunk.vertexArray);
      glGenBuffers(1, &gpuChunk.vertexBuffer);
      glGenBuffers(1, &gpuChunk.indexBuffer);
      glBindVertexArray(gpuChunk.vertexArray);
      glBindBuffer(GL_ARRAY_BUFFER, gpuChunk.vertexBuffer);
      glBufferData(GL_ARRAY_BUFFER, vertexBytes, page.vertices.constData(),
                   GL_STATIC_DRAW);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                            nullptr);
      glEnableVertexAttribArray(1);
      glVertexAttribPointer(
          1, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
          reinterpret_cast<const void *>(offsetof(MeshVertex, red)));
      glEnableVertexAttribArray(2);
      glVertexAttribPointer(
          2, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
          reinterpret_cast<const void *>(offsetof(MeshVertex, normalX)));
      glEnableVertexAttribArray(3);
      glVertexAttribPointer(
          3, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
          reinterpret_cast<const void *>(offsetof(MeshVertex, textureU)));
      glEnableVertexAttribArray(4);
      glVertexAttribPointer(
          4, 1, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
          reinterpret_cast<const void *>(offsetof(MeshVertex, textureWeight)));
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpuChunk.indexBuffer);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexBytes, page.indices.constData(),
                   GL_STATIC_DRAW);
      const GLenum uploadError = glGetError();
      if (uploadError == GL_NO_ERROR) {
        mScene->mMeshCacheResidentBytes += byteCount;
        mScene->mFullResolutionMeshGpuChunks.append(gpuChunk);
      } else {
        if (meshPagingQaEnabled())
          logMeshPagingQa(MeshPagingQaEvent::Reject, mScene->mMeshCache, mScene->mDesiredMeshCacheNodes,
                          page.nodeId, byteCount, mScene->mMeshCacheResidentBytes, sharedPageGpuBudget(), "gl-error");
        glDeleteBuffers(1, &gpuChunk.indexBuffer);
        glDeleteBuffers(1, &gpuChunk.vertexBuffer);
        glDeleteVertexArrays(1, &gpuChunk.vertexArray);
        mScene->mMeshCacheFailedNodes.insert(page.nodeId);
        mScene->mMeshCacheError =
            QCoreApplication::translate("Workbench", "GPU memory could not accept mesh-cache node %1.")
                .arg(page.nodeId);
      }
      glBindBuffer(GL_ARRAY_BUFFER, 0);
      glBindVertexArray(0);
      updateManagedResourceUsage();
      if (meshPagingQaEnabled() && uploadError == GL_NO_ERROR)
        logMeshPagingQa(MeshPagingQaEvent::Upload, mScene->mMeshCache, mScene->mDesiredMeshCacheNodes,
                        page.nodeId, byteCount, mScene->mMeshCacheResidentBytes, sharedPageGpuBudget());
    }
  }
  updateManagedResourceUsage(); // include trailing rejected/duplicate/invalid page releases
  mScene->mUploadedFullResolutionMeshTriangleCount = 0;
  for (const FullResolutionMeshGpuChunk &chunk :
       std::as_const(mScene->mFullResolutionMeshGpuChunks)) {
    mScene->mUploadedFullResolutionMeshTriangleCount += chunk.indexCount / 3;
  }
  requestMissingMeshCachePages();
  updateFrameRefreshPolicy();
  update();
}

void NativeViewport::uploadPendingMesh() {
  if (!mScene->mMeshUploadPending || !mScene->mMeshVertexBuffer.isCreated() ||
      !mScene->mMeshIndexBuffer.isCreated()) {
    return;
  }

  QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mScene->mMeshVertexArray);
  while (glGetError() != GL_NO_ERROR) {}
  mScene->mMeshVertexBuffer.bind();
  const qsizetype vertexBytes =
      mScene->mPendingMeshVertices.size() * static_cast<qsizetype>(sizeof(MeshVertex));
  mScene->mMeshVertexBuffer.allocate(
      mScene->mPendingMeshVertices.isEmpty() ? nullptr
                                     : mScene->mPendingMeshVertices.constData(),
      static_cast<int>(vertexBytes));
  const qint64 allocatedVertexBytes = std::max(0, mScene->mMeshVertexBuffer.size());
  mScene->mMeshVertexBuffer.release();

  mScene->mMeshIndexBuffer.bind();
  const qsizetype indexBytes =
      mScene->mPendingMeshIndices.size() * static_cast<qsizetype>(sizeof(quint32));
  mScene->mMeshIndexBuffer.allocate(
      mScene->mPendingMeshIndices.isEmpty() ? nullptr : mScene->mPendingMeshIndices.constData(),
      static_cast<int>(indexBytes));
  mScene->resourceMeshGpuBytes = allocatedVertexBytes +
      std::max(0, mScene->mMeshIndexBuffer.size());
  const GLenum uploadError = glGetError();
  mScene->mMeshIndexBuffer.release();

  if (uploadError != GL_NO_ERROR || mScene->resourceMeshGpuBytes != vertexBytes + indexBytes) {
    mScene->resourceNeedsReload = true;
    mScene->resourceForcePagedRetry = true;
    mScene->resourceResidentUploadFailed = true;
    mScene->resourceRetryAfterMs = mResourceClock.elapsed();
    mScene->mRenderedMeshIndexCount = 0;
    qWarning().noquote() << QCoreApplication::translate(
        "Workbench", "Insufficient memory for the mesh preview and texture.");
  }

  mScene->mPendingMeshVertices = {};
  mScene->mPendingMeshIndices = {};
  mScene->mMeshUploadPending = false;
}

void NativeViewport::synchronizeGaussianRenderingAvailability(
    const bool previousAvailability) {
  const bool available = gaussianRenderingAvailable();
  if (available != previousAvailability) {
    emit gaussianRenderingAvailabilityChanged(available);
  }
  if (!available && mScene->mRenderMode == RenderMode::Gaussians) {
    mScene->mRenderMode = RenderMode::Points;
    rebuildRenderedVertices();
    emit renderModeChanged(mScene->mRenderMode);
  }
}

void NativeViewport::drawPointCloud(const QMatrix4x4 &viewProjection) {
  mInteractionLodActive = false;
  mScene->mProgressiveUploadActive = false;
  if ((mScene->mRenderedPointCount <= 0 &&
       mScene->mFullResolutionPointGpuChunks.isEmpty()) ||
      mPointProgram == nullptr ||
      !mPointProgram->isLinked()) {
    return;
  }

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                      GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  mPointProgram->bind();
  mPointProgram->setUniformValue("viewProjection", viewProjection);
  mPointProgram->setUniformValue("selected", mModelSelected ? 1.0F : 0.0F);
  mPointProgram->setUniformValue(
      "pointSize",
      pointPreviewDiameterPixels(static_cast<float>(devicePixelRatioF())));
  {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mScene->mPointVertexArray);
    if (mScene->mRenderedPointCount > 0) {
      noteRenderDiagnosticDraw("draw-points-base", mScene->mRenderedPointCount);
      glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(mScene->mRenderedPointCount));
    }
  }
  const auto residentChunk = [this](const int nodeId)
      -> FullResolutionGpuChunk * {
    const auto iterator = std::find_if(
        mScene->mFullResolutionPointGpuChunks.begin(),
        mScene->mFullResolutionPointGpuChunks.end(),
        [nodeId](const FullResolutionGpuChunk &chunk) {
          return chunk.nodeId == nodeId;
        });
    return iterator == mScene->mFullResolutionPointGpuChunks.end() ? nullptr
                                                            : &(*iterator);
  };
  QSet<int> drawNodes;
  bool desiredMissing = false;
  for (const int desired : std::as_const(mScene->mDesiredPointCacheNodes)) {
    int nodeId = desired;
    FullResolutionGpuChunk *chunk = residentChunk(nodeId);
    if (chunk == nullptr) {
      desiredMissing = true;
    }
    while (chunk == nullptr && nodeId >= 0 && mScene->mPointCache.isValid()) {
      nodeId = mScene->mPointCache.nodes.at(nodeId).parent;
      chunk = nodeId >= 0 ? residentChunk(nodeId) : nullptr;
    }
    if (chunk != nullptr) {
      drawNodes.insert(chunk->nodeId);
    }
  }
  bool hierarchicalLod = false;
  for (const int nodeId : std::as_const(drawNodes)) {
    FullResolutionGpuChunk *chunk = residentChunk(nodeId);
    if (chunk == nullptr) {
      continue;
    }
    chunk->lastUsedFrame = mScene->mPointCacheFrameSerial;
    hierarchicalLod =
        hierarchicalLod ||
        (mScene->mPointCache.isValid() &&
         mScene->mPointCache.nodes.at(nodeId).depth < PointCloudCacheIndex::OctreeDepth);
    glBindVertexArray(chunk->vertexArray);
    noteRenderDiagnosticDraw("draw-points-chunk", chunk->pointCount);
    glDrawArrays(GL_POINTS, 0, chunk->pointCount);
  }
  mInteractionLodActive = hierarchicalLod;
  mScene->mProgressiveUploadActive =
      desiredMissing || mScene->pointReadsActive() || !mScene->mPointCacheReadsInFlight.isEmpty() ||
      !mScene->mPendingPointCachePages.isEmpty();
  glBindVertexArray(0);
  mPointProgram->release();
  glDisable(GL_BLEND);
}

void NativeViewport::drawMesh(const QMatrix4x4 &viewProjection) {
  mInteractionLodActive = false;
  mScene->mProgressiveUploadActive = false;
  mScene->mDrawnFullResolutionMeshTriangleCount = 0;
  if ((mScene->mRenderedMeshIndexCount < 3 &&
       mScene->mFullResolutionMeshGpuChunks.isEmpty()) ||
      mMeshProgram == nullptr || !mMeshProgram->isLinked()) {
    return;
  }

  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  mMeshProgram->bind();
  mMeshProgram->setUniformValue("viewProjection", viewProjection);
  mMeshProgram->setUniformValue("model", modelMatrix());
  mMeshProgram->setUniformValue("selected", mModelSelected ? 1.0F : 0.0F);
  mMeshProgram->setUniformValue("cameraPosition", cameraPosition());
  const bool textureEnabled =
      mScene->mMeshHasTextureCoordinates && mScene->mMeshTextureReady && mScene->mMeshTexture != 0;
  mMeshProgram->setUniformValue("albedoTexture", 0);
  mMeshProgram->setUniformValue("textureEnabled", textureEnabled);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, textureEnabled ? mScene->mMeshTexture : 0);
  if (mScene->mRenderedMeshIndexCount >= 3 && mScene->mMeshIndexBuffer.isCreated()) {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mScene->mMeshVertexArray);
    mScene->mMeshIndexBuffer.bind();
    noteRenderDiagnosticDraw("draw-mesh-base", mScene->mRenderedMeshIndexCount);
    glDrawElements(GL_TRIANGLES,
                   static_cast<GLsizei>(mScene->mRenderedMeshIndexCount),
                   GL_UNSIGNED_INT, nullptr);
    mScene->mMeshIndexBuffer.release();
  }
  const auto residentChunk = [this](const int nodeId)
      -> FullResolutionMeshGpuChunk * {
    const auto iterator = std::find_if(
        mScene->mFullResolutionMeshGpuChunks.begin(),
        mScene->mFullResolutionMeshGpuChunks.end(),
        [nodeId](const FullResolutionMeshGpuChunk &chunk) {
          return chunk.nodeId == nodeId;
        });
    return iterator == mScene->mFullResolutionMeshGpuChunks.end() ? nullptr
                                                           : &(*iterator);
  };
  QSet<int> drawNodes;
  bool desiredMissing = false;
  for (const int desired : std::as_const(mScene->mDesiredMeshCacheNodes)) {
    int nodeId = desired;
    FullResolutionMeshGpuChunk *chunk = residentChunk(nodeId);
    if (chunk == nullptr) {
      desiredMissing = true;
    }
    while (chunk == nullptr && nodeId >= 0 && pagedMeshAvailable()) {
      nodeId = mScene->mMeshCache.nodes.at(nodeId).parent;
      chunk = nodeId >= 0 ? residentChunk(nodeId) : nullptr;
    }
    if (chunk != nullptr) {
      drawNodes.insert(chunk->nodeId);
    }
  }
  bool hierarchicalLod = false;
  for (const int nodeId : std::as_const(drawNodes)) {
    FullResolutionMeshGpuChunk *chunk = residentChunk(nodeId);
    if (chunk == nullptr) {
      continue;
    }
    chunk->lastUsedFrame = mScene->mMeshCacheFrameSerial;
    hierarchicalLod =
        hierarchicalLod ||
        (pagedMeshAvailable() && !mScene->mMeshCache.nodes.at(nodeId).isLeaf());
    glBindVertexArray(chunk->vertexArray);
    noteRenderDiagnosticDraw("draw-mesh-chunk", chunk->indexCount);
    glDrawElements(GL_TRIANGLES, chunk->indexCount, GL_UNSIGNED_INT, nullptr);
    mScene->mDrawnFullResolutionMeshTriangleCount += chunk->indexCount / 3;
  }
  glBindVertexArray(0);
  mInteractionLodActive = hierarchicalLod;
  mScene->mProgressiveUploadActive =
      desiredMissing || !mScene->mMeshCacheReadsInFlight.isEmpty() ||
      !mScene->mPendingMeshCachePages.isEmpty();
  glBindTexture(GL_TEXTURE_2D, 0);
  mMeshProgram->release();
}

void NativeViewport::drawTrainingPointCloud(
    const QMatrix4x4 &viewProjection) {
  const GLsizei drawCount = mTrainingGpuPreview.pointCount();
  const GLuint vertexArray = mTrainingGpuPreview.activeVertexArray();
  if (drawCount <= 0 || vertexArray == 0 || mPointProgram == nullptr ||
      !mPointProgram->isLinked()) {
    return;
  }

  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                      GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  mPointProgram->bind();
  mPointProgram->setUniformValue("viewProjection", viewProjection);
  mPointProgram->setUniformValue("selected", mModelSelected ? 1.0F : 0.0F);
  mPointProgram->setUniformValue(
      "pointSize",
      pointPreviewDiameterPixels(static_cast<float>(devicePixelRatioF())));
  glBindVertexArray(vertexArray);
  // The shared VAO advances its attributes per Gaussian instance for the
  // splat renderer. Point rendering consumes the same GPU memory per vertex,
  // so temporarily switch position/color back to a per-vertex rate.
  glVertexAttribDivisor(0, 0);
  glVertexAttribDivisor(1, 0);
  noteRenderDiagnosticDraw("draw-training-points", drawCount);
  glDrawArrays(GL_POINTS, 0, drawCount);
  glVertexAttribDivisor(0, 1);
  glVertexAttribDivisor(1, 1);
  glBindVertexArray(0);
  mPointProgram->release();
  glDisable(GL_BLEND);
}

int NativeViewport::effectiveShDegree() const {
  if (trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame()) return -1;
  const int available = mScene->gaussianGpu.shDegree();
  return mMaximumShDegree < 0 ? available : std::min(available, mMaximumShDegree);
}

void NativeViewport::setMaximumShDegree(int degree) {
  mMaximumShDegree = std::clamp(degree, -1, 4);
  update(); // uniforms only: no worker reset, vertex rebuild, or SH upload
}

void NativeViewport::drawGaussianCloud(const QMatrix4x4 &view,
                                       const QMatrix4x4 &projection) {
  // File-based live previews use the same validated per-snapshot attributes as
  // saved scenes. Only an actual IPC frame bypasses that file-owned contract.
  const bool ipcPreview = (trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame());
  const qsizetype drawCount =
      ipcPreview ? mTrainingGpuPreview.pointCount() : mScene->mRenderedPointCount;
  if (drawCount <= 0 || mGaussianProgram == nullptr ||
      !mGaussianProgram->isLinked()) {
    return;
  }

  const qreal ratio = devicePixelRatioF();
  // Keep sorted Gaussian blending, but respect depth already written by the
  // physical reference axes and opaque models. Gaussian fragments still do
  // not write depth, so their back-to-front compositing remains unchanged.
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  mGaussianProgram->bind();
  const bool indexed = !ipcPreview && mScene->indexedGaussians;
  mGaussianProgram->setUniformValue("indexedAttributes", indexed);
  mGaussianProgram->setUniformValue("surfelRendering", !ipcPreview && mScene->mHasSurfelAttributes);
  mGaussianProgram->setUniformValue("fullMipFilter",
      mScene->mQualityMip.appliesToRasterFrame(ipcPreview, mScene->mHasSurfelAttributes));
  mGaussianProgram->setUniformValue("gaussianAttributes", 2);
  mGaussianProgram->setUniformValue("gaussianOrder", 3);
  mGaussianProgram->setUniformValue("gaussianSh", 4);
  mGaussianProgram->setUniformValue("shDegree", effectiveShDegree());
  mGaussianProgram->setUniformValue("shCoefficientCount", mScene->gaussianGpu.shCoefficientCount());
  const QMatrix4x4 inverseView = view.inverted();
  mGaussianProgram->setUniformValue("shCameraPosition", inverseView.map(QVector3D()));
  mGaussianProgram->setUniformValue("shParallelDirection", inverseView.mapVector(QVector3D(0, 0, -1)).normalized());
  mGaussianProgram->setUniformValue("shOrthographic", mOrthographic);
  mGaussianProgram->setUniformValue("view", view);
  mGaussianProgram->setUniformValue("selected",
                                    mModelSelected ? 1.0F : 0.0F);
  mGaussianProgram->setUniformValue("projection", projection);
  mGaussianProgram->setUniformValue(
      "viewportPixels", QVector2D(static_cast<float>(width() * ratio),
                                  static_cast<float>(height() * ratio)));
  if (ipcPreview) {
    glBindVertexArray(mTrainingGpuPreview.activeVertexArray());
    noteRenderDiagnosticDraw("draw-ipc-gaussians", drawCount);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(drawCount));
    glBindVertexArray(0);
  } else if (indexed) {
    mScene->gaussianGpu.bind();
    noteRenderDiagnosticDraw("draw-indexed-gaussians", drawCount);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(drawCount));
    mScene->gaussianGpu.unbind();
  } else {
    mScene->gaussianGpu.bindTextures();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mScene->mGaussianVertexArray);
    noteRenderDiagnosticDraw("draw-compatibility-gaussians", drawCount);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(drawCount));
    mScene->gaussianGpu.unbindTextures();
  }
  mGaussianProgram->release();
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

void NativeViewport::drawInfiniteGrid(const QMatrix4x4 &viewProjection) {
  if (!mViewportGridSettings.gridVisible && !mViewportGridSettings.axesVisible) return;
  if (!mGridShaderReady || mGridProgram == nullptr ||
      !mGridProgram->isLinked() || !mGridVertexArray.isCreated()) {
    return;
  }

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glEnable(GL_MULTISAMPLE);
  // These shaders emit straight RGB, but the QOpenGLWidget framebuffer is
  // presented/captured as premultiplied RGBA. Keep the opaque backdrop alpha
  // opaque: applying SRC_ALPHA to alpha as well would produce 1-a+a*a and
  // make Qt unpremultiply bright day-grid pixels into saturated colours.
  // RGB blending is unchanged; only the alpha-channel source factor differs.
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                      GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glLineWidth(1.0F);
  mGridProgram->bind();
  const auto referenceMapping=sceneDisplayTransform(coordinateReferenceScene(),*mScene);
  const auto referenceOffset=referenceMapping.inverted().mapVector(orbitFrame(viewOrbitAngles()).cameraOffsetDirection).normalized();
  const ReferenceGridPlane plane = referenceGridPlane({std::atan2(referenceOffset.x(),referenceOffset.y())*180.F/kPi,
      std::asin(std::clamp(referenceOffset.z(),-1.F,1.F))*180.F/kPi},mOrthographic);
  auto grid = referenceGridFrame(plane);
  grid.axisU=referenceMapping.mapVector(grid.axisU).normalized();
  grid.axisV=referenceMapping.mapVector(grid.axisV).normalized();
  grid.normal=referenceMapping.mapVector(grid.normal).normalized();
  const QVector3D gridPlaneOrigin = gridOrigin(plane);
  const QVector3D relativeCamera = cameraPosition() - gridPlaneOrigin;
  Q_UNUSED(viewProjection);
  bool invertible = false;
  const QMatrix4x4 projection = projectionMatrix();
  const QMatrix4x4 inverseProjection = projection.inverted(&invertible);
  if (!invertible) {
    mGridProgram->release();
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    return;
  }
  const QMatrix4x4 inverseView = viewMatrix().inverted();
  QMatrix3x3 inverseRotation;
  QMatrix3x3 planeBasis;
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column)
      inverseRotation(row, column) = inverseView(row, column);
    planeBasis(row, 0) = grid.axisU[row];
    planeBasis(row, 1) = grid.axisV[row];
    planeBasis(row, 2) = grid.normal[row];
  }
  const QVector3D cameraInPlane(
      QVector3D::dotProduct(relativeCamera, grid.axisU),
      QVector3D::dotProduct(relativeCamera, grid.axisV),
      QVector3D::dotProduct(relativeCamera, grid.normal));
  const auto configuredSteps = displayGridSteps(mViewportGridSettings);
  std::array<GLfloat, blender_grid::StepCount> steps;
  if (configuredSteps) steps = *configuredSteps;
  else steps.fill(1.0F); // Axes remain safe; an invalid periodic grid is not drawn.
  std::array<QVector2D, blender_grid::StepCount> phases;
  for (int level = 0; level < blender_grid::StepCount; ++level) {
    phases[level] = QVector2D(
        static_cast<float>(std::remainder(static_cast<double>(cameraInPlane.x()), static_cast<double>(steps[level]))),
        static_cast<float>(std::remainder(static_cast<double>(cameraInPlane.y()), static_cast<double>(steps[level]))));
  }
  mGridProgram->setUniformValue("inverseProjection", inverseProjection);
  mGridProgram->setUniformValue("inverseViewRotation", inverseRotation);
  mGridProgram->setUniformValue("planeBasis", planeBasis);
  mGridProgram->setUniformValue("cameraPlanePosition", cameraInPlane);
  mGridProgram->setUniformValueArray("gridSteps", steps.data(), blender_grid::StepCount, 1);
  mGridProgram->setUniformValueArray("gridPhase", phases.data(), blender_grid::StepCount);
  mGridProgram->setUniformValue("gridDistance", static_cast<float>(std::min(
      static_cast<double>(mDistance) * 8.0, static_cast<double>(std::numeric_limits<float>::max()) / 16.0)));
  mGridProgram->setUniformValue("gridLineSize", static_cast<float>(std::max(devicePixelRatioF() - 1.0, 0.0) * 0.5));
  mGridProgram->setUniformValue("isOrthographic", mOrthographic ? 1 : 0);
  mGridProgram->setUniformValue("showGrid", mViewportGridSettings.gridVisible && configuredSteps ? 1 : 0);
  mGridProgram->setUniformValue("showAxes", mViewportGridSettings.axesVisible ? 1 : 0);
  mGridProgram->setUniformValue("fixedGrid", mViewportGridSettings.spacingMode == ViewportGridSettings::SpacingMode::Fixed ? 1 : 0);
  mGridProgram->setUniformValue("gridDensity", static_cast<float>(mViewportGridSettings.density));
  mGridProgram->setUniformValue("gridSubdivisions", mViewportGridSettings.subdivisions);
  mGridProgram->setUniformValue("gridColor", lightViewport()
      ? QVector4D(0.33F, 0.33F, 0.33F, 0.18F)
      : QVector4D(0.50F, 0.50F, 0.50F, 0.28F));
  mGridProgram->setUniformValue("gridEmphasisColor", lightViewport()
      ? QVector4D(0.26F, 0.26F, 0.26F, 0.28F)
      : QVector4D(0.62F, 0.62F, 0.62F, 0.45F));
  mGridProgram->setUniformValue("axisUColor", QVector4D(grid.axisUColor, 0.72F));
  mGridProgram->setUniformValue("axisVColor", QVector4D(grid.axisVColor, 0.72F));
  QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mGridVertexArray);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  mGridProgram->release();
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

void NativeViewport::enterEvent(QEnterEvent *event) {
  mPointerPosition = event->position();
  if (mMode == InteractionMode::Brush && !mTemporaryOrbitActive) {
    mBrushCursorPosition = event->position();
    mBrushCursorVisible = true;
    update();
  }
  QOpenGLWidget::enterEvent(event);
}

void NativeViewport::leaveEvent(QEvent *event) {
  if (mBrushCursorVisible) {
    mBrushCursorVisible = false;
    update();
  }
  if (!mNavigationInteractionActive &&
      mNavigationHover.part != NavigationGizmoPart::None) {
    mNavigationHover = {};
    setToolTip({});
    setCursor(defaultInteractionCursor(mMode));
    update();
  }
  if (!mModelDragActive &&
      (mTransformGizmoHover.isValid() || mTransformToolHover >= 0)) {
    mTransformGizmoHover = {};
    mTransformToolHover = -1;
    setToolTip({});
    setCursor(defaultInteractionCursor(mMode));
    update();
  }
  QOpenGLWidget::leaveEvent(event);
}

void NativeViewport::mouseDoubleClickEvent(QMouseEvent *event) {
  if (mEditToolsLocked && event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
    mObservationDragActive = false;
    mPressedButtons = Qt::NoButton;
    const auto navigationHit = hitTestNavigationGizmo(navigationGizmo(), event->position());
    if (navigationHit.part == NavigationGizmoPart::None && !mNavigationInteractionActive)
      (void)centerObservationAt(event->position());
    event->accept();
    return;
  }
  QOpenGLWidget::mouseDoubleClickEvent(event);
}

void NativeViewport::mousePressEvent(QMouseEvent *event) {
  mPointerPosition = event->position();
  if (mModelDragActive) {
    if (event->button() == Qt::LeftButton) {
      updateModelTransform(event->position(), event->modifiers());
      finishModelTransform(true);
      event->accept();
      return;
    }
    if (event->button() == Qt::RightButton) {
      finishModelTransform(false);
      event->accept();
      return;
    }
    if (event->button() == Qt::MiddleButton && !mTrackballRotation) {
      const QVector3D pivot =
          mScene->mSceneCenter + mModelDragStartTransform.translation;
      const auto center = projectPoint(pivot, viewProjectionMatrix());
      int closestAxis = -1;
      qreal closestDistance = std::numeric_limits<qreal>::max();
      if (center.has_value()) {
        for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
          QVector3D axis;
          axis[axisIndex] = 1.0F;
          const auto endpoint = projectPoint(
              pivot + axis * std::max(mScene->mSceneRadius, 1.0F),
              viewProjectionMatrix());
          if (!endpoint.has_value()) {
            continue;
          }
          const QPointF direction = *endpoint - *center;
          const qreal lengthSquared = direction.x() * direction.x() +
                                      direction.y() * direction.y();
          if (lengthSquared <= 1.0e-8) {
            continue;
          }
          const QPointF offset = event->position() - *center;
          const qreal projection =
              (offset.x() * direction.x() + offset.y() * direction.y()) /
              lengthSquared;
          const QPointF nearest = *center + direction * projection;
          const qreal distance = QLineF(nearest, event->position()).length();
          if (distance < closestDistance) {
            closestDistance = distance;
            closestAxis = axisIndex;
          }
        }
      }
      if (closestAxis >= 0) {
        applyTransformConstraint(
            closestAxis, event->modifiers().testFlag(Qt::ShiftModifier));
      }
      event->accept();
      return;
    }
  }
  if (!mEditToolsLocked && event->button() == Qt::LeftButton && selectableModelAvailable()) {
    const int tool = hitTestTransformToolStrip(transformToolStrip(),
                                               event->position());
    if (tool >= 0) {
      activateTransformToolAt(tool);
      event->accept();
      return;
    }
    if (mModelSelected && !event->modifiers().testFlag(Qt::ControlModifier) &&
        !event->modifiers().testFlag(Qt::ShiftModifier)) {
      const TransformGizmoHandle handle = hitTestTransformGizmo(
          modelTransformGizmo(), event->position(), mTransformGizmoMode);
      if (handle.isValid()) {
        beginTransformGizmoDrag(handle, event->position(), event->modifiers());
        event->accept();
        return;
      }
    }
  }
  updateNavigationGizmoHover(event->position());
  if (isTrimInteractionMode(mMode) &&
      isTemporaryOrbitShortcut(event->button(), event->modifiers())) {
    mTemporaryOrbitActive = true;
    mSelectionGestureActive = false;
    mSelectionPath.clear();
    mBrushCursorVisible = false;
    mPressedButtons = event->buttons();
    mLastMousePosition = event->position().toPoint();
    setCursor(Qt::ClosedHandCursor);
    setToolTip(QCoreApplication::translate("Workbench", "Ctrl + 左键：旋转视角"));
    update();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton) {
    mTemporaryOrbitActive = false;
  }
  if (event->button() == Qt::LeftButton &&
      mNavigationHover.part != NavigationGizmoPart::None) {
    mNavigationInteractionActive = true;
    mNavigationDragging = false;
    mNavigationPress = mNavigationHover;
    mNavigationPressPosition = event->position().toPoint();
    mLastMousePosition = mNavigationPressPosition;
    mPressedButtons = Qt::NoButton;
    if (mNavigationPress.part == NavigationGizmoPart::Zoom) {
      setCursor(Qt::SizeVerCursor);
    } else {
      setCursor(Qt::ClosedHandCursor);
    }
    update();
    event->accept();
    return;
  }

  if (!mEditToolsLocked && event->button() == Qt::LeftButton &&
      mMode == InteractionMode::Inspect &&
      (selectableModelAvailable() || mSceneStates.size() > 1)) {
    const QString id = mSceneStates.size() > 1 ? sceneObjectAt(event->position()) :
        modelHitAt(event->position()) ? mScene->id : QString();
    const bool toggle = event->modifiers().testFlag(Qt::ControlModifier);
    const bool extend = event->modifiers().testFlag(Qt::ShiftModifier);
    if (!id.isEmpty() || (mSceneStates.size() == 1 && modelHitAt(event->position()))) {
      QStringList ids = (toggle || extend) ? selectedSceneIds() : QStringList();
      if (toggle && ids.contains(id)) ids.removeAll(id);
      else if (!ids.contains(id)) ids.append(id);
      setSceneSelection(ids, id);
      event->accept();
      return;
    }
    if (!toggle && !extend && mModelSelected) clearSelection();
  }

  mPressedButtons = event->buttons();
  mLastMousePosition = event->position().toPoint();
  if (mEditToolsLocked && mMode == InteractionMode::Inspect &&
      event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
    beginObservationRotation(event->position(), true);
  }
  if (mMode == InteractionMode::Brush && !mTemporaryOrbitActive) {
    mBrushCursorPosition = event->position();
    mBrushCursorVisible = true;
  }
  if (event->button() == Qt::LeftButton && !mScene->mSelectionBusy &&
      (mMode == InteractionMode::Rectangle || mMode == InteractionMode::Lasso ||
       mMode == InteractionMode::Brush) &&
      hasEditableScene()) {
    mSelectionGestureActive = true;
    mSelectionStart = event->position();
    mSelectionCurrent = event->position();
    mSelectionPath.clear();
    if (mMode == InteractionMode::Lasso || mMode == InteractionMode::Brush) {
      mSelectionPath.append(event->position());
    }
    update();
  }
  event->accept();
}

void NativeViewport::mouseMoveEvent(QMouseEvent *event) {
  mPointerPosition = event->position();
  if (mModelDragActive) {
    updateModelTransform(event->position(), event->modifiers());
    event->accept();
    return;
  }
  if (mNavigationInteractionActive) {
    updateNavigationGizmoInteraction(event->position().toPoint());
    event->accept();
    return;
  }

  if (mMode == InteractionMode::Brush && !mTemporaryOrbitActive) {
    mBrushCursorPosition = event->position();
    mBrushCursorVisible = true;
  }
  if (mSelectionGestureActive) {
    mSelectionCurrent = event->position();
    if ((mMode == InteractionMode::Lasso || mMode == InteractionMode::Brush) &&
        (mSelectionPath.isEmpty() ||
         QLineF(mSelectionPath.last(), event->position()).length() >= 2.0)) {
      mSelectionPath.append(event->position());
    }
    update();
    event->accept();
    return;
  }
  if (mPressedButtons == Qt::NoButton) {
    updateTransformGizmoHover(event->position());
    if (mTransformToolHover >= 0 || mTransformGizmoHover.isValid()) {
      if (mNavigationHover.part != NavigationGizmoPart::None) {
        mNavigationHover = {};
        update();
      }
    } else {
      updateNavigationGizmoHover(event->position());
      updateObservationNavigationToolTip();
    }
    if (mMode == InteractionMode::Brush) {
      update();
      event->accept();
      return;
    }
    QOpenGLWidget::mouseMoveEvent(event);
    return;
  }

  const QPoint current = event->position().toPoint();
  const QPoint delta = current - mLastMousePosition;
  mLastMousePosition = current;
  if (!delta.isNull()) {
    mCameraManipulated = true;
    leaveCameraView();
  }

  if (mEditToolsLocked && mMode == InteractionMode::Inspect) {
    const bool left = mPressedButtons.testFlag(Qt::LeftButton);
    const bool pan = mPressedButtons.testFlag(Qt::MiddleButton) ||
                     mPressedButtons.testFlag(Qt::RightButton) ||
                     (left && event->modifiers().testFlag(Qt::ControlModifier));
    const bool zoom = left && event->modifiers().testFlag(Qt::ShiftModifier);
    mViewSnapTimer->stop();
    if (pan || zoom) {
      // Modifiers take priority even during a drag. Discard the rotation
      // anchor so resuming orbit cannot jump back to the pre-pan camera.
      mObservationDragActive = false;
      if (pan) panCamera(delta);
      else mDistance = clampViewportDistance(
          mDistance * std::pow(1.008F, static_cast<float>(delta.y())), transformedSceneRadius());
    } else if (left) {
      // Only a fresh press may capture an axis. Crossing an arc or releasing
      // Ctrl/Shift mid-drag must not unexpectedly constrain free navigation.
      if (!mObservationDragActive)
        beginObservationRotation(event->position() - QPointF(delta), false);
      updateObservationRotation(event->position());
    }
    update();
    event->accept();
    return;
  }

  const bool pan =
      !mTemporaryOrbitActive &&
      (mPressedButtons.testFlag(Qt::MiddleButton) ||
       mPressedButtons.testFlag(Qt::RightButton) ||
       (mEditToolsLocked ? event->modifiers().testFlag(Qt::ControlModifier)
                         : event->modifiers().testFlag(Qt::ShiftModifier)));
  if (pan) {
    panCamera(delta);
  } else if (mPressedButtons.testFlag(Qt::LeftButton)) {
    orbitCamera(delta);
  }

  update();
  event->accept();
}

void NativeViewport::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && mObservationDragActive) {
    if (!event->modifiers().testFlag(Qt::ControlModifier) &&
        !event->modifiers().testFlag(Qt::ShiftModifier) &&
        !event->buttons().testFlag(Qt::MiddleButton) &&
        !event->buttons().testFlag(Qt::RightButton))
      updateObservationRotation(event->position());
    mObservationDragActive = false;
    update();
  }
  if (event->button() == Qt::LeftButton && mTransformGizmoDragActive) {
    updateModelTransform(event->position(), event->modifiers());
    finishModelTransform(true);
    updateTransformGizmoHover(event->position());
    mPressedButtons = event->buttons();
    event->accept();
    return;
  }
  if (event->button() == Qt::LeftButton && mNavigationInteractionActive) {
    finishNavigationGizmoInteraction();
    mPressedButtons = event->buttons();
    event->accept();
    return;
  }

  if (event->button() == Qt::LeftButton && mSelectionGestureActive) {
    mSelectionCurrent = event->position();
    if ((mMode == InteractionMode::Lasso || mMode == InteractionMode::Brush) &&
        (mSelectionPath.isEmpty() ||
         mSelectionPath.last() != event->position())) {
      mSelectionPath.append(event->position());
    }
    finishSelectionGesture(event->modifiers());
  }
  const bool finishedTemporaryOrbit =
      event->button() == Qt::LeftButton && mTemporaryOrbitActive;
  if (finishedTemporaryOrbit) {
    mTemporaryOrbitActive = false;
  }
  mPressedButtons = event->buttons();
  if (mCameraManipulated && mPressedButtons == Qt::NoButton) {
    mCameraManipulated = false;
    update(); // direction changes are sorted once, immediately before drawing
  }
  if (finishedTemporaryOrbit) {
    if (mMode == InteractionMode::Brush) {
      mBrushCursorPosition = event->position();
      mBrushCursorVisible = true;
    }
    setCursor(defaultInteractionCursor(mMode));
    setToolTip({});
    mNavigationHover = {};
    updateNavigationGizmoHover(event->position());
    update();
  }
  event->accept();
}

void NativeViewport::wheelEvent(QWheelEvent *event) {
  const float steps = event->angleDelta().y() != 0
      ? static_cast<float>(event->angleDelta().y()) / 120.0F
      : static_cast<float>(event->pixelDelta().y()) / 120.0F;
  zoomCamera(std::pow(0.84F, steps), event->position());
  event->accept();
}

void NativeViewport::zoomCamera(const float factor, const QPointF &cursor) {
  if (!std::isfinite(factor) || factor <= 0 ||
      !std::isfinite(cursor.x()) || !std::isfinite(cursor.y())) return;
  leaveCameraView();
  mViewSnapTimer->stop();
  const auto beforeView = viewMatrix();
  const auto beforeProjection = projectionMatrix();
  mDistance = clampViewportDistance(mDistance * factor, transformedSceneRadius());
  if (mZoomToCursorEnabled) {
    if (const auto target = blenderViewZoomTarget(mTarget, beforeView, beforeProjection,
            viewMatrix(), projectionMatrix(), size(), cursor)) mTarget = *target;
  }
  update();
}

NavigationGizmoLayout NativeViewport::navigationGizmo() const {
  return navigationGizmoLayout(
      viewMatrix(),
      QSizeF(static_cast<qreal>(width()), static_cast<qreal>(height())),
      QFontMetricsF(font()).height());
}

void NativeViewport::updateNavigationGizmoHover(const QPointF &position) {
  const NavigationGizmoHit hit =
      hitTestNavigationGizmo(navigationGizmo(), position);
  const bool hoverChanged = hit != mNavigationHover;
  if (!hoverChanged && hit.part == NavigationGizmoPart::None) {
    return;
  }

  mNavigationHover = hit;
  const auto navigationCursor = [&](const Qt::CursorShape cursor) {
    if (!mNavigationInteractionActive && !mObservationDragActive) setCursor(cursor);
  };
  QString tooltip;
  switch (hit.part) {
  case NavigationGizmoPart::Rotate:
    tooltip = hit.axis == NavigationAxis::None
                  ? QCoreApplication::translate("Workbench", "拖动环绕视图；从轴向视图旋转时自动切换到透视")
                  : QCoreApplication::translate("Workbench", "单击 %1 轴进入正交视图；拖动旋转，离开轴向时恢复透视")
                        .arg(navigationAxisLabel(hit.axis));
    navigationCursor(Qt::OpenHandCursor);
    break;
  case NavigationGizmoPart::Zoom:
    {
      const ViewportZoomLimits limits =
          viewportZoomLimits(transformedSceneRadius());
      tooltip = QCoreApplication::translate("Workbench", "上下拖动缩放视图（%1 – %2）")
                    .arg(formatViewportDistance(limits.minimumDistance),
                         formatViewportDistance(limits.maximumDistance));
    }
    navigationCursor(Qt::SizeVerCursor);
    break;
  case NavigationGizmoPart::Pan:
    tooltip = QCoreApplication::translate("Workbench", "拖动平移视图");
    navigationCursor(Qt::OpenHandCursor);
    break;
  case NavigationGizmoPart::Camera:
    tooltip = camerasAvailable()
                  ? (mCameraViewActive ? QCoreApplication::translate("Workbench", "返回用户视图")
                                       : QCoreApplication::translate("Workbench", "切换到场景相机视图"))
                  : QCoreApplication::translate("Workbench", "当前场景没有可用相机");
    navigationCursor(camerasAvailable() ? Qt::PointingHandCursor
                                        : Qt::ForbiddenCursor);
    break;
  case NavigationGizmoPart::Projection:
    tooltip =
        mOrthographic
            ? QCoreApplication::translate("Workbench", "切换到透视投影")
            : QCoreApplication::translate("Workbench", "切换到正交投影");
    navigationCursor(Qt::PointingHandCursor);
    break;
  case NavigationGizmoPart::None:
    navigationCursor(defaultInteractionCursor(mMode));
    break;
  }
  setToolTip(tooltip);
  if (hoverChanged) update();
}

void NativeViewport::updateNavigationGizmoInteraction(const QPoint &current) {
  const QPoint totalDelta = current - mNavigationPressPosition;
  if (!mNavigationDragging &&
      totalDelta.manhattanLength() < QApplication::startDragDistance()) {
    return;
  }
  if (!mNavigationDragging) {
    mNavigationDragging = true;
    mViewSnapTimer->stop();
    if (mNavigationPress.part == NavigationGizmoPart::Rotate ||
        mNavigationPress.part == NavigationGizmoPart::Zoom ||
        mNavigationPress.part == NavigationGizmoPart::Pan) {
      leaveCameraView();
    }
  }

  const QPoint delta = current - mLastMousePosition;
  mLastMousePosition = current;
  if (delta.isNull()) {
    return;
  }

  switch (mNavigationPress.part) {
  case NavigationGizmoPart::Rotate: {
    orbitCamera(delta);
    break;
  }
  case NavigationGizmoPart::Zoom:
    mDistance = clampViewportDistance(
        mDistance * std::pow(1.008F, static_cast<float>(delta.y())),
        transformedSceneRadius());
    break;
  case NavigationGizmoPart::Pan:
    panCamera(delta);
    break;
  case NavigationGizmoPart::Camera:
  case NavigationGizmoPart::Projection:
  case NavigationGizmoPart::None:
    break;
  }
  update();
}

void NativeViewport::finishNavigationGizmoInteraction() {
  const NavigationGizmoHit pressed = mNavigationPress;
  const bool dragged = mNavigationDragging;
  mNavigationInteractionActive = false;
  mNavigationDragging = false;
  mNavigationPress = {};

  if (!dragged) {
    if (pressed.part == NavigationGizmoPart::Rotate) {
      setAxisView(pressed.axis);
    } else if (pressed.part == NavigationGizmoPart::Camera) {
      toggleCameraView();
    } else if (pressed.part == NavigationGizmoPart::Projection) {
      leaveCameraView();
      mOrthographic = !mOrthographic;
      update();
    }
  }

  updateNavigationGizmoHover(QPointF(mLastMousePosition));
}

void NativeViewport::setAxisView(const NavigationAxis axis) {
  mObservationDragActive = false;
  const std::optional<OrbitAngles> target = navigationAxisViewAngles(axis);
  if (!target.has_value()) {
    return;
  }

  mViewSnapTimer->stop();
  leaveCameraView();
  // Adapted axis_set_view (Blender v4.5.0 view3d_navigate.cc): with Auto
  // Perspective enabled, a named axis view uses orthographic projection.
  mOrthographic = true;
  mAxisView = axis;
  const float targetYaw =
      shortestEquivalentAngle(mYawDegrees, target->yawDegrees);
  mSnapStartAngles = viewOrbitAngles();
  mSnapTargetAngles = {targetYaw, target->pitchDegrees, 0.0F};
  mViewSnapClock.start();
  mViewSnapTimer->start();
}

bool NativeViewport::prepareCameraOrbit() {
  const bool before = mOrthographic;
  mOrthographic = blenderViewOrbitOrthographic(
      mOrthographic, mAxisView != NavigationAxis::None);
  mAxisView = NavigationAxis::None;
  return before != mOrthographic;
}

void NativeViewport::orbitCamera(const QPoint &delta) {
  if (delta.isNull()) return;
  mViewSnapTimer->stop();
  (void)prepareCameraOrbit();
  // Editing and the navigation gizmo must follow the current screen axes,
  // just like locked observation. World yaw/pitch reverses drags after a
  // rolled/upside-down view and collapses horizontal orbit at the poles.
  const OrbitAngles angles =
      orbitAnglesAfterScreenDrag(viewOrbitAngles(), QPointF(delta));
  mYawDegrees = angles.yawDegrees;
  mPitchDegrees = angles.pitchDegrees;
  mRollDegrees = angles.rollDegrees;
}

void NativeViewport::panCamera(const QPoint &delta) {
  mViewSnapTimer->stop();
  if (const auto target = blenderViewPanTarget(mTarget, viewMatrix(), projectionMatrix(),
          size(), QPointF(delta))) mTarget = *target;
}

void NativeViewport::toggleCameraView() {
  if (mCameraViewActive && mStoredCameraView.has_value()) {
    mTarget = mStoredCameraView->target;
    mYawDegrees = mStoredCameraView->yawDegrees;
    mPitchDegrees = mStoredCameraView->pitchDegrees;
    mRollDegrees = mStoredCameraView->rollDegrees;
    mPerspectiveFovDegrees = mStoredCameraView->fovDegrees;
    mFocalAspectCorrection = mStoredCameraView->focalAspectCorrection;
    mDistance =
        clampViewportDistance(mStoredCameraView->distance,
                              transformedSceneRadius());
    mOrthographic = mStoredCameraView->orthographic;
    mAxisView = mStoredCameraView->axisView;
    mCameraViewActive = false;
    mStoredCameraView.reset();
    update();
    return;
  }
  (void)setSourceCameraView(0);
}

bool NativeViewport::setSourceCameraView(const qsizetype index) {
  if (!camerasAvailable() || index < 0 || index >= cameraCount()) return false;
  const CameraPose &camera = mScene->mCameraTrajectory.cameras().at(index);
  mSourceCameraIntrinsics.reset();
  mSourceCameraClipping.reset();
  const auto clipping=mScene->mCameraTrajectory.clipping();
  const double sourceNear=std::max(clipping.value(QStringLiteral("znear")).toDouble(),
      clipping.value(QStringLiteral("backend_near")).toDouble());
  const double sourceFar=clipping.value(QStringLiteral("zfar")).toDouble();
  if(std::isfinite(sourceNear)&&std::isfinite(sourceFar)&&sourceNear>0.&&sourceFar>sourceNear) {
    const double localScale=mScene->mSceneCoordinates.displayScale*modelMatrix().mapVector(camera.forward).length();
    mSourceCameraClipping=std::array<double,2>{sourceNear*localScale,sourceFar*localScale};
  }
  if(camera.intrinsics.size()==3 && camera.width>0 && camera.height>0) {
    const auto row0=camera.intrinsics[0].toArray(),row1=camera.intrinsics[1].toArray();
    if(row0.size()==3 && row1.size()==3) {
      const double offset=camera.pixelCoordinates==QStringLiteral("opencv_integer")?.5:0.;
      mSourceCameraIntrinsics=std::array<double,7>{row0[0].toDouble(),row1[1].toDouble(),
          row0[2].toDouble()+offset,row1[2].toDouble()+offset,row0[1].toDouble(),double(camera.width),double(camera.height)};
    }
  }
  const auto mapping = modelMatrix();
  const QVector3D forward = mapping.mapVector(camera.forward).normalized();
  QVector3D up = mapping.mapVector(-camera.imageDown);
  up = (up - forward * QVector3D::dotProduct(up, forward)).normalized();
  if (forward.lengthSquared() < 1.0e-6F) {
    return false;
  }
  if (up.lengthSquared() < 1.0e-6F) return false;
  if (!mCameraViewActive)
    mStoredCameraView = StoredCameraView{mTarget, mYawDegrees, mPitchDegrees,
        mDistance, mOrthographic, mRollDegrees, mPerspectiveFovDegrees,
        mFocalAspectCorrection, mAxisView};
  mViewSnapTimer->stop();
  mRollDegrees = 0.0F;
  const QVector3D cameraOffset = -forward;
  mPitchDegrees =
      std::asin(std::clamp(cameraOffset.z(), -1.0F, 1.0F)) * 180.0F / kPi;
  mYawDegrees = std::atan2(cameraOffset.x(), cameraOffset.y()) * 180.0F / kPi;
  const QVector3D baseUp = orbitFrame(viewOrbitAngles()).upDirection;
  mRollDegrees = std::atan2(QVector3D::dotProduct(cameraOffset,
      QVector3D::crossProduct(baseUp, up)), QVector3D::dotProduct(baseUp, up)) * 180.0F / kPi;
  mTarget = mapping.map(mScene->mSceneCoordinates.localFromGlobal(camera.globalPosition)) + forward * mDistance;
  mPerspectiveFovDegrees = camera.verticalFovDegrees;
  mFocalAspectCorrection = camera.focalAspectCorrection;
  mOrthographic = false;
  mAxisView = NavigationAxis::None;
  mCameraViewActive = true;
  update();
  return true;
}

void NativeViewport::leaveCameraView() {
  if (!mCameraViewActive) {
    return;
  }
  mCameraViewActive = false;
  mStoredCameraView.reset();
}

QVector3D NativeViewport::cameraPosition() const {
  const OrbitFrame frame = orbitFrame(viewOrbitAngles());
  return mTarget + frame.cameraOffsetDirection * mDistance;
}

void NativeViewport::keyPressEvent(QKeyEvent *event) {
  if (mRenderDiagnostics && event->key() == Qt::Key_F12 &&
      event->modifiers() == Qt::NoModifier && !event->isAutoRepeat()) {
    mRenderDiagnosticActiveId = mScene->id;
    // request() must save desktop pixels before any GL access; update only
    // after that request has finished capturing the existing frame.
    if (mRenderDiagnostics->request(this, mRenderedFrameSerial,
                                    mPresentedFrameSerial, renderDiagnosticMetadata()))
      update();
    event->accept();
    return;
  }
  if (mEditToolsLocked) {
    // Do not let edit shortcuts change mode, selection, or transform state.
    // Unhandled keys still reach window-level navigation and lock actions.
    QOpenGLWidget::keyPressEvent(event);
    return;
  }
  if (!mModelDragActive) {
    if (event->key() == Qt::Key_A && event->modifiers().testFlag(Qt::ControlModifier) &&
        mMode == InteractionMode::Inspect) {
      selectAllModels();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_Escape && mModelSelected) {
      clearSelection();
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_G && mModelSelected) {
      beginModelTransform(InteractionMode::Move);
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_R && mModelSelected) {
      beginModelTransform(InteractionMode::Rotate);
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_S && mModelSelected) {
      beginModelTransform(InteractionMode::Scale);
      event->accept();
      return;
    }
    if (event->key() == Qt::Key_Comma && mModelSelected) {
      toggleTransformGizmoOrientation();
      event->accept();
      return;
    }
    QOpenGLWidget::keyPressEvent(event);
    return;
  }

  mTransformModifiers = event->modifiers();
  switch (event->key()) {
  case Qt::Key_Escape:
    finishModelTransform(false);
    event->accept();
    return;
  case Qt::Key_Return:
  case Qt::Key_Enter:
    finishModelTransform(true);
    event->accept();
    return;
  case Qt::Key_G:
    if (mMode != InteractionMode::Move) {
      finishModelTransform(false);
      beginModelTransform(InteractionMode::Move);
    }
    event->accept();
    return;
  case Qt::Key_R:
    if (mMode != InteractionMode::Rotate) {
      finishModelTransform(false);
      beginModelTransform(InteractionMode::Rotate);
    } else if (!mTrackballRotation) {
      mScene->mModelRotation = mModelDragStartTransform.rotation;
      mTrackballRotation = true;
      mTransformConstraint = {};
      mTransformNumericInput.clear();
      updateModelTransform(mModelTransformCurrentPosition,
                           mTransformModifiers);
    }
    event->accept();
    return;
  case Qt::Key_S:
    if (mMode != InteractionMode::Scale) {
      finishModelTransform(false);
      beginModelTransform(InteractionMode::Scale);
    }
    event->accept();
    return;
  case Qt::Key_X:
  case Qt::Key_Y:
  case Qt::Key_Z:
    applyTransformConstraint(
        event->key() == Qt::Key_X ? 0 : event->key() == Qt::Key_Y ? 1 : 2,
        event->modifiers().testFlag(Qt::ShiftModifier));
    event->accept();
    return;
  case Qt::Key_0:
  case Qt::Key_1:
  case Qt::Key_2:
  case Qt::Key_3:
  case Qt::Key_4:
  case Qt::Key_5:
  case Qt::Key_6:
  case Qt::Key_7:
  case Qt::Key_8:
  case Qt::Key_9:
  case Qt::Key_Minus:
  case Qt::Key_Period:
  case Qt::Key_Comma:
  case Qt::Key_Backspace:
  case Qt::Key_Delete:
    updateTransformNumericInput(event->key(), event->text());
    event->accept();
    return;
  case Qt::Key_Control:
  case Qt::Key_Shift:
    updateModelTransform(mModelTransformCurrentPosition, event->modifiers());
    event->accept();
    return;
  default:
    break;
  }
  QOpenGLWidget::keyPressEvent(event);
}

void NativeViewport::keyReleaseEvent(QKeyEvent *event) {
  if (mModelDragActive &&
      (event->key() == Qt::Key_Control || event->key() == Qt::Key_Shift)) {
    updateModelTransform(mModelTransformCurrentPosition, event->modifiers());
    event->accept();
    return;
  }
  QOpenGLWidget::keyReleaseEvent(event);
}

QVector3D NativeViewport::gridOrigin(const ReferenceGridPlane plane) const {
  const auto &reference = coordinateReferenceScene();
  const auto &coordinates = reference.mSceneCoordinates;
  if (!coordinates.valid) {
    return referenceGridOrigin();
  }
  const auto mapping = sceneDisplayTransform(reference, *mScene);
  if (mReferencePlaneMode == ReferencePlaneMode::WorldZero) {
    return mapping.map(coordinates.localFromGlobal({0.0, 0.0, 0.0}));
  }
  QVector3D minimum, maximum;
  if (!collectionReferenceBounds(minimum, maximum)) return mapping.map(QVector3D());
  switch (plane) {
  case ReferenceGridPlane::XY:
    return mapping.map(QVector3D(0.0F, 0.0F, minimum.z()));
  case ReferenceGridPlane::XZ:
    return mapping.map(QVector3D(0.0F, minimum.y(), 0.0F));
  case ReferenceGridPlane::YZ:
    return mapping.map(QVector3D(minimum.x(), 0.0F, 0.0F));
  }
  return referenceGridOrigin();
}

QString NativeViewport::formatViewportDistance(const float localDistance) const {
  return formatReferenceLength(static_cast<double>(localDistance) / referenceCoordinateToLocalLength());
}

ReferenceGridScale NativeViewport::sceneGridScale() const {
  const auto reference = sceneReferenceCoordinates();
  const double displayRatio = reference.valid && mScene->mSceneCoordinates.valid
      ? sourceToSceneUnitScale() * reference.displayScale / mScene->mSceneCoordinates.displayScale : 1.0;
  auto scale = referenceGridScale(static_cast<float>(mDistance * displayRatio),
      qMax(1, qRound(height() * devicePixelRatioF())));
  const float inverseRatio = static_cast<float>(1.0 / displayRatio);
  scale.minimumStep *= inverseRatio;
  scale.lowerMinorStep *= inverseRatio;
  scale.upperMinorStep *= inverseRatio;
  scale.displayMajorStep *= inverseRatio;
  scale.visibleDistance *= inverseRatio;
  // Report the central emphasized Blender level, not the retired 1-2-5
  // finite-patch spacing. Other pixels may show the adjacent blended levels.
  const double worldPerPixel = (mOrthographic ? 2.0 : 2.0 * mDistance) /
      std::max(1e-12, static_cast<double>(std::abs(projectionMatrix()(1, 1)))) /
      std::max(1.0, height() * devicePixelRatioF());
  const double level = std::max(0.0, std::floor(std::log10(
      std::max(worldPerPixel * 4.0 / scale.minimumStep, 1e-20))) + 1.0);
  scale.displayMajorStep = static_cast<float>(scale.minimumStep * std::pow(10.0, level + 1.0));
  return scale;
}

QMatrix4x4 NativeViewport::viewMatrix() const {
  QMatrix4x4 view;
  const OrbitFrame frame = orbitFrame(viewOrbitAngles());
  const QVector3D position = mTarget + frame.cameraOffsetDirection * mDistance;
  view.lookAt(position, mTarget, frame.upDirection);
  return view;
}

QMatrix4x4 NativeViewport::projectionMatrix() const {
  if (mRenderingInactiveScene) return mCollectionProjection;
  QMatrix4x4 projection;
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height())
                   : 1.0F;
  // Projection limits belong to the shared reference, not the active source.
  // Otherwise a metre/centimetre selection changes clipping precision and the
  // minimum orthographic field of view even though the camera did not move.
  const auto reference = sceneReferenceCoordinates();
  const float referenceLocalUnit = reference.valid && mScene->mSceneCoordinates.valid
      ? static_cast<float>(mScene->mSceneCoordinates.displayScale /
          (sourceToSceneUnitScale() * reference.displayScale)) : 1.0F;
  float nearPlane = std::max(0.001F * referenceLocalUnit, mDistance / 10000.0F);
  const float modelDistance =
      (transformedSceneCenter() - cameraPosition()).length();
  const float transformedRadius =
      mScene->mSceneRadius *
      std::max({std::abs(mScene->mModelScale.x()), std::abs(mScene->mModelScale.y()),
                std::abs(mScene->mModelScale.z())});
  float farPlane = mScene->visible
      ? std::max(100.0F * referenceLocalUnit, modelDistance + transformedRadius * 12.0F)
      : std::max(100.0F * referenceLocalUnit, mDistance * 2.0F);
  for (const auto &state : displayedSceneStates()) {
    const auto mapping = sceneDisplayTransform(*state, *mScene);
    const float radius = state->mSceneRadius *
        std::max({std::abs(state->mModelScale.x()), std::abs(state->mModelScale.y()),
                  std::abs(state->mModelScale.z())}) *
        mapping.mapVector(QVector3D(1, 0, 0)).length();
    farPlane = std::max(farPlane, (mapping.map(state->mSceneCenter + state->mModelTranslation) -
                                  cameraPosition()).length() + radius * 12);
  }
  if(mCameraViewActive&&mSourceCameraClipping) {
    const double near=(*mSourceCameraClipping)[0],far=(*mSourceCameraClipping)[1];
    if(std::isfinite(near)&&std::isfinite(far)&&near>0.&&far>near&&far<std::numeric_limits<float>::max()) {
      nearPlane=static_cast<float>(near);farPlane=static_cast<float>(far);
    }
  }
  if (mOrthographic) {
    const float halfHeight =
        std::max(0.05F * referenceLocalUnit, mDistance * std::tan(radians(mPerspectiveFovDegrees * 0.5F)));
    projection.ortho(-halfHeight * aspect, halfHeight * aspect, -halfHeight,
                     halfHeight, nearPlane, farPlane);
  } else {
    projection.perspective(mPerspectiveFovDegrees, aspect, nearPlane, farPlane);
    projection(0, 0) *= mFocalAspectCorrection;
    if(mCameraViewActive && mSourceCameraIntrinsics) {
      const auto &k=*mSourceCameraIntrinsics;
      projection(0,0)=static_cast<float>(2.*k[0]/(k[6]*aspect));
      projection(0,1)=static_cast<float>(-2.*k[4]/(k[6]*aspect));
      projection(0,2)=static_cast<float>((k[5]-2.*k[2])/(k[6]*aspect));
      projection(1,1)=static_cast<float>(2.*k[1]/k[6]);
      projection(1,2)=static_cast<float>(2.*k[3]/k[6]-1.);
    }
  }
  return projection;
}

QMatrix4x4 NativeViewport::viewProjectionMatrix() const {
  return projectionMatrix() * viewMatrix();
}

QMatrix4x4 NativeViewport::modelMatrix() const {
  return mLayerDisplayTransform * ModelTransform{mScene->mModelTranslation, mScene->mModelRotation, mScene->mModelScale}
      .matrix(mScene->mSceneCenter);
}

QVector3D NativeViewport::modelBoundsMinimum() const {
  if (mScene->mSceneCoordinates.valid) {
    return mScene->mSceneCoordinates.localMinimum();
  }
  return mScene->mSceneCenter - QVector3D(mScene->mSceneRadius, mScene->mSceneRadius, mScene->mSceneRadius);
}

QVector3D NativeViewport::modelBoundsMaximum() const {
  if (mScene->mSceneCoordinates.valid) {
    return mScene->mSceneCoordinates.localMaximum();
  }
  return mScene->mSceneCenter + QVector3D(mScene->mSceneRadius, mScene->mSceneRadius, mScene->mSceneRadius);
}

std::array<QVector3D, 8>
NativeViewport::transformedModelBoundsCorners() const {
  const QVector3D minimum = modelBoundsMinimum();
  const QVector3D maximum = modelBoundsMaximum();
  const QMatrix4x4 transform = modelMatrix();
  std::array<QVector3D, 8> corners;
  for (int corner = 0; corner < 8; ++corner) {
    const QVector3D point((corner & 1) != 0 ? maximum.x() : minimum.x(),
                          (corner & 2) != 0 ? maximum.y() : minimum.y(),
                          (corner & 4) != 0 ? maximum.z() : minimum.z());
    corners[static_cast<std::size_t>(corner)] = transform.map(point);
  }
  return corners;
}

QVector<QVector3D> NativeViewport::displayedBoundsCorners() const {
  QVector<QVector3D> corners;
  for (const auto &state : displayedSceneStates()) {
    const QVector3D low = state->mSceneCoordinates.valid ? state->mSceneCoordinates.localMinimum() :
        state->mSceneCenter - QVector3D(state->mSceneRadius, state->mSceneRadius, state->mSceneRadius);
    const QVector3D high = state->mSceneCoordinates.valid ? state->mSceneCoordinates.localMaximum() :
        state->mSceneCenter + QVector3D(state->mSceneRadius, state->mSceneRadius, state->mSceneRadius);
    const QMatrix4x4 transform = sceneDisplayTransform(*state, *mScene) *
        ModelTransform{state->mModelTranslation, state->mModelRotation, state->mModelScale}.matrix(state->mSceneCenter);
    for (int corner = 0; corner < 8; ++corner) corners.append(transform.map(QVector3D(
        corner & 1 ? high.x() : low.x(), corner & 2 ? high.y() : low.y(),
        corner & 4 ? high.z() : low.z())));
  }
  return corners;
}

QVector3D NativeViewport::transformedSceneCenter() const {
  if (!mScene->visible) {
    const auto corners = displayedBoundsCorners();
    if (corners.isEmpty()) return {};
    QVector3D low = corners.first(), high = low;
    for (const auto &corner : corners) {
      for (int axis = 0; axis < 3; ++axis) {
        low[axis] = std::min(low[axis], corner[axis]);
        high[axis] = std::max(high[axis], corner[axis]);
      }
    }
    return (low + high) * 0.5F;
  }
  return mScene->mSceneCenter + mScene->mModelTranslation;
}

float NativeViewport::transformedSceneRadius() const {
  const QVector3D center = transformedSceneCenter();
  float radius = 0.0F;
  if (!mScene->visible) {
    for (const auto &corner : displayedBoundsCorners())
      radius = std::max(radius, (corner - center).length());
    return std::max(radius, 1.0e-4F);
  }
  for (const QVector3D &corner : transformedModelBoundsCorners()) {
    radius = std::max(radius, (corner - center).length());
  }
  return std::max(radius, 1.0e-4F);
}

bool NativeViewport::modelHitAt(const QPointF &position) {
  if (!selectableModelAvailable()) {
    return false;
  }
  const std::optional<WorldRay> worldRay =
      screenRay(position, size(), viewProjectionMatrix());
  if (!worldRay.has_value()) {
    return false;
  }
  const std::optional<WorldRay> localRay =
      rayInModelSpace(*worldRay, modelMatrix());
  if (!localRay.has_value()) {
    return false;
  }
  const float padding = std::max(mScene->mSceneRadius * 0.015F, 1.0e-4F);
  const QVector3D paddingVector(padding, padding, padding);
  if (!rayAabbDistance(*localRay, modelBoundsMinimum() - paddingVector,
                       modelBoundsMaximum() + paddingVector)
           .has_value()) {
    return false;
  }

  // The AABB is deliberately only a cheap broad phase.  Large scans often
  // contain irregular silhouettes and empty volume, so selecting everything
  // inside that box makes camera navigation frustrating.  A successful GPU
  // mask pass accepts only pixels covered by real model primitives.
  return modelGeometryHitAt(position).value_or(true);
}

QString NativeViewport::sceneObjectAt(const QPointF &position) {
  const auto active = mScene;
  mCollectionProjection = projectionMatrix();
  QString bestId;
  float nearestDepth = 1.0F;
  for (const auto &state : displayedSceneStates()) {
    QScopedValueRollback<std::shared_ptr<SceneState>> scope(mScene, state);
    QScopedValueRollback<bool> background(mRenderingInactiveScene, state != active);
    QScopedValueRollback<QMatrix4x4> display(mLayerDisplayTransform,
                                             sceneDisplayTransform(*state, *active));
    if (!selectableModelAvailable()) continue;
    float depth = 1.0F;
    const auto hit = modelGeometryHitAt(position, &depth);
    if (hit.value_or(false) && depth < nearestDepth) {
      nearestDepth = depth;
      bestId = state->id;
    }
  }
  return bestId;
}

std::optional<bool>
NativeViewport::modelGeometryHitAt(const QPointF &position, float *hitDepth,
                                  QVector3D *hitWorldPosition) {
  if (!mScene->visible) return false;
  const auto mapping =
      modelPickViewport(position, size(), devicePixelRatioF(), 6.0);
  if (!mapping.has_value()) return std::nullopt;

  // Object selection must be as read-only as selection from the scene tree.
  // Rendering a temporary pick FBO in the main viewport changes Qt/driver GL
  // state and used to detach the caller's current context. File-backed 3DGS
  // and 2DGS already retain the exact displayed point centres on the CPU.
  // Keep world-position navigation and GPU-only IPC previews on their existing
  // surface-picking path; neither is represented as a static CPU point set.
  const bool liveGpuFrame = trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame();
  if (hitWorldPosition == nullptr && mScene->mHasGaussianAttributes &&
      mScene->mRenderMode != RenderMode::Mesh && !liveGpuFrame &&
      !mScene->mPreviewOnlyScene && !mScene->mPreviewVertices.isEmpty()) {
    if (mScene->processingGpuSuspended || mScene->resourceUploadDeferred ||
        mScene->mPointUploadPending || mScene->resourcePointGpuBytes <= 0 ||
        mScene->mRenderedPointCount <= 0) return false;
    const auto depth = pointModelPickDepth(
        std::span<const PointCloudVertex>(mScene->mPreviewVertices.constData(),
            static_cast<std::size_t>(mScene->mPreviewVertices.size())),
        mScene->mEditModel.deletedBits(), viewProjectionMatrix() * modelMatrix(),
        *mapping, std::max(3.0, 3.0 * devicePixelRatioF()), hitDepth != nullptr);
    if (hitDepth) *hitDepth = depth.value_or(1.0F);
    return depth.has_value();
  }
  // Picking only observes already displayed storage. Promotion/upload belongs
  // to paintGL(), not an input callback with an arbitrary current context.
  if (mScene->processingGpuSuspended || mScene->resourceUploadDeferred) return false;
  if (!mModelPickShaderReady || mModelPickProgram == nullptr ||
      !mModelPickProgram->isLinked() || context() == nullptr ||
      !context()->isValid()) {
    return std::nullopt;
  }
  auto *previousContext = QOpenGLContext::currentContext();
  auto *previousSurface = previousContext ? previousContext->surface() : nullptr;
  auto *viewportContext = context();
  if (previousContext && previousContext != viewportContext && !previousSurface)
    return std::nullopt;
  const auto restoreContext = qScopeGuard([&] {
    if (previousContext && previousSurface) {
      if (QOpenGLContext::currentContext() != previousContext ||
          previousContext->surface() != previousSurface)
        previousContext->makeCurrent(previousSurface);
    } else if (!previousContext && QOpenGLContext::currentContext() == viewportContext) {
      viewportContext->doneCurrent();
    }
  });
  // makeCurrent() also binds the widget FBO, so do not call it when the
  // caller already owns this context with a different framebuffer bound.
  if (previousContext != viewportContext) makeCurrent();
  if (QOpenGLContext::currentContext() != viewportContext) {
    return std::nullopt;
  }
  QOpenGLFunctions_3_3_Core desktopGl;
  if (!desktopGl.initializeOpenGLFunctions()) return std::nullopt;

  GLint previousDrawFramebuffer = 0;
  GLint previousReadFramebuffer = 0;
  GLint previousViewport[4] = {0, 0, 0, 0};
  GLint previousProgram = 0;
  GLint previousVertexArray = 0;
  GLint previousArrayBuffer = 0;
  GLint previousPixelPackBuffer = 0;
  GLint previousPixelUnpackBuffer = 0;
  GLint previousReadBuffer = GL_BACK;
  GLint previousPackAlignment = 4;
  GLint previousPackRowLength = 0;
  GLint previousPackSkipRows = 0;
  GLint previousPackSkipPixels = 0;
  GLint previousPackSwapBytes = GL_FALSE;
  GLint previousPackLsbFirst = GL_FALSE;
  GLint previousActiveTexture = GL_TEXTURE0;
  GLint previousTexture0 = 0;
  GLint previousRenderbuffer = 0;
  GLint previousDepthFunction = GL_LESS;
  GLdouble previousClearDepth = 1.0;
  GLfloat previousClearColor[4] = {0.0F, 0.0F, 0.0F, 1.0F};
  GLboolean previousDepthMask = GL_TRUE;
  GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
  glGetIntegerv(GL_VIEWPORT, previousViewport);
  glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelPackBuffer);
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &previousPixelUnpackBuffer);
  glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
  glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &previousPackRowLength);
  glGetIntegerv(GL_PACK_SKIP_ROWS, &previousPackSkipRows);
  glGetIntegerv(GL_PACK_SKIP_PIXELS, &previousPackSkipPixels);
  glGetIntegerv(GL_PACK_SWAP_BYTES, &previousPackSwapBytes);
  glGetIntegerv(GL_PACK_LSB_FIRST, &previousPackLsbFirst);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
  glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
  desktopGl.glGetDoublev(GL_DEPTH_CLEAR_VALUE, &previousClearDepth);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, previousClearColor);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);
  glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);

  const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
  const GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);
  const GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean ditherEnabled = glIsEnabled(GL_DITHER);
  const GLboolean multisampleEnabled = glIsEnabled(GL_MULTISAMPLE);
  const GLboolean pointSizeEnabled = glIsEnabled(GL_PROGRAM_POINT_SIZE);
  const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean stencilEnabled = glIsEnabled(GL_STENCIL_TEST);

  const auto restoreGlState = qScopeGuard([&] {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
    glReadBuffer(static_cast<GLenum>(previousReadBuffer));
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    glClearColor(previousClearColor[0], previousClearColor[1], previousClearColor[2], previousClearColor[3]);
    desktopGl.glClearDepth(previousClearDepth);
    glDepthFunc(static_cast<GLenum>(previousDepthFunction));
    glDepthMask(previousDepthMask);
    glColorMask(previousColorMask[0], previousColorMask[1], previousColorMask[2], previousColorMask[3]);
    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, previousPackRowLength);
    glPixelStorei(GL_PACK_SKIP_ROWS, previousPackSkipRows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, previousPackSkipPixels);
    glPixelStorei(GL_PACK_SWAP_BYTES, previousPackSwapBytes);
    glPixelStorei(GL_PACK_LSB_FIRST, previousPackLsbFirst);
    glUseProgram(static_cast<GLuint>(previousProgram));
    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPixelPackBuffer));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(previousPixelUnpackBuffer));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
    const auto restoreCapability = [this](const GLenum capability, const GLboolean enabled) {
      enabled == GL_TRUE ? glEnable(capability) : glDisable(capability);
    };
    restoreCapability(GL_BLEND, blendEnabled);
    restoreCapability(GL_CULL_FACE, cullEnabled);
    restoreCapability(GL_DEPTH_TEST, depthEnabled);
    restoreCapability(GL_DITHER, ditherEnabled);
    restoreCapability(GL_MULTISAMPLE, multisampleEnabled);
    restoreCapability(GL_PROGRAM_POINT_SIZE, pointSizeEnabled);
    restoreCapability(GL_SCISSOR_TEST, scissorEnabled);
    restoreCapability(GL_STENCIL_TEST, stencilEnabled);
  });

  std::optional<bool> result;
  {
    // A null texture pointer must mean allocation-only, never offset zero in
    // a caller's unpack PBO. FBO construction also changes texture/RBO binds.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::Depth);
    format.setInternalTextureFormat(GL_RGBA8);
    format.setSamples(0);
    QOpenGLFramebufferObject framebuffer(mapping->framebufferSize, format);
    if (framebuffer.isValid() && framebuffer.bind()) {
      glViewport(mapping->viewportOrigin.x(), mapping->viewportOrigin.y(),
                 mapping->sourceViewportSize.width(),
                 mapping->sourceViewportSize.height());
      glDisable(GL_BLEND);
      glDisable(GL_CULL_FACE);
      glDisable(GL_DITHER);
      glDisable(GL_MULTISAMPLE);
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_STENCIL_TEST);
      glEnable(GL_DEPTH_TEST);
      glEnable(GL_PROGRAM_POINT_SIZE);
      glDepthFunc(GL_LESS);
      glDepthMask(GL_TRUE);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
      desktopGl.glClearDepth(1.0);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

      const QMatrix4x4 modelViewProjection =
          viewProjectionMatrix() * modelMatrix() *
          ((trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame()) ? trainingPreviewCoordinateTransform() : QMatrix4x4());
      const float pointSize = std::max(
          3.0F, 3.0F * static_cast<float>(devicePixelRatioF()));
      bool drewPrimitives = false;
      mModelPickProgram->bind();
      mModelPickProgram->setUniformValue("viewProjection",
                                         modelViewProjection);
      mModelPickProgram->setUniformValue("pointSize", pointSize);

      if (mScene->mRenderMode == RenderMode::Mesh && meshRenderingAvailable()) {
        mModelPickProgram->setUniformValue("pointPrimitive", false);
        if (mScene->mRenderedMeshIndexCount >= 3 &&
            mScene->mMeshVertexArray.isCreated() && mScene->mMeshIndexBuffer.isCreated()) {
          glBindVertexArray(mScene->mMeshVertexArray.objectId());
          glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mScene->mMeshIndexBuffer.bufferId());
          glDrawElements(GL_TRIANGLES,
                         static_cast<GLsizei>(mScene->mRenderedMeshIndexCount),
                         GL_UNSIGNED_INT, nullptr);
          drewPrimitives = true;
        }
        for (const FullResolutionMeshGpuChunk &chunk :
             std::as_const(mScene->mFullResolutionMeshGpuChunks)) {
          if (chunk.vertexArray == 0 || chunk.indexCount < 3) {
            continue;
          }
          glBindVertexArray(chunk.vertexArray);
          glDrawElements(GL_TRIANGLES, chunk.indexCount, GL_UNSIGNED_INT,
                         nullptr);
          drewPrimitives = true;
        }
      } else if ((trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame()) &&
                 mScene->mRenderMode == RenderMode::Points) {
        const GLuint vertexArray = mTrainingGpuPreview.activeVertexArray();
        const GLsizei pointCount = mTrainingGpuPreview.pointCount();
        if (vertexArray != 0 && pointCount > 0) {
          mModelPickProgram->setUniformValue("pointPrimitive", true);
          glBindVertexArray(vertexArray);
          GLint previousDivisor = 0;
          glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &previousDivisor);
          glVertexAttribDivisor(0, 0);
          glDrawArrays(GL_POINTS, 0, pointCount);
          glVertexAttribDivisor(0, static_cast<GLuint>(previousDivisor));
          drewPrimitives = true;
        }
      } else if (mScene->mRenderMode == RenderMode::Gaussians &&
                 gaussianRenderingAvailable()) {
        const bool livePreview = (trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame());
        const GLuint vertexArray =
            livePreview ? mTrainingGpuPreview.activeVertexArray()
                        : mScene->mGaussianVertexArray.objectId();
        const GLsizei pointCount = static_cast<GLsizei>(
            livePreview ? mTrainingGpuPreview.pointCount()
                        : mScene->mRenderedPointCount);
        if (vertexArray != 0 && pointCount > 0) {
          mModelPickProgram->setUniformValue("pointPrimitive", true);
          glBindVertexArray(vertexArray);
          GLint previousDivisor = 0;
          glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &previousDivisor);
          glVertexAttribDivisor(0, 0);
          glDrawArrays(GL_POINTS, 0, pointCount);
          glVertexAttribDivisor(0, static_cast<GLuint>(previousDivisor));
          drewPrimitives = true;
        }
      } else {
        mModelPickProgram->setUniformValue("pointPrimitive", true);
        if (mScene->mRenderedPointCount > 0 && mScene->mPointVertexArray.isCreated()) {
          glBindVertexArray(mScene->mPointVertexArray.objectId());
          glDrawArrays(GL_POINTS, 0,
                       static_cast<GLsizei>(mScene->mRenderedPointCount));
          drewPrimitives = true;
        }
        for (const FullResolutionGpuChunk &chunk :
             std::as_const(mScene->mFullResolutionPointGpuChunks)) {
          if (chunk.vertexArray == 0 || chunk.pointCount <= 0) {
            continue;
          }
          glBindVertexArray(chunk.vertexArray);
          glDrawArrays(GL_POINTS, 0, chunk.pointCount);
          drewPrimitives = true;
        }
      }

      glBindVertexArray(0);
      mModelPickProgram->release();
      if (drewPrimitives) {
        const qsizetype sampleCount =
            static_cast<qsizetype>(mapping->framebufferSize.width()) *
            mapping->framebufferSize.height();
        std::vector<quint8> samples(static_cast<std::size_t>(sampleCount));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // Both vectors below are tightly packed W*H allocations. Inherited
        // row strides/skips can overwrite them, and byte swapping corrupts
        // the float depth samples used for ordering and surface navigation.
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, mapping->framebufferSize.width(),
                     mapping->framebufferSize.height(), GL_RED,
                     GL_UNSIGNED_BYTE, samples.data());
        result = modelPickBufferHasCoverage(samples);
        if ((hitDepth != nullptr || hitWorldPosition != nullptr) && result.value_or(false)) {
          std::vector<float> depths(static_cast<std::size_t>(sampleCount), 1.0F);
          glReadPixels(0, 0, mapping->framebufferSize.width(),
                       mapping->framebufferSize.height(), GL_DEPTH_COMPONENT,
                       GL_FLOAT, depths.data());
          float bestDepth = 1.0F;
          int nearestPixel = std::numeric_limits<int>::max();
          std::size_t bestIndex = 0;
          for (std::size_t index = 0; index < samples.size(); ++index) {
            if (samples[index] == 0 || !std::isfinite(depths[index]) || depths[index] >= 1.0F) continue;
            const int x = static_cast<int>(index) % mapping->framebufferSize.width() - mapping->sampleCenter.x();
            const int y = static_cast<int>(index) / mapping->framebufferSize.width() - mapping->sampleCenter.y();
            const int pixelDistance = x * x + y * y;
            // Surface navigation uses the closest covered pixel, not a
            // foreground outlier at the edge of the point-picking tolerance.
            if (hitWorldPosition ? pixelDistance < nearestPixel ||
                    (pixelDistance == nearestPixel && depths[index] < bestDepth)
                                 : depths[index] < bestDepth) {
              nearestPixel = pixelDistance;
              bestDepth = depths[index];
              bestIndex = index;
            }
          }
          if (hitDepth) *hitDepth = bestDepth;
          if (hitWorldPosition && bestDepth < 1.0F) {
            const float x = static_cast<float>(bestIndex % mapping->framebufferSize.width()) - mapping->viewportOrigin.x() + 0.5F;
            const float y = static_cast<float>(bestIndex / mapping->framebufferSize.width()) - mapping->viewportOrigin.y() + 0.5F;
            bool invertible = false;
            const auto inverse = viewProjectionMatrix().inverted(&invertible);
            const QVector4D point = inverse * QVector4D(
                2.0F * x / mapping->sourceViewportSize.width() - 1.0F,
                2.0F * y / mapping->sourceViewportSize.height() - 1.0F,
                2.0F * bestDepth - 1.0F, 1.0F);
            if (invertible && std::isfinite(point.w()) && std::abs(point.w()) > 1.0e-8F)
              *hitWorldPosition = point.toVector3DAffine();
            else result = false;
          } else if (hitWorldPosition) {
            result = false;
          }
        }
      }
    }
  }

  return result;
}

QPointF NativeViewport::currentPointerPosition() const {
  if (!mPointerPosition.isNull() && mPointerPosition.x() >= 0.0 &&
      mPointerPosition.y() >= 0.0 &&
      mPointerPosition.x() <= width() && mPointerPosition.y() <= height()) {
    return mPointerPosition;
  }
  return QPointF(width() * 0.5, height() * 0.5);
}

float NativeViewport::modelGizmoWorldRadius() const {
  // Source bounds provide a stable unit reference. Neither camera zoom nor
  // the transform being edited may resize the handles in world space.
  if (mSelectedSceneIds.size() > 1) {
    if (mModelDragActive) return mGroupGizmoRadius;
    float radius = 0.0F;
    const auto pivot = selectionPivot();
    for (const auto &corner : selectionBoundsCorners()) radius = std::max(radius, (corner - pivot).length());
    return std::max(1.0e-4F, radius) *
        (mTransformGizmoMode == TransformGizmoMode::Transform ? 0.5F : 0.3F);
  }
  return mScene->mSceneRadius *
         (mTransformGizmoMode == TransformGizmoMode::Transform ? 0.5F : 0.3F);
}

TransformGizmoLayout NativeViewport::modelTransformGizmo() const {
  if (!mModelSelected || !selectableModelAvailable()) {
    return {};
  }
  // A diagonal scale is defined in the object's local basis. The standalone
  // scale and combined tools therefore use that basis even when move/rotate
  // are configured for world orientation; this avoids silently introducing
  // an unrepresentable shear into the persisted TRS transform.
  const QQuaternion orientation =
      modelGizmoUsesLocalOrientation() ? mScene->mModelRotation : QQuaternion();
  auto layout = transformGizmoLayout(
      selectionPivot(), orientation, viewProjectionMatrix(),
      QSizeF(width(), height()), mTransformGizmoMode, modelGizmoWorldRadius());
  if (mSelectedSceneIds.size() > 1) {
    for (auto &axis : layout.axes) {
      axis.scaleHandle = {};
      axis.scaleLine = {};
      if (mTransformGizmoMode == TransformGizmoMode::Scale) axis.visible = false;
    }
    if (mTransformGizmoMode == TransformGizmoMode::Scale) {
      for (auto &plane : layout.planeHandles) plane.clear();
    }
  }
  return layout;
}

TransformToolStripLayout NativeViewport::transformToolStrip() const {
  return transformToolStripLayout(QSizeF(width(), height()),
                                  QFontMetricsF(font()).height());
}

QString NativeViewport::transformGizmoHandleDescription(
    const TransformGizmoHandle &handle) const {
  static const std::array<QString, 3> axes = {
      QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
  const QString axis = handle.axis >= 0 && handle.axis < 3
                           ? axes.at(static_cast<std::size_t>(handle.axis))
                           : QString();
  switch (handle.kind) {
  case TransformGizmoHandleKind::MoveAxis:
    return QCoreApplication::translate("Workbench", "沿 %1 轴移动").arg(axis);
  case TransformGizmoHandleKind::MovePlane:
    return QCoreApplication::translate("Workbench", "在垂直于 %1 轴的平面移动").arg(axis);
  case TransformGizmoHandleKind::MoveView:
    return QCoreApplication::translate("Workbench", "沿视图平面自由移动");
  case TransformGizmoHandleKind::RotateAxis:
    return QCoreApplication::translate("Workbench", "绕 %1 轴旋转").arg(axis);
  case TransformGizmoHandleKind::RotateView:
    return QCoreApplication::translate("Workbench", "绕视图轴旋转");
  case TransformGizmoHandleKind::RotateTrackball:
    return QCoreApplication::translate("Workbench", "自由轨迹球旋转");
  case TransformGizmoHandleKind::ScaleAxis:
    return QCoreApplication::translate("Workbench", "沿局部 %1 轴缩放").arg(axis);
  case TransformGizmoHandleKind::ScalePlane:
    return QCoreApplication::translate("Workbench", "在局部 %1 平面的双轴缩放").arg(axis);
  case TransformGizmoHandleKind::ScaleUniform:
    return QCoreApplication::translate("Workbench", "等比缩放");
  case TransformGizmoHandleKind::None:
    return {};
  }
  return {};
}

void NativeViewport::updateTransformGizmoHover(const QPointF &position) {
  if (mEditToolsLocked || mModelDragActive) {
    return;
  }
  const int tool = selectableModelAvailable()
                       ? hitTestTransformToolStrip(transformToolStrip(), position)
                       : -1;
  TransformGizmoHandle handle;
  if (tool < 0 && mModelSelected) {
    handle = hitTestTransformGizmo(modelTransformGizmo(), position,
                                   mTransformGizmoMode);
  }
  if (tool == mTransformToolHover && handle == mTransformGizmoHover) {
    return;
  }
  mTransformToolHover = tool;
  mTransformGizmoHover = handle;

  if (tool >= 0) {
    const std::array<QString, 4> descriptions = {
        QCoreApplication::translate("Workbench", "移动工具（G）"), QCoreApplication::translate("Workbench", "旋转工具（R）"),
        QCoreApplication::translate("Workbench", "缩放工具（S）"),
        QCoreApplication::translate("Workbench", "组合变换工具")};
    if (tool < 4) {
      setToolTip(descriptions.at(static_cast<std::size_t>(tool)));
    } else {
      setToolTip(mSelectedSceneIds.size() > 1 && modelGizmoOrientationLocked()
                     ? QCoreApplication::translate("Workbench", "多选采用等比缩放，保持模型之间的比例；无需选择缩放坐标系")
                     : modelGizmoOrientationLocked()
                     ? mTransformGizmoMode == TransformGizmoMode::Scale
                           ? QCoreApplication::translate("Workbench", "局部锁定：非等比缩放必须沿模型自身轴，避免产生剪切")
                           : QCoreApplication::translate("Workbench", "局部锁定：组合工具包含缩放；切换到移动或旋转工具后可选择全局坐标")
                     : QCoreApplication::translate("Workbench", "切换全局/局部坐标系（,）"));
    }
    setCursor(tool == 4 && modelGizmoOrientationLocked()
                  ? Qt::ArrowCursor
                  : Qt::PointingHandCursor);
  } else if (handle.isValid()) {
    setToolTip(transformGizmoHandleDescription(handle) +
               QCoreApplication::translate("Workbench", " · Ctrl 吸附 · Shift 精细"));
    switch (handle.kind) {
    case TransformGizmoHandleKind::MoveAxis:
    case TransformGizmoHandleKind::MovePlane:
    case TransformGizmoHandleKind::MoveView:
      setCursor(Qt::SizeAllCursor);
      break;
    case TransformGizmoHandleKind::RotateAxis:
    case TransformGizmoHandleKind::RotateView:
    case TransformGizmoHandleKind::RotateTrackball:
      setCursor(Qt::OpenHandCursor);
      break;
    case TransformGizmoHandleKind::ScaleAxis:
    case TransformGizmoHandleKind::ScalePlane:
    case TransformGizmoHandleKind::ScaleUniform:
      setCursor(Qt::SizeFDiagCursor);
      break;
    case TransformGizmoHandleKind::None:
      break;
    }
  } else {
    setToolTip({});
    setCursor(defaultInteractionCursor(mMode));
  }
  update();
}

void NativeViewport::toggleTransformGizmoOrientation() {
  if (mEditToolsLocked) return;
  QString message;
  if (modelGizmoOrientationLocked()) {
    message = mSelectedSceneIds.size() > 1
                  ? QCoreApplication::translate("Workbench", "多选采用等比缩放，保持模型之间的比例")
                  : mTransformGizmoMode == TransformGizmoMode::Scale
                  ? QCoreApplication::translate("Workbench", "局部锁定：缩放只使用模型自身轴，避免产生剪切")
                  : QCoreApplication::translate("Workbench", "局部锁定：请切换到移动或旋转工具后再选择全局坐标");
  } else {
    mTransformGizmoLocal = !mTransformGizmoLocal;
    message = mTransformGizmoLocal ? QCoreApplication::translate("Workbench", "已切换到局部坐标系")
                                   : QCoreApplication::translate("Workbench", "已切换到全局坐标系");
  }
  setToolTip(message);
  QToolTip::showText(mapToGlobal(currentPointerPosition().toPoint()), message,
                     this);
  update();
}

void NativeViewport::activateTransformToolAt(const int toolIndex) {
  if (mEditToolsLocked) return;
  if (toolIndex == 4) {
    toggleTransformGizmoOrientation();
    return;
  }
  if (toolIndex < 0 || toolIndex > 3 || !selectableModelAvailable()) {
    return;
  }
  if (!mModelSelected) {
    selectModel();
  }
  static const std::array<TransformGizmoMode, 4> modes = {
      TransformGizmoMode::Move, TransformGizmoMode::Rotate,
      TransformGizmoMode::Scale, TransformGizmoMode::Transform};
  setModelGizmoMode(modes.at(static_cast<std::size_t>(toolIndex)));
  updateTransformGizmoHover(mPointerPosition);
}

void NativeViewport::beginTransformGizmoDrag(
    const TransformGizmoHandle &handle, const QPointF &position,
    const Qt::KeyboardModifiers modifiers) {
  if (mEditToolsLocked) return;
  if (!handle.isValid() || !mModelSelected || !selectableModelAvailable()) {
    return;
  }
  if (mSelectedSceneIds.size() > 1 &&
      (handle.kind == TransformGizmoHandleKind::ScaleAxis ||
       handle.kind == TransformGizmoHandleKind::ScalePlane)) return;
  mPointerPosition = position;
  InteractionMode operation = InteractionMode::Inspect;
  bool trackball = false;
  switch (handle.kind) {
  case TransformGizmoHandleKind::MoveAxis:
  case TransformGizmoHandleKind::MovePlane:
  case TransformGizmoHandleKind::MoveView:
    operation = InteractionMode::Move;
    break;
  case TransformGizmoHandleKind::RotateAxis:
  case TransformGizmoHandleKind::RotateView:
    operation = InteractionMode::Rotate;
    break;
  case TransformGizmoHandleKind::RotateTrackball:
    operation = InteractionMode::Rotate;
    trackball = true;
    break;
  case TransformGizmoHandleKind::ScaleAxis:
  case TransformGizmoHandleKind::ScalePlane:
  case TransformGizmoHandleKind::ScaleUniform:
    operation = InteractionMode::Scale;
    break;
  case TransformGizmoHandleKind::None:
    return;
  }
  beginModelTransform(operation, trackball);
  if (!mModelDragActive) {
    return;
  }
  const bool local = modelGizmoUsesLocalOrientation() ||
                     operation == InteractionMode::Scale;
  switch (handle.kind) {
  case TransformGizmoHandleKind::MoveAxis:
  case TransformGizmoHandleKind::RotateAxis:
  case TransformGizmoHandleKind::ScaleAxis:
    mTransformConstraint = {TransformConstraintKind::Axis, handle.axis, local};
    break;
  case TransformGizmoHandleKind::MovePlane:
  case TransformGizmoHandleKind::ScalePlane:
    mTransformConstraint = {TransformConstraintKind::Plane, handle.axis, local};
    break;
  case TransformGizmoHandleKind::MoveView:
  case TransformGizmoHandleKind::RotateView:
  case TransformGizmoHandleKind::RotateTrackball:
  case TransformGizmoHandleKind::ScaleUniform:
  case TransformGizmoHandleKind::None:
    mTransformConstraint = {};
    break;
  }
  mTransformModifiers = modifiers;
  mTransformGizmoDragActive = true;
  mTransformGizmoPress = handle;
  mTransformGizmoHover = handle;
  if (handle.kind == TransformGizmoHandleKind::RotateAxis) {
    const auto layout = modelTransformGizmo();
    mModelRotationRadiusPixels = static_cast<float>(layout.radius *
        (mTransformGizmoMode == TransformGizmoMode::Transform ? 0.84 : 0.78));
  }
  setCursor(operation == InteractionMode::Rotate
                ? Qt::ClosedHandCursor
                : defaultInteractionCursor(operation));
  setToolTip(transformGizmoHandleDescription(handle));
  update();
}

void NativeViewport::beginModelTransform(const InteractionMode mode,
                                         const bool trackball) {
  if (mEditToolsLocked || !mModelSelected || !selectableModelAvailable() ||
      (mode != InteractionMode::Move && mode != InteractionMode::Rotate &&
       mode != InteractionMode::Scale)) {
    return;
  }
  if (mModelDragActive) {
    finishModelTransform(false);
  }
  mGroupPivot = selectionPivot();
  mGroupPreviewPivot = mGroupPivot;
  mGroupGizmoRadius = modelGizmoWorldRadius();
  mTransformDragBefore = selectedTransforms();
  const InteractionMode previousMode = mMode;
  mMode = mode;
  mModelDragActive = true;
  mTrackballRotation = mode == InteractionMode::Rotate && trackball;
  mTransformConstraint = {};
  mTransformNumericInput.clear();
  mTransformModifiers = Qt::NoModifier;
  mModelDragStartTransform = {mScene->mModelTranslation, mScene->mModelRotation, mScene->mModelScale};
  mModelTransformStartPosition = currentPointerPosition();
  mModelTransformCurrentPosition = mModelTransformStartPosition;
  mModelDragPlaneNormal = mTarget - cameraPosition();
  if (mModelDragPlaneNormal.lengthSquared() <= 1.0e-12F) {
    mModelDragPlaneNormal = QVector3D(0.0F, 0.0F, -1.0F);
  } else {
    mModelDragPlaneNormal.normalize();
  }
  const QVector3D pivot =
      mTransformDragBefore.size() > 1 ? mGroupPivot :
      mScene->mSceneCenter + mModelDragStartTransform.translation;
  const auto center = projectPoint(pivot, viewProjectionMatrix());
  mModelTransformScreenCenter =
      center.value_or(QPointF(width() * 0.5, height() * 0.5));
  mModelRotationRadiusPixels = static_cast<float>(modelTransformGizmo().radius);
  const auto startRay = screenRay(mModelTransformStartPosition, size(),
                                  viewProjectionMatrix());
  const auto intersection =
      startRay.has_value()
          ? rayPlaneIntersection(*startRay, pivot, mModelDragPlaneNormal)
          : std::nullopt;
  mModelDragStartIntersection = intersection.value_or(pivot);
  setFocus(Qt::ShortcutFocusReason);
  setCursor(defaultInteractionCursor(mMode));
  setToolTip({});
  if (previousMode != mMode) {
    emit interactionModeChanged(mMode);
  }
  notifyModelInteractionState();
  update();
}

QVector3D NativeViewport::transformConstraintAxis() const {
  if (mTransformConstraint.axis < 0 || mTransformConstraint.axis > 2) {
    return {};
  }
  QVector3D axis;
  axis[mTransformConstraint.axis] = 1.0F;
  if (mTransformConstraint.local) {
    axis = mModelDragStartTransform.rotation.rotatedVector(axis);
  }
  return axis.normalized();
}

QString NativeViewport::transformConstraintLabel() const {
  if (mTransformConstraint.kind == TransformConstraintKind::None ||
      mTransformConstraint.axis < 0 || mTransformConstraint.axis > 2) {
    return QCoreApplication::translate("Workbench", "自由");
  }
  static const std::array<QString, 3> labels = {
      QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
  const QString space = mTransformConstraint.local
                            ? QCoreApplication::translate("Workbench", "局部")
                            : QCoreApplication::translate("Workbench", "全局");
  return mTransformConstraint.kind == TransformConstraintKind::Plane
             ? QCoreApplication::translate("Workbench", "%1 %2 平面").arg(space, labels.at(
                                                          mTransformConstraint.axis))
             : QCoreApplication::translate("Workbench", "%1 %2 轴").arg(space, labels.at(
                                                        mTransformConstraint.axis));
}

float NativeViewport::transformSnapStep(const bool fine) const {
  const ReferenceGridScale scale = sceneGridScale();
  const float coarse = std::max(scale.displayMajorStep * 0.1F,
                                scale.minimumStep);
  return fine ? coarse * 0.1F : coarse;
}

void NativeViewport::updateModelTransform(
    const QPointF &position, const Qt::KeyboardModifiers modifiers) {
  if (!mModelDragActive) {
    return;
  }
  mModelTransformCurrentPosition = position;
  mTransformModifiers = modifiers;
  const QVector3D pivot =
      mTransformDragBefore.size() > 1 ? mGroupPivot :
      mScene->mSceneCenter + mModelDragStartTransform.translation;
  const auto startRay = screenRay(mModelTransformStartPosition, size(),
                                  viewProjectionMatrix());
  const auto currentRay =
      screenRay(position, size(), viewProjectionMatrix());
  bool numericReady = false;
  const double numericValue = mTransformNumericInput.toDouble(&numericReady);
  const bool precision = modifiers.testFlag(Qt::ShiftModifier);
  const bool snap = modifiers.testFlag(Qt::ControlModifier);

  if (mMode == InteractionMode::Move) {
    QVector3D delta;
    const QVector3D axis = transformConstraintAxis();
    if (startRay.has_value() && currentRay.has_value() &&
        mTransformConstraint.kind == TransformConstraintKind::Axis) {
      const auto startParameter = rayAxisParameter(*startRay, pivot, axis);
      const auto currentParameter = rayAxisParameter(*currentRay, pivot, axis);
      if (startParameter.has_value() && currentParameter.has_value()) {
        delta = axis * (*currentParameter - *startParameter);
      }
    } else if (startRay.has_value() && currentRay.has_value()) {
      const QVector3D normal =
          mTransformConstraint.kind == TransformConstraintKind::Plane
              ? axis
              : mModelDragPlaneNormal;
      const auto start = rayPlaneIntersection(*startRay, pivot, normal);
      const auto current = rayPlaneIntersection(*currentRay, pivot, normal);
      if (start.has_value() && current.has_value()) {
        delta = *current - *start;
      }
    }
    if (precision) {
      delta *= 0.1F;
    }
    if (numericReady && std::isfinite(numericValue)) {
      QVector3D direction = delta;
      if (mTransformConstraint.kind == TransformConstraintKind::Axis) {
        direction = axis;
      } else if (direction.lengthSquared() <= 1.0e-12F) {
        const OrbitFrame frame = orbitFrame(viewOrbitAngles());
        direction = QVector3D::crossProduct(mModelDragPlaneNormal,
                                            frame.upDirection);
      }
      if (direction.lengthSquared() > 1.0e-12F) {
        // Numeric translation uses the shared scene unit, just like the grid.
        // GPU-local distances still use the active source's precision scale.
        const double localDistance = numericValue * mScene->mSceneCoordinates.displayScale /
                                     sourceToSceneUnitScale();
        delta = direction.normalized() * static_cast<float>(localDistance);
      }
    }
    if (snap) {
      const float step = transformSnapStep(precision);
      if (mTransformConstraint.kind == TransformConstraintKind::Axis) {
        const float amount = QVector3D::dotProduct(delta, axis);
        delta = axis * (std::round(amount / step) * step);
      } else if (mTransformConstraint.kind == TransformConstraintKind::Plane &&
                 mTransformConstraint.local) {
        QVector3D snapped;
        for (int component = 0; component < 3; ++component) {
          if (component == mTransformConstraint.axis) {
            continue;
          }
          QVector3D localAxis;
          localAxis[component] = 1.0F;
          localAxis = mModelDragStartTransform.rotation.rotatedVector(localAxis)
                          .normalized();
          const float amount = QVector3D::dotProduct(delta, localAxis);
          snapped += localAxis * (std::round(amount / step) * step);
        }
        delta = snapped;
      } else {
        for (int component = 0; component < 3; ++component) {
          delta[component] = std::round(delta[component] / step) * step;
        }
        if (mTransformConstraint.kind == TransformConstraintKind::Plane &&
            mTransformConstraint.axis >= 0) {
          delta[mTransformConstraint.axis] = 0.0F;
        }
      }
    }
    mScene->mModelTranslation = mModelDragStartTransform.translation + delta;
    mScene->mModelRotation = mModelDragStartTransform.rotation;
    mScene->mModelScale = mModelDragStartTransform.scale;
    if (mTransformDragBefore.size() > 1) applyGroupTransform(delta, {}, 1.0F);
  } else if (mMode == InteractionMode::Rotate) {
    QQuaternion deltaRotation;
    if (mTrackballRotation) {
      deltaRotation = trackballRotationDelta(
          mModelTransformStartPosition, position, mModelTransformScreenCenter,
          mModelRotationRadiusPixels, viewMatrix());
      QVector3D rotationAxis;
      float angle = 0.0F;
      deltaRotation.getAxisAndAngle(&rotationAxis, &angle);
      if (numericReady && std::isfinite(numericValue)) {
        angle = static_cast<float>(numericValue);
      } else if (precision) {
        angle *= 0.1F;
      }
      if (snap) {
        const float step = precision ? 1.0F : 5.0F;
        angle = std::round(angle / step) * step;
      }
      if (rotationAxis.lengthSquared() > 1.0e-12F) {
        deltaRotation =
            QQuaternion::fromAxisAndAngle(rotationAxis.normalized(), angle);
      }
    } else {
      QVector3D axis = transformConstraintAxis();
      if (axis.lengthSquared() <= 1.0e-12F) {
        axis = cameraPosition() - pivot;
        if (axis.lengthSquared() <= 1.0e-12F) {
          axis = QVector3D(0.0F, 0.0F, 1.0F);
        }
        axis.normalize();
      }
      float angle = 0.0F;
      if (numericReady && std::isfinite(numericValue)) {
        angle = static_cast<float>(numericValue);
      } else if (mTransformConstraint.kind != TransformConstraintKind::None) {
        angle = axisRotationDragDegrees(
            mModelTransformStartPosition, position, pivot, axis,
            viewProjectionMatrix(), size(), mModelRotationRadiusPixels);
      } else {
        const QPointF start =
            mModelTransformStartPosition - mModelTransformScreenCenter;
        const QPointF current = position - mModelTransformScreenCenter;
        const float startAngle = std::atan2(static_cast<float>(-start.y()),
                                            static_cast<float>(start.x()));
        const float currentAngle = std::atan2(static_cast<float>(-current.y()),
                                              static_cast<float>(current.x()));
        angle = (currentAngle - startAngle) * 180.0F / kPi;
      }
      if (precision) {
        angle *= 0.1F;
      }
      if (snap) {
        const float step = precision ? 1.0F : 5.0F;
        angle = std::round(angle / step) * step;
      }
      deltaRotation = QQuaternion::fromAxisAndAngle(axis, angle);
    }
    mScene->mModelTranslation = mModelDragStartTransform.translation;
    mScene->mModelRotation = normalizedModelRotation(
        deltaRotation * mModelDragStartTransform.rotation);
    mScene->mModelScale = mModelDragStartTransform.scale;
    if (mTransformDragBefore.size() > 1) applyGroupTransform({}, deltaRotation, 1.0F);
  } else if (mMode == InteractionMode::Scale) {
    const QPointF startOffset =
        mModelTransformStartPosition - mModelTransformScreenCenter;
    const QPointF currentOffset = position - mModelTransformScreenCenter;
    const qreal startRadius = std::hypot(startOffset.x(), startOffset.y());
    const qreal currentRadius =
        std::hypot(currentOffset.x(), currentOffset.y());
    float factor = startRadius > 4.0
                       ? static_cast<float>(currentRadius / startRadius)
                       : std::exp(static_cast<float>(
                             (mModelTransformStartPosition.y() - position.y()) /
                             180.0));

    if (mTransformConstraint.kind == TransformConstraintKind::Axis &&
        mTransformConstraint.axis >= 0) {
      const TransformGizmoLayout startLayout = transformGizmoLayout(
          pivot, mModelDragStartTransform.rotation, viewProjectionMatrix(),
          QSizeF(width(), height()), mTransformGizmoMode, modelGizmoWorldRadius());
      const TransformGizmoAxisLayout &axisLayout =
          startLayout.axes[static_cast<std::size_t>(mTransformConstraint.axis)];
      if (startLayout.valid && axisLayout.visible) {
        QPointF direction = axisLayout.scaleEndpoint - startLayout.center;
        const qreal length = std::hypot(direction.x(), direction.y());
        if (length > 1.0) {
          direction /= length;
          const qreal startAmount = startOffset.x() * direction.x() +
                                    startOffset.y() * direction.y();
          const qreal currentAmount = currentOffset.x() * direction.x() +
                                      currentOffset.y() * direction.y();
          if (std::abs(startAmount) > 4.0) {
            factor = static_cast<float>(currentAmount / startAmount);
          }
        }
      }
    }
    if (numericReady && std::isfinite(numericValue)) {
      factor = static_cast<float>(numericValue);
    } else {
      if (precision) {
        factor = 1.0F + (factor - 1.0F) * 0.1F;
      }
      if (snap) {
        const float step = precision ? 0.01F : 0.1F;
        factor = 1.0F + std::round((factor - 1.0F) / step) * step;
      }
    }
    if (!std::isfinite(factor)) {
      factor = 1.0F;
    }
    if (std::abs(factor) < 1.0e-4F) {
      factor = factor < 0.0F ? -1.0e-4F : 1.0e-4F;
    }
    factor = std::clamp(factor, -1.0e4F, 1.0e4F);

    QVector3D factors(1.0F, 1.0F, 1.0F);
    if (mTransformConstraint.kind == TransformConstraintKind::Axis &&
        mTransformConstraint.axis >= 0) {
      factors[mTransformConstraint.axis] = factor;
    } else if (mTransformConstraint.kind == TransformConstraintKind::Plane &&
               mTransformConstraint.axis >= 0) {
      for (int component = 0; component < 3; ++component) {
        if (component != mTransformConstraint.axis) {
          factors[component] = factor;
        }
      }
    } else {
      factors = QVector3D(factor, factor, factor);
    }
    QVector3D scaled(
        mModelDragStartTransform.scale.x() * factors.x(),
        mModelDragStartTransform.scale.y() * factors.y(),
        mModelDragStartTransform.scale.z() * factors.z());
    mScene->mModelTranslation = mModelDragStartTransform.translation;
    mScene->mModelRotation = mModelDragStartTransform.rotation;
    mScene->mModelScale = normalizedModelScale(scaled);
    if (mTransformDragBefore.size() > 1) applyGroupTransform({}, {}, factor);
  }
  notifyModelInteractionState();
  update();
}

void NativeViewport::finishModelTransform(const bool commit) {
  if (!mModelDragActive) {
    return;
  }
  if (!commit) {
    applySceneTransforms(mTransformDragBefore, false);
  }
  mModelDragActive = false;
  mTransformGizmoDragActive = false;
  mTransformGizmoPress = {};
  mTrackballRotation = false;
  mTransformConstraint = {};
  mTransformNumericInput.clear();
  const InteractionMode previousMode = mMode;
  mMode = InteractionMode::Inspect;
  setCursor(defaultInteractionCursor(mMode));
  setToolTip({});
  if (previousMode != mMode) {
    emit interactionModeChanged(mMode);
  }
  if (commit) {
    commitModelTransform();
  }
  mTransformDragBefore.clear();
  notifyModelInteractionState();
  update();
}

void NativeViewport::applyTransformConstraint(const int axis,
                                              const bool plane) {
  if (!mModelDragActive || axis < 0 || axis > 2 || mTrackballRotation) {
    return;
  }
  if (mSelectedSceneIds.size() > 1 && mMode == InteractionMode::Scale) {
    setToolTip(QCoreApplication::translate("Workbench", "多选时使用整体等比缩放，避免对不同朝向的模型产生剪切"));
    return;
  }
  const TransformConstraintKind requested =
      plane && (mMode == InteractionMode::Move ||
                mMode == InteractionMode::Scale)
          ? TransformConstraintKind::Plane
          : TransformConstraintKind::Axis;
  if (mTransformConstraint.kind != requested ||
      mTransformConstraint.axis != axis) {
    mTransformConstraint = {requested, axis,
                            mMode == InteractionMode::Scale};
  } else if (!mTransformConstraint.local) {
    mTransformConstraint.local = true;
  } else {
    mTransformConstraint = {};
  }
  updateModelTransform(mModelTransformCurrentPosition, mTransformModifiers);
}

void NativeViewport::updateTransformNumericInput(const int key,
                                                 const QString &text) {
  if (!mModelDragActive) {
    return;
  }
  if (key == Qt::Key_Backspace) {
    if (!mTransformNumericInput.isEmpty()) {
      mTransformNumericInput.chop(1);
    }
  } else if (key == Qt::Key_Delete) {
    mTransformNumericInput.clear();
  } else if (key == Qt::Key_Minus && mTransformNumericInput.isEmpty()) {
    mTransformNumericInput = QStringLiteral("-");
  } else if ((key == Qt::Key_Period || key == Qt::Key_Comma) &&
             !mTransformNumericInput.contains(QLatin1Char('.'))) {
    mTransformNumericInput += mTransformNumericInput.isEmpty()
                                  ? QStringLiteral("0.")
                                  : QStringLiteral(".");
  } else if (text.size() == 1 && text.front().isDigit()) {
    mTransformNumericInput += text;
  }
  updateModelTransform(mModelTransformCurrentPosition, mTransformModifiers);
}

QString NativeViewport::transformStatusText() const {
  if (!mModelDragActive) {
    return {};
  }
  const QString operation =
      mMode == InteractionMode::Move
          ? QCoreApplication::translate("Workbench", "G 移动")
          : mMode == InteractionMode::Scale
                ? QCoreApplication::translate("Workbench", "S 缩放")
                : mTrackballRotation ? QCoreApplication::translate("Workbench", "R R 轨迹球旋转")
                                     : QCoreApplication::translate("Workbench", "R 旋转");
  QString modifiers;
  if (mTransformModifiers.testFlag(Qt::ControlModifier)) {
    modifiers += QCoreApplication::translate("Workbench", " · 吸附");
  }
  if (mTransformModifiers.testFlag(Qt::ShiftModifier)) {
    modifiers += QCoreApplication::translate("Workbench", " · 精细");
  }
  const QString numeric = mTransformNumericInput.isEmpty()
                              ? QString()
                              : QCoreApplication::translate("Workbench", " · 输入 %1")
                                    .arg(mTransformNumericInput);
  return QCoreApplication::translate("Workbench", "%1 · %2%3%4 · 左键/Enter 确认 · 右键/Esc 取消")
      .arg(operation, transformConstraintLabel(), numeric, modifiers);
}

std::optional<QPointF>
NativeViewport::projectPoint(const QVector3D &point,
                             const QMatrix4x4 &viewProjection) const {
  const QVector4D clip = viewProjection * QVector4D(point, 1.0F);
  if (clip.w() <= 0.0001F) {
    return std::nullopt;
  }
  const QVector3D normalized = clip.toVector3DAffine();
  if (normalized.z() < -1.0F || normalized.z() > 1.0F) {
    return std::nullopt;
  }
  return QPointF((qreal(normalized.x()) * 0.5 + 0.5) * width(),
                 (1.0 - (qreal(normalized.y()) * 0.5 + 0.5)) * height());
}

void NativeViewport::drawDepthAwareLines(
    const QVector<DepthOverlayVertex> &vertices,
    const QMatrix4x4 &viewProjection, const float logicalLineWidth,
    const GLenum primitiveMode, const bool writeDepth) {
  if (vertices.isEmpty() || !mDepthOverlayShaderReady ||
      mDepthOverlayProgram == nullptr ||
      !mDepthOverlayProgram->isLinked() ||
      !mDepthOverlayBuffer.isCreated() ||
      !mDepthOverlayVertexArray.isCreated()) {
    return;
  }

  glEnable(GL_DEPTH_TEST);
  // Physical markers own scene depth. Read-only selection annotations run
  // afterwards and may be coplanar with a mesh without suppressing its pixels.
  glDepthFunc(primitiveMode == GL_TRIANGLES ? GL_ALWAYS : writeDepth ? GL_LESS : GL_LEQUAL);
  glDepthMask(writeDepth ? GL_TRUE : GL_FALSE);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glEnable(GL_MULTISAMPLE);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                      GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glLineWidth(std::max(
      1.0F, logicalLineWidth * static_cast<float>(devicePixelRatioF())));

  mDepthOverlayProgram->bind();
  mDepthOverlayProgram->setUniformValue("viewProjection", viewProjection);
  {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(
        &mDepthOverlayVertexArray);
    mDepthOverlayBuffer.bind();
    const qsizetype vertexBytes =
        vertices.size() * static_cast<qsizetype>(sizeof(DepthOverlayVertex));
    mDepthOverlayBuffer.allocate(vertices.constData(),
                                 static_cast<int>(vertexBytes));
    glDrawArrays(primitiveMode, 0, static_cast<GLsizei>(vertices.size()));
    mDepthOverlayBuffer.release();
  }
  mDepthOverlayProgram->release();
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_LESS);
  glLineWidth(1.0F);
  glDisable(GL_BLEND);
}

void NativeViewport::drawDepthAwareModelBounds(
    const QMatrix4x4 &modelViewProjection) {
  if (mEditToolsLocked || !mModelSelected || !selectableModelAvailable()) {
    return;
  }

  const QVector3D minimum = modelBoundsMinimum();
  const QVector3D maximum = modelBoundsMaximum();
  std::array<QVector3D, 8> corners;
  std::array<std::optional<QPointF>, 8> projected;
  for (int corner = 0; corner < 8; ++corner) {
    corners[static_cast<std::size_t>(corner)] = QVector3D(
        (corner & 1) != 0 ? maximum.x() : minimum.x(),
        (corner & 2) != 0 ? maximum.y() : minimum.y(),
        (corner & 4) != 0 ? maximum.z() : minimum.z());
    projected[static_cast<std::size_t>(corner)] = projectPoint(
        corners[static_cast<std::size_t>(corner)], modelViewProjection);
  }
  constexpr std::array<std::array<int, 2>, 12> edges = {
      std::array<int, 2>{0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
      {2, 6},              {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7}};
  constexpr qreal dashPixels = 8.0;
  constexpr qreal gapPixels = 5.0;
  constexpr int maximumDashesPerEdge = 256;
  const QVector3D color = lightViewport()
                            ? QVector3D(0.65F, 0.38F, 0.07F)
                            : QVector3D(1.0F, 166.0F / 255.0F, 64.0F / 255.0F);

  QVector<DepthOverlayVertex> vertices;
  vertices.reserve(384);
  const float alpha = mModelDragActive ? 0.9F : 0.68F;
  const auto appendLine = [&vertices, &color, alpha](
                              const QVector3D &start,
                              const QVector3D &end) {
    vertices.append({start.x(), start.y(), start.z(), color.x(), color.y(),
                     color.z(), alpha});
    vertices.append({end.x(), end.y(), end.z(), color.x(), color.y(),
                     color.z(), alpha});
  };
  for (const auto &edge : edges) {
    const QVector3D &start = corners[static_cast<std::size_t>(edge[0])];
    const QVector3D &end = corners[static_cast<std::size_t>(edge[1])];
    const auto &screenStart = projected[static_cast<std::size_t>(edge[0])];
    const auto &screenEnd = projected[static_cast<std::size_t>(edge[1])];
    if (!screenStart.has_value() || !screenEnd.has_value()) {
      continue;
    }
    const qreal screenLength = QLineF(*screenStart, *screenEnd).length();
    if (!std::isfinite(screenLength) || screenLength <= 0.5) {
      continue;
    }
    const QVector3D delta = end - start;
    if (!mModelDragActive) {
      // Preserve the truthful full-data bounds, but show only compact corner
      // brackets while idle.  A large scan can have a very loose AABB; full
      // edge lines then dominate the scene even though the box itself is no
      // longer a click target.
      constexpr qreal cornerPixels = 26.0;
      constexpr qreal maximumCornerFraction = 0.2;
      constexpr qreal minimumMiddleGapPixels = 10.0;
      const qreal bracketPixels =
          std::min(cornerPixels, screenLength * maximumCornerFraction);
      if (screenLength <= bracketPixels * 2.0 + minimumMiddleGapPixels) {
        appendLine(start, end);
      } else {
        const float bracketRatio =
            static_cast<float>(bracketPixels / screenLength);
        appendLine(start, start + delta * bracketRatio);
        appendLine(end - delta * bracketRatio, end);
      }
      continue;
    }
    const qreal basePattern = dashPixels + gapPixels;
    const qreal pattern =
        std::max(basePattern,
                 screenLength / static_cast<qreal>(maximumDashesPerEdge));
    const qreal dash = pattern * (dashPixels / basePattern);
    for (qreal offset = 0.0; offset < screenLength; offset += pattern) {
      const qreal dashEnd = std::min(offset + dash, screenLength);
      const float startRatio = static_cast<float>(offset / screenLength);
      const float endRatio = static_cast<float>(dashEnd / screenLength);
      appendLine(start + delta * startRatio, start + delta * endRatio);
    }
  }
  drawDepthAwareLines(vertices, modelViewProjection,
                      mModelDragActive ? 1.8F : 1.35F, GL_LINES, false);
}

void NativeViewport::drawCameraTrajectory(QPainter &painter,
                                          const QMatrix4x4 &viewProjection) {
  if (!mScene->visible || !mShowCameras || !camerasAvailable()) {
    return;
  }

  const auto drawSegments = [this, &painter, &viewProjection](
                                const QList<CameraLineSegment> &segments) {
    QList<QLineF> projectedLines;
    projectedLines.reserve(segments.size());
    for (const CameraLineSegment &segment : segments) {
      const auto start = projectPoint(segment.start, viewProjection);
      const auto end = projectPoint(segment.end, viewProjection);
      if (start.has_value() && end.has_value()) {
        projectedLines.append(QLineF(*start, *end));
      }
    }
    if (!projectedLines.isEmpty()) {
      painter.drawLines(projectedLines);
    }
  };

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(themeInk(QColor(109, 160, 255, 205),
                               QColor(43, 103, 190, 220)), 1.8, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  drawSegments(mScene->mCameraGeometry.path);
  painter.setPen(QPen(themeInk(QColor(84, 209, 122, 220),
                               QColor(31, 139, 77, 225)), 1.25, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  drawSegments(mScene->mCameraGeometry.frustums);
  painter.restore();
}

QString NativeViewport::selectionHintText() const {
  return mModelDragActive ? transformStatusText()
      : mSelectedSceneIds.size() > 1
          ? QCoreApplication::translate("Workbench", "已选中 %1 个模型 · G 整体移动 · R 共同中心旋转 · S 等比缩放")
                .arg(mSelectedSceneIds.size())
          : QCoreApplication::translate("Workbench", "模型已选中 · G 移动 · R 旋转 · R R 轨迹球 · S 缩放");
}

QRectF NativeViewport::selectionHintBounds() const {
  if (mEditToolsLocked || !mModelSelected || !selectableModelAvailable() ||
      !projectPoint(selectionPivot(), viewProjectionMatrix()).has_value()) return {};
  const QFontMetrics metrics(font());
  const QRect textBounds = metrics.boundingRect(selectionHintText()).adjusted(-7, -4, 7, 4);
  return transformGizmoHintRect(modelTransformGizmo(), QSizeF(textBounds.size()),
                                QSizeF(width(), height()));
}

void NativeViewport::drawModelSelection(
    QPainter &painter, const QMatrix4x4 &modelViewProjection) {
  if (mEditToolsLocked || !mModelSelected || !selectableModelAvailable()) {
    return;
  }

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  Q_UNUSED(modelViewProjection);
  const ViewportColors colors = viewportColors();
  const auto center = projectPoint(selectionPivot(), viewProjectionMatrix());
  if (center.has_value()) {
    if (mModelDragActive && mMode == InteractionMode::Rotate) {
      painter.setPen(QPen(themeInk(QColor(255, 173, 66, 205),
                                   QColor(168, 102, 18, 225)), 1.7,
                          Qt::DashLine, Qt::RoundCap));
      painter.setBrush(Qt::NoBrush);
      painter.drawEllipse(*center, mModelRotationRadiusPixels,
                          mModelRotationRadiusPixels);
    }
    if (mModelDragActive &&
        mTransformConstraint.kind != TransformConstraintKind::None) {
      const QVector3D pivot =
          mTransformDragBefore.size() > 1 ? mGroupPivot :
          mScene->mSceneCenter + mModelDragStartTransform.translation;
      const QVector3D axis = transformConstraintAxis();
      const float extent = std::max(mScene->mSceneRadius * 3.0F, 1.0F);
      const auto start =
          projectPoint(pivot - axis * extent, viewProjectionMatrix());
      const auto end =
          projectPoint(pivot + axis * extent, viewProjectionMatrix());
      if (start.has_value() && end.has_value()) {
        painter.setPen(QPen(navigationAxisColor(mTransformConstraint.axis),
                            2.2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(*start, *end);
      }
    }
    painter.setPen(QPen(themeInk(QColor(255, 187, 84, 245),
                                 QColor(167, 98, 12, 245)), 1.5));
    painter.setBrush(themeInk(QColor(255, 166, 64, 72),
                              QColor(195, 131, 28, 50)));
    const qreal pivotRadius = modelTransformGizmo().centerHandle.width() * 0.3;
    painter.drawEllipse(*center, pivotRadius, pivotRadius);
    const QString hint = selectionHintText();
    const QRectF badge = selectionHintBounds();
    painter.setPen(overlayBorder());
    painter.setBrush(overlaySurface(colors.hintCard, 225));
    painter.drawRoundedRect(badge, 5.0, 5.0);
    painter.setPen(colors.hintText);
    painter.drawText(badge, Qt::AlignCenter, hint);
  }
  painter.restore();
}

void NativeViewport::drawModelTransformGizmo(QPainter &painter) {
  if (mEditToolsLocked) return;
  const auto layout = modelTransformGizmo();
  if (!layout.valid) return;
  const auto colors = viewportColors();
  TransformGizmoStyle style;
  style.axes = {navigationAxisColor(0), navigationAxisColor(1), navigationAxisColor(2)};
  style.highlight = colors.highlight;
  style.neutral = colors.viewRing;
  style.centerEdge = colors.handleEdge;
  style.light = lightViewport();
  paintTransformGizmo(painter, layout, mTransformGizmoMode, style,
                        mTransformGizmoPress.isValid() ? mTransformGizmoPress : mTransformGizmoHover,
                        mTransformGizmoDragActive || mModelDragActive);
}

void NativeViewport::drawTransformToolStrip(QPainter &painter) {
  if (mEditToolsLocked || !selectableModelAvailable()) {
    return;
  }
  const TransformToolStripLayout layout = transformToolStrip();
  static const std::array<TransformGizmoMode, 4> modes = {
      TransformGizmoMode::Move, TransformGizmoMode::Rotate,
      TransformGizmoMode::Scale, TransformGizmoMode::Transform};
  static const std::array<WorkbenchIconId, 4> icons = {
      WorkbenchIconId::Move, WorkbenchIconId::Rotate,
      WorkbenchIconId::Scale, WorkbenchIconId::Transform};
  const ViewportColors colors = viewportColors();
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  if (layout.background.isEmpty()) {
    painter.restore();
    return;
  }
  const qreal scale = layout.buttons.front().width() / 34.0;
  const qreal rounding = 4.0 * scale;
  painter.setPen(QPen(colors.toolBorder, scale));
  painter.setBrush(colors.toolCard);
  painter.drawRoundedRect(layout.background, rounding + scale,
                          rounding + scale);

  const auto drawSeparator = [&](const QRectF &first, const QRectF &second) {
    painter.setPen(QPen(colors.toolBorder, scale));
    if (std::abs(first.center().y() - second.center().y()) < scale) {
      const qreal x = (first.right() + second.left()) * 0.5;
      painter.drawLine(QPointF(x, first.top() + 8.0 * scale),
                       QPointF(x, first.bottom() - 8.0 * scale));
    } else if (std::abs(first.center().x() - second.center().x()) < scale) {
      const qreal y = (first.bottom() + second.top()) * 0.5;
      painter.drawLine(QPointF(first.left() + 5.0 * scale, y),
                       QPointF(first.right() - 5.0 * scale, y));
    }
    // Wrapped-row boundaries need no rule through neighbouring controls.
  };
  drawSeparator(layout.buttons[2], layout.buttons[3]);
  drawSeparator(layout.buttons[3], layout.orientationButton);

  const auto iconRect = [scale](const QRectF &button) {
    const qreal size = 20.0 * scale;
    return QRectF(button.center() - QPointF(size * 0.5, size * 0.5),
                  QSizeF(size, size));
  };

  for (int index = 0; index < 4; ++index) {
    const QRectF rect = layout.buttons[static_cast<std::size_t>(index)];
    const bool active =
        mTransformGizmoMode == modes.at(static_cast<std::size_t>(index));
    const bool hover = mTransformToolHover == index;
    if (active || hover) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(active ? colors.toolActive : colors.toolHover);
      painter.drawRoundedRect(rect, rounding, rounding);
    }
    paintWorkbenchIcon(painter, icons.at(static_cast<std::size_t>(index)),
                       iconRect(rect),
                       active ? QColor(255, 255, 255) : colors.toolIcon);
  }

  const bool locked = modelGizmoOrientationLocked();
  const bool local = modelGizmoUsesLocalOrientation();
  if (!locked && mTransformToolHover == 4) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.toolHover);
    painter.drawRoundedRect(layout.orientationButton, rounding, rounding);
  }
  const bool uniformScale = locked && mSelectedSceneIds.size() > 1;
  paintWorkbenchIcon(painter,
                     uniformScale ? WorkbenchIconId::Scale
                                  : local ? WorkbenchIconId::Local
                                          : WorkbenchIconId::Global,
                     iconRect(layout.orientationButton),
                     locked ? colors.disabledText : colors.toolIcon);
  if (locked) {
    // The small lock badge communicates the current constraint; the existing
    // translated hover text explains why this coordinate control is disabled.
    const qreal badge = 10.0 * scale;
    const QRectF badgeRect(layout.orientationButton.right() - badge - scale,
                           layout.orientationButton.bottom() - badge - scale,
                           badge, badge);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.toolCard);
    painter.drawRoundedRect(badgeRect.adjusted(-scale, -scale, scale, scale),
                            2.0 * scale, 2.0 * scale);
    paintWorkbenchIcon(painter, WorkbenchIconId::Locked, badgeRect,
                       colors.disabledText);
  }
  painter.restore();
}

void NativeViewport::drawSelectionGesture(QPainter &painter) {
  const bool drawBrushCursor =
      mMode == InteractionMode::Brush && mBrushCursorVisible;
  if (!mSelectionGestureActive && !drawBrushCursor) {
    return;
  }

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  const ViewportColors colors = viewportColors();
  const QColor brushEdge = themeInk(QColor(124, 219, 191), QColor(17, 117, 97));
  if (mMode == InteractionMode::Brush) {
    if (mSelectionGestureActive && !mSelectionPath.isEmpty()) {
      QPainterPath stroke;
      stroke.moveTo(mSelectionPath.first());
      for (qsizetype index = 1; index < mSelectionPath.size(); ++index) {
        stroke.lineTo(mSelectionPath.at(index));
      }
      stroke.lineTo(mSelectionCurrent);
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(translucent(colors.accent, 48), mBrushRadius * 2.0,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawPath(stroke);
      painter.setPen(QPen(translucent(brushEdge, 205), 1.2, Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
      painter.drawPath(stroke);
    }
    if (drawBrushCursor) {
      painter.setBrush(translucent(colors.accent, 20));
      painter.setPen(QPen(translucent(brushEdge, 220), 1.4));
      painter.drawEllipse(mBrushCursorPosition, mBrushRadius, mBrushRadius);
    }
    painter.restore();
    return;
  }

  painter.setPen(QPen(colors.accent, 1.5, Qt::SolidLine));
  painter.setBrush(translucent(colors.accent, 36));
  if (mMode == InteractionMode::Rectangle) {
    painter.drawRect(QRectF(mSelectionStart, mSelectionCurrent).normalized());
  } else if (mMode == InteractionMode::Lasso && !mSelectionPath.isEmpty()) {
    QPainterPath path;
    path.moveTo(mSelectionPath.first());
    for (qsizetype index = 1; index < mSelectionPath.size(); ++index) {
      path.lineTo(mSelectionPath.at(index));
    }
    path.lineTo(mSelectionCurrent);
    path.closeSubpath();
    painter.drawPath(path);
  }
  painter.restore();
}

void NativeViewport::drawOverlay(QPainter &painter) {
  const ViewportColors colors = viewportColors();
  painter.save();
  painter.setPen(overlayBorder());
  painter.setBrush(overlaySurface(colors.card, 225));

  const QString sceneName = (trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame())
                                ? mScene->mRenderMode == RenderMode::Points
                                      ? QCoreApplication::translate("Workbench", "训练中 · 点云增密预览")
                                      : QCoreApplication::translate("Workbench", "训练中 · GPU 实时预览")
                                : mScene->mScenePath.isEmpty()
                                      ? QCoreApplication::translate("Workbench", "未载入场景")
                                      : QFileInfo(mScene->mScenePath).fileName();
  const QString project =
      mProjectLabel.isEmpty() ? QCoreApplication::translate("Workbench", "未打开工程") : mProjectLabel;
  QString count;
  if ((trainingGpuPreviewForScene() && mTrainingGpuPreview.hasFrame())) {
    count = mScene->mRenderMode == RenderMode::Points
                ? QCoreApplication::translate("Workbench", "迭代 %1 | %2 高斯中心（点云）| 共享 GPU 显存")
                      .arg(mTrainingGpuPreview.iteration())
                      .arg(formatCount(mTrainingGpuPreview.pointCount()))
                : QCoreApplication::translate("Workbench", "迭代 %1 | %2 高斯 | 共享 GPU 显存")
                      .arg(mTrainingGpuPreview.iteration())
                      .arg(formatCount(mTrainingGpuPreview.pointCount()));
  } else if (mScene->previewLoadBusy) {
    count = QCoreApplication::translate("Workbench", "正在读取 PLY 场景...");
  } else if (mScene->previewLoadFailed) {
    count = QCoreApplication::translate("Workbench", "预览更新失败 · 继续显示上一结果");
  } else if (!mScene->mSceneLoadMessage.isEmpty()) {
    count = AppLanguage::diagnosticText(mScene->mSceneLoadMessage);
  } else if (pagedMeshAvailable()) {
    count = QCoreApplication::translate("Workbench", "%1 顶点 | %2 面 | %3 三角形 | %4 | 当前绘制 %5 | "
                "GPU LOD 缓存 %6 | 只读")
                .arg(formatCount(mScene->mMeshCache.fullVertexCount),
                     formatCount(mScene->mMeshCache.fullFaceCount),
                     formatCount(mScene->mMeshCache.renderableTriangleCount),
                     mScene->mProgressiveUploadActive
                         ? QCoreApplication::translate("Workbench", "磁盘分页")
                         : mInteractionLodActive
                               ? QCoreApplication::translate("Workbench", "网格分页预览")
                               : QCoreApplication::translate("Workbench", "叶级细节"),
                     formatCount(mScene->mDrawnFullResolutionMeshTriangleCount),
                     formatCount(mScene->mUploadedFullResolutionMeshTriangleCount));
    if (!mScene->mMeshCacheError.isEmpty()) {
      count += QCoreApplication::translate("Workbench", " | 分页错误");
    }
  } else if (mScene->mPreviewOnlyScene) {
    count = QCoreApplication::translate("Workbench", "%1 点 | %2 | GPU 驻留 %3 / %4 | 只读")
                .arg(formatCount(mScene->mPreviewPointCount),
                     mScene->mProgressiveUploadActive
                         ? QCoreApplication::translate("Workbench", "磁盘分页")
                         : mInteractionLodActive
                               ? QCoreApplication::translate("Workbench", "八叉树 LOD")
                               : QCoreApplication::translate("Workbench", "叶级细节"),
                     formatCount(mScene->mUploadedFullResolutionPointCount),
                     formatCount(mPointCacheGpuBudgetBytes /
                                 sizeof(PointPreviewVertex)));
    if (!mScene->mPointCacheError.isEmpty()) {
      count += QCoreApplication::translate("Workbench", " | 分页错误");
    }
  } else if (mScene->mSourceFaceCount > 0 && mScene->mPreviewTriangleCount > 0) {
    count = QCoreApplication::translate("Workbench", "%1 顶点 | %2 面 | %3 预览三角形")
                .arg(formatCount(mScene->mSourceVertexCount),
                     formatCount(mScene->mSourceFaceCount),
                     formatCount(mScene->mPreviewTriangleCount));
  } else if (mScene->mGaussianCount > 0 && mScene->mPreviewPointCount > 0 &&
             mScene->mGaussianCount != mScene->mPreviewPointCount) {
    count = QCoreApplication::translate("Workbench", "%1 %2 | 预览 %3")
                .arg(formatCount(mScene->mGaussianCount),
                     mScene->mHasGaussianAttributes ? QCoreApplication::translate("Workbench", "高斯")
                                            : QCoreApplication::translate("Workbench", "点"),
                     formatCount(mScene->mPreviewPointCount));
  } else if (mScene->mGaussianCount > 0) {
    count = QStringLiteral("%1 %2").arg(
        formatCount(mScene->mGaussianCount),
        mScene->mHasGaussianAttributes ? QCoreApplication::translate("Workbench", "高斯") : QCoreApplication::translate("Workbench", "点"));
  } else {
    count = QCoreApplication::translate("Workbench", "场景数据待载入");
  }
  if (mScene->mHasMesh) {
    if (mScene->mMeshTextureReady && mScene->mMeshTextureSize.isValid()) {
      count += QCoreApplication::translate("Workbench", " | 贴图 %1 · %2×%3")
                   .arg(QFileInfo(mScene->mMeshTexturePath).fileName())
                   .arg(mScene->mMeshTextureSize.width())
                   .arg(mScene->mMeshTextureSize.height());
    } else if (mScene->mMeshHasTextureCoordinates) {
      count += QCoreApplication::translate("Workbench", " | UV · 基础颜色回退");
    } else {
      count += QCoreApplication::translate("Workbench", " | 基础颜色");
    }
    if (!mScene->mMeshTextureError.isEmpty()) {
      count += QCoreApplication::translate("Workbench", " | 贴图警告");
    }
  }
  if (mShowCameras) {
    count += QCoreApplication::translate("Workbench", " | 相机 %1%2")
                 .arg(formatCount(cameraCount()),
                      mScene->mCameraGeometry.decimated ? QCoreApplication::translate("Workbench", "（抽稀）")
                                                : QString());
  }
  if (mScene->mSelectionBusy) {
    count += QCoreApplication::translate("Workbench", " | 正在计算选择");
  } else if (mScene->mEditModel.selectedCount() > 0 || mScene->mEditModel.deletedCount() > 0) {
    count += QCoreApplication::translate("Workbench", " | 已选 %1 | 已删 %2")
                 .arg(formatCount(mScene->mEditModel.selectedCount()),
                      formatCount(mScene->mEditModel.deletedCount()));
  }
  QFont compactFont = font();
  if (compactFont.pointSizeF() > 0.0) {
    compactFont.setPointSizeF(
        (std::max)(8.0, compactFont.pointSizeF() - 1.0));
  } else if (compactFont.pixelSize() > 0) {
    compactFont.setPixelSize((std::max)(11, compactFont.pixelSize() - 1));
  }
  painter.setFont(compactFont);
  const QFontMetrics metrics(compactFont);
  const int lineHeight = metrics.height();
  const int viewportMargin = 8;
  const int headerPaddingX = 9;
  const int headerPaddingY = 5;
  const int lineGap = 1;
  const QString mode =
      mEditToolsLocked ? QCoreApplication::translate("Workbench", "工具已锁定") :
      mScene->mSelectionBusy ? QCoreApplication::translate("Workbench", "选择处理中") : modeLabel(mMode);
  const int modeWidth = qRound(navigationGizmo().rotateBounds.width());
  const QString compactCount = count + QStringLiteral(" · ") + mode;
  const QString title = QStringLiteral("%1  ·  %2").arg(project, sceneName);
  const int widthHint =
      (std::max)(metrics.horizontalAdvance(title),
                 metrics.horizontalAdvance(compactCount)) +
      headerPaddingX * 2;
  const int headerHeight =
      headerPaddingY * 2 + lineHeight * 2 + lineGap;
  const int headerMaxWidth =
      qMax(1, width() - modeWidth - viewportMargin * 4);
  const int headerMinWidth = (std::min)(160, headerMaxWidth);
  const QRect headerRect(
      viewportMargin, viewportMargin,
      std::clamp(widthHint, headerMinWidth, headerMaxWidth), headerHeight);
  // Blender-style unobtrusive viewport captions: no permanent card, border or
  // background wash. A one-pixel shadow keeps text readable over actual models.
  QFont strongFont = compactFont;
  strongFont.setWeight(QFont::DemiBold);
  painter.setFont(strongFont);
  QRect lineRect(headerRect.left() + headerPaddingX,
                 headerRect.top() + headerPaddingY,
                 headerRect.width() - headerPaddingX * 2, lineHeight);
  drawViewportCaption(painter,
      lineRect, Qt::AlignLeft | Qt::AlignVCenter,
      QFontMetrics(strongFont).elidedText(title, Qt::ElideMiddle,
                                          lineRect.width()), colors.text);
  painter.setFont(compactFont);
  painter.setPen(colors.accent);
  lineRect.translate(0, lineHeight + lineGap);
  drawViewportCaption(painter, lineRect, Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(compactCount, Qt::ElideRight, lineRect.width()), colors.muted);

  if (!mProcessingStage.isEmpty()) {
    const QString phase = processingPreviewLabel();
    const QString detail = processingPreviewDetail();
    const int panelWidth = std::min(width() - viewportMargin * 2,
        std::max(metrics.horizontalAdvance(phase), metrics.horizontalAdvance(detail)) + headerPaddingX * 2);
    const QRect panel(viewportMargin, headerRect.bottom() + 6, panelWidth, headerHeight + 3);
    painter.setPen(overlayBorder());
    painter.setBrush(overlaySurface(colors.processCard, 225));
    painter.drawRoundedRect(panel, 5, 5);
    QRect row = panel.adjusted(headerPaddingX, headerPaddingY, -headerPaddingX, -headerPaddingY);
    row.setHeight(lineHeight);
    painter.setPen(colors.processText);
    painter.drawText(row, Qt::AlignVCenter, metrics.elidedText(phase, Qt::ElideRight, row.width()));
    row.translate(0, lineHeight + lineGap);
    painter.setPen(colors.processDetail);
    painter.drawText(row, Qt::AlignVCenter, metrics.elidedText(detail, Qt::ElideRight, row.width()));
    if (mProcessingActive && mProcessingProgress >= 0) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(colors.progress);
      painter.drawRect(QRect(panel.left(), panel.bottom() - 2,
          panel.width() * std::clamp(mProcessingProgress, 0, 100) / 100, 2));
    }
  }

  painter.restore();
}

void NativeViewport::drawAxisGizmo(QPainter &painter) {
  QJsonObject diagnostics;
  const bool recording = mRenderDiagnostics && mRenderDiagnostics->recording();
  drawBlenderNavigationPresentation(painter, navigationGizmo(),
      {.hover = mNavigationHover,
       .press = mNavigationPress,
       .interactionActive = mNavigationInteractionActive,
       .light = lightViewport(),
       .orthographic = mOrthographic,
       .cameraActive = mCameraViewActive,
       .cameraAvailable = camerasAvailable(),
       .backdrop = viewportColors().backdrop}, recording ? &diagnostics : nullptr);
  if (recording) mRenderDiagnostics->note(QStringLiteral("navigation-presentation"), diagnostics);
}

} // namespace gsw

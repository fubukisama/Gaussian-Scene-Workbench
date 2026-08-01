#include "NativeViewport.h"

#include "NavigationGizmo.h"
#include "ScreenSpaceSelection.h"
#include "ViewportCamera.h"

#include <QApplication>
#include <QDebug>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QFutureWatcher>
#include <QLineF>
#include <QMouseEvent>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <QPainterPath>
#include <QSizePolicy>
#include <QTimer>
#include <QVariantAnimation>
#include <QVector2D>
#include <QVector4D>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace gsw {

namespace {
constexpr float kPi = 3.14159265358979323846F;

struct ReferenceGridFrame final {
  QVector3D normal;
  QVector3D axisU;
  QVector3D axisV;
  QVector3D axisUColor;
  QVector3D axisVColor;
};

ReferenceGridFrame referenceGridFrame(const ReferenceGridPlane plane) {
  const QVector3D xColor(0.72F, 0.26F, 0.26F);
  const QVector3D yColor(0.25F, 0.42F, 0.76F);
  const QVector3D zColor(0.38F, 0.68F, 0.25F);
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
  static const std::array<QColor, 3> colors = {
      QColor(226, 67, 67), QColor(62, 116, 232), QColor(104, 185, 57)};
  return colors[static_cast<std::size_t>(axisIndex)];
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
    return QStringLiteral("查看");
  case NativeViewport::InteractionMode::Select:
    return QStringLiteral("选择");
  case NativeViewport::InteractionMode::Rectangle:
    return QStringLiteral("框选");
  case NativeViewport::InteractionMode::Lasso:
    return QStringLiteral("套索");
  case NativeViewport::InteractionMode::Brush:
    return QStringLiteral("笔刷");
  case NativeViewport::InteractionMode::Crop:
    return QStringLiteral("裁剪");
  }
  return {};
}

bool isTrimInteractionMode(const NativeViewport::InteractionMode mode) {
  switch (mode) {
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
  setObjectName(QStringLiteral("nativeViewport"));
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setMinimumSize(0, 0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

  mViewSnapAnimation = new QVariantAnimation(this);
  mViewSnapAnimation->setDuration(220);
  mViewSnapAnimation->setEasingCurve(QEasingCurve::OutCubic);
  connect(mViewSnapAnimation, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            const QPointF angles = value.toPointF();
            mYawDegrees = static_cast<float>(angles.x());
            mPitchDegrees = static_cast<float>(angles.y());
            update();
          });
  connect(mViewSnapAnimation, &QVariantAnimation::finished, this, [this]() {
    mYawDegrees = std::remainder(mYawDegrees, 360.0F);
    if (mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()) {
      rebuildRenderedVertices();
    }
    update();
  });
  mTrainingGpuPreviewTimer = new QTimer(this);
  mTrainingGpuPreviewTimer->setTimerType(Qt::PreciseTimer);
  mTrainingGpuPreviewTimer->setInterval(0);
  connect(mTrainingGpuPreviewTimer, &QTimer::timeout, this,
          QOverload<>::of(&NativeViewport::update));
  mFrameRefreshTimer = new QTimer(this);
  mFrameRefreshTimer->setTimerType(Qt::PreciseTimer);
  mFrameRefreshTimer->setInterval(0);
  connect(mFrameRefreshTimer, &QTimer::timeout, this,
          QOverload<>::of(&NativeViewport::update));
  connect(this, &QOpenGLWidget::frameSwapped, this, [this]() {
    if (mRenderedPointCount <= 0 && mFullResolutionPointCount <= 0 &&
        mRenderedMeshIndexCount <= 0 &&
        mFullResolutionMeshTriangleCount <= 0 &&
        !mTrainingGpuPreview.attached()) {
      return;
    }
    mFrameRateCounter.frameCompleted(FrameRateCounter::Clock::now());
    emit frameMetricsChanged(mFrameRateCounter.framesPerSecond(),
                             mFrameRateCounter.averageFrameMilliseconds());
  });
}

NativeViewport::~NativeViewport() {
  if (context() != nullptr && context()->isValid()) {
    makeCurrent();
    mTrainingGpuPreview.release();
    releaseFullResolutionPointCloud();
    releaseFullResolutionMesh();
    releaseMeshTexture();
    mGridVertexArray.destroy();
    mGaussianVertexArray.destroy();
    mMeshVertexArray.destroy();
    mPointVertexArray.destroy();
    mMeshIndexBuffer.destroy();
    mMeshVertexBuffer.destroy();
    mPointBuffer.destroy();
    doneCurrent();
  }
}

void NativeViewport::setTrainingGpuPreviewDescriptor(
    const TrainingGpuPreviewDescriptor &descriptor) {
  mPendingTrainingGpuPreviewDescriptor = descriptor;
  mTrainingGpuPreviewStopPending = false;
  if (descriptor.state == TrainingGpuPreviewState::Ready) {
    mTrainingGpuPreviewCameraFramed = false;
    mTrainingGpuPreviewTimer->start();
  }
  update();
}

void NativeViewport::stopTrainingGpuPreview() {
  mPendingTrainingGpuPreviewDescriptor.reset();
  mTrainingGpuPreviewStopPending = true;
  mTrainingGpuPreviewCameraFramed = false;
  update();
}

void NativeViewport::setProjectLabel(const QString &label) {
  mProjectLabel = label;
  update();
}

void NativeViewport::setScene(const QString &scenePath,
                              const qint64 gaussianCount) {
  mGaussianCount = gaussianCount;
  if (mRequestedScenePath == scenePath) {
    reloadCameraTrajectory(scenePath, false);
    update();
    return;
  }

  ++mSceneGeneration;
  if (mSelectionBusy) {
    mSelectionBusy = false;
    emit selectionBusyChanged(false);
  }
  mSelectionGestureActive = false;
  mSelectionPath.clear();
  mBrushCursorVisible = false;
  mTemporaryOrbitActive = false;
  mRequestedScenePath = scenePath;
  mScenePath = scenePath;
  mSourceFaceCount = 0;
  mPreviewPointCount = 0;
  mPreviewTriangleCount = 0;
  mRenderedPointCount = 0;
  mFullResolutionPointCount = 0;
  mUploadedFullResolutionPointCount = 0;
  mFullResolutionMeshTriangleCount = 0;
  mUploadedFullResolutionMeshTriangleCount = 0;
  mDrawnFullResolutionMeshTriangleCount = 0;
  mRenderedMeshIndexCount = 0;
  updateFrameRefreshPolicy();
  const bool gaussianAvailabilityChanged = gaussianRenderingAvailable();
  const bool meshAvailabilityChanged = meshRenderingAvailable();
  const bool renderModeChangedToPoints = mRenderMode != RenderMode::Points;
  mHasGaussianAttributes = false;
  mHasMesh = false;
  mPreviewOnlyScene = false;
  mRenderMode = RenderMode::Points;
  const bool hadSceneCoordinates = mSceneCoordinates.valid;
  mSceneCoordinates = {};
  mSceneLoadMessage.clear();
  mSourcePositions.clear();
  mSourcePositions.squeeze();
  mPreviewVertices.clear();
  mPreviewVertices.squeeze();
  mPendingVertices.clear();
  mPendingVertices.squeeze();
  mPointCache = {};
  mDesiredPointCacheNodes.clear();
  mPointCacheReadsInFlight.clear();
  mPointCacheFailedNodes.clear();
  mPendingPointCachePages.clear();
  mPendingPointCachePages.squeeze();
  mPointCacheError.clear();
  mFullResolutionPointClearPending = true;
  mMeshCache = {};
  mDesiredMeshCacheNodes.clear();
  mMeshCacheReadsInFlight.clear();
  mMeshCacheFailedNodes.clear();
  mPendingMeshCachePages.clear();
  mPendingMeshCachePages.squeeze();
  mMeshCacheError.clear();
  mFullResolutionMeshClearPending = true;
  mMeshTexturePath.clear();
  mMeshTextureError.clear();
  mPendingMeshTexture = {};
  mMeshTextureSize = {};
  mMeshHasTextureCoordinates = false;
  mMeshTextureUploadPending = false;
  mMeshTextureClearPending = true;
  mMeshTextureReady = false;
  mPendingMeshVertices.clear();
  mPendingMeshVertices.squeeze();
  mPendingMeshIndices.clear();
  mPendingMeshIndices.squeeze();
  mSceneCenter = QVector3D(0.0F, 0.0F, 0.0F);
  mSceneRadius = 4.0F;
  mEditModel.reset(0);
  mPointUploadPending = true;
  mMeshUploadPending = true;
  notifyEditState();
  if (gaussianAvailabilityChanged) {
    emit gaussianRenderingAvailabilityChanged(false);
  }
  if (meshAvailabilityChanged) {
    emit meshRenderingAvailabilityChanged(false);
  }
  if (renderModeChangedToPoints) {
    emit renderModeChanged(mRenderMode);
  }
  if (hadSceneCoordinates) {
    emit sceneCoordinatesChanged();
  }
  reloadCameraTrajectory(scenePath, true);
  resetCamera();
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

void NativeViewport::setInteractionMode(const InteractionMode mode) {
  mMode = mode;
  mSelectionGestureActive = false;
  mSelectionPath.clear();
  mBrushCursorVisible = false;
  mTemporaryOrbitActive = false;
  setCursor(defaultInteractionCursor(mode));
  update();
}

void NativeViewport::setRenderMode(const RenderMode mode) {
  if (mode == RenderMode::Gaussians && !gaussianRenderingAvailable()) {
    return;
  }
  if (mode == RenderMode::Mesh && !meshRenderingAvailable()) {
    return;
  }
  if (mRenderMode == mode) {
    return;
  }
  mRenderMode = mode;
  rebuildRenderedVertices();
  emit renderModeChanged(mRenderMode);
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
  mViewSnapAnimation->stop();
  mTarget = mSceneCenter;
  mYawDegrees = 42.0F;
  mPitchDegrees = 24.0F;
  mDistance = clampViewportDistance(
      std::max(mSceneRadius * 2.8F, 0.1F), mSceneRadius);
  mOrthographic = false;
  mCameraViewActive = false;
  mStoredCameraView.reset();
  if (!mPreviewVertices.isEmpty()) {
    rebuildRenderedVertices();
  }
  update();
}

void NativeViewport::clearSelection() {
  if (mSelectionBusy) {
    return;
  }
  mEditModel.clearSelection();
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

void NativeViewport::invertSelection() {
  if (mSelectionBusy || !hasEditableScene()) {
    return;
  }
  mEditModel.invertSelection();
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

void NativeViewport::deleteSelection() {
  if (mSelectionBusy || mEditModel.deleteSelection() == 0) {
    return;
  }
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

void NativeViewport::undoEdit() {
  if (mSelectionBusy || mEditModel.undo() == 0) {
    return;
  }
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

void NativeViewport::redoEdit() {
  if (mSelectionBusy || mEditModel.redo() == 0) {
    return;
  }
  rebuildRenderedVertices();
  notifyEditState();
  update();
}

bool NativeViewport::saveCroppedScene(const QString &filePath,
                                      QString *errorMessage) {
  if (!PlyPointCloudLoader::writeFiltered(
          mScenePath, filePath, mEditModel.deletedBits(), errorMessage)) {
    return false;
  }
  mEditModel.markExported();
  notifyEditState();
  return true;
}

bool NativeViewport::hasUnsavedSceneEdits() const {
  return mEditModel.hasUnsavedChanges();
}

bool NativeViewport::hasEditableScene() const {
  return !mPreviewOnlyScene && !mHasMesh && !mScenePath.isEmpty() &&
         mEditModel.pointCount() > 0;
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
  return mReferencePlaneMode == ReferencePlaneMode::ModelBase &&
                 mSceneCoordinates.valid
             ? mSceneCoordinates.globalMinimum.z
             : 0.0;
}

QString NativeViewport::referencePlaneDescription() const {
  if (mReferencePlaneMode == ReferencePlaneMode::WorldZero ||
      !mSceneCoordinates.valid) {
    return QStringLiteral("世界坐标 Z=0");
  }
  return QStringLiteral("模型底部 Z=%1")
      .arg(formatSceneCoordinate(mSceneCoordinates.globalMinimum.z,
                                 mSceneCoordinates));
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
  return (mHasGaussianAttributes || mTrainingGpuPreview.attached()) &&
         mGaussianShaderReady;
}

bool NativeViewport::pagedMeshAvailable() const {
  return mMeshCache.formatVersion == MeshCacheIndex::CurrentFormatVersion &&
         mMeshCache.rootNode >= 0 &&
         mMeshCache.rootNode < mMeshCache.nodes.size() &&
         mMeshCache.nodes.at(mMeshCache.rootNode).isValid();
}

bool NativeViewport::meshRenderingAvailable() const {
  return mHasMesh && mMeshShaderReady &&
         (mRenderedMeshIndexCount > 0 || pagedMeshAvailable());
}

bool NativeViewport::infiniteGridRenderingAvailable() const {
  return mGridShaderReady;
}

bool NativeViewport::camerasAvailable() const {
  return !mCameraTrajectory.cameras().isEmpty();
}

qsizetype NativeViewport::cameraCount() const {
  return mCameraTrajectory.cameras().size();
}

void NativeViewport::initializeGL() {
  initializeOpenGLFunctions();
  constexpr GLenum kGpuMemoryTotalAvailableNvx = 0x9048;
  if (QOpenGLContext::currentContext()->hasExtension(
          QByteArrayLiteral("GL_NVX_gpu_memory_info"))) {
    GLint totalKilobytes = 0;
    glGetIntegerv(kGpuMemoryTotalAvailableNvx, &totalKilobytes);
    if (totalKilobytes > 0) {
      const qsizetype quarterBytes =
          static_cast<qsizetype>(totalKilobytes) * 1024 / 4;
      mPointCacheGpuBudgetBytes = std::clamp<qsizetype>(
          quarterBytes, 512LL * 1024LL * 1024LL,
          2048LL * 1024LL * 1024LL);
      mMeshCacheGpuBudgetBytes = mPointCacheGpuBudgetBytes;
    }
  }
  bool budgetOverrideValid = false;
  const int budgetOverrideMegabytes = qEnvironmentVariableIntValue(
      "GSW_POINT_CACHE_GPU_BUDGET_MB", &budgetOverrideValid);
  if (budgetOverrideValid && budgetOverrideMegabytes >= 64 &&
      budgetOverrideMegabytes <= 4096) {
    mPointCacheGpuBudgetBytes =
        static_cast<qsizetype>(budgetOverrideMegabytes) * 1024LL * 1024LL;
  }
  bool meshBudgetOverrideValid = false;
  const int meshBudgetOverrideMegabytes = qEnvironmentVariableIntValue(
      "GSW_MESH_CACHE_GPU_BUDGET_MB", &meshBudgetOverrideValid);
  if (meshBudgetOverrideValid && meshBudgetOverrideMegabytes >= 64 &&
      meshBudgetOverrideMegabytes <= 4096) {
    mMeshCacheGpuBudgetBytes =
        static_cast<qsizetype>(meshBudgetOverrideMegabytes) * 1024LL * 1024LL;
  }
  glClearColor(0.047F, 0.051F, 0.055F, 1.0F);
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
out vec4 fragmentColor;
void main() {
  float radialDistance = length(gl_PointCoord - vec2(0.5)) * 2.0;
  if (radialDistance > 1.0) {
    discard;
  }
  float alpha = 1.0 - smoothstep(0.72, 1.0, radialDistance);
  fragmentColor = vec4(vertexColor, alpha);
}
)GLSL");
  const bool pointShaderReady =
      vertexCompiled && fragmentCompiled && mPointProgram->link();
  if (!pointShaderReady) {
    mSceneLoadMessage = QStringLiteral("OpenGL point shader failed: %1")
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
out vec3 vertexColor;
out vec3 worldPosition;
out vec3 worldNormal;
out vec2 vertexTextureCoordinate;
out float vertexTextureWeight;
void main() {
  gl_Position = viewProjection * vec4(position, 1.0);
  vertexColor = color;
  worldPosition = position;
  worldNormal = normal;
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
  fragmentColor = vec4(litColor, 1.0);
}
)GLSL");
  mMeshShaderReady = meshVertexCompiled && meshFragmentCompiled &&
                     mMeshProgram->link();
  if (!mMeshShaderReady && mSceneLoadMessage.isEmpty()) {
    mSceneLoadMessage = QStringLiteral("OpenGL mesh shader failed: %1")
                            .arg(mMeshProgram->log());
  }

  mGaussianProgram = new QOpenGLShaderProgram(this);
  const bool gaussianVertexCompiled =
      mGaussianProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                R"GLSL(#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 2) in float opacity;
layout(location = 3) in vec3 scale;
layout(location = 4) in vec4 rotation;

uniform mat4 view;
uniform mat4 projection;
uniform vec2 viewportPixels;

out vec3 vertexColor;
out float vertexOpacity;
out vec2 gaussianCoordinate;

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

  if (cameraCenter.z >= -1e-4 || clipCenter.w <= 0.0 ||
      vertexOpacity <= (1.0 / 255.0)) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vertexOpacity = 0.0;
    return;
  }

  vec3 safeScale = clamp(scale, vec3(1e-8), vec3(1e6));
  mat3 worldRotation = rotationMatrix(rotation);
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
  vec3 jacobianX = vec3(focalX / depth, 0.0,
                        focalX * cameraCenter.x / (depth * depth));
  vec3 jacobianY = vec3(0.0, focalY / depth,
                        focalY * cameraCenter.y / (depth * depth));
  float covarianceXX =
      dot(jacobianX, cameraCovariance * jacobianX) + 0.09;
  float covarianceXY = dot(jacobianX, cameraCovariance * jacobianY);
  float covarianceYY =
      dot(jacobianY, cameraCovariance * jacobianY) + 0.09;

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
                                                R"GLSL(#version 330 core
in vec3 vertexColor;
in float vertexOpacity;
in vec2 gaussianCoordinate;
out vec4 fragmentColor;
void main() {
  float power = -0.5 * dot(gaussianCoordinate, gaussianCoordinate);
  float alpha = vertexOpacity * exp(power);
  if (alpha < (1.0 / 255.0)) {
    discard;
  }
  fragmentColor = vec4(vertexColor * alpha, alpha);
}
)GLSL");
  mGaussianShaderReady = pointShaderReady && gaussianVertexCompiled &&
                         gaussianFragmentCompiled && mGaussianProgram->link();
  if (!mGaussianShaderReady && mSceneLoadMessage.isEmpty()) {
    mSceneLoadMessage = QStringLiteral("OpenGL Gaussian shader failed: %1")
                            .arg(mGaussianProgram->log());
  }

  // Follow Blender's current overlay approach: emit actual line primitives
  // from gl_VertexID and let the multisampled framebuffer rasterize them. A
  // fullscreen periodic fragment shader cannot keep phase precision near the
  // perspective horizon and turns shallow lines into a comb of short dashes.
  // Perspective uses the fixed Z-up ground plane; orthographic axis views use
  // the matching world plane so the scale remains visible in all six views.
  mGridProgram = new QOpenGLShaderProgram(this);
  const bool gridVertexCompiled =
      mGridProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                            R"GLSL(#version 130
uniform mat4 viewProjection;
uniform vec3 gridPlaneOrigin;
uniform vec3 gridAxisU;
uniform vec3 gridAxisV;
uniform vec2 gridCenter;
uniform float gridStep;
uniform float gridHalfSpan;
uniform int gridLineCount;
uniform int drawAxis;
out float lineCoverage;

void main() {
  vec3 world;
  if (drawAxis == 1 || drawAxis == 2) {
    int vertexInLine = gl_VertexID;
    float endpoint = vertexInLine == 0
                         ? -gridHalfSpan
                         : (vertexInLine == 3 ? gridHalfSpan : 0.0);
    float crossCoordinate;
    if (drawAxis == 1) {
      world = gridPlaneOrigin +
              gridAxisU * (gridCenter.x + endpoint);
      crossCoordinate = gridCenter.y;
    } else {
      world = gridPlaneOrigin +
              gridAxisV * (gridCenter.y + endpoint);
      crossCoordinate = gridCenter.x;
    }
    float crossFade = 1.0 - smoothstep(
        gridHalfSpan * 0.72, gridHalfSpan, abs(crossCoordinate));
    float alongFade =
        (vertexInLine == 0 || vertexInLine == 3) ? 0.0 : 1.0;
    lineCoverage = crossFade * alongFade;
  } else {
    // Two GL_LINES segments form each logical line: edge->centre and
    // centre->edge. This gives the rasterizer a continuous alpha ramp at the
    // finite level boundary rather than exposing an abrupt rectangular edge.
    int lineIndex = gl_VertexID / 4;
    int vertexInLine = gl_VertexID - lineIndex * 4;
    float endpoint = vertexInLine == 0
                         ? -gridHalfSpan
                         : (vertexInLine == 3 ? gridHalfSpan : 0.0);
    int halfCount = gridLineCount / 2;
    float offset;
    if (lineIndex < gridLineCount) {
      offset = float(lineIndex - halfCount) * gridStep;
      world = gridPlaneOrigin +
              gridAxisU * (gridCenter.x + endpoint) +
              gridAxisV * (gridCenter.y + offset);
    } else {
      offset = float(lineIndex - gridLineCount - halfCount) * gridStep;
      world = gridPlaneOrigin +
              gridAxisU * (gridCenter.x + offset) +
              gridAxisV * (gridCenter.y + endpoint);
    }
    float offsetFade = 1.0 - smoothstep(
        gridHalfSpan * 0.72, gridHalfSpan, abs(offset));
    float alongFade =
        (vertexInLine == 0 || vertexInLine == 3) ? 0.0 : 1.0;
    lineCoverage = offsetFade * alongFade;
  }
  gl_Position = viewProjection * vec4(world, 1.0);
  // The reference plane is a background overlay and must not disappear when
  // an orthographic pan places world zero just beyond the scene far clip.
  // Preserve projected XY/W, but keep the line inside the clip-depth range;
  // depth testing and writes are disabled for this pass.
  gl_Position.z = 0.0;
}
)GLSL");
  const bool gridFragmentCompiled =
      mGridProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                            R"GLSL(#version 130
out vec4 fragmentColor;
uniform vec4 lineColor;
in float lineCoverage;

void main() {
  fragmentColor = vec4(lineColor.rgb, lineColor.a * lineCoverage);
}
)GLSL");
  mGridShaderReady =
      gridVertexCompiled && gridFragmentCompiled && mGridProgram->link();
  if (!mGridShaderReady && mSceneLoadMessage.isEmpty()) {
    mSceneLoadMessage =
        QStringLiteral("OpenGL reference-grid shader failed: %1")
            .arg(mGridProgram->log());
  }
  if (mGridShaderReady) {
    mGridVertexArray.create();
  }

  mTrainingGpuPreviewCapability =
      TrainingGpuPreviewBuffer::probe(QOpenGLContext::currentContext());

  if (pointShaderReady) {
    mPointBuffer.create();
    mPointBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);

    mPointVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mPointVertexArray);
    mPointBuffer.bind();
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
    mPointBuffer.release();
  }

  if (mMeshShaderReady) {
    mMeshVertexBuffer.create();
    mMeshVertexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
    mMeshIndexBuffer.create();
    mMeshIndexBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);

    mMeshVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mMeshVertexArray);
    mMeshVertexBuffer.bind();
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
    mMeshVertexBuffer.release();
  }

  if (pointShaderReady && mGaussianShaderReady) {
    mGaussianVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mGaussianVertexArray);
    mPointBuffer.bind();
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
    mPointBuffer.release();
  }

  if (meshRenderingAvailable()) {
    emit meshRenderingAvailabilityChanged(true);
    if (mRenderMode != RenderMode::Mesh) {
      mRenderMode = RenderMode::Mesh;
      emit renderModeChanged(mRenderMode);
    }
  } else if (gaussianRenderingAvailable()) {
    emit gaussianRenderingAvailabilityChanged(true);
    if (mRenderMode != RenderMode::Gaussians) {
      mRenderMode = RenderMode::Gaussians;
      rebuildRenderedVertices();
      emit renderModeChanged(mRenderMode);
    }
  }
}

void NativeViewport::resizeGL(const int width, const int height) {
  const qreal ratio = devicePixelRatioF();
  glViewport(0, 0, qRound(width * ratio), qRound(height * ratio));
}

void NativeViewport::paintGL() {
  applyPendingTrainingGpuPreview();
  const bool gaussianWasAvailable = gaussianRenderingAvailable();
  QString gpuPreviewError;
  const bool previewChanged =
      mTrainingGpuPreview.poll(&gpuPreviewError);
  synchronizeGaussianRenderingAvailability(gaussianWasAvailable);
  if (!gpuPreviewError.isEmpty() &&
      gpuPreviewError != mTrainingGpuPreviewError) {
    mTrainingGpuPreviewError = gpuPreviewError;
    emit trainingGpuPreviewStateChanged(
        mTrainingGpuPreview.attached(), QStringLiteral("GPU 共享显存"),
        gpuPreviewError);
  }
  if (previewChanged && mTrainingGpuPreview.attached()) {
    if (mTrainingGpuPreview.hasFrame()) {
      const QVector3D previewCenter = mTrainingGpuPreview.sceneCenter();
      const float previewRadius = mTrainingGpuPreview.sceneRadius();
      if (std::isfinite(previewCenter.x()) &&
          std::isfinite(previewCenter.y()) &&
          std::isfinite(previewCenter.z()) && std::isfinite(previewRadius) &&
          previewRadius > 0.0F) {
        mSceneCenter = previewCenter;
        mSceneRadius = std::max(previewRadius, 1.0e-4F);
        rebuildCameraGeometry();
        if (!mTrainingGpuPreviewCameraFramed) {
          mTrainingGpuPreviewCameraFramed = true;
          resetCamera();
        }
      }
    }
    mTrainingGpuPreviewError.clear();
    emit trainingGpuPreviewStateChanged(
        true, QStringLiteral("GPU 共享显存 · 零 CPU 拷贝"),
        QStringLiteral("迭代 %1 · %2 个高斯")
            .arg(mTrainingGpuPreview.iteration())
            .arg(mTrainingGpuPreview.pointCount()));
  } else if (previewChanged && !mTrainingGpuPreview.attached()) {
    mTrainingGpuPreviewTimer->stop();
    emit trainingGpuPreviewStateChanged(
        false, QStringLiteral("PLY 检查点回退"),
        QStringLiteral("共享显存预览已结束"));
  }
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  uploadPendingPointCloud();
  uploadPendingMeshTexture();
  uploadPendingMesh();

  const QMatrix4x4 view = viewMatrix();
  const QMatrix4x4 projection = projectionMatrix();
  const QMatrix4x4 viewProjection = projection * view;
  updatePointCacheSelection(viewProjection);
  uploadPendingPointCachePages();
  updateMeshCacheSelection(viewProjection);
  uploadPendingMeshCachePages();
  drawInfiniteGrid(viewProjection);
  if (mRenderMode == RenderMode::Mesh && meshRenderingAvailable()) {
    drawMesh(viewProjection);
  } else if (mTrainingGpuPreview.hasFrame() &&
             mRenderMode == RenderMode::Points) {
    drawTrainingPointCloud(viewProjection);
  } else if (mRenderMode == RenderMode::Gaussians &&
             gaussianRenderingAvailable()) {
    drawGaussianCloud(view, projection);
  } else {
    drawPointCloud(viewProjection);
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  drawReferenceAxes(painter, viewProjection);
  drawCameraTrajectory(painter, viewProjection);
  drawSelectionGesture(painter);

  drawOverlay(painter);
  drawAxisGizmo(painter);
  painter.end();
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
          false, QStringLiteral("PLY 检查点回退"),
          QStringLiteral("训练任务已停止"));
    }
  }
  if (!mPendingTrainingGpuPreviewDescriptor.has_value()) {
    return;
  }
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
            : QStringLiteral("训练共享显存发布器已关闭");
    if (wasAttached || descriptor.state == TrainingGpuPreviewState::Failed) {
      emit trainingGpuPreviewStateChanged(
          false, QStringLiteral("PLY 检查点回退"), detail);
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
        false, QStringLiteral("PLY 检查点回退"), error);
    return;
  }
  synchronizeGaussianRenderingAvailability(gaussianWasAvailable);
  mTrainingGpuPreviewError.clear();
  mTrainingGpuPreviewCameraFramed = false;
  if (mRenderMode != RenderMode::Gaussians) {
    mRenderMode = RenderMode::Gaussians;
    emit renderModeChanged(mRenderMode);
  }
  emit trainingGpuPreviewStateChanged(
      true, QStringLiteral("GPU 共享显存 · 零 CPU 拷贝"),
      QStringLiteral("已连接 %1，等待首帧").arg(descriptor.device));
}

void NativeViewport::reloadCameraTrajectory(const QString &scenePath,
                                            const bool clearExisting) {
  const int generation = ++mCameraTrajectoryGeneration;
  if (clearExisting || scenePath.isEmpty()) {
    mCameraTrajectory = {};
    rebuildCameraGeometry();
    emit cameraTrajectoryChanged(0, 0, false, QString(), QString());
  }
  if (scenePath.isEmpty()) {
    return;
  }

  auto *watcher = new QFutureWatcher<CameraTrajectory>(this);
  connect(watcher, &QFutureWatcher<CameraTrajectory>::finished, this,
          [this, watcher, scenePath, generation]() {
            CameraTrajectory trajectory = watcher->result();
            watcher->deleteLater();
            if (generation != mCameraTrajectoryGeneration ||
                scenePath != mRequestedScenePath) {
              return;
            }
            mCameraTrajectory = std::move(trajectory);
            rebuildCameraGeometry();
            emit cameraTrajectoryChanged(
                cameraCount(), mCameraTrajectory.invalidCameraCount(),
                mCameraGeometry.decimated, mCameraTrajectory.sourcePath(),
                mCameraTrajectory.error());
            update();
          });
  watcher->setFuture(QtConcurrent::run(
      [scenePath]() { return CameraTrajectory::loadForScene(scenePath); }));
}

void NativeViewport::rebuildCameraGeometry() {
  mCameraGeometry = mCameraTrajectory.geometry(mSceneRadius);
  if (!mSceneCoordinates.valid ||
      (!mSceneCoordinates.automaticDisplayShift &&
       mSceneCoordinates.displayScale == 1.0)) {
    return;
  }
  const auto transformSegments = [this](QList<CameraLineSegment> &segments) {
    for (CameraLineSegment &segment : segments) {
      segment.start = mSceneCoordinates.localFromGlobal(
          {segment.start.x(), segment.start.y(), segment.start.z()});
      segment.end = mSceneCoordinates.localFromGlobal(
          {segment.end.x(), segment.end.y(), segment.end.z()});
    }
  };
  transformSegments(mCameraGeometry.frustums);
  transformSegments(mCameraGeometry.path);
}

void NativeViewport::startSceneLoad(const QString &scenePath) {
  mSceneLoadMessage = QStringLiteral("正在读取 PLY 场景...");
  emit sceneLoadStarted(scenePath);
  const int generation = mSceneGeneration;

  auto *watcher = new QFutureWatcher<PointCloudData>(this);
  connect(watcher, &QFutureWatcher<PointCloudData>::finished, this,
          [this, watcher, scenePath, generation]() {
            PointCloudData data = watcher->result();
            watcher->deleteLater();
            if (scenePath != mRequestedScenePath ||
                generation != mSceneGeneration) {
              return;
            }
            if (!data.isValid()) {
              mSceneLoadMessage = data.error;
              emit sceneLoadFailed(scenePath, data.error);
              update();
              return;
            }

            mSceneCoordinates = data.coordinates;
            mSceneCenter = data.center();
            mTarget = mSceneCenter;
            mSceneRadius = data.radius();
            rebuildCameraGeometry();
            const bool loadedHasMesh = data.hasMesh();
            mPreviewPointCount = data.previewPointCount();
            mSourceFaceCount = data.sourceFaceCount;
            mPreviewTriangleCount = data.meshCache.isValid()
                                        ? static_cast<qsizetype>(
                                              std::min<qint64>(
                                                  data.meshCache
                                                      .renderableTriangleCount,
                                                  std::numeric_limits<
                                                      qsizetype>::max()))
                                        : data.meshIndices.size() / 3;
            mRenderedMeshIndexCount = data.meshIndices.size();
            mSourcePositions = std::move(data.sourcePositions);
            mPreviewVertices = std::move(data.vertices);
            mPreviewOnlyScene = data.previewOnly;
            mPointCache = std::move(data.pointCache);
            mMeshCache = std::move(data.meshCache);
            mMeshTexturePath = std::move(data.meshTexturePath);
            mMeshTextureError = std::move(data.meshTextureError);
            mPendingMeshTexture = std::move(data.meshTextureImage);
            mMeshHasTextureCoordinates =
                data.meshHasTextureCoordinates ||
                mMeshCache.hasTextureCoordinates;
            mMeshTextureUploadPending =
                mMeshHasTextureCoordinates && !mPendingMeshTexture.isNull();
            mMeshTextureClearPending = true;
            mMeshTextureReady = false;
            if (!mMeshTextureError.isEmpty()) {
              qWarning().noquote()
                  << QStringLiteral("[mesh-texture] %1")
                         .arg(mMeshTextureError);
            }
            mDesiredPointCacheNodes.clear();
            mPointCacheReadsInFlight.clear();
            mPointCacheFailedNodes.clear();
            mPendingPointCachePages.clear();
            mFullResolutionPointCount =
                mPointCache.isValid() ? mPreviewPointCount : 0;
            mUploadedFullResolutionPointCount = 0;
            mFullResolutionPointClearPending = true;
            mDesiredMeshCacheNodes.clear();
            mMeshCacheReadsInFlight.clear();
            mMeshCacheFailedNodes.clear();
            mPendingMeshCachePages.clear();
            mFullResolutionMeshTriangleCount =
                pagedMeshAvailable()
                    ? static_cast<qsizetype>(std::min<qint64>(
                          mMeshCache.renderableTriangleCount,
                          std::numeric_limits<qsizetype>::max()))
                    : 0;
            mUploadedFullResolutionMeshTriangleCount = 0;
            mFullResolutionMeshClearPending = true;
            mPendingMeshVertices = std::move(data.meshVertices);
            mPendingMeshIndices = std::move(data.meshIndices);
            mMeshUploadPending = true;
            mHasMesh = loadedHasMesh;
            mEditModel.reset(
                mHasMesh || mPreviewOnlyScene ? 0 : mSourcePositions.size());
            const bool wasAvailable = gaussianRenderingAvailable();
            // The out-of-core cache intentionally stores only exact XYZ/RGB
            // centers. Do not expose Gaussian mode until scale, rotation, and
            // opacity also have a paged representation.
            mHasGaussianAttributes =
                data.hasGaussianAttributes && !mPreviewOnlyScene;
            const bool isAvailable = gaussianRenderingAvailable();
            if (isAvailable != wasAvailable) {
              emit gaussianRenderingAvailabilityChanged(isAvailable);
            }
            const RenderMode loadedMode =
                mHasMesh && mMeshShaderReady
                    ? RenderMode::Mesh
                    : isAvailable ? RenderMode::Gaussians : RenderMode::Points;
            if (mRenderMode != loadedMode) {
              mRenderMode = loadedMode;
              emit renderModeChanged(mRenderMode);
            }
            mSceneLoadMessage.clear();
            resetCamera();
            updateFrameRefreshPolicy();
            notifyEditState();
            emit meshRenderingAvailabilityChanged(meshRenderingAvailable());
            emit sceneLoaded(data.sourceVertexCount, mPreviewPointCount,
                             data.sourceFaceCount, mPreviewTriangleCount);
            emit sceneCoordinatesChanged();
          });
  watcher->setFuture(QtConcurrent::run(
      [scenePath]() { return PlyPointCloudLoader::load(scenePath); }));
}

void NativeViewport::startSelection(const ScreenSelectionRequest &request,
                                    const SelectionOperation operation) {
  if (mSelectionBusy || !hasEditableScene()) {
    return;
  }

  mSelectionBusy = true;
  emit selectionBusyChanged(true);
  notifyEditState();
  update();

  const int generation = mSceneGeneration;
  const QVector<PointPosition> positions = mSourcePositions;
  const QBitArray deleted = mEditModel.deletedBits();
  const QMatrix4x4 viewProjection = viewProjectionMatrix();
  const QSize viewportSize = size();
  ScreenSelectionRequest selection = request;
  selection.visibleOnly = mVisibleOnlySelection;

  auto *watcher = new QFutureWatcher<QVector<quint32>>(this);
  connect(watcher, &QFutureWatcher<QVector<quint32>>::finished, this,
          [this, watcher, generation, operation]() {
            const QVector<quint32> matches = watcher->result();
            watcher->deleteLater();
            if (generation != mSceneGeneration) {
              return;
            }
            mSelectionBusy = false;
            mEditModel.applySelection(matches, operation);
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
  mPendingVertices.clear();
  mPendingVertices.reserve(mPreviewVertices.size());
  const QBitArray &selected = mEditModel.selectedBits();
  const QBitArray &deleted = mEditModel.deletedBits();
  for (const PointCloudVertex &sourceVertex : mPreviewVertices) {
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
    mPendingVertices.append(renderedVertex);
  }
  if (mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()) {
    const QVector3D forward = (mTarget - cameraPosition()).normalized();
    std::sort(mPendingVertices.begin(), mPendingVertices.end(),
              [&forward](const PointCloudVertex &left,
                         const PointCloudVertex &right) {
                const float leftDepth = left.x * forward.x() +
                                        left.y * forward.y() +
                                        left.z * forward.z();
                const float rightDepth = right.x * forward.x() +
                                         right.y * forward.y() +
                                         right.z * forward.z();
                if (leftDepth == rightDepth) {
                  return left.sourceIndex < right.sourceIndex;
                }
                return leftDepth > rightDepth;
              });
  }
  mRenderedPointCount = mPendingVertices.size();
  mPointUploadPending = true;
  updateFrameRefreshPolicy();
}

void NativeViewport::updateFrameRefreshPolicy() {
  const bool shouldRefreshContinuously =
      mRenderedPointCount > 0 || !mPendingPointCachePages.isEmpty() ||
      mRenderedMeshIndexCount > 0 || mMeshTextureUploadPending ||
      !mPendingMeshCachePages.isEmpty() ||
      !mMeshCacheReadsInFlight.isEmpty() ||
      !mFullResolutionMeshGpuChunks.isEmpty();
  if (shouldRefreshContinuously && !mFrameRefreshTimer->isActive()) {
    mFrameRefreshTimer->start();
  } else if (!shouldRefreshContinuously && mFrameRefreshTimer->isActive()) {
    mFrameRefreshTimer->stop();
  }
}

void NativeViewport::notifyEditState() {
  emit editStateChanged(mEditModel.selectedCount(), mEditModel.deletedCount(),
                        mEditModel.canUndo(), mEditModel.canRedo(),
                        hasEditableScene(), mEditModel.hasUnsavedChanges());
}

void NativeViewport::uploadPendingPointCloud() {
  if (!mPointUploadPending || !mPointBuffer.isCreated()) {
    return;
  }
  QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mPointVertexArray);
  mPointBuffer.bind();
  const qsizetype byteCount = mPendingVertices.size() *
                              static_cast<qsizetype>(sizeof(PointCloudVertex));
  mPointBuffer.allocate(
      mPendingVertices.isEmpty() ? nullptr : mPendingVertices.constData(),
      static_cast<int>(byteCount));
  mPointBuffer.release();
  mPendingVertices.clear();
  mPointUploadPending = false;
}

void NativeViewport::releaseFullResolutionPointCloud() {
  for (const FullResolutionGpuChunk &chunk :
       std::as_const(mFullResolutionPointGpuChunks)) {
    if (chunk.buffer != 0) {
      glDeleteBuffers(1, &chunk.buffer);
    }
    if (chunk.vertexArray != 0) {
      glDeleteVertexArrays(1, &chunk.vertexArray);
    }
  }
  mFullResolutionPointGpuChunks.clear();
  mPointCacheResidentBytes = 0;
  mUploadedFullResolutionPointCount = 0;
}

void NativeViewport::updatePointCacheSelection(
    const QMatrix4x4 &viewProjection) {
  if (!mPreviewOnlyScene || !mPointCache.isValid()) {
    mDesiredPointCacheNodes.clear();
    return;
  }
  ++mPointCacheFrameSerial;
  const bool interacting =
      mPressedButtons != Qt::NoButton || mNavigationInteractionActive ||
      (mViewSnapAnimation != nullptr &&
       mViewSnapAnimation->state() == QAbstractAnimation::Running);
  const float refinementThresholdPixels = interacting ? 140.0F : 28.0F;
  const qsizetype pointBudget = static_cast<qsizetype>(
      (static_cast<long double>(mPointCacheGpuBudgetBytes) * 0.85L) /
      sizeof(PointPreviewVertex));
  const QVector3D eye = cameraPosition();
  const float viewportHeight =
      std::max(1.0F, static_cast<float>(height() * devicePixelRatioF()));
  const auto projectedPixels = [&](const PointCloudCacheNode &node) {
    const QVector3D center =
        (node.boundsMinimum + node.boundsMaximum) * 0.5F;
    const float radius =
        std::max((node.boundsMaximum - node.boundsMinimum).length() * 0.5F,
                 mSceneRadius * 1.0e-6F);
    if (mOrthographic) {
      return radius / std::max(mDistance, 1.0e-5F) * viewportHeight * 2.0F;
    }
    const float distance =
        std::max((center - eye).length() - radius, mSceneRadius * 1.0e-5F);
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
      mPointCache.nodes.at(mPointCache.rootNode);
  qsizetype selectedPoints = 0;
  if (visible(root)) {
    selected.insert(root.id);
    refinable.append(root.id);
    selectedPoints = static_cast<qsizetype>(root.pointCount);
  }
  while (!refinable.isEmpty()) {
    qsizetype bestPosition = -1;
    float bestPriority = refinementThresholdPixels;
    for (qsizetype position = 0; position < refinable.size(); ++position) {
      const PointCloudCacheNode &candidate =
          mPointCache.nodes.at(refinable.at(position));
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
    const PointCloudCacheNode &node = mPointCache.nodes.at(nodeId);
    QVector<int> visibleChildren;
    qsizetype childPoints = 0;
    for (const int childId : node.children) {
      const PointCloudCacheNode &child = mPointCache.nodes.at(childId);
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
      if (!mPointCache.nodes.at(childId).isLeaf()) {
        refinable.append(childId);
      }
    }
  }
  mDesiredPointCacheNodes = std::move(selected);
  requestMissingPointCachePages();
}

void NativeViewport::requestMissingPointCachePages() {
  if (!mPointCache.isValid()) {
    return;
  }
  const auto isResident = [this](const int nodeId) {
    return std::any_of(mFullResolutionPointGpuChunks.cbegin(),
                       mFullResolutionPointGpuChunks.cend(),
                       [nodeId](const FullResolutionGpuChunk &chunk) {
                         return chunk.nodeId == nodeId;
                       });
  };
  QSet<int> needed;
  for (const int desired : std::as_const(mDesiredPointCacheNodes)) {
    int nodeId = desired;
    while (nodeId >= 0 && !isResident(nodeId)) {
      if (!mPointCacheReadsInFlight.contains(nodeId) &&
          !mPointCacheFailedNodes.contains(nodeId)) {
        needed.insert(nodeId);
      }
      nodeId = mPointCache.nodes.at(nodeId).parent;
    }
  }
  QVector<int> ordered = needed.values();
  std::sort(ordered.begin(), ordered.end(), [this](const int left,
                                                   const int right) {
    const PointCloudCacheNode &a = mPointCache.nodes.at(left);
    const PointCloudCacheNode &b = mPointCache.nodes.at(right);
    if (a.depth != b.depth) {
      return a.depth < b.depth;
    }
    return a.pointCount < b.pointCount;
  });
  constexpr qsizetype kMaximumReadsInFlight = 2;
  for (const int nodeId : std::as_const(ordered)) {
    if (mPointCacheReadsInFlight.size() >= kMaximumReadsInFlight) {
      break;
    }
    mPointCacheReadsInFlight.insert(nodeId);
    const int generation = mSceneGeneration;
    const PointCloudCacheIndex cache = mPointCache;
    auto *watcher = new QFutureWatcher<PointCloudCachePage>(this);
    connect(watcher, &QFutureWatcher<PointCloudCachePage>::finished, this,
            [this, watcher, generation, nodeId]() {
              PointCloudCachePage page = watcher->result();
              watcher->deleteLater();
              if (generation != mSceneGeneration) {
                return;
              }
              mPointCacheReadsInFlight.remove(nodeId);
              if (page.isValid()) {
                mPendingPointCachePages.append(std::move(page));
              } else {
                mPointCacheFailedNodes.insert(nodeId);
                mPointCacheError = page.error;
              }
              updateFrameRefreshPolicy();
              update();
            });
    watcher->setFuture(QtConcurrent::run(
        [cache, nodeId]() { return PointCloudCache::readNode(cache, nodeId); }));
  }
}

void NativeViewport::evictPointCacheUntilFits(const qsizetype requiredBytes) {
  const auto isResident = [this](const int nodeId) {
    return std::any_of(mFullResolutionPointGpuChunks.cbegin(),
                       mFullResolutionPointGpuChunks.cend(),
                       [nodeId](const FullResolutionGpuChunk &chunk) {
                         return chunk.nodeId == nodeId;
                       });
  };
  QSet<int> protectedNodes;
  for (const int desired : std::as_const(mDesiredPointCacheNodes)) {
    int nodeId = desired;
    while (nodeId >= 0) {
      if (isResident(nodeId)) {
        protectedNodes.insert(nodeId);
        break;
      }
      nodeId = mPointCache.nodes.at(nodeId).parent;
    }
  }
  while (!mFullResolutionPointGpuChunks.isEmpty() &&
         mPointCacheResidentBytes + requiredBytes >
             mPointCacheGpuBudgetBytes) {
    qsizetype victim = -1;
    quint64 oldest = std::numeric_limits<quint64>::max();
    for (qsizetype index = 0;
         index < mFullResolutionPointGpuChunks.size(); ++index) {
      const FullResolutionGpuChunk &chunk =
          mFullResolutionPointGpuChunks.at(index);
      if (!protectedNodes.contains(chunk.nodeId) &&
          chunk.lastUsedFrame < oldest) {
        victim = index;
        oldest = chunk.lastUsedFrame;
      }
    }
    if (victim < 0) {
      for (qsizetype index = 0;
           index < mFullResolutionPointGpuChunks.size(); ++index) {
        const FullResolutionGpuChunk &chunk =
            mFullResolutionPointGpuChunks.at(index);
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
        mFullResolutionPointGpuChunks.takeAt(victim);
    if (chunk.buffer != 0) {
      glDeleteBuffers(1, &chunk.buffer);
    }
    if (chunk.vertexArray != 0) {
      glDeleteVertexArrays(1, &chunk.vertexArray);
    }
    mPointCacheResidentBytes -= chunk.byteCount;
  }
}

void NativeViewport::uploadPendingPointCachePages() {
  if (mFullResolutionPointClearPending) {
    releaseFullResolutionPointCloud();
    mFullResolutionPointClearPending = false;
  }
  if (mPendingPointCachePages.isEmpty() || mPointProgram == nullptr ||
      !mPointProgram->isLinked()) {
    return;
  }
  PointCloudCachePage page = mPendingPointCachePages.takeFirst();
  const bool alreadyResident = std::any_of(
      mFullResolutionPointGpuChunks.cbegin(),
      mFullResolutionPointGpuChunks.cend(),
      [&page](const FullResolutionGpuChunk &chunk) {
        return chunk.nodeId == page.nodeId;
      });
  if (!alreadyResident && page.nodeId >= 0 &&
      page.nodeId < mPointCache.nodes.size()) {
    const qsizetype byteCount =
        page.vertices.size() * static_cast<qsizetype>(sizeof(PointPreviewVertex));
    evictPointCacheUntilFits(byteCount);
    FullResolutionGpuChunk gpuChunk;
    gpuChunk.nodeId = page.nodeId;
    gpuChunk.pointCount = static_cast<GLsizei>(page.vertices.size());
    gpuChunk.byteCount = byteCount;
    gpuChunk.lastUsedFrame = mPointCacheFrameSerial;
    const PointCloudCacheNode &node = mPointCache.nodes.at(page.nodeId);
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
      mPointCacheResidentBytes += byteCount;
      mFullResolutionPointGpuChunks.append(gpuChunk);
    } else {
      glDeleteBuffers(1, &gpuChunk.buffer);
      glDeleteVertexArrays(1, &gpuChunk.vertexArray);
      mPointCacheFailedNodes.insert(page.nodeId);
      mPointCacheError = QStringLiteral(
          "GPU memory could not accept point-cache node %1.")
                             .arg(page.nodeId);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
  }
  mUploadedFullResolutionPointCount = 0;
  for (const FullResolutionGpuChunk &chunk :
       std::as_const(mFullResolutionPointGpuChunks)) {
    mUploadedFullResolutionPointCount += chunk.pointCount;
  }
  requestMissingPointCachePages();
  updateFrameRefreshPolicy();
  update();
}

void NativeViewport::releaseFullResolutionMesh() {
  for (const FullResolutionMeshGpuChunk &chunk :
       std::as_const(mFullResolutionMeshGpuChunks)) {
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
  mFullResolutionMeshGpuChunks.clear();
  mMeshCacheResidentBytes = 0;
  mUploadedFullResolutionMeshTriangleCount = 0;
  mDrawnFullResolutionMeshTriangleCount = 0;
}

void NativeViewport::releaseMeshTexture() {
  if (mMeshTexture != 0) {
    glDeleteTextures(1, &mMeshTexture);
    mMeshTexture = 0;
  }
  mMeshTextureReady = false;
  mMeshTextureSize = {};
}

void NativeViewport::uploadPendingMeshTexture() {
  if (mMeshTextureClearPending) {
    releaseMeshTexture();
    mMeshTextureClearPending = false;
  }
  if (!mMeshTextureUploadPending) {
    return;
  }
  mMeshTextureUploadPending = false;
  if (!mMeshHasTextureCoordinates || mPendingMeshTexture.isNull()) {
    mPendingMeshTexture = {};
    return;
  }

  GLint maximumTextureSize = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
  QImage image = mPendingMeshTexture;
  if (maximumTextureSize > 0 &&
      (image.width() > maximumTextureSize ||
       image.height() > maximumTextureSize)) {
    image = image.scaled(maximumTextureSize, maximumTextureSize,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    mMeshTextureError =
        QStringLiteral("Texture was reduced to the GPU limit of %1 px.")
            .arg(maximumTextureSize);
  }
  image = image.convertToFormat(QImage::Format_RGBA8888)
              .mirrored(false, true);
  mPendingMeshTexture = {};
  if (image.isNull()) {
    mMeshTextureError = QStringLiteral(
        "The mesh texture could not be converted for OpenGL.");
    return;
  }

  while (glGetError() != GL_NO_ERROR) {
  }
  glGenTextures(1, &mMeshTexture);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, mMeshTexture);
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
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0,
               GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
  glGenerateMipmap(GL_TEXTURE_2D);
  const GLenum uploadError = glGetError();
  glBindTexture(GL_TEXTURE_2D, 0);
  if (uploadError != GL_NO_ERROR || mMeshTexture == 0) {
    glDeleteTextures(1, &mMeshTexture);
    mMeshTexture = 0;
    mMeshTextureError = QStringLiteral(
        "GPU memory could not accept the mesh texture.");
    return;
  }
  mMeshTextureSize = image.size();
  mMeshTextureReady = true;
  updateFrameRefreshPolicy();
}

void NativeViewport::updateMeshCacheSelection(
    const QMatrix4x4 &viewProjection) {
  if (!pagedMeshAvailable()) {
    mDesiredMeshCacheNodes.clear();
    return;
  }
  ++mMeshCacheFrameSerial;
  const bool interacting =
      mPressedButtons != Qt::NoButton || mNavigationInteractionActive ||
      (mViewSnapAnimation != nullptr &&
       mViewSnapAnimation->state() == QAbstractAnimation::Running);
  const float refinementThresholdPixels = interacting ? 180.0F : 34.0F;
  const qsizetype byteBudget = static_cast<qsizetype>(
      static_cast<long double>(mMeshCacheGpuBudgetBytes) * 0.85L);
  const QVector3D eye = cameraPosition();
  const float viewportHeight =
      std::max(1.0F, static_cast<float>(height() * devicePixelRatioF()));
  const auto projectedPixels = [&](const MeshCacheNode &node) {
    const QVector3D center =
        (node.boundsMinimum + node.boundsMaximum) * 0.5F;
    const float radius =
        std::max((node.boundsMaximum - node.boundsMinimum).length() * 0.5F,
                 mSceneRadius * 1.0e-6F);
    if (mOrthographic) {
      return radius / std::max(mDistance, 1.0e-5F) * viewportHeight * 2.0F;
    }
    const float distance =
        std::max((center - eye).length() - radius, mSceneRadius * 1.0e-5F);
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
  const MeshCacheNode &root = mMeshCache.nodes.at(mMeshCache.rootNode);
  qsizetype selectedBytes = 0;
  if (visible(root)) {
    selected.insert(root.id);
    refinable.append(root.id);
    selectedBytes = pageBytes(root);
  }
  while (!refinable.isEmpty()) {
    qsizetype bestPosition = -1;
    float bestPriority = refinementThresholdPixels;
    for (qsizetype position = 0; position < refinable.size(); ++position) {
      const MeshCacheNode &candidate =
          mMeshCache.nodes.at(refinable.at(position));
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
    const MeshCacheNode &node = mMeshCache.nodes.at(nodeId);
    QVector<int> visibleChildren;
    qsizetype childBytes = 0;
    for (const int childId : node.children) {
      const MeshCacheNode &child = mMeshCache.nodes.at(childId);
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
      if (!mMeshCache.nodes.at(childId).isLeaf()) {
        refinable.append(childId);
      }
    }
  }
  mDesiredMeshCacheNodes = std::move(selected);
  requestMissingMeshCachePages();
}

void NativeViewport::requestMissingMeshCachePages() {
  if (!pagedMeshAvailable()) {
    return;
  }
  const auto isResident = [this](const int nodeId) {
    return std::any_of(mFullResolutionMeshGpuChunks.cbegin(),
                       mFullResolutionMeshGpuChunks.cend(),
                       [nodeId](const FullResolutionMeshGpuChunk &chunk) {
                         return chunk.nodeId == nodeId;
                       });
  };
  QSet<int> needed;
  for (const int desired : std::as_const(mDesiredMeshCacheNodes)) {
    int nodeId = desired;
    while (nodeId >= 0 && !isResident(nodeId)) {
      if (!mMeshCacheReadsInFlight.contains(nodeId) &&
          !mMeshCacheFailedNodes.contains(nodeId)) {
        needed.insert(nodeId);
      }
      nodeId = mMeshCache.nodes.at(nodeId).parent;
    }
  }
  QVector<int> ordered = needed.values();
  std::sort(ordered.begin(), ordered.end(), [this](const int left,
                                                   const int right) {
    const MeshCacheNode &a = mMeshCache.nodes.at(left);
    const MeshCacheNode &b = mMeshCache.nodes.at(right);
    if (a.depth != b.depth) {
      return a.depth < b.depth;
    }
    return a.byteCount() < b.byteCount();
  });
  constexpr qsizetype kMaximumReadsInFlight = 2;
  for (const int nodeId : std::as_const(ordered)) {
    if (mMeshCacheReadsInFlight.size() >= kMaximumReadsInFlight) {
      break;
    }
    mMeshCacheReadsInFlight.insert(nodeId);
    const int generation = mSceneGeneration;
    const MeshCacheIndex cache = mMeshCache;
    auto *watcher = new QFutureWatcher<MeshCachePage>(this);
    connect(watcher, &QFutureWatcher<MeshCachePage>::finished, this,
            [this, watcher, generation, nodeId]() {
              MeshCachePage page = watcher->result();
              watcher->deleteLater();
              if (generation != mSceneGeneration) {
                return;
              }
              mMeshCacheReadsInFlight.remove(nodeId);
              if (page.isValid()) {
                mPendingMeshCachePages.append(std::move(page));
              } else {
                mMeshCacheFailedNodes.insert(nodeId);
                mMeshCacheError = page.error;
              }
              updateFrameRefreshPolicy();
              update();
            });
    watcher->setFuture(QtConcurrent::run(
        [cache, nodeId]() { return MeshCache::readNode(cache, nodeId); }));
  }
}

void NativeViewport::evictMeshCacheUntilFits(const qsizetype requiredBytes) {
  const auto isResident = [this](const int nodeId) {
    return std::any_of(mFullResolutionMeshGpuChunks.cbegin(),
                       mFullResolutionMeshGpuChunks.cend(),
                       [nodeId](const FullResolutionMeshGpuChunk &chunk) {
                         return chunk.nodeId == nodeId;
                       });
  };
  QSet<int> protectedNodes;
  for (const int desired : std::as_const(mDesiredMeshCacheNodes)) {
    int nodeId = desired;
    while (nodeId >= 0) {
      if (isResident(nodeId)) {
        protectedNodes.insert(nodeId);
        break;
      }
      nodeId = mMeshCache.nodes.at(nodeId).parent;
    }
  }
  while (!mFullResolutionMeshGpuChunks.isEmpty() &&
         mMeshCacheResidentBytes + requiredBytes >
             mMeshCacheGpuBudgetBytes) {
    qsizetype victim = -1;
    quint64 oldest = std::numeric_limits<quint64>::max();
    for (qsizetype index = 0;
         index < mFullResolutionMeshGpuChunks.size(); ++index) {
      const FullResolutionMeshGpuChunk &chunk =
          mFullResolutionMeshGpuChunks.at(index);
      if (!protectedNodes.contains(chunk.nodeId) &&
          chunk.lastUsedFrame < oldest) {
        victim = index;
        oldest = chunk.lastUsedFrame;
      }
    }
    if (victim < 0) {
      for (qsizetype index = 0;
           index < mFullResolutionMeshGpuChunks.size(); ++index) {
        const FullResolutionMeshGpuChunk &chunk =
            mFullResolutionMeshGpuChunks.at(index);
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
        mFullResolutionMeshGpuChunks.takeAt(victim);
    if (chunk.indexBuffer != 0) {
      glDeleteBuffers(1, &chunk.indexBuffer);
    }
    if (chunk.vertexBuffer != 0) {
      glDeleteBuffers(1, &chunk.vertexBuffer);
    }
    if (chunk.vertexArray != 0) {
      glDeleteVertexArrays(1, &chunk.vertexArray);
    }
    mMeshCacheResidentBytes -= chunk.byteCount;
  }
}

void NativeViewport::uploadPendingMeshCachePages() {
  if (mFullResolutionMeshClearPending) {
    releaseFullResolutionMesh();
    mFullResolutionMeshClearPending = false;
  }
  if (mPendingMeshCachePages.isEmpty() || mMeshProgram == nullptr ||
      !mMeshProgram->isLinked()) {
    return;
  }
  MeshCachePage page = mPendingMeshCachePages.takeFirst();
  const bool alreadyResident = std::any_of(
      mFullResolutionMeshGpuChunks.cbegin(),
      mFullResolutionMeshGpuChunks.cend(),
      [&page](const FullResolutionMeshGpuChunk &chunk) {
        return chunk.nodeId == page.nodeId;
      });
  if (!alreadyResident && page.nodeId >= 0 &&
      page.nodeId < mMeshCache.nodes.size()) {
    const qsizetype vertexBytes =
        page.vertices.size() * static_cast<qsizetype>(sizeof(MeshVertex));
    const qsizetype indexBytes =
        page.indices.size() * static_cast<qsizetype>(sizeof(quint32));
    const qsizetype byteCount = vertexBytes + indexBytes;
    evictMeshCacheUntilFits(byteCount);
    FullResolutionMeshGpuChunk gpuChunk;
    gpuChunk.nodeId = page.nodeId;
    gpuChunk.indexCount = static_cast<GLsizei>(page.indices.size());
    gpuChunk.byteCount = byteCount;
    gpuChunk.lastUsedFrame = mMeshCacheFrameSerial;
    const MeshCacheNode &node = mMeshCache.nodes.at(page.nodeId);
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
      mMeshCacheResidentBytes += byteCount;
      mFullResolutionMeshGpuChunks.append(gpuChunk);
    } else {
      glDeleteBuffers(1, &gpuChunk.indexBuffer);
      glDeleteBuffers(1, &gpuChunk.vertexBuffer);
      glDeleteVertexArrays(1, &gpuChunk.vertexArray);
      mMeshCacheFailedNodes.insert(page.nodeId);
      mMeshCacheError =
          QStringLiteral("GPU memory could not accept mesh-cache node %1.")
              .arg(page.nodeId);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
  }
  mUploadedFullResolutionMeshTriangleCount = 0;
  for (const FullResolutionMeshGpuChunk &chunk :
       std::as_const(mFullResolutionMeshGpuChunks)) {
    mUploadedFullResolutionMeshTriangleCount += chunk.indexCount / 3;
  }
  requestMissingMeshCachePages();
  updateFrameRefreshPolicy();
  update();
}

void NativeViewport::uploadPendingMesh() {
  if (!mMeshUploadPending || !mMeshVertexBuffer.isCreated() ||
      !mMeshIndexBuffer.isCreated()) {
    return;
  }

  QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mMeshVertexArray);
  mMeshVertexBuffer.bind();
  const qsizetype vertexBytes =
      mPendingMeshVertices.size() * static_cast<qsizetype>(sizeof(MeshVertex));
  mMeshVertexBuffer.allocate(
      mPendingMeshVertices.isEmpty() ? nullptr
                                     : mPendingMeshVertices.constData(),
      static_cast<int>(vertexBytes));
  mMeshVertexBuffer.release();

  mMeshIndexBuffer.bind();
  const qsizetype indexBytes =
      mPendingMeshIndices.size() * static_cast<qsizetype>(sizeof(quint32));
  mMeshIndexBuffer.allocate(
      mPendingMeshIndices.isEmpty() ? nullptr : mPendingMeshIndices.constData(),
      static_cast<int>(indexBytes));
  mMeshIndexBuffer.release();

  mPendingMeshVertices.clear();
  mPendingMeshIndices.clear();
  mMeshUploadPending = false;
}

void NativeViewport::synchronizeGaussianRenderingAvailability(
    const bool previousAvailability) {
  const bool available = gaussianRenderingAvailable();
  if (available != previousAvailability) {
    emit gaussianRenderingAvailabilityChanged(available);
  }
  if (!available && mRenderMode == RenderMode::Gaussians) {
    mRenderMode = RenderMode::Points;
    rebuildRenderedVertices();
    emit renderModeChanged(mRenderMode);
  }
}

void NativeViewport::drawPointCloud(const QMatrix4x4 &viewProjection) {
  mInteractionLodActive = false;
  mProgressiveUploadActive = false;
  if ((mRenderedPointCount <= 0 &&
       mFullResolutionPointGpuChunks.isEmpty()) ||
      mPointProgram == nullptr ||
      !mPointProgram->isLinked()) {
    return;
  }

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  mPointProgram->bind();
  mPointProgram->setUniformValue("viewProjection", viewProjection);
  mPointProgram->setUniformValue(
      "pointSize",
      pointPreviewDiameterPixels(static_cast<float>(devicePixelRatioF())));
  {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mPointVertexArray);
    if (mRenderedPointCount > 0) {
      glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(mRenderedPointCount));
    }
  }
  const auto residentChunk = [this](const int nodeId)
      -> FullResolutionGpuChunk * {
    const auto iterator = std::find_if(
        mFullResolutionPointGpuChunks.begin(),
        mFullResolutionPointGpuChunks.end(),
        [nodeId](const FullResolutionGpuChunk &chunk) {
          return chunk.nodeId == nodeId;
        });
    return iterator == mFullResolutionPointGpuChunks.end() ? nullptr
                                                            : &(*iterator);
  };
  QSet<int> drawNodes;
  bool desiredMissing = false;
  for (const int desired : std::as_const(mDesiredPointCacheNodes)) {
    int nodeId = desired;
    FullResolutionGpuChunk *chunk = residentChunk(nodeId);
    if (chunk == nullptr) {
      desiredMissing = true;
    }
    while (chunk == nullptr && nodeId >= 0 && mPointCache.isValid()) {
      nodeId = mPointCache.nodes.at(nodeId).parent;
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
    chunk->lastUsedFrame = mPointCacheFrameSerial;
    hierarchicalLod =
        hierarchicalLod ||
        (mPointCache.isValid() &&
         mPointCache.nodes.at(nodeId).depth < PointCloudCacheIndex::OctreeDepth);
    glBindVertexArray(chunk->vertexArray);
    glDrawArrays(GL_POINTS, 0, chunk->pointCount);
  }
  mInteractionLodActive = hierarchicalLod;
  mProgressiveUploadActive =
      desiredMissing || !mPointCacheReadsInFlight.isEmpty() ||
      !mPendingPointCachePages.isEmpty();
  glBindVertexArray(0);
  mPointProgram->release();
  glDisable(GL_BLEND);
}

void NativeViewport::drawMesh(const QMatrix4x4 &viewProjection) {
  mInteractionLodActive = false;
  mProgressiveUploadActive = false;
  mDrawnFullResolutionMeshTriangleCount = 0;
  if ((mRenderedMeshIndexCount < 3 &&
       mFullResolutionMeshGpuChunks.isEmpty()) ||
      mMeshProgram == nullptr || !mMeshProgram->isLinked()) {
    return;
  }

  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  mMeshProgram->bind();
  mMeshProgram->setUniformValue("viewProjection", viewProjection);
  mMeshProgram->setUniformValue("cameraPosition", cameraPosition());
  const bool textureEnabled =
      mMeshHasTextureCoordinates && mMeshTextureReady && mMeshTexture != 0;
  mMeshProgram->setUniformValue("albedoTexture", 0);
  mMeshProgram->setUniformValue("textureEnabled", textureEnabled);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, textureEnabled ? mMeshTexture : 0);
  if (mRenderedMeshIndexCount >= 3 && mMeshIndexBuffer.isCreated()) {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mMeshVertexArray);
    mMeshIndexBuffer.bind();
    glDrawElements(GL_TRIANGLES,
                   static_cast<GLsizei>(mRenderedMeshIndexCount),
                   GL_UNSIGNED_INT, nullptr);
    mMeshIndexBuffer.release();
  }
  const auto residentChunk = [this](const int nodeId)
      -> FullResolutionMeshGpuChunk * {
    const auto iterator = std::find_if(
        mFullResolutionMeshGpuChunks.begin(),
        mFullResolutionMeshGpuChunks.end(),
        [nodeId](const FullResolutionMeshGpuChunk &chunk) {
          return chunk.nodeId == nodeId;
        });
    return iterator == mFullResolutionMeshGpuChunks.end() ? nullptr
                                                           : &(*iterator);
  };
  QSet<int> drawNodes;
  bool desiredMissing = false;
  for (const int desired : std::as_const(mDesiredMeshCacheNodes)) {
    int nodeId = desired;
    FullResolutionMeshGpuChunk *chunk = residentChunk(nodeId);
    if (chunk == nullptr) {
      desiredMissing = true;
    }
    while (chunk == nullptr && nodeId >= 0 && pagedMeshAvailable()) {
      nodeId = mMeshCache.nodes.at(nodeId).parent;
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
    chunk->lastUsedFrame = mMeshCacheFrameSerial;
    hierarchicalLod =
        hierarchicalLod ||
        (pagedMeshAvailable() && !mMeshCache.nodes.at(nodeId).isLeaf());
    glBindVertexArray(chunk->vertexArray);
    glDrawElements(GL_TRIANGLES, chunk->indexCount, GL_UNSIGNED_INT, nullptr);
    mDrawnFullResolutionMeshTriangleCount += chunk->indexCount / 3;
  }
  glBindVertexArray(0);
  mInteractionLodActive = hierarchicalLod;
  mProgressiveUploadActive =
      desiredMissing || !mMeshCacheReadsInFlight.isEmpty() ||
      !mPendingMeshCachePages.isEmpty();
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
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  mPointProgram->bind();
  mPointProgram->setUniformValue("viewProjection", viewProjection);
  mPointProgram->setUniformValue(
      "pointSize",
      pointPreviewDiameterPixels(static_cast<float>(devicePixelRatioF())));
  glBindVertexArray(vertexArray);
  // The shared VAO advances its attributes per Gaussian instance for the
  // splat renderer. Point rendering consumes the same GPU memory per vertex,
  // so temporarily switch position/color back to a per-vertex rate.
  glVertexAttribDivisor(0, 0);
  glVertexAttribDivisor(1, 0);
  glDrawArrays(GL_POINTS, 0, drawCount);
  glVertexAttribDivisor(0, 1);
  glVertexAttribDivisor(1, 1);
  glBindVertexArray(0);
  mPointProgram->release();
  glDisable(GL_BLEND);
}

void NativeViewport::drawGaussianCloud(const QMatrix4x4 &view,
                                       const QMatrix4x4 &projection) {
  const bool livePreview = mTrainingGpuPreview.hasFrame();
  const qsizetype drawCount =
      livePreview ? mTrainingGpuPreview.pointCount() : mRenderedPointCount;
  if (drawCount <= 0 || mGaussianProgram == nullptr ||
      !mGaussianProgram->isLinked()) {
    return;
  }

  const qreal ratio = devicePixelRatioF();
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  mGaussianProgram->bind();
  mGaussianProgram->setUniformValue("view", view);
  mGaussianProgram->setUniformValue("projection", projection);
  mGaussianProgram->setUniformValue(
      "viewportPixels", QVector2D(static_cast<float>(width() * ratio),
                                  static_cast<float>(height() * ratio)));
  if (livePreview) {
    glBindVertexArray(mTrainingGpuPreview.activeVertexArray());
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(drawCount));
    glBindVertexArray(0);
  } else {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mGaussianVertexArray);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(drawCount));
  }
  mGaussianProgram->release();
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

void NativeViewport::drawInfiniteGrid(const QMatrix4x4 &viewProjection) {
  if (!mGridShaderReady || mGridProgram == nullptr ||
      !mGridProgram->isLinked() || !mGridVertexArray.isCreated()) {
    return;
  }

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glEnable(GL_MULTISAMPLE);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glLineWidth(1.0F);
  mGridProgram->bind();
  const ReferenceGridScale scale = referenceGridScale(
      mDistance, qMax(1, qRound(height() * devicePixelRatioF())));
  const ReferenceGridDrawSpans drawSpans =
      referenceGridDrawSpans(scale);
  const ReferenceGridPlane plane =
      referenceGridPlane({mYawDegrees, mPitchDegrees}, mOrthographic);
  const ReferenceGridFrame grid = referenceGridFrame(plane);
  const QVector3D gridPlaneOrigin = gridOrigin(plane);
  const QVector3D relativeCamera = cameraPosition() - gridPlaneOrigin;

  mGridProgram->setUniformValue("viewProjection", viewProjection);
  mGridProgram->setUniformValue("gridPlaneOrigin", gridPlaneOrigin);
  mGridProgram->setUniformValue("gridAxisU", grid.axisU);
  mGridProgram->setUniformValue("gridAxisV", grid.axisV);

  {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mGridVertexArray);
    const auto drawGridLevel =
        [this, &relativeCamera, &grid](const float step,
                                      const int lineCount,
                                      const QVector4D &color) {
          if (!std::isfinite(step) || step <= 0.0F || lineCount < 3) {
            return;
          }
          const float cameraU =
              QVector3D::dotProduct(relativeCamera, grid.axisU);
          const float cameraV =
              QVector3D::dotProduct(relativeCamera, grid.axisV);
          const QVector2D snappedCenter(
              std::round(cameraU / step) * step,
              std::round(cameraV / step) * step);
          mGridProgram->setUniformValue("gridCenter", snappedCenter);
          mGridProgram->setUniformValue("gridStep", step);
          mGridProgram->setUniformValue("gridHalfSpan",
                                        step * (lineCount / 2));
          mGridProgram->setUniformValue("gridLineCount", lineCount);
          mGridProgram->setUniformValue("drawAxis", 0);
          mGridProgram->setUniformValue("lineColor", color);
          glDrawArrays(GL_LINES, 0, lineCount * 8);
        };

    // Choose one concrete 1-2-5 minor level, then draw the 10x emphasis level
    // over it. Actual MSAA line primitives stay continuous at shallow angles
    // and avoid the periodic-fragment phase loss seen on dense point clouds.
    const float minorStep = scale.displayMajorStep * 0.1F;
    drawGridLevel(minorStep, 161,
                  QVector4D(0.17F, 0.19F, 0.20F, 0.38F));
    drawGridLevel(scale.displayMajorStep, 81,
                  QVector4D(0.29F, 0.31F, 0.33F, 0.58F));

    // Keep the coloured origin axes inside the same camera-relative patch as
    // the major grid. Extending them farther than the grey lines makes a
    // coplanar axis appear to lift into the sky after the grid has faded.
    const float cameraU =
        QVector3D::dotProduct(relativeCamera, grid.axisU);
    const float cameraV =
        QVector3D::dotProduct(relativeCamera, grid.axisV);
    const QVector2D axisCenter(
        std::round(cameraU / scale.displayMajorStep) *
            scale.displayMajorStep,
        std::round(cameraV / scale.displayMajorStep) *
            scale.displayMajorStep);
    mGridProgram->setUniformValue("gridCenter", axisCenter);
    mGridProgram->setUniformValue("gridHalfSpan", drawSpans.axisHalfSpan);
    mGridProgram->setUniformValue("drawAxis", 1);
    mGridProgram->setUniformValue(
        "lineColor",
        QVector4D(grid.axisUColor.x(), grid.axisUColor.y(),
                  grid.axisUColor.z(), 0.86F));
    glDrawArrays(GL_LINES, 0, 4);
    mGridProgram->setUniformValue("drawAxis", 2);
    mGridProgram->setUniformValue(
        "lineColor",
        QVector4D(grid.axisVColor.x(), grid.axisVColor.y(),
                  grid.axisVColor.z(), 0.86F));
    glDrawArrays(GL_LINES, 0, 4);
  }
  mGridProgram->release();
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

void NativeViewport::enterEvent(QEnterEvent *event) {
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
  QOpenGLWidget::leaveEvent(event);
}

void NativeViewport::mousePressEvent(QMouseEvent *event) {
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
    setToolTip(QStringLiteral("Ctrl + 左键：旋转视角"));
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

  mPressedButtons = event->buttons();
  mLastMousePosition = event->position().toPoint();
  if (mMode == InteractionMode::Brush && !mTemporaryOrbitActive) {
    mBrushCursorPosition = event->position();
    mBrushCursorVisible = true;
  }
  if (event->button() == Qt::LeftButton && !mSelectionBusy &&
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
    updateNavigationGizmoHover(event->position());
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

  const bool pan =
      !mTemporaryOrbitActive &&
      (mPressedButtons.testFlag(Qt::MiddleButton) ||
       mPressedButtons.testFlag(Qt::RightButton) ||
       event->modifiers().testFlag(Qt::ShiftModifier));
  if (pan) {
    panCamera(delta);
  } else if (mPressedButtons.testFlag(Qt::LeftButton)) {
    mViewSnapAnimation->stop();
    mOrthographic = false;
    const OrbitAngles angles =
        orbitAnglesAfterLeftDrag({mYawDegrees, mPitchDegrees}, delta);
    mYawDegrees = angles.yawDegrees;
    mPitchDegrees = angles.pitchDegrees;
  }

  update();
  event->accept();
}

void NativeViewport::mouseReleaseEvent(QMouseEvent *event) {
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
    if (mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()) {
      rebuildRenderedVertices();
      update();
    }
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
  leaveCameraView();
  const float steps = static_cast<float>(event->angleDelta().y()) / 120.0F;
  mDistance = clampViewportDistance(
      mDistance * std::pow(0.84F, steps), mSceneRadius);
  update();
  event->accept();
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
  if (hit == mNavigationHover) {
    return;
  }

  mNavigationHover = hit;
  QString tooltip;
  switch (hit.part) {
  case NavigationGizmoPart::Rotate:
    tooltip = QStringLiteral("拖动环绕视图；单击 %1 轴吸附到正交视图")
                  .arg(navigationAxisLabel(hit.axis));
    setCursor(Qt::OpenHandCursor);
    break;
  case NavigationGizmoPart::Zoom:
    {
      const ViewportZoomLimits limits = viewportZoomLimits(mSceneRadius);
      tooltip = QStringLiteral("上下拖动缩放视图（%1 – %2）")
                    .arg(formatViewportDistance(limits.minimumDistance),
                         formatViewportDistance(limits.maximumDistance));
    }
    setCursor(Qt::SizeVerCursor);
    break;
  case NavigationGizmoPart::Pan:
    tooltip = QStringLiteral("拖动平移视图");
    setCursor(Qt::OpenHandCursor);
    break;
  case NavigationGizmoPart::Camera:
    tooltip = camerasAvailable()
                  ? (mCameraViewActive ? QStringLiteral("返回用户视图")
                                       : QStringLiteral("切换到场景相机视图"))
                  : QStringLiteral("当前场景没有可用相机");
    setCursor(camerasAvailable() ? Qt::PointingHandCursor
                                 : Qt::ForbiddenCursor);
    break;
  case NavigationGizmoPart::Projection:
    tooltip = mOrthographic ? QStringLiteral("切换到透视视图")
                            : QStringLiteral("切换到正交视图");
    setCursor(Qt::PointingHandCursor);
    break;
  case NavigationGizmoPart::None:
    setCursor(defaultInteractionCursor(mMode));
    break;
  }
  setToolTip(tooltip);
  update();
}

void NativeViewport::updateNavigationGizmoInteraction(const QPoint &current) {
  const QPoint totalDelta = current - mNavigationPressPosition;
  if (!mNavigationDragging &&
      totalDelta.manhattanLength() < QApplication::startDragDistance()) {
    return;
  }
  if (!mNavigationDragging) {
    mNavigationDragging = true;
    mViewSnapAnimation->stop();
    if (mNavigationPress.part == NavigationGizmoPart::Rotate ||
        mNavigationPress.part == NavigationGizmoPart::Zoom ||
        mNavigationPress.part == NavigationGizmoPart::Pan) {
      leaveCameraView();
    }
    if (mNavigationPress.part == NavigationGizmoPart::Rotate) {
      mOrthographic = false;
    }
  }

  const QPoint delta = current - mLastMousePosition;
  mLastMousePosition = current;
  if (delta.isNull()) {
    return;
  }

  switch (mNavigationPress.part) {
  case NavigationGizmoPart::Rotate: {
    const OrbitAngles angles =
        orbitAnglesAfterLeftDrag({mYawDegrees, mPitchDegrees}, delta);
    mYawDegrees = angles.yawDegrees;
    mPitchDegrees = angles.pitchDegrees;
    break;
  }
  case NavigationGizmoPart::Zoom:
    mDistance = clampViewportDistance(
        mDistance * std::pow(1.008F, static_cast<float>(delta.y())),
        mSceneRadius);
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
  } else if (mRenderMode == RenderMode::Gaussians &&
             gaussianRenderingAvailable()) {
    rebuildRenderedVertices();
  }

  updateNavigationGizmoHover(QPointF(mLastMousePosition));
}

void NativeViewport::setAxisView(const NavigationAxis axis) {
  const std::optional<OrbitAngles> target = navigationAxisViewAngles(axis);
  if (!target.has_value()) {
    return;
  }

  mViewSnapAnimation->stop();
  leaveCameraView();
  mOrthographic = true;
  const float targetYaw =
      shortestEquivalentAngle(mYawDegrees, target->yawDegrees);
  mViewSnapAnimation->setStartValue(QPointF(mYawDegrees, mPitchDegrees));
  mViewSnapAnimation->setEndValue(QPointF(targetYaw, target->pitchDegrees));
  mViewSnapAnimation->start();
}

void NativeViewport::panCamera(const QPoint &delta) {
  const QMatrix4x4 view = viewMatrix();
  const QVector3D right(view(0, 0), view(0, 1), view(0, 2));
  const QVector3D up(view(1, 0), view(1, 1), view(1, 2));
  const float scale = mDistance * 0.0018F;
  mTarget += right * (-static_cast<float>(delta.x()) * scale);
  mTarget += up * (static_cast<float>(delta.y()) * scale);
}

void NativeViewport::toggleCameraView() {
  if (mCameraViewActive && mStoredCameraView.has_value()) {
    mTarget = mStoredCameraView->target;
    mYawDegrees = mStoredCameraView->yawDegrees;
    mPitchDegrees = mStoredCameraView->pitchDegrees;
    mDistance =
        clampViewportDistance(mStoredCameraView->distance, mSceneRadius);
    mOrthographic = mStoredCameraView->orthographic;
    mCameraViewActive = false;
    mStoredCameraView.reset();
    update();
    return;
  }
  if (!camerasAvailable()) {
    return;
  }

  const CameraPose &camera = mCameraTrajectory.cameras().constFirst();
  const QVector3D forward = camera.forward.normalized();
  if (forward.lengthSquared() < 1.0e-6F) {
    return;
  }

  mStoredCameraView = StoredCameraView{mTarget, mYawDegrees, mPitchDegrees,
                                       mDistance, mOrthographic};
  const QVector3D cameraOffset = -forward;
  mPitchDegrees =
      std::asin(std::clamp(cameraOffset.z(), -1.0F, 1.0F)) * 180.0F / kPi;
  mYawDegrees = std::atan2(cameraOffset.x(), cameraOffset.y()) * 180.0F / kPi;
  mTarget = camera.position + forward * mDistance;
  mOrthographic = false;
  mCameraViewActive = true;
  update();
}

void NativeViewport::leaveCameraView() {
  if (!mCameraViewActive) {
    return;
  }
  mCameraViewActive = false;
  mStoredCameraView.reset();
}

QVector3D NativeViewport::cameraPosition() const {
  const OrbitFrame frame = orbitFrame({mYawDegrees, mPitchDegrees});
  return mTarget + frame.cameraOffsetDirection * mDistance;
}

QVector3D NativeViewport::gridOrigin(const ReferenceGridPlane plane) const {
  if (!mSceneCoordinates.valid) {
    return referenceGridOrigin();
  }
  if (mReferencePlaneMode == ReferencePlaneMode::WorldZero) {
    return mSceneCoordinates.localFromGlobal({0.0, 0.0, 0.0});
  }
  const QVector3D minimum = mSceneCoordinates.localMinimum();
  switch (plane) {
  case ReferenceGridPlane::XY:
    return QVector3D(0.0F, 0.0F, minimum.z());
  case ReferenceGridPlane::XZ:
    return QVector3D(0.0F, minimum.y(), 0.0F);
  case ReferenceGridPlane::YZ:
    return QVector3D(minimum.x(), 0.0F, 0.0F);
  }
  return referenceGridOrigin();
}

QString NativeViewport::formatViewportDistance(const float localDistance) const {
  if (!mSceneCoordinates.valid) {
    return formatMetricDistance(localDistance);
  }
  const double sourceDistance =
      static_cast<double>(localDistance) / mSceneCoordinates.displayScale;
  return formatSceneLength(sourceDistance, mSceneCoordinates);
}

QMatrix4x4 NativeViewport::viewMatrix() const {
  QMatrix4x4 view;
  const OrbitFrame frame = orbitFrame({mYawDegrees, mPitchDegrees});
  const QVector3D position = mTarget + frame.cameraOffsetDirection * mDistance;
  view.lookAt(position, mTarget, frame.upDirection);
  return view;
}

QMatrix4x4 NativeViewport::projectionMatrix() const {
  QMatrix4x4 projection;
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height())
                   : 1.0F;
  const float nearPlane = std::max(0.001F, mDistance / 10000.0F);
  const float farPlane = std::max(100.0F, mDistance + mSceneRadius * 12.0F);
  if (mOrthographic) {
    const float halfHeight =
        std::max(0.05F, mDistance * std::tan(radians(23.0F)));
    projection.ortho(-halfHeight * aspect, halfHeight * aspect, -halfHeight,
                     halfHeight, nearPlane, farPlane);
  } else {
    projection.perspective(46.0F, aspect, nearPlane, farPlane);
  }
  return projection;
}

QMatrix4x4 NativeViewport::viewProjectionMatrix() const {
  return projectionMatrix() * viewMatrix();
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
  return QPointF((normalized.x() * 0.5F + 0.5F) * width(),
                 (1.0F - (normalized.y() * 0.5F + 0.5F)) * height());
}

void NativeViewport::drawReferenceAxes(QPainter &painter,
                                       const QMatrix4x4 &viewProjection) {
  const float axisLength = std::max(3.0F, mSceneRadius);
  const ReferenceGridPlane plane =
      referenceGridPlane({mYawDegrees, mPitchDegrees}, mOrthographic);
  const QVector3D originLocal = gridOrigin(plane);
  const auto origin = projectPoint(originLocal, viewProjection);
  const auto xAxis = projectPoint(
      originLocal + QVector3D(axisLength, 0.0F, 0.0F), viewProjection);
  const auto yAxis = projectPoint(
      originLocal + QVector3D(0.0F, axisLength, 0.0F), viewProjection);
  const auto zAxis = projectPoint(
      originLocal + QVector3D(0.0F, 0.0F, axisLength), viewProjection);
  if (origin.has_value() && xAxis.has_value()) {
    painter.setPen(QPen(QColor(214, 91, 91), 2.0));
    painter.drawLine(*origin, *xAxis);
  }
  if (origin.has_value() && yAxis.has_value()) {
    painter.setPen(QPen(QColor(89, 139, 222), 2.0));
    painter.drawLine(*origin, *yAxis);
  }
  if (origin.has_value() && zAxis.has_value()) {
    painter.setPen(QPen(QColor(91, 191, 137), 2.0));
    painter.drawLine(*origin, *zAxis);
  }
}

void NativeViewport::drawCameraTrajectory(QPainter &painter,
                                          const QMatrix4x4 &viewProjection) {
  if (!mShowCameras || !camerasAvailable()) {
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
  painter.setPen(QPen(QColor(109, 160, 255, 205), 1.8, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  drawSegments(mCameraGeometry.path);
  painter.setPen(QPen(QColor(84, 209, 122, 220), 1.25, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
  drawSegments(mCameraGeometry.frustums);
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
  if (mMode == InteractionMode::Brush) {
    if (mSelectionGestureActive && !mSelectionPath.isEmpty()) {
      QPainterPath stroke;
      stroke.moveTo(mSelectionPath.first());
      for (qsizetype index = 1; index < mSelectionPath.size(); ++index) {
        stroke.lineTo(mSelectionPath.at(index));
      }
      stroke.lineTo(mSelectionCurrent);
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(QColor(102, 193, 168, 48), mBrushRadius * 2.0,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter.drawPath(stroke);
      painter.setPen(QPen(QColor(124, 219, 191, 205), 1.2, Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
      painter.drawPath(stroke);
    }
    if (drawBrushCursor) {
      painter.setBrush(QColor(102, 193, 168, 20));
      painter.setPen(QPen(QColor(124, 219, 191, 220), 1.4));
      painter.drawEllipse(mBrushCursorPosition, mBrushRadius, mBrushRadius);
    }
    painter.restore();
    return;
  }

  painter.setPen(QPen(QColor(102, 193, 168), 1.5, Qt::SolidLine));
  painter.setBrush(QColor(102, 193, 168, 36));
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
  painter.save();
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(17, 19, 21, 225));

  const QString sceneName = mTrainingGpuPreview.hasFrame()
                                ? mRenderMode == RenderMode::Points
                                      ? QStringLiteral("训练中 · 点云增密预览")
                                      : QStringLiteral("训练中 · GPU 实时预览")
                                : mScenePath.isEmpty()
                                      ? QStringLiteral("未载入场景")
                                      : QFileInfo(mScenePath).fileName();
  const QString project =
      mProjectLabel.isEmpty() ? QStringLiteral("未打开工程") : mProjectLabel;
  QString count;
  if (mTrainingGpuPreview.hasFrame()) {
    count = mRenderMode == RenderMode::Points
                ? QStringLiteral("迭代 %1 | %2 高斯中心（点云）| 共享 GPU 显存")
                      .arg(mTrainingGpuPreview.iteration())
                      .arg(formatCount(mTrainingGpuPreview.pointCount()))
                : QStringLiteral("迭代 %1 | %2 高斯 | 共享 GPU 显存")
                      .arg(mTrainingGpuPreview.iteration())
                      .arg(formatCount(mTrainingGpuPreview.pointCount()));
  } else if (!mSceneLoadMessage.isEmpty()) {
    count = mSceneLoadMessage;
  } else if (pagedMeshAvailable()) {
    count = QStringLiteral(
                "%1 顶点 | %2 面 | %3 三角形 | %4 | 当前绘制 %5 | "
                "GPU LOD 缓存 %6 | 只读")
                .arg(formatCount(mMeshCache.fullVertexCount),
                     formatCount(mMeshCache.fullFaceCount),
                     formatCount(mMeshCache.renderableTriangleCount),
                     mProgressiveUploadActive
                         ? QStringLiteral("磁盘分页")
                         : mInteractionLodActive
                               ? QStringLiteral("层级 Mesh LOD")
                               : QStringLiteral("叶级细节"),
                     formatCount(mDrawnFullResolutionMeshTriangleCount),
                     formatCount(mUploadedFullResolutionMeshTriangleCount));
    if (!mMeshCacheError.isEmpty()) {
      count += QStringLiteral(" | 分页错误");
    }
  } else if (mPreviewOnlyScene) {
    count = QStringLiteral("%1 点 | %2 | GPU 驻留 %3 / %4 | 只读")
                .arg(formatCount(mPreviewPointCount),
                     mProgressiveUploadActive
                         ? QStringLiteral("磁盘分页")
                         : mInteractionLodActive
                               ? QStringLiteral("八叉树 LOD")
                               : QStringLiteral("叶级细节"),
                     formatCount(mUploadedFullResolutionPointCount),
                     formatCount(mPointCacheGpuBudgetBytes /
                                 sizeof(PointPreviewVertex)));
    if (!mPointCacheError.isEmpty()) {
      count += QStringLiteral(" | 分页错误");
    }
  } else if (mSourceFaceCount > 0 && mPreviewTriangleCount > 0) {
    count = QStringLiteral("%1 顶点 | %2 面 | %3 预览三角形")
                .arg(formatCount(mGaussianCount),
                     formatCount(mSourceFaceCount),
                     formatCount(mPreviewTriangleCount));
  } else if (mGaussianCount > 0 && mPreviewPointCount > 0 &&
             mGaussianCount != mPreviewPointCount) {
    count = QStringLiteral("%1 %2 | 预览 %3")
                .arg(formatCount(mGaussianCount),
                     mHasGaussianAttributes ? QStringLiteral("高斯")
                                            : QStringLiteral("点"),
                     formatCount(mPreviewPointCount));
  } else if (mGaussianCount > 0) {
    count = QStringLiteral("%1 %2").arg(
        formatCount(mGaussianCount),
        mHasGaussianAttributes ? QStringLiteral("高斯") : QStringLiteral("点"));
  } else {
    count = QStringLiteral("场景数据待载入");
  }
  if (mHasMesh) {
    if (mMeshTextureReady && mMeshTextureSize.isValid()) {
      count += QStringLiteral(" | 贴图 %1 · %2×%3")
                   .arg(QFileInfo(mMeshTexturePath).fileName())
                   .arg(mMeshTextureSize.width())
                   .arg(mMeshTextureSize.height());
    } else if (mMeshHasTextureCoordinates) {
      count += QStringLiteral(" | UV · 顶点色回退");
    } else {
      count += QStringLiteral(" | 顶点色");
    }
    if (!mMeshTextureError.isEmpty()) {
      count += QStringLiteral(" | 贴图警告");
    }
  }
  if (mShowCameras) {
    count += QStringLiteral(" | 相机 %1%2")
                 .arg(formatCount(cameraCount()),
                      mCameraGeometry.decimated ? QStringLiteral("（抽稀）")
                                                : QString());
  }
  if (mSelectionBusy) {
    count += QStringLiteral(" | 正在计算选择");
  } else if (mEditModel.selectedCount() > 0 || mEditModel.deletedCount() > 0) {
    count += QStringLiteral(" | 已选 %1 | 已删 %2")
                 .arg(formatCount(mEditModel.selectedCount()),
                      formatCount(mEditModel.deletedCount()));
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
  const int badgeHeight = (std::max)(22, lineHeight + 6);
  const QString mode =
      mSelectionBusy ? QStringLiteral("选择处理中") : modeLabel(mMode);
  const int modeWidth = metrics.horizontalAdvance(mode) + 18;
  const QString title = QStringLiteral("%1  ·  %2").arg(project, sceneName);
  const int widthHint =
      (std::max)(metrics.horizontalAdvance(title),
                 metrics.horizontalAdvance(count)) +
      headerPaddingX * 2;
  const int headerHeight =
      headerPaddingY * 2 + lineHeight * 2 + lineGap;
  const int headerMaxWidth =
      qMax(1, width() - modeWidth - viewportMargin * 4);
  const int headerMinWidth = (std::min)(160, headerMaxWidth);
  const QRect headerRect(
      viewportMargin, viewportMargin,
      std::clamp(widthHint, headerMinWidth, headerMaxWidth), headerHeight);
  painter.setBrush(QColor(17, 19, 21, 215));
  painter.drawRoundedRect(headerRect, 5, 5);

  painter.setPen(QColor(232, 235, 236));
  QFont strongFont = compactFont;
  strongFont.setWeight(QFont::DemiBold);
  painter.setFont(strongFont);
  QRect lineRect(headerRect.left() + headerPaddingX,
                 headerRect.top() + headerPaddingY,
                 headerRect.width() - headerPaddingX * 2, lineHeight);
  painter.drawText(
      lineRect, Qt::AlignLeft | Qt::AlignVCenter,
      QFontMetrics(strongFont).elidedText(title, Qt::ElideMiddle,
                                          lineRect.width()));
  painter.setFont(compactFont);
  painter.setPen(QColor(102, 193, 168));
  lineRect.translate(0, lineHeight + lineGap);
  painter.drawText(lineRect, Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(count, Qt::ElideRight, lineRect.width()));

  const QString renderer =
      mRenderMode == RenderMode::Mesh && meshRenderingAvailable()
          ? pagedMeshAvailable() ? QStringLiteral("分页三角网格")
                                 : QStringLiteral("三角网格")
          : mRenderMode == RenderMode::Gaussians &&
                    gaussianRenderingAvailable()
                ? QStringLiteral("高斯 DC SH")
                : QStringLiteral("点预览");

  const ReferenceGridScale gridScale = referenceGridScale(
      mDistance, qMax(1, qRound(height() * devicePixelRatioF())));
  const double framesPerSecond = mFrameRateCounter.framesPerSecond();
  const double averageFrameMilliseconds =
      mFrameRateCounter.averageFrameMilliseconds();
  const QString frameRateText =
      framesPerSecond > 0.0 && averageFrameMilliseconds > 0.0
          ? QStringLiteral("%1 FPS (%2 ms)")
                .arg(framesPerSecond, 0, 'f', 1)
                .arg(averageFrameMilliseconds, 0, 'f', 1)
          : QStringLiteral("FPS —");
  const QString statusText =
      QStringLiteral(
          "网格 %1  ·  视距 %2  ·  精度 %3  ·  %4  |  %5  ·  %6")
          .arg(formatViewportDistance(gridScale.displayMajorStep),
               formatViewportDistance(mDistance),
               formatViewportDistance(gridScale.minimumStep),
               referencePlaneDescription(), renderer, frameRateText);
  const int statusWidth = metrics.horizontalAdvance(statusText) + 20;
  const QRect statusRect(
      viewportMargin, height() - badgeHeight - viewportMargin,
      (std::min)(statusWidth, qMax(1, width() - viewportMargin * 2)),
      badgeHeight);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(17, 19, 21, 215));
  painter.drawRoundedRect(statusRect, 5, 5);
  painter.setPen(QColor(181, 189, 193));
  painter.drawText(
      statusRect.adjusted(8, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
      metrics.elidedText(statusText, Qt::ElideRight,
                         statusRect.width() - 16));

  const QRect modeRect(width() - modeWidth - viewportMargin, viewportMargin,
                       modeWidth, badgeHeight);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(49, 93, 88, 235));
  painter.drawRoundedRect(modeRect, 5, 5);
  painter.setPen(QColor(238, 246, 244));
  painter.drawText(modeRect, Qt::AlignCenter, mode);

  painter.restore();
}

void NativeViewport::drawAxisGizmo(QPainter &painter) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  const NavigationGizmoLayout layout = navigationGizmo();
  const QColor viewportColor(12, 15, 17);
  const bool rotateActive =
      mNavigationHover.part == NavigationGizmoPart::Rotate ||
      (mNavigationInteractionActive &&
       mNavigationPress.part == NavigationGizmoPart::Rotate);

  if (rotateActive) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 82));
    painter.drawEllipse(layout.rotateBounds);
  }

  std::array<const NavigationAxisHandle *, 6> orderedHandles;
  for (std::size_t index = 0; index < orderedHandles.size(); ++index) {
    orderedHandles[index] = &layout.handles[index];
  }
  std::sort(
      orderedHandles.begin(), orderedHandles.end(),
      [](const NavigationAxisHandle *left, const NavigationAxisHandle *right) {
        return left->depth < right->depth;
      });

  for (const NavigationAxisHandle *handle : orderedHandles) {
    if (!handle->positive || handle->hidden) {
      continue;
    }
    const QColor axisColor = navigationAxisColor(handle->axisIndex);
    const qreal colorAmount =
        (static_cast<qreal>(handle->depth) + 1.0) * 0.25 + 0.5;
    painter.setPen(QPen(mixColor(viewportColor, axisColor, colorAmount),
                        layout.lineWidth, Qt::SolidLine, Qt::RoundCap));
    QLineF axisLine(layout.center, handle->center);
    if (axisLine.length() > handle->radius) {
      axisLine.setLength(axisLine.length() - handle->radius * 0.72);
    }
    painter.drawLine(axisLine);
  }

  QFont axisFont = painter.font();
  axisFont.setBold(true);
  axisFont.setPixelSize(std::max(11, qRound(layout.radius * 0.30)));
  painter.setFont(axisFont);

  for (const NavigationAxisHandle *handle : orderedHandles) {
    if (handle->hidden) {
      continue;
    }
    const QColor axisColor = navigationAxisColor(handle->axisIndex);
    const bool highlighted =
        (mNavigationHover.part == NavigationGizmoPart::Rotate &&
         mNavigationHover.axis == handle->axis) ||
        (mNavigationInteractionActive &&
         mNavigationPress.part == NavigationGizmoPart::Rotate &&
         mNavigationPress.axis == handle->axis);

    QColor fill;
    if (handle->positive) {
      const qreal amount =
          (static_cast<qreal>(handle->depth) + 1.0) * 0.25 + 0.5;
      fill = mixColor(viewportColor, axisColor, amount);
    } else {
      fill = mixColor(viewportColor, axisColor, 0.25);
      fill.setAlphaF(
          std::clamp(static_cast<qreal>(handle->depth) + 1.0, 0.22, 1.0));
    }

    painter.setBrush(fill);
    painter.setPen(QPen(highlighted ? QColor(255, 255, 255, 230) : fill,
                        std::max(1.0, layout.lineWidth * 0.55)));
    painter.drawEllipse(handle->center, handle->radius, handle->radius);

    if (handle->positive || highlighted) {
      painter.setPen(highlighted ? QColor(255, 255, 255)
                                 : QColor(11, 13, 15, 225));
      const QRectF textRect(handle->center.x() - handle->radius * 1.35,
                            handle->center.y() - handle->radius * 1.2,
                            handle->radius * 2.7, handle->radius * 2.4);
      painter.drawText(textRect, Qt::AlignCenter,
                       navigationAxisLabel(handle->axis));
    }
  }

  const auto drawButtonBackground =
      [this, &painter](const QRectF &rect, const NavigationGizmoPart part) {
        const bool active =
            mNavigationHover.part == part ||
            (mNavigationInteractionActive && mNavigationPress.part == part);
        painter.setPen(Qt::NoPen);
        painter.setBrush(active ? QColor(0, 0, 0, 112) : QColor(0, 0, 0, 42));
        painter.drawEllipse(rect);
      };
  const QColor iconColor(216, 220, 224, 225);
  const qreal iconWidth = std::max(1.5, layout.lineWidth * 0.72);

  drawButtonBackground(layout.zoomButton, NavigationGizmoPart::Zoom);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(
      QPen(iconColor, iconWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  const QPointF zoomCenter = layout.zoomButton.center() + QPointF(-2.0, -2.0);
  const qreal zoomRadius = layout.zoomButton.width() * 0.20;
  painter.drawEllipse(zoomCenter, zoomRadius, zoomRadius);
  painter.drawLine(zoomCenter + QPointF(zoomRadius * 0.72, zoomRadius * 0.72),
                   zoomCenter + QPointF(zoomRadius * 1.65, zoomRadius * 1.65));

  drawButtonBackground(layout.panButton, NavigationGizmoPart::Pan);
  painter.setPen(
      QPen(iconColor, iconWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  const QPointF panCenter = layout.panButton.center();
  const qreal panExtent = layout.panButton.width() * 0.23;
  painter.drawLine(panCenter + QPointF(-panExtent, 0.0),
                   panCenter + QPointF(panExtent, 0.0));
  painter.drawLine(panCenter + QPointF(0.0, -panExtent),
                   panCenter + QPointF(0.0, panExtent));
  const qreal arrow = panExtent * 0.36;
  painter.drawLine(panCenter + QPointF(-panExtent, 0.0),
                   panCenter + QPointF(-panExtent + arrow, -arrow));
  painter.drawLine(panCenter + QPointF(-panExtent, 0.0),
                   panCenter + QPointF(-panExtent + arrow, arrow));
  painter.drawLine(panCenter + QPointF(panExtent, 0.0),
                   panCenter + QPointF(panExtent - arrow, -arrow));
  painter.drawLine(panCenter + QPointF(panExtent, 0.0),
                   panCenter + QPointF(panExtent - arrow, arrow));
  painter.drawLine(panCenter + QPointF(0.0, -panExtent),
                   panCenter + QPointF(-arrow, -panExtent + arrow));
  painter.drawLine(panCenter + QPointF(0.0, -panExtent),
                   panCenter + QPointF(arrow, -panExtent + arrow));
  painter.drawLine(panCenter + QPointF(0.0, panExtent),
                   panCenter + QPointF(-arrow, panExtent - arrow));
  painter.drawLine(panCenter + QPointF(0.0, panExtent),
                   panCenter + QPointF(arrow, panExtent - arrow));

  drawButtonBackground(layout.cameraButton, NavigationGizmoPart::Camera);
  const QColor cameraIconColor =
      !camerasAvailable()
          ? QColor(135, 140, 145, 120)
          : (mCameraViewActive ? QColor(114, 190, 255) : iconColor);
  painter.setPen(QPen(cameraIconColor, iconWidth, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  const qreal cameraWidth = layout.cameraButton.width() * 0.36;
  const qreal cameraHeight = layout.cameraButton.height() * 0.27;
  const QRectF cameraBody(layout.cameraButton.center().x() - cameraWidth * 0.58,
                          layout.cameraButton.center().y() - cameraHeight * 0.5,
                          cameraWidth, cameraHeight);
  painter.drawRoundedRect(cameraBody, cameraHeight * 0.16, cameraHeight * 0.16);
  QPolygonF cameraLens;
  cameraLens << QPointF(cameraBody.right(),
                        cameraBody.top() + cameraHeight * 0.18)
             << QPointF(cameraBody.right() + cameraWidth * 0.38,
                        cameraBody.top() - cameraHeight * 0.10)
             << QPointF(cameraBody.right() + cameraWidth * 0.38,
                        cameraBody.bottom() + cameraHeight * 0.10)
             << QPointF(cameraBody.right(),
                        cameraBody.bottom() - cameraHeight * 0.18);
  painter.drawPolygon(cameraLens);

  drawButtonBackground(layout.projectionButton,
                       NavigationGizmoPart::Projection);
  painter.setPen(QPen(mOrthographic ? QColor(114, 190, 255) : iconColor,
                      iconWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  const QRectF projectionIcon = layout.projectionButton.adjusted(
      layout.projectionButton.width() * 0.27,
      layout.projectionButton.height() * 0.27,
      -layout.projectionButton.width() * 0.27,
      -layout.projectionButton.height() * 0.27);
  if (mOrthographic) {
    painter.drawRect(projectionIcon);
  } else {
    QPolygonF trapezoid;
    trapezoid << QPointF(projectionIcon.left() + projectionIcon.width() * 0.18,
                         projectionIcon.top())
              << QPointF(projectionIcon.right() - projectionIcon.width() * 0.18,
                         projectionIcon.top())
              << QPointF(projectionIcon.right(), projectionIcon.bottom())
              << QPointF(projectionIcon.left(), projectionIcon.bottom());
    painter.drawPolygon(trapezoid);
  }
  painter.restore();
}

} // namespace gsw

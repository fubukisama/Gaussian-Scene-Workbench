#include "NativeViewport.h"

#include "NavigationGizmo.h"
#include "ScreenSpaceSelection.h"
#include "ViewportCamera.h"

#include <QApplication>
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
#include <QVariantAnimation>
#include <QVector2D>
#include <QVector4D>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace gsw {

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kReferenceGridMinimumVisibleDistance = 10000.0F;
constexpr float kReferenceGridDistanceMultiplier = 256.0F;

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
      QColor(226, 67, 67), QColor(104, 185, 57), QColor(62, 116, 232)};
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
  setMinimumSize(420, 280);

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
}

NativeViewport::~NativeViewport() {
  if (context() != nullptr && context()->isValid()) {
    makeCurrent();
    mGridVertexArray.destroy();
    mGaussianVertexArray.destroy();
    mPointVertexArray.destroy();
    mPointBuffer.destroy();
    doneCurrent();
  }
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
  mRequestedScenePath = scenePath;
  mScenePath = scenePath;
  mPreviewPointCount = 0;
  mRenderedPointCount = 0;
  const bool availabilityChanged = gaussianRenderingAvailable();
  const bool renderModeChangedToPoints = mRenderMode != RenderMode::Points;
  mHasGaussianAttributes = false;
  mRenderMode = RenderMode::Points;
  mSceneLoadMessage.clear();
  mSourcePositions.clear();
  mSourcePositions.squeeze();
  mPreviewVertices.clear();
  mPreviewVertices.squeeze();
  mPendingVertices.clear();
  mPendingVertices.squeeze();
  mSceneCenter = QVector3D(0.0F, 0.0F, 0.0F);
  mSceneRadius = 4.0F;
  mEditModel.reset(0);
  mPointUploadPending = true;
  notifyEditState();
  if (availabilityChanged) {
    emit gaussianRenderingAvailabilityChanged(false);
  }
  if (renderModeChangedToPoints) {
    emit renderModeChanged(mRenderMode);
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
  setCursor(mode == InteractionMode::Rectangle ||
                    mode == InteractionMode::Lasso ||
                    mode == InteractionMode::Brush
                ? Qt::CrossCursor
                : Qt::ArrowCursor);
  update();
}

void NativeViewport::setRenderMode(const RenderMode mode) {
  if (mode == RenderMode::Gaussians && !gaussianRenderingAvailable()) {
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
  mDistance = std::max(mSceneRadius * 2.8F, 0.1F);
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
  return !mScenePath.isEmpty() && mEditModel.pointCount() > 0;
}

bool NativeViewport::gaussianRenderingAvailable() const {
  return mHasGaussianAttributes && mGaussianShaderReady;
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

  // Independently implements Blender-style infinite-grid behavior: a
  // full-screen ray is intersected with this application's fixed Y-up ground
  // plane, so navigation never changes the grid's world-space anchor.
  mGridProgram = new QOpenGLShaderProgram(this);
  const bool gridVertexCompiled =
      mGridProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                            R"GLSL(#version 130
out vec2 clipCoordinate;

void main() {
  vec2 position;
  if (gl_VertexID == 0) {
    position = vec2(-1.0, -1.0);
  } else if (gl_VertexID == 1) {
    position = vec2(3.0, -1.0);
  } else {
    position = vec2(-1.0, 3.0);
  }
  clipCoordinate = position;
  gl_Position = vec4(position, 0.0, 1.0);
}
)GLSL");
  const bool gridFragmentCompiled =
      mGridProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                            R"GLSL(#version 130
in vec2 clipCoordinate;
out vec4 fragmentColor;

uniform mat4 inverseViewProjection;
uniform vec3 cameraWorld;
uniform float cameraDistance;
uniform float gridVisibleDistance;
uniform float uiScale;

vec3 unprojectPoint(float clipDepth) {
  vec4 world = inverseViewProjection *
               vec4(clipCoordinate, clipDepth, 1.0);
  return world.xyz / world.w;
}

float periodicLine(float coordinate, float stepSize, float widthPixels) {
  float normalized = coordinate / stepSize;
  float pixelFootprint = max(fwidth(normalized), 1e-6);
  float lineDistance =
      abs(fract(normalized + 0.5) - 0.5) / pixelFootprint;
  float coverage = 1.0 - smoothstep(widthPixels * 0.5,
                                     widthPixels * 0.5 + 1.0,
                                     lineDistance);
  float frequencyFade = 1.0 - smoothstep(0.32, 0.72, pixelFootprint);
  return coverage * frequencyFade;
}

float gridLines(vec2 coordinate, float stepSize, float widthPixels) {
  return max(periodicLine(coordinate.x, stepSize, widthPixels),
             periodicLine(coordinate.y, stepSize, widthPixels));
}

float originAxis(float coordinate, float widthPixels) {
  float pixelFootprint = max(fwidth(coordinate), 1e-6);
  float lineDistance = abs(coordinate) / pixelFootprint;
  return 1.0 - smoothstep(widthPixels * 0.5,
                          widthPixels * 0.5 + 1.0, lineDistance);
}

void main() {
  vec3 nearPoint = unprojectPoint(-1.0);
  vec3 farPoint = unprojectPoint(1.0);
  vec3 ray = farPoint - nearPoint;
  if (abs(ray.y) < 1e-7) {
    discard;
  }

  float rayParameter = -nearPoint.y / ray.y;
  if (rayParameter <= 0.0) {
    discard;
  }
  // nearPoint/farPoint define a ray, not the grid's extent. Extrapolating
  // beyond the scene far plane keeps the background grid independent from
  // the much tighter depth range used by points and Gaussian splats.
  vec3 world = nearPoint + rayParameter * ray;

  // The scale is uniform for the whole frame. Keeping it out of derivative
  // calculations avoids spatial level boundaries and the moire they create.
  float desiredStep = max(cameraDistance * 0.06, 1e-7);
  float fineStep = pow(10.0, floor(log(desiredStep) / log(10.0)));
  float levelBlend = smoothstep(
      0.12, 0.88,
      clamp(log(desiredStep / fineStep) / log(10.0), 0.0, 1.0));
  float coarseStep = fineStep * 10.0;
  float majorStep = coarseStep * 10.0;

  float fineLines = mix(gridLines(world.xz, fineStep, 0.72 * uiScale),
                        gridLines(world.xz, coarseStep, 0.72 * uiScale),
                        levelBlend);
  float majorLines = mix(gridLines(world.xz, coarseStep, 1.02 * uiScale),
                         gridLines(world.xz, majorStep, 1.02 * uiScale),
                         levelBlend);
  float lineAlpha = max(fineLines * 0.38, majorLines * 0.58);

  float xAxis = originAxis(world.z, 1.35 * uiScale);
  float zAxis = originAxis(world.x, 1.35 * uiScale);
  vec3 minorColor = vec3(0.17, 0.19, 0.20);
  vec3 majorColor = vec3(0.29, 0.31, 0.33);
  vec3 color = mix(minorColor, majorColor, majorLines);
  if (xAxis > 0.0) {
    color = mix(color, vec3(0.72, 0.26, 0.26), xAxis);
    lineAlpha = max(lineAlpha, xAxis * 0.78);
  }
  if (zAxis > 0.0) {
    color = mix(color, vec3(0.25, 0.42, 0.76), zAxis);
    lineAlpha = max(lineAlpha, zAxis * 0.78);
  }

  vec3 toCamera = cameraWorld - world;
  float distanceToCamera = length(toCamera);
  float planeFacing = abs(toCamera.y) / max(distanceToCamera, 1e-6);
  float horizonFade = 1.0 - pow(1.0 - clamp(planeFacing, 0.0, 1.0), 4.0);
  float distanceFade = 1.0 - smoothstep(gridVisibleDistance * 0.5,
                                        gridVisibleDistance,
                                        distanceToCamera);
  float alpha = lineAlpha * horizonFade * distanceFade;
  if (alpha < 0.003) {
    discard;
  }
  fragmentColor = vec4(color, alpha);
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

  if (gaussianRenderingAvailable()) {
    emit gaussianRenderingAvailabilityChanged(true);
    if (mRenderMode != RenderMode::Gaussians) {
      mRenderMode = RenderMode::Gaussians;
      rebuildRenderedVertices();
      emit renderModeChanged(mRenderMode);
    }
  }
  mFrameTimer.start();
}

void NativeViewport::resizeGL(const int width, const int height) {
  const qreal ratio = devicePixelRatioF();
  glViewport(0, 0, qRound(width * ratio), qRound(height * ratio));
}

void NativeViewport::paintGL() {
  QElapsedTimer paintTimer;
  paintTimer.start();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  uploadPendingPointCloud();

  const QMatrix4x4 view = viewMatrix();
  const QMatrix4x4 projection = projectionMatrix();
  const QMatrix4x4 viewProjection = projection * view;
  drawInfiniteGrid(viewProjection);
  if (mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()) {
    drawGaussianCloud(view, projection);
  } else {
    drawPointCloud(viewProjection);
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  if (mPreviewPointCount == 0) {
    drawReferenceAxes(painter, viewProjection);
  }
  drawCameraTrajectory(painter, viewProjection);
  drawSelectionGesture(painter);

  const double frameMilliseconds = paintTimer.nsecsElapsed() / 1000000.0;
  if (mSmoothedFrameMilliseconds <= 0.0) {
    mSmoothedFrameMilliseconds = frameMilliseconds;
  } else {
    mSmoothedFrameMilliseconds =
        mSmoothedFrameMilliseconds * 0.88 + frameMilliseconds * 0.12;
  }
  drawOverlay(painter, mSmoothedFrameMilliseconds);
  drawAxisGizmo(painter);
  painter.end();
  emit frameTimeChanged(mSmoothedFrameMilliseconds);
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
}

void NativeViewport::startSceneLoad(const QString &scenePath) {
  mSceneLoadMessage = QStringLiteral("正在读取点云...");
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

            mSceneCenter = data.center();
            mTarget = mSceneCenter;
            mSceneRadius = data.radius();
            rebuildCameraGeometry();
            mPreviewPointCount = data.vertices.size();
            mSourcePositions = std::move(data.sourcePositions);
            mPreviewVertices = std::move(data.vertices);
            mEditModel.reset(mSourcePositions.size());
            const bool wasAvailable = gaussianRenderingAvailable();
            mHasGaussianAttributes = data.hasGaussianAttributes;
            const bool isAvailable = gaussianRenderingAvailable();
            if (isAvailable != wasAvailable) {
              emit gaussianRenderingAvailabilityChanged(isAvailable);
            }
            const RenderMode loadedMode =
                isAvailable ? RenderMode::Gaussians : RenderMode::Points;
            if (mRenderMode != loadedMode) {
              mRenderMode = loadedMode;
              emit renderModeChanged(mRenderMode);
            }
            mSceneLoadMessage.clear();
            resetCamera();
            notifyEditState();
            emit sceneLoaded(data.sourceVertexCount, mPreviewPointCount);
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

void NativeViewport::drawPointCloud(const QMatrix4x4 &viewProjection) {
  if (mRenderedPointCount <= 0 || mPointProgram == nullptr ||
      !mPointProgram->isLinked()) {
    return;
  }

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  mPointProgram->bind();
  mPointProgram->setUniformValue("viewProjection", viewProjection);
  mPointProgram->setUniformValue("pointSize",
                                 static_cast<float>(2.4 * devicePixelRatioF()));
  {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mPointVertexArray);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(mRenderedPointCount));
  }
  mPointProgram->release();
  glDisable(GL_BLEND);
}

void NativeViewport::drawGaussianCloud(const QMatrix4x4 &view,
                                       const QMatrix4x4 &projection) {
  if (mRenderedPointCount <= 0 || mGaussianProgram == nullptr ||
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
  {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mGaussianVertexArray);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(mRenderedPointCount));
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
  bool invertible = false;
  const QMatrix4x4 inverseViewProjection = viewProjection.inverted(&invertible);
  if (!invertible) {
    return;
  }

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  mGridProgram->bind();
  mGridProgram->setUniformValue("inverseViewProjection", inverseViewProjection);
  mGridProgram->setUniformValue("cameraWorld", cameraPosition());
  mGridProgram->setUniformValue("cameraDistance", mDistance);
  mGridProgram->setUniformValue(
      "gridVisibleDistance",
      std::max(kReferenceGridMinimumVisibleDistance,
               mDistance * kReferenceGridDistanceMultiplier));
  mGridProgram->setUniformValue("uiScale",
                                static_cast<float>(devicePixelRatioF()));
  {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mGridVertexArray);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  mGridProgram->release();
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

void NativeViewport::enterEvent(QEnterEvent *event) {
  if (mMode == InteractionMode::Brush) {
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
    setCursor(mMode == InteractionMode::Rectangle ||
                      mMode == InteractionMode::Lasso ||
                      mMode == InteractionMode::Brush
                  ? Qt::CrossCursor
                  : Qt::ArrowCursor);
    update();
  }
  QOpenGLWidget::leaveEvent(event);
}

void NativeViewport::mousePressEvent(QMouseEvent *event) {
  updateNavigationGizmoHover(event->position());
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
  if (mMode == InteractionMode::Brush) {
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

  if (mMode == InteractionMode::Brush) {
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

  const bool pan = mPressedButtons.testFlag(Qt::MiddleButton) ||
                   mPressedButtons.testFlag(Qt::RightButton) ||
                   event->modifiers().testFlag(Qt::ShiftModifier);
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
  mPressedButtons = event->buttons();
  if (mCameraManipulated && mPressedButtons == Qt::NoButton) {
    mCameraManipulated = false;
    if (mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()) {
      rebuildRenderedVertices();
      update();
    }
  }
  event->accept();
}

void NativeViewport::wheelEvent(QWheelEvent *event) {
  leaveCameraView();
  const float steps = static_cast<float>(event->angleDelta().y()) / 120.0F;
  mDistance = std::clamp(mDistance * std::pow(0.84F, steps), 0.05F, 2500.0F);
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
    tooltip = QStringLiteral("上下拖动缩放视图");
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
    setCursor(mMode == InteractionMode::Rectangle ||
                      mMode == InteractionMode::Lasso ||
                      mMode == InteractionMode::Brush
                  ? Qt::CrossCursor
                  : Qt::ArrowCursor);
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
    mDistance =
        std::clamp(mDistance * std::pow(1.008F, static_cast<float>(delta.y())),
                   0.05F, 2500.0F);
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
      snapToNavigationAxis(pressed.axis);
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

void NativeViewport::snapToNavigationAxis(const NavigationAxis axis) {
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
    mDistance = mStoredCameraView->distance;
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
      std::asin(std::clamp(cameraOffset.y(), -1.0F, 1.0F)) * 180.0F / kPi;
  mYawDegrees = std::atan2(cameraOffset.x(), cameraOffset.z()) * 180.0F / kPi;
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
  const float yaw = radians(mYawDegrees);
  const float pitch = radians(mPitchDegrees);
  const float cosPitch = std::cos(pitch);
  return mTarget + QVector3D(mDistance * cosPitch * std::sin(yaw),
                             mDistance * std::sin(pitch),
                             mDistance * cosPitch * std::cos(yaw));
}

QMatrix4x4 NativeViewport::viewMatrix() const {
  QMatrix4x4 view;
  const QVector3D position = cameraPosition();
  const QVector3D forward = (mTarget - position).normalized();
  QVector3D up(0.0F, 1.0F, 0.0F);
  if (std::abs(QVector3D::dotProduct(forward, up)) > 0.999F) {
    up = mPitchDegrees >= 0.0F ? QVector3D(0.0F, 0.0F, -1.0F)
                               : QVector3D(0.0F, 0.0F, 1.0F);
  }
  view.lookAt(position, mTarget, up);
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
  const auto origin = projectPoint(QVector3D(0.0F, 0.0F, 0.0F), viewProjection);
  const auto xAxis = projectPoint(QVector3D(3.0F, 0.0F, 0.0F), viewProjection);
  const auto yAxis = projectPoint(QVector3D(0.0F, 3.0F, 0.0F), viewProjection);
  const auto zAxis = projectPoint(QVector3D(0.0F, 0.0F, 3.0F), viewProjection);
  if (origin.has_value() && xAxis.has_value()) {
    painter.setPen(QPen(QColor(214, 91, 91), 2.0));
    painter.drawLine(*origin, *xAxis);
  }
  if (origin.has_value() && yAxis.has_value()) {
    painter.setPen(QPen(QColor(91, 191, 137), 2.0));
    painter.drawLine(*origin, *yAxis);
  }
  if (origin.has_value() && zAxis.has_value()) {
    painter.setPen(QPen(QColor(89, 139, 222), 2.0));
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

void NativeViewport::drawOverlay(QPainter &painter,
                                 const double frameMilliseconds) {
  painter.save();
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(17, 19, 21, 225));

  const QString sceneName = mScenePath.isEmpty()
                                ? QStringLiteral("未载入场景")
                                : QFileInfo(mScenePath).fileName();
  const QString project =
      mProjectLabel.isEmpty() ? QStringLiteral("未打开工程") : mProjectLabel;
  QString count;
  if (!mSceneLoadMessage.isEmpty()) {
    count = mSceneLoadMessage;
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
  const QFontMetrics metrics(font());
  const int lineHeight = metrics.height();
  const int headerPaddingX = 12;
  const int headerPaddingY = 8;
  const int lineGap = 2;
  const int widthHint = std::max({metrics.horizontalAdvance(project),
                                  metrics.horizontalAdvance(sceneName),
                                  metrics.horizontalAdvance(count)}) +
                        headerPaddingX * 2 + 4;
  const int headerHeight = headerPaddingY * 2 + lineHeight * 3 + lineGap * 2;
  const QRect headerRect(12, 12,
                         std::clamp(widthHint, 180, qMax(180, width() - 24)),
                         headerHeight);
  painter.drawRoundedRect(headerRect, 4, 4);

  painter.setPen(QColor(232, 235, 236));
  QFont strongFont = font();
  strongFont.setWeight(QFont::DemiBold);
  painter.setFont(strongFont);
  QRect lineRect(headerRect.left() + headerPaddingX,
                 headerRect.top() + headerPaddingY,
                 headerRect.width() - headerPaddingX * 2, lineHeight);
  painter.drawText(
      lineRect, Qt::AlignLeft | Qt::AlignVCenter,
      metrics.elidedText(project, Qt::ElideMiddle, lineRect.width()));
  painter.setFont(font());
  painter.setPen(QColor(174, 181, 185));
  lineRect.translate(0, lineHeight + lineGap);
  painter.drawText(
      lineRect, Qt::AlignLeft | Qt::AlignVCenter,
      metrics.elidedText(sceneName, Qt::ElideMiddle, lineRect.width()));
  painter.setPen(QColor(102, 193, 168));
  lineRect.translate(0, lineHeight + lineGap);
  painter.drawText(lineRect, Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(count, Qt::ElideRight, lineRect.width()));

  const QString renderer =
      mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()
          ? QStringLiteral("原生高斯预览 (DC SH)")
          : QStringLiteral("原生点预览");
  const QString metric = QStringLiteral("%1  |  CPU 提交 %2 ms")
                             .arg(renderer)
                             .arg(frameMilliseconds, 0, 'f', 2);
  const int metricWidth = metrics.horizontalAdvance(metric) + 24;
  const int badgeHeight = std::max(26, lineHeight + 10);
  const QRect metricRect(12, height() - badgeHeight - 12,
                         std::min(metricWidth, width() - 24), badgeHeight);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(17, 19, 21, 225));
  painter.drawRoundedRect(metricRect, 4, 4);
  painter.setPen(QColor(165, 172, 176));
  painter.drawText(
      metricRect.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
      metrics.elidedText(metric, Qt::ElideRight, metricRect.width() - 20));

  const QString mode =
      mSelectionBusy ? QStringLiteral("选择处理中") : modeLabel(mMode);
  const int modeWidth = metrics.horizontalAdvance(mode) + 22;
  const QRect modeRect(width() - modeWidth - 12, 12, modeWidth, badgeHeight);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(49, 93, 88, 235));
  painter.drawRoundedRect(modeRect, 4, 4);
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

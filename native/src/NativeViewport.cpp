#include "NativeViewport.h"

#include "ScreenSpaceSelection.h"

#include <QEnterEvent>
#include <QEvent>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QOpenGLShaderProgram>
#include <QVector2D>
#include <QVector4D>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace gsw {

namespace {
constexpr float kPi = 3.14159265358979323846F;

float radians(const float degrees) { return degrees * kPi / 180.0F; }

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
    return QStringLiteral("%1 M").arg(static_cast<double>(count) / 1000000.0, 0, 'f', 2);
  }
  if (count >= 1000) {
    return QStringLiteral("%1 K").arg(static_cast<double>(count) / 1000.0, 0, 'f', 1);
  }
  return QString::number(count);
}
} // namespace

NativeViewport::NativeViewport(QWidget *parent) : QOpenGLWidget(parent) {
  setObjectName(QStringLiteral("nativeViewport"));
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setMinimumSize(420, 280);
}

NativeViewport::~NativeViewport() {
  if (context() != nullptr && context()->isValid()) {
    makeCurrent();
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

void NativeViewport::setScene(const QString &scenePath, const qint64 gaussianCount) {
  mGaussianCount = gaussianCount;
  if (mRequestedScenePath == scenePath) {
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
  mEditModel.reset(0);
  mPointUploadPending = true;
  notifyEditState();
  if (availabilityChanged) {
    emit gaussianRenderingAvailabilityChanged(false);
  }
  if (renderModeChangedToPoints) {
    emit renderModeChanged(mRenderMode);
  }
  if (scenePath.isEmpty()) {
    mSceneRadius = 4.0F;
    resetCamera();
    return;
  }
  startSceneLoad(scenePath);
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
  mYawDegrees = 42.0F;
  mPitchDegrees = 24.0F;
  mDistance = std::max(mSceneRadius * 2.8F, 0.1F);
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
  if (!PlyPointCloudLoader::writeFiltered(mScenePath, filePath,
                                          mEditModel.deletedBits(),
                                          errorMessage)) {
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

void NativeViewport::initializeGL() {
  initializeOpenGLFunctions();
  glClearColor(0.047F, 0.051F, 0.055F, 1.0F);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_PROGRAM_POINT_SIZE);

  mPointProgram = new QOpenGLShaderProgram(this);
  const bool vertexCompiled = mPointProgram->addShaderFromSourceCode(
      QOpenGLShader::Vertex,
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
  const bool fragmentCompiled = mPointProgram->addShaderFromSourceCode(
      QOpenGLShader::Fragment,
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
    mSceneLoadMessage = QStringLiteral("OpenGL point shader failed: %1").arg(mPointProgram->log());
  }

  mGaussianProgram = new QOpenGLShaderProgram(this);
  const bool gaussianVertexCompiled = mGaussianProgram->addShaderFromSourceCode(
      QOpenGLShader::Vertex,
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
      mGaussianProgram->addShaderFromSourceCode(
          QOpenGLShader::Fragment,
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
    mSceneLoadMessage =
        QStringLiteral("OpenGL Gaussian shader failed: %1")
            .arg(mGaussianProgram->log());
  }

  if (pointShaderReady) {
    mPointBuffer.create();
    mPointBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);

    mPointVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mPointVertexArray);
    mPointBuffer.bind();
    mPointProgram->bind();
    mPointProgram->enableAttributeArray(0);
    mPointProgram->setAttributeBuffer(0, GL_FLOAT, offsetof(PointCloudVertex, x), 3,
                                      sizeof(PointCloudVertex));
    mPointProgram->enableAttributeArray(1);
    mPointProgram->setAttributeBuffer(1, GL_FLOAT, offsetof(PointCloudVertex, red), 3,
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
    mGaussianProgram->setAttributeBuffer(
        0, GL_FLOAT, offsetof(PointCloudVertex, x), 3,
        sizeof(PointCloudVertex));
    mGaussianProgram->enableAttributeArray(1);
    mGaussianProgram->setAttributeBuffer(
        1, GL_FLOAT, offsetof(PointCloudVertex, red), 3,
        sizeof(PointCloudVertex));
    mGaussianProgram->enableAttributeArray(2);
    mGaussianProgram->setAttributeBuffer(
        2, GL_FLOAT, offsetof(PointCloudVertex, opacity), 1,
        sizeof(PointCloudVertex));
    mGaussianProgram->enableAttributeArray(3);
    mGaussianProgram->setAttributeBuffer(
        3, GL_FLOAT, offsetof(PointCloudVertex, scaleX), 3,
        sizeof(PointCloudVertex));
    mGaussianProgram->enableAttributeArray(4);
    mGaussianProgram->setAttributeBuffer(
        4, GL_FLOAT, offsetof(PointCloudVertex, rotationW), 4,
        sizeof(PointCloudVertex));
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
  if (mRenderMode == RenderMode::Gaussians &&
      gaussianRenderingAvailable()) {
    drawGaussianCloud(view, projection);
  } else {
    drawPointCloud(viewProjection);
  }

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  if (mPreviewPointCount == 0) {
    drawGrid(painter, viewProjection);
  }
  drawSelectionGesture(painter);

  const double frameMilliseconds = paintTimer.nsecsElapsed() / 1000000.0;
  if (mSmoothedFrameMilliseconds <= 0.0) {
    mSmoothedFrameMilliseconds = frameMilliseconds;
  } else {
    mSmoothedFrameMilliseconds = mSmoothedFrameMilliseconds * 0.88 + frameMilliseconds * 0.12;
  }
  drawOverlay(painter, mSmoothedFrameMilliseconds);
  drawAxisGizmo(painter);
  painter.end();
  emit frameTimeChanged(mSmoothedFrameMilliseconds);
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
            if (scenePath != mRequestedScenePath || generation != mSceneGeneration) {
              return;
            }
            if (!data.isValid()) {
              mSceneLoadMessage = data.error;
              emit sceneLoadFailed(scenePath, data.error);
              update();
              return;
            }

            mTarget = data.center();
            mSceneRadius = data.radius();
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
    const qsizetype sourceIndex = static_cast<qsizetype>(sourceVertex.sourceIndex);
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
  if (mRenderMode == RenderMode::Gaussians &&
      gaussianRenderingAvailable()) {
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
  const qsizetype byteCount = mPendingVertices.size() * static_cast<qsizetype>(sizeof(PointCloudVertex));
  mPointBuffer.allocate(mPendingVertices.isEmpty() ? nullptr : mPendingVertices.constData(),
                        static_cast<int>(byteCount));
  mPointBuffer.release();
  mPendingVertices.clear();
  mPointUploadPending = false;
}

void NativeViewport::drawPointCloud(const QMatrix4x4 &viewProjection) {
  if (mRenderedPointCount <= 0 || mPointProgram == nullptr || !mPointProgram->isLinked()) {
    return;
  }

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  mPointProgram->bind();
  mPointProgram->setUniformValue("viewProjection", viewProjection);
  mPointProgram->setUniformValue("pointSize", static_cast<float>(2.4 * devicePixelRatioF()));
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
      "viewportPixels",
      QVector2D(static_cast<float>(width() * ratio),
                static_cast<float>(height() * ratio)));
  {
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(
        &mGaussianVertexArray);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(mRenderedPointCount));
  }
  mGaussianProgram->release();
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
  QOpenGLWidget::leaveEvent(event);
}

void NativeViewport::mousePressEvent(QMouseEvent *event) {
  mPressedButtons = event->buttons();
  mLastMousePosition = event->position().toPoint();
  if (mMode == InteractionMode::Brush) {
    mBrushCursorPosition = event->position();
    mBrushCursorVisible = true;
  }
  if (event->button() == Qt::LeftButton && !mSelectionBusy &&
      (mMode == InteractionMode::Rectangle ||
       mMode == InteractionMode::Lasso || mMode == InteractionMode::Brush) &&
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
  if (mMode == InteractionMode::Brush) {
    mBrushCursorPosition = event->position();
    mBrushCursorVisible = true;
  }
  if (mSelectionGestureActive) {
    mSelectionCurrent = event->position();
    if ((mMode == InteractionMode::Lasso ||
         mMode == InteractionMode::Brush) &&
        (mSelectionPath.isEmpty() ||
         QLineF(mSelectionPath.last(), event->position()).length() >= 2.0)) {
      mSelectionPath.append(event->position());
    }
    update();
    event->accept();
    return;
  }
  if (mPressedButtons == Qt::NoButton) {
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
  }

  const bool pan = mPressedButtons.testFlag(Qt::MiddleButton) ||
                   mPressedButtons.testFlag(Qt::RightButton) ||
                   event->modifiers().testFlag(Qt::ShiftModifier);
  if (pan) {
    const QVector3D forward = (mTarget - cameraPosition()).normalized();
    const QVector3D right = QVector3D::crossProduct(forward, QVector3D(0.0F, 1.0F, 0.0F)).normalized();
    const QVector3D up = QVector3D::crossProduct(right, forward).normalized();
    const float scale = mDistance * 0.0018F;
    mTarget += right * (-static_cast<float>(delta.x()) * scale);
    mTarget += up * (static_cast<float>(delta.y()) * scale);
  } else if (mPressedButtons.testFlag(Qt::LeftButton)) {
    mYawDegrees += static_cast<float>(delta.x()) * 0.32F;
    mPitchDegrees = std::clamp(mPitchDegrees + static_cast<float>(delta.y()) * 0.28F, -86.0F, 86.0F);
  }

  update();
  event->accept();
}

void NativeViewport::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && mSelectionGestureActive) {
    mSelectionCurrent = event->position();
    if ((mMode == InteractionMode::Lasso ||
         mMode == InteractionMode::Brush) &&
        (mSelectionPath.isEmpty() ||
         mSelectionPath.last() != event->position())) {
      mSelectionPath.append(event->position());
    }
    finishSelectionGesture(event->modifiers());
  }
  mPressedButtons = event->buttons();
  if (mCameraManipulated && mPressedButtons == Qt::NoButton) {
    mCameraManipulated = false;
    if (mRenderMode == RenderMode::Gaussians &&
        gaussianRenderingAvailable()) {
      rebuildRenderedVertices();
      update();
    }
  }
  event->accept();
}

void NativeViewport::wheelEvent(QWheelEvent *event) {
  const float steps = static_cast<float>(event->angleDelta().y()) / 120.0F;
  mDistance = std::clamp(mDistance * std::pow(0.84F, steps), 0.05F, 2500.0F);
  update();
  event->accept();
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
  view.lookAt(cameraPosition(), mTarget, QVector3D(0.0F, 1.0F, 0.0F));
  return view;
}

QMatrix4x4 NativeViewport::projectionMatrix() const {
  QMatrix4x4 projection;
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height())
                   : 1.0F;
  const float nearPlane = std::max(0.001F, mDistance / 10000.0F);
  const float farPlane =
      std::max(100.0F, mDistance + mSceneRadius * 12.0F);
  projection.perspective(46.0F, aspect, nearPlane, farPlane);
  return projection;
}

QMatrix4x4 NativeViewport::viewProjectionMatrix() const {
  return projectionMatrix() * viewMatrix();
}

std::optional<QPointF> NativeViewport::projectPoint(const QVector3D &point,
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

void NativeViewport::drawGrid(QPainter &painter, const QMatrix4x4 &viewProjection) {
  constexpr int extent = 20;
  for (int value = -extent; value <= extent; ++value) {
    const bool major = value % 5 == 0;
    QColor color = major ? QColor(63, 68, 72, 205) : QColor(43, 47, 50, 165);
    if (value == 0) {
      color = QColor(73, 79, 83, 230);
    }
    painter.setPen(QPen(color, major ? 1.1 : 0.7));

    const auto xStart = projectPoint(QVector3D(static_cast<float>(-extent), 0.0F, static_cast<float>(value)), viewProjection);
    const auto xEnd = projectPoint(QVector3D(static_cast<float>(extent), 0.0F, static_cast<float>(value)), viewProjection);
    if (xStart.has_value() && xEnd.has_value()) {
      painter.drawLine(*xStart, *xEnd);
    }

    const auto zStart = projectPoint(QVector3D(static_cast<float>(value), 0.0F, static_cast<float>(-extent)), viewProjection);
    const auto zEnd = projectPoint(QVector3D(static_cast<float>(value), 0.0F, static_cast<float>(extent)), viewProjection);
    if (zStart.has_value() && zEnd.has_value()) {
      painter.drawLine(*zStart, *zEnd);
    }
  }

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
      painter.setPen(QPen(QColor(124, 219, 191, 205), 1.2,
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
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

void NativeViewport::drawOverlay(QPainter &painter, const double frameMilliseconds) {
  painter.save();
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(17, 19, 21, 225));

  const QString sceneName = mScenePath.isEmpty() ? QStringLiteral("未载入场景") : QFileInfo(mScenePath).fileName();
  const QString project = mProjectLabel.isEmpty() ? QStringLiteral("未打开工程") : mProjectLabel;
  QString count;
  if (!mSceneLoadMessage.isEmpty()) {
    count = mSceneLoadMessage;
  } else if (mGaussianCount > 0 && mPreviewPointCount > 0 && mGaussianCount != mPreviewPointCount) {
    count = QStringLiteral("%1 %2 | 预览 %3")
                .arg(formatCount(mGaussianCount),
                     mHasGaussianAttributes ? QStringLiteral("高斯")
                                            : QStringLiteral("点"),
                     formatCount(mPreviewPointCount));
  } else if (mGaussianCount > 0) {
    count = QStringLiteral("%1 %2")
                .arg(formatCount(mGaussianCount),
                     mHasGaussianAttributes ? QStringLiteral("高斯")
                                            : QStringLiteral("点"));
  } else {
    count = QStringLiteral("场景数据待载入");
  }
  if (mSelectionBusy) {
    count += QStringLiteral(" | 正在计算选择");
  } else if (mEditModel.selectedCount() > 0 || mEditModel.deletedCount() > 0) {
    count += QStringLiteral(" | 已选 %1 | 已删 %2")
                 .arg(formatCount(mEditModel.selectedCount()),
                      formatCount(mEditModel.deletedCount()));
  }
  const QFontMetrics metrics(font());
  const int widthHint = std::max({metrics.horizontalAdvance(project), metrics.horizontalAdvance(sceneName),
                                  metrics.horizontalAdvance(count)}) + 28;
  const QRect headerRect(12, 12, std::clamp(widthHint, 180, qMax(180, width() - 24)), 70);
  painter.drawRoundedRect(headerRect, 4, 4);

  painter.setPen(QColor(232, 235, 236));
  QFont strongFont = font();
  strongFont.setWeight(QFont::DemiBold);
  painter.setFont(strongFont);
  painter.drawText(headerRect.adjusted(12, 8, -12, -42), Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(project, Qt::ElideMiddle, headerRect.width() - 24));
  painter.setFont(font());
  painter.setPen(QColor(174, 181, 185));
  painter.drawText(headerRect.adjusted(12, 30, -12, -20), Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(sceneName, Qt::ElideMiddle, headerRect.width() - 24));
  painter.setPen(QColor(102, 193, 168));
  painter.drawText(headerRect.adjusted(12, 49, -12, -4), Qt::AlignLeft | Qt::AlignVCenter, count);

  const QString renderer =
      mRenderMode == RenderMode::Gaussians && gaussianRenderingAvailable()
          ? QStringLiteral("原生高斯预览 (DC SH)")
          : QStringLiteral("原生点预览");
  const QString metric = QStringLiteral("%1  |  CPU 提交 %2 ms")
                             .arg(renderer)
                             .arg(frameMilliseconds, 0, 'f', 2);
  const int metricWidth = metrics.horizontalAdvance(metric) + 24;
  const QRect metricRect(12, height() - 38, std::min(metricWidth, width() - 24), 26);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(17, 19, 21, 225));
  painter.drawRoundedRect(metricRect, 4, 4);
  painter.setPen(QColor(165, 172, 176));
  painter.drawText(metricRect.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                   metrics.elidedText(metric, Qt::ElideRight, metricRect.width() - 20));

  const QString mode = mSelectionBusy ? QStringLiteral("选择处理中") : modeLabel(mMode);
  const int modeWidth = metrics.horizontalAdvance(mode) + 22;
  const QRect modeRect(width() - modeWidth - 12, 12, modeWidth, 26);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(49, 93, 88, 235));
  painter.drawRoundedRect(modeRect, 4, 4);
  painter.setPen(QColor(238, 246, 244));
  painter.drawText(modeRect, Qt::AlignCenter, mode);

  painter.restore();
}

void NativeViewport::drawAxisGizmo(QPainter &painter) {
  painter.save();
  const QPointF origin(width() - 54.0, height() - 52.0);
  painter.setRenderHint(QPainter::Antialiasing, true);

  painter.setPen(QPen(QColor(214, 91, 91), 2.2));
  painter.drawLine(origin, origin + QPointF(30.0, 8.0));
  painter.setPen(QColor(235, 120, 120));
  painter.drawText(origin + QPointF(34.0, 13.0), QStringLiteral("X"));

  painter.setPen(QPen(QColor(91, 191, 137), 2.2));
  painter.drawLine(origin, origin + QPointF(0.0, -31.0));
  painter.setPen(QColor(117, 213, 158));
  painter.drawText(origin + QPointF(-4.0, -36.0), QStringLiteral("Y"));

  painter.setPen(QPen(QColor(89, 139, 222), 2.2));
  painter.drawLine(origin, origin + QPointF(-18.0, 17.0));
  painter.setPen(QColor(116, 160, 233));
  painter.drawText(origin + QPointF(-29.0, 27.0), QStringLiteral("Z"));
  painter.restore();
}

} // namespace gsw

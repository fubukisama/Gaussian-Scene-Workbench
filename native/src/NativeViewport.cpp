#include "NativeViewport.h"

#include <QFileInfo>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QOpenGLShaderProgram>
#include <QVector4D>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>

namespace gsw {

namespace {
constexpr float kPi = 3.14159265358979323846F;
constexpr float kVisibleDepthTolerance = 0.006F;
constexpr int kMaximumDepthGridDimension = 1280;

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

struct ProjectedSourcePoint {
  float x = 0.0F;
  float y = 0.0F;
  float depth = 0.0F;
};

bool projectSourcePoint(const PointPosition &point, const QMatrix4x4 &viewProjection,
                        const QSize &viewport, ProjectedSourcePoint &projected) {
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
  if (normalizedX < -1.0F || normalizedX > 1.0F || normalizedY < -1.0F ||
      normalizedY > 1.0F || normalizedZ < -1.0F || normalizedZ > 1.0F) {
    return false;
  }
  projected.x = (normalizedX * 0.5F + 0.5F) * viewport.width();
  projected.y = (1.0F - (normalizedY * 0.5F + 0.5F)) * viewport.height();
  projected.depth = normalizedZ;
  return true;
}

bool pointInsidePolygon(const QPointF &point, const QPolygonF &polygon) {
  if (polygon.size() < 3) {
    return false;
  }
  bool inside = false;
  qsizetype previous = polygon.size() - 1;
  for (qsizetype current = 0; current < polygon.size(); ++current) {
    const QPointF &a = polygon.at(current);
    const QPointF &b = polygon.at(previous);
    const bool crosses = (a.y() > point.y()) != (b.y() > point.y());
    if (crosses) {
      const qreal intersectionX =
          (b.x() - a.x()) * (point.y() - a.y()) / (b.y() - a.y()) + a.x();
      if (point.x() < intersectionX) {
        inside = !inside;
      }
    }
    previous = current;
  }
  return inside;
}

QVector<quint32> selectSourcePoints(const QVector<PointPosition> &positions,
                                    const QBitArray &deleted,
                                    const QMatrix4x4 &viewProjection,
                                    const QSize &viewport,
                                    const QRectF &selectionRectangle,
                                    const QPolygonF &selectionLasso,
                                    const bool visibleOnly) {
  QVector<quint32> matches;
  matches.reserve(std::min<qsizetype>(positions.size() / 10, 1'000'000));
  if (positions.isEmpty() || viewport.isEmpty()) {
    return matches;
  }

  const bool useLasso = selectionLasso.size() >= 3;
  const QRectF lassoBounds = selectionLasso.boundingRect();
  auto contains = [&](const ProjectedSourcePoint &point) {
    const QPointF screenPoint(point.x, point.y);
    if (!useLasso) {
      return selectionRectangle.contains(screenPoint);
    }
    return lassoBounds.contains(screenPoint) &&
           pointInsidePolygon(screenPoint, selectionLasso);
  };

  int gridWidth = 0;
  int gridHeight = 0;
  QVector<float> depthGrid;
  if (visibleOnly) {
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
      if (deleted.testBit(index)) {
        continue;
      }
      ProjectedSourcePoint point;
      if (!projectSourcePoint(positions.at(index), viewProjection, viewport, point)) {
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
    if (deleted.testBit(index)) {
      continue;
    }
    ProjectedSourcePoint point;
    if (!projectSourcePoint(positions.at(index), viewProjection, viewport, point) ||
        !contains(point)) {
      continue;
    }
    if (visibleOnly) {
      const int gridX = std::clamp(
          static_cast<int>(point.x / viewport.width() * gridWidth), 0,
          gridWidth - 1);
      const int gridY = std::clamp(
          static_cast<int>(point.y / viewport.height() * gridHeight), 0,
          gridHeight - 1);
      if (point.depth > depthGrid.at(gridY * gridWidth + gridX) +
                            kVisibleDepthTolerance) {
        continue;
      }
    }
    matches.append(static_cast<quint32>(index));
  }
  return matches;
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
  mRequestedScenePath = scenePath;
  mScenePath = scenePath;
  mPreviewPointCount = 0;
  mRenderedPointCount = 0;
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
  mSelectionLasso.clear();
  setCursor(mode == InteractionMode::Rectangle || mode == InteractionMode::Lasso
                ? Qt::CrossCursor
                : Qt::ArrowCursor);
  update();
}

void NativeViewport::setVisibleOnlySelection(const bool enabled) {
  mVisibleOnlySelection = enabled;
}

void NativeViewport::resetCamera() {
  mYawDegrees = 42.0F;
  mPitchDegrees = 24.0F;
  mDistance = std::max(mSceneRadius * 2.8F, 0.1F);
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
  const bool shaderReady = vertexCompiled && fragmentCompiled && mPointProgram->link();
  if (!shaderReady) {
    mSceneLoadMessage = QStringLiteral("OpenGL point shader failed: %1").arg(mPointProgram->log());
  } else {
    mPointVertexArray.create();
    QOpenGLVertexArrayObject::Binder vertexArrayBinder(&mPointVertexArray);
    mPointBuffer.create();
    mPointBuffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
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

  const QMatrix4x4 viewProjection = viewProjectionMatrix();
  drawPointCloud(viewProjection);

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
            rebuildRenderedVertices();
            mSceneLoadMessage.clear();
            resetCamera();
            notifyEditState();
            emit sceneLoaded(data.sourceVertexCount, mPreviewPointCount);
          });
  watcher->setFuture(QtConcurrent::run(
      [scenePath]() { return PlyPointCloudLoader::load(scenePath); }));
}

void NativeViewport::startSelection(const QRectF &rectangle,
                                    const QPolygonF &lasso,
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
  const bool visibleOnly = mVisibleOnlySelection;

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
      [positions, deleted, viewProjection, viewportSize, rectangle, lasso,
       visibleOnly]() {
        return selectSourcePoints(positions, deleted, viewProjection,
                                  viewportSize, rectangle, lasso, visibleOnly);
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
  QPolygonF lasso = mSelectionLasso;
  mSelectionLasso.clear();

  if (mMode == InteractionMode::Rectangle && rectangle.width() < 3.0 &&
      rectangle.height() < 3.0) {
    rectangle = QRectF(mSelectionCurrent - QPointF(4.0, 4.0), QSizeF(8.0, 8.0));
  }
  if (mMode == InteractionMode::Lasso && lasso.size() < 3) {
    update();
    return;
  }

  SelectionOperation operation = SelectionOperation::Replace;
  if (modifiers.testFlag(Qt::ShiftModifier)) {
    operation = SelectionOperation::Add;
  } else if (modifiers.testFlag(Qt::AltModifier)) {
    operation = SelectionOperation::Subtract;
  }
  startSelection(rectangle, lasso, operation);
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
    }
    mPendingVertices.append(renderedVertex);
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

void NativeViewport::mousePressEvent(QMouseEvent *event) {
  mPressedButtons = event->buttons();
  mLastMousePosition = event->position().toPoint();
  if (event->button() == Qt::LeftButton && !mSelectionBusy &&
      (mMode == InteractionMode::Rectangle || mMode == InteractionMode::Lasso) &&
      hasEditableScene()) {
    mSelectionGestureActive = true;
    mSelectionStart = event->position();
    mSelectionCurrent = event->position();
    mSelectionLasso.clear();
    if (mMode == InteractionMode::Lasso) {
      mSelectionLasso.append(event->position());
    }
    update();
  }
  event->accept();
}

void NativeViewport::mouseMoveEvent(QMouseEvent *event) {
  if (mSelectionGestureActive) {
    mSelectionCurrent = event->position();
    if (mMode == InteractionMode::Lasso &&
        (mSelectionLasso.isEmpty() ||
         QLineF(mSelectionLasso.last(), event->position()).length() >= 3.0)) {
      mSelectionLasso.append(event->position());
    }
    update();
    event->accept();
    return;
  }
  if (mPressedButtons == Qt::NoButton) {
    QOpenGLWidget::mouseMoveEvent(event);
    return;
  }

  const QPoint current = event->position().toPoint();
  const QPoint delta = current - mLastMousePosition;
  mLastMousePosition = current;

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
    if (mMode == InteractionMode::Lasso) {
      mSelectionLasso.append(event->position());
    }
    finishSelectionGesture(event->modifiers());
  }
  mPressedButtons = event->buttons();
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

QMatrix4x4 NativeViewport::viewProjectionMatrix() const {
  QMatrix4x4 projection;
  const float aspect =
      height() > 0 ? static_cast<float>(width()) / static_cast<float>(height())
                   : 1.0F;
  const float nearPlane = std::max(0.001F, mDistance / 10000.0F);
  const float farPlane =
      std::max(100.0F, mDistance + mSceneRadius * 12.0F);
  projection.perspective(46.0F, aspect, nearPlane, farPlane);

  QMatrix4x4 view;
  view.lookAt(cameraPosition(), mTarget, QVector3D(0.0F, 1.0F, 0.0F));
  return projection * view;
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
  if (!mSelectionGestureActive) {
    return;
  }

  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(QColor(102, 193, 168), 1.5, Qt::SolidLine));
  painter.setBrush(QColor(102, 193, 168, 36));
  if (mMode == InteractionMode::Rectangle) {
    painter.drawRect(QRectF(mSelectionStart, mSelectionCurrent).normalized());
  } else if (mMode == InteractionMode::Lasso && !mSelectionLasso.isEmpty()) {
    QPainterPath path;
    path.moveTo(mSelectionLasso.first());
    for (qsizetype index = 1; index < mSelectionLasso.size(); ++index) {
      path.lineTo(mSelectionLasso.at(index));
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
    count = QStringLiteral("%1 点 | 预览 %2").arg(formatCount(mGaussianCount), formatCount(mPreviewPointCount));
  } else if (mGaussianCount > 0) {
    count = QStringLiteral("%1 点").arg(formatCount(mGaussianCount));
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

  const QString metric = QStringLiteral("原生点云预览  |  帧处理 %1 ms")
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

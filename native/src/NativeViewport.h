#pragma once

#include "CameraTrajectory.h"
#include "NavigationGizmo.h"
#include "PlyPointCloudLoader.h"
#include "SceneEditModel.h"
#include "ScreenSpaceSelection.h"

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QVector3D>

#include <optional>

class QMouseEvent;
class QPainter;
class QOpenGLShaderProgram;
class QEnterEvent;
class QEvent;
class QWheelEvent;
class QVariantAnimation;

namespace gsw {

class NativeViewport final : public QOpenGLWidget,
                             protected QOpenGLExtraFunctions {
  Q_OBJECT

public:
  enum class InteractionMode { Inspect, Select, Rectangle, Lasso, Brush, Crop };

  enum class RenderMode { Points, Gaussians };
  Q_ENUM(RenderMode)

  explicit NativeViewport(QWidget *parent = nullptr);
  ~NativeViewport() override;

  void setProjectLabel(const QString &label);
  void setScene(const QString &scenePath, qint64 gaussianCount);
  void setShowCameras(bool enabled);
  void setInteractionMode(InteractionMode mode);
  void setRenderMode(RenderMode mode);
  void setVisibleOnlySelection(bool enabled);
  void setBrushRadius(int pixels);
  void resetCamera();
  void clearSelection();
  void invertSelection();
  void deleteSelection();
  void undoEdit();
  void redoEdit();

  [[nodiscard]] bool saveCroppedScene(const QString &filePath,
                                      QString *errorMessage = nullptr);
  [[nodiscard]] bool hasUnsavedSceneEdits() const;
  [[nodiscard]] bool hasEditableScene() const;
  [[nodiscard]] bool gaussianRenderingAvailable() const;
  [[nodiscard]] bool camerasAvailable() const;
  [[nodiscard]] qsizetype cameraCount() const;
  [[nodiscard]] RenderMode renderMode() const { return mRenderMode; }
  [[nodiscard]] bool infiniteGridRenderingAvailable() const;
  [[nodiscard]] static QVector3D referenceGridOrigin() {
    return QVector3D(0.0F, 0.0F, 0.0F);
  }

signals:
  void frameTimeChanged(double milliseconds);
  void sceneLoadStarted(const QString &scenePath);
  void sceneLoaded(qint64 sourceVertexCount, qsizetype previewVertexCount);
  void sceneLoadFailed(const QString &scenePath, const QString &message);
  void editStateChanged(qsizetype selectedCount, qsizetype deletedCount,
                        bool canUndo, bool canRedo, bool sceneReady,
                        bool hasUnsavedChanges);
  void selectionBusyChanged(bool busy);
  void gaussianRenderingAvailabilityChanged(bool available);
  void renderModeChanged(gsw::NativeViewport::RenderMode mode);
  void cameraTrajectoryChanged(qsizetype cameraCount,
                               qsizetype invalidCameraCount,
                               bool displayDecimated, const QString &sourcePath,
                               const QString &error);

protected:
  void initializeGL() override;
  void resizeGL(int width, int height) override;
  void paintGL() override;
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  struct StoredCameraView {
    QVector3D target;
    float yawDegrees = 0.0F;
    float pitchDegrees = 0.0F;
    float distance = 0.0F;
    bool orthographic = false;
  };

  [[nodiscard]] QVector3D cameraPosition() const;
  [[nodiscard]] QMatrix4x4 viewMatrix() const;
  [[nodiscard]] QMatrix4x4 projectionMatrix() const;
  [[nodiscard]] QMatrix4x4 viewProjectionMatrix() const;
  [[nodiscard]] std::optional<QPointF>
  projectPoint(const QVector3D &point, const QMatrix4x4 &viewProjection) const;
  void reloadCameraTrajectory(const QString &scenePath, bool clearExisting);
  void rebuildCameraGeometry();
  void startSceneLoad(const QString &scenePath);
  void startSelection(const ScreenSelectionRequest &request,
                      SelectionOperation operation);
  void finishSelectionGesture(Qt::KeyboardModifiers modifiers);
  void rebuildRenderedVertices();
  void notifyEditState();
  void uploadPendingPointCloud();
  void drawPointCloud(const QMatrix4x4 &viewProjection);
  void drawGaussianCloud(const QMatrix4x4 &view, const QMatrix4x4 &projection);
  void drawInfiniteGrid(const QMatrix4x4 &viewProjection);
  void drawReferenceAxes(QPainter &painter, const QMatrix4x4 &viewProjection);
  void drawCameraTrajectory(QPainter &painter,
                            const QMatrix4x4 &viewProjection);
  void drawSelectionGesture(QPainter &painter);
  void drawOverlay(QPainter &painter, double frameMilliseconds);
  void drawAxisGizmo(QPainter &painter);
  [[nodiscard]] NavigationGizmoLayout navigationGizmo() const;
  void updateNavigationGizmoHover(const QPointF &position);
  void updateNavigationGizmoInteraction(const QPoint &current);
  void finishNavigationGizmoInteraction();
  void snapToNavigationAxis(NavigationAxis axis);
  void panCamera(const QPoint &delta);
  void toggleCameraView();
  void leaveCameraView();

  QString mProjectLabel;
  QString mScenePath;
  QString mRequestedScenePath;
  QString mSceneLoadMessage;
  qint64 mGaussianCount = 0;
  qsizetype mPreviewPointCount = 0;
  qsizetype mRenderedPointCount = 0;
  InteractionMode mMode = InteractionMode::Inspect;
  QPoint mLastMousePosition;
  Qt::MouseButtons mPressedButtons = Qt::NoButton;
  QPointF mSelectionStart;
  QPointF mSelectionCurrent;
  QPolygonF mSelectionPath;
  QPointF mBrushCursorPosition;
  qreal mBrushRadius = 32.0;
  bool mSelectionGestureActive = false;
  bool mBrushCursorVisible = false;
  bool mVisibleOnlySelection = true;
  bool mSelectionBusy = false;
  bool mCameraManipulated = false;
  bool mNavigationInteractionActive = false;
  bool mNavigationDragging = false;
  bool mOrthographic = false;
  bool mCameraViewActive = false;
  bool mShowCameras = false;
  bool mHasGaussianAttributes = false;
  bool mGaussianShaderReady = false;
  bool mGridShaderReady = false;
  int mSceneGeneration = 0;
  int mCameraTrajectoryGeneration = 0;
  RenderMode mRenderMode = RenderMode::Points;
  QVector3D mSceneCenter = QVector3D(0.0F, 0.0F, 0.0F);
  QVector3D mTarget = QVector3D(0.0F, 0.0F, 0.0F);
  float mYawDegrees = 42.0F;
  float mPitchDegrees = 24.0F;
  float mDistance = 12.0F;
  float mSceneRadius = 4.0F;
  NavigationGizmoHit mNavigationHover;
  NavigationGizmoHit mNavigationPress;
  QPoint mNavigationPressPosition;
  std::optional<StoredCameraView> mStoredCameraView;
  QVector<PointPosition> mSourcePositions;
  QVector<PointCloudVertex> mPreviewVertices;
  QVector<PointCloudVertex> mPendingVertices;
  CameraTrajectory mCameraTrajectory;
  CameraTrajectoryGeometry mCameraGeometry;
  SceneEditModel mEditModel;
  bool mPointUploadPending = false;
  QOpenGLShaderProgram *mPointProgram = nullptr;
  QOpenGLShaderProgram *mGaussianProgram = nullptr;
  QOpenGLShaderProgram *mGridProgram = nullptr;
  QOpenGLBuffer mPointBuffer{QOpenGLBuffer::VertexBuffer};
  QOpenGLVertexArrayObject mPointVertexArray;
  QOpenGLVertexArrayObject mGaussianVertexArray;
  QOpenGLVertexArrayObject mGridVertexArray;
  QVariantAnimation *mViewSnapAnimation = nullptr;
  QElapsedTimer mFrameTimer;
  double mSmoothedFrameMilliseconds = 0.0;
};

} // namespace gsw

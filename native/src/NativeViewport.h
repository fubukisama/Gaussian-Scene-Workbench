#pragma once

#include "CameraTrajectory.h"
#include "FrameRateCounter.h"
#include "ModelInteraction.h"
#include "NavigationGizmo.h"
#include "PlyPointCloudLoader.h"
#include "SceneEditModel.h"
#include "ScreenSpaceSelection.h"
#include "TrainingGpuPreviewBuffer.h"
#include "TransformGizmo.h"

#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QQuaternion>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVector3D>

#include <optional>

class QMouseEvent;
class QPainter;
class QOpenGLShaderProgram;
class QEnterEvent;
class QEvent;
class QKeyEvent;
class QWheelEvent;
class QVariantAnimation;
class QTimer;

namespace gsw {

enum class ReferenceGridPlane;

class NativeViewport final : public QOpenGLWidget,
                             protected QOpenGLExtraFunctions {
  Q_OBJECT

public:
  enum class InteractionMode {
    Inspect,
    Move,
    Rotate,
    Scale,
    Select,
    Rectangle,
    Lasso,
    Brush,
    Crop
  };
  Q_ENUM(InteractionMode)

  enum class RenderMode { Points, Mesh, Gaussians };
  Q_ENUM(RenderMode)

  enum class ReferencePlaneMode { ModelBase, WorldZero };
  Q_ENUM(ReferencePlaneMode)

  explicit NativeViewport(QWidget *parent = nullptr);
  ~NativeViewport() override;

  void setProjectLabel(const QString &label);
  void setScene(const QString &scenePath, qint64 gaussianCount);
  void setShowCameras(bool enabled);
  void setInteractionMode(InteractionMode mode);
  void selectModel();
  void selectModelForMove();
  void selectModelForRotate();
  void selectModelForScale();
  void setModelGizmoMode(TransformGizmoMode mode);
  void setModelTransform(const QVector3D &translation,
                         const QQuaternion &rotation,
                         const QVector3D &scale =
                             QVector3D(1.0F, 1.0F, 1.0F));
  void setModelTranslation(const QVector3D &translation);
  void setRenderMode(RenderMode mode);
  void setReferencePlaneMode(ReferencePlaneMode mode);
  void setVisibleOnlySelection(bool enabled);
  void setBrushRadius(int pixels);
  void setAxisView(NavigationAxis axis);
  void resetCamera();
  void clearSelection();
  void invertSelection();
  void deleteSelection();
  void undoEdit();
  void redoEdit();
  void setTrainingGpuPreviewDescriptor(
      const TrainingGpuPreviewDescriptor &descriptor);
  void stopTrainingGpuPreview();

  [[nodiscard]] bool saveCroppedScene(const QString &filePath,
                                      QString *errorMessage = nullptr);
  [[nodiscard]] bool hasUnsavedSceneEdits() const;
  [[nodiscard]] bool hasEditableScene() const;
  [[nodiscard]] bool gaussianRenderingAvailable() const;
  [[nodiscard]] bool meshRenderingAvailable() const;
  [[nodiscard]] qsizetype residentMeshTriangleCount() const {
    return mUploadedFullResolutionMeshTriangleCount;
  }
  [[nodiscard]] bool meshTextureAvailable() const {
    return mMeshTextureReady;
  }
  [[nodiscard]] bool camerasAvailable() const;
  [[nodiscard]] qsizetype cameraCount() const;
  [[nodiscard]] RenderMode renderMode() const { return mRenderMode; }
  [[nodiscard]] ReferencePlaneMode referencePlaneMode() const {
    return mReferencePlaneMode;
  }
  [[nodiscard]] const SceneCoordinateInfo &sceneCoordinates() const {
    return mSceneCoordinates;
  }
  [[nodiscard]] QString scenePath() const { return mScenePath; }
  [[nodiscard]] QVector3D modelTranslation() const {
    return mModelTranslation;
  }
  [[nodiscard]] QQuaternion modelRotation() const { return mModelRotation; }
  [[nodiscard]] QVector3D modelScale() const { return mModelScale; }
  [[nodiscard]] TransformGizmoMode modelGizmoMode() const {
    return mTransformGizmoMode;
  }
  [[nodiscard]] bool modelTransformActive() const {
    return mModelDragActive;
  }
  [[nodiscard]] bool modelSelected() const { return mModelSelected; }
  [[nodiscard]] bool selectableModelAvailable() const;
  [[nodiscard]] double referencePlaneElevation() const;
  [[nodiscard]] QString referencePlaneDescription() const;
  [[nodiscard]] bool infiniteGridRenderingAvailable() const;
  [[nodiscard]] TrainingGpuPreviewCapability
  trainingGpuPreviewCapability() const {
    return mTrainingGpuPreviewCapability;
  }
  [[nodiscard]] bool trainingGpuPreviewActive() const {
    return mTrainingGpuPreview.attached();
  }
  [[nodiscard]] static QVector3D referenceGridOrigin() {
    return QVector3D(0.0F, 0.0F, 0.0F);
  }

signals:
  void frameMetricsChanged(double framesPerSecond,
                           double averageFrameMilliseconds);
  void sceneLoadStarted(const QString &scenePath);
  void sceneLoaded(qint64 sourceVertexCount, qsizetype previewVertexCount,
                   qint64 sourceFaceCount, qsizetype previewTriangleCount);
  void sceneLoadFailed(const QString &scenePath, const QString &message);
  void editStateChanged(qsizetype selectedCount, qsizetype deletedCount,
                        bool canUndo, bool canRedo, bool sceneReady,
                        bool hasUnsavedChanges);
  void selectionBusyChanged(bool busy);
  void gaussianRenderingAvailabilityChanged(bool available);
  void meshRenderingAvailabilityChanged(bool available);
  void renderModeChanged(gsw::NativeViewport::RenderMode mode);
  void sceneCoordinatesChanged();
  void referencePlaneModeChanged(
      gsw::NativeViewport::ReferencePlaneMode mode);
  void cameraTrajectoryChanged(qsizetype cameraCount,
                               qsizetype invalidCameraCount,
                               bool displayDecimated, const QString &sourcePath,
                               const QString &error);
  void trainingGpuPreviewStateChanged(bool active, const QString &mode,
                                      const QString &detail);
  void interactionModeChanged(gsw::NativeViewport::InteractionMode mode);
  void modelInteractionStateChanged(bool available, bool selected,
                                    bool canUndo, bool canRedo,
                                    const QVector3D &translation,
                                    const QQuaternion &rotation,
                                    const QVector3D &scale);
  void modelTransformCommitted(const QVector3D &translation,
                               const QQuaternion &rotation,
                               const QVector3D &scale);

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
  void keyPressEvent(QKeyEvent *event) override;
  void keyReleaseEvent(QKeyEvent *event) override;

private:
  struct FullResolutionGpuChunk {
    int nodeId = -1;
    GLuint vertexArray = 0;
    GLuint buffer = 0;
    GLsizei pointCount = 0;
    qsizetype byteCount = 0;
    quint64 lastUsedFrame = 0;
    QVector3D boundsMinimum;
    QVector3D boundsMaximum;
  };

  struct FullResolutionMeshGpuChunk {
    int nodeId = -1;
    GLuint vertexArray = 0;
    GLuint vertexBuffer = 0;
    GLuint indexBuffer = 0;
    GLsizei indexCount = 0;
    qsizetype byteCount = 0;
    quint64 lastUsedFrame = 0;
    QVector3D boundsMinimum;
    QVector3D boundsMaximum;
  };

  struct StoredCameraView {
    QVector3D target;
    float yawDegrees = 0.0F;
    float pitchDegrees = 0.0F;
    float distance = 0.0F;
    bool orthographic = false;
  };

  enum class TransformConstraintKind { None, Axis, Plane };

  struct TransformConstraint final {
    TransformConstraintKind kind = TransformConstraintKind::None;
    int axis = -1;
    bool local = false;
  };

  [[nodiscard]] QVector3D cameraPosition() const;
  [[nodiscard]] QVector3D gridOrigin(ReferenceGridPlane plane) const;
  [[nodiscard]] QString formatViewportDistance(float localDistance) const;
  [[nodiscard]] bool pagedMeshAvailable() const;
  [[nodiscard]] QMatrix4x4 viewMatrix() const;
  [[nodiscard]] QMatrix4x4 projectionMatrix() const;
  [[nodiscard]] QMatrix4x4 viewProjectionMatrix() const;
  [[nodiscard]] QMatrix4x4 modelMatrix() const;
  [[nodiscard]] QVector3D modelBoundsMinimum() const;
  [[nodiscard]] QVector3D modelBoundsMaximum() const;
  [[nodiscard]] QVector3D transformedSceneCenter() const;
  [[nodiscard]] bool modelHitAt(const QPointF &position) const;
  [[nodiscard]] std::optional<QPointF>
  projectPoint(const QVector3D &point, const QMatrix4x4 &viewProjection) const;
  void reloadCameraTrajectory(const QString &scenePath, bool clearExisting);
  void rebuildCameraGeometry();
  void startSceneLoad(const QString &scenePath);
  void startSelection(const ScreenSelectionRequest &request,
                      SelectionOperation operation);
  void finishSelectionGesture(Qt::KeyboardModifiers modifiers);
  void rebuildRenderedVertices();
  void updateFrameRefreshPolicy();
  void notifyEditState();
  void uploadPendingPointCloud();
  void updatePointCacheSelection(const QMatrix4x4 &viewProjection);
  void requestMissingPointCachePages();
  void uploadPendingPointCachePages();
  void evictPointCacheUntilFits(qsizetype requiredBytes);
  void releaseFullResolutionPointCloud();
  void updateMeshCacheSelection(const QMatrix4x4 &viewProjection);
  void requestMissingMeshCachePages();
  void uploadPendingMeshCachePages();
  void evictMeshCacheUntilFits(qsizetype requiredBytes);
  void releaseFullResolutionMesh();
  void releaseMeshTexture();
  void uploadPendingMeshTexture();
  void uploadPendingMesh();
  void synchronizeGaussianRenderingAvailability(bool previousAvailability);
  void applyPendingTrainingGpuPreview();
  void drawPointCloud(const QMatrix4x4 &viewProjection);
  void drawMesh(const QMatrix4x4 &viewProjection);
  void drawTrainingPointCloud(const QMatrix4x4 &viewProjection);
  void drawGaussianCloud(const QMatrix4x4 &view, const QMatrix4x4 &projection);
  void drawInfiniteGrid(const QMatrix4x4 &viewProjection);
  void drawReferenceAxes(QPainter &painter, const QMatrix4x4 &viewProjection);
  void drawCameraTrajectory(QPainter &painter,
                            const QMatrix4x4 &viewProjection);
  void drawSelectionGesture(QPainter &painter);
  void drawModelSelection(QPainter &painter,
                          const QMatrix4x4 &modelViewProjection);
  void drawModelTransformGizmo(QPainter &painter);
  void drawTransformToolStrip(QPainter &painter);
  void drawOverlay(QPainter &painter);
  void drawAxisGizmo(QPainter &painter);
  [[nodiscard]] NavigationGizmoLayout navigationGizmo() const;
  void updateNavigationGizmoHover(const QPointF &position);
  void updateNavigationGizmoInteraction(const QPoint &current);
  void finishNavigationGizmoInteraction();
  void panCamera(const QPoint &delta);
  void toggleCameraView();
  void leaveCameraView();
  void beginModelTransform(InteractionMode mode, bool trackball = false);
  void updateModelTransform(const QPointF &position,
                            Qt::KeyboardModifiers modifiers);
  void finishModelTransform(bool commit);
  void applyTransformConstraint(int axis, bool plane);
  void updateTransformNumericInput(int key, const QString &text);
  [[nodiscard]] QVector3D transformConstraintAxis() const;
  [[nodiscard]] QString transformConstraintLabel() const;
  [[nodiscard]] QString transformStatusText() const;
  [[nodiscard]] float transformSnapStep(bool fine) const;
  [[nodiscard]] float projectedModelRadius(const QPointF &center) const;
  [[nodiscard]] QPointF currentPointerPosition() const;
  [[nodiscard]] TransformGizmoLayout modelTransformGizmo() const;
  [[nodiscard]] TransformToolStripLayout transformToolStrip() const;
  void updateTransformGizmoHover(const QPointF &position);
  void beginTransformGizmoDrag(const TransformGizmoHandle &handle,
                               const QPointF &position,
                               Qt::KeyboardModifiers modifiers);
  void activateTransformToolAt(int toolIndex);
  [[nodiscard]] QString
  transformGizmoHandleDescription(const TransformGizmoHandle &handle) const;
  void commitModelTransform();
  void resetModelTransformHistory();
  void notifyModelInteractionState();
  [[nodiscard]] bool canUndoModelTransform() const;
  [[nodiscard]] bool canRedoModelTransform() const;

  QString mProjectLabel;
  QString mScenePath;
  QString mRequestedScenePath;
  QString mSceneLoadMessage;
  qint64 mGaussianCount = 0;
  qint64 mSourceFaceCount = 0;
  qsizetype mPreviewPointCount = 0;
  qsizetype mPreviewTriangleCount = 0;
  qsizetype mRenderedPointCount = 0;
  qsizetype mFullResolutionPointCount = 0;
  qsizetype mUploadedFullResolutionPointCount = 0;
  qsizetype mFullResolutionMeshTriangleCount = 0;
  qsizetype mUploadedFullResolutionMeshTriangleCount = 0;
  qsizetype mDrawnFullResolutionMeshTriangleCount = 0;
  qsizetype mRenderedMeshIndexCount = 0;
  InteractionMode mMode = InteractionMode::Inspect;
  QPoint mLastMousePosition;
  Qt::MouseButtons mPressedButtons = Qt::NoButton;
  QPointF mSelectionStart;
  QPointF mSelectionCurrent;
  QPolygonF mSelectionPath;
  QPointF mBrushCursorPosition;
  qreal mBrushRadius = 32.0;
  bool mSelectionGestureActive = false;
  bool mModelSelected = false;
  bool mModelDragActive = false;
  bool mBrushCursorVisible = false;
  bool mVisibleOnlySelection = true;
  bool mSelectionBusy = false;
  bool mCameraManipulated = false;
  bool mTemporaryOrbitActive = false;
  bool mNavigationInteractionActive = false;
  bool mNavigationDragging = false;
  bool mOrthographic = false;
  bool mCameraViewActive = false;
  bool mShowCameras = false;
  bool mHasGaussianAttributes = false;
  bool mHasMesh = false;
  bool mGaussianShaderReady = false;
  bool mMeshShaderReady = false;
  bool mGridShaderReady = false;
  bool mPreviewOnlyScene = false;
  bool mInteractionLodActive = false;
  bool mProgressiveUploadActive = false;
  bool mFullResolutionPointClearPending = false;
  bool mFullResolutionMeshClearPending = false;
  int mSceneGeneration = 0;
  int mCameraTrajectoryGeneration = 0;
  RenderMode mRenderMode = RenderMode::Points;
  ReferencePlaneMode mReferencePlaneMode = ReferencePlaneMode::ModelBase;
  SceneCoordinateInfo mSceneCoordinates;
  QVector3D mSceneCenter = QVector3D(0.0F, 0.0F, 0.0F);
  QVector3D mModelTranslation = QVector3D(0.0F, 0.0F, 0.0F);
  QQuaternion mModelRotation;
  QVector3D mModelScale = QVector3D(1.0F, 1.0F, 1.0F);
  ModelTransform mModelDragStartTransform;
  QVector3D mModelDragStartIntersection = QVector3D(0.0F, 0.0F, 0.0F);
  QVector3D mModelDragPlaneNormal = QVector3D(0.0F, 0.0F, 1.0F);
  QPointF mModelTransformStartPosition;
  QPointF mModelTransformCurrentPosition;
  QPointF mModelTransformScreenCenter;
  QPointF mPointerPosition;
  float mModelRotationRadiusPixels = 96.0F;
  bool mTrackballRotation = false;
  bool mTransformGizmoDragActive = false;
  bool mTransformGizmoLocal = false;
  TransformGizmoMode mTransformGizmoMode = TransformGizmoMode::Move;
  TransformGizmoHandle mTransformGizmoHover;
  TransformGizmoHandle mTransformGizmoPress;
  int mTransformToolHover = -1;
  TransformConstraint mTransformConstraint;
  QString mTransformNumericInput;
  Qt::KeyboardModifiers mTransformModifiers = Qt::NoModifier;
  QVector<ModelTransform> mModelTransformHistory;
  qsizetype mModelTransformHistoryIndex = 0;
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
  PointCloudCacheIndex mPointCache;
  QSet<int> mDesiredPointCacheNodes;
  QSet<int> mPointCacheReadsInFlight;
  QSet<int> mPointCacheFailedNodes;
  QVector<PointCloudCachePage> mPendingPointCachePages;
  QVector<FullResolutionGpuChunk> mFullResolutionPointGpuChunks;
  qsizetype mPointCacheResidentBytes = 0;
  qsizetype mPointCacheGpuBudgetBytes = 1024LL * 1024LL * 1024LL;
  quint64 mPointCacheFrameSerial = 0;
  QString mPointCacheError;
  MeshCacheIndex mMeshCache;
  QSet<int> mDesiredMeshCacheNodes;
  QSet<int> mMeshCacheReadsInFlight;
  QSet<int> mMeshCacheFailedNodes;
  QVector<MeshCachePage> mPendingMeshCachePages;
  QVector<FullResolutionMeshGpuChunk> mFullResolutionMeshGpuChunks;
  qsizetype mMeshCacheResidentBytes = 0;
  qsizetype mMeshCacheGpuBudgetBytes = 1024LL * 1024LL * 1024LL;
  quint64 mMeshCacheFrameSerial = 0;
  QString mMeshCacheError;
  QString mMeshTexturePath;
  QString mMeshTextureError;
  QImage mPendingMeshTexture;
  QSize mMeshTextureSize;
  bool mMeshHasTextureCoordinates = false;
  bool mMeshTextureUploadPending = false;
  bool mMeshTextureClearPending = false;
  bool mMeshTextureReady = false;
  GLuint mMeshTexture = 0;
  QVector<MeshVertex> mPendingMeshVertices;
  QVector<quint32> mPendingMeshIndices;
  CameraTrajectory mCameraTrajectory;
  CameraTrajectoryGeometry mCameraGeometry;
  SceneEditModel mEditModel;
  bool mPointUploadPending = false;
  bool mMeshUploadPending = false;
  QOpenGLShaderProgram *mPointProgram = nullptr;
  QOpenGLShaderProgram *mMeshProgram = nullptr;
  QOpenGLShaderProgram *mGaussianProgram = nullptr;
  QOpenGLShaderProgram *mGridProgram = nullptr;
  QOpenGLBuffer mPointBuffer{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer mMeshVertexBuffer{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer mMeshIndexBuffer{QOpenGLBuffer::IndexBuffer};
  QOpenGLVertexArrayObject mPointVertexArray;
  QOpenGLVertexArrayObject mMeshVertexArray;
  QOpenGLVertexArrayObject mGaussianVertexArray;
  QOpenGLVertexArrayObject mGridVertexArray;
  TrainingGpuPreviewBuffer mTrainingGpuPreview;
  TrainingGpuPreviewCapability mTrainingGpuPreviewCapability;
  std::optional<TrainingGpuPreviewDescriptor>
      mPendingTrainingGpuPreviewDescriptor;
  bool mTrainingGpuPreviewStopPending = false;
  bool mTrainingGpuPreviewCameraFramed = false;
  QString mTrainingGpuPreviewError;
  QTimer *mTrainingGpuPreviewTimer = nullptr;
  QTimer *mFrameRefreshTimer = nullptr;
  QVariantAnimation *mViewSnapAnimation = nullptr;
  FrameRateCounter mFrameRateCounter;
};

} // namespace gsw

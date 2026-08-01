#pragma once

#include "CameraTrajectory.h"
#include "FrameRateCounter.h"
#include "NavigationGizmo.h"
#include "PlyPointCloudLoader.h"
#include "SceneEditModel.h"
#include "ScreenSpaceSelection.h"
#include "TrainingGpuPreviewBuffer.h"

#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
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
class QWheelEvent;
class QVariantAnimation;
class QTimer;

namespace gsw {

class NativeViewport final : public QOpenGLWidget,
                             protected QOpenGLExtraFunctions {
  Q_OBJECT

public:
  enum class InteractionMode { Inspect, Select, Rectangle, Lasso, Brush, Crop };

  enum class RenderMode { Points, Mesh, Gaussians };
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
  void cameraTrajectoryChanged(qsizetype cameraCount,
                               qsizetype invalidCameraCount,
                               bool displayDecimated, const QString &sourcePath,
                               const QString &error);
  void trainingGpuPreviewStateChanged(bool active, const QString &mode,
                                      const QString &detail);

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

  [[nodiscard]] QVector3D cameraPosition() const;
  [[nodiscard]] bool pagedMeshAvailable() const;
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
  void drawOverlay(QPainter &painter);
  void drawAxisGizmo(QPainter &painter);
  [[nodiscard]] NavigationGizmoLayout navigationGizmo() const;
  void updateNavigationGizmoHover(const QPointF &position);
  void updateNavigationGizmoInteraction(const QPoint &current);
  void finishNavigationGizmoInteraction();
  void panCamera(const QPoint &delta);
  void toggleCameraView();
  void leaveCameraView();

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

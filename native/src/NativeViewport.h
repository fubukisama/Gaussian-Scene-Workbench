#pragma once

#include "PlyPointCloudLoader.h"
#include "SceneEditModel.h"

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
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
class QWheelEvent;

namespace gsw {

class NativeViewport final : public QOpenGLWidget, protected QOpenGLFunctions {
  Q_OBJECT

public:
  enum class InteractionMode {
    Inspect,
    Select,
    Rectangle,
    Lasso,
    Crop
  };

  explicit NativeViewport(QWidget *parent = nullptr);
  ~NativeViewport() override;

  void setProjectLabel(const QString &label);
  void setScene(const QString &scenePath, qint64 gaussianCount);
  void setInteractionMode(InteractionMode mode);
  void setVisibleOnlySelection(bool enabled);
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

signals:
  void frameTimeChanged(double milliseconds);
  void sceneLoadStarted(const QString &scenePath);
  void sceneLoaded(qint64 sourceVertexCount, qsizetype previewVertexCount);
  void sceneLoadFailed(const QString &scenePath, const QString &message);
  void editStateChanged(qsizetype selectedCount, qsizetype deletedCount,
                        bool canUndo, bool canRedo, bool sceneReady,
                        bool hasUnsavedChanges);
  void selectionBusyChanged(bool busy);

protected:
  void initializeGL() override;
  void resizeGL(int width, int height) override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;

private:
  [[nodiscard]] QVector3D cameraPosition() const;
  [[nodiscard]] QMatrix4x4 viewProjectionMatrix() const;
  [[nodiscard]] std::optional<QPointF> projectPoint(const QVector3D &point,
                                                    const QMatrix4x4 &viewProjection) const;
  void startSceneLoad(const QString &scenePath);
  void startSelection(const QRectF &rectangle, const QPolygonF &lasso,
                      SelectionOperation operation);
  void finishSelectionGesture(Qt::KeyboardModifiers modifiers);
  void rebuildRenderedVertices();
  void notifyEditState();
  void uploadPendingPointCloud();
  void drawPointCloud(const QMatrix4x4 &viewProjection);
  void drawGrid(QPainter &painter, const QMatrix4x4 &viewProjection);
  void drawSelectionGesture(QPainter &painter);
  void drawOverlay(QPainter &painter, double frameMilliseconds);
  void drawAxisGizmo(QPainter &painter);

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
  QPolygonF mSelectionLasso;
  bool mSelectionGestureActive = false;
  bool mVisibleOnlySelection = true;
  bool mSelectionBusy = false;
  int mSceneGeneration = 0;
  QVector3D mTarget = QVector3D(0.0F, 0.0F, 0.0F);
  float mYawDegrees = 42.0F;
  float mPitchDegrees = 24.0F;
  float mDistance = 12.0F;
  float mSceneRadius = 4.0F;
  QVector<PointPosition> mSourcePositions;
  QVector<PointCloudVertex> mPreviewVertices;
  QVector<PointCloudVertex> mPendingVertices;
  SceneEditModel mEditModel;
  bool mPointUploadPending = false;
  QOpenGLShaderProgram *mPointProgram = nullptr;
  QOpenGLBuffer mPointBuffer{QOpenGLBuffer::VertexBuffer};
  QOpenGLVertexArrayObject mPointVertexArray;
  QElapsedTimer mFrameTimer;
  double mSmoothedFrameMilliseconds = 0.0;
};

} // namespace gsw

#pragma once

#include "PlyPointCloudLoader.h"

#include <QElapsedTimer>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPointF>
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
  void resetCamera();

signals:
  void frameTimeChanged(double milliseconds);
  void sceneLoadStarted(const QString &scenePath);
  void sceneLoaded(qint64 sourceVertexCount, qsizetype previewVertexCount);
  void sceneLoadFailed(const QString &scenePath, const QString &message);

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
  [[nodiscard]] std::optional<QPointF> projectPoint(const QVector3D &point,
                                                    const QMatrix4x4 &viewProjection) const;
  void startSceneLoad(const QString &scenePath);
  void uploadPendingPointCloud();
  void drawPointCloud(const QMatrix4x4 &viewProjection);
  void drawGrid(QPainter &painter, const QMatrix4x4 &viewProjection);
  void drawOverlay(QPainter &painter, double frameMilliseconds);
  void drawAxisGizmo(QPainter &painter);

  QString mProjectLabel;
  QString mScenePath;
  QString mRequestedScenePath;
  QString mSceneLoadMessage;
  qint64 mGaussianCount = 0;
  qsizetype mPreviewPointCount = 0;
  InteractionMode mMode = InteractionMode::Inspect;
  QPoint mLastMousePosition;
  Qt::MouseButtons mPressedButtons;
  QVector3D mTarget = QVector3D(0.0F, 0.0F, 0.0F);
  float mYawDegrees = 42.0F;
  float mPitchDegrees = 24.0F;
  float mDistance = 12.0F;
  float mSceneRadius = 4.0F;
  QVector<PointCloudVertex> mPendingVertices;
  bool mPointUploadPending = false;
  QOpenGLShaderProgram *mPointProgram = nullptr;
  QOpenGLBuffer mPointBuffer{QOpenGLBuffer::VertexBuffer};
  QOpenGLVertexArrayObject mPointVertexArray;
  QElapsedTimer mFrameTimer;
  double mSmoothedFrameMilliseconds = 0.0;
};

} // namespace gsw

#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>
#include <memory>

class QOpenGLWidget;

namespace gsw {

// Opt-in evidence collection only. Never schedules a paint or changes scene data.
// The owner must destroy/release this object with the viewport context current.
class RenderFrameDiagnostics final {
public:
  explicit RenderFrameDiagnostics(QString outputRoot);
  ~RenderFrameDiagnostics();
  RenderFrameDiagnostics(const RenderFrameDiagnostics &) = delete;
  RenderFrameDiagnostics &operator=(const RenderFrameDiagnostics &) = delete;

  bool request(QOpenGLWidget *viewport, quint64 rendered, quint64 presented,
               const QJsonObject &scene);
  bool beginFrame(QOpenGLWidget *viewport, quint64 serial,
                  const QJsonObject &scene);
  bool pendingFrame() const;
  bool recording() const;
  void note(const QString &name, const QJsonObject &meta = {});
  void stage(const QString &name, const QJsonObject &meta = {});
  void endFrame(quint64 serial);
  void presented(QOpenGLWidget *viewport, quint64 serial);
  QString lastManifestPath() const;
  void release();

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace gsw

#pragma once

#include <QQuaternion>
#include <QString>
#include <QVector3D>

namespace gsw {

// Persisted, independent source objects. Translation uses source units, not a
// loader's temporary display shift; importing never rewrites the source file.
struct SceneObject final {
  QString id;
  QString path;
  QVector3D translation;
  QQuaternion rotation;
  QVector3D scale{1, 1, 1};
  qint64 vertexCount = 0;
};

} // namespace gsw

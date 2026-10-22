#pragma once

#include <QJsonObject>
#include <QString>

namespace gsw {

// Project-owned request preferences, never a camera/source/output contract.
// Canonical identities and ordinary dataset locations form separate scopes.
class TrainingRequestStore final {
public:
  TrainingRequestStore(QString workspaceRoot, QString datasetPath,
                       QString cameraIdentity = {});
  [[nodiscard]] QJsonObject load(QString *error = nullptr) const;
  [[nodiscard]] bool save(const QJsonObject &request, QString *error = nullptr) const;

private:
  QString mWorkspaceRoot;
  QString mDatasetPath;
  QString mCameraIdentity;
};

} // namespace gsw

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace gsw {

struct RecoveryWorkspace {
  QString sessionId;
  QString displayName;
  QString rootPath;
  QString projectFilePath;
  QString datasetPath;
  QString scenePath;
  QDateTime updatedUtc;

  [[nodiscard]] bool isValid() const {
    return !sessionId.isEmpty() && !rootPath.isEmpty();
  }
};

struct ProjectSnapshot {
  QString snapshotId;
  QString snapshotPath;
  QString sourceProjectFilePath;
  QDateTime createdUtc;

  [[nodiscard]] bool isValid() const {
    return !snapshotId.isEmpty() && !snapshotPath.isEmpty() &&
           createdUtc.isValid();
  }
};

class RecoveryStore final {
public:
  explicit RecoveryStore(QString workspaceBase);

  [[nodiscard]] std::optional<RecoveryWorkspace>
  beginWorkspace(const QString &displayName,
                 QString *errorMessage = nullptr) const;
  bool checkpoint(const RecoveryWorkspace &workspace,
                  QString *errorMessage = nullptr) const;
  [[nodiscard]] QList<RecoveryWorkspace>
  recoverableWorkspaces(QString *errorMessage = nullptr) const;
  bool discardWorkspace(const RecoveryWorkspace &workspace,
                        QString *errorMessage = nullptr) const;
  bool completeWorkspace(const RecoveryWorkspace &workspace,
                         const QString &managedProjectRoot,
                         QString *errorMessage = nullptr) const;
  [[nodiscard]] std::optional<ProjectSnapshot>
  createProjectSnapshot(const QString &projectFilePath,
                        const QString &projectDataRoot,
                        int maximumSnapshots = 20,
                        QString *errorMessage = nullptr) const;
  [[nodiscard]] std::optional<ProjectSnapshot>
  createProjectSnapshot(const QByteArray &projectStateJson,
                        const QString &sourceProjectFilePath,
                        const QString &projectDataRoot,
                        int maximumSnapshots = 20,
                        QString *errorMessage = nullptr) const;
  [[nodiscard]] QList<ProjectSnapshot>
  projectSnapshots(const QString &projectDataRoot,
                   QString *errorMessage = nullptr) const;
  bool restoreProjectSnapshot(const ProjectSnapshot &snapshot,
                              const QString &targetProjectFilePath,
                              QString *errorMessage = nullptr) const;
  [[nodiscard]] QByteArray
  projectSnapshotJson(const ProjectSnapshot &snapshot,
                      QString *errorMessage = nullptr) const;

private:
  QString mWorkspaceBase;
};

} // namespace gsw

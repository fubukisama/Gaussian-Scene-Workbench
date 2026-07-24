#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace gsw {

struct ExternalBackupSnapshot {
  QString snapshotId;
  QString manifestPath;
  QString projectKey;
  QString projectName;
  QDateTime createdUtc;
  qsizetype fileCount = 0;
  qint64 totalBytes = 0;

  [[nodiscard]] bool isValid() const {
    return !snapshotId.isEmpty() && !manifestPath.isEmpty() &&
           !projectKey.isEmpty() && createdUtc.isValid();
  }
};

class ExternalBackupStore final {
public:
  explicit ExternalBackupStore(QString backupRoot);

  [[nodiscard]] std::optional<ExternalBackupSnapshot>
  backupProject(const QString &projectFilePath,
                const QString &projectDataRoot,
                QString *errorMessage = nullptr,
                const QString &linkedDatasetPath = {},
                const QString &linkedScenePath = {}) const;
  [[nodiscard]] QList<ExternalBackupSnapshot>
  snapshots(QString *errorMessage = nullptr) const;
  bool restore(const ExternalBackupSnapshot &snapshot,
               const QString &destinationRoot,
               QString *errorMessage = nullptr) const;

  [[nodiscard]] QString rootPath() const;

private:
  QString mBackupRoot;
};

} // namespace gsw

#include "BackendLocator.h"

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>

namespace gsw {
namespace {

bool hasBackendMarkers(const QString &directoryPath) {
  const QDir directory(directoryPath);
  static const QStringList markers = {
      QStringLiteral("native/worker/gsw_worker.py"),
      QStringLiteral("native/worker/import_preflight.py"),
      QStringLiteral("crop_editor/server.py"),
      QStringLiteral("crop_editor/video_extract.py"),
      QStringLiteral("scripts/check_3dgs_env.ps1"),
      QStringLiteral("gaussian-splatting/train.py"),
  };

  for (const QString &marker : markers) {
    if (!QFileInfo(directory.filePath(marker)).isFile()) {
      return false;
    }
  }
  return true;
}

QString validatedRoot(const QString &directoryPath) {
  if (!QDir::isAbsolutePath(directoryPath)) {
    return {};
  }

  const QString cleanPath = QDir::cleanPath(directoryPath);
  if (!QFileInfo(cleanPath).isDir() || !hasBackendMarkers(cleanPath)) {
    return {};
  }
  return cleanPath;
}

Qt::CaseSensitivity pathCaseSensitivity() {
#ifdef Q_OS_WIN
  return Qt::CaseInsensitive;
#else
  return Qt::CaseSensitive;
#endif
}

void appendUniquePath(QStringList *paths, const QString &path) {
  if (paths == nullptr || path.trimmed().isEmpty()) {
    return;
  }

  const QString cleanPath = QDir::cleanPath(path);
  const Qt::CaseSensitivity sensitivity = pathCaseSensitivity();
  for (const QString &existing : *paths) {
    if (existing.compare(cleanPath, sensitivity) == 0) {
      return;
    }
  }
  paths->append(cleanPath);
}

QStringList gaussianSearchVolumeRoots(const QString &repositoryRoot,
                                      const QStringList &configuredRoots) {
  QStringList roots;
  if (!configuredRoots.isEmpty()) {
    for (const QString &root : configuredRoots) {
      appendUniquePath(&roots, root);
    }
    return roots;
  }

  if (!repositoryRoot.isEmpty()) {
    const QStorageInfo repositoryStorage(repositoryRoot);
    if (repositoryStorage.isValid() && repositoryStorage.isReady()) {
      appendUniquePath(&roots, repositoryStorage.rootPath());
    }
  }

  for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
    if (storage.isValid() && storage.isReady()) {
      appendUniquePath(&roots, storage.rootPath());
    }
  }
  return roots;
}

} // namespace

QString BackendLocator::findRepositoryRoot(const QString &applicationDirectory,
                                           const QString &configuredRoot) {
  if (!configuredRoot.isEmpty()) {
    return validatedRoot(configuredRoot);
  }

  if (!QDir::isAbsolutePath(applicationDirectory)) {
    return {};
  }

  QDir candidate(QDir::cleanPath(applicationDirectory));
  constexpr int maximumParentLevels = 10;
  for (int level = 0; level <= maximumParentLevels; ++level) {
    const QString located = validatedRoot(candidate.path());
    if (!located.isEmpty()) {
      return located;
    }
    if (level == maximumParentLevels || !candidate.cdUp()) {
      break;
    }
  }
  return {};
}

QStringList BackendLocator::gaussianPythonCandidates(
    const QString &repositoryRoot, const QStringList &searchVolumeRoots) {
  QStringList candidates;
  const QStringList configuredPrefixes = {
      qEnvironmentVariable("GAUSSIAN_SPLATTING_CONDA_PREFIX"),
      qEnvironmentVariable("GS_CONDA_PREFIX")};
  for (const QString &configured : configuredPrefixes) {
    if (!configured.isEmpty()) {
      appendUniquePath(
          &candidates,
          QDir(configured).filePath(QStringLiteral("python.exe")));
    }
  }

  const QString activeCondaPrefix = qEnvironmentVariable("CONDA_PREFIX");
  if (QFileInfo(activeCondaPrefix).fileName().compare(
          QStringLiteral("gaussian_splatting"), Qt::CaseInsensitive) == 0) {
    appendUniquePath(
        &candidates,
        QDir(activeCondaPrefix).filePath(QStringLiteral("python.exe")));
  }

  const QStringList volumeRoots =
      gaussianSearchVolumeRoots(repositoryRoot, searchVolumeRoots);
  for (const QString &volumeRoot : volumeRoots) {
    const QStringList conventionalPrefixes = {
        QDir(volumeRoot).filePath(
            QStringLiteral("miniforge3/envs/gaussian_splatting")),
        QDir(volumeRoot).filePath(
            QStringLiteral("conda/envs/gaussian_splatting")),
        QDir(volumeRoot).filePath(
            QStringLiteral("anaconda/envs/gaussian_splatting")),
        QDir(volumeRoot).filePath(
            QStringLiteral("anaconda3/envs/gaussian_splatting")),
    };
    for (const QString &prefix : conventionalPrefixes) {
      appendUniquePath(
          &candidates,
          QDir(prefix).filePath(QStringLiteral("python.exe")));
    }
  }

  const QString home = QDir::homePath();
  const QStringList homePrefixes = {
      QDir(home).filePath(QStringLiteral("miniforge3/envs/gaussian_splatting")),
      QDir(home).filePath(QStringLiteral("miniconda3/envs/gaussian_splatting")),
      QDir(home).filePath(QStringLiteral("anaconda3/envs/gaussian_splatting")),
  };
  for (const QString &prefix : homePrefixes) {
    appendUniquePath(
        &candidates,
        QDir(prefix).filePath(QStringLiteral("python.exe")));
  }

  for (QString &candidate : candidates) {
    candidate = QDir::toNativeSeparators(candidate);
  }
  return candidates;
}

QString BackendLocator::findGaussianPython(
    const QString &repositoryRoot, const QStringList &searchVolumeRoots) {
  for (const QString &candidate :
       gaussianPythonCandidates(repositoryRoot, searchVolumeRoots)) {
    if (QFileInfo(candidate).isFile()) {
      return candidate;
    }
  }
  return {};
}

} // namespace gsw

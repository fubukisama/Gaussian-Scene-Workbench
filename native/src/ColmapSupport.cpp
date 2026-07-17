#include "ColmapSupport.h"

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStringList>
#include <QVersionNumber>

#include <algorithm>

namespace gsw {

namespace {
bool directoryHasImages(const QString &path) {
  static const QStringList filters = {
      QStringLiteral("*.jpg"),  QStringLiteral("*.jpeg"),
      QStringLiteral("*.png"),  QStringLiteral("*.tif"),
      QStringLiteral("*.tiff"), QStringLiteral("*.bmp"),
      QStringLiteral("*.webp"), QStringLiteral("*.exr")};
  return QFileInfo(path).isDir() &&
         !QDir(path).entryList(filters, QDir::Files).isEmpty();
}

bool completeModel(const QDir &directory, const QString &extension) {
  return QFileInfo::exists(directory.filePath(QStringLiteral("cameras.") + extension)) &&
         QFileInfo::exists(directory.filePath(QStringLiteral("images.") + extension)) &&
         QFileInfo::exists(directory.filePath(QStringLiteral("points3D.") + extension));
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
  for (const QString &existing : *paths) {
    if (existing.compare(cleanPath, pathCaseSensitivity()) == 0) {
      return;
    }
  }
  paths->append(cleanPath);
}

QStringList colmapSearchVolumeRoots(const QString &repositoryRoot,
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

QString datasetImageDirectory(const QString &datasetPath) {
  if (datasetPath.isEmpty()) {
    return {};
  }
  const QDir dataset(datasetPath);
  const QStringList candidates = {
      dataset.filePath(QStringLiteral("input")),
      dataset.filePath(QStringLiteral("images")), dataset.absolutePath()};
  for (const QString &candidate : candidates) {
    if (directoryHasImages(candidate)) {
      return QDir::cleanPath(candidate);
    }
  }
  return {};
}

bool hasRecognizedColmapScene(const QString &datasetPath) {
  if (datasetPath.isEmpty()) {
    return false;
  }
  const QDir sparse(QDir(datasetPath).filePath(QStringLiteral("sparse/0")));
  const bool binary = completeModel(sparse, QStringLiteral("bin"));
  const bool text = completeModel(sparse, QStringLiteral("txt"));
  const bool ply = QFileInfo::exists(
                       sparse.filePath(QStringLiteral("points3D.ply"))) &&
                   ((QFileInfo::exists(
                         sparse.filePath(QStringLiteral("cameras.bin"))) &&
                     QFileInfo::exists(
                         sparse.filePath(QStringLiteral("images.bin")))) ||
                    (QFileInfo::exists(
                         sparse.filePath(QStringLiteral("cameras.txt"))) &&
                     QFileInfo::exists(
                         sparse.filePath(QStringLiteral("images.txt")))));
  return binary || text || ply;
}

bool hasRecognizedTrainingScene(const QString &datasetPath) {
  return hasRecognizedColmapScene(datasetPath) ||
         QFileInfo::exists(
             QDir(datasetPath).filePath(QStringLiteral("transforms_train.json")));
}

bool hasColmapWorkingData(const QString &datasetPath) {
  if (datasetPath.isEmpty()) {
    return false;
  }
  const QDir dataset(datasetPath);
  for (const QString &name : {QStringLiteral("distorted"),
                              QStringLiteral("sparse"),
                              QStringLiteral("stereo")}) {
    const QDir candidate(dataset.filePath(name));
    if (candidate.exists() &&
        !candidate.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
      return true;
    }
  }
  return false;
}

QString findVersionedColmapExecutable(const QString &installRoot) {
  const QDir root(installRoot);
  if (!root.exists()) {
    return {};
  }
  const QString direct = root.filePath(QStringLiteral("bin/colmap.exe"));

  struct Candidate {
    QVersionNumber version;
    QString executable;
  };
  QList<Candidate> candidates;
  const QFileInfoList directories =
      root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  for (const QFileInfo &directory : directories) {
    const QVersionNumber version =
        QVersionNumber::fromString(directory.fileName());
    const QString executable =
        QDir(directory.absoluteFilePath())
            .filePath(QStringLiteral("bin/colmap.exe"));
    if (!version.isNull() && QFileInfo(executable).isFile()) {
      candidates.append({version, executable});
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &left, const Candidate &right) {
              return QVersionNumber::compare(left.version, right.version) > 0;
            });
  if (!candidates.isEmpty()) {
    return QDir::toNativeSeparators(
        QFileInfo(candidates.first().executable).absoluteFilePath());
  }
  return QFileInfo(direct).isFile()
             ? QDir::toNativeSeparators(
                   QFileInfo(direct).absoluteFilePath())
             : QString();
}

QString findColmapExecutable(const QString &repositoryRoot,
                             const QString &preferredPath,
                             const QStringList &searchVolumeRoots) {
  QStringList candidates;
  candidates << preferredPath << qEnvironmentVariable("COLMAP_PATH")
             << qEnvironmentVariable("COLMAP_EXE");
  if (!repositoryRoot.isEmpty()) {
    const QDir repository(repositoryRoot);
    candidates << repository.filePath(QStringLiteral("third_party/colmap/bin/colmap.exe"))
               << repository.filePath(QStringLiteral("tools/colmap/bin/colmap.exe"))
               << repository.filePath(QStringLiteral("colmap/bin/colmap.exe"));
  }
  for (const QString &volumeRoot :
       colmapSearchVolumeRoots(repositoryRoot, searchVolumeRoots)) {
    const QDir volume(volumeRoot);
    candidates << findVersionedColmapExecutable(
                      volume.filePath(QStringLiteral("Tools/COLMAP")))
               << findVersionedColmapExecutable(
                      volume.filePath(QStringLiteral("tools/colmap")))
               << findVersionedColmapExecutable(
                      volume.filePath(QStringLiteral("COLMAP")));
  }
  candidates << QDir(QDir::homePath())
                    .filePath(QStringLiteral(
                        "Downloads/colmap-x64-windows-cuda/bin/colmap.exe"))
             << QStandardPaths::findExecutable(QStringLiteral("colmap.exe"))
             << QStandardPaths::findExecutable(QStringLiteral("colmap"));

  for (const QString &candidate : candidates) {
    const QFileInfo info(candidate);
    if (!candidate.isEmpty() && info.exists() && info.isFile()) {
      return QDir::toNativeSeparators(info.absoluteFilePath());
    }
  }
  return {};
}

} // namespace gsw

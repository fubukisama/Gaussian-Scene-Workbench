#include "ColmapSupport.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

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

QString findColmapExecutable(const QString &repositoryRoot,
                             const QString &preferredPath) {
  QStringList candidates;
  candidates << preferredPath << qEnvironmentVariable("COLMAP_PATH")
             << qEnvironmentVariable("COLMAP_EXE");
  if (!repositoryRoot.isEmpty()) {
    const QDir repository(repositoryRoot);
    candidates << repository.filePath(QStringLiteral("third_party/colmap/bin/colmap.exe"))
               << repository.filePath(QStringLiteral("tools/colmap/bin/colmap.exe"))
               << repository.filePath(QStringLiteral("colmap/bin/colmap.exe"));
    const QDir drive(repository.rootPath());
    candidates << drive.filePath(QStringLiteral("COLMAP/bin/colmap.exe"))
               << drive.filePath(QStringLiteral("colmap/bin/colmap.exe"));
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

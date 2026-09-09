#include "UntitledWorkspaceStorage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QStorageInfo>

namespace gsw {
namespace {

Qt::CaseSensitivity localPathCaseSensitivity() {
#ifdef Q_OS_WIN
  return Qt::CaseInsensitive;
#else
  return Qt::CaseSensitive;
#endif
}

QString comparablePath(const QString &path) {
  if (path.trimmed().isEmpty()) {
    return {};
  }
  const QFileInfo info(path);
  const QString canonical = info.canonicalFilePath();
  return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                              : canonical);
}

QString existingAncestor(QString path) {
  path = QFileInfo(path).absoluteFilePath();
  QFileInfo info(path);
  while (!info.exists()) {
    const QString parent = info.absolutePath();
    if (parent == path) {
      return {};
    }
    path = parent;
    info.setFile(path);
  }
  return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

QString pathKey(const QString &path) {
  const QString comparable = comparablePath(path);
  return localPathCaseSensitivity() == Qt::CaseInsensitive
             ? comparable.toCaseFolded()
             : comparable;
}

} // namespace

QString findUntitledWorkspaceBase(
    const UntitledWorkspaceSearchOptions &options, QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }

  const QString systemRoot = comparablePath(options.systemRoot);
  QSet<QString> checkedRoots;

  const auto tryCandidate = [&](const QString &candidate,
                                const bool allowSystemVolume) -> QString {
    if (candidate.trimmed().isEmpty()) {
      return {};
    }
    const QString ancestor = existingAncestor(candidate);
    if (ancestor.isEmpty()) {
      return {};
    }
    const QStorageInfo storage(ancestor);
    if (!storage.isValid() || !storage.isReady() || storage.isReadOnly()) {
      return {};
    }
    const QString volumeRoot = comparablePath(storage.rootPath());
    if (!allowSystemVolume &&
        volumeRoot.compare(systemRoot, localPathCaseSensitivity()) == 0) {
      return {};
    }

    const QString base =
        allowSystemVolume
            ? QFileInfo(candidate).absoluteFilePath()
            : QDir(storage.rootPath())
                  .filePath(QStringLiteral(
                      ".Gaussian-Scene-Workbench/temporary"));
    const QString key = pathKey(base);
    if (checkedRoots.contains(key)) {
      return {};
    }
    checkedRoots.insert(key);

    if (!QDir().mkpath(base) || !QFileInfo(base).isDir() ||
        !QFileInfo(base).isWritable()) {
      return {};
    }
    return QDir::cleanPath(QFileInfo(base).absoluteFilePath());
  };

  const QString configured =
      tryCandidate(options.configuredRoot, true);
  if (!configured.isEmpty()) {
    return configured;
  }

  for (const QString &candidate : options.preferredRoots) {
    const QString selected = tryCandidate(candidate, false);
    if (!selected.isEmpty()) {
      return selected;
    }
  }
  for (const QString &candidate : options.mountedRoots) {
    const QString selected = tryCandidate(candidate, false);
    if (!selected.isEmpty()) {
      return selected;
    }
  }

  const QString fallback = tryCandidate(options.fallbackRoot, true);
  if (!fallback.isEmpty()) {
    return fallback;
  }

  if (errorMessage != nullptr) {
    *errorMessage = QCoreApplication::translate("Workbench", "找不到可写的临时工作区。已检查非系统盘和当前用户临时目录；"
        "也可通过 GSW_UNTITLED_ROOT 指定位置。");
  }
  return {};
}

QString defaultUntitledWorkspaceBase(QString *errorMessage) {
  UntitledWorkspaceSearchOptions options;
  options.configuredRoot =
      qEnvironmentVariable("GSW_UNTITLED_ROOT").trimmed();
  options.systemRoot = QStorageInfo::root().rootPath();
  options.preferredRoots = {QCoreApplication::applicationDirPath(),
                            QDir::currentPath()};
  for (const QStorageInfo &storage : QStorageInfo::mountedVolumes()) {
    options.mountedRoots.append(storage.rootPath());
  }
  options.fallbackRoot =
      QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
          .filePath(QStringLiteral(
              "Gaussian Scene Workbench/temporary"));
  return findUntitledWorkspaceBase(options, errorMessage);
}

} // namespace gsw

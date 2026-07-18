#include "MediaProjectBootstrap.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace gsw {
namespace {

void assignError(QString *target, const QString &message) {
  if (target != nullptr) {
    *target = message;
  }
}

bool isReservedWindowsName(const QString &name) {
  static const QSet<QString> reserved = {
      QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
      QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
      QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
      QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
      QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
      QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
      QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
      QStringLiteral("LPT9"),
  };
  return reserved.contains(
      name.section(QLatin1Char('.'), 0, 0).toUpper());
}

QString safeProjectComponent(QString value) {
  value = value.trimmed();
  value.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*]+)")),
                QStringLiteral("_"));
  while (value.endsWith(QLatin1Char('.')) ||
         value.endsWith(QLatin1Char(' '))) {
    value.chop(1);
  }
  if (value.isEmpty()) {
    value = QStringLiteral("Gaussian Scene Project");
  }
  if (isReservedWindowsName(value)) {
    value.prepend(QStringLiteral("GSW-"));
  }
  return value.left(120);
}

bool isVideoFile(const QFileInfo &source) {
  static const QSet<QString> videoExtensions = {
      QStringLiteral("mp4"),  QStringLiteral("mov"),
      QStringLiteral("avi"),  QStringLiteral("mkv"),
      QStringLiteral("webm"), QStringLiteral("m4v"),
  };
  return source.isFile() &&
         videoExtensions.contains(source.suffix().toLower());
}

bool isGenericMediaDirectory(const QString &name) {
  static const QSet<QString> genericNames = {
      QStringLiteral("dcim"),      QStringLiteral("frames"),
      QStringLiteral("images"),    QStringLiteral("input"),
      QStringLiteral("originals"), QStringLiteral("photos"),
      QStringLiteral("pictures"),  QStringLiteral("source"),
  };
  return genericNames.contains(name.toCaseFolded());
}

QString logicalMediaRoot(const QStringList &sourcePaths) {
  if (sourcePaths.isEmpty()) {
    return {};
  }
  const QFileInfo first(sourcePaths.constFirst());
  QDir root(first.isDir() ? first.absoluteFilePath() : first.absolutePath());
  const bool singleVideo = sourcePaths.size() == 1 && isVideoFile(first);
  while (!singleVideo && isGenericMediaDirectory(root.dirName())) {
    if (!root.cdUp()) {
      break;
    }
  }
  return QDir::cleanPath(root.absolutePath());
}

QString sourceLabel(const QStringList &sourcePaths) {
  if (sourcePaths.isEmpty()) {
    return {};
  }
  const QFileInfo first(sourcePaths.constFirst());
  if (sourcePaths.size() == 1 && isVideoFile(first)) {
    return first.completeBaseName();
  }
  return QDir(logicalMediaRoot(sourcePaths)).dirName();
}

QString sourceSiblingDirectory(const QStringList &sourcePaths) {
  QDir siblingParent(logicalMediaRoot(sourcePaths));
  if (!siblingParent.cdUp()) {
    siblingParent = QDir(logicalMediaRoot(sourcePaths));
  }
  return QDir::cleanPath(siblingParent.absolutePath());
}

} // namespace

QString suggestedMediaProjectName(const QStringList &sourcePaths) {
  return safeProjectComponent(sourceLabel(sourcePaths));
}

QString suggestedMediaSceneName(const QStringList &sourcePaths) {
  QString value = sourceLabel(sourcePaths).trimmed();
  value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]+")),
                QStringLiteral("_"));
  while (value.contains(QStringLiteral(".."))) {
    value.replace(QStringLiteral(".."), QStringLiteral("_"));
  }
  while (value.startsWith(QLatin1Char('.')) ||
         value.endsWith(QLatin1Char('.'))) {
    if (value.startsWith(QLatin1Char('.'))) {
      value.remove(0, 1);
    }
    if (value.endsWith(QLatin1Char('.'))) {
      value.chop(1);
    }
  }
  if (value.isEmpty() || isReservedWindowsName(value) ||
      !value.contains(QRegularExpression(QStringLiteral("[A-Za-z0-9]")))) {
    return QStringLiteral("dataset");
  }
  return value.left(120);
}

std::optional<MediaProjectBootstrapPlan>
planMediaProjectBootstrap(const QStringList &sourcePaths,
                          const QString &displayName,
                          QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  if (sourcePaths.isEmpty()) {
    assignError(errorMessage, QStringLiteral("没有可用于创建工程的媒体来源。"));
    return std::nullopt;
  }

  const QFileInfo first(sourcePaths.constFirst());
  if (!first.exists()) {
    assignError(errorMessage,
                QStringLiteral("媒体来源不存在：%1").arg(sourcePaths.constFirst()));
    return std::nullopt;
  }

  const QString projectName = safeProjectComponent(
      displayName.isEmpty() ? suggestedMediaProjectName(sourcePaths)
                            : displayName);
  const QString projectsDirectory =
      QDir(sourceSiblingDirectory(sourcePaths))
          .filePath(QStringLiteral("Gaussian Scene Workbench Projects"));
  QString rootPath = QDir(projectsDirectory).filePath(projectName);
  for (int suffix = 2; QFileInfo::exists(rootPath); ++suffix) {
    rootPath = QDir(projectsDirectory)
                   .filePath(QStringLiteral("%1-%2").arg(projectName).arg(suffix));
  }

  MediaProjectBootstrapPlan plan;
  plan.displayName = QFileInfo(rootPath).fileName();
  plan.rootPath = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());
  plan.projectFilePath =
      QDir(plan.rootPath)
          .filePath(safeProjectComponent(plan.displayName) +
                    QStringLiteral(".gsw.json"));
  return plan;
}

bool materializeMediaProjectBootstrap(
    const MediaProjectBootstrapPlan &plan, QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  if (plan.rootPath.isEmpty() || QFileInfo::exists(plan.rootPath)) {
    assignError(errorMessage,
                QStringLiteral("自动工程目录已存在或无效：%1")
                    .arg(QDir::toNativeSeparators(plan.rootPath)));
    return false;
  }
  if (!QDir().mkpath(plan.rootPath) || !QFileInfo(plan.rootPath).isDir()) {
    assignError(errorMessage,
                QStringLiteral("无法在素材旁创建工程目录：%1")
                    .arg(QDir::toNativeSeparators(plan.rootPath)));
    return false;
  }
  return true;
}

} // namespace gsw

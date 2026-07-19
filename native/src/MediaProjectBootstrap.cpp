#include "MediaProjectBootstrap.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace gsw {
namespace {

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

} // namespace gsw

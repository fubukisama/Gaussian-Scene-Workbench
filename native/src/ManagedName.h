#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>

namespace gsw {

// Display names are data, never paths. Keep legacy safe ASCII components stable;
// encode other names into a reserved namespace with a full UTF-8 SHA-256 digest.
// Keep this mapping in sync with native/worker/gsw_worker.py.
inline QString managedStorageName(const QString &displayName) {
  if (displayName.trimmed().isEmpty()) return {};
  static const QRegularExpression ascii(QStringLiteral("\\A[A-Za-z0-9_.-]+\\z"));
  static const QSet<QString> devices = {
      QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL"),
      QStringLiteral("COM1"), QStringLiteral("COM2"), QStringLiteral("COM3"), QStringLiteral("COM4"),
      QStringLiteral("COM5"), QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
      QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"), QStringLiteral("LPT3"),
      QStringLiteral("LPT4"), QStringLiteral("LPT5"), QStringLiteral("LPT6"), QStringLiteral("LPT7"),
      QStringLiteral("LPT8"), QStringLiteral("LPT9")};
  if (displayName.size() <= 120 && ascii.match(displayName).hasMatch() &&
      !displayName.startsWith(QLatin1Char('.')) && !displayName.endsWith(QLatin1Char('.')) &&
      !displayName.contains(QStringLiteral("..")) &&
      !displayName.startsWith(QStringLiteral("gsw-name-"), Qt::CaseInsensitive) &&
      !devices.contains(displayName.section(QLatin1Char('.'), 0, 0).toUpper())) return displayName;
  return QStringLiteral("gsw-name-") + QString::fromLatin1(
      QCryptographicHash::hash(displayName.toUtf8(), QCryptographicHash::Sha256).toHex());
}

inline QString managedDisplayName(const QString &directory) {
  if (directory.isEmpty()) return {};
  QFile file(QDir(directory).filePath(QStringLiteral(".gsw-name.json")));
  if (file.open(QIODevice::ReadOnly)) {
    const auto metadata = QJsonDocument::fromJson(file.readAll()).object();
    const QString name = metadata.value(QStringLiteral("displayName")).toString();
    if (metadata.value(QStringLiteral("version")).toInt() == 1 && !name.trimmed().isEmpty() &&
        metadata.value(QStringLiteral("storageName")).toString() == QFileInfo(directory).fileName())
      return name;
  }
  return QFileInfo(directory).fileName();
}

} // namespace gsw

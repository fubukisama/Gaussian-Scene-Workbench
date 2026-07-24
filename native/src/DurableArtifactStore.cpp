#include "DurableArtifactStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace gsw {
namespace {

QString normalizedAbsolutePath(const QString &path) {
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString integrityPath(const QString &artifactPath) {
  return artifactPath + QStringLiteral(".integrity.json");
}

void assignError(QString *target, const QString &message) {
  if (target != nullptr) {
    *target = message;
  }
}

QString hashFile(const QString &path, QString *errorMessage) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to read artifact for hashing: %1")
                    .arg(file.errorString()));
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  constexpr qint64 chunkSize = 4LL * 1024LL * 1024LL;
  while (!file.atEnd()) {
    const QByteArray chunk = file.read(chunkSize);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
      assignError(errorMessage,
                  QStringLiteral("Unable to hash artifact: %1")
                      .arg(file.errorString()));
      return {};
    }
    hash.addData(chunk);
  }
  return QString::fromLatin1(hash.result().toHex());
}

bool writeIntegrityRecord(const DurableArtifact &artifact,
                          QString *errorMessage) {
  QSaveFile record(integrityPath(artifact.path));
  record.setDirectWriteFallback(false);
  if (!record.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create artifact integrity record: "
                               "%1")
                    .arg(record.errorString()));
    return false;
  }
  const QJsonObject root{
      {QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("sha256"), artifact.sha256},
      {QStringLiteral("size"), artifact.size},
      {QStringLiteral("publishedUtc"),
       QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
  const QByteArray serialized =
      QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (record.write(serialized) != serialized.size()) {
    record.cancelWriting();
    assignError(errorMessage,
                QStringLiteral("Unable to write artifact integrity record: "
                               "%1")
                    .arg(record.errorString()));
    return false;
  }
  if (!record.commit()) {
    assignError(errorMessage,
                QStringLiteral("Unable to commit artifact integrity record: "
                               "%1")
                    .arg(record.errorString()));
    return false;
  }
  return true;
}

} // namespace

DurableArtifact DurableArtifactStore::publish(const QString &sourcePath,
                                              const QString &destinationPath,
                                              QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  const QString source = normalizedAbsolutePath(sourcePath);
  const QString destination = normalizedAbsolutePath(destinationPath);
  const QFileInfo sourceInfo(source);
  if (!sourceInfo.exists() || !sourceInfo.isFile()) {
    assignError(errorMessage,
                QStringLiteral("Artifact source does not exist: %1")
                    .arg(source));
    return {};
  }
  if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
    assignError(errorMessage,
                QStringLiteral("Unable to create artifact directory: %1")
                    .arg(QFileInfo(destination).absolutePath()));
    return {};
  }

  if (source == destination) {
    QString hashError;
    const QString hash = hashFile(source, &hashError);
    if (hash.isEmpty()) {
      assignError(errorMessage, hashError);
      return {};
    }
    const DurableArtifact artifact{destination, hash, sourceInfo.size()};
    return writeIntegrityRecord(artifact, errorMessage) ? artifact
                                                        : DurableArtifact{};
  }

  QFile input(source);
  if (!input.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to open artifact source: %1")
                    .arg(input.errorString()));
    return {};
  }
  QSaveFile output(destination);
  output.setDirectWriteFallback(false);
  if (!output.open(QIODevice::WriteOnly)) {
    assignError(errorMessage,
                QStringLiteral("Unable to create durable artifact: %1")
                    .arg(output.errorString()));
    return {};
  }

  QCryptographicHash copiedHash(QCryptographicHash::Sha256);
  qint64 copiedBytes = 0;
  constexpr qint64 chunkSize = 4LL * 1024LL * 1024LL;
  while (!input.atEnd()) {
    const QByteArray chunk = input.read(chunkSize);
    if (chunk.isEmpty() && input.error() != QFileDevice::NoError) {
      output.cancelWriting();
      assignError(errorMessage,
                  QStringLiteral("Unable to read artifact source: %1")
                      .arg(input.errorString()));
      return {};
    }
    if (output.write(chunk) != chunk.size()) {
      output.cancelWriting();
      assignError(errorMessage,
                  QStringLiteral("Unable to write durable artifact: %1")
                      .arg(output.errorString()));
      return {};
    }
    copiedHash.addData(chunk);
    copiedBytes += chunk.size();
  }
  input.close();

  QString verificationError;
  const QString copiedSha256 =
      QString::fromLatin1(copiedHash.result().toHex());
  const QString sourceSha256 = hashFile(source, &verificationError);
  if (sourceSha256.isEmpty() || sourceSha256 != copiedSha256 ||
      QFileInfo(source).size() != copiedBytes) {
    output.cancelWriting();
    assignError(errorMessage,
                verificationError.isEmpty()
                    ? QStringLiteral(
                          "Artifact source changed while it was being copied.")
                    : verificationError);
    return {};
  }
  if (!output.commit()) {
    assignError(errorMessage,
                QStringLiteral("Unable to publish durable artifact: %1")
                    .arg(output.errorString()));
    return {};
  }

  const DurableArtifact artifact{destination, copiedSha256, copiedBytes};
  if (!writeIntegrityRecord(artifact, errorMessage)) {
    return {};
  }
  return artifact;
}

bool DurableArtifactStore::verify(const QString &artifactPath,
                                  QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  const QString artifact = normalizedAbsolutePath(artifactPath);
  QFile record(integrityPath(artifact));
  if (!record.open(QIODevice::ReadOnly)) {
    assignError(errorMessage,
                QStringLiteral("Artifact integrity record is missing: %1")
                    .arg(integrityPath(artifact)));
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(record.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    assignError(errorMessage,
                QStringLiteral("Artifact integrity record is invalid: %1")
                    .arg(parseError.errorString()));
    return false;
  }
  const QJsonObject root = document.object();
  const QString expectedHash =
      root.value(QStringLiteral("sha256")).toString();
  const qint64 expectedSize =
      root.value(QStringLiteral("size")).toInteger(-1);
  const QFileInfo artifactInfo(artifact);
  if (root.value(QStringLiteral("schemaVersion")).toInt() != 1 ||
      expectedHash.size() != 64 || expectedSize < 0 ||
      !artifactInfo.isFile() || artifactInfo.size() != expectedSize) {
    assignError(errorMessage,
                QStringLiteral("Artifact size or integrity metadata does not "
                               "match."));
    return false;
  }
  QString hashError;
  const QString actualHash = hashFile(artifact, &hashError);
  if (actualHash != expectedHash) {
    assignError(errorMessage,
                hashError.isEmpty()
                    ? QStringLiteral("Artifact checksum does not match.")
                    : hashError);
    return false;
  }
  return true;
}

} // namespace gsw

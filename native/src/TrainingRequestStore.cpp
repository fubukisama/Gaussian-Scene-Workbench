#include "TrainingRequestStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <utility>

namespace gsw {
namespace {

constexpr qint64 maximumStoreBytes = 1024 * 1024;

void fail(QString *error, const char *code) {
  if (error) *error = QString::fromLatin1(code);
}

QString location(const QString &value) {
  QString path = QDir::cleanPath(QFileInfo(value).absoluteFilePath());
#ifdef Q_OS_WIN
  path = path.toCaseFolded();
#endif
  return path;
}

QString scope(const QString &workspace, const QString &dataset, const QString &identity) {
  if (!identity.isEmpty()) return QStringLiteral("identity:") + identity;
  const QString relative = QDir(location(workspace)).relativeFilePath(location(dataset));
  const QString key = relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) &&
          !QDir::isAbsolutePath(relative)
      ? QStringLiteral("relative:") + relative : QStringLiteral("absolute:") + location(dataset);
  return QStringLiteral("dataset:") + QString::fromLatin1(
      QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool safePath(const QString &path) {
  QFileInfo current(path);
  for (;;) {
    if (current.isSymLink() || current.isJunction()) return false;
    const QString parent = current.absolutePath();
    if (current.absoluteFilePath() == parent) break;
    current.setFile(parent);
  }
  return true;
}

QJsonObject readStore(const QString &path, QString *error) {
  if (error) error->clear();
  if (!safePath(path)) { fail(error, "training_request_store_unsafe_path"); return {}; }
  QFile file(path);
  if (!file.exists()) return {};
  if (file.size() > maximumStoreBytes || !file.open(QIODevice::ReadOnly)) {
    fail(error, "training_request_store_unreadable"); return {};
  }
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
  const auto object = document.object();
  if (parse.error != QJsonParseError::NoError || !document.isObject() ||
      object.value(QStringLiteral("schema")) != QStringLiteral("gsw.training_requests/1") ||
      !object.value(QStringLiteral("requests")).isObject()) {
    fail(error, "training_request_store_invalid"); return {};
  }
  return object.value(QStringLiteral("requests")).toObject();
}

bool validRequest(const QJsonObject &request) {
  const QSet<QString> fields{QStringLiteral("schema"), QStringLiteral("backend"),
      QStringLiteral("quality"), QStringLiteral("iterations"), QStringLiteral("resolution"),
      QStringLiteral("resolutionPolicy"), QStringLiteral("solveResolution"),
      QStringLiteral("runtimeOptions"), QStringLiteral("contextPosePolicy"),
      QStringLiteral("qualityPlan")};
  if (request.value(QStringLiteral("schema")) != QStringLiteral("gsw.training_dialog_configuration/1"))
    return false;
  for (auto it = request.begin(); it != request.end(); ++it)
    if (!fields.contains(it.key())) return false;
  if (request.contains(QStringLiteral("qualityPlan")) &&
      !request.value(QStringLiteral("qualityPlan")).isObject()) return false;
  return QJsonDocument(request).toJson(QJsonDocument::Compact).size() <= maximumStoreBytes / 2;
}

} // namespace

TrainingRequestStore::TrainingRequestStore(QString workspaceRoot, QString datasetPath,
                                         QString cameraIdentity)
    : mWorkspaceRoot(std::move(workspaceRoot)), mDatasetPath(std::move(datasetPath)),
      mCameraIdentity(std::move(cameraIdentity)) {}

QJsonObject TrainingRequestStore::load(QString *error) const {
  if (error) error->clear();
  if (mWorkspaceRoot.isEmpty() || mDatasetPath.isEmpty()) return {};
  const auto requests = readStore(QDir(mWorkspaceRoot).filePath(
      QStringLiteral(".gsw/training-requests.json")), error);
  const auto request = requests.value(scope(mWorkspaceRoot, mDatasetPath, mCameraIdentity)).toObject();
  if (!request.isEmpty() && !validRequest(request)) {
    fail(error, "training_request_store_invalid"); return {};
  }
  return request;
}

bool TrainingRequestStore::save(const QJsonObject &request, QString *error) const {
  if (error) error->clear();
  if (mWorkspaceRoot.isEmpty() || mDatasetPath.isEmpty() || !validRequest(request)) {
    fail(error, "training_request_store_invalid"); return false;
  }
  const QString directory = QDir(mWorkspaceRoot).filePath(QStringLiteral(".gsw"));
  const QString path = QDir(directory).filePath(QStringLiteral("training-requests.json"));
  if (!safePath(path) || !QDir().mkpath(directory)) {
    fail(error, "training_request_store_unsafe_path"); return false;
  }
  QLockFile lock(QDir(directory).filePath(QStringLiteral("training-requests.lock")));
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) { fail(error, "training_request_store_busy"); return false; }
  QString readError;
  auto requests = readStore(path, &readError);
  if (!readError.isEmpty()) { if (error) *error = readError; return false; }
  requests.insert(scope(mWorkspaceRoot, mDatasetPath, mCameraIdentity), request);
  const QByteArray bytes = QJsonDocument(QJsonObject{
      {QStringLiteral("schema"), QStringLiteral("gsw.training_requests/1")},
      {QStringLiteral("requests"), requests}}).toJson(QJsonDocument::Indented);
  if (bytes.size() > maximumStoreBytes) {
    fail(error, "training_request_store_size_limit"); return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
    fail(error, "training_request_store_write_failed"); return false;
  }
  return true;
}

} // namespace gsw

#include "RuntimeComponentEnvironment.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace gsw {
namespace {
bool keysAre(const QJsonObject &object, const QStringList &keys) {
  if (object.size() != keys.size()) return false;
  for (auto it = object.begin(); it != object.end(); ++it)
    if (!keys.contains(it.key())) return false;
  return true;
}

bool safeAbsolutePath(const QString &path, const bool directory) {
  if (!QDir::isAbsolutePath(path) || path.startsWith(QStringLiteral("//")) ||
      path.startsWith(QStringLiteral("\\\\")) || path.contains(QChar::Null) ||
      path.split(QRegularExpression(QStringLiteral("[/\\\\]"))).contains(QStringLiteral(".."))) return false;
  QFileInfo item(QDir::cleanPath(path));
  if (directory ? !item.isDir() : !item.isFile()) return false;
  for (;;) {
    if (item.isSymbolicLink() || item.isJunction()) return false;
    const QString parent = item.absolutePath();
    if (parent == item.absoluteFilePath()) return true;
    item.setFile(parent);
  }
}

bool digestMatches(const QString &path, const QJsonValue &value) {
  static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
  if (!value.isString() || !hash.match(value.toString()).hasMatch() || !safeAbsolutePath(path, false)) return false;
  QFile file(path);
  if (file.size() > 4 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) return false;
  QCryptographicHash digest(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    const QByteArray bytes = file.read(1024 * 1024);
    if (bytes.isEmpty() && file.error() != QFileDevice::NoError) return false;
    digest.addData(bytes);
  }
  return QString::fromLatin1(digest.result().toHex()) == value.toString();
}
}

QString runtimeComponentInstallationRoot(const QString &applicationDirectory) {
  // QDir::absoluteFilePath preserves "..". Normalize only this trusted
  // executable-directory derivation, never component roots from JSON.
  return QDir::cleanPath(QDir(applicationDirectory).absoluteFilePath(QStringLiteral("..")));
}

RuntimeComponentEnvironmentResult resolveRuntimeComponents(
    const QString &installationRoot, const QProcessEnvironment &inherited) {
  RuntimeComponentEnvironmentResult result{inherited, {}, {}, {}, {}};
  struct Component { const char *key; const char *variable; const char *manifest; bool acceptance; };
  const Component components[] = {
      {"camera_rasterizer", "GSW_CAMERA_RASTERIZER_ROOT", "camera-rasterizer-proof.json", false},
      {"quality_rasterizer", "GSW_QUALITY_KERNEL_ROOT", "quality-rasterizer-build.json", true}};
  for (const auto &component : components)
    if (inherited.contains(QString::fromLatin1(component.variable))) result.overridden.append(QString::fromLatin1(component.variable));
  // Explicit complete user configuration never consults installation defaults.
  if (result.overridden.size() == 2) return result;
  const QString path = QDir(installationRoot).filePath(QStringLiteral("runtime-components.json"));
  const QFileInfo info(path);
  if (!info.exists() && !info.isSymbolicLink() && !info.isJunction()) return result;
  const auto fail = [&](const char *code, const QString &detail) {
    result.errorCode = QString::fromLatin1(code);
    result.errorDetail = detail;
    return result;
  };
  if (!safeAbsolutePath(path, false) || info.size() > 64 * 1024)
    return fail("runtime_components_file_unsafe", path);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return fail("runtime_components_read_failed", path);
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    return fail("runtime_components_invalid_json", path);
  const auto object = document.object();
  if (!keysAre(object, {QStringLiteral("schema"), QStringLiteral("components")}) ||
      object.value(QStringLiteral("schema")) != QStringLiteral("gsw.runtime_components/1") ||
      !object.value(QStringLiteral("components")).isObject())
    return fail("runtime_components_schema_invalid", path);
  const auto values = object.value(QStringLiteral("components")).toObject();
  if (values.isEmpty() || values.size() > 2) return fail("runtime_components_schema_invalid", path);
  for (auto it = values.begin(); it != values.end(); ++it)
    if (it.key() != QStringLiteral("camera_rasterizer") && it.key() != QStringLiteral("quality_rasterizer"))
      return fail("runtime_components_schema_invalid", it.key());
  QList<QPair<QString, QString>> additions;
  for (const auto &component : components) {
    const QString key = QString::fromLatin1(component.key), variable = QString::fromLatin1(component.variable);
    if (!values.contains(key)) continue;
    if (!values.value(key).isObject()) return fail("runtime_components_schema_invalid", key);
    const auto entry = values.value(key).toObject();
    QStringList keys{QStringLiteral("root"), QStringLiteral("manifest_sha256")};
    if (component.acceptance) keys.append(QStringLiteral("acceptance_sha256"));
    if (!keysAre(entry, keys) || !entry.value(QStringLiteral("root")).isString() ||
        !entry.value(QStringLiteral("manifest_sha256")).isString() ||
        component.acceptance && !entry.value(QStringLiteral("acceptance_sha256")).isString())
      return fail("runtime_components_schema_invalid", key);
    if (inherited.contains(variable)) continue;
    const QString root = entry.value(QStringLiteral("root")).toString();
    if (!safeAbsolutePath(root, true)) return fail("runtime_component_path_invalid", key + QStringLiteral(": ") + root);
    const QString metadata = QDir(root).filePath(QString::fromLatin1(component.manifest));
    if (!digestMatches(metadata, entry.value(QStringLiteral("manifest_sha256"))))
      return fail("runtime_component_hash_mismatch", metadata);
    if (component.acceptance) {
      const QString acceptance = QDir(root).filePath(QStringLiteral("quality-rasterizer-acceptance.json"));
      if (!digestMatches(acceptance, entry.value(QStringLiteral("acceptance_sha256"))))
        return fail("runtime_component_hash_mismatch", acceptance);
    }
    additions.append({variable, QDir::cleanPath(root)});
  }
  // Commit only after all non-overridden components validate. Failed resolution
  // never leaks a half-applied overlay or modifies caller/process/global state.
  for (const auto &entry : additions) {
    result.environment.insert(entry.first, entry.second);
    result.inserted.append(entry.first);
  }
  return result;
}

QString runtimeComponentEnvironmentError(const RuntimeComponentEnvironmentResult &result) {
  return QCoreApplication::translate("Workbench", "运行组件配置无效，未启动 Python：%1\n%2")
      .arg(result.errorCode, result.errorDetail);
}
} // namespace gsw

#include "RuntimeComponentEnvironment.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <iostream>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
int failures = 0;
int checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
bool write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QString sha(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto arguments = app.arguments();
  if (arguments.size() < 3) return 2;
  QDir().mkpath(arguments[2]);
  QTemporaryDir fixture(QDir(arguments[2]).filePath(QStringLiteral("runtime-components-XXXXXX")));
  if (!fixture.isValid()) return 2;
  const QString root = fixture.path();
  const QString stock = QDir(root).filePath(QStringLiteral("stock-v2"));
  const QString quality = QDir(root).filePath(QStringLiteral("quality-build3"));
  QDir().mkpath(stock); QDir().mkpath(quality);
  const QByteArray proof("{\"fixture\":\"not-GPU-acceptance\"}"), build("{\"fixture\":\"build\"}"), acceptance("{\"fixture\":\"acceptance\"}");
  write(QDir(stock).filePath(QStringLiteral("camera-rasterizer-proof.json")), proof);
  write(QDir(quality).filePath(QStringLiteral("quality-rasterizer-build.json")), build);
  write(QDir(quality).filePath(QStringLiteral("quality-rasterizer-acceptance.json")), acceptance);
  QProcessEnvironment base;
  base.insert(QStringLiteral("PATH"), QStringLiteral("unchanged-path"));
  base.insert(QStringLiteral("PYTHONPATH"), QStringLiteral("unchanged-pythonpath"));
  const auto absent = gsw::resolveRuntimeComponents(root, base);
  check(absent.ready() && absent.environment.toStringList() == base.toStringList(), "absent config preserves all inherited entries");
  const QJsonObject components{
      {QStringLiteral("camera_rasterizer"), QJsonObject{{QStringLiteral("root"), stock}, {QStringLiteral("manifest_sha256"), sha(proof)}}},
      {QStringLiteral("quality_rasterizer"), QJsonObject{{QStringLiteral("root"), quality}, {QStringLiteral("manifest_sha256"), sha(build)},
          {QStringLiteral("acceptance_sha256"), sha(acceptance)}}}};
  const QJsonObject manifest{{QStringLiteral("schema"), QStringLiteral("gsw.runtime_components/1")},
      {QStringLiteral("components"), components}};
  const QString manifestPath = QDir(root).filePath(QStringLiteral("runtime-components.json"));
  write(manifestPath, QJsonDocument(manifest).toJson());
  const auto resolved = gsw::resolveRuntimeComponents(root, base);
  check(resolved.ready(), "valid per-install binding resolves");
  check(resolved.environment.value(QStringLiteral("GSW_CAMERA_RASTERIZER_ROOT")) == stock, "camera component enters only child environment");
  check(resolved.environment.value(QStringLiteral("GSW_QUALITY_KERNEL_ROOT")) == quality, "quality component enters only child environment");
  check(base.toStringList() == absent.environment.toStringList(), "caller environment remains immutable");
  const QString applicationDirectory = QDir(root).filePath(QStringLiteral("bin"));
  QDir().mkpath(applicationDirectory);
  const QString installationRoot = gsw::runtimeComponentInstallationRoot(applicationDirectory);
  const auto fromApplicationDirectory = gsw::resolveRuntimeComponents(installationRoot, base);
  check(fromApplicationDirectory.ready() &&
      fromApplicationDirectory.environment.value(QStringLiteral("GSW_CAMERA_RASTERIZER_ROOT")) == stock &&
      fromApplicationDirectory.environment.value(QStringLiteral("GSW_QUALITY_KERNEL_ROOT")) == quality,
      "real bin-to-parent caller path resolves the installation manifest before launching Python");
  const auto globalBefore = QProcessEnvironment::systemEnvironment().toStringList();
  auto explicitEnvironment = base;
  explicitEnvironment.insert(QStringLiteral("GSW_CAMERA_RASTERIZER_ROOT"), QStringLiteral("explicit-old-stock"));
  auto overridden = gsw::resolveRuntimeComponents(root, explicitEnvironment);
  check(overridden.ready() && overridden.environment.value(QStringLiteral("GSW_CAMERA_RASTERIZER_ROOT")) == QStringLiteral("explicit-old-stock"),
      "explicit component override remains verbatim");
  check(overridden.environment.value(QStringLiteral("GSW_QUALITY_KERNEL_ROOT")) == quality, "only missing component is injected");
  explicitEnvironment.insert(QStringLiteral("GSW_QUALITY_KERNEL_ROOT"), QString());
  write(manifestPath, QByteArray("not JSON"));
  overridden = gsw::resolveRuntimeComponents(root, explicitEnvironment);
  check(overridden.ready() && overridden.environment.toStringList() == explicitEnvironment.toStringList(),
      "complete explicit environment including empty value does not consult defaults");
  auto invalid = gsw::resolveRuntimeComponents(root, base);
  check(!invalid.ready() && invalid.environment.toStringList() == base.toStringList(), "invalid JSON preserves complete inherited environment and blocks launch");
  check(!gsw::runtimeComponentEnvironmentError(invalid).isEmpty(), "typed failure has a user-visible error return");

  auto modified = components;
  auto wrongQuality = modified.value(QStringLiteral("quality_rasterizer")).toObject();
  wrongQuality.insert(QStringLiteral("manifest_sha256"), QString(64, QLatin1Char('0')));
  modified.insert(QStringLiteral("quality_rasterizer"), wrongQuality);
  write(manifestPath, QJsonDocument(QJsonObject{{QStringLiteral("schema"), QStringLiteral("gsw.runtime_components/1")},
      {QStringLiteral("components"), modified}}).toJson());
  invalid = gsw::resolveRuntimeComponents(root, base);
  check(!invalid.ready() && invalid.inserted.isEmpty() && invalid.environment.toStringList() == base.toStringList(),
      "late component failure never commits a partial camera overlay");

  for (const auto &badRoot : {QStringLiteral("../relative"), QStringLiteral("//server/share"), root + QStringLiteral("/../stock-v2"),
      root + QStringLiteral("/bin/../stock-v2"), root + QStringLiteral("/missing")}) {
    modified = components;
    auto row = modified.value(QStringLiteral("camera_rasterizer")).toObject();
    row.insert(QStringLiteral("root"), badRoot);
    modified.insert(QStringLiteral("camera_rasterizer"), row);
    write(manifestPath, QJsonDocument(QJsonObject{{QStringLiteral("schema"), QStringLiteral("gsw.runtime_components/1")},
        {QStringLiteral("components"), modified}}).toJson());
    check(!gsw::resolveRuntimeComponents(root, base).ready(), "unsafe or absent local component root is refused");
  }
  auto unknown = manifest;
  unknown.insert(QStringLiteral("environment"), QJsonObject{{QStringLiteral("PATH"), QStringLiteral("cannot-inject")}});
  write(manifestPath, QJsonDocument(unknown).toJson());
  check(!gsw::resolveRuntimeComponents(root, base).ready(), "unknown environment injection field is rejected");
  modified = components;
  modified.insert(QStringLiteral("unknown_component"), QJsonObject{});
  unknown = manifest; unknown.insert(QStringLiteral("components"), modified);
  write(manifestPath, QJsonDocument(unknown).toJson());
  check(!gsw::resolveRuntimeComponents(root, base).ready(), "only two fixed component identifiers are accepted");

  write(manifestPath, QJsonDocument(manifest).toJson());
  write(QDir(quality).filePath(QStringLiteral("quality-rasterizer-acceptance.json")), QByteArray("changed acceptance"));
  check(!gsw::resolveRuntimeComponents(root, base).ready(), "acceptance metadata hash must match exactly");
  write(QDir(quality).filePath(QStringLiteral("quality-rasterizer-acceptance.json")), acceptance);
  const QString alias = QDir(root).filePath(QStringLiteral("stock-junction"));
  QProcess link;
  auto linkEnvironment = QProcessEnvironment::systemEnvironment();
  link.setProcessEnvironment(linkEnvironment);
#ifdef Q_OS_WIN
  link.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *value) { value->flags |= CREATE_NO_WINDOW; });
  link.start(QStringLiteral("cmd.exe"), {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
      QDir::toNativeSeparators(alias), QDir::toNativeSeparators(stock)});
  const bool linkFinished = link.waitForFinished(10000);
  if (!linkFinished || link.exitCode() != 0) std::cerr << link.errorString().toStdString() << '\n'
      << link.readAllStandardOutput().constData() << link.readAllStandardError().constData() << '\n';
  check(linkFinished && link.exitCode() == 0, "create actual owned directory junction for refusal test");
#else
  check(QFile::link(stock, alias), "create actual owned directory link for refusal test");
#endif
  modified = components;
  auto linked = modified.value(QStringLiteral("camera_rasterizer")).toObject();
  linked.insert(QStringLiteral("root"), alias);
  modified.insert(QStringLiteral("camera_rasterizer"), linked);
  unknown = manifest; unknown.insert(QStringLiteral("components"), modified);
  write(manifestPath, QJsonDocument(unknown).toJson());
  check(!gsw::resolveRuntimeComponents(root, base).ready(), "directory junction root is refused before metadata read");
  QDir().rmdir(alias);
  write(manifestPath, QJsonDocument(manifest).toJson());
  auto childBase = QProcessEnvironment::systemEnvironment();
  childBase.remove(QStringLiteral("GSW_CAMERA_RASTERIZER_ROOT"));
  childBase.remove(QStringLiteral("GSW_QUALITY_KERNEL_ROOT"));
  childBase.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
  childBase.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  childBase.insert(QStringLiteral("CUDA_VISIBLE_DEVICES"), QStringLiteral("-1"));
  childBase.insert(QStringLiteral("TEMP"), root); childBase.insert(QStringLiteral("TMP"), root);
  const auto childEnvironment = gsw::resolveRuntimeComponents(root, childBase);
  QProcess child;
  child.setProcessEnvironment(childEnvironment.environment);
#ifdef Q_OS_WIN
  child.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *value) { value->flags |= CREATE_NO_WINDOW; });
#endif
  child.start(arguments[1], {QStringLiteral("-B"), QStringLiteral("-c"), QStringLiteral(
      "import os,json;print(json.dumps({k:os.environ.get(k) for k in ('GSW_CAMERA_RASTERIZER_ROOT','GSW_QUALITY_KERNEL_ROOT')}))")});
  check(child.waitForFinished(15000) && child.exitCode() == 0, "hidden CPU Python echo child exits successfully");
  const auto echoed = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
  check(echoed.value(QStringLiteral("GSW_CAMERA_RASTERIZER_ROOT")) == stock &&
      echoed.value(QStringLiteral("GSW_QUALITY_KERNEL_ROOT")) == quality, "actual Python child inherits both per-install bindings");
  check(QProcessEnvironment::systemEnvironment().toStringList() == globalBefore, "no process/global environment values were changed");
  std::cout << checks << " checks, " << failures << " failures\n";
  return failures ? 1 : 0;
}

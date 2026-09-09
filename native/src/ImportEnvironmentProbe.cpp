#include <QCoreApplication>
#include "ImportEnvironmentProbe.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>

namespace gsw {

ImportEnvironmentProbeResult
ImportEnvironmentProbe::run(const QString &python, const QString &backendRoot,
                            const bool requireVideo,
                            const QProcessEnvironment &environment,
                            const int timeoutMilliseconds) {
  ImportEnvironmentProbeResult result;
  result.python = QDir::toNativeSeparators(python);

  const QString script =
      QDir(backendRoot).filePath(QStringLiteral("native/worker/import_preflight.py"));
  if (!QFileInfo(python).isFile()) {
    result.errorMessage = QCoreApplication::translate("Workbench", "所选 Python 不存在：%1").arg(result.python);
    return result;
  }
  if (!QFileInfo(script).isFile()) {
    result.errorMessage = QCoreApplication::translate("Workbench", "安装目录缺少导入预检脚本：%1")
                              .arg(QDir::toNativeSeparators(script));
    return result;
  }

  QStringList arguments = {QStringLiteral("-B"), script,
                           QStringLiteral("--backend-root"), backendRoot};
  if (requireVideo) {
    arguments.append(QStringLiteral("--require-video"));
  }

  QProcess process;
  process.setWorkingDirectory(backendRoot);
  process.setProcessEnvironment(environment);
  process.setProgram(python);
  process.setArguments(arguments);
  process.start();
  if (!process.waitForStarted(5000)) {
    result.errorMessage = QCoreApplication::translate("Workbench", "无法启动所选 Python：%1")
                              .arg(process.errorString());
    return result;
  }
  if (!process.waitForFinished(timeoutMilliseconds)) {
    process.kill();
    process.waitForFinished(5000);
    result.errorMessage = QCoreApplication::translate("Workbench", "导入环境预检超时（%1 秒）。")
                              .arg(timeoutMilliseconds / 1000);
    return result;
  }

  const QByteArray standardOutput = process.readAllStandardOutput();
  const QByteArray standardError = process.readAllStandardError();
  const QList<QByteArray> outputLines = standardOutput.trimmed().split('\n');
  const QByteArray reportLine = outputLines.isEmpty() ? QByteArray{} : outputLines.constLast().trimmed();
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(reportLine, &parseError);
  if (!document.isObject()) {
    QString detail = QString::fromUtf8(standardError).trimmed();
    if (detail.isEmpty()) {
      detail = QString::fromUtf8(standardOutput).trimmed();
    }
    result.errorMessage = QCoreApplication::translate("Workbench", "导入环境预检未返回有效报告：%1")
                              .arg(detail.isEmpty() ? parseError.errorString() : detail);
    return result;
  }

  const QJsonObject report = document.object();
  result.ready = report.value(QStringLiteral("ready")).toBool(false) &&
                 process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
  result.python = QDir::toNativeSeparators(
      report.value(QStringLiteral("python")).toString(result.python));
  result.videoBackend = report.value(QStringLiteral("videoBackend")).toString();
  if (!result.ready) {
    QString detail = report.value(QStringLiteral("error")).toString().trimmed();
    if (detail.isEmpty()) {
      detail = QString::fromUtf8(standardError).trimmed();
    }
    if (detail.isEmpty()) {
      detail = QCoreApplication::translate("Workbench", "预检进程退出代码 %1").arg(process.exitCode());
    }
    result.errorMessage = detail;
  }
  return result;
}

} // namespace gsw

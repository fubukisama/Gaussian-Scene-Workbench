#include <QCoreApplication>
#include "TrainingEnvironmentProbe.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>

namespace gsw {

TrainingEnvironmentProbeResult TrainingEnvironmentProbe::run(
    const QString &python, const QString &backendRoot,
    const QString &datasetPath, const QString &backend, const bool runColmap,
    const QProcessEnvironment &environment, const int timeoutMilliseconds) {
  TrainingEnvironmentProbeResult result;
  result.python = QDir::toNativeSeparators(python);

  const QString script = QDir(backendRoot).filePath(
      QStringLiteral("native/worker/training_preflight.py"));
  if (!QFileInfo(python).isFile()) {
    result.errorMessage = QCoreApplication::translate("Workbench", "所选 Python 不存在：%1").arg(result.python);
    return result;
  }
  if (!QFileInfo(script).isFile()) {
    result.errorMessage = QCoreApplication::translate("Workbench", "安装目录缺少训练预检脚本：%1")
                              .arg(QDir::toNativeSeparators(script));
    return result;
  }

  QStringList arguments = {
      QStringLiteral("-B"), script,
      QStringLiteral("--backend-root"), backendRoot,
      QStringLiteral("--dataset"), datasetPath,
      QStringLiteral("--backend"), backend};
  if (runColmap) {
    arguments.append(QStringLiteral("--run-colmap"));
  }

  QProcess process;
  process.setWorkingDirectory(backendRoot);
  process.setProcessEnvironment(environment);
  process.setProgram(python);
  process.setArguments(arguments);
  process.start();
  if (!process.waitForStarted(5000)) {
    result.errorMessage = QCoreApplication::translate("Workbench", "无法启动训练预检：%1")
                              .arg(process.errorString());
    return result;
  }
  if (!process.waitForFinished(timeoutMilliseconds)) {
    process.kill();
    process.waitForFinished(5000);
    result.errorMessage = QCoreApplication::translate("Workbench", "训练环境预检超时（%1 秒）。")
                              .arg(timeoutMilliseconds / 1000);
    return result;
  }

  const QByteArray standardOutput = process.readAllStandardOutput();
  const QByteArray standardError = process.readAllStandardError();
  const QList<QByteArray> outputLines = standardOutput.trimmed().split('\n');
  const QByteArray reportLine = outputLines.isEmpty()
                                    ? QByteArray{}
                                    : outputLines.constLast().trimmed();
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(reportLine, &parseError);
  if (!document.isObject()) {
    QString detail = QString::fromUtf8(standardError).trimmed();
    if (detail.isEmpty()) {
      detail = QString::fromUtf8(standardOutput).trimmed();
    }
    result.errorMessage = QCoreApplication::translate("Workbench", "训练环境预检未返回有效报告：%1")
                              .arg(detail.isEmpty() ? parseError.errorString() : detail);
    return result;
  }

  const QJsonObject report = document.object();
  result.ready = report.value(QStringLiteral("ready")).toBool(false) &&
                 process.exitStatus() == QProcess::NormalExit &&
                 process.exitCode() == 0;
  result.policyBlocked = report.value(QStringLiteral("policyBlocked")).toBool(false);
  result.hasReconstruction = report.value(QStringLiteral("hasReconstruction")).toBool(false);
  result.imageCount = report.value(QStringLiteral("imageCount")).toInt(0);
  result.python = QDir::toNativeSeparators(
      report.value(QStringLiteral("python")).toString(result.python));
  result.cudaDevice = report.value(QStringLiteral("cudaDevice")).toString();
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

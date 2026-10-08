#include "TrainingReconstructionSummary.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

namespace gsw {
TrainingReconstructionSummaryScanner::TrainingReconstructionSummaryScanner(QObject *parent)
    : QObject(parent) {}

TrainingReconstructionSummaryScanner::~TrainingReconstructionSummaryScanner() { cancel(); }

void TrainingReconstructionSummaryScanner::cancel() {
  ++mGeneration;
  if (!mProcess) return;
  auto *process = mProcess;
  mProcess = nullptr;
  process->disconnect(this);
  if (process->state() != QProcess::NotRunning) process->kill();
  process->deleteLater();
}

void TrainingReconstructionSummaryScanner::request(
    const QString &datasetPath, const bool runColmap, const QString &python,
    const QString &backendRoot, const QProcessEnvironment &environment) {
  cancel();
  const auto generation = mGeneration;
  const QString script = QDir(backendRoot).filePath(
      QStringLiteral("native/worker/training_reconstruction_summary.py"));
  if (!QFileInfo(python).isFile() || !QFileInfo(script).isFile()) {
    const QString diagnostic = !QFileInfo(python).isFile() ? python : script;
    QTimer::singleShot(0, this, [this, generation, diagnostic] {
      if (generation == mGeneration) emit summaryReady({{}, diagnostic, false});
    });
    return;
  }
  auto *process = new QProcess(this);
  mProcess = process;
  process->setWorkingDirectory(backendRoot);
  process->setProcessEnvironment(environment);
  process->setProgram(python);
  QStringList arguments{QStringLiteral("-B"), script,
                        QStringLiteral("--backend-root"), backendRoot,
                        QStringLiteral("--dataset"), datasetPath};
  if (runColmap) arguments.append(QStringLiteral("--run-colmap"));
  process->setArguments(arguments);
  auto *timeout = new QTimer(process);
  timeout->setSingleShot(true);
  connect(timeout, &QTimer::timeout, this, [this, process, generation] {
    if (generation != mGeneration || process != mProcess) return;
    cancel();
    emit summaryReady({{}, QStringLiteral("timeout"), false});
  });
  connect(process, &QProcess::errorOccurred, this,
          [this, process, generation](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart || generation != mGeneration || process != mProcess) return;
    const QString diagnostic = process->errorString();
    cancel();
    emit summaryReady({{}, diagnostic, false});
  });
  connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, process, generation, timeout](int exitCode, QProcess::ExitStatus exitStatus) {
    if (generation != mGeneration || process != mProcess) return;
    timeout->stop();
    TrainingReconstructionSummary summary;
    const QByteArray output = process->readAllStandardOutput();
    if (output.size() <= 4 * 1024 * 1024) {
      const auto document = QJsonDocument::fromJson(output.trimmed());
      summary.report = document.object();
      summary.ready = document.isObject() && exitCode == 0 &&
                      exitStatus == QProcess::NormalExit &&
                      summary.report.value(QStringLiteral("version")).toInt() == 1 &&
                      summary.report.value(QStringLiteral("ready")).toBool();
      summary.diagnostic = summary.report.value(QStringLiteral("diagnostic")).toString();
    }
    if (!summary.ready && summary.diagnostic.isEmpty())
      summary.diagnostic = QString::fromUtf8(process->readAllStandardError()).trimmed();
    mProcess = nullptr;
    process->deleteLater();
    emit summaryReady(summary);
  });
  timeout->start(30000);
  process->start();
}
}

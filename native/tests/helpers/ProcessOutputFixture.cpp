#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdio>

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("summary-worker")) {
    QJsonObject summary{{"version", 1}, {"phase", "loaded"}, {"backend", "3dgs"},
        {"quality", "original_quality"}, {"iterations", 30000}, {"resolution", 1},
        {"optimizer", "default"}, {"densifyUntil", 0}, {"densificationInterval", 80},
        {"densifyGradient", .00012}, {"antialiasing", true}, {"exposureCompensation", false},
        {"trainImageCount", 2}, {"trainDimensionKinds", 1},
        {"trainDimensions", QJsonArray{QJsonValue(QJsonArray{1928, 1084, 2})}},
        {"trainPixels", qint64(1928) * 1084 * 2}};
    const auto publish = [&](const QJsonObject &metadata) {
      const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 1}, {"type", "status"},
          {"state", "running"}, {"stage", "train"}, {"iteration", 3},
          {"trainingSummary", metadata}}).toJson(QJsonDocument::Compact);
      std::printf("[worker-event] %s\n", bytes.constData());
      std::fflush(stdout);
    };
    publish(summary);
    auto invalid = summary; invalid["trainPixels"] = 1; publish(invalid);
    invalid = summary; invalid["resolution"] = true; publish(invalid);
    invalid = summary; invalid["trainDimensions"] = QJsonArray{QJsonArray{1928, 1084}}; publish(invalid);
    invalid = summary; invalid["version"] = 2; publish(invalid);
    invalid = summary; invalid["trainPixels"] = 1.0e20; publish(invalid);
    summary["backend"] = QStringLiteral("2dgs"); summary["optimizer"] = QStringLiteral("adam");
    summary["depthRatio"] = 0.0; summary["resolution"] = 16; publish(summary);
    return 0;
  }
  if (arguments.size() == 5 && arguments.at(1) == QStringLiteral("completion-worker")) {
    const bool mesh = arguments.at(4) == QStringLiteral("mesh");
    const bool sparse = arguments.at(4) == QStringLiteral("colmap_sparse");
    const QByteArray bytes = QJsonDocument(QJsonObject{
        {"version", 1}, {"type", "status"}, {"state", "running"},
        {"stage", sparse ? "colmap" : mesh ? "mesh_ready" : "train"}, {"previewPath", arguments.at(2)},
        {"previewKind", arguments.at(4)}, {"previewIteration", 1}, {"gaussianCount", 4}})
        .toJson(QJsonDocument::Compact);
    std::printf("[worker-event] %s\n", bytes.constData()); std::fflush(stdout);
    QDeadlineTimer deadline(10000);
    while (!QFileInfo::exists(arguments.at(3)) && !deadline.hasExpired()) QThread::msleep(10);
    if (!QFileInfo::exists(arguments.at(3))) return 7;
    std::puts("[worker-event] {\"version\":1,\"type\":\"status\",\"state\":\"done\",\"stage\":\"done\"}");
    std::fflush(stdout);
    return 0;
  }
  if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("mesh-worker")) {
    const auto publish = [&](const QString &state, const QString &stage) {
      const QByteArray bytes = QJsonDocument(QJsonObject{
          {"version", 1}, {"type", "status"}, {"state", state}, {"stage", stage},
          {"previewPath", arguments.at(2)}, {"previewKind", "mesh"}, {"previewIteration", 1}})
          .toJson(QJsonDocument::Compact);
      std::printf("[worker-event] %s\n", bytes.constData());
      std::fflush(stdout);
    };
    publish(QStringLiteral("running"), QStringLiteral("mesh_ready"));
    QThread::msleep(250);
    const bool failed = arguments.at(3) == QStringLiteral("failed");
    publish(failed ? QStringLiteral("failed") : QStringLiteral("done"), QStringLiteral("done"));
    return failed ? 1 : 0;
  }
  if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("pause-worker")) {
    char command[32] = {};
    if (!std::fgets(command, sizeof(command), stdin) || QByteArray(command).trimmed() != "pause") return 8;
    std::puts("[worker-event] {\"version\":1,\"type\":\"status\",\"state\":\"paused\",\"stage\":\"paused\"}");
    std::fflush(stdout);
    return 75;
  }
  if (arguments.size() >= 2 && arguments.at(1) == QStringLiteral("tree-child")) {
    QDeadlineTimer deadline(60000);
    while (!deadline.hasExpired()) {
      QThread::msleep(100);
    }
    return 0;
  }
  if (arguments.size() == 3 &&
      (arguments.at(1) == QStringLiteral("tree-parent") ||
       arguments.at(1) == QStringLiteral("tree-parent-exits"))) {
    qint64 childProcessId = 0;
    if (!QProcess::startDetached(application.applicationFilePath(),
                                 {QStringLiteral("tree-child")}, {},
                                 &childProcessId)) {
      return 6;
    }
    QFile ready(arguments.at(2));
    if (!ready.open(QIODevice::WriteOnly) ||
        ready.write(QByteArray::number(childProcessId)) <= 0) {
      return 7;
    }
    ready.close();
    if (arguments.at(1) == QStringLiteral("tree-parent-exits")) {
      QTimer::singleShot(300, &application, &QCoreApplication::quit);
    }
    return application.exec();
  }
  if (application.arguments().size() != 3) {
    return 2;
  }

#ifdef Q_OS_WIN
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  QFile output;
  if (!output.open(stdout, QIODevice::WriteOnly)) {
    return 3;
  }

  const QString readyPath = application.arguments().at(1);
  const QString releasePath = application.arguments().at(2);
  output.write("[worker-event] {\"version\":1,\"type\":\"status\",\"state\":\"run");
  output.flush();

  QFile ready(readyPath);
  if (!ready.open(QIODevice::WriteOnly) || ready.write("ready") != 5) {
    return 4;
  }
  ready.close();

  QDeadlineTimer deadline(10000);
  while (!QFileInfo::exists(releasePath) && !deadline.hasExpired()) {
    QThread::msleep(10);
  }
  if (!QFileInfo::exists(releasePath)) {
    return 5;
  }

  output.write(
      "ning\",\"stage\":\"train\",\"progressPercent\":37,"
      "\"iteration\":11100,\"totalIterations\":30000,"
      "\"loss\":0.0234,\"psnr\":27.5,\"gaussianCount\":123456,"
      "\"iterationMilliseconds\":12.5,\"elapsedSeconds\":144.0,"
      "\"previewIteration\":10000,"
      "\"densityGuardIteration\":3100,\"densityGuardDeferred\":3300,"
      "\"previewPath\":\"E:/model/point_cloud.ply\","
      "\"previewKind\":\"colmap_sparse\"}\n"
      "[gsw-training-gpu-preview] {\"version\":2,\"type\":\"gpu_preview\","
      "\"state\":\"ready\",\"sessionId\":\"fixture-session\","
      "\"producerPid\":4242,\"memoryHandle\":\"0x41c\","
      "\"memoryHandleType\":\"opaque_win32_kmt\","
      "\"allocationBytes\":112000,\"slotBytes\":56000,"
      "\"slotCount\":2,\"strideBytes\":56,\"capacity\":1000,"
      "\"controlMapping\":\"Local\\\\GSW-GPU-fixture-control\","
      "\"frameEvent\":\"Local\\\\GSW-GPU-fixture-frame\","
      "\"releaseEvent0\":\"Local\\\\GSW-GPU-fixture-release-0\","
      "\"releaseEvent1\":\"Local\\\\GSW-GPU-fixture-release-1\","
      "\"device\":\"NVIDIA fixture\"} [30/07 02:25:25]\nplain log\n");
  output.flush();
  return 0;
}

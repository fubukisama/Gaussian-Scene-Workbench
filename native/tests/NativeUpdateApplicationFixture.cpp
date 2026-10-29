// SPDX-License-Identifier: GPL-3.0-or-later
// Remote-only executable fixture. This is NOT the GSW application and its
// marker must never be described as formal GSW startup/renderer acceptance.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QProcess>
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include "NativeStartupHealth.h"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef GSW_UPDATE_FIXTURE_LABEL
#error A distinct A/B fixture label is required.
#endif

namespace {
class StartupSurface final : public QWidget {
public:
  explicit StartupSurface(QWidget *parent) : QWidget(parent) { setMinimumSize(320, 70); }
  [[nodiscard]] bool painted() const { return mPainted; }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(34, 42, 52));
    painter.setPen(QColor(100, 180, 210));
    painter.drawRect(rect().adjusted(8, 8, -9, -9));
    mPainted = true;
  }
private:
  bool mPainted = false;
};
bool marker(const QString &path, const QJsonObject &record, QJsonObject *diagnostic = nullptr) {
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
  const bool opened = file.open(QIODevice::WriteOnly);
  const qint64 written = opened ? file.write(bytes) : -1;
  const bool committed = written == bytes.size() && file.commit();
  if (diagnostic) {
    diagnostic->insert(QStringLiteral("opened"), opened);
    diagnostic->insert(QStringLiteral("writtenBytes"), double(written));
    diagnostic->insert(QStringLiteral("committed"), committed);
    diagnostic->insert(QStringLiteral("fileError"), int(file.error()));
  }
  return committed;
}
void trace(const QString &path, const QElapsedTimer &clock, const char *phase,
    QJsonObject fields = {}) {
  // Remote-fixture diagnostics only. Separate from the polled atomic marker;
  // never contains IPC capabilities, environment, repository URLs or user data.
  fields.insert(QStringLiteral("schema"), QStringLiteral("gsw.update_fixture_trace/1"));
  fields.insert(QStringLiteral("testOnly"), true);
  fields.insert(QStringLiteral("pid"), double(QCoreApplication::applicationPid()));
  fields.insert(QStringLiteral("phase"), QLatin1String(phase));
  fields.insert(QStringLiteral("elapsedMs"), double(clock.elapsed()));
  fields.insert(QStringLiteral("utcMs"), double(QDateTime::currentMSecsSinceEpoch()));
  QFile file(path + QStringLiteral(".trace.jsonl"));
  if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
    file.write(QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n');
    file.flush();
  }
}

int peerProbe(const QString &path, const QElapsedTimer &clock, const QString &window) {
  // Deliberately a different OS process with a valid inherited one-shot test
  // capability. This bypasses only the CLIENT's own-window precheck so the
  // actual SERVER's peer-process check is exercised. No capability is logged.
  const auto pipeName = qEnvironmentVariable("GSW_NATIVE_STARTUP_PIPE");
  const auto attempt = qEnvironmentVariable("GSW_NATIVE_STARTUP_ATTEMPT");
  const auto nonce = qEnvironmentVariable("GSW_NATIVE_STARTUP_NONCE");
  if (attempt.size() != 32 || nonce.size() != 64 ||
      pipeName != QStringLiteral("\\\\.\\pipe\\GSW.Startup.") + attempt) return 5;
  const auto message = QJsonDocument(QJsonObject{{"schema", "gsw.native_startup_ready/1"},
      {"attemptId", attempt}, {"nonce", nonce}, {"healthScope", "qt-fixture-window-surface"},
      {"window", window}}).toJson(QJsonDocument::Compact);
#ifdef Q_OS_WIN
  const HANDLE pipe = CreateFileW(reinterpret_cast<LPCWSTR>(pipeName.utf16()), GENERIC_WRITE, 0,
      nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  trace(path, clock, "peer_probe_connected", {{"pipeOpened", pipe != INVALID_HANDLE_VALUE}});
  if (pipe == INVALID_HANDLE_VALUE) return 5;
  const HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!completion) { CloseHandle(pipe); return 5; }
  OVERLAPPED operation{}; operation.hEvent = completion;
  DWORD sent = 0;
  bool completed = WriteFile(pipe, message.constData(), DWORD(message.size()), &sent, &operation);
  DWORD error = completed ? ERROR_SUCCESS : GetLastError();
  if (!completed && error == ERROR_IO_PENDING) {
    if (WaitForSingleObject(completion, 300) == WAIT_OBJECT_0)
      completed = GetOverlappedResult(pipe, &operation, &sent, FALSE);
    else {
      CancelIoEx(pipe, &operation);
      GetOverlappedResult(pipe, &operation, &sent, TRUE);
    }
    error = completed ? ERROR_SUCCESS : GetLastError();
  }
  trace(path, clock, "peer_probe_written", {{"writeCompleted", completed},
      {"writtenBytes", double(sent)}, {"osError", double(error)}});
  CloseHandle(completion); CloseHandle(pipe);
  return 0; // A server rejection/broken pipe is expected, never a retry loop.
#else
  Q_UNUSED(message);
  return 5;
#endif
}
}
int main(int argc, char **argv) {
  const QString path = qEnvironmentVariable("GSW_UPDATE_FIXTURE_MARKER");
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) return 3;
  QElapsedTimer clock; clock.start();
  trace(path, clock, "main_enter");
  if (argc == 3 && QByteArray(argv[1]) == "--fixture-health-peer") {
    QCoreApplication app(argc, argv);
    return peerProbe(path, clock, QString::fromLatin1(argv[2]));
  }
  const QString healthMode = qEnvironmentVariable("GSW_UPDATE_FIXTURE_HEALTH_MODE");
  if (!QStringList{QString(), QStringLiteral("exit-before-window"), QStringLiteral("normal-exit-no-ack"),
      QStringLiteral("long-running-no-ack"), QStringLiteral("wrong-peer")}.contains(healthMode)) return 3;
  if (healthMode == QLatin1String("exit-before-window")) {
    QCoreApplication app(argc, argv);
    trace(path, clock, "exit_before_window");
    return marker(path, {{"schema", "gsw.update_application_fixture/1"}, {"testOnly", true},
      {"formalGswAcceptance", false}, {"label", QString::fromLatin1(GSW_UPDATE_FIXTURE_LABEL)},
      {"buildId", QStringLiteral("fixture-") + QString::fromLatin1(GSW_UPDATE_FIXTURE_LABEL).toLower()},
      {"pid", double(QCoreApplication::applicationPid())}, {"phase", "normal_exit"}, {"exitCode", 0},
      {"windowShown", false}, {"viewportPainted", false}, {"fixtureHealthMode", healthMode}}) ? 0 : 4;
  }
  QApplication app(argc, argv);
  trace(path, clock, "application_constructed");
  const QString label = QString::fromLatin1(GSW_UPDATE_FIXTURE_LABEL);
  QWidget window;
  window.setWindowTitle(QString::fromLatin1("GSW isolated update fixture ") + label);
  window.setAttribute(Qt::WA_ShowWithoutActivating);
  window.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
  auto *layout = new QVBoxLayout(&window);
  // Test-only explanatory text; no production UI/catalog is being exercised.
  layout->addWidget(new QLabel(QString::fromUtf8(
      "隔离更新测试程序 / Isolated update fixture / 隔離更新テスト\n") + label, &window));
  auto *surface = new StartupSurface(&window);
  layout->addWidget(surface);
  window.resize(480, 140);
  bool shown = false;
  QJsonObject record{{"schema", "gsw.update_application_fixture/1"}, {"testOnly", true},
      {"formalGswAcceptance", false}, {"label", label},
      {"buildId", QString::fromLatin1("fixture-") + label.toLower()},
      {"pid", double(QCoreApplication::applicationPid())},
      {"executable", QCoreApplication::applicationFilePath()},
      {"installationRoot", qEnvironmentVariable("GSW_NATIVE_INSTALL_ROOT")},
      {"arguments", QJsonArray::fromStringList(app.arguments().mid(1))},
      {"diagnosticsRoot", QDir::fromNativeSeparators(qEnvironmentVariable("GSW_RENDER_DIAGNOSTICS_DIR"))},
      {"qtVersion", QString::fromLatin1(qVersion())},
      {"platform", QGuiApplication::platformName()}};
  record.insert(QStringLiteral("fixtureHealthMode"), healthMode);
  trace(path, clock, "before_show");
  window.show();
  trace(path, clock, "after_show");
  QTimer::singleShot(100, &app, [&] {
    trace(path, clock, "ready_timer");
    shown = window.isVisible() && window.windowHandle();
    record.insert(QString::fromLatin1("windowShown"), shown);
    record.insert(QString::fromLatin1("viewportPainted"), surface->painted());
    record.insert(QString::fromLatin1("healthScope"), QString::fromLatin1("qt-fixture-window-surface"));
    record.insert(QString::fromLatin1("phase"), QString::fromLatin1("shown"));
    QJsonObject markerDiagnostic{{"windowShown", shown}, {"viewportPainted", surface->painted()}};
    trace(path, clock, "before_shown_marker");
    const bool written = shown && surface->painted() && marker(path, record, &markerDiagnostic);
    trace(path, clock, "after_shown_marker", markerDiagnostic);
    if (!written) app.exit(4);
    else {
      if (healthMode.isEmpty()) {
        trace(path, clock, "before_health_hook");
        gsw::reportNativeStartupReady(quintptr(window.winId()), "qt-fixture-window-surface");
        trace(path, clock, "after_health_hook");
      } else if (healthMode == QLatin1String("wrong-peer")) {
        QProcess peer;
        peer.setProgram(QCoreApplication::applicationFilePath());
        peer.setArguments({QStringLiteral("--fixture-health-peer"),
          QString::number(quint64(window.winId()), 16).rightJustified(16, QLatin1Char('0'))});
        peer.setStandardInputFile(QProcess::nullDevice());
        peer.setStandardOutputFile(QProcess::nullDevice());
        peer.setStandardErrorFile(QProcess::nullDevice());
        qint64 peerPid = 0;
        const bool started = peer.startDetached(&peerPid);
        trace(path, clock, "peer_probe_started", {{"started", started}, {"peerPid", double(peerPid)}});
        if (!started) { app.exit(5); return; }
      } else trace(path, clock, "health_ack_suppressed");
      const auto quit = [&] {
        trace(path, clock, "quit_timer");
        app.quit();
      };
      // Fixed, test-only long-running sample. The positive 100/150 ms path,
      // public 15 s observer and driver's 25 s acceptance deadline are unchanged.
      if (healthMode == QLatin1String("long-running-no-ack")) QTimer::singleShot(16000, &app, quit);
      else QTimer::singleShot(150, &app, quit);
    }
  });
  const int result = app.exec();
  trace(path, clock, "event_loop_returned", {{"exitCode", result}});
  record.insert(QString::fromLatin1("phase"), QString::fromLatin1("normal_exit"));
  record.insert(QString::fromLatin1("exitCode"), result);
  QJsonObject finalDiagnostic{{"windowShown", shown}, {"exitCode", result}};
  trace(path, clock, "before_final_marker");
  const bool finalWritten = shown && result == 0 && marker(path, record, &finalDiagnostic);
  trace(path, clock, "after_final_marker", finalDiagnostic);
  if (!finalWritten) return 4;
  return result;
}

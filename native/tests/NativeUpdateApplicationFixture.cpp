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
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include "NativeStartupHealth.h"

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
}
int main(int argc, char **argv) {
  const QString path = qEnvironmentVariable("GSW_UPDATE_FIXTURE_MARKER");
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) return 3;
  QElapsedTimer clock; clock.start();
  trace(path, clock, "main_enter");
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
      trace(path, clock, "before_health_hook");
      gsw::reportNativeStartupReady(quintptr(window.winId()), "qt-fixture-window-surface");
      trace(path, clock, "after_health_hook");
      QTimer::singleShot(150, &app, [&] {
        trace(path, clock, "quit_timer");
        app.quit();
      });
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

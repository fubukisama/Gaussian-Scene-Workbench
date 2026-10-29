// SPDX-License-Identifier: GPL-3.0-or-later
// Remote-only executable fixture. This is NOT the GSW application and its
// marker must never be described as formal GSW startup/renderer acceptance.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
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
bool marker(const QString &path, const QJsonObject &record) {
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  const QString path = qEnvironmentVariable("GSW_UPDATE_FIXTURE_MARKER");
  if (path.isEmpty() || !QDir::isAbsolutePath(path)) return 3;
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
  window.show();
  QTimer::singleShot(100, &app, [&] {
    shown = window.isVisible() && window.windowHandle();
    record.insert(QString::fromLatin1("windowShown"), shown);
    record.insert(QString::fromLatin1("viewportPainted"), surface->painted());
    record.insert(QString::fromLatin1("healthScope"), QString::fromLatin1("qt-fixture-window-surface"));
    record.insert(QString::fromLatin1("phase"), QString::fromLatin1("shown"));
    if (!shown || !surface->painted() || !marker(path, record)) app.exit(4);
    else QTimer::singleShot(150, &app, &QCoreApplication::quit);
  });
  const int result = app.exec();
  record.insert(QString::fromLatin1("phase"), QString::fromLatin1("normal_exit"));
  record.insert(QString::fromLatin1("exitCode"), result);
  if (!shown || result != 0 || !marker(path, record)) return 4;
  return result;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only external protocol fixture. No trust decisions, network download,
// payload staging or current.json writes happen in this executable.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments().mid(1);
  if (args.size() != 2 || !QStringList{"--check", "--prepare", "--apply-prepared"}.contains(args[0]) ||
      !QStringList{"stable", "preview"}.contains(args[1])) return 3;
  QDir root(QCoreApplication::applicationDirPath());
  if (root.dirName() != QLatin1String("bin") || !root.cdUp()) return 3;
  const auto operation = args[0], channel = args[1];
  const auto emitEvent = [&](const QString &phase, const QString &code = QString(), QJsonObject fields = {}) {
    QJsonObject log{{"schema", "gsw.update_ui_helper_fixture/1"}, {"testOnly", true},
        {"pid", double(QCoreApplication::applicationPid())}, {"operation", operation},
        {"channel", channel}, {"phase", phase}};
    QFile journal(root.filePath(QString::fromLatin1("ui-fixture-events.jsonl")));
    if (journal.open(QIODevice::WriteOnly | QIODevice::Append)) {
      journal.write(QJsonDocument(log).toJson(QJsonDocument::Compact)); journal.write("\n"); journal.flush();
    }
    if (phase == QLatin1String("started") || phase == QLatin1String("exited")) return;
    fields.insert(QString::fromLatin1("schema"), QString::fromLatin1("gsw.update_event/1"));
    fields.insert(QString::fromLatin1("phase"), phase);
    if (!code.isEmpty()) fields.insert(QString::fromLatin1("code"), code);
    auto bytes = QJsonDocument(fields).toJson(QJsonDocument::Compact); bytes += '\n';
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); std::fflush(stdout);
  };
  emitEvent(QString::fromLatin1("started"));
  const auto cancelled = std::make_shared<std::atomic_bool>(false);
  std::thread([cancelled] {
    char line[32]{};
    while (std::fgets(line, int(sizeof(line)), stdin))
      if (QByteArray(line).trimmed() == "cancel") cancelled->store(true);
  }).detach();
  const auto finish = [&](int status) { emitEvent(QString::fromLatin1("exited")); app.exit(status); };
  const auto available = [&] {
    const auto build = channel == QLatin1String("preview") ? QString::fromLatin1("ui-fixture-preview-b") : QString::fromLatin1("ui-fixture-b");
    const QJsonObject notes{{"zh_CN", QString::fromUtf8("中文 <b>原样</b> & <script>fixture</script>")},
        {"en_US", QString::fromLatin1("English <b>literal</b> & <script>fixture</script>")},
        {"ja_JP", QString::fromUtf8("日本語 <b>そのまま</b> & <script>fixture</script>")}};
    emitEvent(QString::fromLatin1("available"), QString::fromLatin1("VERIFIED"),
        {{"offer", QJsonObject{{"buildId", build}, {"displayVersion", "UI fixture <b>2 & preview</b>"},
            {"channel", channel}, {"archiveLength", 16 * 1024 * 1024}, {"releaseNotes", notes}}}});
  };
  QTimer timer;
  timer.setInterval(20);
  bool began = false;
  QObject::connect(&timer, &QTimer::timeout, &app, [&] {
    if (cancelled->load()) {
      emitEvent(QString::fromLatin1("failed"), QString::fromLatin1("download_cancelled")); finish(2); return;
    }
    if (!began) {
      began = true;
      const auto fault = root.filePath(QString::fromLatin1("ui-fixture-fail-next"));
      if (QFile::exists(fault)) {
        QFile::remove(fault);
        emitEvent(QString::fromLatin1("failed"), QString::fromLatin1("FIXTURE_TRANSIENT")); finish(2); return;
      }
      emitEvent(QString::fromLatin1("checking"));
      if (operation == QLatin1String("--apply-prepared")) {
        emitEvent(QString::fromLatin1("staging"));
        QFile::remove(root.filePath(QString::fromLatin1("updates/prepared.json")));
        emitEvent(QString::fromLatin1("activated"), QString::fromLatin1("NEXT_LAUNCH")); finish(0); return;
      }
      available();
      if (operation == QLatin1String("--check")) { finish(0); return; }
      emitEvent(QString::fromLatin1("downloading"), {}, {{"receivedBytes", 4 * 1024 * 1024}, {"totalBytes", 16 * 1024 * 1024}});
    }
    if (!QFile::exists(root.filePath(QString::fromLatin1("ui-fixture-allow-complete")))) return;
    emitEvent(QString::fromLatin1("verifying"));
    if (!QDir().mkpath(root.filePath(QString::fromLatin1("updates")))) { finish(3); return; }
    QSaveFile prepared(root.filePath(QString::fromLatin1("updates/prepared.json")));
    if (!prepared.open(QIODevice::WriteOnly) || prepared.write("{\"testOnly\":true}\n") <= 0 || !prepared.commit()) { finish(3); return; }
    emitEvent(QString::fromLatin1("prepared"), QString::fromLatin1("READY_FOR_NEXT_LAUNCH")); finish(0);
  });
  timer.start();
  QTimer::singleShot(60000, &app, [&] { finish(4); });
  return app.exec();
}

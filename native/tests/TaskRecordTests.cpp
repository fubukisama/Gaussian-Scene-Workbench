#include "TaskRecord.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QtTest>

#include <filesystem>

using gsw::TaskRecord;

class TaskRecordTests final : public QObject {
  Q_OBJECT
private slots:
  void lifecycleIsSessionOnlyAndStable();
  void retainsExactRawTextAndBoundedUnicodeTail();
  void incrementalSignalsReconstructRetainedLog();
  void exportsRawUtf8Atomically();
  void remapsOnlyManagedOutputDirectories();
  void externalDirectoryLinkStaysExternalAfterOldRootMoves();
  void logViewPreservesSelectionAndScrollWhenNotFollowing();
  void logViewFollowingAndDocumentTrimming();
};

void TaskRecordTests::lifecycleIsSessionOnlyAndStable() {
  TaskRecord record(QStringLiteral("模型_日本語"), {});
  QSignalSpy changed(&record, &TaskRecord::changed);
  QCOMPARE(record.name(), QStringLiteral("模型_日本語"));
  QCOMPARE(record.state(), QStringLiteral("starting"));
  QVERIFY(record.startedAt().isValid());
  QVERIFY(!record.finishedAt().isValid());
  record.setState(QStringLiteral("running"));
  QCOMPARE(changed.count(), 1);
  record.setState(QStringLiteral("running"));
  QCOMPARE(changed.count(), 1);
  record.setName(QStringLiteral("3DGS | object.ply"));
  record.finish(QStringLiteral("paused"));
  QVERIFY(record.finishedAt().isValid());
  const QDateTime finished = record.finishedAt();
  record.finish(QStringLiteral("paused"));
  QCOMPARE(record.finishedAt(), finished);
  QCOMPARE(record.state(), QStringLiteral("paused"));
  QVERIFY(record.diagnosticSummary().contains(record.name()));
  QVERIFY(record.diagnosticSummary().contains(record.startedAt().toString(Qt::ISODateWithMs)));
  QVERIFY(record.diagnosticSummary().contains(finished.toString(Qt::ISODateWithMs)));
  record.appendLog(QStringLiteral("raw-output-not-part-of-summary\n"));
  QVERIFY(!record.diagnosticSummary().contains(QStringLiteral("raw-output-not-part-of-summary")));
}

void TaskRecordTests::retainsExactRawTextAndBoundedUnicodeTail() {
  TaskRecord raw;
  const QString text = QString::fromUtf8("中文 日本語 😀\r\nraw\rline\n");
  raw.appendLog(text);
  QCOMPARE(raw.logText(), text);
  QVERIFY(!raw.logTruncated());
  const QString emoji = QString::fromUtf8("😀");
  TaskRecord oneChunk;
  oneChunk.appendLog(QStringLiteral("A") + emoji + QString(TaskRecord::MaximumLogCharacters - 1, QLatin1Char('x')));
  QCOMPARE(oneChunk.logText(), QString(TaskRecord::MaximumLogCharacters - 1, QLatin1Char('x')));
  QVERIFY(oneChunk.logTruncated());
  TaskRecord incremental;
  incremental.appendLog(emoji + QString(TaskRecord::MaximumLogCharacters - 2, QLatin1Char('x')));
  QCOMPARE(incremental.logText().size(), TaskRecord::MaximumLogCharacters);
  incremental.appendLog(QStringLiteral("A"));
  QCOMPARE(incremental.logText(), QString(TaskRecord::MaximumLogCharacters - 2, QLatin1Char('x')) + QStringLiteral("A"));
  QVERIFY(!incremental.logText().front().isLowSurrogate());
  TaskRecord splitPair;
  splitPair.appendLog(QString(QChar(0xd83d)));
  splitPair.appendLog(QString(QChar(0xde00)) + QString(TaskRecord::MaximumLogCharacters - 1, QLatin1Char('y')));
  QCOMPARE(splitPair.logText(), QString(TaskRecord::MaximumLogCharacters - 1, QLatin1Char('y')));
  QVERIFY(splitPair.logTruncated());
}

void TaskRecordTests::incrementalSignalsReconstructRetainedLog() {
  TaskRecord record;
  QString consumer;
  int notifications = 0;
  connect(&record, &TaskRecord::logChanged, &record,
      [&](const QString &appended, const qsizetype removed) {
        QVERIFY(removed >= 0 && removed <= consumer.size());
        consumer.remove(0, removed);
        consumer += appended;
        ++notifications;
      });
  for (const auto &chunk : {QStringLiteral("first\r\n"),
          QString(TaskRecord::MaximumLogCharacters - 4, QLatin1Char('a')),
          QString::fromUtf8("😀 tail\n"),
          QString(TaskRecord::MaximumLogCharacters * 2, QLatin1Char('b')),
          QStringLiteral("last\n")}) {
    record.appendLog(chunk);
    QCOMPARE(consumer, record.logText());
    QVERIFY(consumer.size() <= TaskRecord::MaximumLogCharacters);
  }
  QCOMPARE(notifications, 5);
  record.appendLog({});
  QCOMPARE(notifications, 5);
}

void TaskRecordTests::exportsRawUtf8Atomically() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  TaskRecord record;
  const QString raw = QString::fromUtf8("中文 日本語 😀\r\nraw\rline\n");
  record.appendLog(raw);
  const QString path = QDir(temporary.path()).filePath(QStringLiteral("任务-日本語.txt"));
  QString error;
  QVERIFY2(record.exportLog(path, &error), qPrintable(error));
  QFile exported(path);
  QVERIFY(exported.open(QIODevice::ReadOnly));
  QCOMPARE(exported.readAll(), raw.toUtf8());
  exported.close();
  QVERIFY(!record.exportLog(QDir(temporary.path()).filePath(QStringLiteral("missing-parent/log.txt")), &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(QFileInfo::exists(path));
  QVERIFY(!record.exportLog(temporary.path(), &error));
  QVERIFY(QFileInfo(temporary.path()).isDir());
  QVERIFY(exported.open(QIODevice::ReadOnly));
  QCOMPARE(exported.readAll(), raw.toUtf8());
}

void TaskRecordTests::remapsOnlyManagedOutputDirectories() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  const QString oldRoot = root.filePath(QStringLiteral("old"));
  const QString newRoot = root.filePath(QStringLiteral("new"));
  const QString output = QDir(oldRoot).filePath(QStringLiteral("output/meshes/run"));
  QVERIFY(QDir().mkpath(output));
  TaskRecord managed(QStringLiteral("mesh"), output);
  QVERIFY(root.rename(QStringLiteral("old"), QStringLiteral("old-moved")));
  QVERIFY(!QFileInfo::exists(oldRoot));
  QVERIFY(managed.remapManagedOutputDirectory(oldRoot, newRoot));
  QCOMPARE(managed.outputDirectory(), QDir(newRoot).filePath(QStringLiteral("output/meshes/run")));
  TaskRecord external(QStringLiteral("3dgs"), root.filePath(QStringLiteral("old-external/result")));
  const QString externalBefore = external.outputDirectory();
  QVERIFY(!external.remapManagedOutputDirectory(oldRoot, newRoot));
  QCOMPARE(external.outputDirectory(), externalBefore);
  TaskRecord noDirectory;
  QVERIFY(!noDirectory.remapManagedOutputDirectory(oldRoot, newRoot));
  TaskRecord rootOutput({}, oldRoot);
  QVERIFY(rootOutput.remapManagedOutputDirectory(oldRoot, newRoot));
  QCOMPARE(rootOutput.outputDirectory(), newRoot);
}

void TaskRecordTests::externalDirectoryLinkStaysExternalAfterOldRootMoves() {
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  QDir root(temporary.path());
  const QString oldRoot = root.filePath(QStringLiteral("old"));
  const QString external = root.filePath(QStringLiteral("external/result"));
  const QString link = QDir(oldRoot).filePath(QStringLiteral("linked"));
  QVERIFY(QDir().mkpath(oldRoot));
  QVERIFY(QDir().mkpath(external));
  std::error_code error;
  std::filesystem::create_directory_symlink(std::filesystem::path(external.toStdWString()),
      std::filesystem::path(link.toStdWString()), error);
  if (error) QSKIP("Directory symbolic links are unavailable to this test account");
  TaskRecord record(QStringLiteral("mesh"), link);
  QVERIFY(root.rename(QStringLiteral("old"), QStringLiteral("old-moved")));
  QVERIFY(!QFileInfo::exists(oldRoot));
  QVERIFY(!record.remapManagedOutputDirectory(oldRoot, root.filePath(QStringLiteral("new"))));
  QCOMPARE(record.outputDirectory(), QDir::cleanPath(link));
}

void TaskRecordTests::logViewPreservesSelectionAndScrollWhenNotFollowing() {
  QPlainTextEdit view;
  view.setReadOnly(true);
  view.setLineWrapMode(QPlainTextEdit::NoWrap);
  QStringList lines;
  for (int index = 0; index < 100; ++index)
    lines.append(QStringLiteral("%1 ").arg(index) + QString(150, QLatin1Char('x')));
  view.setPlainText(lines.join(QLatin1Char('\n')));
  view.resize(360, 160);
  view.show();
  QVERIFY(QTest::qWaitForWindowExposed(&view));
  QTextCursor cursor(view.document());
  cursor.setPosition(5);
  cursor.setPosition(20, QTextCursor::KeepAnchor);
  view.setTextCursor(cursor);
  view.verticalScrollBar()->setValue(10);
  view.horizontalScrollBar()->setValue(30);
  const int vertical = view.verticalScrollBar()->value();
  const int horizontal = view.horizontalScrollBar()->value();
  const QString selected = view.textCursor().selectedText();
  TaskRecord::appendToLogView(&view, QStringLiteral("\nlatest"), false);
  QCOMPARE(view.textCursor().position(), 20);
  QCOMPARE(view.textCursor().anchor(), 5);
  QCOMPARE(view.textCursor().selectedText(), selected);
  QCOMPARE(view.verticalScrollBar()->value(), vertical);
  QCOMPARE(view.horizontalScrollBar()->value(), horizontal);
  QVERIFY(view.toPlainText().endsWith(QStringLiteral("\nlatest")));
}

void TaskRecordTests::logViewFollowingAndDocumentTrimming() {
  QPlainTextEdit view;
  view.setPlainText(QStringLiteral("first\nsecond"));
  TaskRecord::appendToLogView(&view, QString::fromUtf8("\n😀 last"), true, 6);
  QCOMPARE(view.toPlainText(), QString::fromUtf8("second\n😀 last"));
  QCOMPARE(view.textCursor().position(), view.document()->characterCount() - 1);
  TaskRecord::appendToLogView(&view, {}, false, 999999);
  QCOMPARE(view.toPlainText(), QString());
  view.setPlainText(QString::fromUtf8("😀 kept"));
  TaskRecord::appendToLogView(&view, {}, false, 2);
  QCOMPARE(view.toPlainText(), QStringLiteral(" kept"));
  TaskRecord::appendToLogView(&view, {}, false, 999999);
  const QString raw = QString::fromUtf8("first\r\n中文 😀\r\nlast");
  // The helper consumes document characters. Raw CRLF retention cuts must be
  // converted by the dialog to this normalized representation first.
  QString normalized = raw;
  normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  TaskRecord::appendToLogView(&view, normalized, false);
  QTextDocument expected;
  expected.setPlainText(raw);
  QCOMPARE(view.toPlainText(), expected.toPlainText());
}

QTEST_MAIN(TaskRecordTests)
#include "TaskRecordTests.moc"

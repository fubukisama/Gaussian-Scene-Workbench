#include "TaskActionsSmokeTest.h"
#include "AppLanguage.h"
#include "MainWindow.h"
#include "TaskDetailsDialog.h"
#include "TaskRecord.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>

#include <functional>
#include <memory>

namespace gsw {

// Only the operating-system file opener is replaced. All task actions and
// dialogs below are the production widgets used by the desktop application.
class TaskFileUrlReceiver final : public QObject {
  Q_OBJECT
public:
  QList<QUrl> urls;
public slots:
  void receive(const QUrl &url) { urls.append(url); }
};

bool runTaskActionsSmokeTest(MainWindow &window) {
  bool passed = true;
  const auto check = [&](const bool ok, const char *message) {
    if (!ok) { passed = false; qCritical() << "Task actions FAIL:" << message; }
    return ok;
  };
  const auto wait = [](const std::function<bool()> &predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
      QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
    }
    return predicate();
  };
  const auto write = [](const QString &path, const QByteArray &text) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
  };
  const auto read = [](const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
  };
  auto *table = window.findChild<QTableWidget *>(QStringLiteral("taskRecords"));
  auto *console = window.findChild<QPlainTextEdit *>(QStringLiteral("taskConsole"));
  auto *details = window.findChild<QAction *>(QStringLiteral("taskDetailsAction"));
  auto *openFolder = window.findChild<QAction *>(QStringLiteral("openTaskFolderAction"));
  auto *copy = window.findChild<QAction *>(QStringLiteral("copyTaskSummaryAction"));
  auto *follow = window.findChild<QAction *>(QStringLiteral("followLatestLogsAction"));
  auto *exportLogs = window.findChild<QAction *>(QStringLiteral("exportCurrentLogsAction"));
  if (!check(table && console && details && openFolder && copy && follow && exportLogs,
             "production task actions and log widgets exist") ||
      !check(!window.mProcessSupervisor.isRunning(), "test begins without a user task")) return false;

  const QString originalLanguage = AppLanguage::current();
  const QFont originalFont = qApp->font();
  QSettings settings;
  const bool hadFollowPreference = settings.contains(QStringLiteral("ui/followLogs"));
  const QVariant originalFollowPreference = settings.value(QStringLiteral("ui/followLogs"));
  const bool originalFollow = follow->isChecked();
  auto originalClipboard = std::make_unique<QMimeData>();
  if (const auto *mime = QApplication::clipboard()->mimeData())
    for (const auto &format : mime->formats()) originalClipboard->setData(format, mime->data(format));
  QTemporaryDir temporary;
  TaskFileUrlReceiver fileReceiver;
  QDesktopServices::setUrlHandler(QStringLiteral("file"), &fileReceiver, "receive");
  const auto restore = qScopeGuard([&] {
    window.mPendingDatasetImport.reset();
    window.mPendingReconstruction.reset();
    window.mPendingTraining.reset();
    window.mPendingMesh.reset();
    if (window.mProcessSupervisor.isRunning()) {
      window.mProcessSupervisor.stop();
      wait([&] { return !window.mProcessSupervisor.isRunning(); });
    }
    QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    qApp->setFont(originalFont);
    AppLanguage::apply(originalLanguage, false);
    follow->setChecked(originalFollow);
    if (hadFollowPreference) settings.setValue(QStringLiteral("ui/followLogs"), originalFollowPreference);
    else settings.remove(QStringLiteral("ui/followLogs"));
    QApplication::clipboard()->setMimeData(originalClipboard.release());
  });

  if (!check(temporary.isValid(), "owned task fixture directory")) return false;
  const QDir root(temporary.path());
  const QString helper = qEnvironmentVariable("GSW_PROCESS_OUTPUT_FIXTURE",
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("gsw_process_output_fixture.exe")));
  if (!check(QFileInfo(helper).isFile(), "real supervised worker fixture is available")) return false;

  QString savePath;
  QString expectedMessage;
  int savedDialogs = 0;
  int messageDialogs = 0;
  QTimer modalAutomation;
  QObject::connect(&modalAutomation, &QTimer::timeout, &window, [&] {
    if (auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
      if (savePath.isEmpty()) { check(false, "unexpected file dialog"); file->reject(); return; }
      file->setDirectory(QFileInfo(savePath).absolutePath());
      file->selectFile(QFileInfo(savePath).fileName());
      ++savedDialogs;
      QMetaObject::invokeMethod(file, "accept", Qt::DirectConnection);
    } else if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      ++messageDialogs;
      check(!expectedMessage.isEmpty() && box->text() == expectedMessage,
            "missing-folder error is translated and contains the exact owned path");
      box->accept();
    }
  });
  modalAutomation.start(10);
  const auto selectRow = [&](const int row) {
    window.mTaskTabs->setCurrentWidget(window.mTaskPage);
    table->selectionModel()->setCurrentIndex(table->model()->index(row, 0),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  };

  // A failed OS launch must leave a selectable record even though taskStarted
  // is never sent. Its log and diagnostics remain usable after later tasks.
  const int failedRow = table->rowCount();
  const QString failedName = QString::fromUtf8("failed-launch 日本語 中文");
  check(window.mProcessSupervisor.start(failedName, root.filePath(QStringLiteral("missing-worker.exe")),
      {}, root.path()), "accept missing executable attempt");
  if (!check(wait([&] { return !window.mProcessSupervisor.isRunning() && table->rowCount() == failedRow + 1; }),
      "failed launch completes with a visible row")) return false;
  check(table->item(failedRow, 0)->text() == QCoreApplication::translate("Workbench", "失败"),
        "failed row displays the localized failure state");
  const QString failureResult = QCoreApplication::translate("Workbench", "退出码 %1").arg(-1);
  check(table->item(failedRow, 3)->text() == failureResult, "failed-launch result preserves exit code");
  selectRow(failedRow);
  check(details->isEnabled() && openFolder->isEnabled() && copy->isEnabled(), "failed record exposes actions");
  copy->trigger();
  check(QApplication::clipboard()->text().contains(failedName) &&
        QApplication::clipboard()->text().contains(QDir::toNativeSeparators(root.path())) &&
        QApplication::clipboard()->text().contains(failureResult), "copied summary includes name, folder and failure outcome");
  openFolder->trigger();
  check(fileReceiver.urls.size() == 1 && fileReceiver.urls.constLast() == QUrl::fromLocalFile(root.path()),
        "folder action sends the exact task directory to the OS boundary");

  // Each pipeline takes its real pending context through the accepted-launch
  // signal. Reset only the arranged context before the helper can finish: this
  // test verifies task presentation, not reconstruction/training result gates.
  const QStringList kinds{QStringLiteral("media"), QStringLiteral("colmap"), QStringLiteral("3dgs"),
      QStringLiteral("2dgs"), QStringLiteral("bounded_tsdf"), QStringLiteral("unbounded_tsdf"),
      QStringLiteral("sugar"), QStringLiteral("gs2mesh"), QStringLiteral("openmvs")};
  QStringList rawMarkers;
  for (int index = 0; index < kinds.size(); ++index) {
    const QString kind = kinds.at(index);
    const QString name = kind + QString::fromUtf8(" task 日本語 中文");
    const QString directory = root.filePath(QStringLiteral("output-") + kind);
    const QString working = root.filePath(QStringLiteral("worker-") + kind);
    const QString ready = root.filePath(kind + QStringLiteral("-ready"));
    const QString release = root.filePath(kind + QStringLiteral("-release"));
    if (!check(QDir().mkpath(directory) && QDir().mkpath(working), "create distinct owned task/output folders")) return false;
    if (kind == QStringLiteral("media"))
      window.mPendingDatasetImport = MainWindow::PendingDatasetImport{name, directory, root.path(), {}, {}, {}, working, {}};
    else if (kind == QStringLiteral("colmap"))
      window.mPendingReconstruction = MainWindow::PendingReconstruction{name, root.path(), directory};
    else if (kind == QStringLiteral("3dgs") || kind == QStringLiteral("2dgs"))
      window.mPendingTraining = MainWindow::PendingTraining{name, root.path(), working, directory, kind, 3};
    else window.mPendingMesh = MainWindow::PendingMesh{name, root.path(), directory, {}};
    const int row = table->rowCount();
    check(window.mProcessSupervisor.start(name, helper, {ready, release}, working), "launch actual held task worker");
    window.mPendingDatasetImport.reset(); window.mPendingReconstruction.reset();
    window.mPendingTraining.reset(); window.mPendingMesh.reset();
    if (!check(wait([&] { return QFileInfo::exists(ready); }), "real worker is alive before UI assertions")) return false;
    check(table->rowCount() == row + 1 && table->item(row, 1)->text() == name, "one new record per accepted task");
    selectRow(row); copy->trigger();
    check(QApplication::clipboard()->text().contains(QDir::toNativeSeparators(directory)) &&
          !QApplication::clipboard()->text().contains(QDir::toNativeSeparators(working)),
          "pipeline record uses output/dataset directory, not worker working directory");
    openFolder->trigger();
    check(fileReceiver.urls.constLast() == QUrl::fromLocalFile(directory), "every pipeline opens its own output folder");
    const QString marker = QStringLiteral("RAW_") + kind + QString::fromUtf8(" 日本語 中文");
    rawMarkers.append(marker);
    window.mProcessSupervisor.outputReady(marker + QLatin1Char('\n'));
    if (index == 0) {
      window.mTaskTabs->setCurrentWidget(window.mLogPage);
      follow->setChecked(false);
      QString lines;
      for (int line = 0; line < 120; ++line) lines += QString::fromUtf8("stable line %1\n").arg(line);
      window.mProcessSupervisor.outputReady(lines);
      QTextCursor cursor(console->document()); cursor.setPosition(5); cursor.setPosition(21, QTextCursor::KeepAnchor);
      console->setTextCursor(cursor); console->verticalScrollBar()->setValue(0);
      const QString selectedText = console->textCursor().selectedText();
      const int scroll = console->verticalScrollBar()->value();
      const QString retainedBefore = console->toPlainText();
      const int rowCount = table->rowCount();
      for (const auto &language : AppLanguage::supported()) {
        check(AppLanguage::apply(language, false), "apply every supported language while a task is running");
        check(window.mProcessSupervisor.isRunning() && window.mProcessSupervisor.activeTask() == name &&
              table->rowCount() == rowCount && table->item(row, 1)->text() == name,
              "language switch retains active worker and task identity");
        check(console->toPlainText() == retainedBefore && console->textCursor().selectedText() == selectedText &&
              console->verticalScrollBar()->value() == scroll, "language switch retains raw logs, selection and scroll");
        check(details->text() == QCoreApplication::translate("Workbench", "任务详情") &&
              follow->text() == QCoreApplication::translate("Workbench", "跟随最新日志"), "task actions translate immediately");
      }
      AppLanguage::apply(originalLanguage, false);
      window.mProcessSupervisor.outputReady(QString::fromUtf8("additional live log\n"));
      check(console->textCursor().selectedText() == selectedText && console->verticalScrollBar()->value() == scroll,
            "new output does not steal selection or scroll when follow is off");
      follow->setChecked(true);
      window.mProcessSupervisor.outputReady(QString::fromUtf8("followed tail\n"));
      check(console->verticalScrollBar()->value() == console->verticalScrollBar()->maximum(), "follow on reaches the latest log");
    }
    check(write(release, QByteArrayLiteral("release")), "release owned worker before its deadline");
    if (!check(wait([&] { return !window.mProcessSupervisor.isRunning(); }), "owned worker exits cleanly")) return false;
    check(table->item(row, 0)->text() == QCoreApplication::translate("Workbench", "完成"), "completed task retains a usable record");
  }

  // Exercise the real details action and modal, not a separately constructed
  // substitute. All locale changes happen with the same dialog and record.
  selectRow(failedRow);
  int inspectedDetails = 0;
  QTimer detailsAutomation;
  QObject::connect(&detailsAutomation, &QTimer::timeout, &window, [&] {
    auto *dialog = qobject_cast<TaskDetailsDialog *>(QApplication::activeModalWidget());
    if (!dialog || inspectedDetails) return;
    ++inspectedDetails;
    auto *name = dialog->findChild<QLabel *>(QStringLiteral("taskDetailsName"));
    auto *state = dialog->findChild<QLabel *>(QStringLiteral("taskDetailsState"));
    auto *logs = dialog->findChild<QPlainTextEdit *>(QStringLiteral("taskDetailsLogs"));
    auto *copyButton = dialog->findChild<QAction *>(QStringLiteral("taskDetailsCopySummaryAction"));
    auto *folderButton = dialog->findChild<QAction *>(QStringLiteral("taskDetailsOpenOutputAction"));
    auto *exportButton = dialog->findChild<QAction *>(QStringLiteral("taskDetailsExportLogAction"));
    auto *actionBar = dialog->findChild<QToolBar *>(QStringLiteral("taskDetailsActions"));
    auto *closeButton = dialog->findChild<QPushButton *>(QStringLiteral("taskDetailsClose"));
    if (!check(name && state && logs && copyButton && folderButton && exportButton && closeButton && actionBar,
               "real modal exposes task data and log actions")) { dialog->reject(); return; }
    const QString retained = logs->toPlainText();
    check(!retained.isEmpty() && retained.contains(failedName), "failed launch has its own retained diagnostic log");
    for (const auto &marker : rawMarkers) check(!retained.contains(marker), "later tasks do not contaminate failed task logs");
    for (const auto &language : AppLanguage::supported()) {
      AppLanguage::apply(language, false);
      check(dialog->windowTitle() == QCoreApplication::translate("Workbench", "任务详情") &&
            state->text() == QCoreApplication::translate("Workbench", "失败") &&
            copyButton->text() == QCoreApplication::translate("Workbench", "复制任务摘要"), "details text translates live");
      check(name->text() == failedName && logs->toPlainText() == retained, "details language changes preserve user task name and raw logs");
    }
    copyButton->trigger();
    check(QApplication::clipboard()->text().contains(failedName), "details copy button exports the selected task summary");
    folderButton->trigger();
    check(fileReceiver.urls.constLast() == QUrl::fromLocalFile(root.path()), "details folder button preserves exact output");
    savePath = root.filePath(QStringLiteral("retained-log.txt"));
    exportButton->trigger();
    check(read(savePath) == retained.toUtf8(), "details retained-log export matches displayed raw log byte for byte");
    savePath.clear();
    AppLanguage::apply(QStringLiteral("ja_JP"), false);
    QFont large = originalFont; large.setPointSizeF(qMax(8.0, originalFont.pointSizeF()) * 1.5);
    qApp->setFont(large);
    dialog->resize(360, 560); dialog->layout()->activate();
    check(dialog->width() <= 360, "compact Japanese details can use the requested 360-pixel width");
    auto *overflow = actionBar->findChild<QToolButton *>(QStringLiteral("qt_toolbar_ext_button"));
    for (auto *action : {copyButton, folderButton, exportButton}) {
      auto *button = actionBar->widgetForAction(action);
      const QRect bounds(button->mapTo(dialog, QPoint()), button->size());
      check((button->isVisible() && dialog->rect().contains(bounds)) ||
            (overflow && overflow->isVisible() && action->isEnabled()),
            "Japanese 150-percent-font compact actions are visible or reachable through toolbar overflow");
    }
    auto *copyWidget = actionBar->widgetForAction(copyButton);
    auto *folderWidget = actionBar->widgetForAction(folderButton);
    check(!copyWidget->isVisible() || !folderWidget->isVisible() ||
          !QRect(copyWidget->mapTo(dialog, QPoint()), copyWidget->size()).intersects(
              QRect(folderWidget->mapTo(dialog, QPoint()), folderWidget->size())), "compact details actions do not overlap");
    check(closeButton->isVisible() && dialog->rect().contains(
        QRect(closeButton->mapTo(dialog, QPoint()), closeButton->size())), "compact details close button remains usable");
    qApp->setFont(originalFont); AppLanguage::apply(originalLanguage, false);
    closeButton->click();
  });
  detailsAutomation.start(10);
  details->trigger();
  detailsAutomation.stop();
  check(inspectedDetails == 1 && table->item(failedRow, 1)->text() == failedName, "opening and closing details does not lose the record");

  // The folder may disappear after the task completes. Recheck when executing
  // the action and report an actionable, localized error without opening it.
  const int missingRow = failedRow + 1;
  const QString missingDirectory = root.filePath(QStringLiteral("output-media"));
  selectRow(missingRow);
  check(QDir().rmdir(missingDirectory), "remove only the owned empty output fixture folder");
  const int beforeOpen = fileReceiver.urls.size();
  expectedMessage = QCoreApplication::translate("Workbench", "任务文件夹不存在：%1")
      .arg(QDir::toNativeSeparators(missingDirectory));
  openFolder->trigger();
  check(messageDialogs == 1 && fileReceiver.urls.size() == beforeOpen, "missing folder reports error without an OS open request");
  expectedMessage.clear();

  savePath = root.filePath(QStringLiteral("session-log.txt"));
  const QByteArray sessionLog = console->toPlainText().toUtf8();
  exportLogs->trigger();
  check(read(savePath) == sessionLog && savedDialogs >= 2, "current-log action writes the retained session text exactly");
  savePath.clear();

  // CRLF may span a retention cut or a worker chunk. Observe only the actual
  // detail view: its normalized display must not gain an extra blank line.
  auto record = QSharedPointer<TaskRecord>::create(QString::fromUtf8("CRLF fixture"), root.path());
  record->appendLog(QStringLiteral("first\r"));
  TaskDetailsDialog crlf(record, &window);
  auto *crlfLogs = crlf.findChild<QPlainTextEdit *>(QStringLiteral("taskDetailsLogs"));
  auto *crlfFollow = crlf.findChild<QCheckBox *>(QStringLiteral("taskDetailsFollow"));
  check(crlfLogs && crlfFollow, "CRLF fixture uses the real details view");
  record->appendLog(QStringLiteral("\nsecond\r\n"));
  check(crlfLogs && crlfLogs->toPlainText() == QStringLiteral("first\nsecond\n"), "split CRLF chunks display once and retain original text");
  check(record->logText() == QStringLiteral("first\r\nsecond\r\n"), "display normalization does not alter retained raw bytes");
  const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
  const QString boundedBody(static_cast<int>(TaskRecord::MaximumLogCharacters - 4), QLatin1Char('x'));
  auto cutRecord = QSharedPointer<TaskRecord>::create(QString::fromUtf8("CRLF retention cut"), root.path());
  cutRecord->appendLog(QStringLiteral("\r\n") + boundedBody + emoji);
  TaskDetailsDialog cutDialog(cutRecord, &window);
  auto *cutLogs = cutDialog.findChild<QPlainTextEdit *>(QStringLiteral("taskDetailsLogs"));
  cutRecord->appendLog(QStringLiteral("Z"));
  check(cutRecord->logTruncated() && cutRecord->logText().size() == TaskRecord::MaximumLogCharacters &&
        cutRecord->logText() == QLatin1Char('\n') + boundedBody + emoji + QLatin1Char('Z'),
        "retention cut preserves the CRLF tail and Unicode raw data within the bound");
  check(cutLogs && cutLogs->toPlainText() == QLatin1Char('\n') + boundedBody + emoji + QLatin1Char('Z'),
        "CRLF retention cut leaves the actual detail display consistent with retained bytes");
  auto unicodeRecord = QSharedPointer<TaskRecord>::create(QString::fromUtf8("Unicode retention cut"), root.path());
  const QString unicodeBody(static_cast<int>(TaskRecord::MaximumLogCharacters - 2), QLatin1Char('y'));
  unicodeRecord->appendLog(emoji + unicodeBody);
  TaskDetailsDialog unicodeDialog(unicodeRecord, &window);
  auto *unicodeLogs = unicodeDialog.findChild<QPlainTextEdit *>(QStringLiteral("taskDetailsLogs"));
  unicodeRecord->appendLog(QStringLiteral("Z"));
  check(unicodeRecord->logTruncated() && unicodeRecord->logText() == unicodeBody + QLatin1Char('Z') &&
        !unicodeRecord->logText().at(0).isLowSurrogate(), "retention never leaves an orphan Unicode surrogate");
  check(unicodeLogs && unicodeLogs->toPlainText() == unicodeBody + QLatin1Char('Z'),
        "actual detail display handles an emoji crossing the retention boundary");
  qInfo() << "Task actions:" << (passed ? "PASS" : "FAIL") << "9 pipeline contexts, three live locales";
  return passed;
}

} // namespace gsw

#include "TaskActionsSmokeTest.moc"

#include "FileDialogHistorySmokeTest.h"
#include "AppLanguage.h"
#include "AppTheme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontInfo>
#include <QMainWindow>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QScreen>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <algorithm>

namespace gsw {
namespace {
void settle(int milliseconds = 100) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

bool waitFor(const std::function<bool()> &condition, int milliseconds = 2500) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!condition() && elapsed.elapsed() < milliseconds) settle(30);
  return condition();
}

struct VisibleEntry {
  QAbstractItemView *view = nullptr;
  QModelIndex index;
};

VisibleEntry leftEntry(QFileDialog &dialog, const QString &literalName) {
  // Observe what the user can actually see and click. No history settings,
  // private controller properties, or QFileDialog internal URL roles.
  for (auto *view : dialog.findChildren<QAbstractItemView *>()) {
    if (!view->isVisible() || !view->model()) continue;
    const QPoint origin = view->mapTo(&dialog, QPoint());
    if (origin.x() + view->width() > dialog.width() / 2) continue;
    std::function<QModelIndex(const QModelIndex &)> find = [&](const QModelIndex &parent) {
      for (int row = 0; row < view->model()->rowCount(parent); ++row) {
        const QModelIndex index = view->model()->index(row, 0, parent);
        if (index.data(Qt::DisplayRole).toString() == literalName &&
            !view->visualRect(index).isEmpty()) return index;
        if (const QModelIndex child = find(index); child.isValid()) return child;
      }
      return QModelIndex();
    };
    const QModelIndex index = find(view->rootIndex());
    if (index.isValid()) return {view, index};
  }
  return {};
}

bool clickEntry(const VisibleEntry &entry) {
  if (!entry.view || !entry.index.isValid()) return false;
  entry.view->scrollTo(entry.index);
  settle();
  const QRect rect = entry.view->visualRect(entry.index);
  if (rect.isEmpty() || !entry.view->viewport()->rect().contains(rect.center())) return false;
  QWidget *viewport = entry.view->viewport();
  const QPoint local = rect.center();
  const QPoint global = viewport->mapToGlobal(local);
  QMouseEvent press(QEvent::MouseButtonPress, local, global,
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(viewport, &press);
  QApplication::sendEvent(viewport, &release);
  settle();
  return true;
}
} // namespace

bool runFileDialogHistorySmokeTest(QMainWindow &workbench) {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      qWarning() << "File dialog history smoke:" << message;
      passed = false;
    }
  };
  const QString fixtureRoot = QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("test-file-dialog-history"));
  check(QDir().mkpath(fixtureRoot), "create isolated fixture root on the application drive");
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("session-XXXXXX")));
  check(temporary.isValid(), "create isolated existing-file fixture");
  if (!temporary.isValid()) return false;
  const QString folderName = QStringLiteral("GSW History Folder 日本語");
  const QString fileName = QStringLiteral("sample-history.ply");
  const QString folder = QDir(temporary.path()).filePath(folderName);
  const QString path = QDir(folder).filePath(fileName);
  check(QDir().mkpath(folder), "create the independently known history folder");
  QFile fixture(path);
  check(fixture.open(QIODevice::WriteOnly), "create the independently known history file");
  if (!fixture.isOpen()) return false;
  fixture.write("ply\nformat ascii 1.0\nelement vertex 0\nend_header\n");
  fixture.close();

  QFileDialog first(&workbench);
  first.setOption(QFileDialog::DontUseNativeDialog);
  first.setAcceptMode(QFileDialog::AcceptOpen);
  first.setFileMode(QFileDialog::ExistingFile);
  first.setNameFilter(QStringLiteral("PLY (*.ply)"));
  first.setDirectory(folder);
  first.selectFile(fileName);
  QTimer::singleShot(150, &first, [&] {
    auto *buttons = first.findChild<QDialogButtonBox *>();
    auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
    check(open && open->isVisible() && open->isEnabled(),
          "real existing-file dialog exposes an enabled Open button");
    if (open && open->isEnabled()) open->click();
    else first.reject();
  });
  const int result = first.exec();
  check(result == QDialog::Accepted && first.selectedFiles() == QStringList{path},
        "opening the real fixture is accepted through the actual dialog button");
  if (result != QDialog::Accepted) return false;

  QFileDialog second(&workbench);
  second.setOption(QFileDialog::DontUseNativeDialog);
  second.setAcceptMode(QFileDialog::AcceptOpen);
  second.setFileMode(QFileDialog::ExistingFile);
  second.setNameFilter(QStringLiteral("PLY (*.ply)"));
  second.setDirectory(temporary.path());
  second.show();
  second.activateWindow();
  settle();
  const bool folderShown = waitFor([&] { return leftEntry(second, folderName).index.isValid(); });
  const bool fileShown = waitFor([&] { return leftEntry(second, fileName).index.isValid(); });
  check(folderShown, "accepted file's folder appears as a visible left-column history entry");
  check(fileShown, "accepted file appears as a visible left-column history entry");
  if (folderShown) {
    check(clickEntry(leftEntry(second, folderName)), "folder history entry is actually clickable");
    check(second.directory().absolutePath() == folder && second.isVisible(),
          "left-column folder click navigates without accepting or closing the dialog");
  }
  if (fileShown) {
    second.setDirectory(temporary.path());
    settle();
    check(clickEntry(leftEntry(second, fileName)), "file history entry is actually clickable");
    qInfo() << "History file click public state:" << second.directory().absolutePath()
            << second.selectedFiles() << second.isVisible();
    check(second.directory().absolutePath() == folder && second.selectedFiles() == QStringList{path} &&
          second.isVisible(),
          "left-column file click navigates and selects the actual file without accepting");
  }
  const QString capture = qEnvironmentVariable("GSW_FILE_HISTORY_CAPTURE_DIR");
  if (!capture.isEmpty()) {
    check(QDir().mkpath(capture) && second.grab().save(QDir(capture).filePath(QStringLiteral("history-left-column.png"))),
          "save a screenshot of the real history dialog");
  }
  second.reject();
  qInfo() << "File dialog history smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runFileDialogHistoryPersistenceSmokeTest(QMainWindow &workbench, bool restartedChild) {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      qWarning() << "File dialog history persistence smoke:" << message;
      passed = false;
    }
  };
  // The expected folder and file are independent literal fixtures. The child
  // sees only the real dialog, not the parent's memory or history storage.
  const QString folderName = QStringLiteral("GSW Persistent Folder 日本語");
  const QString fileName = QStringLiteral("remembered-after-restart.ply");
  if (restartedChild) {
    const QString temporary = qEnvironmentVariable("GSW_HISTORY_RESTART_FIXTURE");
    const QString folder = QDir(temporary).filePath(folderName);
    const QString path = QDir(folder).filePath(fileName);
    check(QFileInfo(folder).isDir() && QFileInfo(path).isFile(),
          "restarted application's independently known fixtures still exist");
    if (!passed) return false;
    QFileDialog dialog(&workbench);
    dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(QFileDialog::AcceptOpen);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilter(QStringLiteral("PLY (*.ply)"));
    dialog.setDirectory(temporary);
    dialog.show();
    dialog.activateWindow();
    settle();
    const bool folderShown = waitFor([&] { return leftEntry(dialog, folderName).index.isValid(); });
    const bool fileShown = waitFor([&] { return leftEntry(dialog, fileName).index.isValid(); });
    check(folderShown, "fresh application process retains the accepted folder in the visible left column");
    check(fileShown, "fresh application process retains the accepted file in the visible left column");
    if (folderShown) {
      check(clickEntry(leftEntry(dialog, folderName)), "restarted application's folder history entry is clickable");
      check(dialog.directory().absolutePath() == folder && dialog.isVisible(),
            "restarted application's folder entry navigates without accepting");
    }
    if (fileShown) {
      dialog.setDirectory(temporary);
      settle();
      check(clickEntry(leftEntry(dialog, fileName)), "restarted application's file history entry is clickable");
      check(dialog.directory().absolutePath() == folder && dialog.selectedFiles() == QStringList{path} && dialog.isVisible(),
            "restarted application's file entry selects the correct Unicode-path file without accepting");
    }
    const QString capture = qEnvironmentVariable("GSW_FILE_HISTORY_CAPTURE_DIR");
    if (!capture.isEmpty())
      check(QDir().mkpath(capture) && dialog.grab().save(QDir(capture).filePath(QStringLiteral("history-persistence-child.png"))),
            "save the restarted application's real history dialog");
    dialog.reject();
    qInfo() << "File dialog history persistence child:" << (passed ? "PASS" : "FAIL");
    return passed;
  }

  const QString fixtureRoot = QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("test-file-dialog-history"));
  check(QDir().mkpath(fixtureRoot), "create isolated cross-process fixture root on the application drive");
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("restart-XXXXXX")));
  check(temporary.isValid(), "create an existing-file fixture lasting until the restarted application exits");
  if (!temporary.isValid()) return false;
  const QString folder = QDir(temporary.path()).filePath(folderName);
  const QString path = QDir(folder).filePath(fileName);
  check(QDir().mkpath(folder), "create the independently known persistent folder");
  QFile fixture(path);
  check(fixture.open(QIODevice::WriteOnly), "create the independently known persistent file");
  if (!fixture.isOpen()) return false;
  fixture.write("ply\nformat ascii 1.0\nelement vertex 0\nend_header\n");
  fixture.close();
  QFileDialog accepted(&workbench);
  accepted.setOption(QFileDialog::DontUseNativeDialog);
  accepted.setAcceptMode(QFileDialog::AcceptOpen);
  accepted.setFileMode(QFileDialog::ExistingFile);
  accepted.setNameFilter(QStringLiteral("PLY (*.ply)"));
  accepted.setDirectory(folder);
  accepted.selectFile(fileName);
  QTimer::singleShot(150, &accepted, [&] {
    auto *buttons = accepted.findChild<QDialogButtonBox *>();
    auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
    check(open && open->isVisible() && open->isEnabled(), "persistent fixture opens through the actual Open button");
    if (open && open->isEnabled()) open->click();
    else accepted.reject();
  });
  const int result = accepted.exec();
  check(result == QDialog::Accepted && accepted.selectedFiles() == QStringList{path},
        "the first application process accepts the independently known fixture");
  if (!passed) return false;

  // Flush the application settings backend as an application exit would. The
  // test neither reads keys nor asserts a persistence representation; only a
  // separate process's visible file dialog is the acceptance oracle.
  QSettings().sync();
  const QString sharedSettings = qApp->property("gswSmokeSettingsDirectory").toString();
  check(!sharedSettings.isEmpty() && QFileInfo(sharedSettings).isDir(),
        "cross-process QA uses an existing isolated settings root, never user settings");
  if (!passed) return false;
  QProcess child;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("GSW_HISTORY_RESTART_SETTINGS"), sharedSettings);
  environment.insert(QStringLiteral("GSW_HISTORY_RESTART_FIXTURE"), temporary.path());
  child.setProcessEnvironment(environment);
  child.setProcessChannelMode(QProcess::MergedChannels);
  child.start(QCoreApplication::applicationFilePath(),
              {QStringLiteral("--smoke-test-file-dialog-history-persistence-child"),
               QStringLiteral("--language"), QStringLiteral("en_US"),
               QStringLiteral("--theme"), QStringLiteral("dark")});
  check(child.waitForStarted(3000), "start a fresh application process using the same executable");
  QElapsedTimer deadline;
  deadline.start();
  while (child.state() != QProcess::NotRunning && deadline.elapsed() < 20000) settle(30);
  const bool finished = child.state() == QProcess::NotRunning;
  if (!finished) {
    // QProcess owns this child; never discover or stop unrelated user apps.
    child.kill();
    child.waitForFinished(2000);
  }
  qInfo().noquote() << QString::fromUtf8(child.readAllStandardOutput());
  check(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        "a fresh application process retains navigable file and folder history across the process boundary");
  qInfo() << "File dialog history persistence smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runFileDialogHistoryRankingSmokeTest(QMainWindow &workbench) {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      qWarning() << "File dialog history ranking smoke:" << message;
      passed = false;
    }
  };
  const QString fixtureRoot = QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("test-file-dialog-history"));
  check(QDir().mkpath(fixtureRoot), "create isolated ranking fixture root on the application drive");
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("ranking-XXXXXX")));
  check(temporary.isValid(), "create isolated frequency-ranking fixtures");
  if (!temporary.isValid()) return false;
  const QStringList folderNames{QStringLiteral("Rank Folder A"), QStringLiteral("Rank Folder B"), QStringLiteral("Rank Folder C")};
  const QStringList fileNames{QStringLiteral("rank-A.ply"), QStringLiteral("rank-B.ply"), QStringLiteral("rank-C.ply")};
  QStringList folders;
  QStringList paths;
  for (int i = 0; i < 3; ++i) {
    folders.append(QDir(temporary.path()).filePath(folderNames.at(i)));
    paths.append(QDir(folders.last()).filePath(fileNames.at(i)));
    check(QDir().mkpath(folders.last()), "create an independently named ranking folder");
    QFile fixture(paths.last());
    check(fixture.open(QIODevice::WriteOnly), "create an independently named ranking file");
    if (!fixture.isOpen()) return false;
    fixture.write("ply\nformat ascii 1.0\nelement vertex 0\nend_header\n");
  }
  // A is opened twice. B and C are opened once, with B used last. The worked
  // example's literal visible order is A, B, C: usage first, recency for ties.
  // A pure MRU list instead produces B, A, C and must fail this public test.
  for (const int fixtureIndex : {0, 2, 0, 1}) {
    QFileDialog accepted(&workbench);
    accepted.setOption(QFileDialog::DontUseNativeDialog);
    accepted.setAcceptMode(QFileDialog::AcceptOpen);
    accepted.setFileMode(QFileDialog::ExistingFile);
    accepted.setNameFilter(QStringLiteral("PLY (*.ply)"));
    accepted.setDirectory(folders.at(fixtureIndex));
    accepted.selectFile(fileNames.at(fixtureIndex));
    QTimer::singleShot(150, &accepted, [&] {
      auto *buttons = accepted.findChild<QDialogButtonBox *>();
      auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
      check(open && open->isVisible() && open->isEnabled(), "ranking example uses the actual Open button");
      if (open && open->isEnabled()) open->click();
      else accepted.reject();
    });
    const int result = accepted.exec();
    check(result == QDialog::Accepted && accepted.selectedFiles() == QStringList{paths.at(fixtureIndex)},
          "frequency example accepts exactly the independently known file");
    if (!passed) return false;
  }
  QFileDialog observed(&workbench);
  observed.setOption(QFileDialog::DontUseNativeDialog);
  observed.setAcceptMode(QFileDialog::AcceptOpen);
  observed.setFileMode(QFileDialog::ExistingFile);
  observed.setNameFilter(QStringLiteral("PLY (*.ply)"));
  observed.setDirectory(temporary.path());
  observed.show();
  observed.activateWindow();
  settle();
  const auto checkOrder = [&](const QStringList &literalNames, const char *message) {
    const bool shown = waitFor([&] {
      for (const QString &name : literalNames)
        if (!leftEntry(observed, name).index.isValid()) return false;
      return true;
    });
    check(shown, "all independently named ranking entries appear in the visible left column");
    if (!shown) return;
    QList<int> visibleY;
    for (const QString &name : literalNames) {
      const auto entry = leftEntry(observed, name);
      const QRect rect = entry.view->visualRect(entry.index);
      visibleY.append(entry.view->viewport()->mapTo(&observed, rect.center()).y());
    }
    qInfo() << "History ranking visible positions:" << literalNames << visibleY;
    check(visibleY.at(0) < visibleY.at(1) && visibleY.at(1) < visibleY.at(2), message);
  };
  checkOrder(fileNames, "frequent file A precedes newer file B, while equally frequent newer B precedes C");
  checkOrder(folderNames, "frequent folder A precedes newer folder B, while equally frequent newer B precedes C");
  const QString capture = qEnvironmentVariable("GSW_FILE_HISTORY_CAPTURE_DIR");
  if (!capture.isEmpty())
    check(QDir().mkpath(capture) && observed.grab().save(QDir(capture).filePath(QStringLiteral("history-frequency-ranking.png"))),
          "save the real frequency-ranked left column");
  observed.reject();
  qInfo() << "File dialog history ranking smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runFileDialogHistoryFilterSmokeTest(QMainWindow &workbench) {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      qWarning() << "File dialog history filter smoke:" << message;
      passed = false;
    }
  };
  const QString fixtureRoot = QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("test-file-dialog-history"));
  check(QDir().mkpath(fixtureRoot), "create isolated filter and safety fixture root on the application drive");
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("filter-XXXXXX")));
  check(temporary.isValid(), "create isolated filter and safety fixtures");
  if (!temporary.isValid()) return false;
  const QStringList folderNames{QStringLiteral("PLY Input Folder"), QStringLiteral("GLB Input Folder"),
                                QStringLiteral("Stale Input Folder"), QStringLiteral("Race Input Folder"),
                                QStringLiteral("Cancelled Input Folder")};
  const QStringList fileNames{QStringLiteral("filtered-input.ply"), QStringLiteral("filtered-input.glb"),
                              QStringLiteral("stale-input.ply"), QStringLiteral("race-input.ply"),
                              QStringLiteral("cancelled-input.ply")};
  QStringList folders;
  QStringList paths;
  for (int i = 0; i < folderNames.size(); ++i) {
    folders.append(QDir(temporary.path()).filePath(folderNames.at(i)));
    paths.append(QDir(folders.last()).filePath(fileNames.at(i)));
    check(QDir().mkpath(folders.last()), "create an independently named filter/safety folder");
    QFile fixture(paths.last());
    check(fixture.open(QIODevice::WriteOnly), "create an independently named filter/safety file");
    if (!fixture.isOpen()) return false;
    fixture.write("real file-window fixture\n");
  }
  const QString plyFilter = QStringLiteral("PLY (*.ply)");
  const QString glbFilter = QStringLiteral("GLB (*.glb)");
  for (int i = 0; i < 4; ++i) {
    QFileDialog accepted(&workbench);
    accepted.setOption(QFileDialog::DontUseNativeDialog);
    accepted.setAcceptMode(QFileDialog::AcceptOpen);
    accepted.setFileMode(QFileDialog::ExistingFile);
    accepted.setNameFilter(QStringLiteral("Models (*.ply *.glb)"));
    accepted.setDirectory(folders.at(i));
    accepted.selectFile(fileNames.at(i));
    QTimer::singleShot(150, &accepted, [&] {
      auto *buttons = accepted.findChild<QDialogButtonBox *>();
      auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
      check(open && open->isVisible() && open->isEnabled(), "filter fixtures are accepted through the actual Open button");
      if (open && open->isEnabled()) open->click();
      else accepted.reject();
    });
    check(accepted.exec() == QDialog::Accepted && accepted.selectedFiles() == QStringList{paths.at(i)},
          "accept exactly the known filter/safety file");
    if (!passed) return false;
  }

  QFileDialog cancelled(&workbench);
  cancelled.setOption(QFileDialog::DontUseNativeDialog);
  cancelled.setAcceptMode(QFileDialog::AcceptOpen);
  cancelled.setFileMode(QFileDialog::ExistingFile);
  cancelled.setNameFilter(plyFilter);
  cancelled.setDirectory(folders.at(4));
  cancelled.selectFile(fileNames.at(4));
  QTimer::singleShot(150, &cancelled, [&] {
    auto *buttons = cancelled.findChild<QDialogButtonBox *>();
    auto *cancel = buttons ? buttons->button(QDialogButtonBox::Cancel) : nullptr;
    check(cancel && cancel->isVisible(), "browsing/filename safety example uses the actual Cancel button");
    if (cancel) cancel->click();
    else cancelled.reject();
  });
  check(cancelled.exec() == QDialog::Rejected, "cancel an existing filename after browsing without opening it");
  // These paths were created above in this owned temporary directory. The
  // original model inputs and all user paths remain untouched.
  check(QFile::remove(paths.at(2)) && QDir().rmdir(folders.at(2)), "remove only the owned stale fixture and its empty folder");

  QFileDialog observed(&workbench);
  observed.setOption(QFileDialog::DontUseNativeDialog);
  observed.setAcceptMode(QFileDialog::AcceptOpen);
  observed.setFileMode(QFileDialog::ExistingFile);
  observed.setNameFilters({plyFilter, glbFilter});
  observed.selectNameFilter(plyFilter);
  observed.setDirectory(temporary.path());
  observed.show();
  observed.activateWindow();
  settle();
  check(waitFor([&] { return leftEntry(observed, fileNames.at(0)).index.isValid(); }),
        "PLY-compatible accepted file is visible in the real left history column");
  check(!leftEntry(observed, fileNames.at(1)).index.isValid(),
        "initial PLY name filter excludes accepted GLB files from the visible history column");
  check(!leftEntry(observed, folderNames.at(4)).index.isValid() && !leftEntry(observed, fileNames.at(4)).index.isValid(),
        "cancelled browsing and filename do not create folder or file history entries");
  check(!leftEntry(observed, folderNames.at(2)).index.isValid() && !leftEntry(observed, fileNames.at(2)).index.isValid(),
        "deleted file and folder history entries are hidden without failing the dialog");

  auto *type = observed.findChild<QComboBox *>(QStringLiteral("fileTypeCombo"));
  check(type && type->findText(glbFilter) >= 0, "actual visible file-type combo offers the GLB filter");
  if (type && type->findText(glbFilter) >= 0) {
    type->showPopup();
    settle();
    auto *view = type->view();
    const QModelIndex glb = view->model()->index(type->findText(glbFilter), type->modelColumn(), type->rootModelIndex());
    check(clickEntry({view, glb}), "change the actual file-type popup by clicking its GLB row");
    check(observed.selectedNameFilter() == glbFilter, "file dialog receives the user-selected GLB filter");
    check(waitFor([&] { return leftEntry(observed, fileNames.at(1)).index.isValid(); }),
          "user-selected GLB filter immediately reveals the compatible remembered file");
    check(!leftEntry(observed, fileNames.at(0)).index.isValid(),
          "user-selected GLB filter hides incompatible remembered PLY files");
    observed.selectNameFilter(plyFilter);
    check(waitFor([&] { return leftEntry(observed, fileNames.at(0)).index.isValid(); }),
          "programmatic public name-filter selection also refreshes visible compatible history");
    check(!leftEntry(observed, fileNames.at(1)).index.isValid(),
          "programmatic PLY filter selection hides incompatible remembered GLB files");
  }

  // Race: a visible remembered file disappears before the click is handled.
  // Capture the actual hit point before removing the owned fixture, then click
  // that point without a queued refresh between deletion and input dispatch.
  auto *name = observed.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
  check(name != nullptr, "real filename editor is available for stale-click safety");
  if (name) {
    name->setText(QStringLiteral("keep-current-name.ply"));
    const QString beforeDirectory = observed.directory().absolutePath();
    const QString beforeName = name->text();
    bool acceptedByClick = false;
    QObject::connect(&observed, &QDialog::accepted, &observed, [&] { acceptedByClick = true; });
    const auto race = leftEntry(observed, fileNames.at(3));
    check(race.view && race.index.isValid(), "owned race fixture has a visible history entry before deletion");
    if (race.view && race.index.isValid()) {
      race.view->scrollTo(race.index);
      settle();
      auto *viewport = race.view->viewport();
      const QPoint local = race.view->visualRect(race.index).center();
      const QPoint global = viewport->mapToGlobal(local);
      check(QFile::remove(paths.at(3)), "remove only the owned race file immediately before clicking");
      QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
      QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(viewport, &press);
      QApplication::sendEvent(viewport, &release);
      settle();
      check(observed.directory().absolutePath() == beforeDirectory && name->text() == beforeName &&
            observed.isVisible() && !acceptedByClick,
            "clicking a file removed after display preserves directory and filename and never accepts the dialog");
    }
  }
  const QString capture = qEnvironmentVariable("GSW_FILE_HISTORY_CAPTURE_DIR");
  if (!capture.isEmpty())
    check(QDir().mkpath(capture) && observed.grab().save(QDir(capture).filePath(QStringLiteral("history-filter-and-safety.png"))),
          "save the real filtered history and safety state");
  observed.reject();

  for (const bool save : {false, true}) {
    QFileDialog restricted(&workbench);
    restricted.setOption(QFileDialog::DontUseNativeDialog);
    restricted.setAcceptMode(save ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    restricted.setFileMode(save ? QFileDialog::AnyFile : QFileDialog::Directory);
    restricted.setNameFilter(plyFilter);
    restricted.setDirectory(temporary.path());
    restricted.show();
    restricted.activateWindow();
    settle();
    check(waitFor([&] { return leftEntry(restricted, folderNames.at(0)).index.isValid(); }),
          "directory/save dialog retains navigable remembered folders");
    check(!leftEntry(restricted, fileNames.at(0)).index.isValid() && !leftEntry(restricted, fileNames.at(1)).index.isValid(),
          "directory and save dialogs do not display remembered input files");
    restricted.reject();
  }
  qInfo() << "File dialog history filter smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}

bool runFileDialogHistoryUiSmokeTest(QMainWindow &workbench) {
  bool passed = true;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) {
      qWarning() << "File dialog history UI smoke:" << message;
      passed = false;
    }
  };
  struct Labels {
    QString locale;
    QString folders;
    QString files;
    QString desktop;
    QString clear;
  };
  // Human-reviewed literal UI oracles, not translations read back from the
  // application under test or labels copied from its history model.
  const QList<Labels> languages{
      {QStringLiteral("zh_CN"), QStringLiteral("常用文件夹"), QStringLiteral("常用文件"),
       QStringLiteral("桌面"), QStringLiteral("清除历史")},
      {QStringLiteral("en_US"), QStringLiteral("Frequent Folders"), QStringLiteral("Frequent Files"),
       QStringLiteral("Desktop"), QStringLiteral("Clear History")},
      {QStringLiteral("ja_JP"), QStringLiteral("よく使うフォルダー"), QStringLiteral("よく使うファイル"),
       QStringLiteral("デスクトップ"), QStringLiteral("履歴を消去")}};
  const QString initialLanguage = AppLanguage::current();
  const QString fixtureRoot = QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("test-file-dialog-history"));
  check(QDir().mkpath(fixtureRoot), "create isolated history-UI fixture root on the application drive");
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("ui-XXXXXX")));
  check(temporary.isValid(), "create isolated Unicode history-UI fixture");
  if (!temporary.isValid()) return false;
  const QString folderName = QStringLiteral("常用モデル_研究");
  const QString fileName = QStringLiteral("記憶モデル_测试.ply");
  const QString folder = QDir(temporary.path()).filePath(folderName);
  const QString path = QDir(folder).filePath(fileName);
  const QByteArray sourceBytes("ply\nformat ascii 1.0\nelement vertex 0\nend_header\n");
  check(QDir().mkpath(folder), "create the independently named Unicode folder");
  QFile fixture(path);
  check(fixture.open(QIODevice::WriteOnly), "create the independently named Unicode file");
  if (!fixture.isOpen()) return false;
  fixture.write(sourceBytes);
  fixture.close();

  QFileDialog accepted(&workbench);
  accepted.setOption(QFileDialog::DontUseNativeDialog);
  accepted.setAcceptMode(QFileDialog::AcceptOpen);
  accepted.setFileMode(QFileDialog::ExistingFile);
  accepted.setNameFilter(QStringLiteral("PLY (*.ply)"));
  accepted.setDirectory(folder);
  accepted.selectFile(fileName);
  QTimer::singleShot(150, &accepted, [&] {
    auto *buttons = accepted.findChild<QDialogButtonBox *>();
    auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
    check(open && open->isVisible() && open->isEnabled(), "Unicode history fixture uses the real Open button");
    if (open && open->isEnabled()) open->click();
    else accepted.reject();
  });
  check(accepted.exec() == QDialog::Accepted && accepted.selectedFiles() == QStringList{path},
        "accept the exact Unicode-path fixture through the real file window");
  if (!passed) return false;

  QFileDialog observed(&workbench);
  observed.setOption(QFileDialog::DontUseNativeDialog);
  observed.setAcceptMode(QFileDialog::AcceptOpen);
  observed.setFileMode(QFileDialog::ExistingFile);
  observed.setNameFilters({QStringLiteral("PLY (*.ply)"), QStringLiteral("All files (*)")});
  observed.selectNameFilter(QStringLiteral("PLY (*.ply)"));
  observed.setDirectory(folder);
  observed.selectFile(fileName);
  observed.show();
  observed.activateWindow();
  settle();
  const QRect available = observed.screen()->availableGeometry();
  observed.resize(std::min(900, available.width() - 64), std::min(640, available.height() - 64));
  settle();
  const QSize userSize = observed.size();
  auto *name = observed.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
  check(name != nullptr, "real Unicode dialog filename editor exists");
  if (!name) return false;
  const QString desktopPath = QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).absolutePath();
  const QString capture = qEnvironmentVariable("GSW_FILE_HISTORY_CAPTURE_DIR");
  const auto clearButton = [&](const QString &literal) -> QPushButton * {
    for (auto *button : observed.findChildren<QPushButton *>())
      if (button->isVisible() && button->text() == literal) return button;
    return nullptr;
  };
  for (const auto &labels : languages) {
    int normalPlacesHeight = 0;
    int normalFontPixels = 0;
    for (const int scale : {100, 150}) {
      const QString beforeDirectory = observed.directory().absolutePath();
      const QString beforeName = name->text();
      const QString beforeFilter = observed.selectedNameFilter();
      const QStringList beforeSelection = observed.selectedFiles();
      check(AppLanguage::apply(labels.locale, false), "switch the real file-window language immediately");
      AppTheme::apply(*qApp, scale, false);
      settle(150);
      check(observed.directory().absolutePath() == beforeDirectory && name->text() == beforeName &&
            observed.selectedNameFilter() == beforeFilter && observed.selectedFiles() == beforeSelection,
            "live language and scale retain directory, Unicode filename, filter, and file selection");
      check(observed.size() == userSize, "live language and scale retain the user's resized file-window dimensions");
      const bool folderGroup = waitFor([&] { return leftEntry(observed, labels.folders).index.isValid(); });
      const bool fileGroup = waitFor([&] { return leftEntry(observed, labels.files).index.isValid(); });
      check(folderGroup && fileGroup, "folder and file history group captions match independent locale literals");
      if (folderGroup && fileGroup) {
        for (const QString &caption : {labels.folders, labels.files}) {
          const auto group = leftEntry(observed, caption);
          const QVariant role = group.index.data(Qt::FontRole);
          const QFont renderedFont = role.isValid()
              ? role.value<QFont>().resolve(group.view->font()) : group.view->font();
          const QFontInfo rendered(renderedFont);
          const QFontInfo inherited(group.view->font());
          qInfo() << "History UI font:" << labels.locale << scale << caption
                  << rendered.family() << rendered.pixelSize() << inherited.family() << inherited.pixelSize();
          check(rendered.family() == inherited.family() && rendered.pixelSize() == inherited.pixelSize() && renderedFont.bold(),
                "visible bold history group uses the current effective family and size rather than stale initial font metrics");
        }
      }
      const auto desktop = leftEntry(observed, labels.desktop);
      check(desktop.view && desktop.index.isValid(), "translated Desktop shortcut remains visible at the current font scale");
      if (desktop.view && desktop.index.isValid()) {
        const int pixels = QFontInfo(desktop.view->font()).pixelSize();
        const int rowCount = desktop.view->model()->rowCount(desktop.view->rootIndex());
        bool allPlacesFit = true;
        for (int row = 0; row < rowCount; ++row) {
          const QModelIndex index = desktop.view->model()->index(row, 0, desktop.view->rootIndex());
          const QRect rect = desktop.view->visualRect(index);
          const QRect viewport = desktop.view->viewport()->rect();
          // Qt's non-wrapping QListView may return a logical rectangle wider
          // than its viewport; its delegate elides the painted text. Verify
          // whole vertical rows and their visible hit area, not that internal
          // logical width. Actual place clicks below prove navigation works.
          const bool visibleRow = !rect.isEmpty() && rect.top() >= viewport.top() &&
                                  rect.bottom() <= viewport.bottom() && !rect.intersected(viewport).isEmpty();
          if (!visibleRow)
            qInfo() << "History UI places clipping:" << labels.locale << scale << row
                    << index.data(Qt::DisplayRole).toString() << rect << viewport
                    << desktop.view->sizeHintForRow(row) << desktop.view->height();
          allPlacesFit = allPlacesFit && visibleRow;
        }
        check(allPlacesFit, "all actual places rows fit visibly after live font/scale changes");
        check(!desktop.view->horizontalScrollBar()->isVisible(), "long place names do not add a horizontal scrollbar to the compact left column");
        if (scale == 100) {
          normalPlacesHeight = desktop.view->height();
          normalFontPixels = pixels;
        } else {
          check(pixels > normalFontPixels, "150 percent truly increases the actual places font size");
          check(desktop.view->height() > normalPlacesHeight,
                "places area grows with the larger actual row metrics instead of retaining its old fixed height");
        }
        const auto file = leftEntry(observed, fileName);
        check(clickEntry(file), "Unicode history file stays visible and clickable after a live language/scale change");
        check(observed.directory().absolutePath() == folder && observed.selectedFiles() == QStringList{path} && observed.isVisible(),
              "scaled history click selects the original Unicode file without accepting");
        const QString nameBeforeDesktop = name->text();
        const QString filterBeforeDesktop = observed.selectedNameFilter();
        check(clickEntry(leftEntry(observed, labels.desktop)), "Desktop place is actually clickable at the current live scale");
        check(observed.directory().absolutePath() == desktopPath && name->text() == nameBeforeDesktop &&
              observed.selectedNameFilter() == filterBeforeDesktop && observed.isVisible(),
              "Desktop click navigates while preserving the Unicode filename and selected filter");
        observed.setDirectory(folder);
        observed.selectFile(path);
        settle();
      }
      check(clearButton(labels.clear) != nullptr, "clear-history control is visible with the independent locale caption");
      if (!capture.isEmpty())
        check(QDir().mkpath(capture) && observed.grab().save(QDir(capture).filePath(
                  QStringLiteral("history-ui-%1-%2.png").arg(labels.locale).arg(scale))),
              "save the real multilingual history window at the current live scale");
    }
  }
  check(AppLanguage::apply(initialLanguage, false), "restore the starting language for its localized clear confirmation");
  settle();
  const auto originalLabels = std::find_if(languages.begin(), languages.end(), [&](const Labels &labels) {
    return labels.locale == initialLanguage;
  });
  QPushButton *clear = originalLabels == languages.end() ? nullptr : clearButton(originalLabels->clear);
  check(clear != nullptr, "starting locale has its actual clear-history button before confirmation");
  if (clear) {
    const QString beforeDirectory = observed.directory().absolutePath();
    const QString beforeName = name->text();
    const QString beforeFilter = observed.selectedNameFilter();
    const QStringList beforeSelection = observed.selectedFiles();
    const auto confirm = [&](QMessageBox::StandardButton answer) {
      bool seen = false;
      QElapsedTimer elapsed;
      elapsed.start();
      QTimer inspector;
      inspector.setInterval(20);
      QObject::connect(&inspector, &QTimer::timeout, &observed, [&] {
        auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (message) {
          seen = true;
          inspector.stop();
          auto *button = message->button(answer);
          check(button && button->isVisible() && button->isEnabled(), "real clear-history confirmation offers the requested No/Yes answer");
          if (button) button->click();
          else message->reject();
        } else if (elapsed.elapsed() > 2500) {
          inspector.stop();
          check(false, "clear-history operation presents its real confirmation dialog");
          if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
        }
      });
      inspector.start();
      clear->click();
      inspector.stop();
      check(seen, "clear-history operation was observed through its actual visible confirmation");
      settle();
    };
    confirm(QMessageBox::No);
    check(leftEntry(observed, folderName).index.isValid() && leftEntry(observed, fileName).index.isValid(),
          "declining clear-history preserves the remembered Unicode folder and file");
    confirm(QMessageBox::Yes);
    check(waitFor([&] { return !leftEntry(observed, folderName).index.isValid() && !leftEntry(observed, fileName).index.isValid(); }),
          "confirming clear-history removes both remembered entries from the same real window");
    check(observed.directory().absolutePath() == beforeDirectory && name->text() == beforeName &&
          observed.selectedNameFilter() == beforeFilter && observed.selectedFiles() == beforeSelection && observed.isVisible(),
          "clearing history leaves the current dialog directory, filename, filter, selection and open state unchanged");
    QFile original(path);
    check(original.open(QIODevice::ReadOnly) && original.readAll() == sourceBytes,
          "clearing history does not delete or modify the original source file");
    QFileDialog fresh(&workbench);
    fresh.setOption(QFileDialog::DontUseNativeDialog);
    fresh.setAcceptMode(QFileDialog::AcceptOpen);
    fresh.setFileMode(QFileDialog::ExistingFile);
    fresh.setNameFilter(QStringLiteral("PLY (*.ply)"));
    fresh.setDirectory(temporary.path());
    fresh.show();
    fresh.activateWindow();
    settle();
    check(!leftEntry(fresh, folderName).index.isValid() && !leftEntry(fresh, fileName).index.isValid(),
          "a newly opened real file window does not restore cleared history in the same application");
    fresh.reject();
  }
  observed.reject();
  AppTheme::apply(*qApp, 100, false);
  AppLanguage::apply(initialLanguage, false);
  qInfo() << "File dialog history UI smoke:" << initialLanguage << (passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw

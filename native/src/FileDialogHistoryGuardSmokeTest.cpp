#include "FileDialogHistoryGuardSmokeTest.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>

namespace gsw {
namespace {
using Check = std::function<void(bool, const char *)>;

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
  // Read only the visible left-column model and hit geometry, not history
  // storage, controller state, item identity roles, or QFileDialog URL roles.
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

QSet<QString> pathSet(const QStringList &paths) {
  return QSet<QString>(paths.cbegin(), paths.cend());
}

bool createFixture(const QString &path, const Check &check) {
  QFile file(path);
  check(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
        "create only a new owned file-window fixture");
  if (!file.isOpen()) return false;
  const QByteArray content("ply\nformat ascii 1.0\nelement vertex 0\nend_header\n");
  const bool written = file.write(content) == content.size();
  file.close();
  check(written, "write the complete independently known fixture contents");
  return written;
}

bool acceptFiles(QMainWindow &workbench, const QString &folder,
                 const QStringList &literalNames, const Check &check) {
  QStringList expectedPaths;
  for (const QString &name : literalNames) expectedPaths.append(QDir(folder).filePath(name));

  QFileDialog dialog(&workbench);
  dialog.setOption(QFileDialog::DontUseNativeDialog);
  dialog.setAcceptMode(QFileDialog::AcceptOpen);
  dialog.setFileMode(literalNames.size() > 1 ? QFileDialog::ExistingFiles : QFileDialog::ExistingFile);
  dialog.setNameFilter(QStringLiteral("PLY (*.ply)"));
  dialog.setDirectory(folder);
  if (literalNames.size() == 1) dialog.selectFile(literalNames.first());

  // Modal dialogs are bounded even if a fixture or button unexpectedly fails.
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &dialog, &QDialog::reject);
  timeout.start(3000);
  QTimer::singleShot(150, &dialog, [&] {
    if (literalNames.size() > 1) {
      // ExistingFiles supports quoted names in its actual filename editor.
      // This does not inject history records or select a private filesystem model.
      auto *editor = dialog.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
      check(editor && editor->isVisible(), "real multi-file dialog exposes its filename editor");
      if (!editor) { dialog.reject(); return; }
      QStringList quoted;
      for (const QString &name : literalNames)
        quoted.append(QLatin1Char('"') + name + QLatin1Char('"'));
      editor->setText(quoted.join(QLatin1Char(' ')));
    }
    const QStringList selected = dialog.selectedFiles();
    const bool selectionCorrect = selected.size() == expectedPaths.size() &&
                                  pathSet(selected) == pathSet(expectedPaths);
    check(selectionCorrect, "actual dialog selection matches every independently named input exactly once");
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
    const bool canOpen = open && open->isVisible() && open->isEnabled();
    check(canOpen, "real file dialog exposes an enabled Open button");
    if (selectionCorrect && canOpen) open->click();
    else dialog.reject();
  });
  const int result = dialog.exec();
  timeout.stop();
  const QStringList selected = dialog.selectedFiles();
  const bool accepted = result == QDialog::Accepted && selected.size() == expectedPaths.size() &&
                        pathSet(selected) == pathSet(expectedPaths);
  check(accepted, "real Open accepts precisely the independently known file selection");
  return accepted;
}
} // namespace

bool runFileDialogHistoryGuardSmokeTest(QMainWindow &workbench) {
  bool passed = true;
  const Check check = [&](bool condition, const char *message) {
    if (!condition) {
      qWarning() << "File dialog history guard smoke:" << message;
      passed = false;
    }
  };
  // The application smoke entry point supplies an isolated settings directory.
  // Inspect only that isolation contract, never a history key or its encoding.
  const QString isolatedSettings = qApp->property("gswSmokeSettingsDirectory").toString();
  check(!isolatedSettings.isEmpty() && QFileInfo(isolatedSettings).isDir(),
        "history guard uses an isolated application settings root, never user settings");
  if (!passed) return false;

  const QString fixtureRoot = QDir(QCoreApplication::applicationDirPath())
      .filePath(QStringLiteral("test-file-dialog-history"));
  check(QDir().mkpath(fixtureRoot), "create isolated guard fixtures on the application drive");
  QTemporaryDir temporary(QDir(fixtureRoot).filePath(QStringLiteral("guard-XXXXXX")));
  check(temporary.isValid(), "create an owned temporary file-window fixture directory");
  if (!temporary.isValid()) return false;

  // These are two different existing filenames, not alternate spellings of
  // the same file. History must preserve both original targets independently.
  const QString unicodeFolder = QDir(temporary.path()).filePath(QStringLiteral("Distinct Unicode Inputs"));
  check(QDir().mkpath(unicodeFolder), "create the owned Unicode input folder");
  const QStringList unicodeNames{QStringLiteral("stra\u00dfe.ply"), QStringLiteral("strasse.ply")};
  for (const QString &name : unicodeNames) {
    if (!createFixture(QDir(unicodeFolder).filePath(name), check)) return false;
    if (!acceptFiles(workbench, unicodeFolder, {name}, check)) return false;
  }
  {
    QFileDialog observed(&workbench);
    observed.setOption(QFileDialog::DontUseNativeDialog);
    observed.setAcceptMode(QFileDialog::AcceptOpen);
    observed.setFileMode(QFileDialog::ExistingFile);
    observed.setNameFilter(QStringLiteral("PLY (*.ply)"));
    observed.setDirectory(temporary.path());
    observed.show();
    observed.activateWindow();
    settle();
    for (const QString &name : unicodeNames) {
      const bool shown = waitFor([&] { return leftEntry(observed, name).index.isValid(); });
      check(shown, "both distinct Unicode input filenames survive in the visible history column");
      if (!shown) continue;
      observed.setDirectory(temporary.path());
      settle();
      check(clickEntry(leftEntry(observed, name)), "each distinct Unicode history entry is actually clickable");
      check(observed.isVisible() && observed.directory().absolutePath() == unicodeFolder &&
            observed.selectedFiles() == QStringList{QDir(unicodeFolder).filePath(name)},
            "Unicode history click selects its original file, not another folded-name target");
    }
    observed.reject();
  }

  // Work through the public multi-file window twice to make 128 independently
  // owned inputs frequent. Do not manufacture count values or storage records.
  const QString capacityFolder = QDir(temporary.path()).filePath(QStringLiteral("Full History Inputs"));
  check(QDir().mkpath(capacityFolder), "create the owned capacity input folder");
  QStringList oldNames;
  QStringList oldPaths;
  for (int i = 0; i < 128; ++i) {
    oldNames.append(QStringLiteral("old-input-%1.ply").arg(i, 3, 10, QLatin1Char('0')));
    oldPaths.append(QDir(capacityFolder).filePath(oldNames.last()));
    if (!createFixture(oldPaths.last(), check)) return false;
  }
  if (!acceptFiles(workbench, capacityFolder, oldNames, check) ||
      !acceptFiles(workbench, capacityFolder, oldNames, check)) return false;

  const QString newName = QStringLiteral("newly-opened-after-capacity.ply");
  const QString newPath = QDir(capacityFolder).filePath(newName);
  if (!createFixture(newPath, check) || !acceptFiles(workbench, capacityFolder, {newName}, check)) return false;
  // Remove only the 128 files just created by this runner, never an imported
  // user model or a history-derived path. Keep their directory and the new file.
  for (const QString &path : oldPaths)
    check(QFile::remove(path), "remove only an owned old fixture after its accepted selections");
  check(QFileInfo(capacityFolder).isDir() && QFileInfo(newPath).isFile(),
        "capacity fixture folder and the newly accepted input remain intact");

  QFileDialog observed(&workbench);
  observed.setOption(QFileDialog::DontUseNativeDialog);
  observed.setAcceptMode(QFileDialog::AcceptOpen);
  observed.setFileMode(QFileDialog::ExistingFile);
  observed.setNameFilter(QStringLiteral("PLY (*.ply)"));
  observed.setDirectory(temporary.path());
  observed.show();
  observed.activateWindow();
  settle();
  const bool newShown = waitFor([&] { return leftEntry(observed, newName).index.isValid(); });
  check(newShown, "newly opened file remains reachable after a full frequently used history");
  if (newShown) {
    check(clickEntry(leftEntry(observed, newName)), "new file history entry is actually clickable after capacity");
    check(observed.isVisible() && observed.directory().absolutePath() == capacityFolder &&
          observed.selectedFiles() == QStringList{newPath},
          "new file history click selects its exact original path without accepting");
  }
  observed.reject();
  qInfo() << "File dialog history guard smoke:" << (passed ? "PASS" : "FAIL");
  return passed;
}
} // namespace gsw

#include "FileDialogHistory.h"
#include "AppLanguage.h"
#include "AppTheme.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace gsw {
namespace {
constexpr auto controllerName = "gswFileHistoryController";
constexpr int pathRole = Qt::UserRole;
constexpr int directoryRole = Qt::UserRole + 1;
constexpr int entryLimit = 128;
constexpr int byteLimit = 512 * 1024;
constexpr qint64 timestampLimit = 9000000000000000LL;

QString identity(const QString &path) {
  const QFileInfo info(path);
  QString key = info.canonicalFilePath();
  if (key.isEmpty()) key = info.absoluteFilePath();
  return QDir::cleanPath(key);
}

bool sameIdentity(const QString &a, const QString &b) {
#ifdef Q_OS_WIN
  return CompareStringOrdinal(reinterpret_cast<LPCWCH>(a.utf16()), a.size(),
                              reinterpret_cast<LPCWCH>(b.utf16()), b.size(), TRUE) == CSTR_EQUAL;
#else
  return a == b;
#endif
}

bool containsIdentity(const QStringList &values, const QString &key) {
  return std::any_of(values.cbegin(), values.cend(), [&](const QString &value) {
    return sameIdentity(value, key);
  });
}

struct Entry {
  QString path;
  QString key;
  int uses = 1;
  qint64 lastUsed = 0;
};

void rank(QList<Entry> &entries) {
  // Retention is recent-first; display remains frequency-first. Otherwise a
  // full list of frequent old files would permanently exclude new choices.
  while (entries.size() > entryLimit)
    entries.erase(std::min_element(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
      return a.lastUsed < b.lastUsed;
    }));
  std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
    return a.uses != b.uses ? a.uses > b.uses : a.lastUsed > b.lastUsed;
  });
}

struct SessionHistory {
  QList<Entry> folders;
  QList<Entry> files;
  SessionHistory() { reload(); }
  void reload() {
    QSettings settings;
    settings.sync();
    const QByteArray bytes = settings.value(QStringLiteral("fileDialog/historyV1")).toByteArray();
    const auto object = bytes.size() <= byteLimit ? QJsonDocument::fromJson(bytes).object() : QJsonObject();
    const auto read = [](const QJsonArray &values) {
      QList<Entry> entries;
      QStringList seen;
      for (const auto &value : values) {
        const auto item = value.toObject();
        const QString path = value.isString() ? value.toString() : item.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || path.size() > 32768 || !QFileInfo(path).isAbsolute()) continue;
        const QString key = identity(path);
        if (containsIdentity(seen, key)) continue;
        const int uses = value.isString() ? 1 : item.value(QStringLiteral("uses")).toInt();
        const qint64 last = value.isString() ? 0 : item.value(QStringLiteral("lastUsed")).toInteger(-1);
        if (uses < 1 || uses > 1000000 || last < 0 || last >= timestampLimit) continue;
        seen.append(key);
        entries.append({path, key, uses, last});
        if (entries.size() >= entryLimit) break;
      }
      rank(entries);
      return entries;
    };
    folders = read(object.value(QStringLiteral("folders")).toArray());
    files = read(object.value(QStringLiteral("files")).toArray());
  }
  void save() {
    const auto encode = [](const QList<Entry> &entries) {
      QJsonArray values;
      for (const auto &entry : entries)
        values.append(QJsonObject{{QStringLiteral("path"), entry.path},
                                  {QStringLiteral("uses"), entry.uses},
                                  {QStringLiteral("lastUsed"), static_cast<double>(entry.lastUsed)}});
      return values;
    };
    const auto bytes = [&] {
      return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                      {QStringLiteral("folders"), encode(folders)},
                                      {QStringLiteral("files"), encode(files)}}).toJson(QJsonDocument::Compact);
    };
    QByteArray encoded = bytes();
    // Apply the same byte bound when writing and reading. Drop whole oldest
    // records rather than truncating a Unicode path into a different target.
    while (encoded.size() > byteLimit && (!folders.isEmpty() || !files.isEmpty())) {
      const auto oldest = [](QList<Entry> &values) {
        return std::min_element(values.begin(), values.end(), [](const Entry &a, const Entry &b) {
          return a.lastUsed < b.lastUsed;
        });
      };
      const auto folder = oldest(folders);
      const auto file = oldest(files);
      if (file == files.end() || (folder != folders.end() && folder->lastUsed <= file->lastUsed))
        folders.erase(folder);
      else files.erase(file);
      encoded = bytes();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("fileDialog/historyV1"), encoded);
    settings.sync();
  }
  void clear() {
    folders.clear();
    files.clear();
    QSettings settings;
    settings.remove(QStringLiteral("fileDialog/historyV1"));
    settings.sync();
  }
};

SessionHistory &history() {
  static SessionHistory value;
  return value;
}

void remember(QList<Entry> &entries, const QString &path) {
  const QString key = identity(path);
  auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry &entry) {
    return sameIdentity(entry.key, key);
  });
  qint64 last = std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch());
  for (const auto &entry : entries) last = std::max(last, entry.lastUsed + 1);
  last = std::min(last, timestampLimit - 1);
  if (found == entries.end()) entries.append({path, key, 1, last});
  else {
    found->path = path; // Keep the selected Unicode path, not the canonical alias.
    found->key = key;
    found->uses = std::min(found->uses + 1, 1000000);
    found->lastUsed = last;
  }
  rank(entries);
}

class HistoryController final : public QObject {
public:
  HistoryController(QFileDialog *dialog, QListView *places, QSplitter *splitter)
      : QObject(dialog), mDialog(dialog), mPlaces(places) {
    setObjectName(QLatin1String(controllerName));
    const auto widths = splitter->sizes();
    const int position = splitter->indexOf(places);
    mColumn = new QWidget(dialog);
    mColumn->setObjectName(QStringLiteral("fileHistoryColumn"));
    mColumn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *layout = new QVBoxLayout(mColumn);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    splitter->replaceWidget(position, mColumn);
    places->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    places->setTextElideMode(Qt::ElideMiddle);
    places->setResizeMode(QListView::Adjust);
    places->setUniformItemSizes(false);
    layout->addWidget(places);
    mTree = new QTreeWidget(mColumn);
    mTree->setObjectName(QStringLiteral("fileHistoryTree"));
    mTree->setHeaderHidden(true);
    mTree->setRootIsDecorated(false);
    mTree->setFrameShape(QFrame::NoFrame);
    mTree->setUniformRowHeights(true);
    mTree->setTextElideMode(Qt::ElideMiddle);
    mTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mTree->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    layout->addWidget(mTree, 1);
    mFolders = new QTreeWidgetItem(mTree);
    mFiles = new QTreeWidgetItem(mTree);
    for (auto *group : {mFolders, mFiles}) {
      group->setFlags(Qt::ItemIsEnabled);
      group->setExpanded(true);
    }
    mClear = new QPushButton(mColumn);
    mClear->setObjectName(QStringLiteral("clearFileHistory"));
    mClear->setAutoDefault(false);
    mClear->setDefault(false);
    layout->addWidget(mClear);
    connect(mClear, &QPushButton::clicked, this, [this] {
      const auto answer = QMessageBox::question(mDialog,
          QCoreApplication::translate("Workbench", "清除历史"),
          QCoreApplication::translate("Workbench", "清除常用文件夹和文件记录？不会删除或修改任何文件。"),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
      if (answer != QMessageBox::Yes) return;
      history().clear();
      for (auto *widget : QApplication::allWidgets())
        if (auto *file = qobject_cast<QFileDialog *>(widget))
          if (auto *controller = file->findChild<QObject *>(QLatin1String(controllerName), Qt::FindDirectChildrenOnly))
            static_cast<HistoryController *>(controller)->scheduleRefresh();
    });
    splitter->setSizes(widths);
    dialog->installEventFilter(this);
    connect(dialog, &QObject::destroyed, this, [this] { mTearingDown = true; });
    places->installEventFilter(this);
    mTree->installEventFilter(this);
    mPlacesViewport = places->viewport();
    mPlacesViewport->installEventFilter(this);
    connect(dialog, &QFileDialog::filterSelected, this, [this] { scheduleRefresh(); });
    if (auto *type = dialog->findChild<QComboBox *>(QStringLiteral("fileTypeCombo")))
      connect(type, &QComboBox::currentIndexChanged, this, [this] { scheduleRefresh(); });
    connect(mTree, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int) { navigate(item); });
    connect(dialog, &QDialog::accepted, this, [this] {
      // Browsing and cancellation do not enter the history. Save destinations
      // remember only their existing parent; they are not opened input files.
      history().reload();
      QStringList foldersSeen;
      QStringList filesSeen;
      const auto rememberFolder = [&](const QString &path) {
        const QString key = identity(path);
        if (!containsIdentity(foldersSeen, key)) { foldersSeen.append(key); remember(history().folders, path); }
      };
      for (const QString &path : mDialog->selectedFiles()) {
        const QFileInfo info(path);
        if (info.isDir()) rememberFolder(info.absoluteFilePath());
        else {
          const QString parent = info.absolutePath();
          if (QFileInfo(parent).isDir()) rememberFolder(parent);
          const QString key = identity(path);
          if (mDialog->acceptMode() == QFileDialog::AcceptOpen && info.isFile() && !containsIdentity(filesSeen, key)) {
            filesSeen.append(key);
            remember(history().files, info.absoluteFilePath());
          }
        }
      }
      history().save();
    });
    AppLanguage::onChanged(this, [this] {
      if (mTearingDown || !mDialog) return;
      refreshText();
      scheduleMetrics();
    });
    history().reload();
    refresh();
  }

private:
  bool eventFilter(QObject *watched, QEvent *event) override {
    // QWidget deletes its original splitter children before QObject clears
    // weak pointers. Its destroyed signal marks teardown before those deletes.
    if (mTearingDown || !mDialog) return QObject::eventFilter(watched, event);
    if (watched == mDialog && event->type() == QEvent::Show) scheduleRefresh();
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange || event->type() == QEvent::LanguageChange)
      scheduleMetrics();
    if (watched == mPlacesViewport && event->type() == QEvent::Resize) scheduleMetrics();
    return QObject::eventFilter(watched, event);
  }

  void scheduleMetrics() {
    if (mMetricsPending) return;
    mMetricsPending = true;
    QTimer::singleShot(0, this, [this] {
      mMetricsPending = false;
      if (!mTearingDown && mDialog) refreshMetrics();
    });
  }

  void refreshMetrics() {
    const int scale = qApp->property("gswUiScalePercent").toInt();
    mColumn->setMaximumWidth(AppTheme::scaled(260, scale));
    QFont font = mTree->font();
    font.setBold(true);
    for (auto *group : {mFolders, mFiles}) group->setFont(0, font);
    mPlaces->doItemsLayout();
    const int count = mPlaces->model() ? mPlaces->model()->rowCount() : 0;
    int rowHeight = mPlaces->fontMetrics().height() + AppTheme::scaled(12, scale);
    for (int row = 0; row < count; ++row) rowHeight = std::max(rowHeight, mPlaces->sizeHintForRow(row));
    mPlaces->setGridSize(QSize(std::max(1, mPlaces->viewport()->width()), rowHeight));
    int height = 2 * mPlaces->frameWidth() + AppTheme::scaled(8, scale);
    height += count * rowHeight;
    mPlaces->setFixedHeight(height);
  }

  void scheduleRefresh() {
    if (mRefreshPending) return;
    mRefreshPending = true;
    QTimer::singleShot(0, this, [this] {
      mRefreshPending = false;
      if (!mTearingDown && mDialog) refresh();
    });
  }

  bool matchesFilter(const QString &path) const {
    QString filter = mDialog->selectedNameFilter();
    // HideNameFilterDetails can strip wildcards from the visible combo text.
    // Read the original machine patterns by the current index, not its label.
    if (auto *type = mDialog->findChild<QComboBox *>(QStringLiteral("fileTypeCombo"));
        type && type->currentIndex() >= 0 && type->currentIndex() < mDialog->nameFilters().size())
      filter = mDialog->nameFilters().at(type->currentIndex());
    static const QRegularExpression patterns(QStringLiteral("\\(([^()]*)\\)\\s*$"));
    const auto match = patterns.match(filter);
    if (!match.hasMatch()) return true;
    const QStringList wildcards = match.captured(1).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    return wildcards.isEmpty() || QDir::match(wildcards, QFileInfo(path).fileName());
  }

  void refreshText() {
    mFolders->setText(0, QCoreApplication::translate("Workbench", "常用文件夹"));
    mFiles->setText(0, QCoreApplication::translate("Workbench", "常用文件"));
    mTree->setAccessibleName(QCoreApplication::translate("Workbench", "文件窗口历史"));
    mClear->setText(QCoreApplication::translate("Workbench", "清除历史"));
    mClear->setToolTip(QCoreApplication::translate("Workbench", "仅清除文件窗口历史，不删除或修改文件。"));
  }

  void addEntry(QTreeWidgetItem *group, const QString &path, bool directory) {
    const QFileInfo info(path);
    if ((directory && !info.isDir()) || (!directory && !info.isFile())) return;
    auto *item = new QTreeWidgetItem(group);
    item->setText(0, info.fileName().isEmpty() ? QDir::toNativeSeparators(path) : info.fileName());
    item->setToolTip(0, QDir::toNativeSeparators(path));
    item->setIcon(0, mDialog->style()->standardIcon(
        directory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
    item->setData(0, pathRole, path);
    item->setData(0, directoryRole, directory);
  }

  void refresh() {
    qDeleteAll(mFolders->takeChildren());
    qDeleteAll(mFiles->takeChildren());
    refreshText();
    for (const auto &entry : history().folders) {
      addEntry(mFolders, entry.path, true);
      if (mFolders->childCount() >= 8) break;
    }
    const bool showFiles = mDialog->acceptMode() == QFileDialog::AcceptOpen &&
                           mDialog->fileMode() != QFileDialog::Directory;
    if (showFiles)
      for (const auto &entry : history().files) {
        if (matchesFilter(entry.path)) addEntry(mFiles, entry.path, false);
        if (mFiles->childCount() >= 12) break;
      }
    mFiles->setHidden(!showFiles);
    refreshMetrics();
  }

  void navigate(QTreeWidgetItem *item) {
    const QString path = item->data(0, pathRole).toString();
    if (path.isEmpty()) return;
    const QFileInfo info(path);
    const bool directory = item->data(0, directoryRole).toBool();
    if ((directory && !info.isDir()) || (!directory && !info.isFile())) return;
    if (directory) mDialog->setDirectory(path);
    else {
      mDialog->setDirectory(info.absolutePath());
      // QFileDialog intentionally ignores selectFile while its filename
      // editor has focus. A history click is an explicit replacement choice.
      mTree->setFocus(Qt::MouseFocusReason);
      mDialog->selectFile(info.absoluteFilePath());
    }
  }

  QPointer<QFileDialog> mDialog;
  QListView *mPlaces;
  QPointer<QWidget> mPlacesViewport;
  QWidget *mColumn;
  QTreeWidget *mTree;
  QPushButton *mClear;
  QTreeWidgetItem *mFolders;
  QTreeWidgetItem *mFiles;
  bool mRefreshPending = false;
  bool mMetricsPending = false;
  bool mTearingDown = false;
};
} // namespace

void FileDialogHistory::attach(QFileDialog *dialog) {
  if (!dialog || dialog->findChild<QObject *>(QLatin1String(controllerName),
                                             Qt::FindDirectChildrenOnly)) return;
  auto *places = dialog->findChild<QListView *>(QStringLiteral("sidebar"));
  auto *splitter = places ? qobject_cast<QSplitter *>(places->parentWidget()) : nullptr;
  if (places && splitter && splitter->indexOf(places) >= 0)
    new HistoryController(dialog, places, splitter);
}
} // namespace gsw

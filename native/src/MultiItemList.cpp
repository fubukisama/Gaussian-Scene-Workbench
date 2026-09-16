#include "MultiItemList.h"
#include "AppLanguage.h"

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>

namespace gsw {
QList<QPersistentModelIndex> selectedListRows(QAbstractItemView *view) {
  QList<QPersistentModelIndex> result;
  for (const auto &index : view->selectionModel()->selectedRows()) result.append(index);
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.row() > b.row(); });
  return result;
}

MultiItemList::MultiItemList(QAbstractItemView *view, const char *removeText,
                           std::function<void()> remove, bool contextMenu)
    : QObject(view), mView(view), mRemoveSelected(std::move(remove)) {
  view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  view->setSelectionBehavior(QAbstractItemView::SelectRows);
  view->installEventFilter(this);
  view->viewport()->installEventFilter(this);
  AppLanguage::bind(view, "toolTip", AppLanguage::source("Ctrl 点选多选 · Shift 连选 · Ctrl+A 全选 · Delete 移除所选"));
  mAll = AppLanguage::text(new QAction(this), AppLanguage::source("全选"));
  mNone = AppLanguage::text(new QAction(this), AppLanguage::source("取消全选"));
  mRemove = AppLanguage::text(new QAction(this), removeText);
  mAll->setObjectName(QStringLiteral("listSelectAll"));
  mNone->setObjectName(QStringLiteral("listClearSelection"));
  mRemove->setObjectName(QStringLiteral("listRemoveSelected"));
  connect(mAll, &QAction::triggered, view, &QAbstractItemView::selectAll);
  connect(mNone, &QAction::triggered, view, &QAbstractItemView::clearSelection);
  connect(mRemove, &QAction::triggered, this, [this]() { if (mRemoveSelected) mRemoveSelected(); });
  connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { refresh(); });
  connect(view->model(), &QAbstractItemModel::rowsInserted, this, [this]() { refresh(); });
  connect(view->model(), &QAbstractItemModel::rowsRemoved, this, [this]() { refresh(); });
  connect(view->model(), &QAbstractItemModel::modelReset, this, [this]() { refresh(); });
  if (contextMenu) {
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
      // Right-clicking within the selection must not collapse a batch.
      const auto index = mView->indexAt(pos);
      if (index.isValid() && !mView->selectionModel()->isSelected(index))
        mView->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
      QMenu menu(mView);
      addToMenu(menu);
      menu.exec(mView->viewport()->mapToGlobal(pos));
    });
  }
  refresh();
}

void MultiItemList::refresh() {
  const bool selected = mView->selectionModel()->hasSelection();
  mAll->setEnabled(mView->model()->rowCount() > 0);
  mNone->setEnabled(selected);
  mRemove->setEnabled(selected);
}

void MultiItemList::addToMenu(QMenu &menu) {
  refresh();
  menu.addAction(mAll);
  menu.addAction(mNone);
  menu.addSeparator();
  menu.addAction(mRemove);
}

QWidget *MultiItemList::createBar(QWidget *parent) {
  auto *bar = new QWidget(parent);
  auto *layout = new QHBoxLayout(bar);
  layout->setContentsMargins(0, 0, 0, 0);
  for (auto *action : {mAll, mNone}) {
    auto *button = new QToolButton(bar);
    button->setDefaultAction(action);
    layout->addWidget(button);
  }
  auto *count = new QLabel(bar);
  count->setObjectName(QStringLiteral("listSelectionCount"));
  const auto update = [this, count]() {
    count->setText(QCoreApplication::translate("Workbench", "已选 %1 项").arg(selectedListRows(mView).size()));
  };
  connect(mView->selectionModel(), &QItemSelectionModel::selectionChanged, count, update);
  AppLanguage::onChanged(count, update);
  update();
  layout->addWidget(count);
  layout->addStretch();
  auto *remove = new QToolButton(bar);
  remove->setDefaultAction(mRemove);
  layout->addWidget(remove);
  return bar;
}

bool MultiItemList::eventFilter(QObject *, QEvent *event) {
  if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress) return false;
  auto *key = static_cast<QKeyEvent *>(event);
  const bool remove = key->key() == Qt::Key_Delete && key->modifiers() == Qt::NoModifier;
  const bool all = key->matches(QKeySequence::SelectAll);
  if (!remove && !all) return false;
  event->accept();
  if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
    if (all) mAll->trigger();
    else if (mRemove->isEnabled()) mRemove->trigger();
  }
  return true;
}

bool confirmListRemoval(QWidget *parent, const QString &effect, const QStringList &items) {
  if (items.isEmpty()) return false;
  QMessageBox box(QMessageBox::Warning, QCoreApplication::translate("Workbench", "确认批量移除"),
                  QCoreApplication::translate("Workbench", "将处理所选的 %1 项。\n%2").arg(items.size()).arg(effect),
                  QMessageBox::Yes | QMessageBox::Cancel, parent);
  box.setTextFormat(Qt::PlainText);
  box.setInformativeText(items.mid(0, 8).join(QLatin1Char('\n')));
  box.setDetailedText(items.join(QLatin1Char('\n')));
  box.setDefaultButton(QMessageBox::Cancel);
  return box.exec() == QMessageBox::Yes;
}

void installRecordRemoval(QTableWidget *table, QPushButton *restore, QWidget *barParent,
                          const char *effect, std::function<QString(int)> describe,
                          std::function<bool(int, QString *)> remove) {
  auto *selection = new MultiItemList(table, AppLanguage::source("删除所选记录"), [=]() {
    const auto rows = selectedListRows(table);
    QStringList names;
    QList<int> ids;
    for (const auto &row : rows) {
      const int id = row.data(Qt::UserRole).toInt();
      ids.append(id);
      names.append(describe(id));
    }
    if (!confirmListRemoval(barParent, QCoreApplication::translate("Workbench", effect), names)) return;
    // All disk operations run off the UI thread; the dialog stays alive until completion.
    QProgressDialog progress(QCoreApplication::translate("Workbench", "正在处理所选记录…"), QString(), 0, 0, barParent);
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::ApplicationModal);
    QFutureWatcher<QList<QPair<int, QString>>> watcher;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &progress, &QProgressDialog::close);
    table->setEnabled(false);
    watcher.setFuture(QtConcurrent::run([ids, remove]() {
      QList<QPair<int, QString>> result;
      for (int id : ids) {
        QString error;
        if (remove(id, &error)) result.emplaceBack(id, QString());
        else result.emplaceBack(id, error.isEmpty() ? QCoreApplication::translate("Workbench", "移除失败") : error);
      }
      return result;
    }));
    if (!watcher.isFinished()) progress.exec();
    watcher.waitForFinished();
    QStringList errors;
    for (const auto &[id, error] : watcher.result()) {
      if (!error.isEmpty()) { errors.append(describe(id) + QLatin1Char('\n') + error); continue; }
      for (int row = table->rowCount() - 1; row >= 0; --row)
        if (table->item(row, 0)->data(Qt::UserRole).toInt() == id) table->removeRow(row);
    }
    table->setEnabled(true);
    if (!errors.isEmpty()) {
      QMessageBox box(QMessageBox::Warning, QCoreApplication::translate("Workbench", "部分记录未能移除"),
          QCoreApplication::translate("Workbench", "已移除成功项；失败项仍保留在列表中，可重试。"), QMessageBox::Ok, barParent);
      box.setDetailedText(errors.join(QLatin1Char('\n')));
      box.exec();
    }
  });
  auto *layout = qobject_cast<QVBoxLayout *>(barParent->layout());
  layout->insertWidget(layout->count() - 1, selection->createBar(barParent));
  AppLanguage::bind(restore, "toolTip", AppLanguage::source("恢复操作每次只能选择一项；多选可批量删除。"));
  const auto update = [table, restore]() { restore->setEnabled(selectedListRows(table).size() == 1); };
  QObject::connect(table, &QTableWidget::itemSelectionChanged, restore, update);
  update();
}
} // namespace gsw

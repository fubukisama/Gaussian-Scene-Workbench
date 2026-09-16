#include "MultiItemList.h"
#include "AppLanguage.h"
#include <QApplication>
#include <QDialog>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

class MultiItemListTests final : public QObject {
  Q_OBJECT
private slots:
  void shortcutsAndRangeSelection();
  void recordRemovalPreservesIdentityAndFailures();
};

void MultiItemListTests::shortcutsAndRangeSelection() {
  for (int kind = 0; kind < 3; ++kind) {
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    QAbstractItemView *view = nullptr;
    if (kind == 0) {
      auto *table = new QTableWidget(5, 2);
      for (int row = 0; row < 5; ++row)
        for (int col = 0; col < 2; ++col) table->setItem(row, col, new QTableWidgetItem(QString::number(row)));
      view = table;
    } else if (kind == 1) {
      auto *list = new QListWidget;
      list->addItems({"one", "two", "three", "four", "five"});
      view = list;
    } else {
      auto *tree = new QTreeWidget;
      for (int row = 0; row < 5; ++row) new QTreeWidgetItem(tree, {QString::number(row)});
      view = tree;
    }
    layout->addWidget(view);
    int removed = 0;
    gsw::MultiItemList selection(view, gsw::AppLanguage::source("移除所选"), [&]() { ++removed; });
    QAction global(&window);
    global.setShortcut(QKeySequence::Delete);
    window.addAction(&global);
    int viewportDeletes = 0;
    connect(&global, &QAction::triggered, &window, [&]() { ++viewportDeletes; });
    window.resize(500, 300);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
    view->setFocus();
    auto click = [&](int row, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
      QTest::mouseClick(view->viewport(), Qt::LeftButton, modifiers,
                        view->visualRect(view->model()->index(row, 0)).center());
    };
    click(0);
    click(2, Qt::ControlModifier);
    QCOMPARE(gsw::selectedListRows(view).size(), 2);
    click(4, Qt::ShiftModifier);
    QVERIFY(gsw::selectedListRows(view).size() >= 3);
    QTest::keyClick(view, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(gsw::selectedListRows(view).size(), 5);
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(removed, 1);
    QCOMPARE(viewportDeletes, 0);
    view->clearSelection();
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(removed, 1);
    QCOMPARE(viewportDeletes, 0);
  }
}

void MultiItemListTests::recordRemovalPreservesIdentityAndFailures() {
  QDialog dialog;
  auto *layout = new QVBoxLayout(&dialog);
  auto *table = new QTableWidget(3, 1, &dialog);
  for (int row = 0; row < 3; ++row) {
    auto *item = new QTableWidgetItem(QString::number(3 - row));
    item->setData(Qt::UserRole, row);
    table->setItem(row, 0, item);
  }
  layout->addWidget(table);
  auto *restore = new QPushButton("Restore", &dialog);
  layout->addWidget(restore);
  QList<int> removed;
  gsw::installRecordRemoval(table, restore, &dialog, gsw::AppLanguage::source("删除所选记录"),
      [](int id) { return QString::number(id); },
      [&](int id, QString *error) {
        removed.append(id);
        if (id == 1) { *error = "test-owned failure"; return false; }
        return true;
      });
  table->setSortingEnabled(true);
  table->sortItems(0, Qt::AscendingOrder);
  table->selectAll();
  QVERIFY(!restore->isEnabled());
  auto *remove = table->findChild<QAction *>("listRemoveSelected");
  QVERIFY(remove);
  QTimer dismiss;
  bool confirm = false;
  connect(&dismiss, &QTimer::timeout, &dialog, [&]() {
    if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
      if (box->standardButtons().testFlag(QMessageBox::Yes)) {
        QCOMPARE(static_cast<QAbstractButton *>(box->defaultButton()), box->button(QMessageBox::Cancel));
        box->done(confirm ? QMessageBox::Yes : QMessageBox::Cancel);
      } else box->accept();
    }
  });
  dismiss.start(5);
  remove->trigger();
  QVERIFY(removed.isEmpty());
  QCOMPARE(table->rowCount(), 3);
  confirm = true;
  remove->trigger();
  QCOMPARE(removed.size(), 3);
  QCOMPARE(table->rowCount(), 1);
  QCOMPARE(table->item(0, 0)->data(Qt::UserRole).toInt(), 1);
  QCOMPARE(gsw::selectedListRows(table).size(), 1);
  QVERIFY(restore->isEnabled());
  table->clearSelection();
  QVERIFY(!restore->isEnabled());
}

QTEST_MAIN(MultiItemListTests)
#include "MultiItemListTests.moc"

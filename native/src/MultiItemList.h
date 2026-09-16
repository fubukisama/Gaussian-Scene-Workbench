#pragma once

#include <QAbstractItemView>
#include <QAction>
#include <QPersistentModelIndex>
#include <functional>

class QMenu;
class QPushButton;
class QTableWidget;

namespace gsw {
// Shared list-only shortcuts. Never let Delete/Ctrl+A reach viewport editing.
class MultiItemList final : public QObject {
public:
  MultiItemList(QAbstractItemView *view, const char *removeText,
                std::function<void()> remove, bool contextMenu = true);
  QWidget *createBar(QWidget *parent);
  void addToMenu(QMenu &menu);
  void refresh();
  QAction *removeAction() const { return mRemove; }
protected:
  bool eventFilter(QObject *, QEvent *) override;
private:
  QAbstractItemView *mView;
  QAction *mAll;
  QAction *mNone;
  QAction *mRemove;
  std::function<void()> mRemoveSelected;
};

QList<QPersistentModelIndex> selectedListRows(QAbstractItemView *view);
bool confirmListRemoval(QWidget *parent, const QString &effect, const QStringList &items);
// Each first-column UserRole is an immutable source-record index.
void installRecordRemoval(QTableWidget *table, QPushButton *restore, QWidget *barParent,
                          const char *effect,
                          std::function<QString(int)> describe,
                          std::function<bool(int, QString *)> remove);
} // namespace gsw

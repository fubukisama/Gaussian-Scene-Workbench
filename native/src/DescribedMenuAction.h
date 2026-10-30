#pragma once

#include <QAccessible>
#include <QAccessibleObject>
#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QPointer>
#include <QStyle>
#include <QWindow>

namespace gsw {
namespace detail {

// Original public-API adapter for explicitly opted-in leaf actions only.
// Qt 6.8's stock MenuItem exposes Name/Accelerator but no Description/Help.
// Do not use a recursive factory, replace cached interfaces, or depend on Qt's
// private QAccessibleMenuItem ABI. The real QMenu still owns navigation/popups.
class DescribedMenuItem final : public QAccessibleObject,
                                public QAccessibleActionInterface {
public:
  DescribedMenuItem(QMenu *menu, QAction *action)
      : QAccessibleObject(action), mMenu(menu), mAction(action) {}

  bool isValid() const override { return mMenu && mAction; }
  QObject *object() const override { return mAction.data(); }
  QAccessibleInterface *parent() const override {
    return mMenu ? QAccessible::queryAccessibleInterface(mMenu.data()) : nullptr;
  }
  QAccessibleInterface *child(int) const override { return nullptr; }
  int childCount() const override { return 0; }
  int indexOfChild(const QAccessibleInterface *) const override { return -1; }
  QAccessibleInterface *childAt(int, int) const override { return nullptr; }
  QAccessible::Role role() const override { return QAccessible::MenuItem; }
  QRect rect() const override {
    if (!isValid()) return {};
    return mMenu->actionGeometry(mAction.data()).translated(mMenu->mapToGlobal(QPoint()));
  }
  QWindow *window() const override {
    for (QWidget *widget = mMenu.data(); widget; widget = widget->parentWidget())
      if (auto *handle = widget->windowHandle()) return handle;
    return nullptr;
  }
  QAccessible::State state() const override {
    QAccessible::State result;
    if (!isValid()) { result.invalid = true; return result; }
    result.disabled = !mAction->isEnabled();
    result.checkable = mAction->isCheckable();
    result.checked = mAction->isChecked();
    result.focused = mMenu->activeAction() == mAction.data();
    result.invisible = !mMenu->isVisible() || !mAction->isVisible();
    result.hotTracked = mMenu->style()->styleHint(QStyle::SH_Menu_MouseTracking, nullptr, mMenu.data());
    return result;
  }
  QString text(QAccessible::Text kind) const override {
    if (!isValid()) return {};
    if (kind == QAccessible::Description) return mAction->toolTip();
    if (kind == QAccessible::Help) return mAction->whatsThis();
    if (kind == QAccessible::Accelerator) {
      const auto shortcut = mAction->shortcut();
      return (shortcut.isEmpty() ? QKeySequence::mnemonic(mAction->text()) : shortcut).toString();
    }
    if (kind != QAccessible::Name) return {};
    // A single ampersand marks a mnemonic; a doubled ampersand is literal.
    const QString label = mAction->text();
    QString name;
    for (qsizetype index = 0; index < label.size(); ++index) {
      if (label[index] != QLatin1Char('&')) name += label[index];
      else if (index + 1 < label.size() && label[index + 1] == QLatin1Char('&')) {
        name += QLatin1Char('&');
        ++index;
      }
    }
    return name;
  }
  void setText(QAccessible::Text, const QString &) override {}
  void *interface_cast(QAccessible::InterfaceType kind) override {
    return kind == QAccessible::ActionInterface
        ? static_cast<QAccessibleActionInterface *>(this) : nullptr;
  }
  QStringList actionNames() const override {
    return isValid() ? QStringList{pressAction()} : QStringList{};
  }
  void doAction(const QString &name) override {
    // Normal assistive activation, never used to manufacture test navigation.
    if (isValid() && mAction->isEnabled() && name == pressAction()) mAction->trigger();
  }
  QStringList keyBindingsForAction(const QString &) const override { return {}; }

private:
  QPointer<QMenu> mMenu;
  QPointer<QAction> mAction;
};
} // namespace detail

inline void addDescribedMenuAction(QMenu *menu, QAction *action) {
  if (!menu || !action) return;
  // Register before insertion: a menu's child query must see this interface
  // on its first lookup. Never evict an interface another provider already owns.
  if (!action->menu() && !action->isSeparator() &&
      !QAccessible::queryAccessibleInterface(action)) {
    QAccessible::registerAccessibleInterface(new detail::DescribedMenuItem(menu, action));
    QObject::connect(action, &QAction::changed, action,
                     [action, previous = action->toolTip()]() mutable {
      const QString description = action->toolTip();
      if (description == previous) return;
      previous = description;
      if (QAccessible::isActive()) {
        QAccessibleEvent changed(action, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&changed);
      }
    });
  }
  menu->addAction(action);
}
} // namespace gsw

#pragma once

// QA-only observation of an existing owned MainWindow's real menu path.
// No QAction is triggered and no menu/accessibility/focus property is assigned.
#include "AppLanguage.h"
#include "AppTheme.h"
#include "NativeViewport.h"
#include "ProcessSupervisor.h"
#include "WorkspaceDocument.h"

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeyEvent>
#include <QList>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QStyle>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>

#include <functional>
#include <optional>
#include <utility>

namespace gsw::tests {

struct AccessibleMenuItemExpectation {
  QAction *action = nullptr; // The real existing action, used only for identity/routing.
  QString name;              // Independent, localized user-facing oracle from the test.
  bool disabled = false;
  std::optional<bool> checked;
  // Require these independent fragments in the assistive Description or Help,
  // not merely in the QAction tooltip. For an idle disabled reason use a real
  // reason oracle, not the action's generic description of its normal function.
  QStringList explanationFragments;
};

struct MenuKeyboardPathResult {
  bool passed = false;
  QJsonObject evidence;
};

// Passive QA observer for the independent synthetic-Qt Alt route. It neither
// accepts nor filters an event, and it never sends a ShortcutOverride itself.
class MenuKeyboardEventObserver final : public QObject {
public:
  MenuKeyboardEventObserver(bool enabled,
      std::function<bool(QWidget *)> isOwned,
      std::function<QJsonObject(const QObject *)> identity)
      : enabled_(enabled), isOwned_(std::move(isOwned)), identity_(std::move(identity)) {
    if (enabled_) qApp->installEventFilter(this);
  }
  ~MenuKeyboardEventObserver() override {
    if (enabled_ && qApp) qApp->removeEventFilter(this);
  }
  QJsonArray events;
  int droppedEvents = 0;
  bool altPressObserved = false;
  bool altShortcutOverrideObserved = false;
  bool altReleaseObserved = false;
  bool helperDispatch = false;
  int dispatchKey = 0;
  QString dispatchPhase;

protected:
  bool eventFilter(QObject *receiver, QEvent *event) override {
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease &&
        event->type() != QEvent::ShortcutOverride) return false;
    auto *key = static_cast<QKeyEvent *>(event);
    auto *widget = qobject_cast<QWidget *>(receiver);
    const bool owned = widget && isOwned_(widget);
    const auto modifiers = key->modifiers();
    if (events.size() < 2048) {
      events.append(QJsonObject{{"type", int(event->type())}, {"key", key->key()},
          {"modifiers", int(modifiers)}, {"spontaneous", event->spontaneous()},
          {"receiver", identity_(receiver)}, {"ownedReceiver", owned},
          {"helperDispatch", helperDispatch}, {"dispatchKey", dispatchKey},
          {"dispatchPhase", dispatchPhase}});
    } else ++droppedEvents;
    if (owned && helperDispatch && dispatchKey == Qt::Key_Alt &&
        key->key() == Qt::Key_Alt && !event->spontaneous()) {
      if (dispatchPhase == QStringLiteral("press")) {
        if (event->type() == QEvent::KeyPress) altPressObserved = true;
        if (event->type() == QEvent::ShortcutOverride && modifiers == Qt::AltModifier)
          altShortcutOverrideObserved = true;
      } else if (dispatchPhase == QStringLiteral("release") &&
                 event->type() == QEvent::KeyRelease && modifiers == Qt::NoModifier) {
        altReleaseObserved = true;
      }
    }
    return false;
  }

private:
  bool enabled_;
  std::function<bool(QWidget *)> isOwned_;
  std::function<QJsonObject(const QObject *)> identity_;
};

// A top-level menu followed by at most one existing submenu is enough for this
// bounded tranche (Language, Appearance, Workflow and Help). The caller supplies
// current locale/theme expectations and an existing public business-state
// snapshot. The default synthetic-Qt F10 observation remains unchanged and an
// explicit qt_alt route is an independent additional probe, never a fallback.
// A missing/uncontrollable F10 seam remains explicit NOT_RUN;
// setActiveAction(), popup(), show() or direct QAction::trigger() are not fallbacks.
inline MenuKeyboardPathResult checkMenuKeyboardPath(
    QMainWindow &ownedWindow, const QString &caseId, const QList<QMenu *> &path,
    const QList<AccessibleMenuItemExpectation> &expectations,
    const std::function<QJsonObject()> &publicSnapshot,
    const QString &entryRoute = QStringLiteral("qt_f10")) {
  MenuKeyboardPathResult result;
  auto &out = result.evidence;
  out = QJsonObject{{"schema", "gsw.menu_keyboard_path/1"},
                   {"sourceCommit", qEnvironmentVariable("GSW_UI_SOURCE_COMMIT")},
                   {"caseId", caseId}, {"platform", QApplication::platformName()},
                   {"status", "NOT_RUN"}, {"inputKind", "synthetic Qt widget key events"},
                   {"entryRoute", entryRoute}, {"nativeOsKeyboardClaim", false},
                   {"fullAccessibilityClaim", false},
                   {"ownershipPolicy", "QObject-parent-tree-and-existing-menu-action-graph"},
                   {"physicalOsKeyboardClaim", false}, {"directActionsTriggeredByHelper", false}};
  const bool altRoute = entryRoute == QStringLiteral("qt_alt");
  if (altRoute) out["f10Observed"] = QJsonValue(QJsonValue::Null);
  QJsonArray errors, keys, pathRecords, itemRecords, triggeredActions, receiverChecks;
  bool missingDisabledReasonOracle = false;
  const auto fail = [&](const char *reason) { errors.append(QString::fromLatin1(reason)); };
  auto *bar = ownedWindow.findChild<QMenuBar *>();
  const int altNavigationStyleHint = bar
      ? bar->style()->styleHint(QStyle::SH_MenuBar_AltKeyNavigation, nullptr, bar) : -1;
  out["altNavigationStyleHint"] = altNavigationStyleHint;
  if (!altRoute && entryRoute != QStringLiteral("qt_f10"))
    fail("unknown explicit menu keyboard entry route");
  const auto objectIdentity = [](const QObject *object) {
    return object ? QJsonObject{{"id", object->objectName()},
        {"class", QString::fromLatin1(object->metaObject()->className())},
        {"instance", QString::number(quintptr(object), 16)}} : QJsonObject{{"null", true}};
  };
  const auto objectOwnedByWindow = [&](const QObject *object) {
    // QObject ownership crosses QWidget window boundaries; QWidget::isAncestorOf
    // explicitly requires the same window and therefore rejects Qt::Popup menus.
    for (int depth = 0; object && depth < 64; ++depth, object = object->parent())
      if (object == &ownedWindow) return true;
    return false;
  };
  const auto parentChain = [&](const QObject *object) {
    QJsonArray chain;
    for (int depth = 0; object && depth < 16; ++depth, object = object->parent()) {
      chain.append(objectIdentity(object));
      if (object == &ownedWindow) break;
    }
    return chain;
  };
  const auto reachableMenu = [&](const QMenu *target) {
    if (!target || !bar || !objectOwnedByWindow(bar) || !objectOwnedByWindow(target)) return false;
    QList<QMenu *> pending;
    for (auto *action : bar->actions()) if (action->menu()) pending.append(action->menu());
    QSet<QMenu *> seen;
    // A shared action graph can revisit a menu. Bound and de-duplicate it rather
    // than treating an arbitrary QObject-owned popup as an accepted menu.
    while (!pending.isEmpty() && seen.size() < 128) {
      auto *menu = pending.takeFirst();
      if (seen.contains(menu) || !objectOwnedByWindow(menu)) continue;
      seen.insert(menu);
      if (menu == target) return true;
      for (auto *action : menu->actions()) if (action->menu()) pending.append(action->menu());
    }
    return false;
  };
  const auto widgetOwnership = [&](QWidget *widget) {
    auto *menuWindow = widget ? qobject_cast<QMenu *>(widget->window()) : nullptr;
    const bool objectOwned = objectOwnedByWindow(widget);
    const bool mainWindow = widget && widget->window() == &ownedWindow;
    const bool menuReachable = menuWindow && reachableMenu(menuWindow);
    return QJsonObject{{"object", objectIdentity(widget)}, {"parentChain", parentChain(widget)},
        {"window", objectIdentity(widget ? widget->window() : nullptr)},
        {"isWindow", widget && widget->isWindow()},
        {"windowType", widget ? int(widget->windowType()) : -1},
        {"widgetAncestorSameWindow", widget && ownedWindow.isAncestorOf(widget)},
        {"qobjectOwnedByMainWindow", objectOwned}, {"windowIsMainWindow", mainWindow},
        {"menuWindowReachableFromBar", bool(menuReachable)},
        {"accepted", objectOwned && (mainWindow || menuReachable)}};
  };
  out["menuBarOwnership"] = widgetOwnership(bar);
  out["initialFocusOwnership"] = widgetOwnership(QApplication::focusWidget());
  out["initialPopupOwnership"] = widgetOwnership(QApplication::activePopupWidget());
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QApplication::platformName() != QStringLiteral("windows") || !ownedWindow.isVisible() ||
      !ownedWindow.isActiveWindow() || QApplication::activeModalWidget() ||
      QApplication::activePopupWidget())
    fail("requires the active owned remote Windows main window without another modal/popup");
  if (!bar || !bar->isVisibleTo(&ownedWindow) || bar->isNativeMenuBar() || path.isEmpty() ||
      path.size() > 2 || expectations.isEmpty() || expectations.size() > 8 || !publicSnapshot)
    fail("existing Qt menu bar, one/two-menu path, independent expectations and snapshot are required");
  if (altRoute && altNavigationStyleHint <= 0)
    fail("the existing menu style does not enable Alt-key navigation");
  if (bar && !path.isEmpty()) {
    const bool barContainsTop = path.first() && bar->actions().contains(path.first()->menuAction());
    out["barContainsTopMenuAction"] = barContainsTop;
    if (!barContainsTop)
      fail("the supplied top menu is not an actual action of the owned menu bar");
    QJsonArray ownership;
    bool prefixReachable = barContainsTop;
    for (qsizetype index = 0; index < path.size(); ++index) {
      QMenu *menu = path.at(index);
      const bool parentContainsAction = !index ? barContainsTop : menu && path.at(index - 1) &&
          path.at(index - 1)->actions().contains(menu->menuAction());
      prefixReachable = prefixReachable && parentContainsAction;
      auto facts = widgetOwnership(menu);
      facts["pathIndex"] = int(index);
      facts["parentContainsMenuAction"] = parentContainsAction;
      facts["pathActionPrefixReachable"] = prefixReachable;
      facts["menuActionRefersToThisMenu"] = menu && menu->menuAction()->menu() == menu;
      ownership.append(facts);
      if (!menu || !objectOwnedByWindow(menu) || !reachableMenu(menu) || !prefixReachable ||
          menu->menuAction()->menu() != menu)
        fail("menu path is not reachable through the existing owned menu actions");
    }
    out["menuPathOwnership"] = ownership;
    if (path.last()) for (const auto &expected : expectations)
      if (!expected.action || expected.name.isEmpty() ||
          !path.last()->actions().contains(expected.action))
        fail("each independent named item must be an actual leaf-menu action");
    for (const auto &expected : expectations)
      if (expected.disabled && expected.explanationFragments.isEmpty()) {
        missingDisabledReasonOracle = true;
        fail("a disabled item requires an independent unavailability-reason oracle");
      }
  }
  if (!errors.isEmpty()) {
    out["status"] = missingDisabledReasonOracle
        ? QStringLiteral("NOT_RUN_DISABLED_REASON_ORACLE") : QStringLiteral("NOT_RUN_PRECONDITION");
    out["errors"] = errors;
    return result;
  }

  const QJsonObject before = publicSnapshot();
  out["publicSnapshotBefore"] = before;
  const QPointer<QWidget> originalFocus(QApplication::focusWidget());
  const QPointer<QMainWindow> owner(&ownedWindow);
  QElapsedTimer deadline;
  deadline.start();
  constexpr int maximumKeyEvents = 96;
  constexpr int perCaseBudgetMs = 2500;
  int keyCount = 0;
  const auto actionRecord = [](QAction *action) {
    return action ? QJsonObject{{"id", action->objectName()}, {"data", action->data().toString()},
                                {"text", action->text()}, {"enabled", action->isEnabled()}}
                  : QJsonObject{{"null", true}};
  };
  const auto isOwned = [&](QWidget *widget) {
    if (!owner || !widget || !objectOwnedByWindow(widget)) return false;
    if (widget->window() == owner.data()) return true;
    // Only actual QMenus reachable from this main window's existing menu bar
    // can cross the window boundary. Another owned dialog/combo popup is not a
    // keyboard target, and a foreign menu is rejected even if its text matches.
    auto *menuWindow = qobject_cast<QMenu *>(widget->window());
    return menuWindow && reachableMenu(menuWindow);
  };
  MenuKeyboardEventObserver keyObserver(altRoute, isOwned, objectIdentity);
  const auto recordObservedKeys = [&] {
    if (!altRoute) return;
    out["qtKeyEvents"] = keyObserver.events;
    out["qtKeyEventsDropped"] = keyObserver.droppedEvents;
    out["altPressObserved"] = keyObserver.altPressObserved;
    out["altShortcutOverrideObserved"] = keyObserver.altShortcutOverrideObserved;
    out["altReleaseObserved"] = keyObserver.altReleaseObserved;
  };
  // Observe the real actions, including dangerous actions, without invoking any.
  // Connections are scoped and always disconnected before returning.
  struct TriggerObservers {
    QList<QMetaObject::Connection> connections;
    ~TriggerObservers() { for (const auto &connection : connections) QObject::disconnect(connection); }
  } observers;
  QSet<QAction *> observed;
  for (auto *menu : ownedWindow.findChildren<QMenu *>()) for (auto *action : menu->actions()) {
    if (observed.contains(action)) continue;
    observed.insert(action);
    observers.connections.append(QObject::connect(action, &QAction::triggered, &ownedWindow,
        [&, action] { triggeredActions.append(actionRecord(action)); }));
  }
  const auto settle = [] {
    // Fixed bounded dispatch, not a retry, performance measurement, or timeout extension.
    QEventLoop loop;
    QTimer::singleShot(15, &loop, &QEventLoop::quit);
    loop.exec();
  };
  const auto key = [&](const int code, const bool cleanup = false) {
    // Reserve four ESC events and 100 ms of this same budget for cleanup.
    if (!owner || keyCount >= maximumKeyEvents - (cleanup ? 0 : 4) ||
        deadline.elapsed() >= perCaseBudgetMs - (cleanup ? 0 : 100)) {
      fail("bounded menu key-event budget exhausted or owned window destroyed");
      return false;
    }
    QWidget *receiver = QApplication::activePopupWidget();
    if (!receiver) receiver = QApplication::focusWidget();
    auto ownership = widgetOwnership(receiver);
    ownership["key"] = code;
    ownership["cleanup"] = cleanup;
    receiverChecks.append(ownership);
    if (!isOwned(receiver)) {
      fail("keyboard receiver is outside the original owned window/menu hierarchy");
      return false;
    }
    QPointer<QWidget> target(receiver);
    // Qt 6.8.3 QKeyEvent::modifiers() XORs AltModifier for Key_Alt.
    // QApplication::notify passes that accessor into a newly constructed
    // ShortcutOverride (a second XOR). Thus AltModifier here makes the normal
    // derived ShortcutOverride expose AltModifier; the press exposes NoModifier.
    // The release also stores AltModifier so its accessor exposes NoModifier.
    // These are explicitly synthetic Qt events, not physical/Win32 key evidence.
    const auto constructionModifiers = code == Qt::Key_Alt ? Qt::AltModifier : Qt::NoModifier;
    QKeyEvent press(QEvent::KeyPress, code, constructionModifiers);
    keyObserver.helperDispatch = true;
    keyObserver.dispatchKey = code;
    keyObserver.dispatchPhase = QStringLiteral("press");
    QCoreApplication::sendEvent(target.data(), &press);
    QJsonValue releaseModifiers(QJsonValue::Null);
    if (target) {
      QKeyEvent release(QEvent::KeyRelease, code, constructionModifiers);
      keyObserver.dispatchPhase = QStringLiteral("release");
      QCoreApplication::sendEvent(target.data(), &release);
      releaseModifiers = int(release.modifiers());
    }
    keyObserver.helperDispatch = false;
    keyObserver.dispatchPhase.clear();
    ++keyCount;
    settle();
    keys.append(QJsonObject{{"key", code}, {"receiver", target ? target->objectName() : QString()},
                            {"pressConstructionModifiers", int(constructionModifiers)},
                            {"releaseConstructionModifiers", int(constructionModifiers)},
                            {"pressAccessorModifiers", int(press.modifiers())},
                            {"releaseAccessorModifiers", releaseModifiers},
                            {"topActive", actionRecord(bar->activeAction())},
                            {"popup", QApplication::activePopupWidget()
                                ? QString::fromLatin1(QApplication::activePopupWidget()->metaObject()->className())
                                : QString()}});
    return true;
  };
  const auto closeWithEscape = [&] {
    for (int index = 0; index < 4; ++index) {
      QWidget *popup = QApplication::activePopupWidget();
      if (popup && !isOwned(popup)) {
        out["rejectedPopupOwnership"] = widgetOwnership(popup);
        fail("foreign popup appeared; it is not closed by this helper");
        break;
      }
      if (!popup && !bar->hasFocus()) break;
      if (!key(Qt::Key_Escape, true)) break;
    }
    return !QApplication::activePopupWidget() && !path.first()->isVisible() &&
        (path.size() == 1 || !path.last()->isVisible());
  };

  bool entryObserved = false;
  if (altRoute) {
    const bool altSent = key(Qt::Key_Alt);
    const bool naturalBarFocus = altSent && keyObserver.altReleaseObserved && bar->hasFocus() &&
        QApplication::focusWidget() == bar && bar->activeAction() &&
        bar->actions().contains(bar->activeAction()) && !QApplication::activePopupWidget();
    out["altReleaseNaturallyFocusedMenuBar"] = naturalBarFocus;
    entryObserved = naturalBarFocus && keyObserver.altPressObserved &&
        keyObserver.altShortcutOverrideObserved && keyObserver.droppedEvents == 0;
    out["altEntryObserved"] = entryObserved;
  } else {
    const bool f10Sent = key(Qt::Key_F10);
    const bool f10Observed = f10Sent && (bar->hasFocus() ||
        (isOwned(QApplication::activePopupWidget()) && bar->activeAction()));
    out["f10Observed"] = f10Observed;
    entryObserved = f10Observed;
  }
  if (!entryObserved) {
    closeWithEscape();
    out["status"] = altRoute ? QStringLiteral("NOT_RUN_ALT_ENTRY_NOT_OBSERVED")
                             : QStringLiteral("NOT_RUN_F10_NOT_OBSERVED");
    out["errors"] = errors;
    out["keys"] = keys;
    out["receiverOwnershipChecks"] = receiverChecks;
    out["triggeredActions"] = triggeredActions;
    const QJsonObject after = owner ? publicSnapshot() : QJsonObject{};
    out["publicSnapshotAfter"] = after;
    out["publicStateUnchanged"] = owner && after == before;
    recordObservedKeys();
    return result;
  }

  QMenu *top = path.first();
  for (qsizetype index = 0; bar->activeAction() != top->menuAction() &&
       index <= bar->actions().size(); ++index) if (!key(Qt::Key_Right)) break;
  if (bar->activeAction() != top->menuAction()) fail("right-arrow traversal did not reach the actual top menu");
  if (bar->activeAction() == top->menuAction() && !top->isVisible()) key(Qt::Key_Down);
  if (!top->isVisible() || QApplication::activePopupWidget() != top)
    fail("Down did not expose the actual top-menu popup");
  else pathRecords.append(QJsonObject{{"id", top->objectName()}, {"title", top->title()},
                                      {"visible", true}});

  // Select a submenu only with actual Down/Right input; never setActiveAction.
  if (errors.isEmpty() && path.size() == 2) {
    QMenu *submenu = path.last();
    for (qsizetype index = 0; top->activeAction() != submenu->menuAction() &&
         index <= top->actions().size(); ++index) if (!key(Qt::Key_Down)) break;
    if (top->activeAction() != submenu->menuAction()) fail("Down traversal did not reach the actual submenu entry");
    else key(Qt::Key_Right);
    if (!submenu->isVisible() || QApplication::activePopupWidget() != submenu)
      fail("Right did not expose the actual submenu popup");
    else pathRecords.append(QJsonObject{{"id", submenu->objectName()}, {"title", submenu->title()},
                                        {"visible", true}});
  }

  QMenu *leaf = path.last();
  if (errors.isEmpty()) for (const auto &expected : expectations) {
    QJsonObject item{{"action", actionRecord(expected.action)}, {"expectedName", expected.name},
                     {"expectedDisabled", expected.disabled},
                     {"expectedExplanationFragments", QJsonArray::fromStringList(expected.explanationFragments)}};
    if (expected.checked) item["expectedChecked"] = *expected.checked;
    if (expected.action->isEnabled() == expected.disabled)
      fail("actual action enabled state contradicts the independent case expectation");
    // Route by public object identity, with public action geometry as a recorded
    // fallback. Do not use the expected accessible name to locate its own oracle.
    QAccessibleInterface *matched = nullptr;
    QString matchBasis;
    auto *menuInterface = QAccessible::queryAccessibleInterface(leaf);
    const QRect actionRect(leaf->mapToGlobal(leaf->actionGeometry(expected.action).topLeft()),
                           leaf->actionGeometry(expected.action).size());
    if (menuInterface && menuInterface->isValid()) {
      for (int index = 0; index < menuInterface->childCount(); ++index) {
        auto *child = menuInterface->child(index);
        if (!child || !child->isValid() || child->role() != QAccessible::MenuItem) continue;
        if (child->object() == expected.action) { matched = child; matchBasis = QStringLiteral("object-identity"); break; }
        if (!matched && actionRect.isValid() && actionRect.contains(child->rect().center()) &&
            child->rect().contains(actionRect.center())) {
          matched = child;
          matchBasis = QStringLiteral("public-action-geometry");
        }
      }
    }
    if (!matched) {
      fail("actual visible menu item has no identifiable QAccessible MenuItem interface");
      item["accessibleInterfaceFound"] = false;
    } else {
      const QString name = matched->text(QAccessible::Name);
      const QString description = matched->text(QAccessible::Description);
      const QString help = matched->text(QAccessible::Help);
      const auto state = matched->state();
      item["accessibleInterfaceFound"] = true;
      item["matchBasis"] = matchBasis;
      item["observedName"] = name;
      item["observedDescription"] = description;
      item["observedHelp"] = help;
      item["observedDisabled"] = bool(state.disabled);
      item["observedChecked"] = bool(state.checked);
      item["observedRole"] = int(matched->role());
      if (name != expected.name) fail("actual menu-item accessible Name differs from independent localized expectation");
      if (bool(state.disabled) != expected.disabled) fail("assistive disabled state differs from independent expectation");
      if (expected.checked && bool(state.checked) != *expected.checked) fail("assistive checked state differs from independent expectation");
      const QString explanation = description + QLatin1Char('\n') + help;
      for (const QString &fragment : expected.explanationFragments)
        if (fragment.isEmpty() || !explanation.contains(fragment))
          fail("actual assistive explanation lacks an independently specified schedule/disabled-reason fragment");
    }
    // Disabled actions are inspected as disabled, not sent Return/Enter. Enabled
    // leaf actions must be reached by the actual arrow chain without activation.
    if (!expected.disabled) {
      for (qsizetype index = 0; leaf->activeAction() != expected.action &&
           index <= leaf->actions().size(); ++index) if (!key(Qt::Key_Down)) break;
      const bool reached = leaf->activeAction() == expected.action;
      item["arrowReached"] = reached;
      if (!reached) fail("actual Down traversal did not reach the expected enabled menu item");
    } else item["arrowNavigationVerdict"] = QStringLiteral("disabled item not activated; no Return/Enter sent");
    itemRecords.append(item);
  }
  const bool escaped = closeWithEscape();
  const bool focusRestored = originalFocus && QApplication::focusWidget() == originalFocus.data();
  const QJsonObject after = owner ? publicSnapshot() : QJsonObject{};
  const bool unchanged = owner && after == before;
  if (!escaped) fail("ESC did not close every actual menu popup");
  if (!focusRestored) fail("ESC did not naturally return focus to the original owned control");
  if (!triggeredActions.isEmpty()) fail("menu traversal unexpectedly triggered a real action");
  if (!unchanged) fail("menu observation changed the public business-state snapshot");
  if (altRoute && keyObserver.droppedEvents)
    fail("passive Alt keyboard event evidence exceeded its fixed recording bound");
  out["path"] = pathRecords;
  out["items"] = itemRecords;
  out["keys"] = keys;
  out["receiverOwnershipChecks"] = receiverChecks;
  out["keyCount"] = keyCount;
  out["elapsedMs"] = deadline.elapsed();
  out["triggeredActions"] = triggeredActions;
  out["escClosedPopups"] = escaped;
  out["originalFocusId"] = originalFocus ? originalFocus->objectName() : QString();
  out["finalFocusId"] = QApplication::focusWidget() ? QApplication::focusWidget()->objectName() : QString();
  out["focusNaturallyRestored"] = focusRestored;
  out["publicStateUnchanged"] = unchanged;
  out["publicSnapshotAfter"] = after;
  out["errors"] = errors;
  recordObservedKeys();
  result.passed = errors.isEmpty();
  out["status"] = result.passed ? QStringLiteral("PASS") : QStringLiteral("FAIL");
  return result;
}

// Public QObject/widget/domain observers only. No private MainWindow members,
// State objects, changing renderer counters, wall-clock samples or setters.
inline QJsonObject menuKeyboardPublicSnapshot(QMainWindow &window) {
  auto *workspace = window.findChild<WorkspaceDocument *>();
  auto *viewport = window.findChild<NativeViewport *>();
  auto *supervisor = window.findChild<ProcessSupervisor *>();
  auto *tasks = window.findChild<QTableWidget *>(QStringLiteral("taskRecords"));
  QJsonObject value{{"locale", AppLanguage::current()}, {"savedLocale", AppLanguage::saved()},
                    {"qtKeyboardModifiers", int(QGuiApplication::keyboardModifiers())},
                    {"themeMode", int(AppTheme::currentThemeMode())},
                    {"theme", int(AppTheme::currentTheme())}, {"windowTitle", window.windowTitle()},
                    {"workspaceFound", bool(workspace)}, {"viewportFound", bool(viewport)},
                    {"supervisorFound", bool(supervisor)}, {"taskTableFound", bool(tasks)}};
  if (workspace) value["workspace"] = QJsonObject{
      {"hasProject", workspace->hasProject()}, {"untitled", workspace->isUntitled()},
      {"modified", workspace->isModified()}, {"rootPath", workspace->rootPath()},
      {"projectFilePath", workspace->projectFilePath()}, {"datasetPath", workspace->datasetPath()},
      {"cameraIdentity", workspace->cameraDatasetIdentity()}, {"imageCount", double(workspace->imageCount())},
      {"sceneCollection", workspace->sceneCollectionJson()},
      {"workflowArtifacts", workspace->workflowArtifacts()}};
  if (viewport) {
    const auto target = viewport->viewTarget();
    const auto orbit = viewport->viewOrbitAngles();
    const auto translation = viewport->modelTranslation();
    const auto rotation = viewport->modelRotation();
    const auto scale = viewport->modelScale();
    value["viewport"] = QJsonObject{
        {"sceneObjectCount", double(viewport->sceneObjectCount())},
        {"activeSceneId", viewport->activeSceneId()},
        {"selectedSceneIds", QJsonArray::fromStringList(viewport->selectedSceneIds())},
        {"modelSelected", viewport->modelSelected()}, {"editToolsLocked", viewport->editToolsLocked()},
        {"target", QJsonArray{target.x(), target.y(), target.z()}},
        {"distance", viewport->viewDistance()},
        {"orbit", QJsonArray{orbit.yawDegrees, orbit.pitchDegrees, orbit.rollDegrees}},
        {"orthographic", viewport->orthographicProjection()},
        {"modelTranslation", QJsonArray{translation.x(), translation.y(), translation.z()}},
        {"modelRotation", QJsonArray{rotation.scalar(), rotation.x(), rotation.y(), rotation.z()}},
        {"modelScale", QJsonArray{scale.x(), scale.y(), scale.z()}},
        {"processingPreviewVisible", viewport->processingPreviewVisible()},
        {"processingPreviewPaused", viewport->processingPreviewPaused()}};
  }
  if (supervisor) value["task"] = QJsonObject{{"running", supervisor->isRunning()},
      {"activeTask", supervisor->activeTask()}, {"stopRequested", supervisor->wasStopRequested()}};
  if (tasks) value["taskRows"] = tasks->rowCount();
  return value;
}

// Opt-in early-return adapter for the three dedicated CTests. The normal
// native_language matrix never calls this function or gains a new test phase.
inline bool runOwnedMenuKeyboardAcceptance(
    QMainWindow &window, const std::function<QJsonObject()> &publicSnapshot) {
  const QString root = qApp->property("gswUiFormEvidenceProcessRoot").toString();
  const QString base = qEnvironmentVariable("GSW_UI_SURFACE_EVIDENCE");
  const QString settings = qApp->property("gswSmokeSettingsDirectory").toString();
  const QString requestedEntryRoute = qEnvironmentVariable("GSW_UI_MENU_ENTRY_METHOD");
  const QString entryRoute = requestedEntryRoute.isEmpty() ? QStringLiteral("qt_f10") : requestedEntryRoute;
  if (qEnvironmentVariable("GSW_UI_MENU_KEYBOARD_ACCEPTANCE") != QStringLiteral("1") ||
      qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QApplication::platformName() != QStringLiteral("windows") || !publicSnapshot ||
      !QDir::isAbsolutePath(base) || !QFileInfo(base).isDir() ||
      !QDir::isAbsolutePath(root) || !QFileInfo(root).isDir() ||
      !QDir::isAbsolutePath(settings) || !QFileInfo(settings).isDir() ||
      !qApp->findChild<QObject *>(QStringLiteral("gswUiSurfaceEvidenceAdapter"))) {
    qCritical() << "MENU_KEYBOARD_ACCEPTANCE: isolated remote native evidence/settings adapter required; NOT_RUN";
    return false;
  }
  const QString locale = AppLanguage::current();
  const int localeIndex = QStringList{QStringLiteral("zh_CN"), QStringLiteral("en_US"),
                                     QStringLiteral("ja_JP")}.indexOf(locale);
  const QJsonObject initial = publicSnapshot();
  auto *workspace = window.findChild<WorkspaceDocument *>();
  auto *viewport = window.findChild<NativeViewport *>();
  auto *supervisor = window.findChild<ProcessSupervisor *>();
  auto *tasks = window.findChild<QTableWidget *>(QStringLiteral("taskRecords"));
  auto *bar = window.findChild<QMenuBar *>();
  auto *language = window.findChild<QMenu *>(QStringLiteral("languageMenu"));
  auto *appearance = window.findChild<QMenu *>(QStringLiteral("appearanceMenu"));
  const auto action = [&](const char *id) { return window.findChild<QAction *>(QString::fromLatin1(id)); };
  auto *pause = action("pauseTrainingAction");
  auto *resume = action("resumeTrainingAction");
  auto *automatic = action("automaticThemeAction");
  auto *light = action("lightThemeAction");
  auto *dark = action("darkThemeAction");
  auto *about = action("aboutWorkbenchAction");
  auto *update = action("nativeUpdateAction");
  if (localeIndex < 0 || !workspace || !viewport || !supervisor || !tasks || !bar || !language ||
      !appearance || !pause || !resume || !automatic || !light || !dark || !about || !update ||
      !workspace->sceneObjects().isEmpty() || workspace->imageCount() != 0 ||
      viewport->sceneObjectCount() != 0 || supervisor->isRunning() || tasks->rowCount() != 0) {
    qCritical() << "MENU_KEYBOARD_ACCEPTANCE: the original owned empty/idle workbench is required; NOT_RUN";
    return false;
  }
  const auto topContaining = [&](QAction *entry) -> QMenu * {
    for (auto *top : bar->actions()) if (top->menu() && top->menu()->actions().contains(entry)) return top->menu();
    return nullptr;
  };
  const auto dataAction = [](QMenu *menu, const QString &data) -> QAction * {
    for (auto *entry : menu->actions()) if (entry->data().toString() == data) return entry;
    return nullptr;
  };
  // Independent fixed oracles: never copy current QAction text/tooltip into an
  // expected value. New disabled-reason semantics deliberately form RED gates;
  // no production strings or enabled state are assigned here.
  const QStringList automaticNames{QStringLiteral("自动（按时间）"), QStringLiteral("Automatic (Time-based)"), QStringLiteral("自動（時刻に応じて）")};
  const QStringList lightNames{QStringLiteral("浅色（白天）"), QStringLiteral("Light (Day)"), QStringLiteral("ライト（昼間）")};
  const QStringList darkNames{QStringLiteral("深色（黑夜）"), QStringLiteral("Dark (Night)"), QStringLiteral("ダーク（夜間）")};
  const QStringList pauseNames{QStringLiteral("暂停训练"), QStringLiteral("Pause Training"), QStringLiteral("学習を一時停止")};
  const QStringList resumeNames{QStringLiteral("继续训练"), QStringLiteral("Resume Training"), QStringLiteral("学習を再開")};
  const QStringList pauseReasons{QStringLiteral("没有正在运行的训练"), QStringLiteral("No training is running"), QStringLiteral("実行中の学習はありません")};
  const QStringList resumeReasons{QStringLiteral("没有可用的完整训练检查点"), QStringLiteral("No complete training checkpoint"), QStringLiteral("完全な学習チェックポイントがありません")};
  const QStringList aboutNames{QStringLiteral("关于 Gaussian Splatting Workbench"), QStringLiteral("About Gaussian Splatting Workbench"), QStringLiteral("Gaussian Splatting Workbench について")};
  const QStringList updateNames{QStringLiteral("软件更新…"), QStringLiteral("Software updates…"), QStringLiteral("ソフトウェアの更新…")};
  const UiThemeMode mode = AppTheme::currentThemeMode();
  struct Case { QString id; QList<QMenu *> path; QList<AccessibleMenuItemExpectation> items; };
  const QList<Case> cases{
      {QStringLiteral("language"), {topContaining(language->menuAction()), language}, {
          {dataAction(language, QStringLiteral("zh_CN")), QStringLiteral("简体中文"), false, locale == QStringLiteral("zh_CN"), {}},
          {dataAction(language, QStringLiteral("en_US")), QStringLiteral("English"), false, locale == QStringLiteral("en_US"), {}},
          {dataAction(language, QStringLiteral("ja_JP")), QStringLiteral("日本語"), false, locale == QStringLiteral("ja_JP"), {}}}},
      {QStringLiteral("appearance"), {topContaining(appearance->menuAction()), appearance}, {
          {automatic, automaticNames[localeIndex], false, mode == UiThemeMode::Automatic, {QStringLiteral("06:00"), QStringLiteral("18:00")}},
          {light, lightNames[localeIndex], false, mode == UiThemeMode::Light, {}},
          {dark, darkNames[localeIndex], false, mode == UiThemeMode::Dark, {}}}},
      {QStringLiteral("workflow-idle"), {topContaining(pause)}, {
          {pause, pauseNames[localeIndex], true, std::nullopt, {pauseReasons[localeIndex]}},
          {resume, resumeNames[localeIndex], true, std::nullopt, {resumeReasons[localeIndex]}}}},
      {QStringLiteral("help"), {topContaining(about)}, {
          {about, aboutNames[localeIndex], false, std::nullopt, {}},
          {update, updateNames[localeIndex], false, std::nullopt, {}}}}};
  QJsonArray records;
  bool accepted = true;
  for (const auto &test : cases) {
    if (!accepted) {
      QJsonObject skipped{{"caseId", test.id}, {"status", "NOT_RUN_PREVIOUS_CASE_NOT_ACCEPTED"}};
      if (entryRoute == QStringLiteral("qt_alt")) {
        skipped["entryRoute"] = entryRoute;
        skipped["f10Observed"] = QJsonValue(QJsonValue::Null);
      }
      records.append(skipped);
      continue;
    }
    const auto result = checkMenuKeyboardPath(window, test.id, test.path, test.items, publicSnapshot, entryRoute);
    qInfo().noquote() << "MENU_KEYBOARD_JSON" << QString::fromUtf8(QJsonDocument(result.evidence).toJson(QJsonDocument::Compact));
    records.append(result.evidence);
    accepted = result.passed;
  }
  const bool unchanged = publicSnapshot() == initial;
  QJsonObject receipt{{"schema", "gsw.menu_keyboard_acceptance/1"},
      {"sourceCommit", qEnvironmentVariable("GSW_UI_SOURCE_COMMIT")}, {"locale", locale},
      {"entryRoute", entryRoute}, {"inputKind", "synthetic Qt widget key events"},
      {"physicalOsKeyboardClaim", false}, {"nativeOsKeyboardClaim", false},
      {"fullAccessibilityClaim", false},
      {"expectedCaseCount", 4}, {"cases", records}, {"publicStateUnchanged", unchanged},
      {"normalLanguageMatrixExecuted", false}, {"newFixtureOrWorkerStarted", false},
      {"fullAcceptance", false}, {"status", accepted && unchanged ? "PASS" : "NOT_ACCEPTED"}};
  if (entryRoute == QStringLiteral("qt_alt")) receipt["f10Observed"] = QJsonValue(QJsonValue::Null);
  const QByteArray bytes = QJsonDocument(receipt).toJson(QJsonDocument::Indented);
  QFile file(QDir(root).filePath(entryRoute == QStringLiteral("qt_alt")
      ? QStringLiteral("menu-alt-keyboard-results.json") : QStringLiteral("menu-keyboard-results.json")));
  const bool saved = file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
      file.write(bytes) == bytes.size() && file.flush();
  if (!saved) qCritical() << "MENU_KEYBOARD_ACCEPTANCE: unable to preserve a new source-bound receipt";
  qInfo().noquote() << "MENU_KEYBOARD_ACCEPTANCE" << (accepted && unchanged && saved ? "PASS" : "NOT_ACCEPTED");
  return accepted && unchanged && saved;
}

} // namespace gsw::tests

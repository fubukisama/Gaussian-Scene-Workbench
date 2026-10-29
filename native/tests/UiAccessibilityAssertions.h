#pragma once

// QA-only public-interface assertions for an existing owned native window.
// No control is renamed, enabled, reparented or assigned an accessibility name.
#include <QAccessible>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>

namespace gsw::tests {

struct UiAccessibleFieldResult {
  bool passed = false;
  QJsonObject evidence;
};

// Use only after the existing real-window fixture has exposed and activated its
// own dialog. The caller supplies an independent translated name expectation and
// its public business-state snapshot. objectName remains a diagnostic machine ID,
// never a substitute for the user-facing QAccessible::Name.
inline UiAccessibleFieldResult checkAccessibleEditableField(
    QWidget &ownedWindow, QWidget &tabStart, QLineEdit &field,
    const QString &expectedName,
    const std::function<QJsonObject()> &publicSnapshot) {
  UiAccessibleFieldResult result;
  auto &out = result.evidence;
  out = QJsonObject{{"schema", "gsw.ui_accessible_editable_field/1"},
                   {"sourceCommit", qEnvironmentVariable("GSW_UI_SOURCE_COMMIT")},
                   {"platform", QApplication::platformName()},
                   {"windowId", ownedWindow.objectName()},
                   {"startId", tabStart.objectName()},
                   {"fieldId", field.objectName()},
                   {"expectedName", expectedName},
                   {"expectedRole", "EditableText"},
                   {"status", "NOT_RUN"}};
  QJsonArray errors;
  const auto fail = [&](const char *message) { errors.append(QString::fromLatin1(message)); };
  // This helper must not turn a static/local check into an application test.
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QApplication::platformName() != QStringLiteral("windows")) {
    fail("requires the existing remote Windows real-window fixture");
  }
  if (!ownedWindow.isWindow() || !ownedWindow.isVisible() ||
      !ownedWindow.isActiveWindow()) {
    fail("owned native window must already be visible and active");
  }
  if (!ownedWindow.isAncestorOf(&tabStart) || !ownedWindow.isAncestorOf(&field) ||
      tabStart.window() != &ownedWindow || field.window() != &ownedWindow) {
    fail("both actual controls must belong to this owned window");
  }
  if (!tabStart.isVisibleTo(&ownedWindow) || !tabStart.isEnabled() ||
      !field.isVisibleTo(&ownedWindow) || !field.isEnabled() || field.isReadOnly()) {
    fail("this slice requires the enabled editable field and enabled Tab starting control");
  }
  if (expectedName.trimmed().isEmpty() || !publicSnapshot)
    fail("independent expected name and public-state snapshot are required");
  if (!errors.isEmpty()) {
    out["status"] = QStringLiteral("PRECONDITION_FAILED");
    out["errors"] = errors;
    return result;
  }

  const QJsonObject before = publicSnapshot();
  const QPointer<QWidget> windowGuard(&ownedWindow);
  const QPointer<QWidget> startGuard(&tabStart);
  const QPointer<QLineEdit> fieldGuard(&field);
  const QPointer<QWidget> originalFocus(QApplication::focusWidget());
  const auto focusRecord = [](QWidget *widget) {
    return widget ? QJsonObject{{"id", widget->objectName()},
                               {"class", QString::fromLatin1(widget->metaObject()->className())}}
                  : QJsonObject{{"null", true}};
  };
  const auto focusIsInside = [&] {
    QWidget *focus = QApplication::focusWidget();
    return windowGuard && focus &&
        (focus == windowGuard.data() || windowGuard->isAncestorOf(focus)) &&
        focus->window() == windowGuard.data();
  };

  // Query Qt's actual assistive-technology interface. Qt owns the returned
  // interface cache; do not delete it or retain it across event processing.
  if (auto *accessible = QAccessible::queryAccessibleInterface(&field)) {
    out["interfaceValid"] = accessible->isValid();
    out["observedName"] = accessible->text(QAccessible::Name);
    out["observedDescription"] = accessible->text(QAccessible::Description);
    out["observedRole"] = int(accessible->role());
    out["expectedRoleValue"] = int(QAccessible::EditableText);
    const auto state = accessible->state();
    out["accessibleState"] = QJsonObject{{"disabled", bool(state.disabled)},
                                         {"focusable", bool(state.focusable)},
                                         {"invisible", bool(state.invisible)},
                                         {"readOnly", bool(state.readOnly)}};
    if (!accessible->isValid()) fail("actual QAccessible interface is invalid");
    if (accessible->text(QAccessible::Name) != expectedName)
      fail("actual QAccessible Name does not match the independent field label");
    if (accessible->role() != QAccessible::EditableText)
      fail("actual QAccessible role is not EditableText");
    if (state.disabled || !state.focusable || state.invisible || state.readOnly)
      fail("actual QAccessible state disagrees with this enabled editable-field case");
  } else {
    out["interfaceValid"] = false;
    fail("actual QAccessible interface is unavailable");
  }

  // Do not scan anonymous Qt child controls or suppress their focus semantics.
  // hasFocus() deliberately honors the semantic widget's actual focus proxy.
  // Tab is sent through QWidget's public key-event interface, never through the
  // protected focusNextPrevChild implementation or a hand-written focus order.
  tabStart.setFocus(Qt::TabFocusReason);
  QCoreApplication::processEvents(QEventLoop::AllEvents);
  QJsonArray traversal;
  QSet<QWidget *> seen;
  bool reached = false;
  int tabCount = 0;
  if (!windowGuard || !startGuard || !fieldGuard) {
    fail("owned window or tested controls were destroyed while focusing");
  } else if (!startGuard->hasFocus() || !focusIsInside()) {
    fail("actual starting control did not receive focus inside the owned window");
  } else {
    for (; tabCount < 64; ++tabCount) {
      QPointer<QWidget> focus(QApplication::focusWidget());
      traversal.append(focusRecord(focus.data()));
      if (fieldGuard && fieldGuard->hasFocus()) {
        reached = true;
        break;
      }
      if (!windowGuard || !startGuard || !fieldGuard || !focusIsInside()) {
        fail("Tab traversal lost its owned window or controls");
        break;
      }
      if (seen.contains(focus.data())) {
        fail("actual Tab traversal cycled before reaching the editable field");
        break;
      }
      seen.insert(focus.data());
      QKeyEvent press(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
      QCoreApplication::sendEvent(focus.data(), &press);
      if (auto *releaseTarget = QApplication::focusWidget()) {
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Tab, Qt::NoModifier);
        QCoreApplication::sendEvent(releaseTarget, &release);
      }
      QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
    // The 64th key can be the one that reaches the target.
    if (!reached && fieldGuard && fieldGuard->hasFocus() && focusIsInside()) reached = true;
    if (!reached && tabCount == 64) fail("editable field not reached within 64 actual Tab keys");
  }
  out["tabReachedField"] = reached;
  out["tabCount"] = tabCount;
  out["focusTraversal"] = traversal;
  out["finalFocus"] = focusRecord(QApplication::focusWidget());
  if (!reached && errors.isEmpty()) fail("editable field was not reached by actual Tab traversal");

  // Restore only this fixture's original focus; never activate another window.
  if (windowGuard && originalFocus && originalFocus->window() == windowGuard.data()) {
    originalFocus->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
  } else if (windowGuard && !originalFocus && focusIsInside()) {
    if (auto *focus = QApplication::focusWidget()) focus->clearFocus();
  }
  if (!windowGuard || !fieldGuard) {
    fail("owned fixture was destroyed before its public state could be compared");
    out["publicStateUnchanged"] = false;
  } else {
    const bool unchanged = publicSnapshot() == before;
    out["publicStateUnchanged"] = unchanged;
    if (!unchanged) fail("focus/accessibility observation changed the public business-state snapshot");
  }
  result.passed = errors.isEmpty() && reached;
  out["errors"] = errors;
  out["status"] = result.passed ? QStringLiteral("PASS") : QStringLiteral("FAIL");
  return result;
}

} // namespace gsw::tests

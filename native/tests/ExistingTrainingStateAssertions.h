#pragma once

// QA-only tracer for the existing owned CPU observer's public pause boundary.
// Reuse RuntimeDecouplingUiTests; do not create another lease, trainer or gallery.
#include "ExistingTrainingDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>

namespace gsw::tests {

struct PausedObserverPresentationResult {
  bool passed = false;
  QJsonObject evidence;
};

// Call after the real owned fixture has acknowledged pause and exited normally,
// from the existing observer-ended real-window matrix validator. Expectations
// come from that fixture and the test's independently specified locale strings,
// not from private State, production stateLabel(), or the combo's existing text.
inline PausedObserverPresentationResult checkPausedObserverPresentation(
    const ExistingTrainingDialog &observer, const QString &expectedJobId,
    const QString &expectedOutputName, const QString &expectedPausedLabel,
    const QString &expectedEndedLabel) {
  PausedObserverPresentationResult result;
  const QJsonObject before = observer.currentReport();
  const bool connectedBefore = observer.isConnected();
  const bool inspectionBusyBefore = observer.isBusy();
  auto *combo = observer.findChild<QComboBox *>(QStringLiteral("existingTrainingJobCombo"));
  auto *details = observer.findChild<QLabel *>(QStringLiteral("existingTrainingState"));
  const QString expectedCaption = expectedOutputName + QStringLiteral(" | ") + expectedJobId +
      QStringLiteral(" | ") + expectedPausedLabel + QStringLiteral(" | ") + expectedEndedLabel;
  auto &out = result.evidence;
  out = QJsonObject{{"schema", "gsw.paused_observer_presentation/1"},
                   {"sourceCommit", qEnvironmentVariable("GSW_UI_SOURCE_COMMIT")},
                   {"platform", QApplication::platformName()},
                   {"windowId", observer.objectName()},
                   {"expectedJobId", expectedJobId},
                   {"expectedOutputName", expectedOutputName},
                   {"expectedPausedLabel", expectedPausedLabel},
                   {"expectedEndedLabel", expectedEndedLabel},
                   {"reportJobId", before.value(QStringLiteral("jobId"))},
                   {"recordedState", before.value(QStringLiteral("recordedState"))},
                   {"liveness", before.value(QStringLiteral("liveness"))},
                   {"connectedBefore", connectedBefore},
                   {"inspectionBusyBefore", inspectionBusyBefore},
                   {"currentReport", before},
                   {"expectedCaption", expectedCaption},
                   {"status", "NOT_RUN"}};
  QJsonArray errors;
  const auto fail = [&](const char *message) { errors.append(QString::fromLatin1(message)); };
  if (qEnvironmentVariable("GITHUB_ACTIONS") != QStringLiteral("true") ||
      QApplication::platformName() != QStringLiteral("windows") || !observer.isVisible())
    fail("requires the existing remote Windows observer-ended real-window fixture");
  // The owned fixture already acknowledged pause and exited normally before
  // this read-only presentation matrix. Its connected observer continues its
  // normal two-second inspect polling: a transient helper is not a new training
  // state and must not invalidate the last accepted terminal report. Observe
  // that activity explicitly; never stop it, wait/retry for idle, or change UI.
  if (!connectedBefore ||
      before.value(QStringLiteral("jobId")).toString() != expectedJobId ||
      before.value(QStringLiteral("recordedState")) != QStringLiteral("paused") ||
      before.value(QStringLiteral("liveness")) != QStringLiteral("ended"))
    fail("owned observer has not reached the expected acknowledged paused/ended boundary");
  if (expectedJobId.isEmpty() || expectedOutputName.isEmpty() ||
      expectedPausedLabel.isEmpty() || expectedEndedLabel.isEmpty())
    fail("independent fixture identity, output name and localized state labels are required");
  if (!combo || !details) fail("actual observer combo or state label is unavailable");
  if (!errors.isEmpty()) {
    out["status"] = QStringLiteral("PRECONDITION_FAILED");
    out["errors"] = errors;
    return result;
  }

  // currentText/ToolTipRole retain the complete text even when the narrow
  // widget visually elides it. Do not mistake ordinary clipping for this bug.
  const QString caption = combo->currentText();
  const QString tooltip = combo->currentData(Qt::ToolTipRole).toString();
  const QString selectedId = combo->currentData().toString();
  out["observedCaption"] = caption;
  out["observedToolTip"] = tooltip;
  out["selectedJobId"] = selectedId;
  out["detailsText"] = details->text();
  out["comboEnabled"] = combo->isEnabled();
  if (selectedId != expectedJobId) fail("selected combo identity differs from the attached owned job");
  if (caption != expectedCaption)
    fail("selected job caption contradicts the acknowledged paused/ended report");
  if (tooltip != expectedCaption)
    fail("selected job tooltip contradicts the acknowledged paused/ended report");
  if (!details->text().contains(expectedPausedLabel) || !details->text().contains(expectedEndedLabel))
    fail("current observer details do not expose the expected paused/ended state");
  const QJsonObject after = observer.currentReport();
  out["afterCurrentReport"] = after;
  out["connectedAfter"] = observer.isConnected();
  out["inspectionBusyAfter"] = observer.isBusy();
  const bool unchanged = after == before && observer.isConnected() &&
      combo->currentData().toString() == selectedId;
  out["publicStateUnchanged"] = unchanged;
  if (!unchanged) fail("read-only presentation observation changed public report or connection identity");
  result.passed = errors.isEmpty();
  out["errors"] = errors;
  out["status"] = result.passed ? QStringLiteral("PASS") : QStringLiteral("FAIL");
  return result;
}

} // namespace gsw::tests

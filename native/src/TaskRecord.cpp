#include "TaskRecord.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QScrollBar>
#include <QStringList>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>

#include <algorithm>

namespace gsw {
namespace {
QString absoluteDirectory(const QString &path) {
  return path.trimmed().isEmpty() ? QString()
      : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString displayTime(const QDateTime &time, const char *missing) {
  return time.isValid() ? time.toString(Qt::ISODateWithMs)
                       : QCoreApplication::translate("Workbench", missing);
}
} // namespace

TaskRecord::TaskRecord(const QString &name, const QString &outputDirectory, QObject *parent)
    : QObject(parent), mName(name), mStartedAt(QDateTime::currentDateTime()),
      mOutputDirectory(absoluteDirectory(outputDirectory)) {
  refreshOutputIdentity();
}

void TaskRecord::assertGuiThread() const {
  Q_ASSERT(QThread::currentThread() == thread());
}

void TaskRecord::setName(const QString &name) {
  assertGuiThread();
  if (mName == name) return;
  mName = name;
  emit changed();
}

void TaskRecord::setState(const QString &state) {
  assertGuiThread();
  refreshOutputIdentity();
  if (mState == state) return;
  mState = state;
  emit changed();
}

void TaskRecord::setOutputDirectory(const QString &directory) {
  assertGuiThread();
  const QString normalized = absoluteDirectory(directory);
  if (mOutputDirectory == normalized) return;
  mOutputDirectory = normalized;
  mCanonicalOutputDirectory.clear();
  mCanonicalAncestors.clear();
  refreshOutputIdentity();
  emit changed();
}

void TaskRecord::refreshOutputIdentity() {
  if (mOutputDirectory.isEmpty() || !mCanonicalOutputDirectory.isEmpty()) return;
  mCanonicalOutputDirectory = QFileInfo(mOutputDirectory).canonicalFilePath();
  if (mCanonicalOutputDirectory.isEmpty()) return;
  QDir ancestor(mOutputDirectory);
  do {
    const QString lexical = QDir::cleanPath(ancestor.absolutePath());
    const QString canonical = QFileInfo(lexical).canonicalFilePath();
    if (!canonical.isEmpty()) mCanonicalAncestors.insert(lexical, canonical);
  } while (ancestor.cdUp());
}

void TaskRecord::finish(const QString &state) {
  assertGuiThread();
  refreshOutputIdentity();
  const bool stateChanged = mState != state;
  mState = state;
  if (!mFinishedAt.isValid()) {
    mFinishedAt = QDateTime::currentDateTime();
    emit changed();
  } else if (stateChanged) {
    emit changed();
  }
}

void TaskRecord::appendLog(const QString &text) {
  assertGuiThread();
  if (text.isEmpty()) return;
  refreshOutputIdentity();
  const qsizetype previousSize = mLogText.size();
  const QChar previousLast = mLogText.isEmpty() ? QChar() : mLogText.back();
  qsizetype removed = 0;
  if (text.size() >= MaximumLogCharacters) {
    // Do not temporarily retain a potentially very large incoming chunk.
    mLogText = text.right(MaximumLogCharacters);
    removed = previousSize + text.size() - mLogText.size();
    const qsizetype cut = text.size() - mLogText.size();
    const QChar preceding = cut > 0 ? text.at(cut - 1) : previousLast;
    if (!mLogText.isEmpty() && mLogText.at(0).isLowSurrogate() && preceding.isHighSurrogate()) {
      mLogText.remove(0, 1);
      ++removed;
    }
  } else {
    mLogText += text;
    removed = std::max<qsizetype>(0, mLogText.size() - MaximumLogCharacters);
    // Never leave a low surrogate at the start of the retained tail.
    if (removed > 0 && removed < mLogText.size() &&
        mLogText.at(removed).isLowSurrogate() && mLogText.at(removed - 1).isHighSurrogate()) {
      ++removed;
    }
    if (removed > 0) mLogText.remove(0, removed);
  }
  if (removed > 0) mLogTruncated = true;
  if (removed >= previousSize) {
    emit logChanged(mLogText, previousSize);
  } else {
    emit logChanged(text, removed);
  }
  emit changed();
}

bool TaskRecord::remapManagedOutputDirectory(const QString &oldRoot, const QString &newRoot) {
  assertGuiThread();
  if (mOutputDirectory.isEmpty() || oldRoot.isEmpty() || newRoot.isEmpty()) return false;
  const QString oldDirectory = absoluteDirectory(oldRoot);
  const QString newDirectory = absoluteDirectory(newRoot);
  refreshOutputIdentity();
  const QString relative = QDir(oldDirectory).relativeFilePath(mOutputDirectory);
  if (QDir::isAbsolutePath(relative) || relative == QStringLiteral("..") ||
      relative.startsWith(QStringLiteral("../"))) return false;
  // Do not mistake a linked external directory for managed project data.
  const QString liveOld = QFileInfo(oldDirectory).canonicalFilePath();
  const QString liveOutput = QFileInfo(mOutputDirectory).canonicalFilePath();
  const QString canonicalOld = liveOld.isEmpty() ? mCanonicalAncestors.value(oldDirectory) : liveOld;
  const QString canonicalOutput = liveOutput.isEmpty() ? mCanonicalOutputDirectory : liveOutput;
  if (!canonicalOld.isEmpty() && !canonicalOutput.isEmpty()) {
    const QString canonicalRelative = QDir(canonicalOld).relativeFilePath(canonicalOutput);
    if (QDir::isAbsolutePath(canonicalRelative) || canonicalRelative == QStringLiteral("..") ||
        canonicalRelative.startsWith(QStringLiteral("../"))) return false;
  }
  const QString remapped = QDir::cleanPath(QDir(newDirectory).filePath(relative));
  if (mOutputDirectory == remapped) return false;
  setOutputDirectory(remapped);
  return true;
}

QString TaskRecord::stateLabel() const {
  if (mState == QStringLiteral("starting")) return QCoreApplication::translate("Workbench", "正在启动");
  if (mState == QStringLiteral("running")) return QCoreApplication::translate("Workbench", "运行中");
  if (mState == QStringLiteral("done")) return QCoreApplication::translate("Workbench", "完成");
  if (mState == QStringLiteral("paused")) return QCoreApplication::translate("Workbench", "已暂停");
  if (mState == QStringLiteral("cancelled")) return QCoreApplication::translate("Workbench", "已取消");
  if (mState == QStringLiteral("failed")) return QCoreApplication::translate("Workbench", "失败");
  if (mState == QStringLiteral("queued")) return QCoreApplication::translate("Workbench", "排队");
  return mState;
}

QString TaskRecord::diagnosticSummary() const {
  const auto line = [](const char *label, const QString &value) {
    return QCoreApplication::translate("Workbench", label) + QStringLiteral(": ") + value;
  };
  QStringList lines{
      line("任务", mName), line("状态", stateLabel()),
      line("开始时间", displayTime(mStartedAt, "未设置")),
      line("结束时间", displayTime(mFinishedAt, "尚未结束")),
      line("输出目录", mOutputDirectory.isEmpty() ? QCoreApplication::translate("Workbench", "未设置")
                                                  : QDir::toNativeSeparators(mOutputDirectory)),
      QCoreApplication::translate("Workbench", "任务日志仅在本次会话保留；每项任务最多保留最近 512 Ki 字符。")};
  if (mLogTruncated) {
    lines.append(QCoreApplication::translate("Workbench", "日志已截断，仅保留最近 %1 个字符")
                     .arg(mLogText.size()));
  }
  return lines.join(QLatin1Char('\n'));
}

bool TaskRecord::exportLog(const QString &path, QString *errorMessage) const {
  assertGuiThread();
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) {
    if (errorMessage) *errorMessage = file.errorString();
    return false;
  }
  const QByteArray bytes = mLogText.toUtf8();
  if (file.write(bytes) != bytes.size() || !file.commit()) {
    if (errorMessage) *errorMessage = file.errorString();
    return false;
  }
  return true;
}

void TaskRecord::appendToLogView(QPlainTextEdit *view, const QString &text, const bool follow,
                               const qsizetype leadingCharactersToRemove) {
  if (!view) return;
  Q_ASSERT(QThread::currentThread() == view->thread());
  QTextCursor saved = view->textCursor();
  saved.setKeepPositionOnInsert(true);
  const int vertical = view->verticalScrollBar()->value();
  const int horizontal = view->horizontalScrollBar()->value();
  QTextCursor writer(view->document());
  if (leadingCharactersToRemove > 0) {
    writer.movePosition(QTextCursor::Start);
    // QTextCursor::Right counts cursor movements, which may span a complete
    // surrogate pair or grapheme. Document positions count UTF-16 code units.
    writer.setPosition(static_cast<int>(std::min<qsizetype>(leadingCharactersToRemove,
        view->document()->characterCount() - 1)), QTextCursor::KeepAnchor);
    writer.removeSelectedText();
  }
  writer.movePosition(QTextCursor::End);
  writer.insertText(text);
  if (follow) {
    view->moveCursor(QTextCursor::End);
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
  } else {
    view->setTextCursor(saved);
    view->verticalScrollBar()->setValue(vertical);
    view->horizontalScrollBar()->setValue(horizontal);
  }
}

} // namespace gsw

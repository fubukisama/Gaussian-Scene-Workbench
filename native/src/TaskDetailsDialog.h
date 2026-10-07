#pragma once

#include <QDialog>
#include <QSharedPointer>

class QCheckBox;
class QAction;
class QBoxLayout;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QResizeEvent;
class QScrollArea;

namespace gsw {
class TaskRecord;

class TaskDetailsDialog final : public QDialog {
  Q_OBJECT
public:
  explicit TaskDetailsDialog(const QSharedPointer<TaskRecord> &record, QWidget *parent = nullptr);

protected:
  void resizeEvent(QResizeEvent *event) override;
  void changeEvent(QEvent *event) override;

private:
  void refreshLayout();
  void refreshSummary();
  void appendRetainedLog(const QString &appendedText, qsizetype removedCharacters);
  void openOutputDirectory();
  void copySummary();
  void exportRetainedLog();
  void setStatus(const char *source, const QString &argument = {});

  QSharedPointer<TaskRecord> mRecord;
  QLabel *mName = nullptr;
  QLabel *mState = nullptr;
  QLabel *mStarted = nullptr;
  QLabel *mFinished = nullptr;
  QLabel *mOutput = nullptr;
  QLabel *mTruncation = nullptr;
  QLabel *mStatus = nullptr;
  QPlainTextEdit *mLogs = nullptr;
  QCheckBox *mFollow = nullptr;
  QAction *mOpenOutput = nullptr;
  QAction *mExportLog = nullptr;
  QScrollArea *mSummaryScroll = nullptr;
  QBoxLayout *mLogActions = nullptr;
  QString mRenderedRawLog;
  const char *mStatusSource = nullptr;
  QString mStatusArgument;
};
} // namespace gsw

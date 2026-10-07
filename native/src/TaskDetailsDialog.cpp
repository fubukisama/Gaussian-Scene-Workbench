#include "TaskDetailsDialog.h"
#include "AppLanguage.h"
#include "TaskRecord.h"
#include "WindowUi.h"

#include <QApplication>
#include <QAction>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTextCursor>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace gsw {
namespace {
QString displayedLog(QString text) {
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  return text;
}

QLabel *valueLabel(QWidget *parent, const char *name) {
  auto *label = new QLabel(parent);
  label->setObjectName(QString::fromLatin1(name));
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}
} // namespace

TaskDetailsDialog::TaskDetailsDialog(const QSharedPointer<TaskRecord> &record, QWidget *parent)
    : QDialog(parent), mRecord(record ? record : QSharedPointer<TaskRecord>::create()) {
  setObjectName(QStringLiteral("taskDetailsDialog"));
  setModal(true);
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("任务详情"));
  setMinimumSize(360, 280);
  resize(760, 560);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(10);
  mSummaryScroll = new QScrollArea(this);
  mSummaryScroll->setObjectName(QStringLiteral("taskDetailsSummaryScroll"));
  mSummaryScroll->setWidgetResizable(true);
  mSummaryScroll->setFrameShape(QFrame::NoFrame);
  mSummaryScroll->setMinimumSize(0, 0);
  mSummaryScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto *summaryBody = new QWidget(mSummaryScroll);
  summaryBody->setObjectName(QStringLiteral("taskDetailsSummaryBody"));
  auto *form = new QFormLayout(summaryBody);
  form->setContentsMargins(0, 0, 0, 0);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setHorizontalSpacing(12);
  form->setVerticalSpacing(6);
  mSummaryScroll->setWidget(summaryBody);
  layout->addWidget(mSummaryScroll);
  mName = valueLabel(this, "taskDetailsName");
  mState = valueLabel(this, "taskDetailsState");
  mStarted = valueLabel(this, "taskDetailsStarted");
  mFinished = valueLabel(this, "taskDetailsFinished");
  mOutput = valueLabel(this, "taskDetailsOutput");
  form->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("任务")), mName);
  form->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("状态")), mState);
  form->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("开始时间")), mStarted);
  form->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("结束时间")), mFinished);
  form->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("输出目录")), mOutput);
  auto *taskActions = new QToolBar(this);
  taskActions->setObjectName(QStringLiteral("taskDetailsActions"));
  taskActions->setMovable(false);
  taskActions->setFloatable(false);
  taskActions->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  taskActions->setMinimumWidth(0);
  mOpenOutput = AppLanguage::text(new QAction(style()->standardIcon(QStyle::SP_DirOpenIcon), {}, this),
                                AppLanguage::source("打开任务文件夹"));
  auto *copy = AppLanguage::text(new QAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView), {}, this),
                               AppLanguage::source("复制任务摘要"));
  mExportLog = AppLanguage::text(new QAction(style()->standardIcon(QStyle::SP_DialogSaveButton), {}, this),
                                AppLanguage::source("导出保留日志..."));
  mOpenOutput->setObjectName(QStringLiteral("taskDetailsOpenOutputAction"));
  copy->setObjectName(QStringLiteral("taskDetailsCopySummaryAction"));
  mExportLog->setObjectName(QStringLiteral("taskDetailsExportLogAction"));
  taskActions->addAction(mOpenOutput);
  taskActions->addAction(copy);
  taskActions->addAction(mExportLog);
  taskActions->widgetForAction(mOpenOutput)->setObjectName(QStringLiteral("taskDetailsOpenOutput"));
  taskActions->widgetForAction(copy)->setObjectName(QStringLiteral("taskDetailsCopySummary"));
  taskActions->widgetForAction(mExportLog)->setObjectName(QStringLiteral("taskDetailsExportLog"));
  layout->addWidget(taskActions);
  auto *retention = AppLanguage::text(new QLabel(this),
      AppLanguage::source("任务日志仅在本次会话保留；每项任务最多保留最近 512 Ki 字符。"));
  retention->setObjectName(QStringLiteral("taskDetailsRetention"));
  retention->setTextFormat(Qt::PlainText);
  retention->setWordWrap(true);
  retention->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  layout->addWidget(retention);
  mTruncation = valueLabel(this, "taskDetailsTruncation");
  layout->addWidget(mTruncation);
  mLogActions = new QBoxLayout(QBoxLayout::LeftToRight);
  mLogActions->addWidget(AppLanguage::text(new QLabel(this), AppLanguage::source("保留日志")));
  mLogActions->addStretch(1);
  mFollow = AppLanguage::text(new QCheckBox(this), AppLanguage::source("跟随最新日志"));
  mFollow->setObjectName(QStringLiteral("taskDetailsFollow"));
  mFollow->setChecked(true);
  mLogActions->addWidget(mFollow);
  layout->addLayout(mLogActions);
  mLogs = new QPlainTextEdit(this);
  mLogs->setObjectName(QStringLiteral("taskDetailsLogs"));
  mLogs->setReadOnly(true);
  mLogs->setUndoRedoEnabled(false);
  mLogs->setLineWrapMode(QPlainTextEdit::NoWrap);
  mLogs->setMinimumSize(0, 0);
  mLogs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
  mRenderedRawLog = mRecord->logText();
  mLogs->setPlainText(displayedLog(mRenderedRawLog));
  TaskRecord::appendToLogView(mLogs, {}, true);
  layout->addWidget(mLogs, 1);
  mStatus = valueLabel(this, "taskDetailsStatus");
  layout->addWidget(mStatus);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  auto *close = buttons->button(QDialogButtonBox::Close);
  close->setObjectName(QStringLiteral("taskDetailsClose"));
  AppLanguage::bind(close, "text", AppLanguage::source("关闭"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(mOpenOutput, &QAction::triggered, this, &TaskDetailsDialog::openOutputDirectory);
  connect(copy, &QAction::triggered, this, &TaskDetailsDialog::copySummary);
  connect(mExportLog, &QAction::triggered, this, &TaskDetailsDialog::exportRetainedLog);
  connect(mFollow, &QCheckBox::toggled, this, [this](const bool follow) {
    if (follow) TaskRecord::appendToLogView(mLogs, {}, true);
  });
  connect(mRecord.data(), &TaskRecord::changed, this, &TaskDetailsDialog::refreshSummary);
  connect(mRecord.data(), &TaskRecord::logChanged, this, &TaskDetailsDialog::appendRetainedLog);
  AppLanguage::onChanged(this, [this] { refreshSummary(); refreshLayout(); });
  refreshSummary();
  WindowUi::prepare(this);
  refreshLayout();
}

void TaskDetailsDialog::resizeEvent(QResizeEvent *event) {
  QDialog::resizeEvent(event);
  refreshLayout();
}

void TaskDetailsDialog::changeEvent(QEvent *event) {
  QDialog::changeEvent(event);
  if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
      event->type() == QEvent::LanguageChange) refreshLayout();
}

void TaskDetailsDialog::refreshLayout() {
  if (!mSummaryScroll || !mLogActions || !layout()) return;
  mSummaryScroll->setMaximumHeight(qMax(fontMetrics().height() * 2, height() / 3));
  int needed = 0;
  int count = 0;
  for (int index = 0; index < mLogActions->count(); ++index) {
    if (auto *widget = mLogActions->itemAt(index)->widget()) {
      needed += widget->sizeHint().width();
      ++count;
    }
  }
  needed += qMax(0, count - 1) * mLogActions->spacing();
  const auto margins = layout()->contentsMargins();
  const int available = width() - margins.left() - margins.right();
  mLogActions->setDirection(needed > available ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}

void TaskDetailsDialog::refreshSummary() {
  mName->setText(mRecord->name());
  mState->setText(mRecord->stateLabel());
  mStarted->setText(mRecord->startedAt().toString(Qt::ISODateWithMs));
  mFinished->setText(mRecord->finishedAt().isValid()
      ? mRecord->finishedAt().toString(Qt::ISODateWithMs)
      : QCoreApplication::translate("Workbench", "尚未结束"));
  mOutput->setText(mRecord->outputDirectory().isEmpty()
      ? QCoreApplication::translate("Workbench", "未设置")
      : QDir::toNativeSeparators(mRecord->outputDirectory()));
  mOpenOutput->setEnabled(!mRecord->outputDirectory().isEmpty() &&
                         QFileInfo(mRecord->outputDirectory()).isDir());
  mExportLog->setEnabled(!mRecord->logText().isEmpty());
  mTruncation->setVisible(mRecord->logTruncated());
  mTruncation->setText(mRecord->logTruncated()
      ? QCoreApplication::translate("Workbench", "日志已截断，仅保留最近 %1 个字符")
            .arg(mRecord->logText().size())
      : QString());
  mStatus->setVisible(mStatusSource != nullptr);
  if (mStatusSource) {
    QString message = QCoreApplication::translate("Workbench", mStatusSource);
    if (!mStatusArgument.isEmpty()) message = message.arg(mStatusArgument);
    mStatus->setText(message);
  }
}

void TaskDetailsDialog::appendRetainedLog(const QString &appendedText,
                                         const qsizetype removedCharacters) {
  const qsizetype displayedRemoval = displayedLog(mRenderedRawLog.left(removedCharacters)).size();
  const bool splitCrLf = (removedCharacters > 0 && removedCharacters < mRenderedRawLog.size() &&
                         mRenderedRawLog.at(removedCharacters - 1) == QLatin1Char('\r') &&
                         mRenderedRawLog.at(removedCharacters) == QLatin1Char('\n')) ||
      (removedCharacters < mRenderedRawLog.size() && mRenderedRawLog.endsWith(QLatin1Char('\r')) &&
       appendedText.startsWith(QLatin1Char('\n')));
  if (!splitCrLf) {
    TaskRecord::appendToLogView(mLogs, displayedLog(appendedText), mFollow->isChecked(), displayedRemoval);
  } else {
    // A CRLF pair can span process chunks or the retention cut. Keep the view
    // consistent with the raw retained text without rewriting the record.
    const int oldAnchor = mLogs->textCursor().anchor();
    const int oldPosition = mLogs->textCursor().position();
    const int vertical = mLogs->verticalScrollBar()->value();
    const int horizontal = mLogs->horizontalScrollBar()->value();
    mLogs->setPlainText(displayedLog(mRecord->logText()));
    if (mFollow->isChecked()) {
      TaskRecord::appendToLogView(mLogs, {}, true);
    } else {
      QTextCursor cursor(mLogs->document());
      const int limit = static_cast<int>(mLogs->toPlainText().size());
      cursor.setPosition(qBound(0, oldAnchor - static_cast<int>(displayedRemoval), limit));
      cursor.setPosition(qBound(0, oldPosition - static_cast<int>(displayedRemoval), limit), QTextCursor::KeepAnchor);
      mLogs->setTextCursor(cursor);
      mLogs->verticalScrollBar()->setValue(vertical);
      mLogs->horizontalScrollBar()->setValue(horizontal);
    }
  }
  mRenderedRawLog = mRecord->logText();
}

void TaskDetailsDialog::openOutputDirectory() {
  const QString path = mRecord->outputDirectory();
  if (path.isEmpty() || !QFileInfo(path).isDir()) {
    QMessageBox::warning(this, QCoreApplication::translate("Workbench", "任务详情"),
        QCoreApplication::translate("Workbench", "任务文件夹不存在：%1")
            .arg(QDir::toNativeSeparators(path)));
    refreshSummary();
    return;
  }
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
    QMessageBox::warning(this, QCoreApplication::translate("Workbench", "任务详情"),
        QCoreApplication::translate("Workbench", "无法打开任务文件夹：%1")
            .arg(QDir::toNativeSeparators(path)));
  }
}

void TaskDetailsDialog::copySummary() {
  QApplication::clipboard()->setText(mRecord->diagnosticSummary());
  setStatus(AppLanguage::source("任务摘要已复制"));
}

void TaskDetailsDialog::exportRetainedLog() {
  const QString directory = QFileInfo(mRecord->outputDirectory()).isDir()
      ? mRecord->outputDirectory() : QDir::currentPath();
  const QString path = QFileDialog::getSaveFileName(this,
      QCoreApplication::translate("Workbench", "保存日志"),
      QDir(directory).filePath(QStringLiteral("task-log.txt")),
      QCoreApplication::translate("Workbench", "文本文件 (*.txt);;所有文件 (*)"));
  if (path.isEmpty()) return;
  QString error;
  if (!mRecord->exportLog(path, &error)) {
    QMessageBox::warning(this, QCoreApplication::translate("Workbench", "无法导出日志"),
        QCoreApplication::translate("Workbench", "无法写入日志：%1").arg(error));
    return;
  }
  setStatus(AppLanguage::source("日志已导出：%1"), QDir::toNativeSeparators(path));
}

void TaskDetailsDialog::setStatus(const char *source, const QString &argument) {
  mStatusSource = source;
  mStatusArgument = argument;
  refreshSummary();
}
} // namespace gsw

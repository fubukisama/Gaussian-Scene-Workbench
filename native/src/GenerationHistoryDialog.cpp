#include "GenerationHistoryDialog.h"
#include "AppLanguage.h"
#include "WindowUi.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace gsw {
namespace {
QString stateLabel(const QString &state) {
  if (state == QStringLiteral("queued")) return QCoreApplication::translate("Workbench", "等待启动");
  if (state == QStringLiteral("running")) return QCoreApplication::translate("Workbench", "运行中");
  if (state == QStringLiteral("paused")) return QCoreApplication::translate("Workbench", "已暂停");
  if (state == QStringLiteral("completed")) return QCoreApplication::translate("Workbench", "已完成");
  if (state == QStringLiteral("cancelled")) return QCoreApplication::translate("Workbench", "已取消");
  if (state == QStringLiteral("failed")) return QCoreApplication::translate("Workbench", "失败");
  return QCoreApplication::translate("Workbench", "已中断；未自动恢复");
}

QString pipelineLabel(const GenerationExperiment &record) {
  if (record.pipeline == QStringLiteral("training"))
    return QCoreApplication::translate("Workbench", "训练") + QStringLiteral(" · ") + record.backend.toUpper();
  if (record.pipeline == QStringLiteral("mesh"))
    return QCoreApplication::translate("Workbench", "生成网格") + QStringLiteral(" · ") + record.backend;
  if (record.pipeline == QStringLiteral("reconstruction"))
    return QCoreApplication::translate("Workbench", "重建");
  if (record.pipeline == QStringLiteral("texturing"))
    return QCoreApplication::translate("Workbench", "纹理烘焙");
  return record.pipeline;
}
} // namespace

GenerationHistoryDialog::GenerationHistoryDialog(const QString &projectRoot, QWidget *parent)
    : QDialog(parent), mStore(projectRoot) {
  // Read only after every member has been constructed: mLoadError follows
  // mRecords in declaration order and cannot be used by its initializer.
  mRecords = mStore.records(&mLoadError);
  setObjectName(QStringLiteral("generationHistoryDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("实验与生成档案"));
  resize(960, 620);
  setMinimumSize(440, 320);
  auto *layout = new QVBoxLayout(this);
  auto *explanation = AppLanguage::text(new QLabel(this), AppLanguage::source(
      "各实验分别保留参数、结果与续训入口。此档案不是自动任务队列；打开工程不会启动训练。"));
  explanation->setTextFormat(Qt::PlainText);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto *splitter = new QSplitter(Qt::Vertical, this);
  mList = new QTreeWidget(splitter);
  mList->setObjectName(QStringLiteral("generationHistoryList"));
  mList->setRootIsDecorated(false);
  mList->setSelectionMode(QAbstractItemView::SingleSelection);
  mList->setColumnCount(4);
  mList->setUniformRowHeights(true);
  mList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  mList->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  mList->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  mList->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  auto *body = new QWidget(splitter);
  auto *detailsLayout = new QVBoxLayout(body);
  detailsLayout->setContentsMargins(0, 4, 0, 0);
  mDetails = new QLabel(body);
  mDetails->setObjectName(QStringLiteral("generationHistoryDetails"));
  mDetails->setTextFormat(Qt::PlainText);
  mDetails->setWordWrap(true);
  mDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
  detailsLayout->addWidget(mDetails);
  auto *parametersLabel = AppLanguage::text(new QLabel(body), AppLanguage::source("记录参数（原始标识）"));
  detailsLayout->addWidget(parametersLabel);
  mParameters = new QPlainTextEdit(body);
  mParameters->setObjectName(QStringLiteral("generationHistoryParameters"));
  mParameters->setReadOnly(true);
  mParameters->setLineWrapMode(QPlainTextEdit::NoWrap);
  detailsLayout->addWidget(mParameters, 1);
  splitter->addWidget(mList);
  splitter->addWidget(body);
  splitter->setStretchFactor(0, 1);
  splitter->setStretchFactor(1, 1);
  layout->addWidget(splitter, 1);
  mNotice = new QLabel(this);
  mNotice->setObjectName(QStringLiteral("generationHistoryNotice"));
  mNotice->setTextFormat(Qt::PlainText);
  mNotice->setWordWrap(true);
  layout->addWidget(mNotice);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  AppLanguage::text(buttons->button(QDialogButtonBox::Close), AppLanguage::source("关闭"));
  mViewResult = AppLanguage::text(new QPushButton(this), AppLanguage::source("查看所选成果"));
  mViewResult->setObjectName(QStringLiteral("generationHistoryViewResult"));
  mResume = AppLanguage::text(new QPushButton(this), AppLanguage::source("继续所选实验"));
  mResume->setObjectName(QStringLiteral("generationHistoryResume"));
  buttons->addButton(mViewResult, QDialogButtonBox::ActionRole);
  buttons->addButton(mResume, QDialogButtonBox::ActionRole);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(mList, &QTreeWidget::itemSelectionChanged, this, &GenerationHistoryDialog::refreshSelection);
  connect(mViewResult, &QPushButton::clicked, this, [this] { choose(Action::ViewResult); });
  connect(mResume, &QPushButton::clicked, this, [this] { choose(Action::Resume); });
  connect(mList, &QTreeWidget::itemDoubleClicked, this, [this] { choose(Action::ViewResult); });
  for (const auto &record : mRecords) {
    auto *item = new QTreeWidgetItem(mList);
    item->setData(0, Qt::UserRole, record.id);
    item->setText(0, record.displayName);
    item->setToolTip(0, record.displayName);
  }
  AppLanguage::onChanged(this, [this] { refreshPresentation(); });
  refreshPresentation();
  if (mList->topLevelItemCount()) mList->setCurrentItem(mList->topLevelItem(0));
  WindowUi::prepare(this);
}

GenerationExperiment GenerationHistoryDialog::selectedRecord() const {
  const auto *item = mList ? mList->currentItem() : nullptr;
  if (!item) return {};
  const QString id = item->data(0, Qt::UserRole).toString();
  for (const auto &record : mRecords) if (record.id == id) return record;
  return {};
}

void GenerationHistoryDialog::setResumeAllowed(bool allowed) {
  mResumeAllowed = allowed;
  refreshSelection();
}

void GenerationHistoryDialog::refreshPresentation() {
  mList->setHeaderLabels({QCoreApplication::translate("Workbench", "实验 / 任务"),
      QCoreApplication::translate("Workbench", "流程"),
      QCoreApplication::translate("Workbench", "记录状态"),
      QCoreApplication::translate("Workbench", "更新时间")});
  for (int index = 0; index < mList->topLevelItemCount(); ++index) {
    auto *item = mList->topLevelItem(index);
    const QString id = item->data(0, Qt::UserRole).toString();
    for (const auto &record : mRecords) {
      if (record.id != id) continue;
      item->setText(1, pipelineLabel(record));
      item->setText(2, stateLabel(record.status));
      item->setText(3, record.updatedAt.toLocalTime().toString(Qt::ISODate));
      break;
    }
  }
  refreshSelection();
}

void GenerationHistoryDialog::refreshSelection() {
  const auto record = selectedRecord();
  const bool selected = record.isValid();
  const bool resultAvailable = selected && !record.resultPath.isEmpty() &&
      QFileInfo(record.resultPath).isFile() && !QFileInfo(record.resultPath).isSymLink();
  mViewResult->setEnabled(resultAvailable);
  const QString parameterText = selected
      ? QString::fromUtf8(QJsonDocument(record.parameters).toJson(QJsonDocument::Indented)) : QString();
  // Language changes only affect presentation labels, not the user's read-only
  // parameter text selection or scroll position.
  if (mParameters->toPlainText() != parameterText) mParameters->setPlainText(parameterText);
  if (!selected) {
    mDetails->setText(QCoreApplication::translate("Workbench", "尚无实验档案"));
    mResume->setEnabled(false);
    mNotice->setText(mLoadError);
    mNotice->setVisible(!mLoadError.isEmpty());
    return;
  }
  const auto availability = mStore.resumeAvailability(record.id);
  mResume->setEnabled(mResumeAllowed && availability.available);
  mDetails->setText(QCoreApplication::translate("Workbench", "数据集：%1\n输出目录：%2\n成果：%3\n续训：%4")
      .arg(QDir::toNativeSeparators(record.datasetPath),
           QDir::toNativeSeparators(record.outputRoot),
           resultAvailable ? QDir::toNativeSeparators(record.resultPath)
                           : QCoreApplication::translate("Workbench", "成果文件不可用"),
           availability.reason));
  mNotice->setText(mLoadError);
  mNotice->setVisible(!mLoadError.isEmpty());
}

void GenerationHistoryDialog::choose(Action action) {
  if (action == Action::ViewResult && !mViewResult->isEnabled()) return;
  if (action == Action::Resume && !mResume->isEnabled()) return;
  mAction = action;
  accept();
}
} // namespace gsw

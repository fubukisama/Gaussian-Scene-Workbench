#include "AppLanguage.h"
#include <QCoreApplication>
#include "DatasetImportDialog.h"
#include "MultiItemList.h"
#include "WrappingCheckBox.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

namespace gsw {
namespace {

QString normalizedPath(const QString &path) {
  const QFileInfo info(path);
  QString normalized = info.canonicalFilePath();
  if (normalized.isEmpty()) {
    normalized = info.absoluteFilePath();
  }
  return QDir::cleanPath(normalized);
}

QString pathKey(const QString &path) {
  const QString normalized = QDir::fromNativeSeparators(path);
#ifdef Q_OS_WIN
  return normalized.toCaseFolded();
#else
  return normalized;
#endif
}

// Keep complete action captions without making narrow import windows wider.
// The list below remains its own scrollable view, not nested inside this body.
class SourceActions final : public QWidget {
public:
  SourceActions(const QList<QPushButton *> &buttons, QWidget *parent)
      : QWidget(parent), mButtons(buttons), mLayout(new QGridLayout(this)) {
    setProperty("gswLayoutContainer", true);
    mLayout->setContentsMargins(0, 0, 0, 0);
    mLayout->setSpacing(6);
    // The previous column arrangement must not freeze the minimum width after
    // maximizing, otherwise the body could never shrink enough to reflow.
    mLayout->setSizeConstraint(QLayout::SetNoConstraint);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    refreshLayout();
  }

  QSize sizeHint() const override {
    int preferredWidth = 0;
    for (auto *button : mButtons) preferredWidth += button->sizeHint().width();
    preferredWidth += (mButtons.size() - 1) * mLayout->spacing();
    return QSize(preferredWidth, heightForWidth(width()));
  }

  QSize minimumSizeHint() const override {
    int minimumWidth = 0;
    for (auto *button : mButtons) {
      minimumWidth = std::max(minimumWidth, button->minimumSizeHint().width());
    }
    return QSize(minimumWidth, heightForWidth(width()));
  }

  int heightForWidth(int width) const override {
    const int columns = columnsForWidth(width);
    int result = 0;
    for (int start = 0; start < mButtons.size(); start += columns) {
      int rowHeight = 0;
      for (int i = start; i < std::min(start + columns, int(mButtons.size())); ++i)
        rowHeight = std::max(rowHeight, mButtons.at(i)->sizeHint().height());
      result += rowHeight + (start ? mLayout->spacing() : 0);
    }
    return result;
  }

  void refreshLayout() {
    const int columns = columnsForWidth(width());
    bool changed = false;
    if (columns != mColumns) {
      while (auto *item = mLayout->takeAt(0)) delete item;
      for (int column = 0; column < mButtons.size(); ++column)
        mLayout->setColumnStretch(column, column < columns ? 1 : 0);
      for (int i = 0; i < mButtons.size(); ++i)
        mLayout->addWidget(mButtons.at(i), i / columns, i % columns);
      mColumns = columns;
      changed = true;
    }
    // A normal QGridLayout has no height-for-width contract of its own. Make
    // the current row height explicit so QWidgetItem cannot fall back to the
    // old single-row minimum and squeeze the reflowed buttons.
    const int requiredHeight = heightForWidth(width());
    if (minimumHeight() != requiredHeight) {
      setMinimumHeight(requiredHeight);
      changed = true;
    }
    if (changed) updateGeometry();
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    refreshLayout();
  }

  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange ||
        event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange ||
        event->type() == QEvent::LanguageChange) {
      // Child buttons settle their font/style size hints after propagation.
      // Coalesce those events and never queue a refresh from resize itself.
      if (!mRefreshQueued) {
        mRefreshQueued = true;
        QTimer::singleShot(0, this, [this]() {
          mRefreshQueued = false;
          refreshLayout();
          updateGeometry();
        });
      }
    }
  }

private:
  int columnsForWidth(int width) const {
    for (const int columns : {4, 2}) {
      int requiredWidth = (columns - 1) * mLayout->spacing();
      for (int column = 0; column < columns; ++column) {
        int columnWidth = 0;
        for (int i = column; i < mButtons.size(); i += columns)
          columnWidth = std::max(columnWidth, mButtons.at(i)->sizeHint().width());
        requiredWidth += columnWidth;
      }
      if (requiredWidth <= width) return columns;
    }
    return 1;
  }

  QList<QPushButton *> mButtons;
  QGridLayout *mLayout;
  int mColumns = 0;
  bool mRefreshQueued = false;
};

} // namespace

DatasetImportDialog::DatasetImportDialog(const QString &initialDirectory,
                                         const QString &suggestedSceneName,
                                         const QStringList &initialSourcePaths,
                                         const QString &projectRoot,
                                         const bool unsavedProject,
                                         QWidget *parent)
    : QDialog(parent),
      mInitialDirectory(normalizedPath(initialDirectory)) {
  setObjectName(QStringLiteral("datasetImportDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("添加照片与视频"));
  setModal(true);
  setMinimumSize(400, 260);
  resize(800, 640);

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(16, 14, 16, 14);
  rootLayout->setSpacing(12);

  // The setup area and source list scroll independently. The final action bar
  // never scrolls away, even when enlarged fonts leave little vertical space.
  auto *scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("dialogBodyScroll"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto *body = new QWidget(scroll);
  body->setObjectName(QStringLiteral("dialogBody"));
  auto *bodyLayout = new QVBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(12);
  scroll->setWidget(body);
  rootLayout->addWidget(scroll, 1);

  auto *introduction = new QLabel(
      unsavedProject
          ? QCoreApplication::translate("Workbench", "素材已选好。照片会复制到未命名工程，视频会按指定帧率抽帧；可先处理，稍后再自由选择工程保存位置。")
          : QCoreApplication::translate("Workbench", "素材已选好。照片会复制到当前工程的托管数据集，视频会按指定帧率抽帧。"),
      this);
  introduction->setObjectName(QStringLiteral("datasetImportIntroductionLabel"));
  introduction->setWordWrap(true);
  bodyLayout->addWidget(introduction);

  auto *projectPath = new QLabel(
      unsavedProject
          ? QCoreApplication::translate("Workbench", "当前工程：未命名工程（尚未保存，首次保存时可选择位置）")
          : QCoreApplication::translate("Workbench", "当前工程数据：%1")
                .arg(QDir::toNativeSeparators(projectRoot)),
      this);
  projectPath->setObjectName(QStringLiteral("datasetImportProjectPathLabel"));
  projectPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
  projectPath->setWordWrap(true);
  bodyLayout->addWidget(projectPath);

  auto *form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(9);

  mSceneName = new QLineEdit(suggestedSceneName, this);
  mSceneName->setMaxLength(std::numeric_limits<int>::max());
  mSceneName->setObjectName(QStringLiteral("datasetImportSceneEdit"));
  mSceneName->setClearButtonEnabled(true);
  AppLanguage::bind(mSceneName, "placeholderText", AppLanguage::source("支持任意文字、空格和符号"));
  AppLanguage::bind(mSceneName, "toolTip", AppLanguage::source("显示名称将完整保留；软件自动生成安全的存储目录名。"));
  form->addRow(QCoreApplication::translate("Workbench", "场景名称"), mSceneName);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mSceneName)), AppLanguage::source("场景名称"));

  mFramesPerSecond = new QDoubleSpinBox(this);
  mFramesPerSecond->setObjectName(QStringLiteral("datasetImportFpsSpin"));
  AppLanguage::bind(mFramesPerSecond, "accessibleName", AppLanguage::source("视频抽帧帧率"));
  mFramesPerSecond->setRange(0.2, 10.0);
  mFramesPerSecond->setSingleStep(0.2);
  mFramesPerSecond->setDecimals(1);
  mFramesPerSecond->setValue(2.0);
  mFramesPerSecond->setSuffix(QStringLiteral(" FPS"));
  mFramesPerSecond->setKeyboardTracking(false);
  form->addRow(QCoreApplication::translate("Workbench", "视频抽帧"), mFramesPerSecond);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mFramesPerSecond)), AppLanguage::source("视频抽帧"));

  mOverwrite = AppLanguage::text(new WrappingCheckBox(
      QCoreApplication::translate("Workbench", "覆盖同名托管数据集（开始前会再次确认）"), this), AppLanguage::source("覆盖同名托管数据集（开始前会再次确认）"));
  mOverwrite->setObjectName(QStringLiteral("datasetImportOverwriteCheck"));
  form->addRow(QString(), mOverwrite);
  bodyLayout->addLayout(form);

  auto *sourceTitle = AppLanguage::text(new QLabel(QCoreApplication::translate("Workbench", "媒体来源"), this), AppLanguage::source("媒体来源"));
  sourceTitle->setObjectName(QStringLiteral("sectionTitle"));
  bodyLayout->addWidget(sourceTitle);

  mSourceList = new QListWidget(this);
  mSourceList->setObjectName(QStringLiteral("datasetImportSourceList"));
  AppLanguage::bind(mSourceList, "accessibleName", AppLanguage::source("待导入媒体来源"));
  mSourceList->setAlternatingRowColors(true);
  mSourceList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  mSourceList->setMinimumHeight(72);
  rootLayout->addWidget(mSourceList, 1);

  auto *addFilesButton = AppLanguage::text(new QPushButton(QCoreApplication::translate("Workbench", "继续添加照片/视频..."), this), AppLanguage::source("继续添加照片/视频..."));
  addFilesButton->setObjectName(QStringLiteral("datasetImportAddFilesButton"));
  auto *addDirectoryButton = AppLanguage::text(new QPushButton(QCoreApplication::translate("Workbench", "继续添加目录..."), this), AppLanguage::source("继续添加目录..."));
  addDirectoryButton->setObjectName(
      QStringLiteral("datasetImportAddDirectoryButton"));
  mRemoveButton = AppLanguage::text(new QPushButton(QCoreApplication::translate("Workbench", "移除所选"), this), AppLanguage::source("移除所选"));
  mRemoveButton->setObjectName(QStringLiteral("datasetImportRemoveButton"));
  mClearButton = AppLanguage::text(new QPushButton(QCoreApplication::translate("Workbench", "清空"), this), AppLanguage::source("清空"));
  mClearButton->setObjectName(QStringLiteral("datasetImportClearButton"));
  auto *sourceButtons = new SourceActions(
      {addFilesButton, addDirectoryButton, mRemoveButton, mClearButton}, body);
  sourceButtons->setObjectName(QStringLiteral("datasetImportSourceActions"));
  bodyLayout->addWidget(sourceButtons);
  AppLanguage::onChanged(sourceButtons,
      [sourceButtons]() { sourceButtons->refreshLayout(); });
  new MultiItemList(mSourceList, AppLanguage::source("移除所选"),
      [this]() { removeSelected(); });
  // Existing remove/clear buttons stay in place; context menu and keyboard share their operation.

  mSummary = new QLabel(this);
  mSummary->setObjectName(QStringLiteral("datasetImportSummaryLabel"));
  mSummary->setWordWrap(true);
  bodyLayout->addWidget(mSummary);
  bodyLayout->addStretch(1);
  for (auto *label : body->findChildren<QLabel *>()) {
    if (label->wordWrap())
      label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  }

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  buttons->setObjectName(QStringLiteral("datasetImportButtonBox"));
  auto *cancelButton = buttons->button(QDialogButtonBox::Cancel);
  cancelButton->setObjectName(QStringLiteral("datasetImportCancelButton"));
  AppLanguage::text(cancelButton, AppLanguage::source("取消"));
  mImportButton =
      AppLanguage::text(buttons->addButton(QCoreApplication::translate("Workbench", "添加到工程"), QDialogButtonBox::AcceptRole), AppLanguage::source("添加到工程"));
  mImportButton->setObjectName(QStringLiteral("datasetImportStartButton"));
  mImportButton->setDefault(true);
  rootLayout->addWidget(buttons);

  connect(addFilesButton, &QPushButton::clicked, this,
          &DatasetImportDialog::addFiles);
  connect(addDirectoryButton, &QPushButton::clicked, this,
          &DatasetImportDialog::addDirectory);
  connect(mRemoveButton, &QPushButton::clicked, this,
          &DatasetImportDialog::removeSelected);
  connect(mClearButton, &QPushButton::clicked, this,
          &DatasetImportDialog::clearSources);
  connect(mSourceList, &QListWidget::itemSelectionChanged, this,
          &DatasetImportDialog::refreshSummary);
  connect(mSceneName, &QLineEdit::textChanged, this,
          &DatasetImportDialog::refreshSummary);
  connect(mOverwrite, &QCheckBox::toggled, this,
          &DatasetImportDialog::refreshSummary);
  connect(buttons, &QDialogButtonBox::accepted, this,
          &DatasetImportDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this,
          &DatasetImportDialog::reject);

  AppLanguage::onChanged(this, [this, introduction, projectPath, unsavedProject, projectRoot]() {
    introduction->setText(unsavedProject
        ? QCoreApplication::translate("Workbench", "素材已选好。照片会复制到未命名工程，视频会按指定帧率抽帧；可先处理，稍后再自由选择工程保存位置。")
        : QCoreApplication::translate("Workbench", "素材已选好。照片会复制到当前工程的托管数据集，视频会按指定帧率抽帧。"));
    projectPath->setText(unsavedProject
        ? QCoreApplication::translate("Workbench", "当前工程：未命名工程（尚未保存，首次保存时可选择位置）")
        : QCoreApplication::translate("Workbench", "当前工程数据：%1").arg(QDir::toNativeSeparators(projectRoot)));
    refreshSummary();
  });
  if (initialSourcePaths.isEmpty()) {
    refreshSummary();
  } else {
    appendSourcePaths(initialSourcePaths);
  }
}

DatasetImportRequest DatasetImportDialog::request() const {
  DatasetImportRequest result;
  result.sceneName = mSceneName->text();
  result.framesPerSecond = mFramesPerSecond->value();
  result.overwrite = mOverwrite->isChecked();
  result.sourcePaths.reserve(mSourceList->count());
  for (int index = 0; index < mSourceList->count(); ++index) {
    result.sourcePaths.append(
        mSourceList->item(index)->data(Qt::UserRole).toString());
  }
  return result;
}

const std::optional<DatasetImportPlan> &DatasetImportDialog::validatedPlan() const {
  return mValidatedPlan;
}

void DatasetImportDialog::accept() {
  const DatasetImportRequest importRequest = request();
  if (importRequest.sceneName.trimmed().isEmpty()) {
    QMessageBox::critical(this, QCoreApplication::translate("Workbench", "场景名称为空"),
                          QCoreApplication::translate("Workbench", "请输入托管数据集的场景名称。"));
    mSceneName->setFocus();
    return;
  }
  if (importRequest.sourcePaths.isEmpty()) {
    QMessageBox::critical(
        this, QCoreApplication::translate("Workbench", "尚未添加媒体"),
        QCoreApplication::translate("Workbench", "请添加照片、视频或包含媒体文件的目录。"));
    return;
  }

  QApplication::setOverrideCursor(Qt::WaitCursor);
  QString error;
  mValidatedPlan = DatasetImportPlan::create(importRequest, &error);
  QApplication::restoreOverrideCursor();
  if (!mValidatedPlan.has_value()) {
    const QString message = error == QStringLiteral("Invalid scene name")
                                ? QCoreApplication::translate("Workbench", "请输入名称，名称不能仅包含空白字符。")
                            : error == QStringLiteral("No supported image or video files were found")
                                ? QCoreApplication::translate("Workbench", "所选来源中没有找到支持的照片或视频文件。")
                                : error;
    QMessageBox::critical(this, QCoreApplication::translate("Workbench", "无法导入媒体"), message);
    return;
  }

  QDialog::accept();
}

void DatasetImportDialog::addFiles() {
  const QStringList paths = QFileDialog::getOpenFileNames(
      this, QCoreApplication::translate("Workbench", "选择照片或视频"), mInitialDirectory,
      QCoreApplication::translate("Workbench", "照片与视频 (*.jpg *.jpeg *.png *.bmp *.tif *.tiff *.webp *.mp4 *.mov *.avi *.mkv *.webm *.m4v);;"
          "照片 (*.jpg *.jpeg *.png *.bmp *.tif *.tiff *.webp);;"
          "视频 (*.mp4 *.mov *.avi *.mkv *.webm *.m4v)"));
  if (paths.isEmpty()) {
    return;
  }
  mInitialDirectory = QFileInfo(paths.constFirst()).absolutePath();
  appendSourcePaths(paths);
}

void DatasetImportDialog::addDirectory() {
  const QString path = QFileDialog::getExistingDirectory(
      this, QCoreApplication::translate("Workbench", "选择包含照片或视频的目录"), mInitialDirectory,
      QFileDialog::ShowDirsOnly);
  if (path.isEmpty()) {
    return;
  }
  mInitialDirectory = path;
  appendSourcePaths({path});
}

void DatasetImportDialog::removeSelected() {
  const QList<QListWidgetItem *> selected = mSourceList->selectedItems();
  for (QListWidgetItem *item : selected) {
    delete mSourceList->takeItem(mSourceList->row(item));
  }
  refreshSummary();
}

void DatasetImportDialog::clearSources() {
  mSourceList->clear();
  refreshSummary();
}

void DatasetImportDialog::appendSourcePaths(const QStringList &paths) {
  QSet<QString> existing;
  for (int index = 0; index < mSourceList->count(); ++index) {
    existing.insert(pathKey(
        mSourceList->item(index)->data(Qt::UserRole).toString()));
  }

  for (const QString &path : paths) {
    const QString normalized = normalizedPath(path);
    const QFileInfo info(normalized);
    const QString key = pathKey(normalized);
    if (!info.exists() || existing.contains(key)) {
      continue;
    }
    existing.insert(key);

    auto *item = new QListWidgetItem(
        style()->standardIcon(info.isDir() ? QStyle::SP_DirIcon
                                           : QStyle::SP_FileIcon),
        QDir::toNativeSeparators(normalized), mSourceList);
    item->setData(Qt::UserRole, normalized);
    item->setToolTip(QDir::toNativeSeparators(normalized));
  }
  refreshSummary();
}

void DatasetImportDialog::refreshSummary() {
  int fileCount = 0;
  int directoryCount = 0;
  for (int index = 0; index < mSourceList->count(); ++index) {
    const QFileInfo info(
        mSourceList->item(index)->data(Qt::UserRole).toString());
    if (info.isDir()) {
      ++directoryCount;
    } else {
      ++fileCount;
    }
  }

  if (mSourceList->count() == 0) {
    mSummary->setText(QCoreApplication::translate("Workbench", "尚未添加媒体来源。"));
  } else {
    QString text = QCoreApplication::translate("Workbench", "已添加 %1 个来源：%2 个文件、%3 个目录。目录会在开始导入时递归扫描。")
                       .arg(mSourceList->count())
                       .arg(fileCount)
                       .arg(directoryCount);
    if (mOverwrite->isChecked()) {
      text += QCoreApplication::translate("Workbench", " 将覆盖同名托管数据集。");
    }
    mSummary->setText(text);
  }

  mRemoveButton->setEnabled(!mSourceList->selectedItems().isEmpty());
  mClearButton->setEnabled(mSourceList->count() > 0);
  mImportButton->setEnabled(mSourceList->count() > 0 &&
                            !mSceneName->text().trimmed().isEmpty());
}

} // namespace gsw

#include "DatasetImportDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QStyle>
#include <QVBoxLayout>

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

} // namespace

DatasetImportDialog::DatasetImportDialog(const QString &initialDirectory,
                                         const QString &suggestedSceneName,
                                         const QStringList &initialSourcePaths,
                                         const QString &projectRoot,
                                         const bool createsProject,
                                         QWidget *parent)
    : QDialog(parent),
      mInitialDirectory(normalizedPath(initialDirectory)) {
  setObjectName(QStringLiteral("datasetImportDialog"));
  setWindowTitle(QStringLiteral("添加照片与视频"));
  setModal(true);
  setMinimumSize(640, 460);

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(16, 14, 16, 14);
  rootLayout->setSpacing(12);

  auto *introduction = new QLabel(
      createsProject
          ? QStringLiteral("素材已选好。开始后会在素材同盘自动创建工程；照片会复制到托管数据集，视频会按指定帧率抽帧。")
          : QStringLiteral("素材已选好。照片会复制到当前工程的托管数据集，视频会按指定帧率抽帧。"),
      this);
  introduction->setObjectName(QStringLiteral("datasetImportIntroductionLabel"));
  introduction->setWordWrap(true);
  rootLayout->addWidget(introduction);

  auto *projectPath = new QLabel(
      QStringLiteral("%1：%2")
          .arg(createsProject ? QStringLiteral("自动工程")
                              : QStringLiteral("当前工程"),
               QDir::toNativeSeparators(projectRoot)),
      this);
  projectPath->setObjectName(QStringLiteral("datasetImportProjectPathLabel"));
  projectPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
  projectPath->setWordWrap(true);
  rootLayout->addWidget(projectPath);

  auto *form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(9);

  mSceneName = new QLineEdit(suggestedSceneName.trimmed(), this);
  mSceneName->setObjectName(QStringLiteral("datasetImportSceneEdit"));
  mSceneName->setClearButtonEnabled(true);
  mSceneName->setPlaceholderText(
      QStringLiteral("仅允许英文、数字、点、下划线和连字符"));
  form->addRow(QStringLiteral("场景名称"), mSceneName);

  mFramesPerSecond = new QDoubleSpinBox(this);
  mFramesPerSecond->setObjectName(QStringLiteral("datasetImportFpsSpin"));
  mFramesPerSecond->setAccessibleName(QStringLiteral("视频抽帧帧率"));
  mFramesPerSecond->setRange(0.2, 10.0);
  mFramesPerSecond->setSingleStep(0.2);
  mFramesPerSecond->setDecimals(1);
  mFramesPerSecond->setValue(2.0);
  mFramesPerSecond->setSuffix(QStringLiteral(" FPS"));
  mFramesPerSecond->setKeyboardTracking(false);
  form->addRow(QStringLiteral("视频抽帧"), mFramesPerSecond);

  mOverwrite = new QCheckBox(
      QStringLiteral("覆盖同名托管数据集（开始前会再次确认）"), this);
  mOverwrite->setObjectName(QStringLiteral("datasetImportOverwriteCheck"));
  form->addRow(QString(), mOverwrite);
  rootLayout->addLayout(form);

  auto *sourceTitle = new QLabel(QStringLiteral("媒体来源"), this);
  sourceTitle->setObjectName(QStringLiteral("sectionTitle"));
  rootLayout->addWidget(sourceTitle);

  mSourceList = new QListWidget(this);
  mSourceList->setObjectName(QStringLiteral("datasetImportSourceList"));
  mSourceList->setAccessibleName(QStringLiteral("待导入媒体来源"));
  mSourceList->setAlternatingRowColors(true);
  mSourceList->setSelectionMode(QAbstractItemView::ExtendedSelection);
  mSourceList->setMinimumHeight(190);
  rootLayout->addWidget(mSourceList, 1);

  auto *sourceButtons = new QHBoxLayout();
  sourceButtons->setSpacing(6);
  auto *addFilesButton = new QPushButton(QStringLiteral("继续添加照片/视频..."), this);
  addFilesButton->setObjectName(QStringLiteral("datasetImportAddFilesButton"));
  auto *addDirectoryButton = new QPushButton(QStringLiteral("继续添加目录..."), this);
  addDirectoryButton->setObjectName(
      QStringLiteral("datasetImportAddDirectoryButton"));
  mRemoveButton = new QPushButton(QStringLiteral("移除所选"), this);
  mRemoveButton->setObjectName(QStringLiteral("datasetImportRemoveButton"));
  mClearButton = new QPushButton(QStringLiteral("清空"), this);
  mClearButton->setObjectName(QStringLiteral("datasetImportClearButton"));
  sourceButtons->addWidget(addFilesButton);
  sourceButtons->addWidget(addDirectoryButton);
  sourceButtons->addStretch(1);
  sourceButtons->addWidget(mRemoveButton);
  sourceButtons->addWidget(mClearButton);
  rootLayout->addLayout(sourceButtons);

  mSummary = new QLabel(this);
  mSummary->setObjectName(QStringLiteral("datasetImportSummaryLabel"));
  mSummary->setWordWrap(true);
  rootLayout->addWidget(mSummary);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  buttons->setObjectName(QStringLiteral("datasetImportButtonBox"));
  auto *cancelButton = buttons->button(QDialogButtonBox::Cancel);
  cancelButton->setObjectName(QStringLiteral("datasetImportCancelButton"));
  cancelButton->setText(QStringLiteral("取消"));
  mImportButton =
      buttons->addButton(QStringLiteral("添加到工程"), QDialogButtonBox::AcceptRole);
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

  if (initialSourcePaths.isEmpty()) {
    refreshSummary();
  } else {
    appendSourcePaths(initialSourcePaths);
  }
}

DatasetImportRequest DatasetImportDialog::request() const {
  DatasetImportRequest result;
  result.sceneName = mSceneName->text().trimmed();
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
  if (importRequest.sceneName.isEmpty()) {
    QMessageBox::critical(this, QStringLiteral("场景名称为空"),
                          QStringLiteral("请输入托管数据集的场景名称。"));
    mSceneName->setFocus();
    return;
  }
  if (importRequest.sourcePaths.isEmpty()) {
    QMessageBox::critical(
        this, QStringLiteral("尚未添加媒体"),
        QStringLiteral("请添加照片、视频或包含媒体文件的目录。"));
    return;
  }

  QApplication::setOverrideCursor(Qt::WaitCursor);
  QString error;
  mValidatedPlan = DatasetImportPlan::create(importRequest, &error);
  QApplication::restoreOverrideCursor();
  if (!mValidatedPlan.has_value()) {
    const QString message = error == QStringLiteral("Invalid scene name")
                                ? QStringLiteral("场景名称需为 1–120 个英文字母、数字、点、下划线或连字符；点不能位于开头或结尾、不能连续出现，且不能使用 Windows 保留名。")
                            : error == QStringLiteral("No supported image or video files were found")
                                ? QStringLiteral("所选来源中没有找到支持的照片或视频文件。")
                                : error;
    QMessageBox::critical(this, QStringLiteral("无法导入媒体"), message);
    return;
  }

  QDialog::accept();
}

void DatasetImportDialog::addFiles() {
  const QStringList paths = QFileDialog::getOpenFileNames(
      this, QStringLiteral("选择照片或视频"), mInitialDirectory,
      QStringLiteral(
          "照片与视频 (*.jpg *.jpeg *.png *.bmp *.tif *.tiff *.webp *.mp4 *.mov *.avi *.mkv *.webm *.m4v);;"
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
      this, QStringLiteral("选择包含照片或视频的目录"), mInitialDirectory,
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
    mSummary->setText(QStringLiteral("尚未添加媒体来源。"));
  } else {
    QString text = QStringLiteral("已添加 %1 个来源：%2 个文件、%3 个目录。目录会在开始导入时递归扫描。")
                       .arg(mSourceList->count())
                       .arg(fileCount)
                       .arg(directoryCount);
    if (mOverwrite->isChecked()) {
      text += QStringLiteral(" 将覆盖同名托管数据集。");
    }
    mSummary->setText(text);
  }

  mRemoveButton->setEnabled(!mSourceList->selectedItems().isEmpty());
  mClearButton->setEnabled(mSourceList->count() > 0);
  mImportButton->setEnabled(mSourceList->count() > 0 &&
                            !mSceneName->text().trimmed().isEmpty());
}

} // namespace gsw

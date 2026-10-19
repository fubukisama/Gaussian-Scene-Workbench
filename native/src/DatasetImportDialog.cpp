#include "AppLanguage.h"
#include "AppTheme.h"
#include <QCoreApplication>
#include "DatasetImportDialog.h"
#include "MultiItemList.h"
#include "WrappingCheckBox.h"
#include "WorkbenchIcons.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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
#include <QJsonArray>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSet>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <cmath>

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

WorkbenchIconId sourceIcon(const QFileInfo &info) {
  if (info.isDir()) return WorkbenchIconId::Folder;
  // Extension lookup only: the presentation layer never reads or decodes a
  // pending import simply to select an icon.
  const QString type = QMimeDatabase().mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
  if (type.startsWith(QStringLiteral("image/"))) return WorkbenchIconId::Image;
  if (type.startsWith(QStringLiteral("video/"))) return WorkbenchIconId::Video;
  return WorkbenchIconId::File;
}

std::optional<QJsonArray> numericVector(const QString &text) {
  const QStringList parts = text.trimmed().split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
  if (parts.size() != 3) return std::nullopt;
  QJsonArray result;
  for (const QString &part : parts) {
    bool valid = false;
    const double value = part.toDouble(&valid);
    if (!valid || !std::isfinite(value)) return std::nullopt;
    result.append(value);
  }
  return result;
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

  mInputMode = new QComboBox(this);
  mInputMode->setObjectName(QStringLiteral("datasetImportInputMode"));
  mInputMode->addItem(QString(), QStringLiteral("solve"));
  AppLanguage::bindComboItem(mInputMode, 0, AppLanguage::source("普通照片／视频（求解相机）"));
  mInputMode->addItem(QString(), QStringLiteral("locked"));
  AppLanguage::bindComboItem(mInputMode, 1, AppLanguage::source("已知相机数据集（锁定位姿）"));
  form->addRow(QCoreApplication::translate("Workbench", "相机输入策略"), mInputMode);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mInputMode)), AppLanguage::source("相机输入策略"));

  mCameraPanel = new QWidget(this);
  mCameraPanel->setProperty("gswLayoutContainer", true);
  auto *cameraForm = new QFormLayout(mCameraPanel);
  cameraForm->setContentsMargins(0, 0, 0, 0);
  cameraForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  cameraForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
  cameraForm->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  const auto cameraRow = [cameraForm](const char *source, QWidget *field) {
    cameraForm->addRow(QCoreApplication::translate("Workbench", source), field);
    AppLanguage::text(qobject_cast<QLabel *>(cameraForm->labelForField(field)), source);
  };
  mCameraAdapter = new QComboBox(mCameraPanel);
  mCameraAdapter->setObjectName(QStringLiteral("datasetImportCameraAdapter"));
  const QList<QPair<QString, const char *>> adapters{
      {QStringLiteral("auto"), AppLanguage::source("按明确格式识别")},
      {QStringLiteral("blender_transforms"), AppLanguage::source("Blender transforms 相机")},
      {QStringLiteral("colmap"), AppLanguage::source("COLMAP text / bin 相机")},
      {QStringLiteral("generic_json"), AppLanguage::source("GSW 通用相机 JSON")},
      {QStringLiteral("research_json"), AppLanguage::source("研究 JSON 适配器")}};
  for (const auto &adapter : adapters) {
    mCameraAdapter->addItem(QString(), adapter.first);
    AppLanguage::bindComboItem(mCameraAdapter, mCameraAdapter->count() - 1, adapter.second);
  }
  cameraRow(AppLanguage::source("来源适配器"), mCameraAdapter);
  auto *sourceRow = new QWidget(mCameraPanel);
  sourceRow->setProperty("gswLayoutContainer", true);
  auto *sourceLayout = new QHBoxLayout(sourceRow);
  sourceLayout->setContentsMargins(0, 0, 0, 0);
  mCameraSource = new QLineEdit(sourceRow);
  mCameraSource->setObjectName(QStringLiteral("datasetImportCameraSource"));
  auto *sourceFile = AppLanguage::text(new QPushButton(sourceRow), AppLanguage::source("选择文件…"));
  auto *sourceDirectory = AppLanguage::text(new QPushButton(sourceRow), AppLanguage::source("选择目录…"));
  sourceLayout->addWidget(mCameraSource, 1);
  sourceLayout->addWidget(sourceFile);
  sourceLayout->addWidget(sourceDirectory);
  cameraRow(AppLanguage::source("相机数据来源"), sourceRow);
  connect(sourceFile, &QPushButton::clicked, this, [this] { chooseCameraSource(false); });
  connect(sourceDirectory, &QPushButton::clicked, this, [this] { chooseCameraSource(true); });

  mCameraUnit = new QComboBox(mCameraPanel);
  mCameraUnit->setObjectName(QStringLiteral("datasetImportSourceUnit"));
  mCameraUnit->addItem(QString(), QStringLiteral("preserve"));
  AppLanguage::bindComboItem(mCameraUnit, 0, AppLanguage::source("保留来源声明；无声明则任意单位"));
  for (const QString &unit : {QStringLiteral("m"), QStringLiteral("cm"), QStringLiteral("mm")})
    mCameraUnit->addItem(unit, unit);
  mCameraUnit->addItem(QString(), QStringLiteral("unknown"));
  AppLanguage::bindComboItem(mCameraUnit, mCameraUnit->count() - 1, AppLanguage::source("明确保留任意单位（未知尺度）"));
  cameraRow(AppLanguage::source("源坐标单位"), mCameraUnit);
  mScaleEvidence = new QLineEdit(mCameraPanel);
  mScaleEvidence->setObjectName(QStringLiteral("datasetImportScaleEvidence"));
  AppLanguage::bind(mScaleEvidence, "placeholderText", AppLanguage::source("注明导出器、单位设置或声明依据；不是独立精度检核"));
  cameraRow(AppLanguage::source("尺度来源说明"), mScaleEvidence);
  auto *scaleHint = AppLanguage::text(new QLabel(mCameraPanel), AppLanguage::source(
      "相机焦距和位姿本身不证明公制尺度。用户声明仅标记为来源声明；不自动标为独立检核。"));
  scaleHint->setWordWrap(true);
  cameraForm->addRow(scaleHint);
  mNormalization = new QComboBox(mCameraPanel);
  mNormalization->setObjectName(QStringLiteral("datasetImportNormalization"));
  mNormalization->addItem(QString(), QStringLiteral("auto"));
  AppLanguage::bindComboItem(mNormalization, 0, AppLanguage::source("自动局部化（记录平移与均匀缩放）"));
  mNormalization->addItem(QString(), QStringLiteral("none"));
  AppLanguage::bindComboItem(mNormalization, 1, AppLanguage::source("不做数值归一化"));
  cameraRow(AppLanguage::source("训练坐标归一化"), mNormalization);
  mInitialization = new QComboBox(mCameraPanel);
  mInitialization->setObjectName(QStringLiteral("datasetImportInitialization"));
  const QList<QPair<QString, const char *>> initializations{
      {QStringLiteral("preserve"), AppLanguage::source("保留来源初始化；未声明则随机初始化")},
      {QStringLiteral("random"), AppLanguage::source("随机初始化（无几何先验）")},
      {QStringLiteral("external_point_cloud"), AppLanguage::source("外部点云初始化（含几何先验）")},
      {QStringLiteral("sfm_sparse"), AppLanguage::source("SfM 稀疏点初始化")}};
  for (const auto &initialization : initializations) {
    mInitialization->addItem(QString(), initialization.first);
    AppLanguage::bindComboItem(mInitialization, mInitialization->count() - 1, initialization.second);
  }
  cameraRow(AppLanguage::source("初始化来源"), mInitialization);
  mInitializationPath = new QLineEdit(mCameraPanel);
  mInitializationPath->setObjectName(QStringLiteral("datasetImportInitializationPath"));
  AppLanguage::bind(mInitializationPath, "placeholderText", AppLanguage::source("初始化点云路径；不要使用评价网格、深度或法向"));
  cameraRow(AppLanguage::source("初始化点云"), mInitializationPath);
  auto *boundsRow = new QWidget(mCameraPanel);
  boundsRow->setProperty("gswLayoutContainer", true);
  auto *boundsLayout = new QHBoxLayout(boundsRow);
  boundsLayout->setContentsMargins(0, 0, 0, 0);
  mBoundsMinimum = new QLineEdit(boundsRow);
  mBoundsMaximum = new QLineEdit(boundsRow);
  mBoundsMinimum->setObjectName(QStringLiteral("datasetImportBoundsMinimum"));
  mBoundsMaximum->setObjectName(QStringLiteral("datasetImportBoundsMaximum"));
  AppLanguage::bind(mBoundsMinimum, "placeholderText", AppLanguage::source("最小值 x y z"));
  AppLanguage::bind(mBoundsMaximum, "placeholderText", AppLanguage::source("最大值 x y z"));
  boundsLayout->addWidget(mBoundsMinimum);
  boundsLayout->addWidget(mBoundsMaximum);
  cameraRow(AppLanguage::source("随机范围（源单位，可选）"), boundsRow);
  auto *initializationHint = AppLanguage::text(new QLabel(mCameraPanel), AppLanguage::source(
      "仅有相机轨迹不保证物体深度。自动随机范围会记录覆盖假设；已知粗范围时请显式填写，不会读取评价几何。"));
  initializationHint->setWordWrap(true);
  cameraForm->addRow(initializationHint);
  form->addRow(mCameraPanel);

  mOverwrite = AppLanguage::text(new WrappingCheckBox(
      QCoreApplication::translate("Workbench", "覆盖同名托管数据集（开始前会再次确认）"), this), AppLanguage::source("覆盖同名托管数据集（开始前会再次确认）"));
  mOverwrite->setObjectName(QStringLiteral("datasetImportOverwriteCheck"));
  form->addRow(QString(), mOverwrite);
  bodyLayout->addLayout(form);

  auto *sourceTitle = AppLanguage::text(new QLabel(QCoreApplication::translate("Workbench", "媒体来源"), this), AppLanguage::source("媒体来源"));
  mSourceTitle = sourceTitle;
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
  mAddFilesButton = addFilesButton;
  addFilesButton->setObjectName(QStringLiteral("datasetImportAddFilesButton"));
  auto *addDirectoryButton = AppLanguage::text(new QPushButton(QCoreApplication::translate("Workbench", "继续添加目录..."), this), AppLanguage::source("继续添加目录..."));
  mAddDirectoryButton = addDirectoryButton;
  addDirectoryButton->setObjectName(
      QStringLiteral("datasetImportAddDirectoryButton"));
  mRemoveButton = AppLanguage::text(new QPushButton(QCoreApplication::translate("Workbench", "移除所选"), this), AppLanguage::source("移除所选"));
  mRemoveButton->setObjectName(QStringLiteral("datasetImportRemoveButton"));
  mClearButton = AppLanguage::text(new QPushButton(QCoreApplication::translate("Workbench", "清空"), this), AppLanguage::source("清空"));
  mClearButton->setObjectName(QStringLiteral("datasetImportClearButton"));
  auto *sourceButtons = new SourceActions(
      {addFilesButton, addDirectoryButton, mRemoveButton, mClearButton}, body);
  sourceButtons->setObjectName(QStringLiteral("datasetImportSourceActions"));
  mMediaActions = sourceButtons;
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
  for (auto *combo : body->findChildren<QComboBox *>()) {
    combo->setMinimumContentsLength(8);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
  connect(mInputMode, &QComboBox::currentIndexChanged, this, &DatasetImportDialog::refreshInputMode);
  connect(mCameraSource, &QLineEdit::textChanged, this, &DatasetImportDialog::refreshSummary);
  connect(mCameraUnit, &QComboBox::currentIndexChanged, this, &DatasetImportDialog::refreshInputMode);
  connect(mInitialization, &QComboBox::currentIndexChanged, this, &DatasetImportDialog::refreshInputMode);
  connect(mOverwrite, &QCheckBox::toggled, this,
          &DatasetImportDialog::refreshSummary);
  connect(buttons, &QDialogButtonBox::accepted, this,
          &DatasetImportDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this,
          &DatasetImportDialog::reject);

  const auto updateContext = [this, introduction, projectPath, unsavedProject, projectRoot]() {
    AppLanguage::bind(this, "windowTitle", mInputMode->currentData().toString() == QStringLiteral("locked")
        ? AppLanguage::source("导入已知相机数据集")
        : AppLanguage::source("添加照片与视频"));
    introduction->setText(mInputMode->currentData().toString() == QStringLiteral("locked")
        ? QCoreApplication::translate("Workbench", "导入已有相机与原图的托管副本，不重新求解位姿。格式、投影、数据划分和尺度将在后台统一验证；失败会停止，不切换为 SfM。")
        : unsavedProject
        ? QCoreApplication::translate("Workbench", "素材已选好。照片会复制到未命名工程，视频会按指定帧率抽帧；可先处理，稍后再自由选择工程保存位置。")
        : QCoreApplication::translate("Workbench", "素材已选好。照片会复制到当前工程的托管数据集，视频会按指定帧率抽帧。"));
    projectPath->setText(unsavedProject
        ? QCoreApplication::translate("Workbench", "当前工程：未命名工程（尚未保存，首次保存时可选择位置）")
        : QCoreApplication::translate("Workbench", "当前工程数据：%1").arg(QDir::toNativeSeparators(projectRoot)));
    refreshSummary();
  };
  AppLanguage::onChanged(this, updateContext);
  connect(mInputMode, &QComboBox::currentIndexChanged, this, updateContext);
  if (initialSourcePaths.isEmpty()) {
    refreshSummary();
  } else {
    appendSourcePaths(initialSourcePaths);
  }
  refreshIcons();
  refreshInputMode();
}

void DatasetImportDialog::changeEvent(QEvent *event) {
  QDialog::changeEvent(event);
  if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
      event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange ||
      event->type() == QEvent::ApplicationFontChange)
    refreshIcons();
}

void DatasetImportDialog::refreshIcons() {
  if (!mSourceList || !mAddFilesButton || !mAddDirectoryButton ||
      !mRemoveButton || !mClearButton || !mImportButton) return;
  const int scale = qApp->property("gswUiScalePercent").toInt();
  const int size = std::max(AppTheme::scaled(18, scale), fontMetrics().height());
  mSourceList->setIconSize(QSize(size, size));
  const auto setButton = [size](QPushButton *button, WorkbenchIconId icon) {
    button->setIcon(workbenchIcon(icon, button->palette()));
    button->setIconSize(QSize(size, size));
  };
  setButton(mAddFilesButton, WorkbenchIconId::ImportMedia);
  setButton(mAddDirectoryButton, WorkbenchIconId::Folder);
  setButton(mRemoveButton, WorkbenchIconId::Remove);
  setButton(mClearButton, WorkbenchIconId::Delete);
  setButton(mImportButton, WorkbenchIconId::Add);
  for (int index = 0; index < mSourceList->count(); ++index) {
    auto *item = mSourceList->item(index);
    item->setIcon(workbenchIcon(sourceIcon(QFileInfo(item->data(Qt::UserRole).toString())),
                               mSourceList->palette()));
  }
}

DatasetImportRequest DatasetImportDialog::request() const {
  DatasetImportRequest result;
  result.sceneName = mSceneName->text();
  result.framesPerSecond = mFramesPerSecond->value();
  result.overwrite = mOverwrite->isChecked();
  if (mInputMode->currentData().toString() == QStringLiteral("locked")) {
    result.cameraInput = mCameraInputTemplate;
    result.cameraInput.insert(QStringLiteral("adapter"), mCameraAdapter->currentData().toString());
    result.cameraInput.insert(QStringLiteral("sourcePath"), QDir::fromNativeSeparators(mCameraSource->text().trimmed()));
    result.cameraInput.insert(QStringLiteral("posePolicy"), QStringLiteral("locked"));
    result.cameraInput.insert(QStringLiteral("normalization"), QJsonObject{{QStringLiteral("mode"), mNormalization->currentData().toString()}});
    const QString unit = mCameraUnit->currentData().toString();
    if (unit != QStringLiteral("preserve")) {
      const bool arbitrary = unit == QStringLiteral("unknown");
      QJsonObject scale{{QStringLiteral("status"), arbitrary ? QStringLiteral("arbitrary") : QStringLiteral("source_declared")},
                        {QStringLiteral("source_unit"), unit},
                        {QStringLiteral("meters_per_unit"), arbitrary ? QJsonValue(QJsonValue::Null) :
                            QJsonValue(unit == QStringLiteral("cm") ? .01 : unit == QStringLiteral("mm") ? .001 : 1.0)},
                        {QStringLiteral("evidence"), arbitrary ? QJsonArray() : QJsonArray{QJsonObject{
                            {QStringLiteral("kind"), QStringLiteral("user_declaration")},
                            {QStringLiteral("description"), mScaleEvidence->text().trimmed()}}}}};
      result.cameraInput.insert(QStringLiteral("scale"), scale);
    } else result.cameraInput.remove(QStringLiteral("scale"));
    const QString initializationKind = mInitialization->currentData().toString();
    if (initializationKind != QStringLiteral("preserve")) {
      QJsonObject initialization{{QStringLiteral("kind"), initializationKind},
                                 {QStringLiteral("frame"), QStringLiteral("source")}};
      const auto suppliedInitialization = mCameraInputTemplate.value(QStringLiteral("initialization")).toObject();
      for (const auto &key : {QStringLiteral("seed"), QStringLiteral("count")})
        if (suppliedInitialization.contains(key)) initialization.insert(key, suppliedInitialization.value(key));
      if (initializationKind != QStringLiteral("random"))
        initialization.insert(QStringLiteral("path"), QDir::fromNativeSeparators(mInitializationPath->text().trimmed()));
      const auto minimum = numericVector(mBoundsMinimum->text());
      const auto maximum = numericVector(mBoundsMaximum->text());
      if (initializationKind == QStringLiteral("random") && minimum && maximum)
        initialization.insert(QStringLiteral("bounds"), QJsonArray{*minimum, *maximum});
      else initialization.remove(QStringLiteral("bounds"));
      if (initializationKind == QStringLiteral("random")) initialization.remove(QStringLiteral("path"));
      result.cameraInput.insert(QStringLiteral("initialization"), initialization);
    } else result.cameraInput.remove(QStringLiteral("initialization"));
    return result;
  }
  result.sourcePaths.reserve(mSourceList->count());
  for (int index = 0; index < mSourceList->count(); ++index) {
    result.sourcePaths.append(
        mSourceList->item(index)->data(Qt::UserRole).toString());
  }
  return result;
}

void DatasetImportDialog::setCameraInput(const QJsonObject &input) {
  mCameraInputTemplate = input;
  mInputMode->setCurrentIndex(mInputMode->findData(QStringLiteral("locked")));
  const QString adapter = input.value(QStringLiteral("adapter")).toString(QStringLiteral("auto"));
  const int adapterIndex = mCameraAdapter->findData(adapter);
  if (adapterIndex >= 0) mCameraAdapter->setCurrentIndex(adapterIndex);
  mCameraSource->setText(QDir::toNativeSeparators(input.value(QStringLiteral("sourcePath")).toString()));
  const QJsonObject scale = input.value(QStringLiteral("scale")).toObject();
  const int unitIndex = mCameraUnit->findData(scale.value(QStringLiteral("source_unit")).toString(QStringLiteral("preserve")));
  if (unitIndex >= 0) mCameraUnit->setCurrentIndex(unitIndex);
  const auto evidence = scale.value(QStringLiteral("evidence")).toArray();
  if (!evidence.isEmpty()) mScaleEvidence->setText(evidence.first().toObject().value(QStringLiteral("description")).toString());
  const int normalizationIndex = mNormalization->findData(input.value(QStringLiteral("normalization")).toObject()
      .value(QStringLiteral("mode")).toString(QStringLiteral("auto")));
  if (normalizationIndex >= 0) mNormalization->setCurrentIndex(normalizationIndex);
  const auto initialization = input.value(QStringLiteral("initialization")).toObject();
  const int initializationIndex = mInitialization->findData(initialization.value(QStringLiteral("kind")).toString(QStringLiteral("preserve")));
  if (initializationIndex >= 0) mInitialization->setCurrentIndex(initializationIndex);
  mInitializationPath->setText(QDir::toNativeSeparators(initialization.value(QStringLiteral("path")).toString()));
  const auto bounds = initialization.value(QStringLiteral("bounds")).toArray();
  const auto vectorText = [](const QJsonArray &vector) {
    QStringList values;
    for (const auto &value : vector) values.append(QString::number(value.toDouble(), 'g', 17));
    return values.join(QLatin1Char(' '));
  };
  if (bounds.size() == 2) {
    mBoundsMinimum->setText(vectorText(bounds.at(0).toArray()));
    mBoundsMaximum->setText(vectorText(bounds.at(1).toArray()));
  }
  refreshInputMode();
}

void DatasetImportDialog::chooseCameraSource(bool directory) {
  const QString path = directory
      ? QFileDialog::getExistingDirectory(this, QCoreApplication::translate("Workbench", "选择相机数据目录"), mInitialDirectory)
      : QFileDialog::getOpenFileName(this, QCoreApplication::translate("Workbench", "选择相机数据文件"), mInitialDirectory,
                                    QCoreApplication::translate("Workbench", "相机 JSON (*.json);;所有文件 (*)"));
  if (path.isEmpty()) return;
  mCameraSource->setText(QDir::toNativeSeparators(path));
  mInitialDirectory = directory ? path : QFileInfo(path).absolutePath();
}

void DatasetImportDialog::refreshInputMode() {
  if (!mCameraPanel || !mSourceList || !mSummary) return;
  const bool camera = mInputMode->currentData().toString() == QStringLiteral("locked");
  mCameraPanel->setVisible(camera);
  mSourceList->setVisible(!camera);
  mSourceTitle->setVisible(!camera);
  mMediaActions->setVisible(!camera);
  mFramesPerSecond->setEnabled(!camera);
  const QString unit = mCameraUnit->currentData().toString();
  mScaleEvidence->setEnabled(unit != QStringLiteral("preserve") && unit != QStringLiteral("unknown"));
  const QString initialization = mInitialization->currentData().toString();
  mInitializationPath->setEnabled(initialization == QStringLiteral("external_point_cloud") || initialization == QStringLiteral("sfm_sparse"));
  mBoundsMinimum->setEnabled(initialization == QStringLiteral("random"));
  mBoundsMaximum->setEnabled(initialization == QStringLiteral("random"));
  refreshSummary();
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
  if (importRequest.cameraInput.isEmpty() && importRequest.sourcePaths.isEmpty()) {
    QMessageBox::critical(
        this, QCoreApplication::translate("Workbench", "尚未添加媒体"),
        QCoreApplication::translate("Workbench", "请添加照片、视频或包含媒体文件的目录。"));
    return;
  }
  if (!importRequest.cameraInput.isEmpty()) {
    const QString unit = mCameraUnit->currentData().toString();
    if (unit != QStringLiteral("preserve") && unit != QStringLiteral("unknown") && mScaleEvidence->text().trimmed().isEmpty()) {
      QMessageBox::critical(this, QCoreApplication::translate("Workbench", "尺度声明缺少依据"),
                            QCoreApplication::translate("Workbench", "请填写单位声明来源；来源声明不会被标记为独立检核。"));
      mScaleEvidence->setFocus();
      return;
    }
    if (mInitialization->currentData().toString() == QStringLiteral("random") &&
        (!mBoundsMinimum->text().trimmed().isEmpty() || !mBoundsMaximum->text().trimmed().isEmpty())) {
      const auto minimum = numericVector(mBoundsMinimum->text());
      const auto maximum = numericVector(mBoundsMaximum->text());
      bool valid = minimum && maximum;
      for (int index = 0; valid && index < 3; ++index) valid = minimum->at(index).toDouble() < maximum->at(index).toDouble();
      if (!valid) {
        QMessageBox::critical(this, QCoreApplication::translate("Workbench", "初始化范围无效"),
                              QCoreApplication::translate("Workbench", "请分别输入三个有限数值，并确保每个最小值都小于最大值；范围采用源坐标单位。"));
        return;
      }
    }
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
    QMessageBox::critical(this, importRequest.cameraInput.isEmpty()
        ? QCoreApplication::translate("Workbench", "无法导入媒体")
        : QCoreApplication::translate("Workbench", "无法导入相机数据"), message);
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
        workbenchIcon(sourceIcon(info), mSourceList->palette()),
        QDir::toNativeSeparators(normalized), mSourceList);
    item->setData(Qt::UserRole, normalized);
    item->setToolTip(QDir::toNativeSeparators(normalized));
  }
  refreshSummary();
}

void DatasetImportDialog::refreshSummary() {
  if (mInputMode && mInputMode->currentData().toString() == QStringLiteral("locked")) {
    mSummary->setText(QCoreApplication::translate("Workbench", "将由共享相机适配器验证并托管图像、相机、划分和尺度记录；锁定模式不运行 COLMAP，测试图不会加入训练。"));
    mImportButton->setEnabled(!mCameraSource->text().trimmed().isEmpty() && !mSceneName->text().trimmed().isEmpty());
    return;
  }
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

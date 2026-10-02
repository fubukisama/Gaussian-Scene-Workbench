#include "AppLanguage.h"
#include <QCoreApplication>
#include "TrainingDialog.h"
#include "ManagedName.h"

#include "ColmapSupport.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>
#include <limits>

namespace gsw {

TrainingDialog::TrainingDialog(const QString &datasetPath, const QString &projectName,
                               const QString &defaultOutputRoot,
                               const bool hasSparseReconstruction,
                               const bool twoDgsAvailable, QWidget *parent)
    : QDialog(parent), mDatasetPath(datasetPath) {
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("训练设置"));
  setModal(true);
  setMinimumWidth(520);

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(16, 14, 16, 14);
  rootLayout->setSpacing(12);

  auto *form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(9);

  auto *datasetValue = new QLabel(QDir::toNativeSeparators(datasetPath), this);
  datasetValue->setObjectName(QStringLiteral("mutedLabel"));
  datasetValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
  datasetValue->setWordWrap(true);
  form->addRow(QCoreApplication::translate("Workbench", "数据集"), datasetValue);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(datasetValue)), AppLanguage::source("数据集"));

  mBackend = new QComboBox(this);
  mBackend->setObjectName(QStringLiteral("trainingBackendCombo"));
  mBackend->addItem(QStringLiteral("3D Gaussian Splatting"), QStringLiteral("3dgs"));
  mBackend->addItem(QStringLiteral("2D Gaussian Splatting"), QStringLiteral("2dgs"));
  form->addRow(QCoreApplication::translate("Workbench", "方法"), mBackend);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mBackend)), AppLanguage::source("方法"));
  auto *pipelineHint = new QLabel(this);
  pipelineHint->setObjectName(QStringLiteral("trainingPipelineHint"));
  pipelineHint->setWordWrap(true);
  const auto updatePipelineHint = [this, pipelineHint, twoDgsAvailable]() {
    const bool surfels = mBackend->currentData().toString() == QStringLiteral("2dgs");
    pipelineHint->setText(surfels && !twoDgsAvailable
        ? QCoreApplication::translate("Workbench", "2DGS 环境尚未配置。请设置 TWO_DGS_DIR 和 TWO_DGS_PYTHON；开始前会检查曲面光栅化扩展。")
        : surfels
        ? QCoreApplication::translate("Workbench", "2DGS 支持暂停、完整状态续训与连续快照预览；视口使用透视校正曲面显示，混合结果与 CUDA 渲染可能不同。")
        : QCoreApplication::translate("Workbench", "3DGS 支持暂停、完整状态续训与连续预览；共享 GPU 预览不可用时回退到快照。"));
  };
  connect(mBackend, &QComboBox::currentIndexChanged, this, updatePipelineHint);
  AppLanguage::onChanged(this, updatePipelineHint);
  updatePipelineHint();
  form->addRow(pipelineHint);

  mQuality = new QComboBox(this);
  mQuality->setObjectName(QStringLiteral("trainingQualityCombo"));
  mQuality->addItem(QCoreApplication::translate("Workbench", "快速预览"), QStringLiteral("quick"));
  AppLanguage::bindComboItem(mQuality, mQuality->count() - 1, AppLanguage::source("快速预览"));
  mQuality->addItem(QCoreApplication::translate("Workbench", "标准"), QStringLiteral("full"));
  AppLanguage::bindComboItem(mQuality, mQuality->count() - 1, AppLanguage::source("标准"));
  mQuality->addItem(QCoreApplication::translate("Workbench", "高质量"), QStringLiteral("quality"));
  AppLanguage::bindComboItem(mQuality, mQuality->count() - 1, AppLanguage::source("高质量"));
  mQuality->addItem(QCoreApplication::translate("Workbench", "最高质量"), QStringLiteral("max_quality"));
  AppLanguage::bindComboItem(mQuality, mQuality->count() - 1, AppLanguage::source("最高质量"));
  form->addRow(QCoreApplication::translate("Workbench", "质量预设"), mQuality);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mQuality)), AppLanguage::source("质量预设"));

  mIterations = new QSpinBox(this);
  mIterations->setObjectName(QStringLiteral("trainingIterationsSpinBox"));
  mIterations->setRange(1000, 200000);
  mIterations->setSingleStep(1000);
  form->addRow(QCoreApplication::translate("Workbench", "迭代次数"), mIterations);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mIterations)), AppLanguage::source("迭代次数"));

  mResolution = new QComboBox(this);
  mResolution->setObjectName(QStringLiteral("trainingResolutionCombo"));
  for (const int resolution : {1, 2, 4, 8}) {
    mResolution->addItem(QStringLiteral("1/%1").arg(resolution), resolution);
  }
  form->addRow(QCoreApplication::translate("Workbench", "训练分辨率"), mResolution);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mResolution)), AppLanguage::source("训练分辨率"));

  mOutputScene = new QLineEdit(projectName + QDateTime::currentDateTime().toString(QStringLiteral("-yyyyMMdd-HHmmss")), this);
  mOutputScene->setObjectName(QStringLiteral("trainingOutputNameEdit"));
  mOutputScene->setMaxLength(std::numeric_limits<int>::max());
  AppLanguage::bind(mOutputScene, "placeholderText", AppLanguage::source("支持任意文字、空格和符号"));
  AppLanguage::bind(mOutputScene, "toolTip", AppLanguage::source("显示名称将完整保留；软件自动生成安全的存储目录名。"));
  form->addRow(QCoreApplication::translate("Workbench", "输出名称"), mOutputScene);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mOutputScene)), AppLanguage::source("输出名称"));

  auto *outputRow = new QWidget(this);
  auto *outputLayout = new QHBoxLayout(outputRow);
  outputLayout->setContentsMargins(0, 0, 0, 0);
  outputLayout->setSpacing(6);
  mOutputRoot = new QLineEdit(QDir::toNativeSeparators(defaultOutputRoot), outputRow);
  auto *browseButton = AppLanguage::text(new QPushButton(QCoreApplication::translate("Workbench", "浏览..."), outputRow), AppLanguage::source("浏览..."));
  AppLanguage::bind(browseButton, "toolTip", AppLanguage::source("选择训练输出目录"));
  outputLayout->addWidget(mOutputRoot, 1);
  outputLayout->addWidget(browseButton);
  form->addRow(QCoreApplication::translate("Workbench", "输出目录"), outputRow);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(outputRow)), AppLanguage::source("输出目录"));

  mRunColmap = AppLanguage::text(new QCheckBox(QCoreApplication::translate("Workbench", "训练前运行 COLMAP 重建"), this), AppLanguage::source("训练前运行 COLMAP 重建"));
  mRunColmap->setChecked(!hasSparseReconstruction);
  form->addRow(QString(), mRunColmap);

  mOverwrite = AppLanguage::text(new QCheckBox(QCoreApplication::translate("Workbench", "允许覆盖同名输出"), this), AppLanguage::source("允许覆盖同名输出"));
  mOverwrite->setChecked(false);
  form->addRow(QString(), mOverwrite);

  rootLayout->addLayout(form);

  auto *note = AppLanguage::text(new QLabel(
      QCoreApplication::translate("Workbench", "训练前检查相机覆盖和稀疏点；退化重建会有限重试，原始照片与已有输出保留。3DGS / 2DGS 快速预览默认 10,000 次迭代、1/2 分辨率。3DGS 增密后继续优化，2DGS 包含法线约束阶段；手动参数不被覆盖。"),
      this), AppLanguage::source("训练前检查相机覆盖和稀疏点；退化重建会有限重试，原始照片与已有输出保留。3DGS / 2DGS 快速预览默认 10,000 次迭代、1/2 分辨率。3DGS 增密后继续优化，2DGS 包含法线约束阶段；手动参数不被覆盖。"));
  note->setObjectName(QStringLiteral("mutedLabel"));
  note->setWordWrap(true);
  rootLayout->addWidget(note);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  AppLanguage::text(buttons->button(QDialogButtonBox::Cancel), AppLanguage::source("取消"));
  auto *startButton = AppLanguage::text(buttons->addButton(QCoreApplication::translate("Workbench", "开始训练"), QDialogButtonBox::AcceptRole), AppLanguage::source("开始训练"));
  startButton->setDefault(true);
  rootLayout->addWidget(buttons);

  connect(mBackend, &QComboBox::currentIndexChanged, this, &TrainingDialog::applyPreset);
  connect(mQuality, &QComboBox::currentIndexChanged, this, &TrainingDialog::applyPreset);
  connect(browseButton, &QPushButton::clicked, this, &TrainingDialog::chooseOutputRoot);
  connect(buttons, &QDialogButtonBox::accepted, this, &TrainingDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &TrainingDialog::reject);
  applyPreset();
}

TrainingConfiguration TrainingDialog::configuration() const {
  TrainingConfiguration result;
  result.backend = mBackend->currentData().toString();
  result.quality = mQuality->currentData().toString();
  result.outputRoot = QDir::cleanPath(mOutputRoot->text().trimmed());
  result.outputScene = mOutputScene->text();
  result.outputStorageName = managedStorageName(result.outputScene);
  result.iterations = mIterations->value();
  result.resolution = mResolution->currentData().toInt();
  result.runColmap = mRunColmap->isChecked();
  result.overwrite = mOverwrite->isChecked();
  return result;
}

void TrainingDialog::accept() {
  if (!datasetContainsImages()) {
    QMessageBox::critical(
        this, QCoreApplication::translate("Workbench", "数据集不可训练"),
        QCoreApplication::translate("Workbench", "所选目录必须包含 images 或 input 子目录，并且其中至少有一张图像。请重新导入数据集根目录。"));
    return;
  }

  const TrainingConfiguration config = configuration();
  if (config.outputScene.trimmed().isEmpty()) {
    QMessageBox::critical(
        this, QCoreApplication::translate("Workbench", "输出名称无效"),
        QCoreApplication::translate("Workbench", "请输入名称，名称不能仅包含空白字符。"));
    return;
  }
  if (config.outputRoot.isEmpty() ||
      (!QFileInfo::exists(config.outputRoot) && !QDir().mkpath(config.outputRoot))) {
    QMessageBox::critical(this, QCoreApplication::translate("Workbench", "输出目录不可用"),
                          QCoreApplication::translate("Workbench", "无法创建或访问指定的输出目录。"));
    return;
  }

  const QDir target(QDir(config.outputRoot).filePath(config.outputStorageName));
  const bool outputContainsData = target.exists() &&
                                  !target.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
  if (outputContainsData && !config.overwrite) {
    QMessageBox::critical(this, QCoreApplication::translate("Workbench", "输出已存在"),
                          QCoreApplication::translate("Workbench", "同名输出目录已有数据。请更改名称，或明确启用覆盖。"));
    return;
  }
  if (outputContainsData && config.overwrite) {
    const auto answer = QMessageBox::warning(
        this, QCoreApplication::translate("Workbench", "确认覆盖训练输出"),
        QCoreApplication::translate("Workbench", "训练开始后会删除以下目录中的现有数据：\n%1")
            .arg(QDir::toNativeSeparators(target.absolutePath())),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
      return;
    }
  }
  QDialog::accept();
}

void TrainingDialog::applyPreset() {
  const QString quality = mQuality->currentData().toString();
  const int iterations = quality == QStringLiteral("quick") ? 10000 : 30000;
  int resolution = 8;
  if (quality == QStringLiteral("quick")) {
    resolution = 2;
  } else if (quality == QStringLiteral("quality")) {
    resolution = 4;
  } else if (quality == QStringLiteral("max_quality")) {
    resolution = 2;
  }
  mIterations->setValue(iterations);
  const int resolutionIndex = mResolution->findData(resolution);
  if (resolutionIndex >= 0) {
    mResolution->setCurrentIndex(resolutionIndex);
  }
}

void TrainingDialog::chooseOutputRoot() {
  const QString directory = QFileDialog::getExistingDirectory(
      this, QCoreApplication::translate("Workbench", "选择训练输出目录"), mOutputRoot->text());
  if (!directory.isEmpty()) {
    mOutputRoot->setText(QDir::toNativeSeparators(directory));
  }
}

bool TrainingDialog::datasetContainsImages() const {
  return !datasetImageDirectory(mDatasetPath).isEmpty();
}

} // namespace gsw

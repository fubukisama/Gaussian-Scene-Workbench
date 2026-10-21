#include "AppLanguage.h"
#include <QCoreApplication>
#include "TrainingDialog.h"
#include "ManagedName.h"
#include "WrappingCheckBox.h"
#include "CameraDatasetInfo.h"
#include "InitializationDialog.h"

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
#include <QJsonArray>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
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
  setMinimumSize(320, 220);
  resize(640, 650);

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(16, 14, 16, 14);
  rootLayout->setSpacing(12);

  // Only the body scrolls; the primary action remains reachable at any height.
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

  auto *form = new QFormLayout();
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(9);

  auto *datasetValue = new QLabel(QDir::toNativeSeparators(datasetPath), this);
  mDatasetValue = datasetValue;
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
  mQuality->addItem(QCoreApplication::translate("Workbench", "最高精度（原始分辨率）"), QStringLiteral("original_quality"));
  AppLanguage::bindComboItem(mQuality, mQuality->count() - 1, AppLanguage::source("最高精度（原始分辨率）"));
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
  AppLanguage::bindComboItem(mResolution, mResolution->findData(1), AppLanguage::source("原始分辨率（1:1）"));
  AppLanguage::bind(mResolution, "toolTip", AppLanguage::source("比例分别作用于图像宽度和高度；原始分辨率不额外降采样。"));
  form->addRow(QCoreApplication::translate("Workbench", "训练分辨率"), mResolution);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mResolution)), AppLanguage::source("训练分辨率"));
  mResolutionPolicy = new QComboBox(this);
  mResolutionPolicy->setObjectName(QStringLiteral("trainingResolutionPolicyCombo"));
  mResolutionPolicy->addItem(QString(), QStringLiteral("preserve"));
  AppLanguage::bindComboItem(mResolutionPolicy, 0, AppLanguage::source("保留输入尺寸（1:1，不自动缩图）"));
  mResolutionPolicy->addItem(QString(), QStringLiteral("manual"));
  AppLanguage::bindComboItem(mResolutionPolicy, 1, AppLanguage::source("显式缩放（使用所选倍率）"));
  mResolutionPolicy->setCurrentIndex(1);
  form->addRow(QCoreApplication::translate("Workbench", "分辨率策略"), mResolutionPolicy);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mResolutionPolicy)), AppLanguage::source("分辨率策略"));
  auto *resolutionHint = AppLanguage::text(new QLabel(this), AppLanguage::source("原始分辨率按 1:1 训练，不自动降采样；显存占用和耗时会明显增加。去畸变仍会进行；已缩小的数据需从原图重新重建。"));
  resolutionHint->setObjectName(QStringLiteral("trainingResolutionHint"));
  resolutionHint->setWordWrap(true);
  form->addRow(resolutionHint);
  const auto updateResolutionHint = [this, resolutionHint]() {
    resolutionHint->setVisible(mResolution->currentData().toInt() == 1);
    AppLanguage::bind(resolutionHint, "text", mPosePolicy && mPosePolicy->currentData().toString() == QStringLiteral("locked")
        ? AppLanguage::source("锁定相机使用已验证的图像与投影；不自动缩图、不重求解，不支持的畸变或投影会拒绝。")
        : AppLanguage::source("原始分辨率按 1:1 训练，不自动降采样；显存占用和耗时会明显增加。去畸变仍会进行；已缩小的数据需从原图重新重建。"));
  };
  connect(mResolution, &QComboBox::currentIndexChanged, this, updateResolutionHint);
  AppLanguage::onChanged(this, updateResolutionHint);
  updateResolutionHint();

  mResourceGuard = AppLanguage::text(new WrappingCheckBox(this),
      AppLanguage::source("启用显存增密保护（独立于性能优化）"));
  mResourceGuard->setObjectName(QStringLiteral("trainingResourceGuard"));
  mResourceGuard->setChecked(true);
  AppLanguage::bind(mResourceGuard, "toolTip", AppLanguage::source(
      "关闭后不按软件显存阈值拒绝、暂缓增密或自动暂停；不会改变执行优化、照片分辨率、增密参数或手动暂停与完整状态保存。真实 CUDA 内存不足仍会报错。"));
  form->addRow(QString(), mResourceGuard);
  const auto addRuntimeInteger = [this, form](const char *source, const char *name,
      const int minimum, const int maximum, const int value, const QString &suffix = {}) {
    auto *spin = new QSpinBox(this);
    spin->setObjectName(QString::fromLatin1(name));
    spin->setRange(minimum, maximum);
    spin->setValue(value);
    spin->setSuffix(suffix);
    form->addRow(QCoreApplication::translate("Workbench", source), spin);
    AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(spin)), source);
    return spin;
  };
  mTrainingMemoryPercent = addRuntimeInteger(AppLanguage::source("训练显存目标"),
      "trainingMemoryPercent", 50, 100, 90, QStringLiteral("%"));
  mMinimumSpareMiB = addRuntimeInteger(AppLanguage::source("最低空闲显存"),
      "trainingMinimumSpareMiB", 0, 1048576, 512, QStringLiteral(" MiB"));
  mMaxConsecutiveDenials = addRuntimeInteger(AppLanguage::source("连续拒绝次数"),
      "trainingMaxConsecutiveDenials", 1, 1000000, 3);
  mGrowthMode = new QComboBox(this);
  mGrowthMode->setObjectName(QStringLiteral("trainingGrowthModeCombo"));
  mGrowthMode->addItem(QString(), QStringLiteral("pause"));
  AppLanguage::bindComboItem(mGrowthMode, 0, AppLanguage::source("质量优先：保存完整状态并安全暂停"));
  mGrowthMode->addItem(QString(), QStringLiteral("defer"));
  AppLanguage::bindComboItem(mGrowthMode, 1, AppLanguage::source("继续训练：暂缓增密并提示质量风险"));
  form->addRow(QCoreApplication::translate("Workbench", "显存持续不足时"), mGrowthMode);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mGrowthMode)), AppLanguage::source("显存持续不足时"));
  auto *growthHint = new QLabel(this);
  growthHint->setObjectName(QStringLiteral("trainingResourceGuardHint"));
  growthHint->setWordWrap(true);
  form->addRow(growthHint);
  const auto updateGuardControls = [this, growthHint]() {
    for (auto *control : {mTrainingMemoryPercent, mMinimumSpareMiB, mMaxConsecutiveDenials})
      control->setEnabled(mResourceGuard->isChecked());
    mGrowthMode->setEnabled(mResourceGuard->isChecked());
    growthHint->setText(mResourceGuard->isChecked()
        ? QCoreApplication::translate("Workbench", "保护开启：连续 %1 次拒绝后按所选方式处理；质量优先会在安全边界保存完整状态并暂停。100% 目标或零余量不保证不会发生 CUDA 内存不足。")
              .arg(mMaxConsecutiveDenials->value())
        : QCoreApplication::translate("Workbench", "保护关闭：不执行软件显存阈值判定，不自动暂缓增密或暂停；执行优化与手动暂停、停止、完整状态保存保持独立。"));
  };
  connect(mResourceGuard, &QCheckBox::toggled, this, updateGuardControls);
  connect(mMaxConsecutiveDenials, &QSpinBox::valueChanged, this, updateGuardControls);
  AppLanguage::onChanged(this, updateGuardControls);
  updateGuardControls();

  auto *recommended = AppLanguage::text(new QPushButton(this),
      AppLanguage::source("应用推荐执行设置"));
  recommended->setObjectName(QStringLiteral("trainingApplyRecommendedExecution"));
  AppLanguage::bind(recommended, "toolTip", AppLanguage::source(
      "仅开启图像暂存、预取、FP32 容量复用、按需曲面辅助计算并选择自动损失与标准渲染器；不会改变显存保护、相机、分辨率、增密或训练迭代。"));
  form->addRow(QString(), recommended);
  mImageStaging = AppLanguage::text(new WrappingCheckBox(this),
      AppLanguage::source("有界图像暂存与传输"));
  mImageStaging->setObjectName(QStringLiteral("trainingImageStaging"));
  mImageStaging->setChecked(true);
  AppLanguage::bind(mImageStaging, "toolTip", AppLanguage::source(
      "仅改变原始像素的主存存储与有界 GPU 传输，不降低照片分辨率。图像预取需要此路径；关闭时仍保留预取请求并明确报告无效原因。"));
  form->addRow(QString(), mImageStaging);
  mCameraPrefetch = AppLanguage::text(new WrappingCheckBox(this),
      AppLanguage::source("跨步图像预取"));
  mCameraPrefetch->setObjectName(QStringLiteral("trainingCameraPrefetch"));
  mCameraPrefetch->setChecked(true);
  AppLanguage::bind(mCameraPrefetch, "toolTip", AppLanguage::source(
      "提前准备下一训练视图并与当前计算重叠；保持原始像素、相机采样顺序和随机状态，不改变照片分辨率。"));
  form->addRow(QString(), mCameraPrefetch);
  mCapacityStorage = AppLanguage::text(new WrappingCheckBox(this),
      AppLanguage::source("FP32 容量缓冲复用"));
  mCapacityStorage->setObjectName(QStringLiteral("trainingCapacityStorage"));
  mCapacityStorage->setChecked(true);
  AppLanguage::bind(mCapacityStorage, "toolTip", AppLanguage::source(
      "复用 FP32 参数与 Adam 缓冲，减少增密时的重复分配；保持原始精度、增密算法和完整状态续训，不支持时保留原执行路径。"));
  form->addRow(QString(), mCapacityStorage);
  mSurfelAuxiliary = AppLanguage::text(new WrappingCheckBox(this),
      AppLanguage::source("按需计算曲面法线"));
  mSurfelAuxiliary->setObjectName(QStringLiteral("trainingSurfelAuxiliary"));
  mSurfelAuxiliary->setChecked(true);
  AppLanguage::bind(mSurfelAuxiliary, "toolTip", AppLanguage::source(
      "仅在法线项权重为零时跳过未使用的 Python 曲面法线计算；保留所有活跃约束，不代表关闭 CUDA 辅助缓冲。"));
  form->addRow(QString(), mSurfelAuxiliary);
  auto *independentPerformanceHint = AppLanguage::text(new QLabel(this), AppLanguage::source(
      "执行选项逐项生效，不由显存保护开关控制；不会自动修改相机、照片分辨率或增密算法。渲染器与损失的实际引擎及回退原因由任务报告记录。"));
  independentPerformanceHint->setObjectName(QStringLiteral("trainingIndependentPerformanceHint"));
  independentPerformanceHint->setWordWrap(true);
  form->addRow(independentPerformanceHint);
  auto *prefetchHint = new QLabel(this);
  prefetchHint->setObjectName(QStringLiteral("trainingPrefetchDependencyHint"));
  prefetchHint->setWordWrap(true);
  form->addRow(prefetchHint);
  const auto updateIndependentPerformanceControls = [this, prefetchHint]() {
    mSurfelAuxiliary->setEnabled(mBackend->currentData().toString() == QStringLiteral("2dgs"));
    prefetchHint->setText(mCameraPrefetch->isChecked() && !mImageStaging->isChecked()
        ? QCoreApplication::translate("Workbench", "图像预取：请求开启，但图像暂存已关闭，因此有效状态为关闭；请求值保留，不会自动改写。")
        : QCoreApplication::translate("Workbench", "图像预取依赖有界图像暂存；3DGS 不使用曲面辅助计算。其他执行设置与显存保护独立。"));
  };
  connect(mImageStaging, &QCheckBox::toggled, this, updateIndependentPerformanceControls);
  connect(mCameraPrefetch, &QCheckBox::toggled, this, updateIndependentPerformanceControls);
  connect(mBackend, &QComboBox::currentIndexChanged, this, updateIndependentPerformanceControls);
  AppLanguage::onChanged(this, updateIndependentPerformanceControls);
  updateIndependentPerformanceControls();
  mImageGpuSlots = addRuntimeInteger(AppLanguage::source("图像 GPU 槽位"),
      "trainingImageGpuSlots", 1, 2, 2);
  mImagePinnedMiB = addRuntimeInteger(AppLanguage::source("锁页图像预算"),
      "trainingImagePinnedMiB", 16, 1024, 256, QStringLiteral(" MiB"));
  mImageGpuMiB = addRuntimeInteger(AppLanguage::source("GPU 图像预算"),
      "trainingImageGpuMiB", 16, 2048, 512, QStringLiteral(" MiB"));
  mLossMode = new QComboBox(this);
  mLossMode->setObjectName(QStringLiteral("trainingLossModeCombo"));
  mLossMode->addItem(QString(), QStringLiteral("auto"));
  AppLanguage::bindComboItem(mLossMode, 0, AppLanguage::source("自动（兼容时使用融合损失）"));
  mLossMode->addItem(QString(), QStringLiteral("stock"));
  AppLanguage::bindComboItem(mLossMode, 1, AppLanguage::source("标准 L1 / SSIM"));
  form->addRow(QCoreApplication::translate("Workbench", "损失执行方式"), mLossMode);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mLossMode)), AppLanguage::source("损失执行方式"));
  mTrainingEngine = new QComboBox(this);
  mTrainingEngine->setObjectName(QStringLiteral("trainingEngineCombo"));
  mTrainingEngine->addItem(QString(), QStringLiteral("stock"));
  AppLanguage::bindComboItem(mTrainingEngine, 0, AppLanguage::source("标准（兼容完整增密与续训）"));
  mTrainingEngine->addItem(QString(), QStringLiteral("fastergs"));
  AppLanguage::bindComboItem(mTrainingEngine, 1, AppLanguage::source("Fast-GS（实验性，兼容时启用）"));
  form->addRow(QCoreApplication::translate("Workbench", "训练渲染后端"), mTrainingEngine);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mTrainingEngine)),
                    AppLanguage::source("训练渲染后端"));
  auto *engineHint = AppLanguage::text(new QLabel(this), AppLanguage::source(
      "Fast-GS 需独立验证过的运行库；目前仅支持不增密的 3DGS 标准 Adam。2DGS、稀疏 Adam、深度约束或旧检查点自动保留标准后端，不关闭这些功能来凑兼容。"));
  engineHint->setWordWrap(true);
  form->addRow(engineHint);
  mProfileInterval = addRuntimeInteger(AppLanguage::source("性能采样间隔（迭代）"),
      "trainingProfileInterval", 1, 1000000, 30);
  connect(recommended, &QPushButton::clicked, this, [this] {
    mImageStaging->setChecked(true);
    mCameraPrefetch->setChecked(true);
    mCapacityStorage->setChecked(true);
    mSurfelAuxiliary->setChecked(true);
    mLossMode->setCurrentIndex(mLossMode->findData(QStringLiteral("auto")));
    mTrainingEngine->setCurrentIndex(mTrainingEngine->findData(QStringLiteral("stock")));
  });

  mOutputScene = new QLineEdit(projectName + QDateTime::currentDateTime().toString(QStringLiteral("-yyyyMMdd-HHmmss")), this);
  mOutputScene->setObjectName(QStringLiteral("trainingOutputNameEdit"));
  mOutputScene->setMaxLength(std::numeric_limits<int>::max());
  AppLanguage::bind(mOutputScene, "placeholderText", AppLanguage::source("支持任意文字、空格和符号"));
  AppLanguage::bind(mOutputScene, "toolTip", AppLanguage::source("显示名称将完整保留；软件自动生成安全的存储目录名。"));
  form->addRow(QCoreApplication::translate("Workbench", "输出名称"), mOutputScene);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mOutputScene)), AppLanguage::source("输出名称"));

  auto *outputRow = new QWidget(this);
  outputRow->setProperty("gswLayoutContainer", true);
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

  mPosePolicy = new QComboBox(this);
  mPosePolicy->setObjectName(QStringLiteral("trainingPosePolicyCombo"));
  mPosePolicy->addItem(QString(), QStringLiteral("solve"));
  AppLanguage::bindComboItem(mPosePolicy, 0, AppLanguage::source("求解／复用 SfM 相机（保留质量检查）"));
  mPosePolicy->addItem(QString(), QStringLiteral("locked"));
  AppLanguage::bindComboItem(mPosePolicy, 1, AppLanguage::source("锁定已知相机（禁止重求解）"));
  connect(mPosePolicy, &QComboBox::currentIndexChanged, this, updateResolutionHint);
  form->addRow(QCoreApplication::translate("Workbench", "位姿策略"), mPosePolicy);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mPosePolicy)), AppLanguage::source("位姿策略"));
  auto *cameraPolicyHint = AppLanguage::text(new QLabel(this), AppLanguage::source(
      "锁定模式验证图像、投影、划分和尺度，不要求 SfM 稀疏点；错误时停止，不回退求相机。切换为求解会生成新的数据版本。"));
  cameraPolicyHint->setWordWrap(true);
  form->addRow(cameraPolicyHint);
  mRunColmap = AppLanguage::text(new WrappingCheckBox(QCoreApplication::translate("Workbench", "训练前运行 COLMAP 重建"), this), AppLanguage::source("训练前运行 COLMAP 重建"));
  mRunColmap->setObjectName(QStringLiteral("trainingRunColmapCheckBox"));
  mRunColmap->setChecked(!hasSparseReconstruction);
  form->addRow(QString(), mRunColmap);

  mReconstructionSummaryLabel = new QLabel(this);
  mReconstructionSummaryLabel->setObjectName(QStringLiteral("trainingReconstructionSummaryLabel"));
  mReconstructionSummaryLabel->setTextFormat(Qt::PlainText);
  mReconstructionSummaryLabel->setWordWrap(true);
  mReconstructionSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(QCoreApplication::translate("Workbench", "相机与重建来源"), mReconstructionSummaryLabel);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mReconstructionSummaryLabel)),
                    AppLanguage::source("相机与重建来源"));
  mInitializationButton = AppLanguage::text(new QPushButton(this), AppLanguage::source("初始化设置与检查…"));
  mInitializationButton->setObjectName(QStringLiteral("trainingInitializationButton"));
  mInitializationButton->setEnabled(false);
  form->addRow(mInitializationButton);
  connect(mInitializationButton, &QPushButton::clicked, this, &TrainingDialog::openInitialization);
  mReconstructionDecisionLabel = new QLabel(this);
  mReconstructionDecisionLabel->setObjectName(QStringLiteral("trainingReconstructionDecisionLabel"));
  mReconstructionDecisionLabel->setTextFormat(Qt::PlainText);
  mReconstructionDecisionLabel->setWordWrap(true);
  mReconstructionDecisionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(QCoreApplication::translate("Workbench", "COLMAP 决策"), mReconstructionDecisionLabel);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mReconstructionDecisionLabel)),
                    AppLanguage::source("COLMAP 决策"));
  mReconstructionScanner = new TrainingReconstructionSummaryScanner(this);
  connect(mReconstructionScanner, &TrainingReconstructionSummaryScanner::summaryReady, this,
          [this](const TrainingReconstructionSummary &summary) {
    mReconstructionSummary = summary;
    mReconstructionSummaryPending = false;
    auto camera = summary.report.value(QStringLiteral("cameraDataset")).toObject();
    if (mPosePolicy->currentData().toString() == QStringLiteral("locked") && !camera.isEmpty() &&
        !mExpectedCameraDatasetIdentity.isEmpty() &&
        camera.value(QStringLiteral("dataIdentity")).toString() != mExpectedCameraDatasetIdentity) {
      camera.insert(QStringLiteral("blocked"), true);
      camera.insert(QStringLiteral("errors"), QJsonArray{QStringLiteral("camera_data_identity_mismatch")});
      camera.insert(QStringLiteral("dataIdentity"), mExpectedCameraDatasetIdentity);
      mReconstructionSummary.report.insert(QStringLiteral("cameraDataset"), camera);
      mReconstructionSummary.report.insert(QStringLiteral("blocked"), true);
    }
    if (!camera.isEmpty()) {
      mCameraDatasetPath = camera.value(QStringLiteral("metadataPath")).toString(mCameraDatasetPath);
      mCameraDatasetIdentity = camera.value(QStringLiteral("dataIdentity")).toString(mCameraDatasetIdentity);
    }
    if (summary.ready && !mRunColmapEdited && mRunColmap->isChecked() &&
        summary.report.value(QStringLiteral("sourceKind")).toString() == QStringLiteral("alignment_cache") &&
        summary.report.value(QStringLiteral("usable")).toBool()) {
      mRunColmap->setChecked(false);
      return;
    }
    refreshReconstructionSummary();
  });
  connect(mRunColmap, &QCheckBox::clicked, this, [this] { mRunColmapEdited = true; });
  connect(mRunColmap, &QCheckBox::toggled, this, &TrainingDialog::scanReconstructionSummary);
  connect(mPosePolicy, &QComboBox::currentIndexChanged, this, &TrainingDialog::refreshCameraPolicy);
  connect(this, &QDialog::finished, mReconstructionScanner,
          &TrainingReconstructionSummaryScanner::cancel);
  AppLanguage::onChanged(this, [this] { refreshReconstructionSummary(); });

  mInputSummaryLabel = new QLabel(this);
  mInputSummaryLabel->setObjectName(QStringLiteral("trainingInputSummaryLabel"));
  mInputSummaryLabel->setTextFormat(Qt::PlainText);
  mInputSummaryLabel->setWordWrap(true);
  mInputSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form->addRow(QCoreApplication::translate("Workbench", "图像尺寸摘要"), mInputSummaryLabel);
  AppLanguage::text(qobject_cast<QLabel *>(form->labelForField(mInputSummaryLabel)),
                    AppLanguage::source("图像尺寸摘要"));
  mInputScanner = new TrainingInputSummaryScanner(this);
  connect(mInputScanner, &TrainingInputSummaryScanner::summaryReady, this,
          [this](const TrainingInputSummary &summary) {
            mInputSummary = summary;
            mInputSummaryPending = false;
            refreshInputSummary();
          });
  connect(mResolution, &QComboBox::currentIndexChanged, this,
          &TrainingDialog::refreshInputSummary);
  connect(mResolution, &QComboBox::currentIndexChanged, this, [this] {
    if (!mUpdatingPolicy) {
      if (mPosePolicy->currentData().toString() != QStringLiteral("locked")) mSolveResolution = mResolution->currentData().toInt();
    }
  });
  connect(mResolutionPolicy, &QComboBox::currentIndexChanged, this, &TrainingDialog::refreshResolutionPolicy);
  connect(mRunColmap, &QCheckBox::toggled, this, &TrainingDialog::scanInputSummary);
  connect(this, &QDialog::finished, mInputScanner,
          &TrainingInputSummaryScanner::cancel);
  AppLanguage::onChanged(this, [this] { refreshInputSummary(); });

  mOverwrite = AppLanguage::text(new WrappingCheckBox(QCoreApplication::translate("Workbench", "允许覆盖同名输出"), this), AppLanguage::source("允许覆盖同名输出"));
  mOverwrite->setChecked(false);
  form->addRow(QString(), mOverwrite);

  bodyLayout->addLayout(form);

  auto *note = AppLanguage::text(new QLabel(
      QCoreApplication::translate("Workbench", "训练前检查相机覆盖和稀疏点；退化重建会有限重试，原始照片与已有输出保留。3DGS / 2DGS 快速预览默认 10,000 次迭代、1/2 分辨率。3DGS 增密后继续优化，2DGS 包含法线约束阶段；手动参数不被覆盖。"),
      this), AppLanguage::source("训练前检查相机覆盖和稀疏点；退化重建会有限重试，原始照片与已有输出保留。3DGS / 2DGS 快速预览默认 10,000 次迭代、1/2 分辨率。3DGS 增密后继续优化，2DGS 包含法线约束阶段；手动参数不被覆盖。"));
  note->setObjectName(QStringLiteral("mutedLabel"));
  note->setWordWrap(true);
  const auto updateTrainingNote = [this, note] {
    AppLanguage::bind(note, "text", mPosePolicy->currentData().toString() == QStringLiteral("locked")
        ? AppLanguage::source("3DGS / 2DGS 共用锁定相机与数据划分策略。原图、相机和尺度来源保持可追溯；预览正常不代表训练、尺度或模型质量已验证。")
        : AppLanguage::source("训练前检查相机覆盖和稀疏点；退化重建会有限重试，原始照片与已有输出保留。3DGS / 2DGS 快速预览默认 10,000 次迭代、1/2 分辨率。3DGS 增密后继续优化，2DGS 包含法线约束阶段；手动参数不被覆盖。"));
  };
  connect(mPosePolicy, &QComboBox::currentIndexChanged, this, updateTrainingNote);
  AppLanguage::onChanged(this, updateTrainingNote);
  bodyLayout->addWidget(note);
  bodyLayout->addStretch(1);

  for (auto *combo : body->findChildren<QComboBox *>()) {
    combo->setMinimumContentsLength(8);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
  for (auto *label : body->findChildren<QLabel *>()) {
    if (label->wordWrap())
      label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  }

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  AppLanguage::text(buttons->button(QDialogButtonBox::Cancel), AppLanguage::source("取消"));
  mStartButton = AppLanguage::text(buttons->addButton(QCoreApplication::translate("Workbench", "开始训练"), QDialogButtonBox::AcceptRole), AppLanguage::source("开始训练"));
  mStartButton->setObjectName(QStringLiteral("trainingStartButton"));
  mStartButton->setDefault(true);
  rootLayout->addWidget(buttons);

  // Backend-specific optimization defaults are resolved by the worker. The
  // common iteration/resolution values here are user edits, not reset triggers.
  connect(mQuality, &QComboBox::currentIndexChanged, this, &TrainingDialog::applyPreset);
  connect(browseButton, &QPushButton::clicked, this, &TrainingDialog::chooseOutputRoot);
  connect(buttons, &QDialogButtonBox::accepted, this, &TrainingDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &TrainingDialog::reject);
  applyPreset();
  const QString canonicalManifest = QDir(datasetPath).filePath(QStringLiteral("camera_dataset.json"));
  if (QFileInfo(canonicalManifest).isFile()) setCameraDatasetReference(canonicalManifest);
  scanInputSummary();
  refreshReconstructionSummary();
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
  result.posePolicy = mPosePolicy->currentData().toString();
  result.runColmap = result.posePolicy != QStringLiteral("locked") && mRunColmap->isChecked();
  result.cameraDatasetPath = mCameraDatasetPath;
  result.cameraDatasetIdentity = mCameraDatasetIdentity;
  result.resolutionPolicy = mResolutionPolicy->currentData().toString();
  result.overwrite = mOverwrite->isChecked();
  result.resourceGuardEnabled = mResourceGuard->isChecked();
  result.imageStaging = mImageStaging->isChecked();
  result.cameraPrefetch = mCameraPrefetch->isChecked();
  result.capacityStorage = mCapacityStorage->isChecked();
  result.surfelAuxiliary = mSurfelAuxiliary->isChecked();
  result.trainingMemoryPercent = mTrainingMemoryPercent->value();
  result.minimumSpareMiB = mMinimumSpareMiB->value();
  result.maxConsecutiveDenials = mMaxConsecutiveDenials->value();
  result.imageGpuSlots = mImageGpuSlots->value();
  result.imagePinnedMiB = mImagePinnedMiB->value();
  result.imageGpuMiB = mImageGpuMiB->value();
  result.profileInterval = mProfileInterval->value();
  result.lossMode = mLossMode->currentData().toString();
  result.trainingEngine = mTrainingEngine->currentData().toString();
  result.growthMode = mGrowthMode->currentData().toString();
  result.runtimeOptions = QJsonObject{
      {QStringLiteral("version"), 2},
      {QStringLiteral("resourceGuard"), QJsonObject{
          {QStringLiteral("enabled"), result.resourceGuardEnabled},
          {QStringLiteral("mode"), result.growthMode},
          {QStringLiteral("usage_fraction"), result.trainingMemoryPercent / 100.0},
          {QStringLiteral("minimum_spare_mib"), result.minimumSpareMiB},
          {QStringLiteral("max_consecutive_denials"), result.maxConsecutiveDenials}}},
      {QStringLiteral("execution"), QJsonObject{
          {QStringLiteral("image_staging"), result.imageStaging},
          {QStringLiteral("camera_prefetch"), result.cameraPrefetch},
          {QStringLiteral("capacity_storage"), result.capacityStorage},
          {QStringLiteral("surfel_auxiliary"), result.surfelAuxiliary},
          {QStringLiteral("loss_mode"), result.lossMode},
          {QStringLiteral("engine"), result.trainingEngine},
          {QStringLiteral("gpu_slots"), result.imageGpuSlots},
          {QStringLiteral("pinned_mib"), result.imagePinnedMiB},
          {QStringLiteral("gpu_mib"), result.imageGpuMiB}}},
      {QStringLiteral("monitoring"), QJsonObject{{QStringLiteral("profile_interval"), result.profileInterval}}}};
  return result;
}

void TrainingDialog::accept() {
  if (mReconstructionProbeConfigured &&
      (!reconstructionPreflightReady() || mReconstructionSummary.report.value(QStringLiteral("blocked")).toBool())) {
    QMessageBox::critical(this, QCoreApplication::translate("Workbench", "相机与重建来源不可用"),
                          reconstructionDecisionText());
    return;
  }
  if (!datasetContainsImages()) {
    QMessageBox::critical(
        this, QCoreApplication::translate("Workbench", "数据集不可训练"),
        mPosePolicy->currentData().toString() == QStringLiteral("locked")
            ? QCoreApplication::translate("Workbench", "规范相机数据没有通过验证的训练视图；不会扫描目录补入测试图，也不会运行 COLMAP。")
            : QCoreApplication::translate("Workbench", "所选目录必须包含 images 或 input 子目录，并且其中至少有一张图像。请重新导入数据集根目录。"));
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
  } else if (quality == QStringLiteral("original_quality")) {
    resolution = 1;
  }
  mIterations->setValue(iterations);
  // A fidelity preset is not permission to change an imported camera dataset's
  // training pixels. Locked input only changes ratio after an explicit policy edit.
  if (mPosePolicy && mPosePolicy->currentData().toString() == QStringLiteral("locked")) return;
  mUpdatingPolicy = true;
  mResolutionPolicy->setCurrentIndex(mResolutionPolicy->findData(QStringLiteral("manual")));
  const int resolutionIndex = mResolution->findData(resolution);
  if (resolutionIndex >= 0) {
    mResolution->setCurrentIndex(resolutionIndex);
  }
  mSolveResolution = resolution;
  mResolution->setEnabled(true);
  mUpdatingPolicy = false;
}

void TrainingDialog::setCameraDatasetReference(const QString &metadataPath,
                                              const QString &dataIdentity,
                                              const QString &posePolicy) {
  mCameraDatasetPath = metadataPath;
  mCameraDatasetIdentity = dataIdentity;
  mExpectedCameraDatasetIdentity = dataIdentity;
  const int index = mPosePolicy->findData(posePolicy);
  if (index >= 0) mPosePolicy->setCurrentIndex(index);
  refreshCameraPolicy();
}

QJsonObject TrainingDialog::cameraDatasetSummary() const {
  return mReconstructionSummary.report.value(QStringLiteral("cameraDataset")).toObject();
}
void TrainingDialog::setInitializationWorkspace(const QString &workspaceRoot) {
  mInitializationWorkspace = workspaceRoot;
  refreshReconstructionSummary();
}
void TrainingDialog::useInitializationDataset(const QString &metadataPath, const QString &dataIdentity) {
  const QFileInfo selected(metadataPath);
  mDatasetPath = selected.isDir() ? selected.absoluteFilePath() : selected.absolutePath();
  if (mDatasetValue) mDatasetValue->setText(QDir::toNativeSeparators(mDatasetPath));
  setCameraDatasetReference(selected.isDir() ? QDir(mDatasetPath).filePath(QStringLiteral("camera_dataset.json"))
                                           : selected.absoluteFilePath(), dataIdentity, QStringLiteral("locked"));
  scanInputSummary();
  scanReconstructionSummary();
}
void TrainingDialog::openInitialization() {
  if (!mReconstructionProbeConfigured || !reconstructionPreflightReady() ||
      mInitializationWorkspace.isEmpty() || mCameraDatasetPath.isEmpty() ||
      mCameraDatasetIdentity.isEmpty() || CameraDatasetInfo::blocked(cameraDatasetSummary())) return;
  const QString inputIdentity = mCameraDatasetIdentity;
  InitializationDialog initialization(mProbePython, mProbeBackendRoot, mInitializationWorkspace,
      mCameraDatasetPath, inputIdentity, cameraDatasetSummary().value(QStringLiteral("initialization")).toObject(),
      mProbeEnvironment, this);
  initialization.setModal(true);
  auto request = initialization.requestDocument();
  request.insert(QStringLiteral("backend"), mBackend->currentData().toString());
  request.insert(QStringLiteral("resolution"), mResolution->currentData().toInt());
  initialization.setRequestDocument(request);
  connect(&initialization, &InitializationDialog::versionCreated, this,
      [this, inputIdentity](const QString &path, const QJsonObject &summary) {
        emit initializationVersionCreated(path, summary, inputIdentity);
      });
  initialization.exec(); // Explicit user action; never opened by a probe or language change.
}

void TrainingDialog::refreshCameraPolicy() {
  if (!mRunColmap || !mResolutionPolicy) return;
  const bool locked = mPosePolicy->currentData().toString() == QStringLiteral("locked");
  mUpdatingPolicy = true;
  if (locked) {
    mRunColmap->setChecked(false);
    if (!mPreviousLocked) {
      mSolveResolution = mResolution->currentData().toInt();
      mResolutionPolicy->setCurrentIndex(mResolutionPolicy->findData(QStringLiteral("preserve")));
      mResolution->setCurrentIndex(mResolution->findData(1));
    }
  } else if (mPreviousLocked) {
    mRunColmap->setChecked(true);
    mResolutionPolicy->setCurrentIndex(mResolutionPolicy->findData(QStringLiteral("manual")));
    mResolution->setCurrentIndex(mResolution->findData(mSolveResolution));
  }
  mRunColmap->setEnabled(!locked);
  mResolution->setEnabled(mResolutionPolicy->currentData().toString() == QStringLiteral("manual"));
  mPreviousLocked = locked;
  mUpdatingPolicy = false;
  scanInputSummary();
  scanReconstructionSummary();
}

void TrainingDialog::refreshResolutionPolicy() {
  if (mUpdatingPolicy) return;
  const bool preserve = mResolutionPolicy->currentData().toString() == QStringLiteral("preserve");
  mUpdatingPolicy = true;
  if (preserve) mResolution->setCurrentIndex(mResolution->findData(1));
  mResolution->setEnabled(!preserve);
  mUpdatingPolicy = false;
  refreshInputSummary();
}

void TrainingDialog::chooseOutputRoot() {
  const QString directory = QFileDialog::getExistingDirectory(
      this, QCoreApplication::translate("Workbench", "选择训练输出目录"), mOutputRoot->text());
  if (!directory.isEmpty()) {
    mOutputRoot->setText(QDir::toNativeSeparators(directory));
  }
}

void TrainingDialog::scanInputSummary() {
  if (mUpdatingPolicy) return;
  if (mPosePolicy && mPosePolicy->currentData().toString() == QStringLiteral("locked")) {
    mInputSummaryPending = false;
    refreshInputSummary();
    return;
  }
  if (mReconstructionSummary.report.value(QStringLiteral("decision")).toString() == QStringLiteral("new_solve_version")) {
    mInputSummaryPending = false;
    refreshInputSummary();
    return;
  }
  mInputSummaryPending = true;
  refreshInputSummary();
  mInputScanner->request(mDatasetPath, mRunColmap->isChecked());
}

void TrainingDialog::refreshInputSummary() {
  if (!mInputSummaryLabel) {
    return;
  }
  if (mPosePolicy && mPosePolicy->currentData().toString() == QStringLiteral("locked")) {
    mInputSummaryLabel->setText(mResolutionPolicy->currentData().toString() == QStringLiteral("preserve")
        ? QCoreApplication::translate("Workbench", "保留每个视图的输入尺寸和完整内参；最终尺寸及缩放后的 K 记录在实际训练相机报告中。")
        : QCoreApplication::translate("Workbench", "明确使用 1/%1 宽高倍率；主点与焦距同步变换，最终尺寸及 K 记录在实际训练相机报告中。")
            .arg(mResolution->currentData().toInt()));
    return;
  }
  if (mReconstructionSummary.report.value(QStringLiteral("decision")).toString() == QStringLiteral("new_solve_version")) {
    mInputSummaryLabel->setText(QCoreApplication::translate("Workbench", "仅复制规范清单中的训练图像后求解；明确使用 1/%1 宽高倍率，实际尺寸和新相机内参将在训练报告中记录。")
        .arg(mResolution->currentData().toInt()));
    return;
  }
  if (mInputSummaryPending) {
    mInputSummaryLabel->setText(QCoreApplication::translate(
        "Workbench", "正在读取图像尺寸（仅读取文件头）…"));
    return;
  }
  if (mInputSummary.imageCount == 0) {
    mInputSummaryLabel->setText(QCoreApplication::translate(
        "Workbench", "未找到可读取的图像文件。"));
    return;
  }
  const QLocale locale;
  QStringList lines;
  lines.append((mRunColmap->isChecked()
      ? QCoreApplication::translate("Workbench", "去畸变前预计尺寸（%1 张目录图像；非注册相机数量）")
      : QCoreApplication::translate("Workbench", "目录尺寸预计值（%1 张图像；非注册相机数量或最终加载尺寸）"))
      .arg(locale.toString(mInputSummary.imageCount)));
  lines.append(QCoreApplication::translate("Workbench", "来源：%1")
      .arg(QDir::toNativeSeparators(mInputSummary.sourceDirectory)));
  const int resolution = mResolution->currentData().toInt();
  long double sourcePixels = 0;
  long double projectedPixels = 0;
  bool zeroDimension = false;
  for (qsizetype index = 0; index < mInputSummary.sizeGroups.size(); ++index) {
    const auto &group = mInputSummary.sizeGroups.at(index);
    const QSize projected = projectedTrainingImageSize(group.sourceSize, resolution);
    zeroDimension = zeroDimension || projected.width() == 0 || projected.height() == 0;
    // Convert before multiplying: arbitrary headers/counts must not overflow
    // a 32-bit dimension or a 64-bit total-pixel accumulator.
    sourcePixels += static_cast<long double>(group.sourceSize.width()) *
                    group.sourceSize.height() * group.imageCount;
    projectedPixels += static_cast<long double>(projected.width()) *
                       projected.height() * group.imageCount;
    if (index < 4) {
      lines.append(QCoreApplication::translate(
          "Workbench", "%1 × %2 → %3 × %4 px（%5 张）")
          .arg(locale.toString(group.sourceSize.width()),
               locale.toString(group.sourceSize.height()),
               locale.toString(projected.width()), locale.toString(projected.height()),
               locale.toString(group.imageCount)));
    }
  }
  if (mInputSummary.sizeGroups.size() > 4) {
    lines.append(QCoreApplication::translate("Workbench", "另有 %1 种图像尺寸。")
        .arg(locale.toString(mInputSummary.sizeGroups.size() - 4)));
  }
  if (sourcePixels > 0) {
    lines.append(QCoreApplication::translate(
        "Workbench", "已知尺寸的目录图像：%1 MP → 预计 %2 MP（%3%）。")
        .arg(locale.toString(double(sourcePixels / 1000000.0L), 'f', 1),
             locale.toString(double(projectedPixels / 1000000.0L), 'f', 1),
             locale.toString(double(projectedPixels * 100.0L / sourcePixels), 'f', 1)));
  }
  if (mInputSummary.unknownSizeCount > 0) {
    lines.append(QCoreApplication::translate(
        "Workbench", "%1 张图像尺寸未知（损坏或格式不受支持）；未推测其尺寸。")
        .arg(locale.toString(mInputSummary.unknownSizeCount)));
  }
  if (mInputSummary.skippedLinkCount > 0) {
    lines.append(QCoreApplication::translate("Workbench", "跳过 %1 个符号链接或范围外路径。")
        .arg(locale.toString(mInputSummary.skippedLinkCount)));
  }
  if (zeroDimension) {
    lines.append(QCoreApplication::translate(
        "Workbench", "有图像缩放后宽或高为 0 px；请提高训练分辨率。"));
  }
  if (mRunColmap->isChecked()) {
    lines.append(QCoreApplication::translate(
        "Workbench", "去畸变可能改变有效尺寸；实际训练尺寸以加载报告为准。"));
  }
  mInputSummaryLabel->setText(lines.join(QLatin1Char('\n')));
}

bool TrainingDialog::datasetContainsImages() const {
  if (mPosePolicy->currentData().toString() == QStringLiteral("locked") ||
      mReconstructionSummary.report.value(QStringLiteral("decision")).toString() == QStringLiteral("new_solve_version")) {
    const auto camera = cameraDatasetSummary();
    const auto train = camera.value(QStringLiteral("counts")).toObject().value(QStringLiteral("train"));
    return reconstructionPreflightReady() && !CameraDatasetInfo::blocked(camera) &&
        (train.isDouble() ? train.toInteger() > 0 : !camera.value(QStringLiteral("splits")).toObject().value(QStringLiteral("train")).toArray().isEmpty());
  }
  return !datasetImageDirectory(mDatasetPath).isEmpty();
}

void TrainingDialog::setReconstructionProbe(const QString &python, const QString &backendRoot,
                                             const QProcessEnvironment &environment) {
  mProbePython = python;
  mProbeBackendRoot = backendRoot;
  mProbeEnvironment = environment;
  mReconstructionProbeConfigured = true;
  scanReconstructionSummary();
}

bool TrainingDialog::effectiveRunColmap() const {
  if (mPosePolicy->currentData().toString() == QStringLiteral("locked")) return false;
  return reconstructionPreflightReady()
      ? mReconstructionSummary.report.value(QStringLiteral("effectiveRunColmap")).toBool()
      : mRunColmap->isChecked();
}

bool TrainingDialog::reconstructionPreflightReady() const {
  return !mReconstructionSummaryPending && mReconstructionSummary.ready &&
      (mPosePolicy->currentData().toString() != QStringLiteral("locked") || !cameraDatasetSummary().isEmpty());
}

QString TrainingDialog::reconstructionDecisionText() const {
  return mReconstructionDecisionLabel ? mReconstructionDecisionLabel->text() : QString();
}

TrainingReconstructionSummary TrainingDialog::reconstructionSummary() const {
  return mReconstructionSummary;
}

void TrainingDialog::scanReconstructionSummary() {
  if (!mReconstructionProbeConfigured || mUpdatingPolicy) return;
  mReconstructionSummaryPending = true;
  refreshReconstructionSummary();
  mReconstructionScanner->request(mDatasetPath, mRunColmap->isChecked(),
                                 mProbePython, mProbeBackendRoot, mProbeEnvironment,
                                 mPosePolicy->currentData().toString(),
                                 mCameraDatasetPath, mExpectedCameraDatasetIdentity);
}

void TrainingDialog::refreshReconstructionSummary() {
  if (!mReconstructionSummaryLabel || !mReconstructionDecisionLabel) return;
  if (mInitializationButton) mInitializationButton->setEnabled(mReconstructionProbeConfigured &&
      reconstructionPreflightReady() && !mInitializationWorkspace.isEmpty() && !mCameraDatasetPath.isEmpty() &&
      !mCameraDatasetIdentity.isEmpty() && !CameraDatasetInfo::blocked(cameraDatasetSummary()));
  if (mStartButton && mReconstructionProbeConfigured)
    mStartButton->setEnabled(reconstructionPreflightReady() &&
                            !mReconstructionSummary.report.value(QStringLiteral("blocked")).toBool());
  if (mReconstructionSummaryPending) {
    const QString pending = QCoreApplication::translate("Workbench", "正在读取相机与重建来源（只读）…");
    mReconstructionSummaryLabel->setText(pending);
    mReconstructionDecisionLabel->setText(pending);
    return;
  }
  if (!mReconstructionSummary.ready) {
    const QString unavailable = mReconstructionProbeConfigured
        ? QCoreApplication::translate("Workbench", "来源预检失败：%1").arg(mReconstructionSummary.diagnostic)
        : QCoreApplication::translate("Workbench", "相机来源尚未预检");
    mReconstructionSummaryLabel->setText(unavailable);
    mReconstructionDecisionLabel->setText(unavailable);
    return;
  }
  const QJsonObject report = mReconstructionSummary.report;
  if (mPosePolicy->currentData().toString() == QStringLiteral("locked") ||
      report.value(QStringLiteral("decision")).toString() == QStringLiteral("new_solve_version")) {
    const auto camera = cameraDatasetSummary();
    mReconstructionSummaryLabel->setText(CameraDatasetInfo::summaryText(camera));
    mReconstructionDecisionLabel->setText(CameraDatasetInfo::decisionText(camera));
    if (mStartButton) mStartButton->setEnabled(reconstructionPreflightReady() && !CameraDatasetInfo::blocked(camera));
    refreshInputSummary();
    return;
  }
  const QLocale locale;
  const auto number = [&](const char *key) {
    const auto value = report.value(QLatin1String(key));
    return value.isDouble() ? locale.toString(qint64(value.toDouble()))
                           : QCoreApplication::translate("Workbench", "未知");
  };
  const QString format = report.value(QStringLiteral("format")).toString();
  QString formatText = QCoreApplication::translate("Workbench", "未知");
  if (format == QStringLiteral("colmap_binary")) formatText = QCoreApplication::translate("Workbench", "COLMAP 二进制");
  else if (format == QStringLiteral("colmap_text")) formatText = QCoreApplication::translate("Workbench", "COLMAP 文本");
  else if (format == QStringLiteral("transforms_json")) formatText = QCoreApplication::translate("Workbench", "Blender / NeRF transforms JSON");
  QStringList lines{QCoreApplication::translate("Workbench", "来源格式：%1").arg(formatText)};
  const QString cameraPath = report.value(QStringLiteral("cameraFile")).toString();
  const QString imagePath = report.value(QStringLiteral("imageFile")).toString();
  const QString pointPath = report.value(QStringLiteral("pointFile")).toString();
  if (!cameraPath.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "相机文件：%1").arg(QDir::toNativeSeparators(cameraPath)));
  if (!imagePath.isEmpty()) lines.append((format == QStringLiteral("transforms_json")
      ? QCoreApplication::translate("Workbench", "相机帧文件：%1")
      : QCoreApplication::translate("Workbench", "注册照片文件：%1")).arg(QDir::toNativeSeparators(imagePath)));
  if (!pointPath.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "稀疏点文件：%1").arg(QDir::toNativeSeparators(pointPath)));
  const QString imagesDirectory = QDir::toNativeSeparators(report.value(QStringLiteral("trainingImagesDirectory")).toString());
  if (!imagesDirectory.isEmpty()) lines.append((format == QStringLiteral("transforms_json")
      ? QCoreApplication::translate("Workbench", "图像引用根目录：%1")
      : QCoreApplication::translate("Workbench", "训练图像目录：%1")).arg(imagesDirectory));
  const QString colmapDirectory = QDir::toNativeSeparators(report.value(QStringLiteral("colmapInputDirectory")).toString());
  if (!colmapDirectory.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "COLMAP 输入目录：%1").arg(colmapDirectory));
  if (format == QStringLiteral("transforms_json"))
    lines.append(QCoreApplication::translate("Workbench", "相机帧 %1；输入照片 %2；相机标定数量不适用于此格式。")
        .arg(number("frameCount"), number("inputImages")));
  else
    lines.append(QCoreApplication::translate("Workbench", "注册照片 %1 / 输入照片 %2；相机标定 %3；有效稀疏点 %4 / %5。")
        .arg(number("registeredImages"), number("inputImages"), number("cameraCount"), number("validPoints"), number("sparsePoints")));
  QStringList reasons;
  for (const auto &reason : report.value(QStringLiteral("reasons")).toArray()) {
    const QString code = reason.toString();
    if (code == QStringLiteral("too_few_views")) reasons.append(QCoreApplication::translate("Workbench", "注册照片过少"));
    else if (code == QStringLiteral("too_few_points")) reasons.append(QCoreApplication::translate("Workbench", "有效稀疏点不足"));
    else if (code == QStringLiteral("invalid_points")) reasons.append(QCoreApplication::translate("Workbench", "稀疏点或轨迹无效"));
    else if (code == QStringLiteral("low_coverage")) reasons.append(QCoreApplication::translate("Workbench", "注册覆盖不足"));
    else if (code == QStringLiteral("input_mismatch")) reasons.append(QCoreApplication::translate("Workbench", "输入照片数量不匹配"));
    else if (code == QStringLiteral("invalid_camera_metadata")) reasons.append(QCoreApplication::translate("Workbench", "相机标定或图像引用无效"));
    else reasons.append(QCoreApplication::translate("Workbench", "模型文件无效"));
  }
  lines.append(report.value(QStringLiteral("usable")).toBool()
      ? (format == QStringLiteral("transforms_json")
         ? QCoreApplication::translate("Workbench", "transforms 参数及引用检查通过（未进行 COLMAP 最低有效性检查）。")
         : QCoreApplication::translate("Workbench", "最低有效性检查通过（不是重建精度保证）。"))
      : QCoreApplication::translate("Workbench", "最低有效性检查未通过：%1").arg(reasons.join(QStringLiteral(" / "))));
  mReconstructionSummaryLabel->setText(lines.join(QLatin1Char('\n')));
  const QString decision = report.value(QStringLiteral("decision")).toString();
  QString decisionText;
  if (decision == QStringLiteral("reuse")) decisionText = QCoreApplication::translate("Workbench", "复用当前重建：已通过最低有效性检查；不运行 COLMAP。");
  else if (decision == QStringLiteral("reuse_transforms")) decisionText = QCoreApplication::translate("Workbench", "复用 transforms 相机帧：参数及图像引用检查通过；不运行 COLMAP。");
  else if (decision == QStringLiteral("reuse_cache")) decisionText = QCoreApplication::translate("Workbench", "复用对齐缓存：启动时恢复缓存，不运行 COLMAP。");
  else if (decision == QStringLiteral("user_rerun")) decisionText = QCoreApplication::translate("Workbench", "用户选择重跑：重新进行相机注册、稀疏重建和去畸变。");
  else if (decision == QStringLiteral("missing_reconstruction")) decisionText = QCoreApplication::translate("Workbench", "缺少可识别重建：后端将先运行 COLMAP。");
  else if (decision == QStringLiteral("undistortion_required")) decisionText = QCoreApplication::translate("Workbench", "只有原始 input 图像，没有去畸变 images：后端必须运行 COLMAP 转换。");
  else if (decision == QStringLiteral("repair_required")) decisionText = QCoreApplication::translate("Workbench", "当前重建不可用：后端将尝试修复重建，而非直接训练。");
  else if (decision == QStringLiteral("invalid_cache")) decisionText = QCoreApplication::translate("Workbench", "对齐缓存不可训练：后端会拒绝复用；请勾选重跑 COLMAP。");
  else if (decision == QStringLiteral("invalid_source")) decisionText = QCoreApplication::translate("Workbench", "相机数据不可读取或不受训练器支持：不能直接复用；请勾选重跑 COLMAP。");
  else if (decision == QStringLiteral("no_images")) decisionText = QCoreApplication::translate("Workbench", "无可用图像：不能开始训练。");
  else decisionText = QCoreApplication::translate("Workbench", "未知");
  mReconstructionDecisionLabel->setText(decisionText);
}

} // namespace gsw

#include "MeshGenerationDialog.h"
#include "AppLanguage.h"
#include "WorkspaceDocument.h"
#include "WrappingCheckBox.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

namespace gsw {
MeshGenerationDialog::MeshGenerationDialog(const QString &modelDirectory, QWidget *parent)
    : QDialog(parent) {
  setObjectName(QStringLiteral("meshGenerationDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("生成几何"));
  setMinimumSize(320, 220);
  resize(640, 540);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(12);
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
  layout->addWidget(scroll, 1);
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->setRowWrapPolicy(QFormLayout::WrapLongRows);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(9);
  auto row = [form](const char *caption, QWidget *field) {
    form->addRow(AppLanguage::text(new QLabel, caption), field);
  };
  auto *sourceRow = new QWidget(this);
  sourceRow->setProperty("gswLayoutContainer", true);
  auto *sourceLayout = new QHBoxLayout(sourceRow);
  sourceLayout->setContentsMargins(0, 0, 0, 0);
  mSource = new QLineEdit(QDir::toNativeSeparators(modelDirectory), sourceRow);
  mSource->setObjectName(QStringLiteral("meshSourceDirectory"));
  auto *browse = AppLanguage::text(new QPushButton(sourceRow), AppLanguage::source("浏览..."));
  sourceLayout->addWidget(mSource, 1);
  sourceLayout->addWidget(browse);
  row(AppLanguage::source("训练输出目录"), sourceRow);
  mSourceSummary = new QLabel(this);
  mSourceSummary->setWordWrap(true);
  form->addRow(mSourceSummary);
  mMode = new QComboBox(this);
  mMode->setObjectName(QStringLiteral("meshMethodCombo"));
  const QStringList modes = {QStringLiteral("bounded"), QStringLiteral("unbounded"),
                            QStringLiteral("sugar"), QStringLiteral("gs2mesh"), QStringLiteral("gs_depth")};
  const char *labels[] = {AppLanguage::source("2DGS 有界 TSDF"), AppLanguage::source("2DGS 无界 TSDF"),
                         AppLanguage::source("SuGaR 网格优化"), AppLanguage::source("GS2Mesh 深度融合"),
                         AppLanguage::source("GS 深度点云（3DGS / 2DGS）")};
  for (int i = 0; i < modes.size(); ++i) {
    mMode->addItem(QString(), modes[i]);
    AppLanguage::bindComboItem(mMode, i, labels[i]);
  }
  row(AppLanguage::source("方法"), mMode);
  auto *datasetRow = new QWidget(this);
  auto *datasetLayout = new QHBoxLayout(datasetRow);
  datasetLayout->setContentsMargins(0, 0, 0, 0);
  mDataset = new QLineEdit(datasetRow);
  mDataset->setObjectName(QStringLiteral("geometryCameraDataset"));
  auto *datasetBrowse = AppLanguage::text(new QPushButton(datasetRow), AppLanguage::source("浏览..."));
  datasetLayout->addWidget(mDataset, 1); datasetLayout->addWidget(datasetBrowse);
  row(AppLanguage::source("已知相机数据集"), datasetRow);
  connect(datasetBrowse, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this,
        QCoreApplication::translate("Workbench", "选择规范相机数据集"), mDataset->text(),
        QCoreApplication::translate("Workbench", "JSON 文件 (*.json);;所有文件 (*)"));
    if (!path.isEmpty()) mDataset->setText(QDir::toNativeSeparators(path));
  });
  mCoordinateMode = new QComboBox(this);
  mCoordinateMode->setObjectName(QStringLiteral("geometryCoordinateMode"));
  for (const auto &item : {QPair<const char *, const char *>{"canonical", AppLanguage::source("规范坐标")},
       {"source", AppLanguage::source("源坐标")}, {"training", AppLanguage::source("训练坐标")}}) {
    const int index = mCoordinateMode->count();
    mCoordinateMode->addItem(QString(), QString::fromLatin1(item.first));
    AppLanguage::bindComboItem(mCoordinateMode, index, item.second);
  }
  // The existing Gaussian PLY is in the executed training frame. Do not
  // mistake unit conversion for the missing origin/rotation conversion.
  mCoordinateMode->setCurrentIndex(mCoordinateMode->findData(QStringLiteral("training")));
  row(AppLanguage::source("输出坐标框架"), mCoordinateMode);
  mAlpha = new QDoubleSpinBox(this); mAlpha->setObjectName(QStringLiteral("geometryAlphaThreshold"));
  mAlpha->setRange(.000001, 1.); mAlpha->setDecimals(6); mAlpha->setValue(.5); mAlpha->setSingleStep(.05);
  row(AppLanguage::source("透明度有效阈值"), mAlpha);
  mPixelStride = new QSpinBox(this); mPixelStride->setObjectName(QStringLiteral("geometryPixelStride"));
  mPixelStride->setRange(1, 1000000); mPixelStride->setValue(1);
  row(AppLanguage::source("像素采样步长"), mPixelStride);
  mMaxPoints = new QSpinBox(this); mMaxPoints->setObjectName(QStringLiteral("geometryMaxPoints"));
  mMaxPoints->setRange(1, 2000000000); mMaxPoints->setValue(50000000);
  row(AppLanguage::source("最大输出点数"), mMaxPoints);
  mMemoryBudget = new QSpinBox(this); mMemoryBudget->setObjectName(QStringLiteral("geometryMemoryBudget"));
  mMemoryBudget->setRange(16, 1048576); mMemoryBudget->setValue(512);
  row(AppLanguage::source("内存预算（MiB）"), mMemoryBudget);
  mResolution = new QSpinBox(this);
  mResolution->setRange(64, 4096);
  mResolution->setValue(512);
  row(AppLanguage::source("TSDF 分辨率"), mResolution);
  mClusters = new QSpinBox(this);
  mClusters->setRange(1, 500);
  mClusters->setValue(50);
  row(AppLanguage::source("保留连通分量数"), mClusters);
  mQuality = new QComboBox(this);
  mQuality->addItem(QString(), QStringLiteral("low"));
  mQuality->addItem(QString(), QStringLiteral("high"));
  AppLanguage::bindComboItem(mQuality, 0, AppLanguage::source("快速预览"));
  AppLanguage::bindComboItem(mQuality, 1, AppLanguage::source("高质量"));
  row(AppLanguage::source("SuGaR 质量"), mQuality);
  mDownsample = new QSpinBox(this);
  mDownsample->setRange(1, 8);
  mDownsample->setValue(2);
  row(AppLanguage::source("GS2Mesh 下采样倍数"), mDownsample);
  mTexture = AppLanguage::text(new WrappingCheckBox(this), AppLanguage::source("完成网格后使用 OpenMVS 烘焙照片纹理"));
  form->addRow(mTexture);
  mTextureResolution = new QSpinBox(this);
  mTextureResolution->setRange(512, 8192);
  mTextureResolution->setValue(2048);
  row(AppLanguage::source("纹理分辨率"), mTextureResolution);
  bodyLayout->addLayout(form);
  mHint = new QLabel(this);
  mHint->setWordWrap(true);
  mHint->setObjectName(QStringLiteral("meshPipelineHint"));
  bodyLayout->addWidget(mHint);
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
  buttons->addButton(AppLanguage::text(new QPushButton(this), AppLanguage::source("开始生成")), QDialogButtonBox::AcceptRole);
  layout->addWidget(buttons);
  connect(browse, &QPushButton::clicked, this, [this]() {
    const QString path = QFileDialog::getExistingDirectory(this,
        QCoreApplication::translate("Workbench", "选择训练输出目录"), mSource->text());
    if (!path.isEmpty()) mSource->setText(QDir::toNativeSeparators(path));
  });
  connect(mSource, &QLineEdit::textChanged, this, &MeshGenerationDialog::refreshSource);
  connect(mMode, &QComboBox::currentIndexChanged, this, &MeshGenerationDialog::refreshOptions);
  connect(mTexture, &QCheckBox::toggled, this, &MeshGenerationDialog::refreshOptions);
  connect(buttons, &QDialogButtonBox::accepted, this, &MeshGenerationDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  AppLanguage::onChanged(this, [this]() { refreshOptions(); });
  refreshSource();
}

void MeshGenerationDialog::refreshSource() {
  mBackend.clear();
  mIteration = 0;
  const QDir source(mSource->text());
  const QRegularExpression pattern(QStringLiteral("^iteration_(\\d+)$"));
  const QDir clouds(source.filePath(QStringLiteral("point_cloud")));
  for (const QString &name : clouds.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    const auto match = pattern.match(name);
    if (match.hasMatch() && QFileInfo::exists(clouds.filePath(name + QStringLiteral("/point_cloud.ply"))))
      mIteration = std::max(mIteration, match.captured(1).toInt());
  }
  if (QFileInfo::exists(source.filePath(QStringLiteral("cfg_args"))) && mIteration > 0) {
    QFile metadata(source.filePath(QStringLiteral("training_backend.json")));
    if (metadata.open(QIODevice::ReadOnly))
      mBackend = QJsonDocument::fromJson(metadata.readAll()).object().value(QStringLiteral("backend")).toString();
    if (mBackend.isEmpty()) {
      QString error;
      const auto ply = WorkspaceDocument::inspectPly(source.filePath(
          QStringLiteral("point_cloud/iteration_%1/point_cloud.ply").arg(mIteration)), &error);
      if (ply.looksLikeGaussianSplat())
        mBackend = ply.properties.contains(QStringLiteral("scale_2")) ? QStringLiteral("3dgs") : QStringLiteral("2dgs");
    }
  }
  const bool canonical = QFileInfo::exists(source.filePath(QStringLiteral("training-camera-contract.json")));
  if (canonical) mMode->setCurrentIndex(mMode->findData(QStringLiteral("gs_depth")));
  else {
    const QString mode = mMode->currentData().toString();
    const bool tsdf = mode == QStringLiteral("bounded") || mode == QStringLiteral("unbounded");
    if (mBackend == QStringLiteral("2dgs") && !tsdf && mode != QStringLiteral("gs_depth")) mMode->setCurrentIndex(0);
    if (mBackend == QStringLiteral("3dgs") && tsdf) mMode->setCurrentIndex(2);
  }
  refreshOptions();
}

void MeshGenerationDialog::refreshOptions() {
  const QString mode = mMode->currentData().toString();
  const bool tsdf = mode == QStringLiteral("bounded") || mode == QStringLiteral("unbounded");
  const bool depth = mode == QStringLiteral("gs_depth");
  mResolution->setEnabled(tsdf);
  mClusters->setEnabled(tsdf);
  mQuality->setEnabled(mode == QStringLiteral("sugar"));
  mDownsample->setEnabled(mode == QStringLiteral("gs2mesh"));
  mTexture->setEnabled(mode != QStringLiteral("sugar") && !depth);
  for (auto *field : {static_cast<QWidget *>(mDataset), static_cast<QWidget *>(mCoordinateMode),
       static_cast<QWidget *>(mAlpha), static_cast<QWidget *>(mPixelStride),
       static_cast<QWidget *>(mMaxPoints), static_cast<QWidget *>(mMemoryBudget)}) field->setEnabled(depth);
  mTextureResolution->setEnabled(mTexture->isEnabled() && mTexture->isChecked());
  mSourceSummary->setText(mBackend.isEmpty()
      ? QCoreApplication::translate("Workbench", "需要包含 cfg_args 和 point_cloud/iteration_* 的训练输出；普通导入 PLY 不含所需相机数据。")
      : QCoreApplication::translate("Workbench", "源模型：%1 · 迭代 %2").arg(mBackend.toUpper()).arg(mIteration));
  mHint->setText(depth ? QCoreApplication::translate("Workbench", "深度点云使用训练视点和有效掩膜，独立另存；不是高斯中心点云，也不保证真实材料边界。透明或高反射区域可能缺失。不融合、不重跑 SfM，照片纹理烘焙不可用于此路线。") + QLatin1Char('\n') +
      QCoreApplication::translate("Workbench", "原生默认使用训练框架与原高斯模型对照。规范或源框架成果将保存但不自动叠加；请用于独立评价或同框架工程，不会猜测配准。")
      : QCoreApplication::translate("Workbench", "独立保存，不覆盖原模型。处理时保留画面，验证后自动显示网格及照片纹理。多张贴图按原分辨率合成预览图集；超出预算或不支持的材质保留几何和纹理包。GLB 取决于后端转换；尚不支持阶段恢复。"));
}

void MeshGenerationDialog::setCameraDatasetPath(const QString &path) {
  mDataset->setText(QDir::toNativeSeparators(path));
}

QJsonObject MeshGenerationDialog::configuration() const {
  if (mMode->currentData() == QStringLiteral("gs_depth")) {
    return {{"modelDirectory", QDir::cleanPath(mSource->text())}, {"iteration", mIteration},
        {"mode", "gs_depth"}, {"bakeTexture", false},
        {"cameraDatasetPath", QDir::cleanPath(mDataset->text())},
        {"meshOptions", QJsonObject{{"coordinate_mode", mCoordinateMode->currentData().toString()},
            {"alpha_threshold", mAlpha->value()}, {"pixel_stride", mPixelStride->value()},
            {"max_points", mMaxPoints->value()}, {"memory_budget_mb", mMemoryBudget->value()}}}};
  }
  return {{QStringLiteral("modelDirectory"), QDir::cleanPath(mSource->text())},
          {QStringLiteral("iteration"), mIteration}, {QStringLiteral("mode"), mMode->currentData().toString()},
          {QStringLiteral("bakeTexture"), mTexture->isEnabled() && mTexture->isChecked()},
          {QStringLiteral("textureResolution"), mTextureResolution->value()},
          {QStringLiteral("meshOptions"), QJsonObject{
              {QStringLiteral("mesh_res"), mResolution->value()},
              {QStringLiteral("num_cluster"), mClusters->value()},
              {QStringLiteral("sugar_quality"), mQuality->currentData().toString()},
              {QStringLiteral("gs2mesh_downsample"), mDownsample->value()}}}};
}

void MeshGenerationDialog::accept() {
  const QString mode = mMode->currentData().toString();
  if (mode == QStringLiteral("gs_depth")) {
    if (mIteration <= 0 || (mBackend != QStringLiteral("3dgs") && mBackend != QStringLiteral("2dgs")) ||
        !QFileInfo::exists(QDir(mSource->text()).filePath(QStringLiteral("training-camera-contract.json")))) {
      QMessageBox::warning(this, QCoreApplication::translate("Workbench", "无法生成网格"),
          QCoreApplication::translate("Workbench", "此方法需要带完整相机合同的 3DGS 或 2DGS 训练产物。"));
      return;
    }
    if (mDataset->text().trimmed().isEmpty() || !QFileInfo::exists(mDataset->text())) {
      QMessageBox::warning(this, QCoreApplication::translate("Workbench", "无法生成网格"),
          QCoreApplication::translate("Workbench", "请选择与训练合同身份一致的规范相机数据集。不会重新求解相机。"));
      return;
    }
    QDialog::accept();
    return;
  }
  const bool tsdf = mode == QStringLiteral("bounded") || mode == QStringLiteral("unbounded");
  if (mIteration <= 0 || (tsdf ? mBackend != QStringLiteral("2dgs") : mBackend != QStringLiteral("3dgs"))) {
    QMessageBox::warning(this, QCoreApplication::translate("Workbench", "无法生成网格"),
        QCoreApplication::translate("Workbench", "所选方法与训练输出不匹配。TSDF 需要 2DGS；SuGaR / GS2Mesh 需要 3DGS。"));
    return;
  }
  QDialog::accept();
}
} // namespace gsw

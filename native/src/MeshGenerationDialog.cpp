#include "MeshGenerationDialog.h"
#include "AppLanguage.h"
#include "WorkspaceDocument.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
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
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

namespace gsw {
MeshGenerationDialog::MeshGenerationDialog(const QString &modelDirectory, QWidget *parent)
    : QDialog(parent) {
  setObjectName(QStringLiteral("meshGenerationDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("生成网格"));
  setMinimumWidth(560);
  auto *layout = new QVBoxLayout(this);
  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  auto row = [form](const char *caption, QWidget *field) {
    form->addRow(AppLanguage::text(new QLabel, caption), field);
  };
  auto *sourceRow = new QWidget(this);
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
                            QStringLiteral("sugar"), QStringLiteral("gs2mesh")};
  const char *labels[] = {AppLanguage::source("2DGS 有界 TSDF"), AppLanguage::source("2DGS 无界 TSDF"),
                         AppLanguage::source("SuGaR 网格优化"), AppLanguage::source("GS2Mesh 深度融合")};
  for (int i = 0; i < modes.size(); ++i) {
    mMode->addItem(QString(), modes[i]);
    AppLanguage::bindComboItem(mMode, i, labels[i]);
  }
  row(AppLanguage::source("方法"), mMode);
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
  mTexture = AppLanguage::text(new QCheckBox(this), AppLanguage::source("完成网格后使用 OpenMVS 烘焙照片纹理"));
  form->addRow(mTexture);
  mTextureResolution = new QSpinBox(this);
  mTextureResolution->setRange(512, 8192);
  mTextureResolution->setValue(2048);
  row(AppLanguage::source("纹理分辨率"), mTextureResolution);
  layout->addLayout(form);
  mHint = new QLabel(this);
  mHint->setWordWrap(true);
  mHint->setObjectName(QStringLiteral("meshPipelineHint"));
  layout->addWidget(mHint);
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
  const bool tsdf = mMode->currentIndex() < 2;
  if (mBackend == QStringLiteral("2dgs") && !tsdf) mMode->setCurrentIndex(0);
  if (mBackend == QStringLiteral("3dgs") && tsdf) mMode->setCurrentIndex(2);
  refreshOptions();
}

void MeshGenerationDialog::refreshOptions() {
  const QString mode = mMode->currentData().toString();
  const bool tsdf = mode == QStringLiteral("bounded") || mode == QStringLiteral("unbounded");
  mResolution->setEnabled(tsdf);
  mClusters->setEnabled(tsdf);
  mQuality->setEnabled(mode == QStringLiteral("sugar"));
  mDownsample->setEnabled(mode == QStringLiteral("gs2mesh"));
  mTexture->setEnabled(mode != QStringLiteral("sugar"));
  mTextureResolution->setEnabled(mTexture->isEnabled() && mTexture->isChecked());
  mSourceSummary->setText(mBackend.isEmpty()
      ? QCoreApplication::translate("Workbench", "需要包含 cfg_args 和 point_cloud/iteration_* 的训练输出；普通导入 PLY 不含所需相机数据。")
      : QCoreApplication::translate("Workbench", "源模型：%1 · 迭代 %2").arg(mBackend.toUpper()).arg(mIteration));
  mHint->setText(QCoreApplication::translate("Workbench", "独立保存，不覆盖原模型。处理时保留画面，验证后自动显示网格及照片纹理。多张贴图按原分辨率合成预览图集；超出预算或不支持的材质保留几何和纹理包。GLB 取决于后端转换；尚不支持阶段恢复。"));
}

QJsonObject MeshGenerationDialog::configuration() const {
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
  const bool tsdf = mMode->currentIndex() < 2;
  if (mIteration <= 0 || (tsdf ? mBackend != QStringLiteral("2dgs") : mBackend != QStringLiteral("3dgs"))) {
    QMessageBox::warning(this, QCoreApplication::translate("Workbench", "无法生成网格"),
        QCoreApplication::translate("Workbench", "所选方法与训练输出不匹配。TSDF 需要 2DGS；SuGaR / GS2Mesh 需要 3DGS。"));
    return;
  }
  QDialog::accept();
}
} // namespace gsw

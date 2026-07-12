#include "ReconstructionDialog.h"

#include "ColmapSupport.h"

#include <QCheckBox>
#include <QComboBox>
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
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace gsw {

ReconstructionDialog::ReconstructionDialog(const QString &datasetPath,
                                           const QString &repositoryRoot,
                                           QWidget *parent)
    : QDialog(parent), mDatasetPath(datasetPath),
      mHasExistingData(hasColmapWorkingData(datasetPath)) {
  setWindowTitle(QStringLiteral("COLMAP 稀疏重建"));
  setModal(true);
  setMinimumWidth(580);

  QSettings settings;
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
  form->addRow(QStringLiteral("数据集"), datasetValue);

  auto *executableRow = new QWidget(this);
  auto *executableLayout = new QHBoxLayout(executableRow);
  executableLayout->setContentsMargins(0, 0, 0, 0);
  executableLayout->setSpacing(6);
  const QString savedExecutable =
      settings.value(QStringLiteral("reconstruction/colmapExecutable"))
          .toString();
  mExecutable = new QLineEdit(
      findColmapExecutable(repositoryRoot, savedExecutable), executableRow);
  mExecutable->setPlaceholderText(QStringLiteral("选择 E 盘上的 colmap.exe"));
  auto *browseButton = new QPushButton(QStringLiteral("浏览..."), executableRow);
  browseButton->setToolTip(QStringLiteral("选择 COLMAP 可执行文件"));
  executableLayout->addWidget(mExecutable, 1);
  executableLayout->addWidget(browseButton);
  form->addRow(QStringLiteral("COLMAP"), executableRow);

  mPreset = new QComboBox(this);
  mPreset->addItem(QStringLiteral("标准"), QStringLiteral("default"));
  mPreset->addItem(QStringLiteral("鲁棒（弱纹理/视角差异大）"),
                   QStringLiteral("robust"));
  mPreset->addItem(QStringLiteral("序列（视频帧/连续航拍）"),
                   QStringLiteral("sequential"));
  int savedPreset = mPreset->findData(
      settings.value(QStringLiteral("reconstruction/preset"),
                     QStringLiteral("default"))
          .toString());
  mPreset->setCurrentIndex(std::max(0, savedPreset));
  form->addRow(QStringLiteral("预设"), mPreset);

  mMatching = new QComboBox(this);
  mMatching->addItem(QStringLiteral("穷举匹配"), QStringLiteral("exhaustive"));
  mMatching->addItem(QStringLiteral("序列匹配"), QStringLiteral("sequential"));
  form->addRow(QStringLiteral("匹配方式"), mMatching);

  mCameraModel = new QComboBox(this);
  for (const auto &[label, value] :
       {std::pair{QStringLiteral("OPENCV（常用相机）"),
                  QStringLiteral("OPENCV")},
        std::pair{QStringLiteral("PINHOLE（已校正）"),
                  QStringLiteral("PINHOLE")},
        std::pair{QStringLiteral("SIMPLE_PINHOLE"),
                  QStringLiteral("SIMPLE_PINHOLE")},
        std::pair{QStringLiteral("RADIAL"), QStringLiteral("RADIAL")},
        std::pair{QStringLiteral("SIMPLE_RADIAL"),
                  QStringLiteral("SIMPLE_RADIAL")}}) {
    mCameraModel->addItem(label, value);
  }
  const int savedCamera = mCameraModel->findData(
      settings.value(QStringLiteral("reconstruction/cameraModel"),
                     QStringLiteral("OPENCV"))
          .toString());
  mCameraModel->setCurrentIndex(std::max(0, savedCamera));
  form->addRow(QStringLiteral("相机模型"), mCameraModel);

  mMaxImageSize = new QComboBox(this);
  mMaxImageSize->addItem(QStringLiteral("原始分辨率"), -1);
  mMaxImageSize->addItem(QStringLiteral("最长边 2400 px"), 2400);
  mMaxImageSize->addItem(QStringLiteral("最长边 1600 px"), 1600);
  mMaxImageSize->addItem(QStringLiteral("最长边 1200 px"), 1200);
  form->addRow(QStringLiteral("特征分辨率"), mMaxImageSize);

  mMaxFeatures = new QSpinBox(this);
  mMaxFeatures->setRange(512, 50000);
  mMaxFeatures->setSingleStep(512);
  mMaxFeatures->setValue(8192);
  form->addRow(QStringLiteral("每图最大特征"), mMaxFeatures);

  mRuntimeMinutes = new QSpinBox(this);
  mRuntimeMinutes->setRange(0, 1440);
  mRuntimeMinutes->setSuffix(QStringLiteral(" 分钟"));
  mRuntimeMinutes->setSpecialValueText(QStringLiteral("不限时"));
  form->addRow(QStringLiteral("Mapper 限时"), mRuntimeMinutes);

  mUseGpu = new QCheckBox(QStringLiteral("使用 GPU 进行特征提取和匹配"), this);
  mUseGpu->setChecked(
      settings.value(QStringLiteral("reconstruction/useGpu"), true).toBool());
  form->addRow(QString(), mUseGpu);

  mSingleCamera = new QCheckBox(QStringLiteral("所有图像共用一套相机参数"), this);
  mSingleCamera->setChecked(
      settings.value(QStringLiteral("reconstruction/singleCamera"), true)
          .toBool());
  form->addRow(QString(), mSingleCamera);

  mReset = new QCheckBox(QStringLiteral("重建前清理旧 COLMAP 缓存"), this);
  mReset->setChecked(!mHasExistingData);
  form->addRow(QString(), mReset);

  rootLayout->addLayout(form);

  auto *note = new QLabel(
      mHasExistingData
          ? QStringLiteral("检测到已有重建或缓存。为避免误覆盖，必须明确启用清理后才能重新运行。")
          : QStringLiteral("结果写入数据集的 sparse/0；混合图像尺寸会由后端自动关闭单相机模式。"),
      this);
  note->setObjectName(QStringLiteral("mutedLabel"));
  note->setWordWrap(true);
  rootLayout->addWidget(note);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
  auto *startButton =
      buttons->addButton(QStringLiteral("开始重建"), QDialogButtonBox::AcceptRole);
  startButton->setDefault(true);
  rootLayout->addWidget(buttons);

  connect(mPreset, &QComboBox::currentIndexChanged, this,
          &ReconstructionDialog::applyPreset);
  connect(browseButton, &QPushButton::clicked, this,
          &ReconstructionDialog::chooseExecutable);
  connect(buttons, &QDialogButtonBox::accepted, this,
          &ReconstructionDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this,
          &ReconstructionDialog::reject);
  applyPreset();
}

ReconstructionConfiguration ReconstructionDialog::configuration() const {
  ReconstructionConfiguration result;
  const QString executable = mExecutable->text().trimmed();
  result.colmapExecutable = executable.isEmpty()
                                ? QString()
                                : QDir::toNativeSeparators(
                                      QDir::cleanPath(executable));
  result.preset = mPreset->currentData().toString();
  result.matching = mMatching->currentData().toString();
  result.cameraModel = mCameraModel->currentData().toString();
  result.featureMaxImageSize = mMaxImageSize->currentData().toInt();
  result.featureMaxNumFeatures = mMaxFeatures->value();
  result.mapperMaxRuntimeSeconds =
      mRuntimeMinutes->value() == 0 ? -1 : mRuntimeMinutes->value() * 60;
  result.matcherGuided = result.preset != QStringLiteral("default");
  result.useGpu = mUseGpu->isChecked();
  result.singleCamera = mSingleCamera->isChecked();
  result.reset = mReset->isChecked();
  return result;
}

void ReconstructionDialog::accept() {
  if (datasetImageDirectory(mDatasetPath).isEmpty()) {
    QMessageBox::critical(
        this, QStringLiteral("数据集没有图像"),
        QStringLiteral("数据集根目录、input 或 images 目录中必须至少包含一张受支持的图像。"));
    return;
  }

  const ReconstructionConfiguration config = configuration();
  const QFileInfo executable(config.colmapExecutable);
  if (!executable.exists() || !executable.isFile()) {
    QMessageBox::critical(
        this, QStringLiteral("COLMAP 不可用"),
        QStringLiteral("未找到有效的 colmap.exe。请选择迁移到 E 盘后的实际文件。"));
    return;
  }
  if (mHasExistingData && !config.reset) {
    QMessageBox::critical(
        this, QStringLiteral("已有重建数据"),
        QStringLiteral("请启用“重建前清理旧 COLMAP 缓存”，或取消本次重建。"));
    return;
  }
  if (mHasExistingData) {
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认重新运行 COLMAP"),
        QStringLiteral("开始后会替换以下数据集中的 distorted、sparse 和 stereo 缓存：\n%1")
            .arg(QDir::toNativeSeparators(mDatasetPath)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
      return;
    }
  }

  QSettings settings;
  settings.setValue(QStringLiteral("reconstruction/colmapExecutable"),
                    config.colmapExecutable);
  settings.setValue(QStringLiteral("reconstruction/preset"), config.preset);
  settings.setValue(QStringLiteral("reconstruction/cameraModel"),
                    config.cameraModel);
  settings.setValue(QStringLiteral("reconstruction/useGpu"), config.useGpu);
  settings.setValue(QStringLiteral("reconstruction/singleCamera"),
                    config.singleCamera);
  QDialog::accept();
}

void ReconstructionDialog::applyPreset() {
  const QString preset = mPreset->currentData().toString();
  const bool robust = preset == QStringLiteral("robust");
  const bool sequential = preset == QStringLiteral("sequential");
  mMatching->setCurrentIndex(
      mMatching->findData(sequential ? QStringLiteral("sequential")
                                     : QStringLiteral("exhaustive")));
  mMaxFeatures->setValue(robust || sequential ? 12000 : 8192);
  mMaxImageSize->setCurrentIndex(
      mMaxImageSize->findData(robust || sequential ? 2400 : -1));
}

void ReconstructionDialog::chooseExecutable() {
  const QString current = mExecutable->text().trimmed();
  const QString filePath = QFileDialog::getOpenFileName(
      this, QStringLiteral("选择 COLMAP"),
      current.isEmpty() ? QString() : QFileInfo(current).absolutePath(),
      QStringLiteral("COLMAP (colmap.exe);;Executable (*.exe);;All files (*.*)"));
  if (!filePath.isEmpty()) {
    mExecutable->setText(QDir::toNativeSeparators(filePath));
  }
}

} // namespace gsw

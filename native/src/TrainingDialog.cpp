#include "TrainingDialog.h"

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
#include <QSpinBox>
#include <QVBoxLayout>

namespace gsw {

namespace {
QString safeSceneName(QString value) {
  value = value.trimmed().toLower();
  value.replace(QRegularExpression(QStringLiteral("[^a-z0-9_.-]+")), QStringLiteral("-"));
  value.replace(QRegularExpression(QStringLiteral("^-+|-+$")), QString());
  if (value.isEmpty()) {
    value = QStringLiteral("scene");
  }
  return value + QDateTime::currentDateTime().toString(QStringLiteral("-yyyyMMdd-HHmmss"));
}

} // namespace

TrainingDialog::TrainingDialog(const QString &datasetPath, const QString &projectName,
                               const QString &defaultOutputRoot,
                               const bool hasSparseReconstruction,
                               const bool twoDgsAvailable, QWidget *parent)
    : QDialog(parent), mDatasetPath(datasetPath) {
  setWindowTitle(QStringLiteral("训练设置"));
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
  form->addRow(QStringLiteral("数据集"), datasetValue);

  mBackend = new QComboBox(this);
  mBackend->addItem(QStringLiteral("3D Gaussian Splatting"), QStringLiteral("3dgs"));
  if (twoDgsAvailable) {
    mBackend->addItem(QStringLiteral("2D Gaussian Splatting"), QStringLiteral("2dgs"));
  }
  form->addRow(QStringLiteral("方法"), mBackend);

  mQuality = new QComboBox(this);
  mQuality->addItem(QStringLiteral("快速预览"), QStringLiteral("quick"));
  mQuality->addItem(QStringLiteral("标准"), QStringLiteral("full"));
  mQuality->addItem(QStringLiteral("高质量"), QStringLiteral("quality"));
  mQuality->addItem(QStringLiteral("最高质量"), QStringLiteral("max_quality"));
  form->addRow(QStringLiteral("质量预设"), mQuality);

  mIterations = new QSpinBox(this);
  mIterations->setRange(1000, 200000);
  mIterations->setSingleStep(1000);
  form->addRow(QStringLiteral("迭代次数"), mIterations);

  mResolution = new QComboBox(this);
  for (const int resolution : {1, 2, 4, 8}) {
    mResolution->addItem(QStringLiteral("1/%1").arg(resolution), resolution);
  }
  form->addRow(QStringLiteral("训练分辨率"), mResolution);

  mOutputScene = new QLineEdit(safeSceneName(projectName), this);
  mOutputScene->setPlaceholderText(QStringLiteral("仅允许英文、数字、点、下划线和连字符"));
  form->addRow(QStringLiteral("输出名称"), mOutputScene);

  auto *outputRow = new QWidget(this);
  auto *outputLayout = new QHBoxLayout(outputRow);
  outputLayout->setContentsMargins(0, 0, 0, 0);
  outputLayout->setSpacing(6);
  mOutputRoot = new QLineEdit(QDir::toNativeSeparators(defaultOutputRoot), outputRow);
  auto *browseButton = new QPushButton(QStringLiteral("浏览..."), outputRow);
  browseButton->setToolTip(QStringLiteral("选择训练输出目录"));
  outputLayout->addWidget(mOutputRoot, 1);
  outputLayout->addWidget(browseButton);
  form->addRow(QStringLiteral("输出目录"), outputRow);

  mRunColmap = new QCheckBox(QStringLiteral("训练前运行 COLMAP 重建"), this);
  mRunColmap->setChecked(!hasSparseReconstruction);
  form->addRow(QString(), mRunColmap);

  mOverwrite = new QCheckBox(QStringLiteral("允许覆盖同名输出"), this);
  mOverwrite->setChecked(false);
  form->addRow(QString(), mOverwrite);

  rootLayout->addLayout(form);

  auto *note = new QLabel(
      QStringLiteral("输出默认写入工程所在磁盘。检测到已有稀疏重建时可关闭 COLMAP；关闭后若数据不可训练，任务会自动尝试恢复或重建。"),
      this);
  note->setObjectName(QStringLiteral("mutedLabel"));
  note->setWordWrap(true);
  rootLayout->addWidget(note);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
  auto *startButton = buttons->addButton(QStringLiteral("开始训练"), QDialogButtonBox::AcceptRole);
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
  result.outputScene = mOutputScene->text().trimmed();
  result.iterations = mIterations->value();
  result.resolution = mResolution->currentData().toInt();
  result.runColmap = mRunColmap->isChecked();
  result.overwrite = mOverwrite->isChecked();
  return result;
}

void TrainingDialog::accept() {
  if (!datasetContainsImages()) {
    QMessageBox::critical(
        this, QStringLiteral("数据集不可训练"),
        QStringLiteral("所选目录必须包含 images 或 input 子目录，并且其中至少有一张图像。请重新导入数据集根目录。"));
    return;
  }

  const TrainingConfiguration config = configuration();
  static const QRegularExpression validSceneName(QStringLiteral("^[A-Za-z0-9_.-]+$"));
  if (!validSceneName.match(config.outputScene).hasMatch()) {
    QMessageBox::critical(this, QStringLiteral("输出名称无效"),
                          QStringLiteral("输出名称只能包含英文字母、数字、点、下划线和连字符。"));
    return;
  }
  if (config.outputRoot.isEmpty() ||
      (!QFileInfo::exists(config.outputRoot) && !QDir().mkpath(config.outputRoot))) {
    QMessageBox::critical(this, QStringLiteral("输出目录不可用"),
                          QStringLiteral("无法创建或访问指定的输出目录。"));
    return;
  }

  const QDir target(QDir(config.outputRoot).filePath(config.outputScene));
  const bool outputContainsData = target.exists() &&
                                  !target.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty();
  if (outputContainsData && !config.overwrite) {
    QMessageBox::critical(this, QStringLiteral("输出已存在"),
                          QStringLiteral("同名输出目录已有数据。请更改名称，或明确启用覆盖。"));
    return;
  }
  if (outputContainsData && config.overwrite) {
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("确认覆盖训练输出"),
        QStringLiteral("训练开始后会删除以下目录中的现有数据：\n%1")
            .arg(QDir::toNativeSeparators(target.absolutePath())),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
      return;
    }
  }
  QDialog::accept();
}

void TrainingDialog::applyPreset() {
  const QString backend = mBackend->currentData().toString();
  const QString quality = mQuality->currentData().toString();
  int iterations = quality == QStringLiteral("quick") ? 7000 : 30000;
  int resolution = 8;
  if (quality == QStringLiteral("quality")) {
    resolution = 4;
  } else if (quality == QStringLiteral("max_quality")) {
    resolution = 2;
  }
  if (backend == QStringLiteral("2dgs") && quality == QStringLiteral("max_quality")) {
    iterations = 30000;
  }
  mIterations->setValue(iterations);
  const int resolutionIndex = mResolution->findData(resolution);
  if (resolutionIndex >= 0) {
    mResolution->setCurrentIndex(resolutionIndex);
  }
}

void TrainingDialog::chooseOutputRoot() {
  const QString directory = QFileDialog::getExistingDirectory(
      this, QStringLiteral("选择训练输出目录"), mOutputRoot->text());
  if (!directory.isEmpty()) {
    mOutputRoot->setText(QDir::toNativeSeparators(directory));
  }
}

bool TrainingDialog::datasetContainsImages() const {
  return !datasetImageDirectory(mDatasetPath).isEmpty();
}

} // namespace gsw

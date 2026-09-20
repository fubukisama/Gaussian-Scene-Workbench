#include "ModelExportDialog.h"
#include "AppLanguage.h"
#include <QComboBox>
#include <QCoreApplication>
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
#include <QVBoxLayout>

namespace gsw {
ModelExportDialog::ModelExportDialog(const ModelExportOptions &source, bool mesh, bool gaussian,
    const QStringList &protectedPaths, QWidget *parent)
    : QDialog(parent), mSource(source), mGaussian(gaussian), mProtectedPaths(protectedPaths) {
  setObjectName(QStringLiteral("modelExportDialog"));
  AppLanguage::bind(this, "windowTitle", AppLanguage::source("导出模型"));
  resize(700, 350);
  auto *layout = new QVBoxLayout(this);
  auto *intro = AppLanguage::text(new QLabel(this), AppLanguage::source("导出当前活动模型，不合并其他模型；使用完整源数据并应用裁剪结果，不受预览点数限制。"));
  intro->setWordWrap(true); layout->addWidget(intro);
  auto *name = new QLabel(QDir::toNativeSeparators(source.sourcePath), this);
  name->setTextFormat(Qt::PlainText); name->setWordWrap(true); layout->addWidget(name);
  auto *form = new QFormLayout;
  layout->addLayout(form);
  mFormat = new QComboBox(this); mFormat->setObjectName(QStringLiteral("modelExportFormat"));
  const auto add = [&](ModelExportFormat format, const char *label) {
    mFormat->addItem({}, static_cast<int>(format));
    AppLanguage::bindComboItem(mFormat, mFormat->count() - 1, label);
  };
  add(ModelExportFormat::Ply, AppLanguage::source("PLY（保留源属性）"));
  if (gaussian) add(ModelExportFormat::Spz, AppLanguage::source("SPZ（压缩高斯）"));
  add(ModelExportFormat::Glb, AppLanguage::source("GLB（glTF 2.0）"));
  add(ModelExportFormat::Xyz, AppLanguage::source("XYZ（坐标与颜色）"));
  add(ModelExportFormat::Csv, AppLanguage::source("CSV（坐标与颜色）"));
  if (mesh) {
    add(ModelExportFormat::Obj, AppLanguage::source("OBJ（网格与贴图）"));
    add(ModelExportFormat::Stl, AppLanguage::source("STL（二进制三角网格）"));
  }
  form->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("文件格式")), mFormat);
  mCoordinates = new QComboBox(this); mCoordinates->setObjectName(QStringLiteral("modelExportCoordinates"));
  mCoordinates->addItem({}); mCoordinates->addItem({});
  AppLanguage::bindComboItem(mCoordinates, 0, AppLanguage::source("原始坐标（不应用模型变换）"));
  AppLanguage::bindComboItem(mCoordinates, 1, AppLanguage::source("场景坐标（应用位移、旋转、缩放）"));
  mCoordinates->setCurrentIndex(gaussian ? 0 : 1);
  form->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("导出坐标")), mCoordinates);
  mSpzOptions = new QWidget(this);
  auto *spzForm = new QFormLayout(mSpzOptions);
  spzForm->setContentsMargins(0, 0, 0, 0);
  mSpzVersion = new QComboBox(mSpzOptions); mSpzVersion->setObjectName(QStringLiteral("spzVersion"));
  mSpzVersion->addItem(QStringLiteral("SPZ v4 (Zstandard)"), 4);
  mSpzVersion->addItem(QStringLiteral("SPZ v3 (gzip)"), 3);
  const int versionIndex = mSpzVersion->findData(source.spzVersion);
  mSpzVersion->setCurrentIndex(versionIndex < 0 ? 0 : versionIndex);
  spzForm->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("SPZ 版本")), mSpzVersion);
  mSpzQuality = new QComboBox(mSpzOptions); mSpzQuality->setObjectName(QStringLiteral("spzQuality"));
  for (const char *key : {AppLanguage::source("紧凑（较小文件）"), AppLanguage::source("均衡（推荐）"), AppLanguage::source("高精度（较大文件）")}) {
    mSpzQuality->addItem({}); AppLanguage::bindComboItem(mSpzQuality, mSpzQuality->count() - 1, key);
  }
  mSpzQuality->setCurrentIndex(source.spzQuality >= 0 && source.spzQuality <= 2 ? source.spzQuality : 1);
  spzForm->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("压缩质量")), mSpzQuality);
  mSpzShDegree = new QComboBox(mSpzOptions); mSpzShDegree->setObjectName(QStringLiteral("spzShDegree"));
  mSpzShDegree->addItem({}, -1);
  AppLanguage::bindComboItem(mSpzShDegree, 0, AppLanguage::source("保留源球谐阶数"));
  for (int degree = 0; degree <= 4; ++degree) mSpzShDegree->addItem(QString::number(degree), degree);
  const int degreeIndex = mSpzShDegree->findData(source.spzMaximumShDegree);
  mSpzShDegree->setCurrentIndex(degreeIndex < 0 ? 0 : degreeIndex);
  spzForm->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("最大球谐阶数")), mSpzShDegree);
  layout->addWidget(mSpzOptions);
  auto *pathLayout = new QHBoxLayout;
  mPath = new QLineEdit(QFileInfo(source.sourcePath).dir().filePath(
      QFileInfo(source.sourcePath).completeBaseName() + QStringLiteral("-exported.ply")), this);
  mPath->setObjectName(QStringLiteral("modelExportPath"));
  auto *browse = AppLanguage::text(new QPushButton(this), AppLanguage::source("选择位置..."));
  pathLayout->addWidget(mPath, 1); pathLayout->addWidget(browse);
  form->addRow(AppLanguage::text(new QLabel(this), AppLanguage::source("输出文件")), pathLayout);
  mDescription = new QLabel(this); mDescription->setWordWrap(true);
  mDescription->setObjectName(QStringLiteral("modelExportDescription"));
  layout->addWidget(mDescription);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  auto *exportButton = buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
  exportButton->setObjectName(QStringLiteral("confirmModelExport"));
  AppLanguage::bind(exportButton, "text", AppLanguage::source("导出"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &ModelExportDialog::confirmExport);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(browse, &QPushButton::clicked, this, &ModelExportDialog::chooseFile);
  connect(mFormat, &QComboBox::currentIndexChanged, this, [this] {
    const QFileInfo path(mPath->text());
    const auto format = static_cast<ModelExportFormat>(mFormat->currentData().toInt());
    const QStringList suffixes{QStringLiteral("ply"), QStringLiteral("glb"), QStringLiteral("obj"),
        QStringLiteral("stl"), QStringLiteral("xyz"), QStringLiteral("csv"), QStringLiteral("spz")};
    const QString base = suffixes.contains(path.suffix().toLower()) ? path.completeBaseName() : path.fileName();
    mPath->setText(path.dir().filePath(base + QLatin1Char('.') + modelExportSuffix(format)));
    refreshDescription();
  });
  AppLanguage::onChanged(this, [this] { refreshDescription(); });
  refreshDescription();
}

ModelExportOptions ModelExportDialog::options() const {
  auto result = mSource;
  result.format = static_cast<ModelExportFormat>(mFormat->currentData().toInt());
  result.destinationPath = mPath->text();
  result.applyTransform = mCoordinates->currentIndex() == 1;
  result.spzVersion = mSpzVersion->currentData().toInt();
  result.spzQuality = mSpzQuality->currentIndex();
  result.spzMaximumShDegree = mSpzShDegree->currentData().toInt();
  return result;
}

void ModelExportDialog::refreshDescription() {
  const auto format = static_cast<ModelExportFormat>(mFormat->currentData().toInt());
  const bool gaussianPly = mGaussian && format == ModelExportFormat::Ply;
  const bool spz = format == ModelExportFormat::Spz;
  mSpzOptions->setVisible(spz);
  mCoordinates->setEnabled(!gaussianPly && !spz);
  if (gaussianPly || spz) mCoordinates->setCurrentIndex(0);
  QString description;
  switch (format) {
  case ModelExportFormat::Spz:
    description = QCoreApplication::translate("Workbench", "SPZ 保留高斯尺度、旋转、不透明度与所选球谐外观；有损量化，不减少高斯数量。仅导出原始坐标，范围须在 ±2048 源单位内；不保存 CRS、单位和自定义属性。v4 更快，v3 用于旧版查看器。降低球谐阶数会减少视角相关细节；高精度也不是无损。取消将在当前编解码阶段结束后生效。"); break;
  case ModelExportFormat::Ply:
    description = QCoreApplication::translate("Workbench", "保留 PLY 顶点属性、网格拓扑与 UV；贴图复制到配套资源目录。原始文件不会被修改。"); break;
  case ModelExportFormat::Obj:
    description = QCoreApplication::translate("Workbench", "导出完整三角网格、颜色、法线与 UV，并生成 MTL 和贴图资源目录；不保存自定义顶点属性。"); break;
  case ModelExportFormat::Stl:
    description = QCoreApplication::translate("Workbench", "STL 仅保存三角形几何，不包含颜色、贴图或单位信息；坐标使用源模型单位。"); break;
  case ModelExportFormat::Glb:
    description = QCoreApplication::translate("Workbench", "GLB 保存完整网格或点云，嵌入颜色、法线、UV 与贴图；转换为 Y 向上和米制。未声明单位时按 1 单位 = 1 m 处理。单文件须小于 4 GiB。"); break;
  case ModelExportFormat::Xyz:
  case ModelExportFormat::Csv:
    description = QCoreApplication::translate("Workbench", "导出 X、Y、Z 与 RGB（0–255），不包含网格面、贴图或其他顶点属性；坐标使用源模型单位。"); break;
  }
  if (mGaussian && !spz) description += QLatin1Char('\n') + (gaussianPly
      ? QCoreApplication::translate("Workbench", "Gaussian PLY export preserves source coordinates and all attributes; baking Gaussian transforms is not supported yet.")
      : QCoreApplication::translate("Workbench", "注意：此格式仅导出高斯中心点及基础颜色，不包含高斯尺度、旋转、不透明度或球谐外观。"));
  mDescription->setText(description);
}

void ModelExportDialog::chooseFile() {
  const auto format = static_cast<ModelExportFormat>(mFormat->currentData().toInt());
  const QString filter = QStringLiteral("%1 (*.%2)").arg(modelExportSuffix(format).toUpper(), modelExportSuffix(format));
  const QString path = QFileDialog::getSaveFileName(this, QCoreApplication::translate("Workbench", "导出模型"),
      mPath->text(), filter, nullptr, QFileDialog::DontConfirmOverwrite);
  if (!path.isEmpty()) mPath->setText(path);
}

void ModelExportDialog::confirmExport() {
  const auto format = static_cast<ModelExportFormat>(mFormat->currentData().toInt());
  QString path = mPath->text();
  if (path.trimmed().isEmpty()) return;
  const QString suffix = QLatin1Char('.') + modelExportSuffix(format);
  if (!path.endsWith(suffix, Qt::CaseInsensitive)) path += suffix;
  mPath->setText(path);
  const QFileInfo target(path);
  for (const QString &protectedPath : mProtectedPaths) {
    const QFileInfo source(protectedPath);
    if (target.absoluteFilePath().compare(source.absoluteFilePath(), Qt::CaseInsensitive) == 0 ||
        (target.exists() && target.canonicalFilePath().compare(source.canonicalFilePath(), Qt::CaseInsensitive) == 0)) {
      QMessageBox::warning(this, QCoreApplication::translate("Workbench", "无法导出模型"),
          QCoreApplication::translate("Workbench", "请另选文件名，不能覆盖工程文件或已加载的模型源文件。"));
      return;
    }
  }
  if (target.exists() && QMessageBox::question(this, QCoreApplication::translate("Workbench", "替换导出文件"),
      QCoreApplication::translate("Workbench", "文件已存在，是否替换？\n%1").arg(QDir::toNativeSeparators(path)),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
  accept();
}
} // namespace gsw

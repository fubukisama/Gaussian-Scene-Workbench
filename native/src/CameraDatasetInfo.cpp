#include "CameraDatasetInfo.h"
#include "AppLanguage.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QLocale>
#include <QStringList>

namespace gsw {
namespace {
QString scaleStatus(const QString &status) {
  if (status == QStringLiteral("source_declared")) return QCoreApplication::translate("Workbench", "来源声明（不是独立检核）");
  if (status == QStringLiteral("constraint_derived")) return QCoreApplication::translate("Workbench", "约束求得（保留外部证据）");
  if (status == QStringLiteral("independently_checked")) return QCoreApplication::translate("Workbench", "来源声称独立检核（保留外部证据）");
  return QCoreApplication::translate("Workbench", "任意单位（未知绝对尺度）");
}
QString initializationName(const QString &kind) {
  if (kind == QStringLiteral("random")) return QCoreApplication::translate("Workbench", "随机初始化（无几何先验）");
  if (kind == QStringLiteral("external_point_cloud")) return QCoreApplication::translate("Workbench", "外部点云初始化（含几何先验）");
  if (kind == QStringLiteral("sfm_sparse")) return QCoreApplication::translate("Workbench", "SfM 稀疏点初始化");
  return QCoreApplication::translate("Workbench", "未声明");
}
QString vectorText(const QJsonArray &array) {
  QStringList parts;
  for (const auto &value : array) parts.append(QLocale().toString(value.toDouble(), 'g', 12));
  return parts.join(QStringLiteral(", "));
}
qint64 count(const QJsonObject &summary, const QString &split) {
  const auto counts = summary.value(QStringLiteral("counts")).toObject();
  if (counts.value(split).isDouble()) return counts.value(split).toInteger();
  return summary.value(QStringLiteral("splits")).toObject().value(split).toArray().size();
}
}

bool CameraDatasetInfo::blocked(const QJsonObject &summary) {
  const auto blocked = summary.value(QStringLiteral("blocked"));
  const QString policy = summary.value(QStringLiteral("posePolicy")).toString();
  return summary.isEmpty() || !blocked.isBool() || blocked.toBool() ||
      (policy != QStringLiteral("locked") && policy != QStringLiteral("solve")) ||
      !summary.value(QStringLiteral("errors")).toArray().isEmpty();
}

QString CameraDatasetInfo::summaryText(const QJsonObject &summary) {
  if (summary.isEmpty()) return QCoreApplication::translate("Workbench", "规范相机数据尚未验证。");
  const QLocale locale;
  QStringList lines;
  const QString sourceKind = summary.value(QStringLiteral("sourceKind")).toString();
  lines.append(QCoreApplication::translate("Workbench", "相机来源：%1；适配器：%2")
      .arg(sourceKind, summary.value(QStringLiteral("adapter")).toString()));
  lines.append(summary.value(QStringLiteral("posePolicy")).toString() == QStringLiteral("locked")
      ? QCoreApplication::translate("Workbench", "位姿策略：锁定；相机内外参不参与优化，也不重跑 SfM。")
      : QCoreApplication::translate("Workbench", "位姿策略：求解；保留普通 SfM 质量检查。"));
  const auto scale = summary.value(QStringLiteral("scale")).toObject();
  const QString unit = scale.value(QStringLiteral("source_unit")).toString(QStringLiteral("unknown"));
  lines.append(QCoreApplication::translate("Workbench", "尺度状态：%1；源单位：%2")
      .arg(scaleStatus(scale.value(QStringLiteral("status")).toString()), unit));
  if (scale.value(QStringLiteral("meters_per_unit")).isDouble())
    lines.append(QCoreApplication::translate("Workbench", "每源单位米数：%1；数值归一化不会增加尺度证据。")
        .arg(locale.toString(scale.value(QStringLiteral("meters_per_unit")).toDouble(), 'g', 15)));
  const auto evidence = scale.value(QStringLiteral("evidence")).toArray();
  for (const auto &entry : evidence) {
    const auto object = entry.toObject();
    const QString detail = object.value(QStringLiteral("description")).toString();
    if (!detail.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "尺度依据：%1").arg(detail));
  }
  lines.append(QCoreApplication::translate("Workbench", "数据划分：训练 %1；验证 %2；测试 %3（测试图不参与训练或增密）。")
      .arg(locale.toString(count(summary, QStringLiteral("train"))),
           locale.toString(count(summary, QStringLiteral("val"))),
           locale.toString(count(summary, QStringLiteral("test")))));
  const auto sourceCounts = summary.value(QStringLiteral("sourceCounts")).toObject();
  if (!sourceCounts.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "来源划分（仅作溯源）：训练 %1；验证 %2；测试 %3。")
      .arg(locale.toString(sourceCounts.value(QStringLiteral("train")).toInteger()),
           locale.toString(sourceCounts.value(QStringLiteral("val")).toInteger()),
           locale.toString(sourceCounts.value(QStringLiteral("test")).toInteger())));
  if (count(summary, QStringLiteral("test")) == 0)
    lines.append(QCoreApplication::translate("Workbench", "未提供测试集；不能显示独立测试指标。"));
  const auto initialization = summary.value(QStringLiteral("initialization")).toObject();
  lines.append(QCoreApplication::translate("Workbench", "初始化：%1").arg(initializationName(initialization.value(QStringLiteral("kind")).toString())));
  const QString assumption = initialization.value(QStringLiteral("assumption")).toString();
  if (!assumption.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "初始化假设：%1").arg(assumption));
  const auto transforms = summary.value(QStringLiteral("transforms")).toObject();
  for (const auto &item : {QPair<QString, const char *>{QStringLiteral("source_to_canonical"), AppLanguage::source("源坐标 → 规范坐标")},
                           QPair<QString, const char *>{QStringLiteral("canonical_to_training"), AppLanguage::source("规范坐标 → 训练坐标")}}) {
    const auto transform = transforms.value(item.first).toObject();
    if (transform.isEmpty()) continue;
    lines.append(QCoreApplication::translate("Workbench", "%1：s=%2；t=[%3]；%4")
        .arg(QCoreApplication::translate("Workbench", item.second),
             locale.toString(transform.value(QStringLiteral("s")).toDouble(1), 'g', 15),
             vectorText(transform.value(QStringLiteral("t")).toArray()),
             transform.value(QStringLiteral("applied")).toBool()
                 ? QCoreApplication::translate("Workbench", "已应用（不会重复归一化）")
                 : QCoreApplication::translate("Workbench", "尚未应用")));
  }
  lines.append(QCoreApplication::translate("Workbench", "数据身份：%1").arg(summary.value(QStringLiteral("dataIdentity")).toString()));
  const QString version = summary.value(QStringLiteral("dataVersion")).toVariant().toString();
  if (!version.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "数据版本：%1").arg(version));
  for (const auto &warning : summary.value(QStringLiteral("warnings")).toArray())
    lines.append(QCoreApplication::translate("Workbench", "数据警告：%1").arg(warning.toString()));
  const QString metadata = summary.value(QStringLiteral("metadataPath")).toString();
  if (!metadata.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "规范元数据：%1").arg(QDir::toNativeSeparators(metadata)));
  if (blocked(summary)) lines.append(decisionText(summary));
  return lines.join(QLatin1Char('\n'));
}

QString CameraDatasetInfo::decisionText(const QJsonObject &summary) {
  if (blocked(summary)) {
    QStringList errors;
    for (const auto &entry : summary.value(QStringLiteral("errors")).toArray()) {
      const auto object = entry.toObject();
      const QString code = entry.isString() ? entry.toString() : object.value(QStringLiteral("code")).toString();
      const QString detail = object.value(QStringLiteral("detail")).toString();
      errors.append(detail.isEmpty() ? code : QStringLiteral("%1: %2").arg(code, detail));
    }
    return (summary.value(QStringLiteral("posePolicy")).toString() == QStringLiteral("solve")
        ? QCoreApplication::translate("Workbench", "相机数据准入失败：%1。已停止，不会自动更换位姿策略。")
        : QCoreApplication::translate("Workbench", "锁定相机准入失败：%1。已停止，不会回退运行 COLMAP。"))
            .arg(errors.isEmpty() ? QCoreApplication::translate("Workbench", "规范数据不可用") : errors.join(QStringLiteral(" / ")));
  }
  if (summary.value(QStringLiteral("posePolicy")).toString() == QStringLiteral("solve"))
    return QCoreApplication::translate("Workbench", "将从训练划分创建独立副本并重新求解；验证／测试影像不参与 SfM。源标定保留，新 SfM 尺度为任意单位，仍须通过 SfM 质量检查。");
  return QCoreApplication::translate("Workbench", "锁定相机准入通过；仅使用训练划分，不重求相机。此结果不是模型质量或尺度精度证明。");
}
} // namespace gsw

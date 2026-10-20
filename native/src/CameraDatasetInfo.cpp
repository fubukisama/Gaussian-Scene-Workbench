#include "CameraDatasetInfo.h"
#include "AppLanguage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QStringList>

namespace gsw {
namespace {
QString scaleStatus(const QString &status, bool internallyVerified) {
  if (status == QStringLiteral("source_declared")) return QCoreApplication::translate("Workbench", "来源声明（不是独立检核）");
  if (status == QStringLiteral("constraint_derived")) return QCoreApplication::translate("Workbench", internallyVerified
      ? AppLanguage::source("约束求得（已重放核验；无模型精度声明）") : AppLanguage::source("约束求得（保留外部证据）"));
  if (status == QStringLiteral("independently_checked")) return QCoreApplication::translate("Workbench", internallyVerified
      ? AppLanguage::source("独立检核通过（仅限参考约束）") : AppLanguage::source("来源声称独立检核（保留外部证据）"));
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

bool CameraDatasetInfo::meshGenerationBlocked(const QJsonObject &configuration) {
  // Only this new depth/mask adapter consumes the complete canonical contract.
  // It still performs actual model/dataset/hash/camera checks in its worker;
  // no legacy external scene reader is made compatible by this exception.
  if (configuration.value(QStringLiteral("mode")) == QStringLiteral("gs_depth")) return false;
  const QString directory = configuration.value(QStringLiteral("modelDirectory")).toString();
  if (directory.trimmed().isEmpty()) return false;
  const QDir output(directory);
  // These are reserved GSW provenance markers, not arbitrary project metadata.
  // Their existence is sufficient: damaged, unreadable or unknown-version
  // contracts must not fall back to an external loader that ignores full K.
  const auto marked = [](const QString &path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
  };
  if (marked(output.filePath(QStringLiteral("training-camera-contract.json")))) return true;
  const int iteration = configuration.value(QStringLiteral("iteration")).toInt();
  return iteration > 0 && marked(output.filePath(
      QStringLiteral("point_cloud/iteration_%1/point_cloud.camera-scale.json").arg(iteration)));
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
  const bool internallyVerified = scale.value(QStringLiteral("verification_origin")) == QStringLiteral("gsw_replay_verified");
  const QString unit = scale.value(QStringLiteral("source_unit")).toString(QStringLiteral("unknown"));
  lines.append(QCoreApplication::translate("Workbench", "尺度状态：%1；源单位：%2")
      .arg(scaleStatus(scale.value(QStringLiteral("status")).toString(), internallyVerified), unit));
  if (scale.value(QStringLiteral("meters_per_unit")).isDouble())
    lines.append(QCoreApplication::translate("Workbench", "每源单位米数：%1；数值归一化不会增加尺度证据。")
        .arg(locale.toString(scale.value(QStringLiteral("meters_per_unit")).toDouble(), 'g', 15)));
  const auto evidence = scale.value(QStringLiteral("evidence")).toArray();
  for (const auto &entry : evidence) {
    const auto object = entry.toObject();
    const QString detail = object.value(QStringLiteral("description")).toString();
    if (!detail.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "尺度依据：%1").arg(detail));
  }
  const auto derivation = summary.value(QStringLiteral("scaleDerivation")).toObject();
  if (!derivation.isEmpty()) {
    const QString mode = derivation.value(QStringLiteral("mode")).toString();
    lines.append(QCoreApplication::translate("Workbench", "定尺度方法：%1；来源数据身份：%2；解摘要：%3")
        .arg(mode == QStringLiteral("scale_only")
                 ? QCoreApplication::translate("Workbench", "仅定尺度（不确定方向与位置）")
                 : mode == QStringLiteral("sim3")
                     ? QCoreApplication::translate("Workbench", "相似变换对齐（尺度、旋转、平移）") : mode,
             derivation.value(QStringLiteral("inputDataIdentity")).toString(),
             derivation.value(QStringLiteral("solutionSha256")).toString()));
    const auto fit = derivation.value(QStringLiteral("fit")).toObject();
    const auto fitValue = [&locale, &fit](const QString &key) {
      return fit.value(key).isDouble() ? locale.toString(fit.value(key).toDouble(), 'g', 17)
          : QCoreApplication::translate("Workbench", "未提供");
    };
    lines.append(QCoreApplication::translate("Workbench", "拟合约束一致性（不是模型精度）：加权 RMSE %1 m；最大残差 %2 m。")
        .arg(fitValue(QStringLiteral("weighted_rmse_m")), fitValue(QStringLiteral("max_residual_m"))));
    const auto checks = derivation.value(QStringLiteral("independentChecks")).toObject();
    const QString status = checks.value(QStringLiteral("status")).toString();
    const QString checkText = !internallyVerified
        ? QCoreApplication::translate("Workbench", "外部声明，尚未由本软件重放核验")
        : status == QStringLiteral("pass")
            ? QCoreApplication::translate("Workbench", "通过（仅限所列参考约束）")
            : status == QStringLiteral("fail")
                ? QCoreApplication::translate("Workbench", "失败")
                : status == QStringLiteral("not_provided")
                    ? QCoreApplication::translate("Workbench", "未提供独立检核")
                    : QCoreApplication::translate("Workbench", "独立检核未验证");
    lines.append(QCoreApplication::translate("Workbench", "独立检核：%1；范围：%2。模型几何精度未测量。")
        .arg(checkText, derivation.value(QStringLiteral("scope")).toString()));
    lines.append(QCoreApplication::translate("Workbench", "原始相机位姿、内参和数据划分保留；仅应用已记录的均匀相似变换，不重新求解相机。"));
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
  const auto declaredNumber = [&locale, &initialization](const QString &key) {
    return initialization.value(key).isDouble() ? locale.toString(initialization.value(key).toInteger())
        : QCoreApplication::translate("Workbench", "未声明（检查时显示实际值）");
  };
  const QString frame = initialization.value(QStringLiteral("frame")).toString();
  lines.append(QCoreApplication::translate("Workbench", "初始化声明：点数 %1；种子 %2；输入框架 %3。")
      .arg(declaredNumber(QStringLiteral("count")), declaredNumber(QStringLiteral("seed")),
           frame == QStringLiteral("canonical") ? QCoreApplication::translate("Workbench", "规范坐标（已记录的尺度）")
                                               : QCoreApplication::translate("Workbench", "源坐标（沿用源单位）")));
  const auto bounds = initialization.value(QStringLiteral("bounds")).toArray();
  if (bounds.size() == 2) lines.append(QCoreApplication::translate("Workbench", "声明粗范围：[%1] → [%2]；不是已测对象边界。")
      .arg(vectorText(bounds.at(0).toArray()), vectorText(bounds.at(1).toArray())));
  else lines.append(QCoreApplication::translate("Workbench", "尚未声明粗范围；相机轨迹不确定物体深度，需单独检查初始化。"));
  QString assumption = initialization.value(QStringLiteral("coverage_assumption")).toString(
      initialization.value(QStringLiteral("assumption")).toString());
  if (assumption == QStringLiteral("scene_near_camera_coverage_center_not_depth_evidence"))
    assumption = QCoreApplication::translate("Workbench", "历史随机范围假设对象接近相机覆盖中心；这不是对象深度证据。");
  if (!assumption.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "初始化假设：%1").arg(assumption));
  const auto materialization = initialization.value(QStringLiteral("materialization")).toObject();
  const QString pointIdentity = materialization.value(QStringLiteral("point_sha256")).toString();
  if (!pointIdentity.isEmpty()) lines.append(QCoreApplication::translate("Workbench", "已冻结初始化点集身份：%1").arg(pointIdentity));
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
      errors.append(importErrorText(detail.isEmpty() ? code : QStringLiteral("%1: %2").arg(code, detail)));
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
QString CameraDatasetInfo::importErrorText(const QString &rawError) {
  const auto match = QRegularExpression(QStringLiteral("\\b(trajectory_[a-z_]+)\\b")).match(rawError);
  if (!match.hasMatch()) return rawError;
  const QString code = match.captured(1);
  const char *guidance = AppLanguage::source("轨迹清单不符合已声明的格式或字段；请检查格式版本、必需字段与数据文件。不会重新求解相机。");
  if (code.contains(QStringLiteral("timestamp"))) guidance = AppLanguage::source("轨迹时间戳必须使用无重复的精确十进制字符串或整数；请核对图像与位姿的时间关联。");
  else if (code.contains(QStringLiteral("association")) || code == QStringLiteral("trajectory_unmatched_image") || code == QStringLiteral("trajectory_pose_reused"))
    guidance = AppLanguage::source("图像与位姿不能按所选规则唯一配对；请检查一对一关联、时间容差和缺帧策略。不会插值或复用同一位姿。");
  else if (code.contains(QStringLiteral("calibration")) || code == QStringLiteral("trajectory_image_dimensions_mismatch"))
    guidance = AppLanguage::source("轨迹导入需要唯一、完整的针孔相机标定、已去畸变声明及匹配的图像尺寸；请核对标定 ID 与内参。");
  else if (code.contains(QStringLiteral("path")) || code == QStringLiteral("trajectory_missing_file"))
    guidance = AppLanguage::source("轨迹引用文件缺失或路径不安全；仅允许数据集内部的真实文件，不跟随链接或越界路径。");
  else if (code.contains(QStringLiteral("quaternion")) || code == QStringLiteral("trajectory_invalid_pose_row") || code == QStringLiteral("trajectory_invalid_number") || code == QStringLiteral("trajectory_conflicting_pose_convention"))
    guidance = AppLanguage::source("轨迹位姿、四元数或坐标约定无效；请核对有限数值、单位四元数及 c2w／w2c 和相机轴声明。不会自动猜测或修复位姿。");
  else if (code == QStringLiteral("trajectory_explicit_split_required")) guidance = AppLanguage::source("每幅轨迹图像必须明确声明训练、验证或测试划分；不会将测试图自动加入训练。");
  else if (code == QStringLiteral("trajectory_unit_declaration_required")) guidance = AppLanguage::source("必须明确声明轨迹单位 m、cm、mm 或 unknown；焦距和预览不能证明绝对尺度。");
  else if (code.contains(QStringLiteral("sensor"))) guidance = AppLanguage::source("当前轨迹入口仅支持单传感器；请提供有效传感器标识。多相机 rig 外参尚未验收，不会静默忽略。");
  else if (code.contains(QStringLiteral("duplicate")) || code == QStringLiteral("trajectory_invalid_view_id")) guidance = AppLanguage::source("轨迹图像路径与 view_id 必须唯一且有效；不会合并同名记录或静默丢弃图像。");
  else if (code == QStringLiteral("trajectory_no_images") || code == QStringLiteral("trajectory_no_poses") || code == QStringLiteral("trajectory_invalid_image")) guidance = AppLanguage::source("轨迹数据缺少可读取图像或有效位姿；请检查文件与清单，不能用空预览替代输入验证。");
  return QCoreApplication::translate("Workbench", guidance) + QLatin1Char('\n') +
      QCoreApplication::translate("Workbench", "原始技术错误（保持不变）：%1").arg(rawError);
}
} // namespace gsw

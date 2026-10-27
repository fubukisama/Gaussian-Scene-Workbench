#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <cmath>
#include <optional>

namespace gsw {

// Viewport presentation only. majorSpacing is in the unmodified scene-reference
// coordinate units; displayUnit never declares or changes a physical scale.
struct ViewportGridSettings final {
  enum class SpacingMode { Automatic, Fixed };
  enum class DisplayUnit { Scene, Millimetres, Centimetres, Metres };

  SpacingMode spacingMode = SpacingMode::Automatic;
  DisplayUnit displayUnit = DisplayUnit::Scene;
  bool gridVisible = true;
  bool axesVisible = true;
  double majorSpacing = 1.0;
  int subdivisions = 10;
  double density = 1.0;

  [[nodiscard]] bool isValid() const {
    const bool validMode = spacingMode == SpacingMode::Automatic ||
                           spacingMode == SpacingMode::Fixed;
    const bool validUnit = displayUnit == DisplayUnit::Scene ||
                          displayUnit == DisplayUnit::Millimetres ||
                          displayUnit == DisplayUnit::Centimetres ||
                          displayUnit == DisplayUnit::Metres;
    return validMode && validUnit && std::isfinite(majorSpacing) &&
           majorSpacing > 0.0 &&
           (subdivisions == 1 || subdivisions == 2 || subdivisions == 5 ||
            subdivisions == 10) &&
           std::isfinite(density) && density >= 0.25 && density <= 4.0;
  }

  [[nodiscard]] std::optional<QJsonObject> toJson() const {
    if (!isValid()) return std::nullopt;
    QString unit = QStringLiteral("scene");
    switch (displayUnit) {
    case DisplayUnit::Scene: break;
    case DisplayUnit::Millimetres: unit = QStringLiteral("mm"); break;
    case DisplayUnit::Centimetres: unit = QStringLiteral("cm"); break;
    case DisplayUnit::Metres: unit = QStringLiteral("m"); break;
    }
    return QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("spacingMode"), spacingMode == SpacingMode::Automatic
             ? QStringLiteral("automatic") : QStringLiteral("fixed")},
        {QStringLiteral("majorSpacing"), majorSpacing},
        {QStringLiteral("subdivisions"), subdivisions},
        {QStringLiteral("density"), density},
        {QStringLiteral("displayUnit"), unit},
        {QStringLiteral("gridVisible"), gridVisible},
        {QStringLiteral("axesVisible"), axesVisible}};
  }

  [[nodiscard]] static std::optional<ViewportGridSettings> fromJson(
      const QJsonObject &object) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
      const QString key = it.key();
      if (key != QStringLiteral("version") &&
          key != QStringLiteral("spacingMode") &&
          key != QStringLiteral("majorSpacing") &&
          key != QStringLiteral("subdivisions") &&
          key != QStringLiteral("density") &&
          key != QStringLiteral("displayUnit") &&
          key != QStringLiteral("gridVisible") &&
          key != QStringLiteral("axesVisible")) return std::nullopt;
    }
    ViewportGridSettings result;
    if (object.contains(QStringLiteral("version"))) {
      const QJsonValue value = object.value(QStringLiteral("version"));
      if (!value.isDouble() || value.toDouble() != 1.0) return std::nullopt;
    }
    if (object.contains(QStringLiteral("spacingMode"))) {
      const QJsonValue value = object.value(QStringLiteral("spacingMode"));
      if (!value.isString()) return std::nullopt;
      const QString mode = value.toString();
      if (mode == QStringLiteral("automatic")) result.spacingMode = SpacingMode::Automatic;
      else if (mode == QStringLiteral("fixed")) result.spacingMode = SpacingMode::Fixed;
      else return std::nullopt;
    }
    if (object.contains(QStringLiteral("displayUnit"))) {
      const QJsonValue value = object.value(QStringLiteral("displayUnit"));
      if (!value.isString()) return std::nullopt;
      const QString unit = value.toString();
      if (unit == QStringLiteral("scene")) result.displayUnit = DisplayUnit::Scene;
      else if (unit == QStringLiteral("mm")) result.displayUnit = DisplayUnit::Millimetres;
      else if (unit == QStringLiteral("cm")) result.displayUnit = DisplayUnit::Centimetres;
      else if (unit == QStringLiteral("m")) result.displayUnit = DisplayUnit::Metres;
      else return std::nullopt;
    }
    if (object.contains(QStringLiteral("majorSpacing"))) {
      const QJsonValue value = object.value(QStringLiteral("majorSpacing"));
      if (!value.isDouble()) return std::nullopt;
      result.majorSpacing = value.toDouble();
    }
    if (object.contains(QStringLiteral("subdivisions"))) {
      const QJsonValue value = object.value(QStringLiteral("subdivisions"));
      if (!value.isDouble()) return std::nullopt;
      const double count = value.toDouble();
      if (count != 1.0 && count != 2.0 && count != 5.0 && count != 10.0)
        return std::nullopt;
      result.subdivisions = static_cast<int>(count);
    }
    if (object.contains(QStringLiteral("density"))) {
      const QJsonValue value = object.value(QStringLiteral("density"));
      if (!value.isDouble()) return std::nullopt;
      result.density = value.toDouble();
    }
    if (object.contains(QStringLiteral("gridVisible"))) {
      const QJsonValue value = object.value(QStringLiteral("gridVisible"));
      if (!value.isBool()) return std::nullopt;
      result.gridVisible = value.toBool();
    }
    if (object.contains(QStringLiteral("axesVisible"))) {
      const QJsonValue value = object.value(QStringLiteral("axesVisible"));
      if (!value.isBool()) return std::nullopt;
      result.axesVisible = value.toBool();
    }
    return result.isValid() ? std::optional<ViewportGridSettings>(result) : std::nullopt;
  }

  // Preserve the difference between an absent optional field and a malformed
  // present one. Calling QJsonValue::toObject() first would lose that distinction.
  [[nodiscard]] static std::optional<ViewportGridSettings> fromJson(
      const QJsonValue &value) {
    if (value.isUndefined()) return ViewportGridSettings{};
    if (!value.isObject()) return std::nullopt;
    return fromJson(value.toObject());
  }
};

} // namespace gsw

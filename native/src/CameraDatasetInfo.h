#pragma once

#include <QJsonObject>
#include <QString>

namespace gsw {

// Presentation-only reader of the shared worker's canonical-dataset summary.
// Never reads source camera formats or infers metric scale from scene geometry.
class CameraDatasetInfo final {
public:
  [[nodiscard]] static QString summaryText(const QJsonObject &summary);
  [[nodiscard]] static QString decisionText(const QJsonObject &summary);
  [[nodiscard]] static bool blocked(const QJsonObject &summary);
};

} // namespace gsw

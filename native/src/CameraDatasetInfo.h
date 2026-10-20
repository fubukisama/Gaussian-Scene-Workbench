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
  // Localized guidance plus unchanged original technical diagnostics.
  [[nodiscard]] static QString importErrorText(const QString &rawError);
  // Capability admission for external mesh backends. Reads only reserved
  // training-contract markers of the explicitly selected output/checkpoint.
  [[nodiscard]] static bool meshGenerationBlocked(const QJsonObject &configuration);
};

} // namespace gsw

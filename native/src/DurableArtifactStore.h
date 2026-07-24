#pragma once

#include <QString>

namespace gsw {

struct DurableArtifact {
  QString path;
  QString sha256;
  qint64 size = 0;

  [[nodiscard]] bool isValid() const {
    return !path.isEmpty() && sha256.size() == 64 && size >= 0;
  }
};

class DurableArtifactStore final {
public:
  [[nodiscard]] static DurableArtifact
  publish(const QString &sourcePath, const QString &destinationPath,
          QString *errorMessage = nullptr);
  [[nodiscard]] static bool verify(const QString &artifactPath,
                                   QString *errorMessage = nullptr);
};

} // namespace gsw

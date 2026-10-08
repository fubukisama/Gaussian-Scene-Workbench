#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace gsw {

struct TrainingEnvironmentProbeResult final {
  bool ready = false;
  bool policyBlocked = false;
  bool hasReconstruction = false;
  bool colmapRequired = false;
  int imageCount = 0;
  QString python;
  QString cudaDevice;
  QString errorMessage;
  QString reconstructionWarning;
};

class TrainingEnvironmentProbe final {
public:
  [[nodiscard]] static TrainingEnvironmentProbeResult
  run(const QString &python, const QString &backendRoot,
      const QString &datasetPath, const QString &backend, bool runColmap,
      const QProcessEnvironment &environment,
      int timeoutMilliseconds = 60000);
};

} // namespace gsw

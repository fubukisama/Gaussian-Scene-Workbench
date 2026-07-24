#pragma once

#include <QString>

namespace gsw {

struct TrainingOutputScene {
  QString path;
  int iteration = -1;

  [[nodiscard]] bool isValid() const { return !path.isEmpty() && iteration >= 0; }
};

struct ActiveTrainingJob {
  QString configurationPath;
  QString outputSceneRoot;

  [[nodiscard]] bool isValid() const {
    return !configurationPath.isEmpty() && !outputSceneRoot.isEmpty();
  }
};

[[nodiscard]] TrainingOutputScene findLatestTrainingOutputScene(
    const QString &outputSceneRoot);
bool saveActiveTrainingJob(const QString &projectRoot,
                           const ActiveTrainingJob &job,
                           QString *errorMessage = nullptr);
[[nodiscard]] ActiveTrainingJob
loadActiveTrainingJob(const QString &projectRoot,
                      QString *errorMessage = nullptr);
bool clearActiveTrainingJob(const QString &projectRoot,
                            QString *errorMessage = nullptr);

} // namespace gsw

#pragma once

#include <QString>

namespace gsw {

struct TrainingOutputScene {
  QString path;
  int iteration = -1;

  [[nodiscard]] bool isValid() const { return !path.isEmpty() && iteration >= 0; }
};

[[nodiscard]] TrainingOutputScene findLatestTrainingOutputScene(
    const QString &outputSceneRoot);

} // namespace gsw

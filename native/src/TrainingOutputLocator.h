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
  // A safely paused/recovered preview is already part of the project. Do not
  // replace a subsequently selected model every time that project is opened.
  bool previewRecovered = false;
  // Stable across pause/resume, unrelated to the selected reference model.
  QString resultSceneId;

  [[nodiscard]] bool isValid() const {
    return !configurationPath.isEmpty() && !outputSceneRoot.isEmpty();
  }
};

[[nodiscard]] TrainingOutputScene findLatestTrainingOutputScene(
    const QString &outputSceneRoot);
// Cheap availability check only. The worker verifies SHA-256 and compatibility
// before deserializing a user-confirmed checkpoint.
[[nodiscard]] int nativeResumeIteration(const QString &outputSceneRoot);
bool saveActiveTrainingJob(const QString &projectRoot,
                           const ActiveTrainingJob &job,
                           QString *errorMessage = nullptr);
[[nodiscard]] ActiveTrainingJob
loadActiveTrainingJob(const QString &projectRoot,
                      QString *errorMessage = nullptr);
bool clearActiveTrainingJob(const QString &projectRoot,
                            QString *errorMessage = nullptr);
// A launch that never reached taskStarted must not replace an earlier resume
// record. Use the same rollback for immediate rejection and OS launch failure.
bool restoreActiveTrainingJobAfterLaunchFailure(
    const QString &projectRoot, const ActiveTrainingJob &previous,
    QString *errorMessage = nullptr);

} // namespace gsw

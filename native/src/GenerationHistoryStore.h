#pragma once

#include "TrainingOutputLocator.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace gsw {

// User display names and machine identifiers are deliberately separate.
// Paths are absolute at this interface and portable inside the archive.
struct GenerationExperiment {
  QString id;
  QString pipeline;
  QString backend;
  QString displayName;
  QString datasetPath;
  QString configurationPath;
  QString outputRoot;
  QString resultPath;
  QString resultSceneId;
  QString status;
  QDateTime createdAt;
  QDateTime updatedAt;
  QJsonObject parameters;

  [[nodiscard]] bool isValid() const;
};

struct GenerationResumeAvailability {
  bool available = false;
  int iteration = -1;
  // Localized presentation only; never stored or used as an identifier.
  QString reason;
};

// A durable experimental archive, not a persisted worker queue. Reading never
// starts jobs or deserializes optimizer state. Worker verification and explicit
// user trust confirmation remain mandatory before full-state resume.
class GenerationHistoryStore final {
public:
  explicit GenerationHistoryStore(const QString &projectRoot);
  bool upsert(const GenerationExperiment &experiment, QString *errorMessage = nullptr) const;
  [[nodiscard]] QList<GenerationExperiment> records(QString *errorMessage = nullptr) const;
  [[nodiscard]] GenerationExperiment record(const QString &id, QString *errorMessage = nullptr) const;
  bool migrateActiveTraining(QString *errorMessage = nullptr) const;
  [[nodiscard]] GenerationResumeAvailability resumeAvailability(const QString &id) const;
  [[nodiscard]] ActiveTrainingJob resumeJob(const QString &id, QString *errorMessage = nullptr) const;

private:
  QString mProjectRoot;
};

} // namespace gsw

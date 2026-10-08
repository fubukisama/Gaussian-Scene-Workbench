#pragma once
#include <QObject>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>

class QProcess;

namespace gsw {
struct TrainingReconstructionSummary {
  QJsonObject report;
  QString diagnostic;
  bool ready = false;
};

// Read-only external dataset inspection. Language/backend/ratio edits do not
// create model state; cancelling this helper never affects a training worker.
class TrainingReconstructionSummaryScanner final : public QObject {
  Q_OBJECT
public:
  explicit TrainingReconstructionSummaryScanner(QObject *parent = nullptr);
  ~TrainingReconstructionSummaryScanner() override;
  void request(const QString &datasetPath, bool runColmap,
               const QString &python, const QString &backendRoot,
               const QProcessEnvironment &environment);
  void cancel();
signals:
  void summaryReady(const gsw::TrainingReconstructionSummary &summary);
private:
  QProcess *mProcess = nullptr;
  quint64 mGeneration = 0;
};
}
Q_DECLARE_METATYPE(gsw::TrainingReconstructionSummary)

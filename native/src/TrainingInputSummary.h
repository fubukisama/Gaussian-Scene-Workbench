#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>
#include <atomic>
#include <memory>

namespace gsw {

struct TrainingImageSizeGroup {
  QSize sourceSize;
  qint64 imageCount = 0;
};

// Directory/header metadata only: this is not a list of registered cameras or
// a promise about the trainer's final decoded tensors.
struct TrainingInputSummary {
  QString sourceDirectory;
  QList<TrainingImageSizeGroup> sizeGroups;
  qint64 imageCount = 0;
  qint64 unknownSizeCount = 0;
  qint64 skippedLinkCount = 0;
  bool cancelled = false;
};

using TrainingInputCancellation = std::shared_ptr<std::atomic_bool>;

[[nodiscard]] TrainingInputSummary scanTrainingInputSummary(
    const QString &datasetPath, bool runColmap,
    const TrainingInputCancellation &cancellation = {});

// The upstream loaders use round(width / resolution), i.e. ties to even,
// rather than Qt's half-away-from-zero rounding.
[[nodiscard]] QSize projectedTrainingImageSize(const QSize &sourceSize,
                                               int resolution);

class TrainingInputSummaryScanner final : public QObject {
  Q_OBJECT

public:
  explicit TrainingInputSummaryScanner(QObject *parent = nullptr);
  ~TrainingInputSummaryScanner() override;

  void request(const QString &datasetPath, bool runColmap);
  void cancel();

signals:
  void summaryReady(const gsw::TrainingInputSummary &summary);

private:
  // A scanner belongs to one dialog. Closing/reopening the dialog creates a
  // fresh cache; changing ratio/language reuses these header-only results.
  QHash<QString, TrainingInputSummary> mCache;
  TrainingInputCancellation mCancellation;
  quint64 mGeneration = 0;
};

} // namespace gsw

Q_DECLARE_METATYPE(gsw::TrainingInputSummary)

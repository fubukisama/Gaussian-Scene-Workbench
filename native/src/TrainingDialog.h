#pragma once

#include <QDialog>
#include <QString>
#include "TrainingInputSummary.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QSpinBox;

namespace gsw {

struct TrainingConfiguration {
  QString backend;
  QString quality;
  QString outputRoot;
  QString outputScene;
  QString outputStorageName;
  int iterations = 10000;
  int resolution = 2;
  bool runColmap = true;
  bool overwrite = false;
};

class TrainingDialog final : public QDialog {
  Q_OBJECT

public:
  TrainingDialog(const QString &datasetPath, const QString &projectName,
                 const QString &defaultOutputRoot, bool hasSparseReconstruction,
                 bool twoDgsAvailable, QWidget *parent = nullptr);

  [[nodiscard]] TrainingConfiguration configuration() const;

protected:
  void accept() override;

private:
  void applyPreset();
  void chooseOutputRoot();
  void refreshInputSummary();
  void scanInputSummary();
  [[nodiscard]] bool datasetContainsImages() const;

  QString mDatasetPath;
  QComboBox *mBackend = nullptr;
  QComboBox *mQuality = nullptr;
  QComboBox *mResolution = nullptr;
  QSpinBox *mIterations = nullptr;
  QLineEdit *mOutputRoot = nullptr;
  QLineEdit *mOutputScene = nullptr;
  QCheckBox *mRunColmap = nullptr;
  QCheckBox *mOverwrite = nullptr;
  QLabel *mInputSummaryLabel = nullptr;
  TrainingInputSummaryScanner *mInputScanner = nullptr;
  TrainingInputSummary mInputSummary;
  bool mInputSummaryPending = false;
};

} // namespace gsw

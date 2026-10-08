#pragma once

#include <QDialog>
#include <QString>
#include "TrainingInputSummary.h"
#include "TrainingReconstructionSummary.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QSpinBox;
class QPushButton;

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
  void setReconstructionProbe(const QString &python, const QString &backendRoot,
                              const QProcessEnvironment &environment);
  [[nodiscard]] bool effectiveRunColmap() const;
  [[nodiscard]] bool reconstructionPreflightReady() const;
  [[nodiscard]] QString reconstructionDecisionText() const;
  [[nodiscard]] TrainingReconstructionSummary reconstructionSummary() const;

protected:
  void accept() override;

private:
  void applyPreset();
  void chooseOutputRoot();
  void refreshInputSummary();
  void scanInputSummary();
  [[nodiscard]] bool datasetContainsImages() const;
  void scanReconstructionSummary();
  void refreshReconstructionSummary();

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
  QLabel *mReconstructionSummaryLabel = nullptr;
  QLabel *mReconstructionDecisionLabel = nullptr;
  QPushButton *mStartButton = nullptr;
  TrainingReconstructionSummaryScanner *mReconstructionScanner = nullptr;
  TrainingReconstructionSummary mReconstructionSummary;
  QString mProbePython;
  QString mProbeBackendRoot;
  QProcessEnvironment mProbeEnvironment;
  bool mReconstructionProbeConfigured = false;
  bool mReconstructionSummaryPending = false;
  bool mRunColmapEdited = false;
};

} // namespace gsw

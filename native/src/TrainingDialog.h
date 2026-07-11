#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace gsw {

struct TrainingConfiguration {
  QString backend;
  QString quality;
  QString outputRoot;
  QString outputScene;
  int iterations = 7000;
  int resolution = 8;
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
};

} // namespace gsw

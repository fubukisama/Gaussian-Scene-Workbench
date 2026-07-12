#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace gsw {

struct ReconstructionConfiguration {
  QString colmapExecutable;
  QString preset;
  QString matching;
  QString cameraModel;
  int featureMaxImageSize = -1;
  int featureMaxNumFeatures = 8192;
  int mapperMaxRuntimeSeconds = -1;
  bool matcherGuided = false;
  bool useGpu = true;
  bool singleCamera = true;
  bool reset = true;
};

class ReconstructionDialog final : public QDialog {
  Q_OBJECT

public:
  ReconstructionDialog(const QString &datasetPath,
                       const QString &repositoryRoot,
                       QWidget *parent = nullptr);

  [[nodiscard]] ReconstructionConfiguration configuration() const;

protected:
  void accept() override;

private:
  void applyPreset();
  void chooseExecutable();

  QString mDatasetPath;
  bool mHasExistingData = false;
  QLineEdit *mExecutable = nullptr;
  QComboBox *mPreset = nullptr;
  QComboBox *mMatching = nullptr;
  QComboBox *mCameraModel = nullptr;
  QComboBox *mMaxImageSize = nullptr;
  QSpinBox *mMaxFeatures = nullptr;
  QSpinBox *mRuntimeMinutes = nullptr;
  QCheckBox *mUseGpu = nullptr;
  QCheckBox *mSingleCamera = nullptr;
  QCheckBox *mReset = nullptr;
};

} // namespace gsw

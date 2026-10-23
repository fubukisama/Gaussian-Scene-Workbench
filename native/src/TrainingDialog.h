#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QString>
#include "TrainingInputSummary.h"
#include "TrainingReconstructionSummary.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;
class QSpinBox;
class QDoubleSpinBox;
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
  bool resourceGuardEnabled = true;
  bool imageStaging = true;
  bool cameraPrefetch = true;
  bool capacityStorage = true;
  bool surfelAuxiliary = true;
  int trainingMemoryPercent = 90;
  int minimumSpareMiB = 512;
  int maxConsecutiveDenials = 3;
  int imageGpuSlots = 2;
  int imagePinnedMiB = 256;
  int imageGpuMiB = 512;
  int profileInterval = 30;
  QString lossMode = QStringLiteral("auto");
  QString trainingEngine = QStringLiteral("stock");
  QString growthMode = QStringLiteral("pause");
  QString posePolicy = QStringLiteral("solve");
  QString cameraDatasetPath;
  QString cameraDatasetIdentity;
  QString resolutionPolicy = QStringLiteral("manual");
  QJsonObject runtimeOptions;
  QJsonObject qualityPlan;
};

class TrainingDialog final : public QDialog {
  Q_OBJECT

public:
  TrainingDialog(const QString &datasetPath, const QString &projectName,
                 const QString &defaultOutputRoot, bool hasSparseReconstruction,
                 bool twoDgsAvailable, QWidget *parent = nullptr);

  [[nodiscard]] TrainingConfiguration configuration() const;
  // Request-only state. Dataset/camera identity, paths, overwrite permission and
  // task state remain owned by the current project and are never restored here.
  [[nodiscard]] QJsonObject configurationDocument() const;
  [[nodiscard]] bool setConfigurationDocument(const QJsonObject &document,
      bool preserveCurrentInputPolicy = false);
  [[nodiscard]] bool setQualityPlanDocument(const QJsonObject &plan);
  [[nodiscard]] bool importQualityPlanFile(const QString &path);
  [[nodiscard]] QString qualityPlanValidationText() const;
  void setReconstructionProbe(const QString &python, const QString &backendRoot,
                              const QProcessEnvironment &environment);
  [[nodiscard]] bool effectiveRunColmap() const;
  [[nodiscard]] bool reconstructionPreflightReady() const;
  [[nodiscard]] QString reconstructionDecisionText() const;
  [[nodiscard]] TrainingReconstructionSummary reconstructionSummary() const;
  void setCameraDatasetReference(const QString &metadataPath,
                                const QString &dataIdentity = {},
                                const QString &posePolicy = QStringLiteral("locked"));
  [[nodiscard]] QJsonObject cameraDatasetSummary() const;
  void setInitializationWorkspace(const QString &workspaceRoot);
  void useInitializationDataset(const QString &metadataPath, const QString &dataIdentity);
signals:
  void initializationVersionCreated(const QString &datasetPath, const QJsonObject &summary,
                                    const QString &inputIdentity);

protected:
  void accept() override;

private:
  void applyPreset();
  void refreshPresetSummary();
  void chooseOutputRoot();
  void refreshInputSummary();
  void scanInputSummary();
  [[nodiscard]] bool datasetContainsImages() const;
  void scanReconstructionSummary();
  void refreshReconstructionSummary();
  void refreshCameraPolicy();
  void refreshResolutionPolicy();
  void openInitialization();
  void refreshQualityPlan();
  void updateQualityPlanFromControls(bool strategyChanged = false, bool filterChanged = false);
  void chooseQualityPlan();
  [[nodiscard]] bool qualityPlanReady() const;

  QString mDatasetPath;
  QComboBox *mBackend = nullptr;
  QComboBox *mQuality = nullptr;
  QString mAppliedQuality = QStringLiteral("quick");
  QLabel *mPresetSummary = nullptr;
  QPushButton *mApplyPresetButton = nullptr;
  QComboBox *mResolution = nullptr;
  QComboBox *mResolutionPolicy = nullptr;
  QComboBox *mPosePolicy = nullptr;
  QSpinBox *mIterations = nullptr;
  QLineEdit *mOutputRoot = nullptr;
  QLineEdit *mOutputScene = nullptr;
  QCheckBox *mRunColmap = nullptr;
  QCheckBox *mOverwrite = nullptr;
  QCheckBox *mResourceGuard = nullptr;
  QCheckBox *mImageStaging = nullptr;
  QCheckBox *mCameraPrefetch = nullptr;
  QCheckBox *mCapacityStorage = nullptr;
  QCheckBox *mSurfelAuxiliary = nullptr;
  QSpinBox *mTrainingMemoryPercent = nullptr;
  QSpinBox *mMinimumSpareMiB = nullptr;
  QSpinBox *mMaxConsecutiveDenials = nullptr;
  QSpinBox *mImageGpuSlots = nullptr;
  QSpinBox *mImagePinnedMiB = nullptr;
  QSpinBox *mImageGpuMiB = nullptr;
  QSpinBox *mProfileInterval = nullptr;
  QComboBox *mLossMode = nullptr;
  QComboBox *mTrainingEngine = nullptr;
  QComboBox *mGrowthMode = nullptr;
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
  bool mUpdatingPolicy = false;
  bool mPreviousLocked = false;
  int mSolveResolution = 2;
  QString mCameraDatasetPath;
  QString mCameraDatasetIdentity;
  QString mExpectedCameraDatasetIdentity;
  QString mInitializationWorkspace;
  QPushButton *mInitializationButton = nullptr;
  QLabel *mDatasetValue = nullptr;
  QJsonObject mQualityPlan;
  QCheckBox *mQualityPlanEnabled = nullptr;
  QDoubleSpinBox *mGeometryLrMultiplier = nullptr;
  QComboBox *mDensificationStrategy = nullptr;
  QComboBox *mSamplingFilter = nullptr;
  QPushButton *mImportQualityPlan = nullptr;
  QLabel *mQualityPlanStatus = nullptr;
  QString mQualityPlanImportError;
  bool mUpdatingQualityPlan = false;
};

} // namespace gsw

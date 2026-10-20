#pragma once

#include <QDialog>
#include <QJsonObject>

class QLineEdit;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;

namespace gsw {
class MeshGenerationDialog final : public QDialog {
  Q_OBJECT
public:
  explicit MeshGenerationDialog(const QString &modelDirectory, QWidget *parent = nullptr);
  [[nodiscard]] QJsonObject configuration() const;
  void setCameraDatasetPath(const QString &path);
protected:
  void accept() override;
private:
  void refreshSource();
  void refreshOptions();
  QLineEdit *mSource;
  QComboBox *mMode;
  QComboBox *mQuality;
  QSpinBox *mResolution;
  QSpinBox *mClusters;
  QSpinBox *mDownsample;
  QCheckBox *mTexture;
  QSpinBox *mTextureResolution;
  QLineEdit *mDataset;
  QComboBox *mCoordinateMode;
  QDoubleSpinBox *mAlpha;
  QSpinBox *mPixelStride;
  QSpinBox *mMaxPoints;
  QSpinBox *mMemoryBudget;
  QLabel *mSourceSummary;
  QLabel *mHint;
  QString mBackend;
  int mIteration = 0;
};
} // namespace gsw

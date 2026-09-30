#pragma once

#include <QDialog>
#include <QJsonObject>

class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QLabel;

namespace gsw {
class MeshGenerationDialog final : public QDialog {
  Q_OBJECT
public:
  explicit MeshGenerationDialog(const QString &modelDirectory, QWidget *parent = nullptr);
  [[nodiscard]] QJsonObject configuration() const;
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
  QLabel *mSourceSummary;
  QLabel *mHint;
  QString mBackend;
  int mIteration = 0;
};
} // namespace gsw

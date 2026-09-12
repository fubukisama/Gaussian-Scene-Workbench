#pragma once
#include "ModelExport.h"
#include <QDialog>

class QComboBox;
class QLineEdit;
class QLabel;

namespace gsw {
class ModelExportDialog final : public QDialog {
public:
  ModelExportDialog(const ModelExportOptions &source, bool mesh, bool gaussian,
                    const QStringList &protectedPaths, QWidget *parent = nullptr);
  [[nodiscard]] ModelExportOptions options() const;
private:
  void refreshDescription();
  void chooseFile();
  void confirmExport();
  ModelExportOptions mSource;
  bool mGaussian;
  QStringList mProtectedPaths;
  QComboBox *mFormat;
  QComboBox *mCoordinates;
  QLineEdit *mPath;
  QLabel *mDescription;
};
} // namespace gsw

#pragma once
#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace gsw {
// Edits only the resourceGuard section for a later explicit full-state resume.
// It does not launch or modify an existing training process.
class RuntimeGuardDialog final : public QDialog {
  Q_OBJECT
public:
  explicit RuntimeGuardDialog(const QJsonObject &guard, QWidget *parent = nullptr);
  [[nodiscard]] QJsonObject configuration() const;
private:
  QJsonObject mOriginal;
  bool mValidInput = true;
  bool mUsageEdited = false;
  QCheckBox *mEnabled = nullptr;
  QComboBox *mMode = nullptr;
  QDoubleSpinBox *mUsage = nullptr;
  QSpinBox *mSpare = nullptr;
  QSpinBox *mDenials = nullptr;
};
}

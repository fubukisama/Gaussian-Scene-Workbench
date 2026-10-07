#pragma once

#include <QDialog>
#include <QPointer>

class QComboBox;
class QDoubleSpinBox;
class QLabel;

namespace gsw {
class NativeViewport;
class ResourceBudgetDialog final : public QDialog {
public:
  explicit ResourceBudgetDialog(NativeViewport *viewport, QWidget *parent = nullptr);

private:
  void applyControls();
  void refreshStatus();
  QPointer<NativeViewport> mViewport;
  QComboBox *mMode = nullptr;
  QDoubleSpinBox *mRamLimit = nullptr;
  QDoubleSpinBox *mGpuLimit = nullptr;
  QLabel *mRamAvailable = nullptr;
  QLabel *mGpuAvailableTitle = nullptr;
  QLabel *mGpuAvailable = nullptr;
  QLabel *mManagedUsage = nullptr;
  QLabel *mReservedUsage = nullptr;
  QLabel *mEffectiveBudget = nullptr;
  QLabel *mMeshCounts = nullptr;
  QLabel *mProbeDescription = nullptr;
};
} // namespace gsw

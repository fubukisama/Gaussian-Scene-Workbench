#pragma once

#include "ViewportGridSettings.h"

#include <QDialog>
#include <memory>

namespace gsw {
struct SceneCoordinateInfo;

class ViewportGridDialog final : public QDialog {
  Q_OBJECT
public:
  explicit ViewportGridDialog(const ViewportGridSettings &settings,
                              const SceneCoordinateInfo &coordinates,
                              QWidget *parent = nullptr);
  ~ViewportGridDialog() override;

  // Updated only by a successful accept; rejecting leaves the input untouched.
  [[nodiscard]] ViewportGridSettings settings() const;
  void accept() override;

private:
  class State;
  std::unique_ptr<State> m;
  void refreshPresentation();
  void refreshValidation();
  void changeDisplayUnit();
  [[nodiscard]] std::optional<double> rawSpacing() const;
  [[nodiscard]] std::optional<ViewportGridSettings> readControls() const;
};

} // namespace gsw

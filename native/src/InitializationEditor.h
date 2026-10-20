#pragma once
#include <QJsonObject>
#include <QWidget>
#include <memory>

namespace gsw {
// JSON-only editor shared by raw import and the admitted-dataset workflow.
// It never parses camera files, generates points, or infers metric units.
class InitializationEditor final : public QWidget {
  Q_OBJECT
public:
  explicit InitializationEditor(bool allowPreserve, QWidget *parent = nullptr);
  ~InitializationEditor() override;
  void setInitialization(const QJsonObject &initialization);
  [[nodiscard]] QJsonObject initialization() const;
  [[nodiscard]] QString validationError() const;
  void setDatasetDirectory(const QString &directory);
signals:
  void initializationChanged();
private:
  class State;
  std::unique_ptr<State> m;
  void refresh();
};
}

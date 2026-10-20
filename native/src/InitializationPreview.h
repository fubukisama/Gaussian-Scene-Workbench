#pragma once
#include <QJsonObject>
#include <QWidget>
#include <memory>

namespace gsw {
// A CPU-only presentation of the shared worker's actual geometry. This widget
// does not generate points, calculate coverage, or infer object depth.
class InitializationPreview final : public QWidget {
  Q_OBJECT
public:
  explicit InitializationPreview(QWidget *parent = nullptr);
  ~InitializationPreview() override;
  bool setReport(const QJsonObject &report);
  void setProjection(const QString &projection);
  void setSelectedView(const QString &viewId);
protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
private:
  class State;
  std::unique_ptr<State> m;
};
}

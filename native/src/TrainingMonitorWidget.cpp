#include "TrainingMonitorWidget.h"

#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace gsw {

namespace {

QString formattedDuration(const double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0) {
    return QStringLiteral("-");
  }
  const qint64 rounded = static_cast<qint64>(std::round(seconds));
  const qint64 hours = rounded / 3600;
  const qint64 minutes = (rounded % 3600) / 60;
  const qint64 remainder = rounded % 60;
  if (hours > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(hours)
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remainder, 2, 10, QLatin1Char('0'));
  }
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(remainder, 2, 10, QLatin1Char('0'));
}

QString stageLabel(const QString &stage) {
  if (stage == QStringLiteral("queued")) {
    return QStringLiteral("排队中");
  }
  if (stage == QStringLiteral("prepare") ||
      stage == QStringLiteral("preparing")) {
    return QStringLiteral("准备数据");
  }
  if (stage == QStringLiteral("colmap")) {
    return QStringLiteral("相机解算");
  }
  if (stage == QStringLiteral("train")) {
    return QStringLiteral("训练中");
  }
  if (stage == QStringLiteral("finalizing")) {
    return QStringLiteral("发布模型");
  }
  return stage.isEmpty() ? QStringLiteral("运行中") : stage;
}

QLabel *metricValue(QWidget *parent) {
  auto *label = new QLabel(QStringLiteral("-"), parent);
  QFont font = label->font();
  font.setBold(true);
  label->setFont(font);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

void addMetric(QGridLayout *layout, const int column, const QString &caption,
               QLabel *value) {
  auto *captionLabel = new QLabel(caption, value->parentWidget());
  captionLabel->setObjectName(QStringLiteral("mutedLabel"));
  layout->addWidget(captionLabel, 0, column);
  layout->addWidget(value, 1, column);
}

} // namespace

class TrainingCurvesWidget final : public QWidget {
public:
  explicit TrainingCurvesWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setMinimumHeight(112);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  void setSamples(const QVector<TrainingSample> &samples) {
    mSamples = samples;
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(15, 18, 20));

    QFont chartFont = font();
    if (chartFont.pointSizeF() > 0.0) {
      chartFont.setPointSizeF(std::max(7.0, chartFont.pointSizeF() * 0.82));
    }
    painter.setFont(chartFont);
    const QFontMetrics metrics(chartFont);
    const int labelHeight = metrics.height();
    const QRectF plot = QRectF(rect()).adjusted(12.0, labelHeight + 10.0,
                                                  -12.0, -10.0);
    if (plot.width() < 10.0 || plot.height() < 10.0) {
      return;
    }

    painter.setPen(QColor(47, 53, 57));
    for (int line = 0; line <= 4; ++line) {
      const qreal y = plot.top() + plot.height() * line / 4.0;
      painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    const QColor lossColor(91, 199, 170);
    const QColor psnrColor(226, 181, 91);
    painter.setPen(lossColor);
    painter.drawText(QPointF(plot.left(), labelHeight), QStringLiteral("Loss"));
    const QString psnrLabel = QStringLiteral("PSNR");
    painter.setPen(psnrColor);
    painter.drawText(QPointF(plot.right() - metrics.horizontalAdvance(psnrLabel),
                             labelHeight),
                     psnrLabel);

    if (mSamples.size() < 2) {
      painter.setPen(QColor(126, 134, 139));
      const QString waiting = QStringLiteral("等待训练采样…");
      painter.drawText(plot, Qt::AlignCenter, waiting);
      return;
    }

    const int firstIteration = mSamples.front().iteration;
    const int lastIteration = std::max(mSamples.back().iteration,
                                       firstIteration + 1);
    drawSeries(painter, plot, lossColor, firstIteration, lastIteration,
               [](const TrainingSample &sample) { return sample.loss; });
    drawSeries(painter, plot, psnrColor, firstIteration, lastIteration,
               [](const TrainingSample &sample) { return sample.psnr; });
  }

private:
  template <typename ValueAccessor>
  void drawSeries(QPainter &painter, const QRectF &plot, const QColor &color,
                  const int firstIteration, const int lastIteration,
                  ValueAccessor valueFor) {
    std::optional<double> minimum;
    std::optional<double> maximum;
    for (const TrainingSample &sample : mSamples) {
      const std::optional<double> value = valueFor(sample);
      if (!value.has_value()) {
        continue;
      }
      minimum = minimum.has_value() ? std::min(*minimum, *value) : *value;
      maximum = maximum.has_value() ? std::max(*maximum, *value) : *value;
    }
    if (!minimum.has_value() || !maximum.has_value()) {
      return;
    }
    double range = *maximum - *minimum;
    if (range < 1e-9) {
      range = std::max(std::abs(*maximum) * 0.1, 1e-3);
      *minimum -= range * 0.5;
      *maximum += range * 0.5;
    }

    QPainterPath path;
    bool started = false;
    for (const TrainingSample &sample : mSamples) {
      const std::optional<double> value = valueFor(sample);
      if (!value.has_value()) {
        continue;
      }
      const qreal x = plot.left() +
                      plot.width() * (sample.iteration - firstIteration) /
                          static_cast<double>(lastIteration - firstIteration);
      const qreal y = plot.bottom() -
                      plot.height() * (*value - *minimum) /
                          (*maximum - *minimum);
      if (!started) {
        path.moveTo(x, y);
        started = true;
      } else {
        path.lineTo(x, y);
      }
    }
    QPen pen(color, 1.8);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.drawPath(path);
  }

  QVector<TrainingSample> mSamples;
};

TrainingMonitorWidget::TrainingMonitorWidget(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(10, 8, 10, 8);
  layout->setSpacing(6);

  auto *heading = new QHBoxLayout();
  mTitle = new QLabel(QStringLiteral("尚未开始训练"), this);
  QFont titleFont = mTitle->font();
  titleFont.setBold(true);
  mTitle->setFont(titleFont);
  mState = new QLabel(QStringLiteral("空闲"), this);
  mState->setObjectName(QStringLiteral("statusWarn"));
  heading->addWidget(mTitle, 1);
  heading->addWidget(mState);
  layout->addLayout(heading);

  mProgress = new QProgressBar(this);
  mProgress->setRange(0, 100);
  mProgress->setValue(0);
  mProgress->setTextVisible(true);
  layout->addWidget(mProgress);

  auto *metrics = new QGridLayout();
  metrics->setHorizontalSpacing(18);
  metrics->setVerticalSpacing(2);
  mIteration = metricValue(this);
  mLoss = metricValue(this);
  mPsnr = metricValue(this);
  mGaussianCount = metricValue(this);
  mSpeed = metricValue(this);
  mElapsed = metricValue(this);
  mRemaining = metricValue(this);
  addMetric(metrics, 0, QStringLiteral("迭代"), mIteration);
  addMetric(metrics, 1, QStringLiteral("Loss"), mLoss);
  addMetric(metrics, 2, QStringLiteral("训练 PSNR"), mPsnr);
  addMetric(metrics, 3, QStringLiteral("高斯数量"), mGaussianCount);
  addMetric(metrics, 4, QStringLiteral("速度"), mSpeed);
  addMetric(metrics, 5, QStringLiteral("已用时"), mElapsed);
  addMetric(metrics, 6, QStringLiteral("预计剩余"), mRemaining);
  for (int column = 0; column < 7; ++column) {
    metrics->setColumnStretch(column, 1);
  }
  layout->addLayout(metrics);

  mCurves = new TrainingCurvesWidget(this);
  layout->addWidget(mCurves, 1);
}

void TrainingMonitorWidget::beginTraining(const QString &taskName,
                                          const QString &backend,
                                          const int expectedIterations) {
  mTelemetry.reset(expectedIterations);
  mTitle->setText(QStringLiteral("%1 · %2").arg(backend.toUpper(), taskName));
  mState->setText(QStringLiteral("启动中"));
  mProgress->setValue(0);
  refreshMetrics();
}

void TrainingMonitorWidget::updateStatus(const WorkerStatus &status) {
  mTelemetry.ingest(status);
  mState->setText(stageLabel(status.stage));
  if (status.progressPercent.has_value()) {
    mProgress->setValue(status.progressPercent.value());
  } else if (mTelemetry.iteration().has_value() &&
             mTelemetry.expectedIterations() > 0) {
    mProgress->setValue(std::clamp(
        static_cast<int>(std::round(
            mTelemetry.iteration().value() * 100.0 /
            mTelemetry.expectedIterations())),
        0, 100));
  }
  refreshMetrics();
}

void TrainingMonitorWidget::finishTraining(const bool succeeded,
                                           const bool cancelled) {
  mState->setText(succeeded ? QStringLiteral("已完成")
                  : cancelled ? QStringLiteral("已取消")
                              : QStringLiteral("失败"));
  if (succeeded) {
    mProgress->setValue(100);
  }
}

const TrainingTelemetry &TrainingMonitorWidget::telemetry() const {
  return mTelemetry;
}

void TrainingMonitorWidget::refreshMetrics() {
  const QLocale locale;
  const int total = mTelemetry.expectedIterations();
  mIteration->setText(mTelemetry.iteration().has_value()
                          ? QStringLiteral("%1 / %2")
                                .arg(locale.toString(*mTelemetry.iteration()))
                                .arg(total > 0 ? locale.toString(total)
                                               : QStringLiteral("-"))
                          : QStringLiteral("- / %1")
                                .arg(total > 0 ? locale.toString(total)
                                               : QStringLiteral("-")));
  mLoss->setText(mTelemetry.loss().has_value()
                     ? locale.toString(*mTelemetry.loss(), 'g', 6)
                     : QStringLiteral("-"));
  mPsnr->setText(mTelemetry.psnr().has_value()
                     ? QStringLiteral("%1 dB").arg(
                           locale.toString(*mTelemetry.psnr(), 'f', 2))
                     : QStringLiteral("-"));
  mGaussianCount->setText(
      mTelemetry.gaussianCount().has_value()
          ? locale.toString(*mTelemetry.gaussianCount())
          : QStringLiteral("-"));
  if (mTelemetry.iterationMilliseconds().has_value() &&
      *mTelemetry.iterationMilliseconds() > 0.0) {
    const double milliseconds = *mTelemetry.iterationMilliseconds();
    mSpeed->setText(QStringLiteral("%1 it/s")
                        .arg(locale.toString(1000.0 / milliseconds, 'f', 1)));
    if (mTelemetry.iteration().has_value() && total > 0) {
      const int remaining = std::max(total - *mTelemetry.iteration(), 0);
      mRemaining->setText(
          formattedDuration(remaining * milliseconds / 1000.0));
    } else {
      mRemaining->setText(QStringLiteral("-"));
    }
  } else {
    mSpeed->setText(QStringLiteral("-"));
    mRemaining->setText(QStringLiteral("-"));
  }
  mElapsed->setText(mTelemetry.elapsedSeconds().has_value()
                        ? formattedDuration(*mTelemetry.elapsedSeconds())
                        : QStringLiteral("-"));
  mCurves->setSamples(mTelemetry.samples());
}

} // namespace gsw

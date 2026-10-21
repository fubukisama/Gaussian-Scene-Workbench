#include "AppLanguage.h"
#include "AppTheme.h"
#include <QCoreApplication>
#include "TrainingMonitorWidget.h"
#include <QJsonArray>
#include <QJsonDocument>

#include <QFontMetrics>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gsw {

namespace {

QString formattedDuration(const double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0 ||
      seconds >= static_cast<double>(std::numeric_limits<qint64>::max())) {
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
  if(stage==QStringLiteral("coordinate_export"))return QCoreApplication::translate("Workbench", "恢复坐标与生成交付物");
  if (stage == QStringLiteral("paused")) return QCoreApplication::translate("Workbench", "已暂停");
  if (stage == QStringLiteral("queued")) {
    return QCoreApplication::translate("Workbench", "排队中");
  }
  if (stage == QStringLiteral("prepare") ||
      stage == QStringLiteral("preparing")) {
    return QCoreApplication::translate("Workbench", "准备数据");
  }
  if (stage == QStringLiteral("colmap")) {
    return QCoreApplication::translate("Workbench", "相机解算 · 稀疏点云生成");
  }
  if (stage == QStringLiteral("train")) {
    return QCoreApplication::translate("Workbench", "训练中");
  }
  if (stage == QStringLiteral("finalizing")) {
    return QCoreApplication::translate("Workbench", "发布模型");
  }
  return stage.isEmpty() ? QCoreApplication::translate("Workbench", "运行中") : stage;
}

QLabel *metricValue(QWidget *parent) {
  auto *label = new QLabel(QStringLiteral("-"), parent);
  QFont font = label->font();
  font.setBold(true);
  label->setFont(font);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QLabel *metricCaption(const char *source, QLabel *value) {
  auto *captionLabel = AppLanguage::text(new QLabel(value->parentWidget()), source);
  captionLabel->setObjectName(QStringLiteral("mutedLabel"));
  captionLabel->setWordWrap(true);
  return captionLabel;
}

QString profileStageLabel(const QString &stage) {
  if (stage == QStringLiteral("setup")) return QCoreApplication::translate("Workbench", "准备");
  if (stage == QStringLiteral("transfer")) return QCoreApplication::translate("Workbench", "图像传输");
  if (stage == QStringLiteral("render")) return QCoreApplication::translate("Workbench", "渲染");
  if (stage == QStringLiteral("loss")) return QCoreApplication::translate("Workbench", "损失计算");
  if (stage == QStringLiteral("backward")) return QCoreApplication::translate("Workbench", "反向传播");
  if (stage == QStringLiteral("densification")) return QCoreApplication::translate("Workbench", "增密");
  if (stage == QStringLiteral("optimizer")) return QCoreApplication::translate("Workbench", "优化器更新");
  if (stage == QStringLiteral("preview")) return QCoreApplication::translate("Workbench", "预览");
  if (stage == QStringLiteral("checkpoint")) return QCoreApplication::translate("Workbench", "检查点");
  if (stage == QStringLiteral("report")) return QCoreApplication::translate("Workbench", "上报");
  return {}; // Only the strict observer parser can supply these identifiers.
}

QString profileTimings(const QJsonObject &timings, const QLocale &locale) {
  QStringList entries;
  for (const QString &stage : {QStringLiteral("setup"), QStringLiteral("transfer"),
      QStringLiteral("render"), QStringLiteral("loss"), QStringLiteral("backward"),
      QStringLiteral("densification"), QStringLiteral("optimizer"), QStringLiteral("preview"),
      QStringLiteral("checkpoint"), QStringLiteral("report")}) {
    if (timings.contains(stage)) entries.append(QStringLiteral("%1 %2 ms")
        .arg(profileStageLabel(stage), locale.toString(timings.value(stage).toDouble(), 'f', 1)));
  }
  return entries.isEmpty() ? QStringLiteral("-") : entries.join(QStringLiteral(" · "));
}

QString observerMebibytes(const QJsonObject &sample, const QString &key, const QLocale &locale) {
  return sample.contains(key) ? locale.toString(sample.value(key).toDouble() / 1048576.0, 'f', 0)
                              : QStringLiteral("-");
}

QString growthReasonLabel(const QString &reason) {
  if (reason == QStringLiteral("vram_pressure")) return QCoreApplication::translate("Workbench", "显存压力");
  if (reason == QStringLiteral("probe_unavailable")) return QCoreApplication::translate("Workbench", "显存观测不可用");
  if (reason == QStringLiteral("invalid_observation")) return QCoreApplication::translate("Workbench", "显存观测无效");
  return QCoreApplication::translate("Workbench", "增密开销无法估算");
}

QString failureReasonLabel(const QString &reason) {
  if (reason == QStringLiteral("resource_probe_unavailable")) return QCoreApplication::translate("Workbench", "显存探测失败");
  if (reason == QStringLiteral("resource_estimate_unavailable")) return QCoreApplication::translate("Workbench", "增密峰值估计失败");
  if (reason == QStringLiteral("cuda_oom") || reason == QStringLiteral("cuda_oom_reported")) return QCoreApplication::translate("Workbench", "CUDA 显存不足");
  if (reason == QStringLiteral("training_exception")) return QCoreApplication::translate("Workbench", "训练发生异常");
  if (reason == QStringLiteral("manual_pause")) return QCoreApplication::translate("Workbench", "手动暂停");
  if (reason == QStringLiteral("vram_pressure")) return QCoreApplication::translate("Workbench", "显存压力");
  return {};
}

} // namespace

class TrainingCurvesWidget final : public QWidget {
public:
  explicit TrainingCurvesWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName(QStringLiteral("trainingCurves"));
    setMinimumHeight(fontMetrics().height() * 6 + 32);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }

  void setSamples(const QVector<TrainingSample> &samples) {
    mSamples = samples;
    update();
  }

protected:
  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
      setMinimumHeight(fontMetrics().height() * 6 + 32);
      updateGeometry();
    }
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::StyleChange) {
      // Theme changes repaint existing samples; they never modify telemetry.
      update();
    }
  }

  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool light = AppTheme::currentTheme() == UiTheme::Light;
    const QColor canvas = light ? QColor(255, 255, 255) : QColor(15, 18, 20);
    const QColor grid = light ? QColor(200, 200, 200) : QColor(47, 53, 57);
    const QColor lossColor = light ? QColor(16, 100, 85) : QColor(91, 199, 170);
    const QColor psnrColor = light ? QColor(128, 75, 10) : QColor(226, 181, 91);
    const QColor waitingColor = light ? QColor(68, 68, 68) : QColor(126, 134, 139);
    painter.fillRect(rect(), canvas);

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

    painter.setPen(grid);
    for (int line = 0; line <= 4; ++line) {
      const qreal y = plot.top() + plot.height() * line / 4.0;
      painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    painter.setPen(lossColor);
    painter.drawText(QPointF(plot.left(), labelHeight), QCoreApplication::translate("Workbench", "Loss"));
    const QString psnrLabel = QStringLiteral("PSNR");
    painter.setPen(psnrColor);
    painter.drawText(QPointF(plot.right() - metrics.horizontalAdvance(psnrLabel),
                             labelHeight),
                     psnrLabel);

    if (mSamples.size() < 2) {
      painter.setPen(waitingColor);
      const QString waiting = QCoreApplication::translate("Workbench", "等待训练采样…");
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
  setMinimumSize(0, 0);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSizeConstraint(QLayout::SetNoConstraint);
  mScrollArea = new QScrollArea(this);
  mScrollArea->setObjectName(QStringLiteral("trainingMonitorScroll"));
  mScrollArea->setFrameShape(QFrame::NoFrame);
  mScrollArea->setMinimumSize(0, 0);
  mScrollArea->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
  mScrollArea->setWidgetResizable(true);
  AppLanguage::bind(mScrollArea, "accessibleName", AppLanguage::source("训练监视内容"));
  AppLanguage::bind(mScrollArea, "accessibleDescription", AppLanguage::source("缩小面板时可滚动查看全部信息；指标会随宽度自动换行。"));
  outer->addWidget(mScrollArea);
  auto *content = new QWidget(mScrollArea);
  content->setObjectName(QStringLiteral("trainingMonitorContent"));
  auto *layout = new QVBoxLayout(content);
  layout->setSizeConstraint(QLayout::SetMinimumSize);
  layout->setContentsMargins(10, 8, 10, 8);
  layout->setSpacing(6);

  auto *heading = new QHBoxLayout();
  mTitle = AppLanguage::text(new QLabel(QCoreApplication::translate("Workbench", "尚未开始训练"), content), AppLanguage::source("尚未开始训练"));
  mTitle->setObjectName(QStringLiteral("trainingTaskTitle"));
  mTitle->setTextFormat(Qt::PlainText);
  mTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  mTitle->setMinimumWidth(0);
  QFont titleFont = mTitle->font();
  titleFont.setBold(true);
  mTitle->setFont(titleFont);
  mState = AppLanguage::text(new QLabel(QCoreApplication::translate("Workbench", "空闲"), content), AppLanguage::source("空闲"));
  mState->setTextFormat(Qt::PlainText);
  mState->setObjectName(QStringLiteral("statusWarn"));
  heading->addWidget(mTitle, 1);
  heading->addWidget(mState);
  layout->addLayout(heading);

  mProgress = new QProgressBar(content);
  mProgress->setRange(0, 100);
  mProgress->setValue(0);
  mProgress->setTextVisible(true);
  layout->addWidget(mProgress);
  mReconstructionQuality = new QLabel(content);
  mReconstructionQuality->setObjectName(QStringLiteral("reconstructionQualityLabel"));
  mReconstructionQuality->setWordWrap(true);
  mReconstructionQuality->hide();
  layout->addWidget(mReconstructionQuality);

  mEffectiveParameters = new QLabel(content);
  mEffectiveParameters->setObjectName(QStringLiteral("trainingEffectiveParameters"));
  mEffectiveParameters->setWordWrap(true);
  mEffectiveParameters->setTextFormat(Qt::PlainText);
  mEffectiveParameters->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mEffectiveParameters->hide();
  layout->addWidget(mEffectiveParameters);

  mMetricsGrid = new QGridLayout();
  mMetricsGrid->setHorizontalSpacing(18);
  mMetricsGrid->setVerticalSpacing(2);
  mIteration = metricValue(content);
  mLoss = metricValue(content);
  mPsnr = metricValue(content);
  mGaussianCount = metricValue(content);
  mSpeed = metricValue(content);
  mElapsed = metricValue(content);
  mRemaining = metricValue(content);
  mMetricValues = {mIteration, mLoss, mPsnr, mGaussianCount, mSpeed, mElapsed, mRemaining};
  mMetricCaptions = {metricCaption(AppLanguage::source("迭代"), mIteration),
      metricCaption(AppLanguage::source("Loss"), mLoss),
      metricCaption(AppLanguage::source("训练 PSNR"), mPsnr),
      metricCaption(AppLanguage::source("高斯数量"), mGaussianCount),
      metricCaption(AppLanguage::source("速度"), mSpeed),
      metricCaption(AppLanguage::source("已用时"), mElapsed),
      metricCaption(AppLanguage::source("预计剩余"), mRemaining)};
  mPrimitiveCountCaption = mMetricCaptions[3];
  for (int index = 0; index < 7; ++index) {
    mMetricValues[index]->setObjectName(QStringLiteral("trainingMetricValue%1").arg(index));
    mMetricValues[index]->setTextFormat(Qt::PlainText);
  }
  layout->addLayout(mMetricsGrid);

  mQualityEvidence = AppLanguage::text(new QLabel(content), AppLanguage::source(
      "质量验证：未提供独立验证结果；训练 PSNR 不等于最终质量。"));
  mQualityEvidence->setObjectName(QStringLiteral("trainingQualityEvidence"));
  mQualityEvidence->setWordWrap(true);
  mQualityEvidence->setTextFormat(Qt::PlainText);
  mQualityEvidence->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mQualityEvidence->setMinimumWidth(0);
  mQualityEvidence->hide();
  layout->addWidget(mQualityEvidence);

  mPerformanceDetails = new QLabel(content);
  mPerformanceDetails->setObjectName(QStringLiteral("trainingPerformanceDetails"));
  mPerformanceDetails->setWordWrap(true);
  mPerformanceDetails->setTextFormat(Qt::PlainText);
  mPerformanceDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mPerformanceDetails->setMinimumWidth(0);
  AppLanguage::bind(mPerformanceDetails, "accessibleName", AppLanguage::source("整步性能与显存"));
  AppLanguage::bind(mPerformanceDetails, "toolTip", AppLanguage::source("整步时间包含优化器、增密和预览。主机与 CUDA 阶段可能重叠，不应相加；速度与预计剩余仅使用整步墙钟时间。"));
  mPerformanceDetails->hide();
  layout->addWidget(mPerformanceDetails);

  mResourceGuardState = new QLabel(content);
  mResourceGuardState->setObjectName(QStringLiteral("trainingResourceGuardState"));
  mResourceGuardState->setWordWrap(true);
  mResourceGuardState->setTextFormat(Qt::PlainText);
  mResourceGuardState->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mResourceGuardState->setMinimumWidth(0);
  mResourceGuardState->hide();
  layout->addWidget(mResourceGuardState);

  mFailureReasonLabel = new QLabel(content);
  mFailureReasonLabel->setObjectName(QStringLiteral("trainingFailureReason"));
  mFailureReasonLabel->setWordWrap(true);
  mFailureReasonLabel->setTextFormat(Qt::PlainText);
  mFailureReasonLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mFailureReasonLabel->setMinimumWidth(0);
  mFailureReasonLabel->hide();
  layout->addWidget(mFailureReasonLabel);

  mDeliveryState=new QLabel(content);
  mDeliveryState->setObjectName(QStringLiteral("trainingDeliveryState"));
  mDeliveryState->setWordWrap(true);mDeliveryState->setTextFormat(Qt::PlainText);
  mDeliveryState->setTextInteractionFlags(Qt::TextSelectableByMouse);mDeliveryState->setMinimumWidth(0);
  AppLanguage::bind(mDeliveryState,"accessibleName",AppLanguage::source("模型交付（训练成功与坐标导出分别记录）"));
  mDeliveryState->hide();layout->addWidget(mDeliveryState);

  mGrowthWarning = new QLabel(content);
  mGrowthWarning->setObjectName(QStringLiteral("trainingGrowthWarning"));
  mGrowthWarning->setWordWrap(true);
  mGrowthWarning->setTextFormat(Qt::PlainText);
  mGrowthWarning->setTextInteractionFlags(Qt::TextSelectableByMouse);
  mGrowthWarning->setMinimumWidth(0);
  AppLanguage::bind(mGrowthWarning, "accessibleName", AppLanguage::source("增密资源警告"));
  mGrowthWarning->hide();
  layout->addWidget(mGrowthWarning);

  mDensityWarning = new QLabel(content);
  mDensityWarning->setObjectName(QStringLiteral("densityGuardWarning"));
  mDensityWarning->setWordWrap(true);
  mDensityWarning->hide();
  layout->addWidget(mDensityWarning);

  mCurves = new TrainingCurvesWidget(content);
  layout->addWidget(mCurves, 1);
  mScrollArea->setWidget(content);
  mScrollArea->viewport()->installEventFilter(this);
  mTitle->installEventFilter(this);
  relayoutMetrics();
  AppLanguage::onChanged(this, [this]() { retranslateStatus(); });
}

bool TrainingMonitorWidget::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::FontChange) {
    // QSS can notify related widgets separately. Measure after font/style
    // propagation, not against a mixture of old and new font metrics.
    QTimer::singleShot(0, this, [this] { relayoutMetrics(); refreshTaskTitle(); });
  } else if (event->type() == QEvent::Resize) {
    if (watched == mScrollArea->viewport()) relayoutMetrics();
    if (watched == mTitle) refreshTaskTitle();
  }
  return QWidget::eventFilter(watched, event);
}

void TrainingMonitorWidget::refreshTaskTitle() {
  const QString full = mHasTraining ? mTaskTitle
      : QCoreApplication::translate("Workbench", "尚未开始训练");
  QString singleLine = full;
  for (const QChar separator : {QChar('\r'), QChar('\n'), QChar('\t'), QChar(0x2028), QChar(0x2029)})
    singleLine.replace(separator, QLatin1Char(' '));
  mTitle->setText(mTitle->fontMetrics().elidedText(singleLine, Qt::ElideMiddle,
                                                 std::max(0, mTitle->width())));
  // Qt tooltips detect HTML automatically. Escape legal markup in user names
  // so the complete name is displayed literally, never interpreted as HTML.
  mTitle->setToolTip(Qt::convertFromPlainText(full));
}

void TrainingMonitorWidget::relayoutMetrics() {
  const QString iterationWidth = QStringLiteral("%1 / %1").arg(QLocale().toString(200000));
  int tileWidth = mIteration->fontMetrics().horizontalAdvance(iterationWidth);
  for (int index = 0; index < 7; ++index) {
    tileWidth = std::max(tileWidth, mMetricCaptions[index]->fontMetrics().horizontalAdvance(mMetricCaptions[index]->text()));
    tileWidth = std::max(tileWidth, mMetricValues[index]->fontMetrics().horizontalAdvance(mMetricValues[index]->text()));
  }
  const int spacing = mMetricsGrid->horizontalSpacing();
  const int available = std::max(0, mScrollArea->viewport()->width() - 20);
  const int columns = std::clamp((available + spacing) / std::max(1, tileWidth + spacing), 1, 7);
  if (columns == mMetricColumns) return;
  mMetricColumns = columns;
  setProperty("metricColumns", columns);
  for (int index = 0; index < 7; ++index) {
    mMetricsGrid->removeWidget(mMetricCaptions[index]);
    mMetricsGrid->removeWidget(mMetricValues[index]);
    mMetricsGrid->setColumnStretch(index, index < columns ? 1 : 0);
  }
  for (int index = 0; index < 7; ++index) {
    const int row = (index / columns) * 2;
    const int column = index % columns;
    mMetricsGrid->addWidget(mMetricCaptions[index], row, column);
    mMetricsGrid->addWidget(mMetricValues[index], row + 1, column);
  }
}

void TrainingMonitorWidget::retranslateStatus() {
  if (mHasTraining) mTitle->setText(mTaskTitle);
  if (mFinished) {
    mState->setText(mSucceeded ? QCoreApplication::translate("Workbench", "已完成")
        : mPaused ? QCoreApplication::translate("Workbench", "已暂停")
        : mCancelled ? QCoreApplication::translate("Workbench", "已取消")
                     : QCoreApplication::translate("Workbench", "失败"));
  } else if (mHasTraining) {
    mState->setText(mLastStage.isEmpty() ? QCoreApplication::translate("Workbench", "启动中") : stageLabel(mLastStage));
  }
  mPrimitiveCountCaption->setText(mSparsePreview ? QCoreApplication::translate("Workbench", "稀疏点数")
                                                : QCoreApplication::translate("Workbench", "高斯数量"));
  // Repaint existing samples; never ingest a status twice or reset telemetry.
  refreshMetrics();
}

void TrainingMonitorWidget::beginTraining(const QString &taskName,
                                          const QString &backend,
                                          const int expectedIterations) {
  mTelemetry.reset(expectedIterations);
  mHasTraining = true;
  mFinished = false;
  mSparsePreview = false;
  mLastStage.clear();
  mDensityGuardIteration = 0;
  mDensityGuardDeferred = 0;
  mReconstructionStatus = {};
  mTrainingSummary = {};
  mRuntimeConfiguration = {};
  mFailureReason.clear();
  mTrainingDelivery={};
  mTaskTitle = QStringLiteral("%1 · %2").arg(backend.toUpper(), taskName);
  mTitle->setText(mTaskTitle);
  mState->setText(QCoreApplication::translate("Workbench", "启动中"));
  mPrimitiveCountCaption->setText(QCoreApplication::translate("Workbench", "高斯数量"));
  mProgress->setValue(0);
  refreshMetrics();
}

void TrainingMonitorWidget::updateStatus(const WorkerStatus &status) {
  mTelemetry.ingest(status);
  // Status events are partial: a later progress/final event must not erase
  // runtime provenance or the last classified pause/failure reason.
  if (!status.runtimeConfiguration.isEmpty()) mRuntimeConfiguration = status.runtimeConfiguration;
  if (!status.failureReason.isEmpty()) mFailureReason = status.failureReason;
  if(!status.trainingDelivery.isEmpty())mTrainingDelivery=status.trainingDelivery;
  if (!status.trainingSummary.isEmpty() &&
      (mTrainingSummary.value(QStringLiteral("phase")).toString() != QStringLiteral("loaded") ||
       status.trainingSummary.value(QStringLiteral("phase")).toString() == QStringLiteral("loaded")))
    mTrainingSummary = status.trainingSummary;
  mLastStage = status.stage;
  const bool knownIssue = QStringList{QStringLiteral("reconstruction_quality"),
      QStringLiteral("source_frames_missing"), QStringLiteral("training_renderer_resume")}
      .contains(status.generationIssue);
  if (!status.reconstructionQuality.isEmpty() || knownIssue) {
    mReconstructionStatus = status;
  }
  if (status.densityGuardIteration.has_value() && status.densityGuardDeferred.has_value()) {
    mDensityGuardIteration = *status.densityGuardIteration;
    mDensityGuardDeferred = *status.densityGuardDeferred;
  }
  mSparsePreview = status.previewKind == QStringLiteral("colmap_sparse") || status.stage == QStringLiteral("colmap");
  mState->setText(stageLabel(status.stage));
  mPrimitiveCountCaption->setText(
      status.previewKind == QStringLiteral("colmap_sparse") ||
              status.stage == QStringLiteral("colmap")
          ? QCoreApplication::translate("Workbench", "稀疏点数")
          : QCoreApplication::translate("Workbench", "高斯数量"));
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
                                           const bool cancelled, const bool paused) {
  mFinished = true;
  mSucceeded = succeeded;
  mCancelled = cancelled;
  mPaused = paused;
  mState->setText(succeeded ? QCoreApplication::translate("Workbench", "已完成")
                  : paused ? QCoreApplication::translate("Workbench", "已暂停")
                  : cancelled ? QCoreApplication::translate("Workbench", "已取消")
                              : QCoreApplication::translate("Workbench", "失败"));
  if (succeeded) {
    mProgress->setValue(100);
  }
  refreshMetrics();
}

const TrainingTelemetry &TrainingMonitorWidget::telemetry() const {
  return mTelemetry;
}

void TrainingMonitorWidget::refreshMetrics() {
  const QLocale locale;
  mQualityEvidence->setVisible(mHasTraining);
  mEffectiveParameters->setVisible(!mTrainingSummary.isEmpty());
  if (!mTrainingSummary.isEmpty()) {
    const auto &summary = mTrainingSummary;
    const QString optimizer = summary.value(QStringLiteral("optimizer")).toString() == QStringLiteral("sparse_adam")
        ? QCoreApplication::translate("Workbench", "稀疏 Adam") : QStringLiteral("Adam");
    const int resolution = summary.value(QStringLiteral("resolution")).toInt();
    const QString resolutionText = (resolution == 1 || resolution == 2 || resolution == 4 || resolution == 8)
        ? QCoreApplication::translate("Workbench", "1/%1 分辨率").arg(resolution)
        : QCoreApplication::translate("Workbench", "目标宽度 %1 px").arg(locale.toString(resolution));
    QString text = QCoreApplication::translate("Workbench", "生效参数：%1 · %2 次迭代 · %3 · 优化器 %4。")
        .arg(summary.value(QStringLiteral("backend")).toString().toUpper(),
             locale.toString(summary.value(QStringLiteral("iterations")).toInt()),
             resolutionText, optimizer);
    if (summary.value(QStringLiteral("phase")).toString() == QStringLiteral("loaded")) {
      QStringList dimensions;
      for (const QJsonValue &value : summary.value(QStringLiteral("trainDimensions")).toArray()) {
        const QJsonArray size = value.toArray();
        dimensions.append(QCoreApplication::translate("Workbench", "%1 × %2 px（%3 张）")
            .arg(locale.toString(size.at(0).toInt()), locale.toString(size.at(1).toInt()),
                 locale.toString(size.at(2).toInt())));
      }
      const int extra = summary.value(QStringLiteral("trainDimensionKinds")).toInt() - dimensions.size();
      if (extra > 0) dimensions.append(QCoreApplication::translate("Workbench", "另有 %1 种尺寸").arg(locale.toString(extra)));
      text += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "实际训练图像：%1 张 · %2 · 总计 %3 MP。")
          .arg(locale.toString(summary.value(QStringLiteral("trainImageCount")).toInt()),
               dimensions.join(QStringLiteral(" · ")),
               locale.toString(summary.value(QStringLiteral("trainPixels")).toDouble() / 1000000.0, 'f', 2));
    } else {
      text += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "实际训练图像尺寸将在相机加载后确认。");
    }
    mEffectiveParameters->setText(text);
    QString details = QCoreApplication::translate("Workbench", "增密截止：%1 · 间隔：%2 · 梯度阈值：%3")
        .arg(locale.toString(summary.value(QStringLiteral("densifyUntil")).toInt()),
             locale.toString(summary.value(QStringLiteral("densificationInterval")).toInt()),
             locale.toString(summary.value(QStringLiteral("densifyGradient")).toDouble(), 'g', 6));
    if (summary.value(QStringLiteral("backend")).toString() == QStringLiteral("3dgs")) {
      const auto enabled = [](bool value) {
        return value ? QCoreApplication::translate("Workbench", "启用") : QCoreApplication::translate("Workbench", "禁用");
      };
      details += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "抗锯齿：%1 · 曝光补偿：%2")
          .arg(enabled(summary.value(QStringLiteral("antialiasing")).toBool()),
               enabled(summary.value(QStringLiteral("exposureCompensation")).toBool()));
    } else {
      details += QLatin1Char('\n') + QCoreApplication::translate("Workbench", "深度混合比例：%1")
          .arg(locale.toString(summary.value(QStringLiteral("depthRatio")).toDouble(), 'g', 6));
    }
    mEffectiveParameters->setToolTip(text + QLatin1Char('\n') + details);
  }
  const WorkerStatus &quality = mReconstructionStatus;
  mReconstructionQuality->setVisible(!quality.reconstructionQuality.isEmpty() || !quality.generationIssue.isEmpty());
  QString message;
  if (quality.generationIssue == QStringLiteral("training_renderer_resume")) {
    message = QCoreApplication::translate("Workbench", "检查点使用的训练渲染后端不可用或已改变。请恢复同一实验环境后续训；未自动切换后端或重置优化器。");
  } else if (quality.generationIssue == QStringLiteral("source_frames_missing")) {
    message = QCoreApplication::translate("Workbench", "原始帧无法完整恢复。请重新导入原始照片或视频；已保留当前模型，不会使用缺失照片的子集继续训练。");
  } else if (!quality.reconstructionQuality.isEmpty()) {
    const QString phase = quality.reconstructionQuality == QStringLiteral("repairing")
        ? QCoreApplication::translate("Workbench", "有限重试中")
        : quality.reconstructionQuality == QStringLiteral("rejected")
        ? QCoreApplication::translate("Workbench", "质量检查未通过 · 已阻止训练")
        : quality.reconstructionQuality == QStringLiteral("partial")
        ? QCoreApplication::translate("Workbench", "部分相机注册 · 覆盖不足")
        : QCoreApplication::translate("Workbench", "最低质量检查通过");
    message = QCoreApplication::translate("Workbench", "重建检查：相机 %1/%2 · 有效稀疏点 %3 · %4。高斯数量和训练 PSNR 不代表新视角质量。")
        .arg(locale.toString(quality.reconstructionViews.value_or(0)),
             locale.toString(quality.reconstructionInputs.value_or(0)),
             locale.toString(quality.reconstructionPoints.value_or(0)), phase);
  }
  mReconstructionQuality->setText(message);
  mDensityWarning->setVisible(mDensityGuardDeferred > 0);
  if (mDensityGuardDeferred > 0) {
    mDensityWarning->setText(QCoreApplication::translate("Workbench",
        "过度裁剪保护：第 %1 次迭代暂缓删除 %2 个高斯。请检查拍摄覆盖与重建尺度；数量不代表几何质量。")
        .arg(locale.toString(mDensityGuardIteration), locale.toString(mDensityGuardDeferred)));
  }
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
  if (const auto rate = mTelemetry.iterationsPerSecond()) {
    mSpeed->setText(QStringLiteral("%1 it/s")
                        .arg(locale.toString(*rate, 'f', 1)));
    const auto remaining = mTelemetry.estimatedRemainingSeconds();
    mRemaining->setText(remaining ? formattedDuration(*remaining) : QStringLiteral("-"));
  } else {
    mSpeed->setText(QStringLiteral("-"));
    mRemaining->setText(QStringLiteral("-"));
  }
  mElapsed->setText(mTelemetry.elapsedSeconds().has_value()
                        ? formattedDuration(*mTelemetry.elapsedSeconds())
                        : QStringLiteral("-"));
  if (mFinished) mRemaining->setText(QStringLiteral("-"));
  const QJsonObject &performance = mTelemetry.performance();
  QStringList profileLines;
  if (!performance.isEmpty()) {
    profileLines.append(QCoreApplication::translate("Workbench", "全步：%1 ms · 样本迭代 %2")
        .arg(locale.toString(*mTelemetry.wallStepMilliseconds(), 'f', 1),
             locale.toString(performance.value(QStringLiteral("iteration")).toInteger())));
    if (performance.contains(QStringLiteral("renderer_engine")) ||
        performance.contains(QStringLiteral("loss_engine"))) {
      const QString renderer = performance.value(QStringLiteral("renderer_engine")).toString();
      const QString loss = performance.value(QStringLiteral("loss_engine")).toString();
      profileLines.append(QCoreApplication::translate("Workbench", "实际渲染后端：%1 · 损失计算：%2")
          .arg(renderer.isEmpty() ? QStringLiteral("-") : renderer == QStringLiteral("fastergs")
                   ? QCoreApplication::translate("Workbench", "Fast-GS")
                   : QCoreApplication::translate("Workbench", "标准"),
               loss.isEmpty() ? QStringLiteral("-") : loss == QStringLiteral("fused_l1_ssim")
                   ? QCoreApplication::translate("Workbench", "融合 L1 + SSIM")
                   : QCoreApplication::translate("Workbench", "标准")));
    }
    profileLines.append(QCoreApplication::translate("Workbench", "主机阶段（含 GPU 提交）：%1")
        .arg(profileTimings(performance.value(QStringLiteral("stages_ms")).toObject(), locale)));
    const QJsonObject gpuStages = performance.value(QStringLiteral("gpu_stages_ms")).toObject();
    if (!gpuStages.isEmpty()) profileLines.append(QCoreApplication::translate("Workbench", "CUDA 阶段（非整步）：%1")
        .arg(profileTimings(gpuStages, locale)));
    if (performance.contains(QStringLiteral("torch_allocated_bytes")) ||
        performance.contains(QStringLiteral("device_total_bytes")))
      profileLines.append(QCoreApplication::translate("Workbench", "GPU 内存：Torch %1 MiB · 保留 %2 MiB · 峰值 %3 MiB · 设备空闲 %4 / %5 MiB")
          .arg(observerMebibytes(performance, QStringLiteral("torch_allocated_bytes"), locale),
               observerMebibytes(performance, QStringLiteral("torch_reserved_bytes"), locale),
               observerMebibytes(performance, QStringLiteral("torch_peak_allocated_bytes"), locale),
               observerMebibytes(performance, QStringLiteral("device_free_bytes"), locale),
               observerMebibytes(performance, QStringLiteral("device_total_bytes"), locale)));
  } else if (const auto cuda = mTelemetry.iterationMilliseconds()) {
    profileLines.append(QCoreApplication::translate("Workbench", "CUDA 阶段（非整步）：%1")
        .arg(QStringLiteral("%1 ms").arg(locale.toString(*cuda, 'f', 1))));
  }
  mPerformanceDetails->setVisible(!profileLines.isEmpty());
  mPerformanceDetails->setText(profileLines.join(QLatin1Char('\n')));
  const auto guard = mRuntimeConfiguration.value(QStringLiteral("effective")).toObject()
      .value(QStringLiteral("resourceGuard")).toObject();
  mResourceGuardState->setVisible(guard.value(QStringLiteral("enabled")).isBool());
  if (guard.value(QStringLiteral("enabled")).isBool()) {
    const auto mode = guard.value(QStringLiteral("mode")).toString() == QStringLiteral("pause")
        ? QCoreApplication::translate("Workbench", "质量优先：保存完整状态并安全暂停")
        : QCoreApplication::translate("Workbench", "继续训练：暂缓增密并提示质量风险");
    mResourceGuardState->setText(guard.value(QStringLiteral("enabled")).toBool()
        ? QCoreApplication::translate("Workbench", "显存保护已启用：%1 · 目标 %2% · 最低余量 %3 MiB · 连续拒绝 %4 次。")
            .arg(mode, locale.toString(guard.value(QStringLiteral("usage_fraction")).toDouble() * 100.0, 'g', 8),
                 locale.toString(guard.value(QStringLiteral("minimum_spare_mib")).toInteger()),
                 locale.toString(guard.value(QStringLiteral("max_consecutive_denials")).toInteger()))
        : QCoreApplication::translate("Workbench", "显存保护已关闭；不会按软件资源阈值暂缓增密。实际 CUDA 内存不足仍会报错。"));
    // Machine-facing policy identifiers and values remain exact and untranslated.
    mResourceGuardState->setToolTip(QString::fromUtf8(QJsonDocument(mRuntimeConfiguration).toJson(QJsonDocument::Indented)));
  }
  mFailureReasonLabel->setText(failureReasonLabel(mFailureReason));
  mFailureReasonLabel->setVisible(!mFailureReasonLabel->text().isEmpty());
  mFailureReasonLabel->setToolTip(mFailureReason);
  const bool trained=mTrainingDelivery.value(QStringLiteral("training_status"))==QStringLiteral("done");
  mDeliveryState->setVisible(trained);
  if(trained) {
    const auto state=mTrainingDelivery.value(QStringLiteral("export_status")).toString();
    const QString delivery=state==QStringLiteral("ready")?QCoreApplication::translate("Workbench","已完成"):
        state==QStringLiteral("running")?QCoreApplication::translate("Workbench","运行中"):
        state==QStringLiteral("cancelled")?QCoreApplication::translate("Workbench","已取消"):
        state==QStringLiteral("blocked")?QCoreApplication::translate("Workbench","交付受限"):
        QCoreApplication::translate("Workbench","失败");
    mDeliveryState->setText(QCoreApplication::translate("Workbench","训练：已完成；坐标交付：%1。").arg(delivery));
    mDeliveryState->setToolTip(QString::fromUtf8(QJsonDocument(mTrainingDelivery).toJson(QJsonDocument::Indented)));
    // A failed/cancelled publication is not a failed/cancelled optimizer run.
    mState->setText(QCoreApplication::translate("Workbench","训练已完成"));
    mProgress->setValue(100);
  }
  const QJsonObject &growth = mTelemetry.growth();
  const bool deferred = growth.value(QStringLiteral("state")).toString() == QStringLiteral("deferred");
  mGrowthWarning->setVisible(deferred);
  if (deferred) {
    const QString reason = growth.value(QStringLiteral("reason")).toString();
    const bool observed = growth.value(QStringLiteral("observation_valid")).toBool();
    mGrowthWarning->setText(QCoreApplication::translate("Workbench", "暂缓增密（迭代 %1 · %2）：空闲 %3 MiB · 本次预计新增峰值 %4 MiB · 可用于增密 %5 MiB。持续受限可能减少细节；保留完整检查点后调整资源续训。")
        .arg(locale.toString(growth.value(QStringLiteral("iteration")).toInteger()),
             growthReasonLabel(reason),
             observed ? observerMebibytes(growth, QStringLiteral("free_bytes"), locale) : QStringLiteral("-"),
             reason == QStringLiteral("estimate_unavailable") ? QStringLiteral("-")
                 : observerMebibytes(growth, QStringLiteral("additional_peak_bytes"), locale),
             observed ? observerMebibytes(growth, QStringLiteral("available_bytes"), locale) : QStringLiteral("-")));
  }
  mCurves->setSamples(mTelemetry.samples());
  relayoutMetrics();
  refreshTaskTitle();
}

} // namespace gsw

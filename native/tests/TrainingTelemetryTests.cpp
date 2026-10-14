#include "TrainingTelemetry.h"

#include <QtTest>
#include <QJsonDocument>
#include <limits>

using namespace gsw;

namespace {
QJsonObject performance(const int iteration = 12) {
  return {{"version", 1}, {"backend", "3dgs"}, {"iteration", iteration},
          {"wall_step_ms", 200.0},
          {"stages_ms", QJsonObject{{"transfer", 15.0}, {"render", 90.0},
             {"loss", 10.0}, {"backward", 40.0}, {"densification", 5.0},
             {"optimizer", 15.0}, {"preview", 25.0}, {"checkpoint", 0.0},
             {"report", 0.0}, {"setup", 0.0}}},
          {"gpu_stages_ms", QJsonObject{{"render", 80.0}, {"loss", 9.0},
                                        {"backward", 35.0}}},
          {"torch_allocated_bytes", 4194304}, {"torch_reserved_bytes", 8388608},
          {"torch_peak_allocated_bytes", 6291456},
          {"device_free_bytes", 10737418240.0}, {"device_total_bytes", 12884901888.0}};
}

QJsonObject growth(const int iteration = 12, const bool admitted = false) {
  QJsonObject result{{"version", 1}, {"backend", "3dgs"}, {"iteration", iteration},
                    {"state", admitted ? "admitted" : "deferred"},
                    {"reason", admitted ? "admitted" : "vram_pressure"},
                    {"allowed", admitted}, {"observation_valid", true},
                    {"candidate_counts_known", true},
                    {"point_count", 10}, {"transient_point_count", 18},
                    {"final_point_count", 15}, {"parameter_bytes", 2360},
                    {"optimizer_state_bytes", 4720}, {"additional_peak_bytes", 20088},
                    {"free_bytes", 1073741824}, {"total_bytes", 12884901888.0},
                    {"spare_bytes", 1288490189}, {"available_bytes", 0},
                    {"cache_credit_bytes", 0}, {"clone_count", 2}, {"split_count", 3}};
  return result;
}

QByteArray statusEvent(const QJsonObject &metadata, const QString &key) {
  return QJsonDocument(QJsonObject{{"version", 1}, {"type", "status"},
      {"state", "running"}, {"stage", "train"}, {"iteration", 12},
      {"loss", 0.1}, {key, metadata}}).toJson(QJsonDocument::Compact);
}
}

class TrainingTelemetryTests final : public QObject {
  Q_OBJECT

private slots:
  void publicPerformanceUsesCompleteWallTimeNotLegacyCudaEvent();
  void publicDecoderValidatesOptionalMetadataIndependently();
  void bothBackendsRetainIndependentObserverContracts_data();
  void bothBackendsRetainIndependentObserverContracts();
  void independentObserversRejectStaleOrInvalidSamples();
  void speedAndEtaRequireCompleteWallObservation();
  void actualRendererAndLossMetadataAreOptionalButStrict();
  void rendererResumeFailureIsRecognizedWithoutWeakeningIssueValidation();
  void capsHistoryAndReplacesDuplicateIteration();
};

void TrainingTelemetryTests::rendererResumeFailureIsRecognizedWithoutWeakeningIssueValidation() {
  QJsonObject event{{"version", 1}, {"type", "status"}, {"state", "failed"},
      {"stage", "train"}, {"iteration", 4407},
      {"generationIssue", "training_renderer_resume"}};
  const auto blocked = ProcessSupervisor::decodeStatusEvent(QJsonDocument(event).toJson());
  QVERIFY(blocked.has_value());
  QCOMPARE(blocked->generationIssue, QStringLiteral("training_renderer_resume"));
  QCOMPARE(blocked->iteration.value(), 4407);
  event["generationIssue"] = "unknown_resume_issue";
  QVERIFY(!ProcessSupervisor::decodeStatusEvent(QJsonDocument(event).toJson()).has_value());
}

void TrainingTelemetryTests::actualRendererAndLossMetadataAreOptionalButStrict() {
  auto actual = performance();
  actual["renderer_engine"] = "fastergs";
  actual["loss_engine"] = "fused_l1_ssim";
  QCOMPARE(normalizeTrainingPerformance(actual), actual);
  for (const QString &field : {QStringLiteral("renderer_engine"), QStringLiteral("loss_engine")}) {
    auto invalid = actual;
    invalid[field] = "requested_but_not_active";
    const auto event = ProcessSupervisor::decodeStatusEvent(statusEvent(invalid, "trainingPerformance"));
    QVERIFY(event.has_value());
    QVERIFY(event->trainingPerformance.isEmpty());
    QCOMPARE(event->iteration.value(), 12);
  }
  actual["renderer_engine"] = "stock";
  actual["loss_engine"] = "stock";
  QCOMPARE(normalizeTrainingPerformance(actual), actual);
  QCOMPARE(normalizeTrainingPerformance(performance()), performance());
}

void TrainingTelemetryTests::bothBackendsRetainIndependentObserverContracts_data() {
  QTest::addColumn<QString>("backend");
  QTest::newRow("3dgs") << QStringLiteral("3dgs");
  QTest::newRow("2dgs") << QStringLiteral("2dgs");
}

void TrainingTelemetryTests::bothBackendsRetainIndependentObserverContracts() {
  QFETCH(QString, backend);
  auto wall = performance();
  auto decision = growth();
  wall["backend"] = backend;
  decision["backend"] = backend;
  const auto encoded = [&](const QJsonObject &wallSample, const QJsonObject &growthSample) {
    return QJsonDocument(QJsonObject{{"version", 1}, {"type", "status"},
        {"state", "running"}, {"stage", "train"}, {"iteration", 12},
        {"trainingPerformance", wallSample}, {"trainingGrowth", growthSample}}).toJson();
  };
  const auto parsed = ProcessSupervisor::decodeStatusEvent(encoded(wall, decision));
  QVERIFY(parsed.has_value());
  TrainingTelemetry telemetry;
  QVERIFY(telemetry.ingest(*parsed));
  QCOMPARE(telemetry.performance(), wall);
  QCOMPARE(telemetry.growth(), decision);
  auto bad = wall;
  bad["gpu_stages_ms"] = QJsonObject{{"render", true}};
  const auto validGrowthOnly = ProcessSupervisor::decodeStatusEvent(encoded(bad, decision));
  QVERIFY(validGrowthOnly.has_value());
  QVERIFY(validGrowthOnly->trainingPerformance.isEmpty());
  QCOMPARE(validGrowthOnly->trainingGrowth, decision);
  bad = decision;
  bad["clone_count"] = 1.5;
  const auto validPerformanceOnly = ProcessSupervisor::decodeStatusEvent(encoded(wall, bad));
  QVERIFY(validPerformanceOnly.has_value());
  QCOMPARE(validPerformanceOnly->trainingPerformance, wall);
  QVERIFY(validPerformanceOnly->trainingGrowth.isEmpty());
  wall["untrusted_caption"] = "ignored";
  QCOMPARE(normalizeTrainingPerformance(wall), telemetry.performance());
}

void TrainingTelemetryTests::speedAndEtaRequireCompleteWallObservation() {
  TrainingTelemetry telemetry;
  telemetry.reset(100);
  WorkerStatus legacy;
  legacy.iteration = 20;
  legacy.iterationMilliseconds = 40.0;
  QVERIFY(telemetry.ingest(legacy));
  QVERIFY(!telemetry.iterationsPerSecond().has_value());
  QVERIFY(!telemetry.estimatedRemainingSeconds().has_value());
  WorkerStatus wall;
  wall.trainingPerformance = performance(20);
  QVERIFY(telemetry.ingest(wall));
  QCOMPARE(telemetry.iterationsPerSecond().value(), 5.0);
  QCOMPARE(telemetry.estimatedRemainingSeconds().value(), 16.0);
  wall.trainingPerformance = performance(21);
  wall.trainingPerformance["wall_step_ms"] = 0.0;
  QVERIFY(telemetry.ingest(wall));
  QVERIFY(!telemetry.iterationsPerSecond().has_value());
  QVERIFY(!telemetry.estimatedRemainingSeconds().has_value());
}

void TrainingTelemetryTests::independentObserversRejectStaleOrInvalidSamples() {
  TrainingTelemetry telemetry;
  telemetry.reset(100);
  WorkerStatus latest;
  latest.iteration = 20;
  latest.loss = 0.1;
  latest.gaussianCount = 1000;
  latest.elapsedSeconds = 5.0;
  latest.trainingPerformance = performance(18);
  latest.trainingGrowth = growth(18);
  QVERIFY(telemetry.ingest(latest));
  QCOMPARE(telemetry.growth(), growth(18));

  WorkerStatus stale;
  stale.iteration = 10;
  stale.loss = 0.9;
  stale.gaussianCount = 50;
  stale.elapsedSeconds = 1.0;
  stale.totalIterations = 50;
  stale.trainingPerformance = performance(17);
  stale.trainingGrowth = growth(17, true);
  QVERIFY(!telemetry.ingest(stale));
  QCOMPARE(telemetry.iteration().value(), 20);
  QCOMPARE(telemetry.loss().value(), 0.1);
  QCOMPARE(telemetry.gaussianCount().value(), qint64(1000));
  QCOMPARE(telemetry.elapsedSeconds().value(), 5.0);
  QCOMPARE(telemetry.expectedIterations(), 100);
  QCOMPARE(telemetry.samples().size(), 1);
  QCOMPARE(telemetry.performance(), performance(18));
  QCOMPARE(telemetry.growth(), growth(18));

  // Observer events arrive independently from numerical metric snapshots.
  stale.trainingPerformance = performance(21);
  stale.trainingGrowth = growth(21, true);
  QVERIFY(telemetry.ingest(stale));
  QCOMPARE(telemetry.iteration().value(), 20);
  QCOMPARE(telemetry.performance(), performance(21));
  QCOMPARE(telemetry.growth(), growth(21, true));
  WorkerStatus invalid;
  invalid.trainingPerformance = performance(22);
  invalid.trainingPerformance["wall_step_ms"] = true;
  invalid.trainingGrowth = growth(22);
  invalid.trainingGrowth["clone_count"] = -1;
  QVERIFY(!telemetry.ingest(invalid));
  QCOMPARE(telemetry.performance(), performance(21));
  QCOMPARE(telemetry.growth(), growth(21, true));
  telemetry.reset();
  QVERIFY(telemetry.growth().isEmpty());
}

void TrainingTelemetryTests::publicDecoderValidatesOptionalMetadataIndependently() {
  const auto valid = ProcessSupervisor::decodeStatusEvent(statusEvent(performance(), "trainingPerformance"));
  QVERIFY(valid.has_value());
  QCOMPARE(valid->trainingPerformance, performance());
  QCOMPARE(valid->iteration.value(), 12);
  QCOMPARE(normalizeTrainingGrowth(growth()), growth());

  auto invalidPerformance = performance();
  invalidPerformance["wall_step_ms"] = true;
  const auto dropped = ProcessSupervisor::decodeStatusEvent(statusEvent(invalidPerformance, "trainingPerformance"));
  QVERIFY(dropped.has_value());
  QCOMPARE(dropped->iteration.value(), 12);
  QCOMPARE(dropped->loss.value(), 0.1);
  QVERIFY(dropped->trainingPerformance.isEmpty());

  for (const QString &key : {QStringLiteral("iteration"), QStringLiteral("torch_allocated_bytes")}) {
    auto invalid = performance();
    invalid[key] = true;
    QVERIFY(normalizeTrainingPerformance(invalid).isEmpty());
  }
  auto invalid = performance();
  invalid["stages_ms"] = QJsonObject{{"surprise_phase", 10.0}};
  QVERIFY(normalizeTrainingPerformance(invalid).isEmpty());
  invalid = performance();
  invalid["wall_step_ms"] = std::numeric_limits<double>::quiet_NaN();
  QVERIFY(normalizeTrainingPerformance(invalid).isEmpty());
  invalid = growth();
  invalid["free_bytes"] = true;
  QVERIFY(normalizeTrainingGrowth(invalid).isEmpty());
  invalid = growth();
  invalid["state"] = "admitted";
  QVERIFY(normalizeTrainingGrowth(invalid).isEmpty());
  invalid = growth();
  invalid["free_bytes"] = 14000000000.0;
  QVERIFY(normalizeTrainingGrowth(invalid).isEmpty());
}

void TrainingTelemetryTests::publicPerformanceUsesCompleteWallTimeNotLegacyCudaEvent() {
  TrainingTelemetry telemetry;
  telemetry.reset(30000);
  WorkerStatus status;
  status.iteration = 12;
  status.loss = 0.1;
  status.iterationMilliseconds = 40.0; // Deliberately different GPU-only event.
  status.trainingPerformance = performance();
  QVERIFY(telemetry.ingest(status));
  QCOMPARE(telemetry.performance(), performance());
  QCOMPARE(telemetry.wallStepMilliseconds().value(), 200.0);
  QCOMPARE(telemetry.iterationMilliseconds().value(), 40.0);
  telemetry.reset();
  QVERIFY(telemetry.performance().isEmpty());
  QVERIFY(!telemetry.wallStepMilliseconds().has_value());
}

void TrainingTelemetryTests::capsHistoryAndReplacesDuplicateIteration() {
  TrainingTelemetry telemetry;
  telemetry.reset(30000);

  for (int iteration = 1; iteration <= 205; ++iteration) {
    WorkerStatus status;
    status.iteration = iteration;
    status.loss = 1.0 / static_cast<double>(iteration);
    status.psnr = 20.0 + static_cast<double>(iteration) / 100.0;
    telemetry.ingest(status);
  }

  QCOMPARE(telemetry.samples().size(), 200);
  QCOMPARE(telemetry.samples().front().iteration, 6);
  QCOMPARE(telemetry.samples().back().iteration, 205);

  WorkerStatus replacement;
  replacement.iteration = 205;
  replacement.loss = 0.001;
  replacement.gaussianCount = 654321;
  QVERIFY(telemetry.ingest(replacement));
  QCOMPARE(telemetry.samples().size(), 200);
  QCOMPARE(telemetry.samples().back().loss.value(), 0.001);
  QCOMPARE(telemetry.gaussianCount().value(), qint64(654321));
  QCOMPARE(telemetry.expectedIterations(), 30000);
}

QTEST_APPLESS_MAIN(TrainingTelemetryTests)

#include "TrainingTelemetryTests.moc"

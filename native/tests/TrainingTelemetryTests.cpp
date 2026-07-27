#include "TrainingTelemetry.h"

#include <QtTest>

using namespace gsw;

class TrainingTelemetryTests final : public QObject {
  Q_OBJECT

private slots:
  void capsHistoryAndReplacesDuplicateIteration();
};

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

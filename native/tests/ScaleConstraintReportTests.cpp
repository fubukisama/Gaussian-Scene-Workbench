#include "SceneCoordinates.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

class ScaleConstraintReportTests final : public QObject {
  Q_OBJECT
private slots:
  void coordinateCsvPreservesConstraintAndIndependentCheckEvidence();
};

void ScaleConstraintReportTests::coordinateCsvPreservesConstraintAndIndependentCheckEvidence() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  gsw::SceneCoordinateInfo info;
  info.valid = true;
  info.globalMinimum = {1., 2., 3.};
  info.globalMaximum = {4., 6., 8.};
  info.metersPerCoordinate = .25;
  info.scaleStatus = QStringLiteral("constraint_derived");
  info.sourceUnit = QStringLiteral("source_units");
  info.dataIdentity = QString(64, QLatin1Char('a'));
  const QJsonObject solution{{"schema", "gsw.scale_solution/1"},
      {"solution_sha256", QString(64, QLatin1Char('b'))},
      {"fit", QJsonObject{{"max_residual_m", 0.},
          {"scope", "scale_and_reference_constraint_agreement"}}},
      {"independent_checks", QJsonObject{{"status", "fail"},
          {"max_residual_m", .032}, {"scope", "selected_reference_observations"}}},
      {"model_geometry_accuracy", "not_measured"}};
  info.provenance = QJsonObject{{"scale", QJsonObject{
      {"derivation", QJsonObject{{"solution", solution},
          {"source_note", "reference, with \"quotes\" and 日本語"}}}}}};
  const QString destination = dir.filePath(QStringLiteral("coordinates.csv"));
  QString error;
  QVERIFY2(gsw::writeSceneCoordinateReport(destination, QStringLiteral("source.ply"),
      info, 0., QStringLiteral("source"), &error), qPrintable(error));
  QFile file(destination);
  QVERIFY(file.open(QIODevice::ReadOnly));
  const QByteArray bytes = file.readAll();
  QByteArray cell;
  const QByteArray prefix("provenance_json,,,,");
  for (const auto &line : bytes.split('\n')) {
    if (line.startsWith(prefix)) cell = line.mid(prefix.size()).trimmed();
  }
  QVERIFY2(!cell.isEmpty(), "CSV must preserve provenance, not just a scale label");
  QVERIFY(cell.endsWith(','));
  cell.chop(1);
  QVERIFY(cell.startsWith('"') && cell.endsWith('"'));
  cell = cell.mid(1, cell.size() - 2);
  cell.replace("\"\"", "\"");
  QJsonParseError parseError;
  const QJsonObject restored = QJsonDocument::fromJson(cell, &parseError).object();
  QCOMPARE(parseError.error, QJsonParseError::NoError);
  QCOMPARE(restored, info.provenance);
  const auto check = restored.value("scale").toObject().value("derivation").toObject()
      .value("solution").toObject().value("independent_checks").toObject();
  QCOMPARE(check.value("status").toString(), QStringLiteral("fail"));
  QCOMPARE(check.value("max_residual_m").toDouble(), .032);
  QVERIFY(bytes.contains("model_geometry_accuracy"));
  QVERIFY(!bytes.contains("model_geometry_accuracy,,,,0"));
}

QTEST_GUILESS_MAIN(ScaleConstraintReportTests)
#include "ScaleConstraintReportTests.moc"

#include "InitializationEditor.h"
#include "AppLanguage.h"
#include "DatasetImportDialog.h"
#include "InitializationPreview.h"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QLineEdit>
#include <QImage>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTranslator>
#include <cstdio>

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication app(argc, argv);
  bool passed = true;
  const auto check = [&](bool ok, const char *text) { std::printf("Initialization editor %s: %s\n", ok ? "PASS" : "FAIL", text); passed &= ok; };
  gsw::InitializationEditor editor(false);
  const QJsonObject declaration{{"kind","random"},{"frame","canonical"},{"count",123},{"seed",qint64(4294967295LL)},
      {"bounds",QJsonArray{QJsonArray{1000000.125,-2.25,3.125},QJsonArray{1000001.125,5.5,7.75}}},
      {"priors",QJsonArray{"declared-origin-note"}},{"provenance",QJsonObject{{"source","generated-fixture"},{"unknown_record",1.25}}},
      {"extra",QJsonObject{{"retain","original"}}}};
  editor.setInitialization(declaration);
  QTemporaryDir rawRoot(QDir(qEnvironmentVariable("GSW_INITIALIZATION_TEST_ROOT", QDir::tempPath())).filePath(QStringLiteral("import-editor-XXXXXX")));
  gsw::DatasetImportDialog rawImport(rawRoot.path(), QStringLiteral("generated import"), {}, rawRoot.path(), true);
  rawImport.setCameraInput({{"adapter","generic_json"},{"sourcePath",rawRoot.filePath(QStringLiteral("cameras.json"))},
      {"posePolicy","locked"},{"initialization",declaration}});
  check(rawImport.request().cameraInput.value("initialization").toObject()==declaration,
      "raw known-camera import retains source/canonical frame and full initialization provenance without claiming visibility was checked");
  check(editor.initialization()==declaration && editor.validationError().isEmpty(), "exact frame, uint32 seed, float64 bounds, priors and unknown provenance round-trip");
  auto *frame=editor.findChild<QComboBox *>(QStringLiteral("initializationFrame"));
  auto *count=editor.findChild<QLineEdit *>(QStringLiteral("initializationCount"));
  auto *seed=editor.findChild<QLineEdit *>(QStringLiteral("initializationSeed"));
  check(frame && count && seed && seed->text()==QStringLiteral("4294967295"), "actual editable controls expose the complete uint32 seed and count");
  if (!frame || !count || !seed) return 1;
  frame->setCurrentIndex(frame->findData(QStringLiteral("source")));
  check(editor.initialization()==declaration && frame->currentData()==QStringLiteral("canonical"), "changing the frame cannot silently relabel existing coordinates");
  const auto probeCatalog = [&](const QString &locale, const char *role, const QString &path) {
    QFile resource(path);
    const bool exists = resource.exists();
    const qint64 size = resource.size();
    const bool opened = resource.open(QIODevice::ReadOnly);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    const bool hashed = opened && digest.addData(&resource);
    const QByteArray sha256 = hashed ? digest.result().toHex() : QByteArray("unavailable");
    QTranslator probe;
    const bool loaded = probe.load(path);
    std::printf("[GSW-LANG-DIAG] locale=%s role=%s resource=%s exists=%d bytes=%lld opened=%d sha256=%s standalone_load=%d current=%s gui_thread=%d\n",
        qPrintable(locale), role, qPrintable(path), int(exists), static_cast<long long>(size),
        int(opened), sha256.constData(), int(loaded), qPrintable(gsw::AppLanguage::current()),
        int(QThread::currentThread() == app.thread()));
    // This uninstalled translator only inspects the compiled resource. It must
    // not change application language, widget state or the original apply path.
    return loaded ? probe.translate("Workbench", "规范坐标（已记录的尺度）") : QString();
  };
  for (const QString &locale: gsw::AppLanguage::supported()) {
    const QString expectedFrameCaption = probeCatalog(locale, "workbench",
        QStringLiteral(":/i18n/workbench_%1.qm").arg(locale));
    if (locale != QStringLiteral("en_US")) {
      const QString qtSuffix = locale == QStringLiteral("zh_CN") ? QStringLiteral("zh_CN") : QStringLiteral("ja");
      probeCatalog(locale, "qtbase", QStringLiteral(":/i18n/qtbase_%1.qm").arg(qtSuffix));
    }
    const bool applied = gsw::AppLanguage::apply(locale,false);
    std::printf("[GSW-LANG-DIAG] locale=%s phase=after-apply applied=%d current=%s gui_thread=%d\n",
        qPrintable(locale), int(applied), qPrintable(gsw::AppLanguage::current()),
        int(QThread::currentThread() == app.thread()));
    check(applied, "actual native catalog switches immediately");
    check(gsw::AppLanguage::current()==locale && !expectedFrameCaption.isEmpty() && frame->currentText()==expectedFrameCaption,
        "actual coordinate-frame caption matches the requested native catalog immediately");
    check(editor.initialization()==declaration && frame->currentData()==QStringLiteral("canonical"), "three-language captions preserve every numeric value and machine frame");
  }
  count->setText(QStringLiteral("3"));
  check(!editor.validationError().isEmpty(), "invalid random count is diagnosed before any helper can run");
  count->setText(QStringLiteral("123")); seed->setText(QStringLiteral("4294967296"));
  check(!editor.validationError().isEmpty(), "seed overflow is refused rather than clamped or wrapped");
  editor.setInitialization(declaration);
  auto *clear=editor.findChild<QPushButton *>(QStringLiteral("initializationClearCoordinates"));
  check(clear!=nullptr, "clearing geometric coordinates is a separate explicit action");
  if(clear)clear->click();
  frame->setCurrentIndex(frame->findData(QStringLiteral("source")));
  check(editor.initialization().value("frame")==QStringLiteral("source") && !editor.initialization().contains("bounds") &&
      editor.initialization().value("seed").toInteger()==4294967295LL && editor.initialization().value("extra")==declaration.value("extra"),
      "explicitly cleared geometry can be re-entered in another frame without losing seed or provenance");
  check(!editor.isVisible() && QApplication::platformName()==QStringLiteral("offscreen"), "no widget is shown and no GSW/OpenGL window is launched");
  gsw::InitializationPreview preview; preview.resize(640,480); preview.setProjection(QStringLiteral("xy"));
  const auto previewReport=[](const double origin,const double principalX) {
    QJsonArray corners;
    for(double x:{-1.,1.})for(double y:{-1.,1.})for(double z:{0.,1.})corners.append(QJsonArray{origin+x,origin+y,origin+z});
    return QJsonObject{{"preview",QJsonObject{{"frame","canonical"},{"display_origin",QJsonArray{origin,origin,origin}},
      {"points",QJsonArray{QJsonArray{origin-.5,origin+.25,origin+.5},QJsonArray{origin+.75,origin-.75,origin+.25}}},
      {"bounds_corners",corners},{"views",QJsonArray{QJsonObject{{"view_id","generated-view"},{"split","train"},{"width",40},{"height",30},
        {"K",QJsonArray{QJsonArray{35.,0.,principalX},QJsonArray{0.,38.,13.},QJsonArray{0.,0.,1.}}},
        {"c2w_opencv",QJsonArray{QJsonArray{1.,0.,0.,origin},QJsonArray{0.,1.,0.,origin-2.},QJsonArray{0.,0.,1.,origin-3.},QJsonArray{0.,0.,0.,1.}}}}}}}}};
  };
  const auto render=[&] {QImage image(preview.size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);preview.render(&image);return image;};
  check(preview.setReport(previewReport(0.,17.)), "CPU preview accepts the explicit full-K camera and actual point-sample presentation fixture");
  const auto nearOrigin=render();
  check(preview.setReport(previewReport(1000000.,17.)) && render()==nearOrigin,
      "million-unit translation with display-origin subtraction leaves the rendered point/camera/bounds relationship unchanged");
  check(preview.setReport(previewReport(0.,5.)) && render()!=nearOrigin,
      "changing principal point changes the drawn frustum rather than collapsing complete K into a centered FOV");
  check(!preview.setReport({}) && !preview.isVisible(), "invalid preview data is refused and no preview window is shown");
  return passed ? 0 : 1;
}

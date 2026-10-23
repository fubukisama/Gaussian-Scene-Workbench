#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <memory>

class QIODevice;

namespace gsw {

struct QualityMipModelContract final {
  bool enabled = false;
  float pixelVariance = .09F;
  bool opacityCompensation = false;
  QString errorCode;
  QString modelSha256;
  QString planHash;
  QJsonObject metadata;

  // Only a validated file snapshot owns this filter. GPU IPC frames have a
  // separate transport contract and must not inherit the scene's file state.
  [[nodiscard]] bool appliesToRasterFrame(bool ipcFrame, bool surfel) const;
  [[nodiscard]] static QualityMipModelContract load(const QString &modelPath,
      bool gaussian, bool surfel, bool rawFilterAttributes, bool requiresSidecar = false);
};

// This exact GLSL is included in NativeViewport and the real offscreen GL test.
[[nodiscard]] QByteArray qualityMipProjectionGlsl();

class QualityMipExportTransaction final {
public:
  QualityMipExportTransaction(const QualityMipModelContract &source, QString destination);
  ~QualityMipExportTransaction();
  QualityMipExportTransaction(const QualityMipExportTransaction &) = delete;
  QualityMipExportTransaction &operator=(const QualityMipExportTransaction &) = delete;
  [[nodiscard]] bool open(QString &error);
  [[nodiscard]] QIODevice &device();
  [[nodiscard]] bool commit(QString &error);
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace gsw

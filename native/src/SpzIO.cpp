#include "SpzIO.h"
#include "PlyPointCloudLoader.h"
#include <load-spz.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gsw {
namespace {
// The upstream codec buffers attributes in RAM; do not pretend it is paged I/O.
constexpr quint64 WorkingBudget = 2ULL * 1024 * 1024 * 1024;
constexpr qint64 FileBudget = 1024LL * 1024 * 1024;
QString invalidSpz() {
  return QCoreApplication::translate("Workbench", "Invalid, truncated or unsupported SPZ data.");
}
QString memoryError() {
  return QCoreApplication::translate("Workbench", "SPZ conversion exceeds the 2 GiB working-memory budget. Use PLY for this model.");
}
bool sameFile(const QString &a, const QString &b) {
  const QFileInfo x(a), y(b);
  return x.absoluteFilePath().compare(y.absoluteFilePath(), Qt::CaseInsensitive) == 0 ||
      (x.exists() && y.exists() && x.canonicalFilePath().compare(y.canonicalFilePath(), Qt::CaseInsensitive) == 0);
}
bool unchanged(const QFileInfo &before) {
  const QFileInfo after(before.absoluteFilePath());
  return after.size() == before.size() && after.lastModified() == before.lastModified();
}
bool writeBytes(QIODevice &out, const char *p, qint64 n, QString &error) {
  if (out.write(p, n) == n) return true;
  error = QCoreApplication::translate("Workbench", "Unable to write model data: %1").arg(out.errorString());
  return false;
}
void appendFloat(QByteArray &out, float v) {
  const auto le = qToLittleEndian(std::bit_cast<quint32>(v));
  out.append(reinterpret_cast<const char *>(&le), 4);
}
// Validate before entering upstream allocation/decompression. Legacy gzip is
// streamed once with a strict exact-size bound, including CRC/end-of-stream.
bool validateSpz(const QByteArray &bytes, const std::function<bool(int)> &cancel,
    ModelExportResult &result) {
  const auto bad = [&] { result.error = invalidSpz(); return false; };
  QByteArray header;
  z_stream stream{};
  const bool gzip = bytes.startsWith(QByteArray::fromHex("1f8b"));
  if (gzip) {
    if (inflateInit2(&stream, 16 | MAX_WBITS) != Z_OK) return bad();
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(bytes.constData()));
    stream.avail_in = static_cast<uInt>(bytes.size());
    header.resize(16);
    stream.next_out = reinterpret_cast<Bytef *>(header.data());
    stream.avail_out = 16;
    const int status = inflate(&stream, Z_NO_FLUSH);
    if ((status != Z_OK && status != Z_STREAM_END) || stream.avail_out) {
      inflateEnd(&stream); return bad();
    }
  } else header = bytes.left(32);
  const auto endInflate = [&] { if (gzip) inflateEnd(&stream); };
  if (header.size() < (gzip ? 16 : 32) || qFromLittleEndian<quint32>(header.constData()) != spz::NGSP_MAGIC) {
    endInflate(); return bad();
  }
  const auto version = qFromLittleEndian<quint32>(header.constData() + 4);
  const quint64 count = qFromLittleEndian<quint32>(header.constData() + 8);
  const auto degree = static_cast<quint8>(header[12]);
  const auto bits = static_cast<quint8>(header[13]);
  const auto flags = static_cast<quint8>(header[14]);
  if ((gzip ? version < 1 || version > 3 : version != 4) || degree > 4 || bits > 24 ||
      !count || count > std::numeric_limits<qint32>::max()) { endInflate(); return bad(); }
  if (flags & ~1U) {
    endInflate();
    result.error = QCoreApplication::translate("Workbench", "This SPZ uses unsupported extensions or flags. Export a standard SPZ without extensions, or use Gaussian PLY; coordinate conventions will not be guessed.");
    return false;
  }
  const quint64 sh = ((degree + 1) * (degree + 1) - 1) * 3;
  const std::array<quint64, 6> sizes{count * (version == 1 ? 6 : 9), count,
      count * 3, count * 3, count * (version < 3 ? 3 : 4), count * sh};
  quint64 payload = 0;
  for (auto n : sizes) payload += n;
  if (quint64(bytes.size()) * (gzip ? 4 : 1) + payload * 3 + count * 64 > WorkingBudget) {
    endInflate(); result.error = memoryError(); return false;
  }
  if (gzip) {
    std::array<Bytef, 65536> buffer;
    quint64 produced = 16;
    int status = Z_OK;
    while (status == Z_OK && produced <= 16 + payload) {
      if (cancel && cancel(5)) { result.cancelled = true; break; }
      stream.next_out = buffer.data(); stream.avail_out = static_cast<uInt>(buffer.size());
      status = inflate(&stream, Z_NO_FLUSH);
      produced += buffer.size() - stream.avail_out;
    }
    const bool valid = status == Z_STREAM_END && produced == 16 + payload && stream.avail_in == 0;
    endInflate();
    if (result.cancelled) return false;
    return valid || bad();
  }
  const int streams = static_cast<quint8>(header[15]);
  if (streams != (degree ? 6 : 5) || qFromLittleEndian<quint32>(header.constData() + 16) != 32 ||
      bytes.size() < 32 + streams * 16) return bad();
  quint64 offset = 32 + streams * 16;
  for (int i = 0; i < streams; ++i) {
    const char *entry = bytes.constData() + 32 + i * 16;
    const auto compressed = qFromLittleEndian<quint64>(entry);
    if (qFromLittleEndian<quint64>(entry + 8) != sizes[i] || compressed > quint64(bytes.size()) - offset) return bad();
    offset += compressed;
  }
  return offset == quint64(bytes.size()) || bad();
}
} // namespace

ModelExportResult exportSpz(const ModelExportOptions &o) {
  ModelExportResult result;
  const auto fail = [&](const QString &e) { result.error = e; return result; };
  const auto cancel = [&](int p) { if (o.progress && o.progress(p)) result.cancelled = true; return result.cancelled; };
  if (sameFile(o.sourcePath, o.destinationPath))
    return fail(QCoreApplication::translate("Workbench", "Choose a new file name; model export cannot overwrite its source."));
  if (o.applyTransform)
    return fail(QCoreApplication::translate("Workbench", "SPZ export currently requires original coordinates; Gaussian transform baking is not supported."));
  if ((o.spzVersion != 3 && o.spzVersion != 4) || o.spzMaximumShDegree < -1 ||
      o.spzMaximumShDegree > 4 || o.spzQuality < 0 || o.spzQuality > 2) return fail(invalidSpz());
  if (cancel(0)) return result;
  const QFileInfo before(o.sourcePath);
  // Cache this metadata now, not lazily after the conversion.
  const auto beforeSize = before.size(); Q_UNUSED(beforeSize);
  const auto beforeTime = before.lastModified(); Q_UNUSED(beforeTime);
  try {
    spz::GaussianCloud cloud;
    int sourceDegree = 0;
    QString error;
    PlyGaussianVisitor visitor;
    visitor.cancelled = [&](int p) { return cancel(p * 60 / 100); };
    visitor.begin = [&](qint64 count, int degree, bool antialiased) {
      if ((!o.deletedVertices.isEmpty() && o.deletedVertices.size() != count) || o.deletedVertices.count(true) >= count) {
        error = QCoreApplication::translate("Workbench", "The edit state no longer matches the source PLY vertex count."); return false;
      }
      const quint64 retained = count - o.deletedVertices.count(true);
      sourceDegree = degree;
      cloud.shDegree = o.spzMaximumShDegree < 0 ? degree : std::min(degree, o.spzMaximumShDegree);
      const size_t sh = ((cloud.shDegree + 1) * (cloud.shDegree + 1) - 1) * 3;
      // Float attributes + packed attributes + parallel compressed chunks + output.
      if (retained > std::numeric_limits<qint32>::max() || retained * (160 + 10 * sh) > WorkingBudget) {
        error = memoryError(); return false;
      }
      cloud.numPoints = static_cast<int>(retained); cloud.antialiased = antialiased;
      cloud.positions.reserve(retained * 3); cloud.scales.reserve(retained * 3);
      cloud.colors.reserve(retained * 3); cloud.rotations.reserve(retained * 4);
      cloud.alphas.reserve(retained); cloud.sh.reserve(retained * sh);
      return true;
    };
    visitor.gaussian = [&](qint64 index, const std::array<double, 86> &v) {
      if (!o.deletedVertices.isEmpty() && o.deletedVertices.testBit(index)) return true;
      const int sourceDim = (sourceDegree + 1) * (sourceDegree + 1) - 1;
      for (int j = 0; j < 14 + sourceDim * 3; ++j) if (!std::isfinite(v[j]) || std::abs(v[j]) > std::numeric_limits<float>::max()) {
        error = QCoreApplication::translate("Workbench", "Gaussian attributes must be finite; SPZ export was not written."); return false;
      }
      for (int j = 0; j < 3; ++j) {
        // Conversion RDF -> RUB flips y/z; use a symmetric conservative bound.
        if (std::abs(v[j]) > 2047.999755859375) {
          error = QCoreApplication::translate("Workbench", "SPZ positions must stay within ±2048 source units. Use PLY for large coordinates; no automatic shift or rescaling was applied."); return false;
        }
        cloud.positions.push_back(static_cast<float>(v[j]));
        cloud.colors.push_back(static_cast<float>(v[3+j]));
        cloud.scales.push_back(static_cast<float>(v[7+j]));
      }
      double norm = 0;
      for (int j = 10; j < 14; ++j) norm += v[j] * v[j];
      if (!std::isfinite(norm) || norm < 1e-20) {
        error = QCoreApplication::translate("Workbench", "Gaussian rotation contains an invalid quaternion; SPZ export was not written."); return false;
      }
      norm = std::sqrt(norm);
      for (const int j : {11, 12, 13, 10}) cloud.rotations.push_back(static_cast<float>(v[j] / norm));
      cloud.alphas.push_back(static_cast<float>(v[6]));
      const int targetDim = (cloud.shDegree + 1) * (cloud.shDegree + 1) - 1;
      for (int k = 0; k < targetDim; ++k)
        for (int c = 0; c < 3; ++c) cloud.sh.push_back(static_cast<float>(v[14 + c * sourceDim + k]));
      return true;
    };
    if (!PlyPointCloudLoader::visitSourceGaussians(o.sourcePath, visitor, error)) {
      if (!result.cancelled) result.error = error;
      return result;
    }
    if (cancel(65)) return result;
    spz::PackOptions pack;
    pack.from = spz::CoordinateSystem::RDF; pack.version = o.spzVersion;
    pack.sh1Bits = o.spzQuality == 0 ? 4 : o.spzQuality == 1 ? 5 : 8;
    pack.shRestBits = o.spzQuality == 0 ? 3 : o.spzQuality == 1 ? 4 : 8;
    std::vector<uint8_t> encoded;
    if (!spz::saveSpz(cloud, pack, &encoded)) return fail(invalidSpz());
    if (cancel(90)) return result;
    if (!unchanged(before)) return fail(QCoreApplication::translate("Workbench", "The source model changed during conversion. Please retry."));
    QSaveFile output(o.destinationPath);
    if (!output.open(QIODevice::WriteOnly)) return fail(QCoreApplication::translate("Workbench", "Unable to create export files in the selected directory."));
    for (size_t offset = 0; offset < encoded.size();) {
      if (cancel(90 + static_cast<int>(9 * offset / std::max<size_t>(1, encoded.size())))) return result;
      const auto n = std::min<size_t>(1024 * 1024, encoded.size() - offset);
      if (!writeBytes(output, reinterpret_cast<const char *>(encoded.data() + offset), n, result.error)) return result;
      offset += n;
    }
    if (cancel(99)) return result;
    if (!unchanged(before)) return fail(QCoreApplication::translate("Workbench", "The source model changed during conversion. Please retry."));
    result.success = output.commit();
    if (!result.success) result.error = QCoreApplication::translate("Workbench", "Unable to write model data: %1").arg(output.errorString());
  } catch (const std::bad_alloc &) { result.error = memoryError(); }
    catch (const std::exception &) { result.error = invalidSpz(); }
  return result;
}

ModelExportResult importSpzToPly(const QString &source, const QString &destination,
    const std::function<bool(int)> &cancelled) {
  ModelExportResult result;
  const auto cancel = [&](int p) { if (cancelled && cancelled(p)) result.cancelled = true; return result.cancelled; };
  if (sameFile(source, destination)) {
    result.error = QCoreApplication::translate("Workbench", "Choose a new file name; model export cannot overwrite its source."); return result;
  }
  if (cancel(0)) return result;
  try {
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly) || input.size() < 16) { result.error = invalidSpz(); return result; }
    if (input.size() > FileBudget) { result.error = memoryError(); return result; }
    const QByteArray bytes = input.readAll();
    if (bytes.size() != input.size()) { result.error = invalidSpz(); return result; }
    if (!validateSpz(bytes, cancelled, result) || cancel(10)) return result;
    const auto packed = spz::loadSpzPacked(reinterpret_cast<const uint8_t *>(bytes.constData()), bytes.size());
    if (packed.numPoints <= 0 || packed.hadSkippedExtensions) { result.error = invalidSpz(); return result; }
    if (cancel(40)) return result;
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) {
      result.error = QCoreApplication::translate("Workbench", "Unable to create export files in the selected directory."); return result;
    }
    const int dim = (packed.shDegree + 1) * (packed.shDegree + 1) - 1;
    QByteArray header = "ply\nformat binary_little_endian 1.0\ncomment gsw_spz_antialiased " + QByteArray::number(packed.antialiased ? 1 : 0) +
        "\ncomment SPZ decoded to PLY (RDF coordinates; quantized source)\nelement vertex " + QByteArray::number(packed.numPoints) + '\n';
    for (const char *name : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
      header += QByteArray("property float ") + name + '\n';
    for (int i = 0; i < dim * 3; ++i) header += "property float f_rest_" + QByteArray::number(i) + '\n';
    header += "end_header\n";
    if (!writeBytes(output, header.constData(), header.size(), result.error)) return result;
    const auto converter = spz::coordinateConverter(spz::CoordinateSystem::RUB, spz::CoordinateSystem::RDF, packed.shDegree);
    QByteArray buffer; buffer.reserve(1024 * 1024 + 344);
    for (int i = 0; i < packed.numPoints; ++i) {
      if ((i & 4095) == 0 && cancel(40 + static_cast<int>(59LL * i / packed.numPoints))) return result;
      const auto g = packed.unpack(i, converter);
      for (float f : g.position) if (!std::isfinite(f)) { result.error = invalidSpz(); return result; }
      for (float f : g.position) appendFloat(buffer, f);
      for (float f : g.color) appendFloat(buffer, f);
      // Quantized alpha endpoints decode to +/-infinity in upstream logit
      // space. Use finite saturated logits so the working PLY can be re-exported.
      appendFloat(buffer, std::isfinite(g.alpha) ? g.alpha : (g.alpha > 0 ? 20.0F : -20.0F));
      for (float f : g.scale) appendFloat(buffer, f);
      for (const int j : {3, 0, 1, 2}) appendFloat(buffer, g.rotation[j]);
      for (const auto *sh : {&g.shR, &g.shG, &g.shB}) for (int k = 0; k < dim; ++k) appendFloat(buffer, (*sh)[k]);
      if (buffer.size() >= 1024 * 1024) {
        if (!writeBytes(output, buffer.constData(), buffer.size(), result.error)) return result;
        buffer.clear();
      }
    }
    if (!writeBytes(output, buffer.constData(), buffer.size(), result.error) || cancel(99)) return result;
    result.success = output.commit();
    if (!result.success) result.error = QCoreApplication::translate("Workbench", "Unable to write model data: %1").arg(output.errorString());
  } catch (const std::bad_alloc &) { result.error = memoryError(); }
    catch (const std::exception &) { result.error = invalidSpz(); }
  return result;
}
} // namespace gsw

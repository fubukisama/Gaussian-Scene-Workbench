#include "PointCloudCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gsw {
namespace {

constexpr int kInternalNodeCount = 1 + 8 + 64;
constexpr int kLeafCount = 512;
constexpr int kNodeCount = kInternalNodeCount + kLeafCount;
constexpr qsizetype kLodPointLimit = 32'768;
constexpr qsizetype kBucketBufferPoints = 8'192;
constexpr std::array<int, 4> kDepthOffsets = {0, 1, 9, 73};
constexpr qint64 kStaleBuildMilliseconds = 24LL * 60LL * 60LL * 1000LL;
constexpr qint64 kObsoleteDataMilliseconds =
    7LL * 24LL * 60LL * 60LL * 1000LL;

QString normalizedSourcePath(const QString &sourcePath) {
  return QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath());
}

QByteArray sourceKey(const QString &sourcePath) {
  return QCryptographicHash::hash(
             normalizedSourcePath(sourcePath).toCaseFolded().toUtf8(),
             QCryptographicHash::Sha256)
      .toHex()
      .left(20);
}

QString cacheRootForSource(const QString &sourcePath) {
  return QFileInfo(normalizedSourcePath(sourcePath))
      .dir()
      .filePath(QStringLiteral(".gsw-cache"));
}

QJsonArray vectorToJson(const QVector3D &value) {
  return {value.x(), value.y(), value.z()};
}

bool vectorFromJson(const QJsonValue &value, QVector3D &result) {
  const QJsonArray values = value.toArray();
  if (values.size() != 3) {
    return false;
  }
  result = QVector3D(static_cast<float>(values.at(0).toDouble()),
                     static_cast<float>(values.at(1).toDouble()),
                     static_cast<float>(values.at(2).toDouble()));
  return std::isfinite(result.x()) && std::isfinite(result.y()) &&
         std::isfinite(result.z());
}

quint64 splitMix64(quint64 value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

int nodeIndex(const int depth, const int prefix) {
  return kDepthOffsets.at(static_cast<std::size_t>(depth)) + prefix;
}

int octreeLeafCode(const PointPreviewVertex &vertex,
                   const QVector3D &minimum, const QVector3D &maximum) {
  const QVector3D extent = maximum - minimum;
  const auto coordinate = [](const float value, const float origin,
                             const float size) {
    if (!std::isfinite(size) || size <= 1.0e-20F) {
      return 0;
    }
    const float normalized = std::clamp((value - origin) / size, 0.0F,
                                        std::nextafter(1.0F, 0.0F));
    return std::clamp(static_cast<int>(normalized * 8.0F), 0, 7);
  };
  const int x = coordinate(vertex.x, minimum.x(), extent.x());
  const int y = coordinate(vertex.y, minimum.y(), extent.y());
  const int z = coordinate(vertex.z, minimum.z(), extent.z());
  int code = 0;
  for (int bit = 2; bit >= 0; --bit) {
    const int child = ((x >> bit) & 1) | (((y >> bit) & 1) << 1) |
                      (((z >> bit) & 1) << 2);
    code = (code << 3) | child;
  }
  return code;
}

void expandBounds(PointCloudCacheNode &node, const PointPreviewVertex &vertex) {
  const QVector3D position(vertex.x, vertex.y, vertex.z);
  if (node.sourcePointCount == 0) {
    node.boundsMinimum = position;
    node.boundsMaximum = position;
  } else {
    node.boundsMinimum.setX(std::min(node.boundsMinimum.x(), position.x()));
    node.boundsMinimum.setY(std::min(node.boundsMinimum.y(), position.y()));
    node.boundsMinimum.setZ(std::min(node.boundsMinimum.z(), position.z()));
    node.boundsMaximum.setX(std::max(node.boundsMaximum.x(), position.x()));
    node.boundsMaximum.setY(std::max(node.boundsMaximum.y(), position.y()));
    node.boundsMaximum.setZ(std::max(node.boundsMaximum.z(), position.z()));
  }
  ++node.sourcePointCount;
}

bool writeAll(QIODevice &device, const char *data, qint64 byteCount,
              QString &error) {
  while (byteCount > 0) {
    const qint64 written = device.write(data, byteCount);
    if (written <= 0) {
      error = QStringLiteral("Unable to write the point-cache payload: %1")
                  .arg(device.errorString());
      return false;
    }
    data += written;
    byteCount -= written;
  }
  return true;
}

bool copyFileInto(QFile &source, QIODevice &destination, QString &error) {
  constexpr qint64 kCopyBlockBytes = 8LL * 1024LL * 1024LL;
  while (!source.atEnd()) {
    const QByteArray bytes = source.read(kCopyBlockBytes);
    if (bytes.isEmpty() && source.error() != QFileDevice::NoError) {
      error = QStringLiteral("Unable to read a point-cache bucket: %1")
                  .arg(source.errorString());
      return false;
    }
    if (!writeAll(destination, bytes.constData(), bytes.size(), error)) {
      return false;
    }
  }
  return true;
}

} // namespace

bool PointCloudCacheIndex::isValid() const {
  if (formatVersion != CurrentFormatVersion || sourcePath.isEmpty() ||
      dataPath.isEmpty() || fullPointCount <= 0 || nodes.isEmpty() ||
      nodes.size() != kNodeCount || rootNode < 0 ||
      rootNode >= nodes.size()) {
    return false;
  }
  const QFileInfo data(dataPath);
  if (!data.isFile()) {
    return false;
  }
  const qint64 dataSize = data.size();
  const auto finiteBounds = [](const QVector3D &minimum,
                               const QVector3D &maximum) {
    return std::isfinite(minimum.x()) && std::isfinite(minimum.y()) &&
           std::isfinite(minimum.z()) && std::isfinite(maximum.x()) &&
           std::isfinite(maximum.y()) && std::isfinite(maximum.z()) &&
           minimum.x() <= maximum.x() && minimum.y() <= maximum.y() &&
           minimum.z() <= maximum.z();
  };
  if (!finiteBounds(boundsMinimum, boundsMaximum) ||
      nodes.at(rootNode).parent != -1 ||
      nodes.at(rootNode).sourcePointCount != fullPointCount) {
    return false;
  }
  for (qsizetype index = 0; index < nodes.size(); ++index) {
    const PointCloudCacheNode &node = nodes.at(index);
    if (node.id != static_cast<int>(index) || node.depth < 0 ||
        node.depth > OctreeDepth ||
        node.sourcePointCount < 0 || node.pointCount < 0 ||
        node.pointCount > node.sourcePointCount ||
        (node.depth == OctreeDepth) != node.children.isEmpty()) {
      return false;
    }
    if (node.id == rootNode) {
      if (node.depth != 0) {
        return false;
      }
    } else if (node.parent < 0 ||
               static_cast<qsizetype>(node.parent) >= nodes.size() ||
               nodes.at(node.parent).depth + 1 != node.depth ||
               !nodes.at(node.parent).children.contains(node.id)) {
      return false;
    }
    if (node.depth < OctreeDepth && node.children.size() != 8) {
      return false;
    }
    for (const int child : node.children) {
      if (child < 0 || static_cast<qsizetype>(child) >= nodes.size() ||
          nodes.at(child).parent != node.id) {
        return false;
      }
    }
    if (node.sourcePointCount == 0) {
      if (node.pointCount != 0) {
        return false;
      }
      continue;
    }
    if (node.pointCount <= 0 || !finiteBounds(node.boundsMinimum,
                                              node.boundsMaximum) ||
        (node.isLeaf() && node.pointCount != node.sourcePointCount) ||
        node.dataOffset < 0 || node.dataOffset > dataSize ||
        node.pointCount >
            (std::numeric_limits<qint64>::max() /
             static_cast<qint64>(sizeof(PointPreviewVertex)))) {
      return false;
    }
    const qint64 byteCount =
        node.pointCount * static_cast<qint64>(sizeof(PointPreviewVertex));
    if (byteCount > dataSize - node.dataOffset) {
      return false;
    }
  }
  return true;
}

QString PointCloudCache::indexPathForSource(const QString &sourcePath) {
  return QDir(cacheRootForSource(sourcePath))
      .filePath(QStringLiteral("%1.json")
                    .arg(QString::fromLatin1(sourceKey(sourcePath))));
}

PointCloudCacheIndex PointCloudCache::loadForSource(
    const QString &sourcePath, QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  PointCloudCacheIndex result;
  const QFileInfo source(normalizedSourcePath(sourcePath));
  const QString indexPath = indexPathForSource(sourcePath);
  QFile indexFile(indexPath);
  if (!source.isFile() || !indexFile.open(QIODevice::ReadOnly)) {
    return result;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(indexFile.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("The point-cache index is malformed.");
    }
    return {};
  }
  const QJsonObject root = document.object();
  result.formatVersion = root.value(QStringLiteral("formatVersion")).toInt();
  result.indexPath = indexPath;
  result.sourcePath = normalizedSourcePath(
      root.value(QStringLiteral("sourcePath")).toString());
  result.sourceSize =
      root.value(QStringLiteral("sourceSize")).toVariant().toLongLong();
  result.sourceModifiedMilliseconds =
      root.value(QStringLiteral("sourceModifiedMilliseconds"))
          .toVariant()
          .toLongLong();
  result.fullPointCount =
      root.value(QStringLiteral("fullPointCount")).toVariant().toLongLong();
  result.rootNode = root.value(QStringLiteral("rootNode")).toInt();
  result.dataPath = QDir(QFileInfo(indexPath).dir()).filePath(
      root.value(QStringLiteral("dataFile")).toString());
  if (!vectorFromJson(root.value(QStringLiteral("boundsMinimum")),
                      result.boundsMinimum) ||
      !vectorFromJson(root.value(QStringLiteral("boundsMaximum")),
                      result.boundsMaximum)) {
    return {};
  }
  const QJsonArray nodes = root.value(QStringLiteral("nodes")).toArray();
  result.nodes.reserve(nodes.size());
  for (const QJsonValue &nodeValue : nodes) {
    const QJsonObject object = nodeValue.toObject();
    PointCloudCacheNode node;
    node.id = object.value(QStringLiteral("id")).toInt(-1);
    node.depth = object.value(QStringLiteral("depth")).toInt();
    node.parent = object.value(QStringLiteral("parent")).toInt(-1);
    node.dataOffset =
        object.value(QStringLiteral("dataOffset")).toVariant().toLongLong();
    node.pointCount =
        object.value(QStringLiteral("pointCount")).toVariant().toLongLong();
    node.sourcePointCount =
        object.value(QStringLiteral("sourcePointCount")).toVariant().toLongLong();
    for (const QJsonValue &child :
         object.value(QStringLiteral("children")).toArray()) {
      node.children.append(child.toInt(-1));
    }
    if (!vectorFromJson(object.value(QStringLiteral("boundsMinimum")),
                        node.boundsMinimum) ||
        !vectorFromJson(object.value(QStringLiteral("boundsMaximum")),
                        node.boundsMaximum)) {
      return {};
    }
    result.nodes.append(node);
  }
  if (result.sourcePath.compare(normalizedSourcePath(sourcePath),
                                Qt::CaseInsensitive) != 0 ||
      result.sourceSize != source.size() ||
      result.sourceModifiedMilliseconds !=
          source.lastModified().toMSecsSinceEpoch() ||
      !result.isValid()) {
    return {};
  }
  return result;
}

PointCloudCachePage PointCloudCache::readNode(
    const PointCloudCacheIndex &index, const int nodeId) {
  PointCloudCachePage result;
  result.nodeId = nodeId;
  if (!index.isValid() || nodeId < 0 || nodeId >= index.nodes.size()) {
    result.error = QStringLiteral("Invalid point-cache node request.");
    return result;
  }
  const PointCloudCacheNode &node = index.nodes.at(nodeId);
  if (node.pointCount <= 0 ||
      node.pointCount > std::numeric_limits<qsizetype>::max()) {
    result.error = QStringLiteral("The point-cache node is empty or too large.");
    return result;
  }
  QFile data(index.dataPath);
  if (!data.open(QIODevice::ReadOnly) || !data.seek(node.dataOffset)) {
    result.error = QStringLiteral("Unable to open point-cache data: %1")
                       .arg(data.errorString());
    return result;
  }
  result.vertices.resize(static_cast<qsizetype>(node.pointCount));
  const qint64 byteCount =
      node.pointCount * static_cast<qint64>(sizeof(PointPreviewVertex));
  const qint64 read = data.read(
      reinterpret_cast<char *>(result.vertices.data()), byteCount);
  if (read != byteCount) {
    result.vertices.clear();
    result.error = QStringLiteral("The point-cache node is truncated.");
  }
  return result;
}

struct PointCloudCacheBuilder::Impl {
  QString sourcePath;
  QVector3D boundsMinimum;
  QVector3D boundsMaximum;
  QString cacheRoot;
  QString buildDirectory;
  QString dataPath;
  QVector<PointCloudCacheNode> nodes;
  QVector<QVector<PointPreviewVertex>> reservoirs;
  QVector<QVector<PointPreviewVertex>> bucketBuffers;
  QVector<QString> bucketPaths;
  bool started = false;
  bool finished = false;

  bool flushBucket(int leafCode, QString &error) {
    QVector<PointPreviewVertex> &buffer = bucketBuffers[leafCode];
    if (buffer.isEmpty()) {
      return true;
    }
    QFile bucket(bucketPaths.at(leafCode));
    if (!bucket.open(QIODevice::WriteOnly | QIODevice::Append)) {
      error = QStringLiteral("Unable to write point-cache bucket: %1")
                  .arg(bucket.errorString());
      return false;
    }
    const qint64 bytes =
        buffer.size() * static_cast<qint64>(sizeof(PointPreviewVertex));
    if (!writeAll(bucket, reinterpret_cast<const char *>(buffer.constData()),
                  bytes, error)) {
      return false;
    }
    buffer.clear();
    return true;
  }

  void cleanupBuildDirectory() {
    if (!buildDirectory.isEmpty() && QFileInfo(buildDirectory).isDir() &&
        QFileInfo(buildDirectory).dir().absolutePath().compare(
            QFileInfo(cacheRoot).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
      QDir(buildDirectory).removeRecursively();
    }
  }
};

PointCloudCacheBuilder::PointCloudCacheBuilder(
    const QString &sourcePath, const QVector3D &boundsMinimum,
    const QVector3D &boundsMaximum)
    : mImpl(std::make_unique<Impl>()) {
  mImpl->sourcePath = normalizedSourcePath(sourcePath);
  mImpl->boundsMinimum = boundsMinimum;
  mImpl->boundsMaximum = boundsMaximum;
  mImpl->cacheRoot = cacheRootForSource(sourcePath);
}

PointCloudCacheBuilder::~PointCloudCacheBuilder() {
  if (mImpl != nullptr && !mImpl->finished) {
    mImpl->cleanupBuildDirectory();
  }
}

bool PointCloudCacheBuilder::begin(QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  QString error;
  if (mImpl->started || mImpl->finished) {
    error = QStringLiteral("The point-cache builder has already been used.");
  }
  const QFileInfo source(mImpl->sourcePath);
  QDir cacheRoot(mImpl->cacheRoot);
  if (error.isEmpty() && !source.isFile()) {
    error = QStringLiteral("The source point cloud no longer exists.");
  }
  if (error.isEmpty() && !cacheRoot.mkpath(QStringLiteral("."))) {
    error = QStringLiteral("Unable to create the point-cache directory %1.")
                .arg(mImpl->cacheRoot);
  }
  const QString key = QString::fromLatin1(sourceKey(mImpl->sourcePath));
  if (error.isEmpty()) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    cacheRoot.setNameFilters({QStringLiteral("%1-build-*").arg(key)});
    for (const QFileInfo &entry :
         cacheRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      if (entry.lastModified().toMSecsSinceEpoch() +
              kStaleBuildMilliseconds <
          now) {
        QDir(entry.absoluteFilePath()).removeRecursively();
      }
    }
  }
  mImpl->buildDirectory = cacheRoot.filePath(
      QStringLiteral("%1-build-%2")
          .arg(key, QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (error.isEmpty() && !QDir().mkpath(mImpl->buildDirectory)) {
    error = QStringLiteral("Unable to create temporary point-cache storage.");
  }
  if (!error.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = error;
    }
    return false;
  }

  mImpl->nodes.resize(kNodeCount);
  mImpl->reservoirs.resize(kInternalNodeCount);
  mImpl->bucketBuffers.resize(kLeafCount);
  mImpl->bucketPaths.resize(kLeafCount);
  for (int depth = 0; depth <= PointCloudCacheIndex::OctreeDepth; ++depth) {
    const int count = 1 << (depth * 3);
    for (int prefix = 0; prefix < count; ++prefix) {
      const int id = nodeIndex(depth, prefix);
      PointCloudCacheNode &node = mImpl->nodes[id];
      node.id = id;
      node.depth = depth;
      node.parent = depth == 0 ? -1 : nodeIndex(depth - 1, prefix >> 3);
      if (depth < PointCloudCacheIndex::OctreeDepth) {
        for (int child = 0; child < 8; ++child) {
          node.children.append(nodeIndex(depth + 1, prefix * 8 + child));
        }
      }
    }
  }
  for (int leaf = 0; leaf < kLeafCount; ++leaf) {
    mImpl->bucketPaths[leaf] =
        QDir(mImpl->buildDirectory)
            .filePath(QStringLiteral("leaf-%1.bin").arg(leaf, 3, 10,
                                                        QLatin1Char('0')));
  }
  mImpl->started = true;
  return true;
}

bool PointCloudCacheBuilder::append(const PointPreviewVertex &vertex,
                                    const qint64 sourceIndex,
                                    QString *errorMessage) {
  if (!mImpl->started || mImpl->finished) {
    if (errorMessage != nullptr) {
      *errorMessage = QStringLiteral("The point-cache builder is not active.");
    }
    return false;
  }
  const int leafCode = octreeLeafCode(
      vertex, mImpl->boundsMinimum, mImpl->boundsMaximum);
  int prefix = 0;
  for (int depth = 0; depth <= PointCloudCacheIndex::OctreeDepth; ++depth) {
    if (depth > 0) {
      const int shift =
          (PointCloudCacheIndex::OctreeDepth - depth) * 3;
      prefix = leafCode >> shift;
    }
    const int id = nodeIndex(depth, prefix);
    PointCloudCacheNode &node = mImpl->nodes[id];
    const qint64 previousCount = node.sourcePointCount;
    expandBounds(node, vertex);
    if (depth < PointCloudCacheIndex::OctreeDepth) {
      QVector<PointPreviewVertex> &reservoir = mImpl->reservoirs[id];
      if (reservoir.size() < kLodPointLimit) {
        reservoir.append(vertex);
      } else {
        const quint64 hash = splitMix64(
            static_cast<quint64>(sourceIndex) ^
            (static_cast<quint64>(id) << 48U));
        const quint64 replacement =
            hash % static_cast<quint64>(previousCount + 1);
        if (replacement < static_cast<quint64>(kLodPointLimit)) {
          reservoir[static_cast<qsizetype>(replacement)] = vertex;
        }
      }
    }
  }
  QVector<PointPreviewVertex> &buffer = mImpl->bucketBuffers[leafCode];
  if (buffer.isEmpty()) {
    buffer.reserve(kBucketBufferPoints);
  }
  buffer.append(vertex);
  if (buffer.size() >= kBucketBufferPoints) {
    QString error;
    if (!mImpl->flushBucket(leafCode, error)) {
      if (errorMessage != nullptr) {
        *errorMessage = error;
      }
      return false;
    }
  }
  return true;
}

PointCloudCacheIndex PointCloudCacheBuilder::finish(QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  PointCloudCacheIndex result;
  QString error;
  if (!mImpl->started || mImpl->finished) {
    error = QStringLiteral("The point-cache builder is not active.");
  }
  for (int leaf = 0; error.isEmpty() && leaf < kLeafCount; ++leaf) {
    if (!mImpl->flushBucket(leaf, error)) {
      break;
    }
  }
  const QFileInfo source(mImpl->sourcePath);
  const QString key = QString::fromLatin1(sourceKey(mImpl->sourcePath));
  const QString dataFileName =
      QStringLiteral("%1-v%2-%3-%4-%5.bin")
          .arg(key)
          .arg(PointCloudCacheIndex::CurrentFormatVersion)
          .arg(source.size())
          .arg(source.lastModified().toMSecsSinceEpoch())
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  const QString dataPath = QDir(mImpl->cacheRoot).filePath(dataFileName);
  QSaveFile data(dataPath);
  data.setDirectWriteFallback(false);
  if (error.isEmpty() && !data.open(QIODevice::WriteOnly)) {
    error = QStringLiteral("Unable to create point-cache data: %1")
                .arg(data.errorString());
  }
  for (PointCloudCacheNode &node : mImpl->nodes) {
    if (!error.isEmpty() || node.sourcePointCount <= 0) {
      continue;
    }
    node.dataOffset = data.pos();
    if (node.depth < PointCloudCacheIndex::OctreeDepth) {
      const QVector<PointPreviewVertex> &points = mImpl->reservoirs.at(node.id);
      node.pointCount = points.size();
      const qint64 byteCount =
          points.size() * static_cast<qint64>(sizeof(PointPreviewVertex));
      writeAll(data, reinterpret_cast<const char *>(points.constData()),
               byteCount, error);
    } else {
      const int leaf = node.id - kInternalNodeCount;
      QFile bucket(mImpl->bucketPaths.at(leaf));
      if (!bucket.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Unable to read a completed point-cache bucket.");
        continue;
      }
      node.pointCount =
          bucket.size() / static_cast<qint64>(sizeof(PointPreviewVertex));
      copyFileInto(bucket, data, error);
    }
  }
  if (error.isEmpty() && !data.commit()) {
    error = QStringLiteral("Unable to publish point-cache data: %1")
                .arg(data.errorString());
  }
  if (!error.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = error;
    }
    return {};
  }

  result.formatVersion = PointCloudCacheIndex::CurrentFormatVersion;
  result.indexPath = PointCloudCache::indexPathForSource(mImpl->sourcePath);
  result.dataPath = dataPath;
  result.sourcePath = mImpl->sourcePath;
  result.sourceSize = source.size();
  result.sourceModifiedMilliseconds = source.lastModified().toMSecsSinceEpoch();
  result.boundsMinimum = mImpl->boundsMinimum;
  result.boundsMaximum = mImpl->boundsMaximum;
  result.nodes = mImpl->nodes;
  result.rootNode = 0;
  result.fullPointCount = result.nodes.constFirst().sourcePointCount;

  QJsonObject root;
  root.insert(QStringLiteral("formatVersion"), result.formatVersion);
  root.insert(QStringLiteral("sourcePath"), result.sourcePath);
  root.insert(QStringLiteral("sourceSize"), result.sourceSize);
  root.insert(QStringLiteral("sourceModifiedMilliseconds"),
              result.sourceModifiedMilliseconds);
  root.insert(QStringLiteral("fullPointCount"), result.fullPointCount);
  root.insert(QStringLiteral("rootNode"), result.rootNode);
  root.insert(QStringLiteral("dataFile"), QFileInfo(dataPath).fileName());
  root.insert(QStringLiteral("boundsMinimum"),
              vectorToJson(result.boundsMinimum));
  root.insert(QStringLiteral("boundsMaximum"),
              vectorToJson(result.boundsMaximum));
  QJsonArray nodes;
  for (const PointCloudCacheNode &node : std::as_const(result.nodes)) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), node.id);
    object.insert(QStringLiteral("depth"), node.depth);
    object.insert(QStringLiteral("parent"), node.parent);
    object.insert(QStringLiteral("dataOffset"), node.dataOffset);
    object.insert(QStringLiteral("pointCount"), node.pointCount);
    object.insert(QStringLiteral("sourcePointCount"), node.sourcePointCount);
    object.insert(QStringLiteral("boundsMinimum"),
                  vectorToJson(node.boundsMinimum));
    object.insert(QStringLiteral("boundsMaximum"),
                  vectorToJson(node.boundsMaximum));
    QJsonArray children;
    for (const int child : node.children) {
      children.append(child);
    }
    object.insert(QStringLiteral("children"), children);
    nodes.append(object);
  }
  root.insert(QStringLiteral("nodes"), nodes);
  QSaveFile indexFile(result.indexPath);
  indexFile.setDirectWriteFallback(false);
  if (!indexFile.open(QIODevice::WriteOnly) ||
      indexFile.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0 ||
      !indexFile.commit()) {
    QFile::remove(dataPath);
    if (errorMessage != nullptr) {
      *errorMessage =
          QStringLiteral("Unable to publish the point-cache index: %1")
              .arg(indexFile.errorString());
    }
    return {};
  }
  mImpl->finished = true;
  mImpl->cleanupBuildDirectory();
  QDir cacheRoot(mImpl->cacheRoot);
  cacheRoot.setNameFilters({QStringLiteral("%1-v*.bin").arg(key)});
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (const QFileInfo &entry : cacheRoot.entryInfoList(QDir::Files)) {
    if (entry.fileName() != dataFileName &&
        entry.lastModified().toMSecsSinceEpoch() +
                kObsoleteDataMilliseconds <
            now) {
      QFile::remove(entry.absoluteFilePath());
    }
  }
  return result;
}

} // namespace gsw

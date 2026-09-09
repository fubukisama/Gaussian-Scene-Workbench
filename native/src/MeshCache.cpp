#include <QCoreApplication>
#include "MeshCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace gsw {
namespace {

constexpr std::array<int, 5> kDepthOffsets = {0, 1, 9, 73, 585};
constexpr int kSpatialLeafCount = 4096;
constexpr int kSpatialNodeCount = 4681;
constexpr qsizetype kVertexWriteBufferSize = 262'144;
constexpr qsizetype kTriangleBucketBufferSize = 512;
constexpr qsizetype kExactPageTriangleLimit = 65'536;
constexpr qint64 kStaleBuildMilliseconds =
    24LL * 60LL * 60LL * 1000LL;
constexpr qint64 kObsoleteDataMilliseconds =
    7LL * 24LL * 60LL * 60LL * 1000LL;

struct PackedSourceVertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float red = 0.72F;
  float green = 0.75F;
  float blue = 0.78F;
};

struct PackedNormalSum {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

struct TriangleRecord {
  quint32 a = 0;
  quint32 b = 0;
  quint32 c = 0;
  std::array<float, 3> textureU = {};
  std::array<float, 3> textureV = {};
  quint32 textured = 0;
};

struct MeshCornerKey {
  quint32 sourceIndex = 0;
  quint32 textureU = 0;
  quint32 textureV = 0;
  quint32 textured = 0;

  bool operator==(const MeshCornerKey &) const = default;
};

size_t qHash(const MeshCornerKey &key, const size_t seed = 0) noexcept {
  return qHashMulti(seed, key.sourceIndex, key.textureU, key.textureV,
                    key.textured);
}

static_assert(sizeof(PackedSourceVertex) == 24);
static_assert(sizeof(PackedNormalSum) == 12);
static_assert(sizeof(TriangleRecord) == 40);

float canonicalTextureCoordinate(const float value) {
  return value == 0.0F ? 0.0F : value;
}

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

qsizetype reservoirLimit(const int depth) {
  constexpr std::array<qsizetype, 5> limits = {32'768, 8'192, 4'096,
                                               2'048, 512};
  return limits.at(static_cast<std::size_t>(std::clamp(depth, 0, 4)));
}

bool finitePosition(const PackedSourceVertex &vertex) {
  return std::isfinite(vertex.x) && std::isfinite(vertex.y) &&
         std::isfinite(vertex.z);
}

int octreeLeafCode(const QVector3D &position, const QVector3D &minimum,
                   const QVector3D &maximum) {
  const QVector3D extent = maximum - minimum;
  const auto coordinate = [](const float value, const float origin,
                             const float size) {
    if (!std::isfinite(size) || size <= 1.0e-20F) {
      return 0;
    }
    const float normalized = std::clamp((value - origin) / size, 0.0F,
                                        std::nextafter(1.0F, 0.0F));
    return std::clamp(static_cast<int>(normalized * 16.0F), 0, 15);
  };
  const int x = coordinate(position.x(), minimum.x(), extent.x());
  const int y = coordinate(position.y(), minimum.y(), extent.y());
  const int z = coordinate(position.z(), minimum.z(), extent.z());
  int code = 0;
  for (int bit = 3; bit >= 0; --bit) {
    const int child = ((x >> bit) & 1) | (((y >> bit) & 1) << 1) |
                      (((z >> bit) & 1) << 2);
    code = (code << 3) | child;
  }
  return code;
}

void expandTriangleBounds(MeshCacheNode &node,
                          const PackedSourceVertex &a,
                          const PackedSourceVertex &b,
                          const PackedSourceVertex &c) {
  const QVector3D first(a.x, a.y, a.z);
  if (node.sourceTriangleCount == 0) {
    node.boundsMinimum = first;
    node.boundsMaximum = first;
  }
  for (const QVector3D &position :
       {first, QVector3D(b.x, b.y, b.z), QVector3D(c.x, c.y, c.z)}) {
    node.boundsMinimum.setX(std::min(node.boundsMinimum.x(), position.x()));
    node.boundsMinimum.setY(std::min(node.boundsMinimum.y(), position.y()));
    node.boundsMinimum.setZ(std::min(node.boundsMinimum.z(), position.z()));
    node.boundsMaximum.setX(std::max(node.boundsMaximum.x(), position.x()));
    node.boundsMaximum.setY(std::max(node.boundsMaximum.y(), position.y()));
    node.boundsMaximum.setZ(std::max(node.boundsMaximum.z(), position.z()));
  }
}

bool writeAll(QIODevice &device, const char *data, qint64 byteCount,
              QString &error) {
  while (byteCount > 0) {
    const qint64 written = device.write(data, byteCount);
    if (written <= 0) {
      error = QCoreApplication::translate("Workbench", "Unable to write the mesh cache: %1")
                  .arg(device.errorString());
      return false;
    }
    data += written;
    byteCount -= written;
  }
  return true;
}

bool readAll(QIODevice &device, char *data, qint64 byteCount, QString &error) {
  while (byteCount > 0) {
    const qint64 read = device.read(data, byteCount);
    if (read <= 0) {
      error = QCoreApplication::translate("Workbench", "The mesh-cache page is truncated: %1")
                  .arg(device.errorString());
      return false;
    }
    data += read;
    byteCount -= read;
  }
  return true;
}

} // namespace

bool MeshCacheIndex::isValid() const {
  if (formatVersion != CurrentFormatVersion || sourcePath.isEmpty() ||
      sourceSize <= 0 || fullVertexCount <= 0 || fullFaceCount <= 0 ||
      fullTriangleCount <= 0 || renderableTriangleCount <= 0 ||
      !coordinates.valid ||
      rootNode < 0 || rootNode >= nodes.size() ||
      !nodes.at(rootNode).isValid() || !QFileInfo(dataPath).isFile()) {
    return false;
  }
  const QFileInfo source(sourcePath);
  if (!source.isFile() || source.size() != sourceSize ||
      source.lastModified().toMSecsSinceEpoch() !=
          sourceModifiedMilliseconds) {
    return false;
  }
  const qint64 dataSize = QFileInfo(dataPath).size();
  for (qsizetype index = 0; index < nodes.size(); ++index) {
    const MeshCacheNode &node = nodes.at(index);
    if (node.id != index || node.parent < -1 ||
        node.parent >= nodes.size() ||
        (index == rootNode ? node.parent != -1 : node.parent < 0)) {
      return false;
    }
    for (const int child : node.children) {
      if (child < 0 || child >= nodes.size() || child == index ||
          nodes.at(child).parent != index) {
        return false;
      }
    }
    if (node.isValid() &&
        (node.byteCount() <= 0 || node.dataOffset > dataSize - node.byteCount())) {
      return false;
    }
  }
  return true;
}

QString MeshCache::cacheDirectoryForSource(const QString &sourcePath) {
  return cacheRootForSource(sourcePath);
}

QString MeshCache::indexPathForSource(const QString &sourcePath) {
  return QDir(cacheRootForSource(sourcePath))
      .filePath(QStringLiteral("mesh-%1.json")
                    .arg(QString::fromLatin1(sourceKey(sourcePath))));
}

MeshCacheIndex MeshCache::loadForSource(const QString &sourcePath,
                                        QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  MeshCacheIndex result;
  QFile indexFile(indexPathForSource(sourcePath));
  if (!indexFile.open(QIODevice::ReadOnly)) {
    return result;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(indexFile.readAll(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "The mesh-cache index is invalid.");
    }
    return {};
  }
  const QJsonObject root = document.object();
  result.formatVersion = root.value(QStringLiteral("formatVersion")).toInt();
  result.indexPath = indexFile.fileName();
  result.sourcePath = root.value(QStringLiteral("sourcePath")).toString();
  result.sourceSize =
      root.value(QStringLiteral("sourceSize")).toVariant().toLongLong();
  result.sourceModifiedMilliseconds =
      root.value(QStringLiteral("sourceModifiedMilliseconds"))
          .toVariant()
          .toLongLong();
  result.fullVertexCount =
      root.value(QStringLiteral("fullVertexCount")).toVariant().toLongLong();
  result.fullFaceCount =
      root.value(QStringLiteral("fullFaceCount")).toVariant().toLongLong();
  result.fullTriangleCount =
      root.value(QStringLiteral("fullTriangleCount")).toVariant().toLongLong();
  result.renderableTriangleCount =
      root.value(QStringLiteral("renderableTriangleCount"))
          .toVariant()
          .toLongLong();
  result.hasTextureCoordinates =
      root.value(QStringLiteral("hasTextureCoordinates")).toBool(false);
  result.rootNode = root.value(QStringLiteral("rootNode")).toInt(-1);
  result.dataPath = QDir(QFileInfo(result.indexPath).absolutePath())
                        .filePath(root.value(QStringLiteral("dataFile"))
                                      .toString());
  if (!vectorFromJson(root.value(QStringLiteral("boundsMinimum")),
                      result.boundsMinimum) ||
      !vectorFromJson(root.value(QStringLiteral("boundsMaximum")),
                      result.boundsMaximum) ||
      !sceneCoordinateInfoFromJson(
          root.value(QStringLiteral("coordinates")).toObject(),
          result.coordinates)) {
    return {};
  }
  const QJsonArray nodes = root.value(QStringLiteral("nodes")).toArray();
  result.nodes.reserve(nodes.size());
  for (const QJsonValue &value : nodes) {
    const QJsonObject object = value.toObject();
    MeshCacheNode node;
    node.id = object.value(QStringLiteral("id")).toInt(-1);
    node.depth = object.value(QStringLiteral("depth")).toInt();
    node.parent = object.value(QStringLiteral("parent")).toInt(-1);
    node.dataOffset =
        object.value(QStringLiteral("dataOffset")).toVariant().toLongLong();
    node.vertexCount =
        object.value(QStringLiteral("vertexCount")).toVariant().toLongLong();
    node.indexCount =
        object.value(QStringLiteral("indexCount")).toVariant().toLongLong();
    node.sourceTriangleCount =
        object.value(QStringLiteral("sourceTriangleCount"))
            .toVariant()
            .toLongLong();
    if (!vectorFromJson(object.value(QStringLiteral("boundsMinimum")),
                        node.boundsMinimum) ||
        !vectorFromJson(object.value(QStringLiteral("boundsMaximum")),
                        node.boundsMaximum)) {
      return {};
    }
    const QJsonArray children =
        object.value(QStringLiteral("children")).toArray();
    node.children.reserve(children.size());
    for (const QJsonValue &child : children) {
      node.children.append(child.toInt(-1));
    }
    result.nodes.append(std::move(node));
  }
  const QString requested = normalizedSourcePath(sourcePath);
  if (normalizedSourcePath(result.sourcePath).compare(
          requested, Qt::CaseInsensitive) != 0 ||
      !result.isValid()) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "The mesh cache is stale or incomplete.");
    }
    return {};
  }
  return result;
}

MeshCachePage MeshCache::readNode(const MeshCacheIndex &index,
                                  const int nodeId) {
  MeshCachePage result;
  result.nodeId = nodeId;
  if (index.formatVersion != MeshCacheIndex::CurrentFormatVersion ||
      index.dataPath.isEmpty() || nodeId < 0 || nodeId >= index.nodes.size()) {
    result.error = QCoreApplication::translate("Workbench", "The requested mesh-cache node is invalid.");
    return result;
  }
  const MeshCacheNode &node = index.nodes.at(nodeId);
  if (!node.isValid() || node.vertexCount > std::numeric_limits<qsizetype>::max() ||
      node.indexCount > std::numeric_limits<qsizetype>::max()) {
    result.error = QCoreApplication::translate("Workbench", "The requested mesh-cache node is empty or too large.");
    return result;
  }
  QFile data(index.dataPath);
  if (!data.open(QIODevice::ReadOnly) || node.byteCount() > data.size() ||
      node.dataOffset > data.size() - node.byteCount() ||
      !data.seek(node.dataOffset)) {
    result.error = QCoreApplication::translate("Workbench", "Unable to open the mesh-cache data: %1")
                       .arg(data.errorString());
    return result;
  }
  result.vertices.resize(static_cast<qsizetype>(node.vertexCount));
  result.indices.resize(static_cast<qsizetype>(node.indexCount));
  if (!readAll(data, reinterpret_cast<char *>(result.vertices.data()),
               node.vertexCount * static_cast<qint64>(sizeof(MeshVertex)),
               result.error) ||
      !readAll(data, reinterpret_cast<char *>(result.indices.data()),
               node.indexCount * static_cast<qint64>(sizeof(quint32)),
               result.error)) {
    result.vertices.clear();
    result.indices.clear();
  }
  return result;
}

struct MeshCacheBuilder::Impl {
  QString sourcePath;
  QString cacheRoot;
  QString buildDirectory;
  QString vertexPath;
  QString normalPath;
  qint64 sourceVertexCount = 0;
  qint64 appendedVertexCount = 0;
  qint64 renderableTriangleCount = 0;
  qint64 sourceSize = 0;
  qint64 sourceModifiedMilliseconds = 0;
  QVector3D boundsMinimum;
  QVector3D boundsMaximum;
  SceneCoordinateInfo coordinates;
  bool hasFiniteBounds = false;
  bool started = false;
  bool verticesFinished = false;
  bool finished = false;
  QFile vertexFile;
  QFile normalFile;
  uchar *vertexMapping = nullptr;
  uchar *normalMapping = nullptr;
  QVector<PackedSourceVertex> vertexWriteBuffer;
  QVector<MeshCacheNode> nodes;
  QVector<QVector<TriangleRecord>> reservoirs;
  QHash<int, QVector<TriangleRecord>> bucketBuffers;
  QVector<QString> bucketPaths;
  bool hasTextureCoordinates = false;

  [[nodiscard]] const PackedSourceVertex *vertices() const {
    return reinterpret_cast<const PackedSourceVertex *>(vertexMapping);
  }

  [[nodiscard]] PackedNormalSum *normalSums() const {
    return reinterpret_cast<PackedNormalSum *>(normalMapping);
  }

  bool flushVertices(QString &error) {
    if (vertexWriteBuffer.isEmpty()) {
      return true;
    }
    const bool ok = writeAll(
        vertexFile,
        reinterpret_cast<const char *>(vertexWriteBuffer.constData()),
        vertexWriteBuffer.size() *
            static_cast<qint64>(sizeof(PackedSourceVertex)),
        error);
    vertexWriteBuffer.clear();
    return ok;
  }

  bool flushBucket(const int leafCode, QString &error) {
    auto iterator = bucketBuffers.find(leafCode);
    if (iterator == bucketBuffers.end() || iterator->isEmpty()) {
      return true;
    }
    QFile bucket(bucketPaths.at(leafCode));
    if (!bucket.open(QIODevice::WriteOnly | QIODevice::Append)) {
      error = QCoreApplication::translate("Workbench", "Unable to write mesh-cache bucket %1: %2")
                  .arg(leafCode)
                  .arg(bucket.errorString());
      return false;
    }
    if (!writeAll(bucket, reinterpret_cast<const char *>(iterator->constData()),
                  iterator->size() *
                      static_cast<qint64>(sizeof(TriangleRecord)),
                  error)) {
      return false;
    }
    iterator->clear();
    return true;
  }

  void releaseMappings() {
    if (normalMapping != nullptr) {
      normalFile.unmap(normalMapping);
      normalMapping = nullptr;
    }
    if (vertexMapping != nullptr) {
      vertexFile.unmap(vertexMapping);
      vertexMapping = nullptr;
    }
    normalFile.close();
    vertexFile.close();
  }

  void cleanupBuildDirectory() {
    releaseMappings();
    if (!buildDirectory.isEmpty() && QFileInfo(buildDirectory).isDir() &&
        QFileInfo(buildDirectory).dir().absolutePath().compare(
            QFileInfo(cacheRoot).absoluteFilePath(), Qt::CaseInsensitive) ==
            0) {
      QDir(buildDirectory).removeRecursively();
    }
  }

  bool writePage(QSaveFile &data, MeshCacheNode &node,
                 const QVector<TriangleRecord> &triangles, QString &error) {
    if (triangles.isEmpty()) {
      return true;
    }
    QHash<MeshCornerKey, quint32> localIndices;
    localIndices.reserve(static_cast<qsizetype>(std::min<qint64>(
        triangles.size() * 2LL, std::numeric_limits<int>::max())));
    QVector<MeshVertex> pageVertices;
    QVector<quint32> pageIndices;
    pageVertices.reserve(
        static_cast<qsizetype>(std::min<qint64>(triangles.size() * 2LL,
                                               196'608LL)));
    pageIndices.reserve(triangles.size() * 3);
    const auto appendIndex = [&](const quint32 sourceIndex,
                                 const float textureU,
                                 const float textureV,
                                 const bool textured) {
      const float canonicalU = canonicalTextureCoordinate(textureU);
      const float canonicalV = canonicalTextureCoordinate(textureV);
      const MeshCornerKey key{
          sourceIndex, std::bit_cast<quint32>(canonicalU),
          std::bit_cast<quint32>(canonicalV), textured ? 1U : 0U};
      const auto existing = localIndices.constFind(key);
      if (existing != localIndices.cend()) {
        pageIndices.append(existing.value());
        return;
      }
      const PackedSourceVertex &source = vertices()[sourceIndex];
      const PackedNormalSum &sum = normalSums()[sourceIndex];
      QVector3D normal(sum.x, sum.y, sum.z);
      if (!std::isfinite(normal.lengthSquared()) ||
          normal.lengthSquared() <= 1.0e-20F) {
        normal = QVector3D(0.0F, 0.0F, 1.0F);
      } else {
        normal.normalize();
      }
      MeshVertex vertex;
      vertex.x = source.x;
      vertex.y = source.y;
      vertex.z = source.z;
      vertex.red = source.red;
      vertex.green = source.green;
      vertex.blue = source.blue;
      vertex.normalX = normal.x();
      vertex.normalY = normal.y();
      vertex.normalZ = normal.z();
      vertex.textureU = canonicalU;
      vertex.textureV = canonicalV;
      vertex.textureWeight = textured ? 1.0F : 0.0F;
      const quint32 local = static_cast<quint32>(pageVertices.size());
      pageVertices.append(vertex);
      localIndices.insert(key, local);
      pageIndices.append(local);
    };
    for (const TriangleRecord &triangle : triangles) {
      const std::array<quint32, 3> sourceIndices = {
          triangle.a, triangle.b, triangle.c};
      for (qsizetype corner = 0; corner < 3; ++corner) {
        appendIndex(sourceIndices.at(static_cast<std::size_t>(corner)),
                    triangle.textureU.at(static_cast<std::size_t>(corner)),
                    triangle.textureV.at(static_cast<std::size_t>(corner)),
                    triangle.textured != 0);
      }
    }
    node.dataOffset = data.pos();
    node.vertexCount = pageVertices.size();
    node.indexCount = pageIndices.size();
    return writeAll(data,
                    reinterpret_cast<const char *>(pageVertices.constData()),
                    pageVertices.size() *
                        static_cast<qint64>(sizeof(MeshVertex)),
                    error) &&
           writeAll(data,
                    reinterpret_cast<const char *>(pageIndices.constData()),
                    pageIndices.size() * static_cast<qint64>(sizeof(quint32)),
                    error);
  }
};

MeshCacheBuilder::MeshCacheBuilder(const QString &sourcePath,
                                   const qint64 sourceVertexCount,
                                   const SceneCoordinateInfo &coordinates)
    : mImpl(std::make_unique<Impl>()) {
  mImpl->sourcePath = normalizedSourcePath(sourcePath);
  mImpl->sourceVertexCount = sourceVertexCount;
  mImpl->coordinates = coordinates;
  mImpl->cacheRoot = cacheRootForSource(sourcePath);
}

void MeshCacheBuilder::setCoordinateInfo(
    const SceneCoordinateInfo &coordinates) {
  mImpl->coordinates = coordinates;
}

MeshCacheBuilder::~MeshCacheBuilder() {
  if (mImpl != nullptr && !mImpl->finished) {
    mImpl->cleanupBuildDirectory();
  }
}

bool MeshCacheBuilder::begin(QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  QString error;
  const QFileInfo source(mImpl->sourcePath);
  QDir cacheRoot(mImpl->cacheRoot);
  if (mImpl->started || mImpl->finished) {
    error = QCoreApplication::translate("Workbench", "The mesh-cache builder has already been used.");
  } else if (mImpl->sourceVertexCount <= 0 ||
             mImpl->sourceVertexCount >
                 static_cast<qint64>(std::numeric_limits<quint32>::max())) {
    error = QCoreApplication::translate("Workbench", "The mesh vertex count exceeds the 32-bit PLY index range.");
  } else if (!source.isFile()) {
    error = QCoreApplication::translate("Workbench", "The source mesh no longer exists.");
  } else if (!cacheRoot.mkpath(QStringLiteral("."))) {
    error = QCoreApplication::translate("Workbench", "Unable to create the mesh-cache directory %1.")
                .arg(mImpl->cacheRoot);
  }
  const QString key = QString::fromLatin1(sourceKey(mImpl->sourcePath));
  if (error.isEmpty()) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    cacheRoot.setNameFilters(
        {QStringLiteral("mesh-%1-build-*").arg(key)});
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
      QStringLiteral("mesh-%1-build-%2")
          .arg(key, QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (error.isEmpty() && !QDir().mkpath(mImpl->buildDirectory)) {
    error = QCoreApplication::translate("Workbench", "Unable to create temporary mesh-cache storage.");
  }
  if (!error.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = error;
    }
    return false;
  }

  mImpl->sourceSize = source.size();
  mImpl->sourceModifiedMilliseconds =
      source.lastModified().toMSecsSinceEpoch();
  mImpl->vertexPath =
      QDir(mImpl->buildDirectory).filePath(QStringLiteral("vertices.bin"));
  mImpl->normalPath =
      QDir(mImpl->buildDirectory).filePath(QStringLiteral("normals.bin"));
  mImpl->vertexFile.setFileName(mImpl->vertexPath);
  if (!mImpl->vertexFile.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "Unable to create the mesh vertex spool: %1")
                          .arg(mImpl->vertexFile.errorString());
    }
    return false;
  }
  mImpl->vertexWriteBuffer.reserve(kVertexWriteBufferSize);
  mImpl->nodes.resize(kSpatialNodeCount);
  mImpl->reservoirs.resize(kSpatialNodeCount);
  mImpl->bucketPaths.resize(kSpatialLeafCount);
  for (int depth = 0; depth <= MeshCacheIndex::SpatialOctreeDepth; ++depth) {
    const int count = 1 << (depth * 3);
    for (int prefix = 0; prefix < count; ++prefix) {
      const int id = nodeIndex(depth, prefix);
      MeshCacheNode &node = mImpl->nodes[id];
      node.id = id;
      node.depth = depth;
      node.parent = depth == 0 ? -1 : nodeIndex(depth - 1, prefix >> 3);
      if (depth < MeshCacheIndex::SpatialOctreeDepth) {
        node.children.reserve(8);
        for (int child = 0; child < 8; ++child) {
          node.children.append(nodeIndex(depth + 1, prefix * 8 + child));
        }
      }
    }
  }
  for (int leaf = 0; leaf < kSpatialLeafCount; ++leaf) {
    mImpl->bucketPaths[leaf] =
        QDir(mImpl->buildDirectory)
            .filePath(QStringLiteral("leaf-%1.bin")
                          .arg(leaf, 4, 10, QLatin1Char('0')));
  }
  mImpl->started = true;
  return true;
}

bool MeshCacheBuilder::appendVertex(const MeshVertex &vertex,
                                    QString *errorMessage) {
  if (!mImpl->started || mImpl->verticesFinished || mImpl->finished ||
      mImpl->appendedVertexCount >= mImpl->sourceVertexCount) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "The mesh-cache vertex stream is not active.");
    }
    return false;
  }
  PackedSourceVertex packed;
  packed.x = vertex.x;
  packed.y = vertex.y;
  packed.z = vertex.z;
  packed.red = vertex.red;
  packed.green = vertex.green;
  packed.blue = vertex.blue;
  if (finitePosition(packed)) {
    const QVector3D position(packed.x, packed.y, packed.z);
    if (!mImpl->hasFiniteBounds) {
      mImpl->boundsMinimum = position;
      mImpl->boundsMaximum = position;
      mImpl->hasFiniteBounds = true;
    } else {
      mImpl->boundsMinimum.setX(
          std::min(mImpl->boundsMinimum.x(), position.x()));
      mImpl->boundsMinimum.setY(
          std::min(mImpl->boundsMinimum.y(), position.y()));
      mImpl->boundsMinimum.setZ(
          std::min(mImpl->boundsMinimum.z(), position.z()));
      mImpl->boundsMaximum.setX(
          std::max(mImpl->boundsMaximum.x(), position.x()));
      mImpl->boundsMaximum.setY(
          std::max(mImpl->boundsMaximum.y(), position.y()));
      mImpl->boundsMaximum.setZ(
          std::max(mImpl->boundsMaximum.z(), position.z()));
    }
  }
  mImpl->vertexWriteBuffer.append(packed);
  ++mImpl->appendedVertexCount;
  if (mImpl->vertexWriteBuffer.size() >= kVertexWriteBufferSize) {
    QString error;
    if (!mImpl->flushVertices(error)) {
      if (errorMessage != nullptr) {
        *errorMessage = error;
      }
      return false;
    }
  }
  return true;
}

bool MeshCacheBuilder::finishVertices(QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  QString error;
  if (!mImpl->started || mImpl->verticesFinished || mImpl->finished) {
    error = QCoreApplication::translate("Workbench", "The mesh-cache vertex stream is not active.");
  } else if (mImpl->appendedVertexCount != mImpl->sourceVertexCount) {
    error = QCoreApplication::translate("Workbench", "The mesh vertex spool is incomplete.");
  } else if (!mImpl->hasFiniteBounds) {
    error = QCoreApplication::translate("Workbench", "The mesh contains no finite vertices.");
  } else if (!mImpl->flushVertices(error) || !mImpl->vertexFile.flush()) {
    if (error.isEmpty()) {
      error = QCoreApplication::translate("Workbench", "Unable to finalize the mesh vertex spool.");
    }
  }
  const qint64 vertexBytes =
      mImpl->sourceVertexCount * static_cast<qint64>(sizeof(PackedSourceVertex));
  const qint64 normalBytes =
      mImpl->sourceVertexCount * static_cast<qint64>(sizeof(PackedNormalSum));
  if (error.isEmpty() && mImpl->vertexFile.size() != vertexBytes) {
    error = QCoreApplication::translate("Workbench", "The mesh vertex spool has an unexpected size.");
  }
  if (error.isEmpty()) {
    mImpl->vertexMapping = mImpl->vertexFile.map(0, vertexBytes);
    if (mImpl->vertexMapping == nullptr) {
      error = QCoreApplication::translate("Workbench", "Unable to map the mesh vertex spool: %1")
                  .arg(mImpl->vertexFile.errorString());
    }
  }
  if (error.isEmpty()) {
    mImpl->normalFile.setFileName(mImpl->normalPath);
    if (!mImpl->normalFile.open(QIODevice::ReadWrite | QIODevice::Truncate) ||
        !mImpl->normalFile.resize(normalBytes)) {
      error = QCoreApplication::translate("Workbench", "Unable to create the mesh normal spool: %1")
                  .arg(mImpl->normalFile.errorString());
    }
  }
  if (error.isEmpty()) {
    mImpl->normalMapping = mImpl->normalFile.map(0, normalBytes);
    if (mImpl->normalMapping == nullptr) {
      error = QCoreApplication::translate("Workbench", "Unable to map the mesh normal spool: %1")
                  .arg(mImpl->normalFile.errorString());
    }
  }
  if (!error.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = error;
    }
    return false;
  }
  mImpl->verticesFinished = true;
  return true;
}

bool MeshCacheBuilder::appendTriangle(const quint32 a, const quint32 b,
                                      const quint32 c,
                                      const qint64 sourceTriangleIndex,
                                      QString *errorMessage) {
  return appendTriangleInternal(a, b, c, sourceTriangleIndex, {}, {}, {},
                                false, errorMessage);
}

bool MeshCacheBuilder::appendTexturedTriangle(
    const quint32 a, const quint32 b, const quint32 c,
    const qint64 sourceTriangleIndex, const QVector2D &textureA,
    const QVector2D &textureB, const QVector2D &textureC,
    QString *errorMessage) {
  return appendTriangleInternal(a, b, c, sourceTriangleIndex, textureA,
                                textureB, textureC, true, errorMessage);
}

bool MeshCacheBuilder::appendTriangleInternal(
    const quint32 a, const quint32 b, const quint32 c,
    const qint64 sourceTriangleIndex, const QVector2D &textureA,
    const QVector2D &textureB, const QVector2D &textureC,
    const bool textured, QString *errorMessage) {
  if (!mImpl->verticesFinished || mImpl->finished) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "The mesh-cache triangle stream is not active.");
    }
    return false;
  }
  if (textured &&
      (!std::isfinite(textureA.x()) || !std::isfinite(textureA.y()) ||
       !std::isfinite(textureB.x()) || !std::isfinite(textureB.y()) ||
       !std::isfinite(textureC.x()) || !std::isfinite(textureC.y()))) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "A textured mesh triangle contains a non-finite UV coordinate.");
    }
    return false;
  }
  if (a >= static_cast<quint64>(mImpl->sourceVertexCount) ||
      b >= static_cast<quint64>(mImpl->sourceVertexCount) ||
      c >= static_cast<quint64>(mImpl->sourceVertexCount)) {
    if (errorMessage != nullptr) {
      *errorMessage = QCoreApplication::translate("Workbench", "A mesh face references a vertex outside the PLY vertex table.");
    }
    return false;
  }
  if (a == b || b == c || c == a) {
    return true;
  }
  const PackedSourceVertex &va = mImpl->vertices()[a];
  const PackedSourceVertex &vb = mImpl->vertices()[b];
  const PackedSourceVertex &vc = mImpl->vertices()[c];
  if (!finitePosition(va) || !finitePosition(vb) || !finitePosition(vc)) {
    return true;
  }
  const QVector3D pa(va.x, va.y, va.z);
  const QVector3D pb(vb.x, vb.y, vb.z);
  const QVector3D pc(vc.x, vc.y, vc.z);
  const QVector3D faceNormal = QVector3D::crossProduct(pb - pa, pc - pa);
  if (!std::isfinite(faceNormal.lengthSquared()) ||
      faceNormal.lengthSquared() <= 1.0e-20F) {
    return true;
  }
  for (const quint32 index : {a, b, c}) {
    PackedNormalSum &sum = mImpl->normalSums()[index];
    sum.x += faceNormal.x();
    sum.y += faceNormal.y();
    sum.z += faceNormal.z();
  }
  const QVector3D centroid = (pa + pb + pc) / 3.0F;
  const int leafCode = octreeLeafCode(
      centroid, mImpl->boundsMinimum, mImpl->boundsMaximum);
  TriangleRecord triangle;
  triangle.a = a;
  triangle.b = b;
  triangle.c = c;
  if (textured) {
    triangle.textureU = {canonicalTextureCoordinate(textureA.x()),
                         canonicalTextureCoordinate(textureB.x()),
                         canonicalTextureCoordinate(textureC.x())};
    triangle.textureV = {canonicalTextureCoordinate(textureA.y()),
                         canonicalTextureCoordinate(textureB.y()),
                         canonicalTextureCoordinate(textureC.y())};
    triangle.textured = 1;
    mImpl->hasTextureCoordinates = true;
  }
  int prefix = 0;
  for (int depth = 0; depth <= MeshCacheIndex::SpatialOctreeDepth; ++depth) {
    if (depth > 0) {
      const int shift =
          (MeshCacheIndex::SpatialOctreeDepth - depth) * 3;
      prefix = leafCode >> shift;
    }
    const int id = nodeIndex(depth, prefix);
    MeshCacheNode &node = mImpl->nodes[id];
    const qint64 previousCount = node.sourceTriangleCount;
    expandTriangleBounds(node, va, vb, vc);
    ++node.sourceTriangleCount;
    QVector<TriangleRecord> &reservoir = mImpl->reservoirs[id];
    const qsizetype limit = reservoirLimit(depth);
    if (reservoir.size() < limit) {
      reservoir.append(triangle);
    } else {
      const quint64 hash = splitMix64(
          static_cast<quint64>(sourceTriangleIndex) ^
          (static_cast<quint64>(id) << 40U));
      const quint64 replacement =
          hash % static_cast<quint64>(previousCount + 1);
      if (replacement < static_cast<quint64>(limit)) {
        reservoir[static_cast<qsizetype>(replacement)] = triangle;
      }
    }
  }
  QVector<TriangleRecord> &bucket = mImpl->bucketBuffers[leafCode];
  if (bucket.isEmpty()) {
    bucket.reserve(kTriangleBucketBufferSize);
  }
  bucket.append(triangle);
  if (bucket.size() >= kTriangleBucketBufferSize) {
    QString error;
    if (!mImpl->flushBucket(leafCode, error)) {
      if (errorMessage != nullptr) {
        *errorMessage = error;
      }
      return false;
    }
  }
  ++mImpl->renderableTriangleCount;
  return true;
}

MeshCacheIndex MeshCacheBuilder::finish(const qint64 sourceFaceCount,
                                        const qint64 sourceTriangleCount,
                                        QString *errorMessage) {
  if (errorMessage != nullptr) {
    errorMessage->clear();
  }
  MeshCacheIndex result;
  QString error;
  if (!mImpl->verticesFinished || mImpl->finished) {
    error = QCoreApplication::translate("Workbench", "The mesh-cache builder is not ready to finish.");
  } else if (sourceFaceCount <= 0 || sourceTriangleCount <= 0 ||
             mImpl->renderableTriangleCount <= 0) {
    error = QCoreApplication::translate("Workbench", "The PLY mesh contains no renderable triangles.");
  }
  for (auto iterator = mImpl->bucketBuffers.begin();
       error.isEmpty() && iterator != mImpl->bucketBuffers.end(); ++iterator) {
    if (!mImpl->flushBucket(iterator.key(), error)) {
      break;
    }
  }
  const QFileInfo sourceBefore(mImpl->sourcePath);
  if (error.isEmpty() &&
      (sourceBefore.size() != mImpl->sourceSize ||
       sourceBefore.lastModified().toMSecsSinceEpoch() !=
           mImpl->sourceModifiedMilliseconds)) {
    error = QCoreApplication::translate("Workbench", "The source mesh changed while its cache was being built.");
  }
  const QString key = QString::fromLatin1(sourceKey(mImpl->sourcePath));
  const QString dataFileName =
      QStringLiteral("mesh-%1-v%2-%3-%4-%5.bin")
          .arg(key)
          .arg(MeshCacheIndex::CurrentFormatVersion)
          .arg(mImpl->sourceSize)
          .arg(mImpl->sourceModifiedMilliseconds)
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
  const QString dataPath = QDir(mImpl->cacheRoot).filePath(dataFileName);
  QSaveFile data(dataPath);
  data.setDirectWriteFallback(false);
  if (error.isEmpty() && !data.open(QIODevice::WriteOnly)) {
    error = QCoreApplication::translate("Workbench", "Unable to create mesh-cache data: %1")
                .arg(data.errorString());
  }

  for (int nodeId = 0;
       error.isEmpty() && nodeId < kSpatialNodeCount; ++nodeId) {
    MeshCacheNode &node = mImpl->nodes[nodeId];
    if (node.sourceTriangleCount <= 0) {
      continue;
    }
    if (node.depth < MeshCacheIndex::SpatialOctreeDepth) {
      if (!mImpl->writePage(data, node, mImpl->reservoirs.at(nodeId), error)) {
        break;
      }
      continue;
    }
    QFile bucket(mImpl->bucketPaths.at(nodeId -
                                       kDepthOffsets.at(4)));
    if (!bucket.open(QIODevice::ReadOnly) ||
        bucket.size() % static_cast<qint64>(sizeof(TriangleRecord)) != 0) {
      error = QCoreApplication::translate("Workbench", "Unable to read a mesh-cache triangle bucket: %1")
                  .arg(bucket.errorString());
      break;
    }
    const qint64 triangleCount =
        bucket.size() / static_cast<qint64>(sizeof(TriangleRecord));
    if (triangleCount <= kExactPageTriangleLimit) {
      QVector<TriangleRecord> triangles(static_cast<qsizetype>(triangleCount));
      if (!readAll(bucket, reinterpret_cast<char *>(triangles.data()),
                   bucket.size(), error) ||
          !mImpl->writePage(data, node, triangles, error)) {
        break;
      }
      continue;
    }

    if (!mImpl->writePage(data, node, mImpl->reservoirs.at(nodeId), error)) {
      break;
    }
    node.children.clear();
    qint64 remaining = triangleCount;
    while (error.isEmpty() && remaining > 0) {
      const qsizetype chunkCount = static_cast<qsizetype>(
          std::min<qint64>(remaining, kExactPageTriangleLimit));
      QVector<TriangleRecord> triangles(chunkCount);
      if (!readAll(bucket, reinterpret_cast<char *>(triangles.data()),
                   chunkCount * static_cast<qint64>(sizeof(TriangleRecord)),
                   error)) {
        break;
      }
      MeshCacheNode child;
      child.id = mImpl->nodes.size();
      child.depth = MeshCacheIndex::SpatialOctreeDepth + 1;
      child.parent = nodeId;
      for (const TriangleRecord &triangle : std::as_const(triangles)) {
        expandTriangleBounds(child, mImpl->vertices()[triangle.a],
                             mImpl->vertices()[triangle.b],
                             mImpl->vertices()[triangle.c]);
        ++child.sourceTriangleCount;
      }
      if (!mImpl->writePage(data, child, triangles, error)) {
        break;
      }
      const int childId = child.id;
      mImpl->nodes.append(std::move(child));
      mImpl->nodes[nodeId].children.append(childId);
      remaining -= chunkCount;
    }
  }
  if (error.isEmpty() && !data.commit()) {
    error = QCoreApplication::translate("Workbench", "Unable to publish mesh-cache data: %1")
                .arg(data.errorString());
  }
  const QFileInfo sourceAfter(mImpl->sourcePath);
  if (error.isEmpty() &&
      (sourceAfter.size() != mImpl->sourceSize ||
       sourceAfter.lastModified().toMSecsSinceEpoch() !=
           mImpl->sourceModifiedMilliseconds)) {
    QFile::remove(dataPath);
    error = QCoreApplication::translate("Workbench", "The source mesh changed while its cache was being built.");
  }
  if (!error.isEmpty()) {
    if (errorMessage != nullptr) {
      *errorMessage = error;
    }
    return {};
  }

  result.formatVersion = MeshCacheIndex::CurrentFormatVersion;
  result.indexPath = MeshCache::indexPathForSource(mImpl->sourcePath);
  result.dataPath = dataPath;
  result.sourcePath = mImpl->sourcePath;
  result.sourceSize = mImpl->sourceSize;
  result.sourceModifiedMilliseconds = mImpl->sourceModifiedMilliseconds;
  result.fullVertexCount = mImpl->sourceVertexCount;
  result.fullFaceCount = sourceFaceCount;
  result.fullTriangleCount = sourceTriangleCount;
  result.renderableTriangleCount = mImpl->renderableTriangleCount;
  result.hasTextureCoordinates = mImpl->hasTextureCoordinates;
  result.boundsMinimum = mImpl->boundsMinimum;
  result.boundsMaximum = mImpl->boundsMaximum;
  result.coordinates = mImpl->coordinates;
  if (!result.coordinates.valid) {
    result.coordinates.valid = true;
    result.coordinates.globalMinimum = {
        result.boundsMinimum.x(), result.boundsMinimum.y(),
        result.boundsMinimum.z()};
    result.coordinates.globalMaximum = {
        result.boundsMaximum.x(), result.boundsMaximum.y(),
        result.boundsMaximum.z()};
  }
  result.nodes = mImpl->nodes;
  result.rootNode = 0;

  QJsonObject root;
  root.insert(QStringLiteral("formatVersion"), result.formatVersion);
  root.insert(QStringLiteral("sourcePath"), result.sourcePath);
  root.insert(QStringLiteral("sourceSize"), result.sourceSize);
  root.insert(QStringLiteral("sourceModifiedMilliseconds"),
              result.sourceModifiedMilliseconds);
  root.insert(QStringLiteral("fullVertexCount"), result.fullVertexCount);
  root.insert(QStringLiteral("fullFaceCount"), result.fullFaceCount);
  root.insert(QStringLiteral("fullTriangleCount"), result.fullTriangleCount);
  root.insert(QStringLiteral("renderableTriangleCount"),
              result.renderableTriangleCount);
  root.insert(QStringLiteral("hasTextureCoordinates"),
              result.hasTextureCoordinates);
  root.insert(QStringLiteral("rootNode"), result.rootNode);
  root.insert(QStringLiteral("dataFile"), QFileInfo(dataPath).fileName());
  root.insert(QStringLiteral("boundsMinimum"),
              vectorToJson(result.boundsMinimum));
  root.insert(QStringLiteral("boundsMaximum"),
              vectorToJson(result.boundsMaximum));
  root.insert(QStringLiteral("coordinates"),
              sceneCoordinateInfoToJson(result.coordinates));
  QJsonArray nodes;
  for (const MeshCacheNode &node : std::as_const(result.nodes)) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), node.id);
    object.insert(QStringLiteral("depth"), node.depth);
    object.insert(QStringLiteral("parent"), node.parent);
    object.insert(QStringLiteral("dataOffset"), node.dataOffset);
    object.insert(QStringLiteral("vertexCount"), node.vertexCount);
    object.insert(QStringLiteral("indexCount"), node.indexCount);
    object.insert(QStringLiteral("sourceTriangleCount"),
                  node.sourceTriangleCount);
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
      *errorMessage = QCoreApplication::translate("Workbench", "Unable to publish the mesh-cache index: %1")
                          .arg(indexFile.errorString());
    }
    return {};
  }
  mImpl->finished = true;
  mImpl->cleanupBuildDirectory();
  QDir cacheRoot(mImpl->cacheRoot);
  cacheRoot.setNameFilters({QStringLiteral("mesh-%1-v*.bin").arg(key)});
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

QVector3D MeshCacheBuilder::boundsMinimum() const {
  return mImpl->boundsMinimum;
}

QVector3D MeshCacheBuilder::boundsMaximum() const {
  return mImpl->boundsMaximum;
}

} // namespace gsw

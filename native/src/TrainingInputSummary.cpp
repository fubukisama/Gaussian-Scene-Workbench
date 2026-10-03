#include "TrainingInputSummary.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QSet>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>

namespace gsw {
namespace {

bool isCancelled(const TrainingInputCancellation &cancellation) {
  return cancellation && cancellation->load(std::memory_order_relaxed);
}

bool isWithinDirectory(const QString &path, const QString &root) {
#ifdef Q_OS_WIN
  constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
  constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
  const QString prefix = root.endsWith(QLatin1Char('/'))
      ? root : root + QLatin1Char('/');
  return path.compare(root, sensitivity) == 0 || path.startsWith(prefix, sensitivity);
}

bool isSupportedImage(const QFileInfo &file) {
  static const QSet<QString> extensions = {
      QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
      QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("bmp"),
      QStringLiteral("webp"), QStringLiteral("exr")};
  return extensions.contains(file.suffix().toLower());
}

TrainingInputSummary scanDirectory(
    const QString &directory, const QString &datasetCanonicalPath,
    const TrainingInputCancellation &cancellation, const bool recursive) {
  TrainingInputSummary result;
  const QFileInfo source(directory);
  const QString canonicalSource = source.canonicalFilePath();
  if (!source.isDir() || source.isSymLink() || canonicalSource.isEmpty() ||
      !isWithinDirectory(canonicalSource, datasetCanonicalPath)) {
    return result;
  }
  result.sourceDirectory = source.absoluteFilePath();
  QList<QString> pending{canonicalSource};
  QSet<QString> visited;
  QHash<quint64, qint64> sizes;
  while (!pending.isEmpty()) {
    if (isCancelled(cancellation)) {
      result.cancelled = true;
      return result;
    }
    const QString current = pending.takeLast();
    if (visited.contains(current)) {
      continue;
    }
    visited.insert(current);
    const auto entries = QDir(current).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
      if (isCancelled(cancellation)) {
        result.cancelled = true;
        return result;
      }
      const QString canonicalEntry = entry.canonicalFilePath();
      // Do not follow symlinks/junctions outside the dataset, nor revisit a
      // directory through aliases. Missing targets also remain uncounted.
      if (entry.isSymLink() || canonicalEntry.isEmpty() ||
          !isWithinDirectory(canonicalEntry, datasetCanonicalPath)) {
        if (entry.isDir() || isSupportedImage(entry)) {
          ++result.skippedLinkCount;
        }
        continue;
      }
      if (entry.isDir()) {
        if (recursive) {
          pending.append(canonicalEntry);
        }
        continue;
      }
      if (!entry.isFile() || !isSupportedImage(entry)) {
        continue;
      }
      ++result.imageCount;
      QImageReader reader(entry.absoluteFilePath());
      const QSize size = reader.size(); // Header read; never reader.read().
      if (size.width() <= 0 || size.height() <= 0) {
        ++result.unknownSizeCount;
        continue;
      }
      const quint64 key = (quint64(quint32(size.width())) << 32) |
                          quint64(quint32(size.height()));
      ++sizes[key];
    }
  }
  for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) {
    result.sizeGroups.append({QSize(int(it.key() >> 32),
                                   int(it.key() & 0xffffffffULL)),
                              it.value()});
  }
  std::sort(result.sizeGroups.begin(), result.sizeGroups.end(),
            [](const auto &first, const auto &second) {
              if (first.imageCount != second.imageCount) {
                return first.imageCount > second.imageCount;
              }
              if (first.sourceSize.width() != second.sourceSize.width()) {
                return first.sourceSize.width() < second.sourceSize.width();
              }
              return first.sourceSize.height() < second.sourceSize.height();
            });
  return result;
}

int roundedDimension(const int dimension, const int divisor) {
  const int quotient = dimension / divisor;
  const int remainder = dimension % divisor;
  const qint64 twiceRemainder = qint64(remainder) * 2;
  return quotient + (twiceRemainder > divisor ||
                     (twiceRemainder == divisor && (quotient % 2) != 0));
}

} // namespace

TrainingInputSummary scanTrainingInputSummary(
    const QString &datasetPath, const bool runColmap,
    const TrainingInputCancellation &cancellation) {
  TrainingInputSummary empty;
  if (isCancelled(cancellation)) {
    empty.cancelled = true;
    return empty;
  }
  const QFileInfo dataset(datasetPath);
  const QString canonicalRoot = dataset.canonicalFilePath();
  if (!dataset.isDir() || canonicalRoot.isEmpty()) {
    return empty;
  }
  const QDir root(dataset.absoluteFilePath());
  const QStringList candidates = runColmap
      ? QStringList{root.filePath(QStringLiteral("input")),
                    root.filePath(QStringLiteral("images")), root.absolutePath()}
      : QStringList{root.filePath(QStringLiteral("images")),
                    root.filePath(QStringLiteral("input")), root.absolutePath()};
  for (qsizetype index = 0; index < candidates.size(); ++index) {
    // A root-only fallback must not count old output renders or unrelated
    // project subdirectories as training photos. input/images are explicit
    // photo collections and can contain camera-specific subdirectories.
    auto result = scanDirectory(candidates.at(index), canonicalRoot, cancellation,
                                index < 2);
    if (result.cancelled || result.imageCount > 0) {
      return result;
    }
  }
  return empty;
}

QSize projectedTrainingImageSize(const QSize &sourceSize, const int resolution) {
  if (sourceSize.width() <= 0 || sourceSize.height() <= 0 ||
      (resolution != 1 && resolution != 2 && resolution != 4 && resolution != 8)) {
    return {};
  }
  return {roundedDimension(sourceSize.width(), resolution),
          roundedDimension(sourceSize.height(), resolution)};
}

TrainingInputSummaryScanner::TrainingInputSummaryScanner(QObject *parent)
    : QObject(parent) {
  qRegisterMetaType<TrainingInputSummary>();
}

TrainingInputSummaryScanner::~TrainingInputSummaryScanner() {
  cancel();
}

void TrainingInputSummaryScanner::cancel() {
  ++mGeneration;
  if (mCancellation) {
    mCancellation->store(true, std::memory_order_relaxed);
    mCancellation.reset();
  }
}

void TrainingInputSummaryScanner::request(const QString &datasetPath,
                                          const bool runColmap) {
  cancel();
  const quint64 generation = mGeneration;
  const QString absolutePath = QFileInfo(datasetPath).absoluteFilePath();
  const QString cacheKey = absolutePath + (runColmap ? QStringLiteral("\ninput")
                                                    : QStringLiteral("\nimages"));
  const auto found = mCache.constFind(cacheKey);
  if (found != mCache.cend()) {
    const auto result = found.value();
    QTimer::singleShot(0, this, [this, generation, result] {
      if (generation == mGeneration) {
        emit summaryReady(result);
      }
    });
    return;
  }
  auto cancellation = std::make_shared<std::atomic_bool>(false);
  mCancellation = cancellation;
  auto *watcher = new QFutureWatcher<TrainingInputSummary>(this);
  connect(watcher, &QFutureWatcher<TrainingInputSummary>::finished, this,
          [this, watcher, generation, cacheKey] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (generation != mGeneration || result.cancelled) {
              return;
            }
            mCache.insert(cacheKey, result);
            mCancellation.reset();
            emit summaryReady(result);
          });
  watcher->setFuture(QtConcurrent::run([absolutePath, runColmap, cancellation] {
    return scanTrainingInputSummary(absolutePath, runColmap, cancellation);
  }));
}

} // namespace gsw

#include <QCoreApplication>
#include "ProcessSupervisor.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QTimer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <QSaveFile>

#include <cmath>
#include <limits>
#include <utility>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsw {

namespace {

const QByteArray kWorkerEventPrefix = QByteArrayLiteral("[worker-event] ");
const QByteArray kTrainingGpuPreviewPrefix =
    QByteArrayLiteral("[gsw-training-gpu-preview] ");

QByteArray jsonObjectPrefix(const QByteArray &payload) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error == QJsonParseError::NoError && document.isObject()) {
    return payload;
  }
  if (parseError.error != QJsonParseError::GarbageAtEnd ||
      parseError.offset <= 0 || parseError.offset > payload.size()) {
    return {};
  }
  const QByteArray prefix = payload.first(parseError.offset).trimmed();
  parseError = {};
  const QJsonDocument prefixDocument =
      QJsonDocument::fromJson(prefix, &parseError);
  return parseError.error == QJsonParseError::NoError &&
                 prefixDocument.isObject()
             ? prefix
             : QByteArray();
}

bool parseOptionalInteger(const QJsonObject &object, const QString &name,
                          std::optional<int> *target, const int minimum = 0) {
  const QJsonValue value = object.value(name);
  if (value.isUndefined()) {
    return true;
  }
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(std::numeric_limits<int>::max())) {
    return false;
  }
  *target = static_cast<int>(number);
  return true;
}

bool parseOptionalReal(const QJsonObject &object, const QString &name,
                       std::optional<double> *target,
                       const double minimum = 0.0) {
  const QJsonValue value = object.value(name);
  if (value.isUndefined()) {
    return true;
  }
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < minimum) {
    return false;
  }
  *target = number;
  return true;
}

// JSON numbers remain exactly representable through every Python/Qt hop.
constexpr double MaximumExactJsonInteger = 9007199254740991.0;

bool nonnegativeInteger(const QJsonValue &value) {
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  return std::isfinite(number) && number >= 0.0 &&
         number <= MaximumExactJsonInteger && std::floor(number) == number;
}

bool nonnegativeReal(const QJsonValue &value) {
  return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0.0;
}

bool observerHeader(const QJsonObject &payload) {
  const auto version = payload.value(QStringLiteral("version"));
  const QString backend = payload.value(QStringLiteral("backend")).toString();
  return version.isDouble() && version.toDouble() == 1.0 &&
         (backend == QStringLiteral("3dgs") || backend == QStringLiteral("2dgs")) &&
         nonnegativeInteger(payload.value(QStringLiteral("iteration")));
}

bool runtimeOptionsObserved(const QJsonObject &options) {
  if(options.value(QStringLiteral("version")).toDouble(-1)!=2.0) return false;
  const auto guard=options.value(QStringLiteral("resourceGuard")).toObject();
  const auto execution=options.value(QStringLiteral("execution")).toObject();
  const auto monitoring=options.value(QStringLiteral("monitoring")).toObject();
  const auto boundedInteger=[](const QJsonValue &value,double minimum,double maximum) {
    return nonnegativeInteger(value) && value.toDouble()>=minimum && value.toDouble()<=maximum;
  };
  if(!guard.value(QStringLiteral("enabled")).isBool() ||
      !QStringList{QStringLiteral("pause"),QStringLiteral("defer")}.contains(guard.value(QStringLiteral("mode")).toString()) ||
      !nonnegativeReal(guard.value(QStringLiteral("usage_fraction"))) ||
      guard.value(QStringLiteral("usage_fraction")).toDouble()<.5 ||
      guard.value(QStringLiteral("usage_fraction")).toDouble()>1.0 ||
      !boundedInteger(guard.value(QStringLiteral("minimum_spare_mib")),0,1048576) ||
      !boundedInteger(guard.value(QStringLiteral("max_consecutive_denials")),1,1000000)) return false;
  for(const auto &key:QStringList{QStringLiteral("image_staging"),QStringLiteral("camera_prefetch"),
      QStringLiteral("capacity_storage"),QStringLiteral("surfel_auxiliary")})
    if(!execution.value(key).isBool()) return false;
  return QStringList{QStringLiteral("auto"),QStringLiteral("stock")}.contains(execution.value(QStringLiteral("loss_mode")).toString()) &&
      QStringList{QStringLiteral("stock"),QStringLiteral("fastergs")}.contains(execution.value(QStringLiteral("engine")).toString()) &&
      boundedInteger(execution.value(QStringLiteral("gpu_slots")),1,2) &&
      boundedInteger(execution.value(QStringLiteral("pinned_mib")),16,1024) &&
      boundedInteger(execution.value(QStringLiteral("gpu_mib")),16,2048) &&
      boundedInteger(monitoring.value(QStringLiteral("profile_interval")),1,1000000);
}

bool persistentSafePath(const QString &path) {
  if (!QDir::isAbsolutePath(path) || QDir::fromNativeSeparators(path).split('/').contains("..")) return false;
  QFileInfo current(path);
  for (;;) {
    if (current.isSymbolicLink()) return false;
#ifdef Q_OS_WIN
    const DWORD attributes=GetFileAttributesW(reinterpret_cast<LPCWSTR>(current.absoluteFilePath().utf16()));
    if (attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
    const QString parent=current.absolutePath();
    if (parent==current.absoluteFilePath()) break;
    current.setFile(parent);
  }
  return true;
}

QJsonObject readPersistentObject(const QString &path,const qint64 maximum) {
  if (!persistentSafePath(path)) return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size()>maximum) return {};
  QJsonParseError error;
  const auto value=QJsonDocument::fromJson(file.readAll(),&error);
  return error.error==QJsonParseError::NoError && value.isObject()?value.object():QJsonObject{};
}

#ifdef Q_OS_WIN
QString persistentStartToken(const HANDLE process) {
  FILETIME creation{},exit{},kernel{},user{};
  if (!GetProcessTimes(process,&creation,&exit,&kernel,&user)) return {};
  const quint64 token=(quint64(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
  return QStringLiteral("win-filetime:")+QString::number(token);
}
#endif

bool persistentIdentityMayWrite(const QJsonObject &identity) {
#ifdef Q_OS_WIN
  const double pid = identity.value(QStringLiteral("pid")).toDouble(-1);
  const QString expected = identity.value(QStringLiteral("startToken")).toString();
  static const QRegularExpression tokenPattern(QStringLiteral("^win-filetime:[1-9][0-9]*$"));
  bool tokenParsed=false;
  const quint64 tokenValue=expected.mid(13).toULongLong(&tokenParsed);
  if (!std::isfinite(pid) || pid < 1 || pid > 4294967295.0 || std::floor(pid) != pid ||
      identity.size()!=2 || !tokenPattern.match(expected).hasMatch() || !tokenParsed || tokenValue==0) return true;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!process) return GetLastError() != ERROR_INVALID_PARAMETER;
  DWORD exitCode = 0;
  const bool readable = GetExitCodeProcess(process, &exitCode);
  const QString token = persistentStartToken(process);
  CloseHandle(process);
  return !readable || token.isEmpty() || (exitCode == STILL_ACTIVE && token == expected);
#else
  Q_UNUSED(identity);
  return true;
#endif
}

bool writePersistentObject(const QString &path, const QJsonObject &object) {
  QSaveFile file(path); file.setDirectWriteFallback(false);
  const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}

bool parseWorkerStatus(const QByteArray &payload, WorkerStatus *status) {
  const QByteArray jsonPayload = jsonObjectPrefix(payload);
  if (jsonPayload.isEmpty()) {
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(jsonPayload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return false;
  }

  const QJsonObject object = document.object();
  const QJsonValue versionValue = object.value(QStringLiteral("version"));
  const QJsonValue typeValue = object.value(QStringLiteral("type"));
  const QJsonValue stateValue = object.value(QStringLiteral("state"));
  const QJsonValue stageValue = object.value(QStringLiteral("stage"));
  if (!versionValue.isDouble() || versionValue.toDouble() != 1.0 ||
      !typeValue.isString() || typeValue.toString() != QStringLiteral("status") ||
      !stateValue.isString() || stateValue.toString().trimmed().isEmpty() ||
      !stageValue.isString() || stageValue.toString().trimmed().isEmpty()) {
    return false;
  }

  WorkerStatus parsedStatus;
  parsedStatus.state = stateValue.toString();
  parsedStatus.stage = stageValue.toString();

  const QJsonValue progressValue = object.value(QStringLiteral("progressPercent"));
  if (!progressValue.isUndefined()) {
    if (!progressValue.isDouble()) {
      return false;
    }
    const double progress = progressValue.toDouble();
    if (!std::isfinite(progress) || std::floor(progress) != progress || progress < 0.0 ||
        progress > 100.0) {
      return false;
    }
    parsedStatus.progressPercent = static_cast<int>(progress);
  }

  if (!parseOptionalInteger(object, QStringLiteral("iteration"),
                            &parsedStatus.iteration) ||
      !parseOptionalInteger(object, QStringLiteral("totalIterations"),
                            &parsedStatus.totalIterations, 1) ||
      !parseOptionalReal(object, QStringLiteral("loss"), &parsedStatus.loss) ||
      !parseOptionalReal(object, QStringLiteral("psnr"), &parsedStatus.psnr) ||
      !parseOptionalReal(object, QStringLiteral("iterationMilliseconds"),
                         &parsedStatus.iterationMilliseconds) ||
      !parseOptionalReal(object, QStringLiteral("elapsedSeconds"),
                         &parsedStatus.elapsedSeconds) ||
      !parseOptionalInteger(object, QStringLiteral("previewIteration"),
                            &parsedStatus.previewIteration) ||
      !parseOptionalInteger(object, QStringLiteral("densityGuardIteration"),
                            &parsedStatus.densityGuardIteration, 1) ||
      !parseOptionalInteger(object, QStringLiteral("densityGuardDeferred"),
                            &parsedStatus.densityGuardDeferred, 1) ||
      !parseOptionalInteger(object, QStringLiteral("reconstructionViews"), &parsedStatus.reconstructionViews) ||
      !parseOptionalInteger(object, QStringLiteral("reconstructionInputs"), &parsedStatus.reconstructionInputs) ||
      !parseOptionalInteger(object, QStringLiteral("reconstructionPoints"), &parsedStatus.reconstructionPoints)) {
    return false;
  }
  const QJsonValue gaussianCount =
      object.value(QStringLiteral("gaussianCount"));
  if (!gaussianCount.isUndefined()) {
    if (!gaussianCount.isDouble()) {
      return false;
    }
    const double count = gaussianCount.toDouble();
    if (!std::isfinite(count) || std::floor(count) != count || count < 0.0 ||
        count > static_cast<double>(std::numeric_limits<qint64>::max())) {
      return false;
    }
    parsedStatus.gaussianCount = static_cast<qint64>(count);
  }
  const QJsonValue previewPath = object.value(QStringLiteral("previewPath"));
  if (!previewPath.isUndefined()) {
    if (!previewPath.isString()) {
      return false;
    }
    parsedStatus.previewPath = previewPath.toString();
  }
  const QJsonValue previewKind = object.value(QStringLiteral("previewKind"));
  if (!previewKind.isUndefined()) {
    if (!previewKind.isString()) {
      return false;
    }
    parsedStatus.previewKind = previewKind.toString();
  }
  for (const QString &key : {QStringLiteral("reconstructionQuality"), QStringLiteral("generationIssue")}) {
    const auto value = object.value(key);
    if (value.isUndefined()) continue;
    const QStringList allowed = key == QStringLiteral("reconstructionQuality")
        ? QStringList{QStringLiteral("accepted"), QStringLiteral("partial"), QStringLiteral("repairing"), QStringLiteral("rejected")}
        : QStringList{QStringLiteral("reconstruction_quality"), QStringLiteral("source_frames_missing"),
                      QStringLiteral("training_renderer_resume"),QStringLiteral("training_cuda_oom"),
                      QStringLiteral("training_cuda_oom_reported"),QStringLiteral("training_exception"),
                      QStringLiteral("training_resource_pause")};
    if (!value.isString() || !allowed.contains(value.toString())) return false;
    if (key == QStringLiteral("reconstructionQuality")) parsedStatus.reconstructionQuality = value.toString();
    else parsedStatus.generationIssue = value.toString();
  }

  const auto delivery=object.value(QStringLiteral("trainingDelivery")).toObject();
  if (delivery.value(QStringLiteral("training_status"))==QStringLiteral("done") &&
      QStringList{QStringLiteral("running"),QStringLiteral("ready"),QStringLiteral("blocked"),
        QStringLiteral("failed"),QStringLiteral("cancelled")}.contains(delivery.value(QStringLiteral("export_status")).toString()))
    parsedStatus.trainingDelivery=delivery;
  // Summary metadata is optional for old workers. Reject malformed metadata
  // independently: a bad caption must never discard valid progress samples.
  const QJsonObject summary = object.value(QStringLiteral("trainingSummary")).toObject();
  const auto positiveInteger = [](const QJsonValue &value, double maximum) {
    const double number = value.toDouble(-1.0);
    return value.isDouble() && std::isfinite(number) && number >= 1.0 &&
           number <= maximum && std::floor(number) == number;
  };
  const auto boundedReal = [](const QJsonValue &value, double maximum) {
    const double number = value.toDouble(-1.0);
    return value.isDouble() && std::isfinite(number) && number >= 0.0 && number <= maximum;
  };
  const QString backend = summary.value(QStringLiteral("backend")).toString();
  const QString phase = summary.value(QStringLiteral("phase")).toString();
  const QString optimizer = summary.value(QStringLiteral("optimizer")).toString();
  const int resolution = summary.value(QStringLiteral("resolution")).toInt();
  bool summaryValid = summary.value(QStringLiteral("version")).toDouble(-1) == 1.0 &&
      (backend == QStringLiteral("3dgs") || backend == QStringLiteral("2dgs")) &&
      (phase == QStringLiteral("configured") || phase == QStringLiteral("loaded")) &&
      (optimizer == QStringLiteral("default") || optimizer == QStringLiteral("adam") ||
       optimizer == QStringLiteral("sparse_adam")) &&
      positiveInteger(summary.value(QStringLiteral("iterations")), 2147483647.0) &&
      positiveInteger(summary.value(QStringLiteral("resolution")), 16.0) && resolution >= 1 &&
      boundedReal(summary.value(QStringLiteral("densifyUntil")), 2147483647.0) &&
      std::floor(summary.value(QStringLiteral("densifyUntil")).toDouble()) == summary.value(QStringLiteral("densifyUntil")).toDouble() &&
      positiveInteger(summary.value(QStringLiteral("densificationInterval")), 2147483647.0) &&
      boundedReal(summary.value(QStringLiteral("densifyGradient")), 1.0);
  if (backend == QStringLiteral("3dgs")) {
    summaryValid = summaryValid && summary.value(QStringLiteral("antialiasing")).isBool() &&
        summary.value(QStringLiteral("exposureCompensation")).isBool();
  } else {
    summaryValid = summaryValid && boundedReal(summary.value(QStringLiteral("depthRatio")), 1.0);
  }
  if (phase == QStringLiteral("loaded")) {
    const QJsonArray dimensions = summary.value(QStringLiteral("trainDimensions")).toArray();
    summaryValid = summaryValid && !dimensions.isEmpty() && dimensions.size() <= 8 &&
        positiveInteger(summary.value(QStringLiteral("trainImageCount")), 2147483647.0) &&
        positiveInteger(summary.value(QStringLiteral("trainDimensionKinds")), 2147483647.0) &&
        summary.value(QStringLiteral("trainDimensionKinds")).toInt() >= dimensions.size() &&
        positiveInteger(summary.value(QStringLiteral("trainPixels")), 9007199254740991.0);
    qint64 listedImages = 0;
    qint64 listedPixels = 0;
    QPair<int, int> previousSize;
    for (const QJsonValue &value : dimensions) {
      const QJsonArray size = value.toArray();
      if (size.size() != 3 || !positiveInteger(size.at(0), 1000000.0) ||
          !positiveInteger(size.at(1), 1000000.0) || !positiveInteger(size.at(2), 2147483647.0)) {
        summaryValid = false;
        break;
      }
      const QPair<int, int> currentSize{size.at(0).toInt(), size.at(1).toInt()};
      const qint64 count = size.at(2).toInt();
      const qint64 pixels = qint64(currentSize.first) * currentSize.second;
      if (currentSize <= previousSize || pixels > 9007199254740991LL / count ||
          listedPixels > 9007199254740991LL - pixels * count) {
        summaryValid = false;
        break;
      }
      previousSize = currentSize;
      listedImages += count;
      listedPixels += pixels * count;
    }
    const qint64 totalImages = summary.value(QStringLiteral("trainImageCount")).toInteger();
    const qint64 totalPixels = summary.value(QStringLiteral("trainPixels")).toInteger();
    summaryValid = summaryValid && listedImages <= totalImages && listedPixels <= totalPixels;
    if (summary.value(QStringLiteral("trainDimensionKinds")).toInt() == dimensions.size())
      summaryValid = summaryValid && listedImages == totalImages && listedPixels == totalPixels;
  }
  if (summaryValid) parsedStatus.trainingSummary = summary;
  parsedStatus.trainingPerformance = normalizeTrainingPerformance(
      object.value(QStringLiteral("trainingPerformance")).toObject());
  parsedStatus.trainingGrowth = normalizeTrainingGrowth(
      object.value(QStringLiteral("trainingGrowth")).toObject());
  const auto runtime=object.value(QStringLiteral("runtimeConfiguration")).toObject();
  if(runtimeOptionsObserved(runtime.value(QStringLiteral("requested")).toObject()) &&
      runtimeOptionsObserved(runtime.value(QStringLiteral("effective")).toObject()) &&
      runtime.value(QStringLiteral("reasons")).isArray() && runtime.value(QStringLiteral("migration")).isObject() &&
      QJsonDocument(runtime).toJson(QJsonDocument::Compact).size()<=1024*1024)
    parsedStatus.runtimeConfiguration=runtime;
  const QString failure=object.value(QStringLiteral("failureReason")).toString();
  if(QStringList{QStringLiteral("cuda_oom"),QStringLiteral("cuda_oom_reported"),QStringLiteral("training_exception"),
      QStringLiteral("manual_pause"),QStringLiteral("resource_probe_unavailable"),
      QStringLiteral("resource_estimate_unavailable"),QStringLiteral("vram_pressure")}.contains(failure))
    parsedStatus.failureReason=failure;

  *status = std::move(parsedStatus);
  return true;
}

} // namespace

QJsonObject normalizeTrainingPerformance(const QJsonObject &payload) {
  if (!observerHeader(payload) || !nonnegativeReal(payload.value(QStringLiteral("wall_step_ms"))))
    return {};
  QJsonObject result;
  for (const QString &key : {QStringLiteral("version"), QStringLiteral("backend"),
                            QStringLiteral("iteration"), QStringLiteral("wall_step_ms")})
    result.insert(key, payload.value(key));
  const QStringList stages{QStringLiteral("setup"), QStringLiteral("transfer"),
      QStringLiteral("render"), QStringLiteral("loss"), QStringLiteral("backward"),
      QStringLiteral("densification"), QStringLiteral("optimizer"), QStringLiteral("preview"),
      QStringLiteral("checkpoint"), QStringLiteral("report")};
  for (const QString &key : {QStringLiteral("stages_ms"), QStringLiteral("gpu_stages_ms")}) {
    const auto value = payload.value(key);
    if (!value.isUndefined() && !value.isObject()) return {};
    const QJsonObject timings = value.toObject();
    for (auto it = timings.begin(); it != timings.end(); ++it)
      if (!stages.contains(it.key()) || !nonnegativeReal(it.value())) return {};
    result.insert(key, timings);
  }
  for (const QString &key : {QStringLiteral("torch_allocated_bytes"),
      QStringLiteral("torch_reserved_bytes"), QStringLiteral("torch_peak_allocated_bytes"),
      QStringLiteral("device_free_bytes"), QStringLiteral("device_total_bytes")}) {
    const auto value = payload.value(key);
    if (value.isUndefined()) continue;
    if (!nonnegativeInteger(value)) return {};
    result.insert(key, value);
  }
  for (const QString &key : {QStringLiteral("renderer_engine"), QStringLiteral("loss_engine")}) {
    const auto value = payload.value(key);
    if (value.isUndefined()) continue;
    const QStringList engines = key == QStringLiteral("renderer_engine")
        ? QStringList{QStringLiteral("stock"), QStringLiteral("fastergs")}
        : QStringList{QStringLiteral("stock"), QStringLiteral("fused_l1_ssim")};
    if (!value.isString() || !engines.contains(value.toString())) return {};
    result.insert(key, value);
  }
  if (result.contains(QStringLiteral("device_free_bytes")) &&
      result.contains(QStringLiteral("device_total_bytes")) &&
      result.value(QStringLiteral("device_free_bytes")).toDouble() >
          result.value(QStringLiteral("device_total_bytes")).toDouble()) return {};
  return result;
}

QJsonObject normalizeTrainingGrowth(const QJsonObject &payload) {
  if (!observerHeader(payload)) return {};
  const QString state = payload.value(QStringLiteral("state")).toString();
  const QString reason = payload.value(QStringLiteral("reason")).toString();
  const bool admitted = state == QStringLiteral("admitted");
  const bool bypassed=state==QStringLiteral("bypassed");
  if ((!admitted && !bypassed && state != QStringLiteral("deferred")) ||
      !QStringList{QStringLiteral("admitted"), QStringLiteral("vram_pressure"),
          QStringLiteral("probe_unavailable"), QStringLiteral("invalid_observation"),
          QStringLiteral("estimate_unavailable"),QStringLiteral("guard_disabled")}.contains(reason) ||
      admitted != (reason == QStringLiteral("admitted")) ||
      bypassed != (reason==QStringLiteral("guard_disabled"))) return {};
  QJsonObject result;
  for (const QString &key : {QStringLiteral("version"), QStringLiteral("backend"),
      QStringLiteral("iteration"), QStringLiteral("state"), QStringLiteral("reason")})
    result.insert(key, payload.value(key));
  for (const QString &key : {QStringLiteral("point_count"), QStringLiteral("transient_point_count"),
      QStringLiteral("final_point_count"), QStringLiteral("parameter_bytes"),
      QStringLiteral("optimizer_state_bytes"), QStringLiteral("additional_peak_bytes"),
      QStringLiteral("free_bytes"), QStringLiteral("total_bytes"), QStringLiteral("spare_bytes"),
      QStringLiteral("available_bytes"), QStringLiteral("cache_credit_bytes"),
      QStringLiteral("clone_count"), QStringLiteral("split_count")}) {
    if (!nonnegativeInteger(payload.value(key))) return {};
    if(bypassed && payload.value(key).toDouble()!=0.0) return {};
    result.insert(key, payload.value(key));
  }
  for (const QString &key : {QStringLiteral("allowed"), QStringLiteral("observation_valid"),
                            QStringLiteral("candidate_counts_known"),QStringLiteral("guard_enabled"),
                            QStringLiteral("cache_reclaim_attempted")}) {
    const auto value = payload.value(key);
    if (value.isUndefined()) continue;
    if (!value.isBool()) return {};
    result.insert(key, value);
  }
  if (result.contains(QStringLiteral("allowed")) &&
      result.value(QStringLiteral("allowed")).toBool() != (admitted || bypassed)) return {};
  if(bypassed && (!result.value(QStringLiteral("guard_enabled")).isBool() ||
      result.value(QStringLiteral("guard_enabled")).toBool() ||
      !result.value(QStringLiteral("observation_valid")).isBool() ||
      result.value(QStringLiteral("observation_valid")).toBool())) return {};
  for(const auto &key:QStringList{QStringLiteral("device_free_before_reclaim_bytes"),
      QStringLiteral("torch_allocated_bytes"),QStringLiteral("torch_reserved_bytes"),
      QStringLiteral("torch_active_bytes"),QStringLiteral("torch_inactive_split_bytes")}) {
    const auto value=payload.value(key);
    if(value.isUndefined()) continue;
    if(!value.isNull() && !nonnegativeInteger(value)) return {};
    result.insert(key,value);
  }
  for(const auto &key:QStringList{QStringLiteral("cache_reclaim_status"),QStringLiteral("allocator_observation_status")}) {
    const auto value=payload.value(key);
    if(value.isUndefined()) continue;
    const QStringList allowed=key==QStringLiteral("cache_reclaim_status")
        ?QStringList{QStringLiteral("not_attempted"),QStringLiteral("reclaimed_remeasured")}
        :QStringList{QStringLiteral("unreported"),QStringLiteral("known"),QStringLiteral("partial"),QStringLiteral("invalid"),QStringLiteral("unavailable")};
    if(!value.isString() || !allowed.contains(value.toString())) return {};
    result.insert(key,value);
  }
  if (admitted && result.value(QStringLiteral("observation_valid")).isBool() &&
      !result.value(QStringLiteral("observation_valid")).toBool()) return {};
  if (result.value(QStringLiteral("observation_valid")).toBool()) {
    const double total = result.value(QStringLiteral("total_bytes")).toDouble();
    const double free = result.value(QStringLiteral("free_bytes")).toDouble();
    if (total <= 0.0 || free > total ||
        result.value(QStringLiteral("cache_credit_bytes")).toDouble() > total - free) return {};
  }
  return result;
}

std::optional<WorkerStatus> ProcessSupervisor::decodeStatusEvent(const QByteArray &payload) {
  WorkerStatus status;
  if (!parseWorkerStatus(payload, &status)) return std::nullopt;
  return status;
}

bool ProcessSupervisor::hasUnfinishedTraining(const QString &jobStore) {
  const QDir store(jobStore);
  if (!store.exists()) return false;
  if (!persistentSafePath(store.absolutePath())) return true;
  const QString owners = store.filePath(QStringLiteral(".p"));
  if (!persistentSafePath(owners)) return true;
  const auto launches = QDir(owners).entryInfoList({QStringLiteral("*.json")},QDir::Files);
  if (launches.size() > 4096) return true;
  for (const auto &file : launches) {
    const auto owner = readPersistentObject(file.absoluteFilePath(),65536);
    if (owner.value(QStringLiteral("version")).toInt() != 1) return true;
    if (!owner.value(QStringLiteral("closed")).toBool(false) &&
        persistentIdentityMayWrite(owner.value(QStringLiteral("workerIdentity")).toObject())) return true;
  }
  const auto files = store.entryInfoList({QStringLiteral("*.json")}, QDir::Files | QDir::NoDotAndDotDot);
  if (files.size() > 4096) return true;
  const QRegularExpression id(QStringLiteral("^[0-9a-f]{32}\\.json$"));
  for (const auto &file : files) {
    if (!id.match(file.fileName()).hasMatch()) continue;
    const auto job = readPersistentObject(file.absoluteFilePath(), 16 * 1024 * 1024);
    if (job.isEmpty()) return true;
    const QString state = job.value(QStringLiteral("status")).toString();
    if (!QStringList{QStringLiteral("queued"),QStringLiteral("running"),QStringLiteral("cancelling")}.contains(state)) continue;
    const auto control = job.value(QStringLiteral("native_control")).toObject();
    const QString session = control.value(QStringLiteral("session")).toString();
    if (!QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(session).hasMatch()) return true;
    const auto lease = readPersistentObject(store.filePath(QStringLiteral(".c/") + session + QStringLiteral(".json")), 65536);
    if (lease.value(QStringLiteral("jobId")) != job.value(QStringLiteral("id")) ||
        lease.value(QStringLiteral("session")) != session ||
        lease.value(QStringLiteral("output")) != control.value(QStringLiteral("output"))) return true;
    if (lease.value(QStringLiteral("closed")).toBool(false)) continue;
    if (persistentIdentityMayWrite(lease.value(QStringLiteral("workerIdentity")).toObject())) return true;
  }
  return false;
}

ProcessSupervisor::ProcessSupervisor(QObject *parent) : QObject(parent) {
  qRegisterMetaType<TrainingGpuPreviewDescriptor>();
  mProcess.setProcessChannelMode(QProcess::MergedChannels);
  mPersistentPoll.setInterval(100);
  connect(&mPersistentPoll,&QTimer::timeout,this,&ProcessSupervisor::pollPersistentTraining);

  connect(&mProcess, &QProcess::readyReadStandardOutput, this, &ProcessSupervisor::drainOutput);
  connect(&mProcess, &QProcess::started, this, [this]() {
    if (!attachProcessToJob()) {
      emit outputReady(
          QCoreApplication::translate("Workbench", "Warning: the task could not be attached to the shutdown job; "
             "process-tree cleanup will use the PID fallback.\n"));
    }
    emit taskStarted(mActiveTask);
    emit runningChanged(true);
  });
  connect(&mProcess, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      emit outputReady(QCoreApplication::translate("Workbench", "Failed to start process: %1\n").arg(mProcess.errorString()));
      const QString failedTask = mActiveTask;
      mActiveTask.clear();
      mAcceptsCancelCommand = false;
      terminateAndReleaseProcessJob();
      emit taskFinished(failedTask, -1, false);
      emit runningChanged(false);
    }
  });
  connect(&mProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
            drainOutput();
            processBufferedOutput(true);
            const QString finishedTask = mActiveTask;
            const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
            mActiveTask.clear();
            mAcceptsCancelCommand = false;
            terminateAndReleaseProcessJob();
            emit taskFinished(finishedTask, exitCode, succeeded);
            emit runningChanged(false);
          });
}

ProcessSupervisor::~ProcessSupervisor() {
  shutdown();
}

bool ProcessSupervisor::isRunning() const { return mPersistentTraining || mProcess.state() != QProcess::NotRunning; }
bool ProcessSupervisor::isPersistentTraining() const { return mPersistentTraining; }
QString ProcessSupervisor::activeTask() const { return mActiveTask; }
bool ProcessSupervisor::wasStopRequested() const { return mStopRequested; }

bool ProcessSupervisor::start(const QString &taskName, const QString &program,
                              const QStringList &arguments, const QString &workingDirectory,
                              const QProcessEnvironment &environment,
                              const bool acceptsCancelCommand) {
  if (mShutdownStarted || isRunning() || program.isEmpty()) {
    return false;
  }

  mOutputBuffer.clear();
  mStopRequested = false;
  mActiveTask = taskName;
  prepareProcessJob();
  if (!workingDirectory.isEmpty()) {
    mProcess.setWorkingDirectory(workingDirectory);
  }
  mAcceptsCancelCommand = acceptsCancelCommand;
  mProcess.setProcessEnvironment(environment.isEmpty()
                                     ? QProcessEnvironment::systemEnvironment()
                                     : environment);
  emit taskLaunchRequested(mActiveTask, mProcess.workingDirectory());
  mProcess.start(program, arguments,
                 acceptsCancelCommand ? QIODevice::ReadWrite : QIODevice::ReadOnly);
  return true;
}

bool ProcessSupervisor::startPersistentTraining(const QString &taskName,const QString &program,
    const QStringList &arguments,const QString &workingDirectory,
    const QProcessEnvironment &environment,const QString &logPath,const QString &jobStore) {
  if(mShutdownStarted || isRunning() || program.isEmpty()) return false;
  if(!persistentSafePath(logPath) || !persistentSafePath(jobStore) ||
      !QFileInfo(logPath).dir().exists()) {
    emit outputReady(QCoreApplication::translate("Workbench","Persistent training could not be started: %1\n")
        .arg(QStringLiteral("persistent_training_path_invalid")));
    return false;
  }
#ifndef Q_OS_WIN
  Q_UNUSED(arguments); Q_UNUSED(workingDirectory); Q_UNUSED(environment);
  emit outputReady(QCoreApplication::translate("Workbench","Persistent training could not be started: %1\n")
      .arg(QStringLiteral("persistent_training_windows_only")));
  return false;
#else
  QFile reserve(logPath);
  if(!reserve.open(QIODevice::WriteOnly|QIODevice::NewOnly)) {
    emit outputReady(QCoreApplication::translate("Workbench","Persistent training could not be started: %1\n")
        .arg(reserve.errorString()));
    return false;
  }
  reserve.close();
  // Publish the launch ownership before the detached process can write. The
  // initial unknown identity deliberately blocks relocation if the UI crashes
  // during CreateProcess; it is never inferred to be a completed training job.
  const QString ownerRoot=QDir(jobStore).filePath(QStringLiteral(".p"));
  if(!persistentSafePath(ownerRoot) || !QDir().mkpath(ownerRoot)) return false;
  const QString ownerPath=QDir(ownerRoot).filePath(QUuid::createUuid().toString(QUuid::Id128)+QStringLiteral(".json"));
  QJsonObject owner{{QStringLiteral("version"),1},{QStringLiteral("closed"),false},
      {QStringLiteral("workerIdentity"),QJsonObject{}}};
  if(!writePersistentObject(ownerPath,owner)) return false;
  mActiveTask=taskName; mOutputBuffer.clear(); mStopRequested=false;
  mPersistentFinalState.clear(); mPersistentJobStore=QDir::cleanPath(jobStore);
  QProcess launcher;
  launcher.setProgram(program); launcher.setArguments(arguments);
  launcher.setWorkingDirectory(workingDirectory);
  launcher.setProcessEnvironment(environment.isEmpty()?QProcessEnvironment::systemEnvironment():environment);
  launcher.setProcessChannelMode(QProcess::MergedChannels);
  launcher.setStandardOutputFile(logPath,QIODevice::Append);
  launcher.setStandardInputFile(QProcess::nullDevice());
  launcher.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *value) {
    value->flags |= CREATE_NO_WINDOW;
    value->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
    value->startupInfo->wShowWindow=SW_HIDE;
  });
  emit taskLaunchRequested(taskName,workingDirectory);
  qint64 pid=0;
  if(!launcher.startDetached(&pid)) {
    owner.insert(QStringLiteral("closed"),true);
    writePersistentObject(ownerPath,owner);
    const QString error=launcher.errorString();
    mActiveTask.clear();
    emit outputReady(QCoreApplication::translate("Workbench","Persistent training could not be started: %1\n").arg(error));
    QTimer::singleShot(0,this,[this,taskName] { emit taskFinished(taskName,-1,false); emit runningChanged(false); });
    return true;
  }
  mPersistentProcessId=pid;
  const HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,static_cast<DWORD>(pid));
  mPersistentQueryHandle=reinterpret_cast<quintptr>(handle);
  mPersistentStartToken=handle?persistentStartToken(handle):QString{};
  owner.insert(QStringLiteral("workerIdentity"),QJsonObject{
      {QStringLiteral("pid"),pid},{QStringLiteral("startToken"),mPersistentStartToken}});
  writePersistentObject(ownerPath,owner);
  mPersistentLog.setFileName(logPath);
  mPersistentLog.open(QIODevice::ReadOnly);
  mPersistentTraining=true;
  emit taskStarted(taskName); emit runningChanged(true);
  mPersistentPoll.start();
  return true;
#endif
}

std::optional<QJsonObject> ProcessSupervisor::persistentConnection() const {
#ifndef Q_OS_WIN
  return std::nullopt;
#else
  if(!mPersistentTraining || !mPersistentQueryHandle || mPersistentStartToken.isEmpty() ||
      !persistentSafePath(mPersistentJobStore)) return std::nullopt;
  DWORD exitCode=0;
  const auto handle=reinterpret_cast<HANDLE>(mPersistentQueryHandle);
  if(!GetExitCodeProcess(handle,&exitCode) || exitCode!=STILL_ACTIVE ||
      persistentStartToken(handle)!=mPersistentStartToken) return std::nullopt;
  const QString connectionRoot=QDir(mPersistentJobStore).filePath(QStringLiteral(".c"));
  if(!persistentSafePath(connectionRoot)) return std::nullopt;
  const auto files=QDir(connectionRoot).entryInfoList({QStringLiteral("*.json")},QDir::Files,QDir::Name);
  if(files.size()>4096) return std::nullopt;
  const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{32}$"));
  for(const auto &file:files) {
    if(!idPattern.match(file.completeBaseName()).hasMatch()) continue;
    const auto lease=readPersistentObject(file.absoluteFilePath(),64*1024);
    const auto identity=lease.value(QStringLiteral("workerIdentity")).toObject();
    if(identity.size()!=2 || identity.value(QStringLiteral("pid")).toInteger(-1)!=mPersistentProcessId ||
        identity.value(QStringLiteral("startToken")).toString()!=mPersistentStartToken) continue;
    const QString id=lease.value(QStringLiteral("jobId")).toString();
    const QString session=lease.value(QStringLiteral("session")).toString();
    const QString backend=lease.value(QStringLiteral("backend")).toString();
    const auto heartbeat=lease.value(QStringLiteral("heartbeat"));
    const double age=QDateTime::currentMSecsSinceEpoch()/1000.0-heartbeat.toDouble();
    if(lease.value(QStringLiteral("version")).toDouble()!=1.0 || !idPattern.match(id).hasMatch() ||
        !idPattern.match(session).hasMatch() || session!=file.completeBaseName() ||
        (backend!=QStringLiteral("3dgs") && backend!=QStringLiteral("2dgs")) ||
        !lease.value(QStringLiteral("closed")).isBool() || lease.value(QStringLiteral("closed")).toBool() ||
        !heartbeat.isDouble() || !std::isfinite(age) || age< -2.0 || age>15.0) continue;
    const auto job=readPersistentObject(QDir(mPersistentJobStore).filePath(id+QStringLiteral(".json")),16*1024*1024);
    const auto control=job.value(QStringLiteral("native_control")).toObject();
    const auto output=control.value(QStringLiteral("output"));
    if(job.value(QStringLiteral("id")).toString()!=id || job.value(QStringLiteral("backend")).toString()!=backend ||
        control.value(QStringLiteral("session")).toString()!=session ||
        control.value(QStringLiteral("backend")).toString()!=backend ||
        !output.isString() || output.toString().isEmpty() || !persistentSafePath(output.toString()) ||
        control.value(QStringLiteral("output"))!=lease.value(QStringLiteral("output")) ||
        !QStringList{QStringLiteral("queued"),QStringLiteral("running"),QStringLiteral("cancelling")}
             .contains(job.value(QStringLiteral("status")).toString())) continue;
    return QJsonObject{{QStringLiteral("jobId"),id},{QStringLiteral("session"),session},
        {QStringLiteral("workerIdentity"),identity},{QStringLiteral("connectionRoot"),connectionRoot}};
  }
  return std::nullopt;
#endif
}

bool ProcessSupervisor::requestPersistentCommand(const QString &command) {
  if(mShutdownStarted || !mPersistentTraining || mStopRequested) return false;
  const auto connection=persistentConnection();
  QString error=QStringLiteral("job_connection_control_unavailable");
#ifdef Q_OS_WIN
  if(connection) {
    const QString root=connection->value(QStringLiteral("connectionRoot")).toString();
    const QString session=connection->value(QStringLiteral("session")).toString();
    const QString target=QDir(root).filePath(session+QStringLiteral("-cmd.json"));
    const QString temporary=QDir(root).filePath(QUuid::createUuid().toString(QUuid::Id128));
    const QByteArray bytes=QJsonDocument(QJsonObject{{QStringLiteral("version"),1},
        {QStringLiteral("jobId"),connection->value(QStringLiteral("jobId"))},
        {QStringLiteral("session"),session},
        {QStringLiteral("workerIdentity"),connection->value(QStringLiteral("workerIdentity"))},
        {QStringLiteral("command"),command},
        {QStringLiteral("nonce"),QUuid::createUuid().toString(QUuid::Id128)},
        {QStringLiteral("requestedAt"),QDateTime::currentMSecsSinceEpoch()/1000.0}}).toJson(QJsonDocument::Compact);
    if(persistentSafePath(target) && persistentSafePath(temporary) && bytes.size()<=4096) {
      const HANDLE file=CreateFileW(reinterpret_cast<LPCWSTR>(temporary.utf16()),GENERIC_WRITE,0,nullptr,CREATE_NEW,
          FILE_ATTRIBUTE_NORMAL,nullptr);
      if(file!=INVALID_HANDLE_VALUE) {
        DWORD written=0;
        const bool durable=WriteFile(file,bytes.constData(),static_cast<DWORD>(bytes.size()),&written,nullptr) &&
            written==static_cast<DWORD>(bytes.size()) && FlushFileBuffers(file);
        CloseHandle(file);
        const bool published=durable && CreateHardLinkW(reinterpret_cast<LPCWSTR>(target.utf16()),
            reinterpret_cast<LPCWSTR>(temporary.utf16()),nullptr);
        if(!published) error=QStringLiteral("job_connection_command_pending_or_unavailable");
        QFile::remove(temporary);
        if(published) {
          emit outputReady(command==QStringLiteral("pause")
              ?QCoreApplication::translate("Workbench","Training pause requested; waiting for the worker to acknowledge it.\n")
              :QCoreApplication::translate("Workbench","Training stop requested; waiting for the worker to acknowledge it.\n"));
          return true;
        }
      }
    }
  }
#else
  Q_UNUSED(command); Q_UNUSED(connection);
#endif
  emit outputReady(QCoreApplication::translate("Workbench","Persistent training command was not sent: %1\n").arg(error));
  return false;
}

void ProcessSupervisor::pollPersistentTraining() {
  if(!mPersistentTraining) return;
  if(!mPersistentLog.isOpen()) mPersistentLog.open(QIODevice::ReadOnly);
  if(mPersistentLog.isOpen()) {
    const auto bytes=mPersistentLog.read(1024*1024);
    if(!bytes.isEmpty()) {
      mOutputBuffer.append(bytes); processBufferedOutput(false);
      // The complete output remains on disk. Bound a malformed unterminated
      // display line without changing worker or training state.
      if(mOutputBuffer.size()>2*1024*1024) processBufferedOutput(true);
    }
  }
  int exitCode=-1;
#ifdef Q_OS_WIN
  if(!mPersistentQueryHandle) {
    const auto handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,
        static_cast<DWORD>(mPersistentProcessId));
    if(handle) {
      const auto token=persistentStartToken(handle);
      if(token.isEmpty() || (!mPersistentStartToken.isEmpty() && token!=mPersistentStartToken)) {
        CloseHandle(handle);
        return; // Unverified identity is not a completed job.
      }
      mPersistentStartToken=token;
      mPersistentQueryHandle=reinterpret_cast<quintptr>(handle);
    } else if(GetLastError()!=ERROR_INVALID_PARAMETER) return;
  }
  if(mPersistentQueryHandle) {
    DWORD code=0;
    if(!GetExitCodeProcess(reinterpret_cast<HANDLE>(mPersistentQueryHandle),&code)) return;
    if(code==STILL_ACTIVE) return;
    exitCode=static_cast<int>(code);
  }
#endif
  if(mPersistentLog.isOpen() && !mPersistentLog.atEnd()) return;
  processBufferedOutput(true);
  const QString task=mActiveTask;
  const bool succeeded=exitCode==0 && mPersistentFinalState!=QStringLiteral("failed") &&
      mPersistentFinalState!=QStringLiteral("cancelled");
  if(mPersistentFinalState.isEmpty() && exitCode!=0)
    emit outputReady(QCoreApplication::translate("Workbench","Training worker ended; its final status is unavailable.\n"));
  detachPersistentTraining();
  emit taskFinished(task,exitCode,succeeded); emit runningChanged(false);
}

void ProcessSupervisor::detachPersistentTraining() {
  mPersistentPoll.stop();
#ifdef Q_OS_WIN
  if(mPersistentQueryHandle) CloseHandle(reinterpret_cast<HANDLE>(mPersistentQueryHandle));
#endif
  mPersistentQueryHandle=0; mPersistentTraining=false; mPersistentProcessId=0;
  mPersistentStartToken.clear(); mPersistentLog.close(); mActiveTask.clear(); mOutputBuffer.clear();
}

bool ProcessSupervisor::requestPause() {
  if (mPersistentTraining) return requestPersistentCommand(QStringLiteral("pause"));
  return isRunning() && mAcceptsCancelCommand && !mStopRequested &&
         mProcess.write("pause\n") == 6;
}

void ProcessSupervisor::stop() {
  if (mShutdownStarted || !isRunning()) {
    return;
  }
  if (mPersistentTraining) {
    if (requestPersistentCommand(QStringLiteral("stop"))) mStopRequested=true;
    return;
  }

  mStopRequested = true;
  const qint64 processId = mProcess.processId();
  if (mAcceptsCancelCommand) {
    emit outputReady(QCoreApplication::translate("Workbench", "Requesting graceful task cancellation...\n"));
    mProcess.write("cancel\n");
    mProcess.waitForBytesWritten(500);
  } else {
    emit outputReady(QCoreApplication::translate("Workbench", "Terminating the task process tree...\n"));
#ifdef Q_OS_WIN
    terminateAndReleaseProcessJob();
    if (mProcess.state() != QProcess::NotRunning) {
      mProcess.waitForFinished(1000);
    }
    if (isRunning()) {
      QProcess::execute(QStringLiteral("taskkill.exe"),
                        {QStringLiteral("/PID"), QString::number(processId),
                         QStringLiteral("/T"), QStringLiteral("/F")});
    }
#else
    mProcess.terminate();
#endif
    if (isRunning() && mProcess.processId() == processId) {
      mProcess.kill();
    }
    return;
  }

  QTimer::singleShot(6500, this, [this, processId]() {
    if (!isRunning() || mProcess.processId() != processId) {
      return;
    }
    emit outputReady(QCoreApplication::translate("Workbench", "Task did not stop in time; terminating its process tree.\n"));
#ifdef Q_OS_WIN
    terminateAndReleaseProcessJob();
    if (mProcess.state() != QProcess::NotRunning) {
      mProcess.waitForFinished(1000);
    }
    if (isRunning()) {
      QProcess::execute(QStringLiteral("taskkill.exe"),
                        {QStringLiteral("/PID"), QString::number(processId),
                         QStringLiteral("/T"), QStringLiteral("/F")});
    }
#endif
    if (isRunning() && mProcess.processId() == processId) {
      mProcess.kill();
    }
  });
}

void ProcessSupervisor::shutdown() {
  if (mShutdownStarted) {
    return;
  }
  mShutdownStarted = true;
  if (mPersistentTraining) {
    detachPersistentTraining();
    return;
  }
  mStopRequested = mStopRequested || isRunning();

  if (!isRunning()) {
    terminateAndReleaseProcessJob();
    return;
  }

  const qint64 processId = mProcess.processId();
  disconnect(&mProcess, nullptr, this, nullptr);
  if (mAcceptsCancelCommand) {
    mProcess.write("cancel\n");
    mProcess.waitForBytesWritten(500);
  }

#ifdef Q_OS_WIN
  terminateAndReleaseProcessJob();
  if (mProcess.state() != QProcess::NotRunning) {
    mProcess.waitForFinished(1000);
  }
  if (mProcess.state() != QProcess::NotRunning && processId > 0) {
    QProcess::execute(QStringLiteral("taskkill.exe"),
                      {QStringLiteral("/PID"), QString::number(processId),
                       QStringLiteral("/T"), QStringLiteral("/F")});
  }
#else
  mProcess.terminate();
#endif

  if (mProcess.state() != QProcess::NotRunning &&
      !mProcess.waitForFinished(3000)) {
    mProcess.kill();
    mProcess.waitForFinished(2000);
  }
  mActiveTask.clear();
  mAcceptsCancelCommand = false;
  mOutputBuffer.clear();
}

void ProcessSupervisor::prepareProcessJob() {
#ifdef Q_OS_WIN
  terminateAndReleaseProcessJob();
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    return;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits))) {
    CloseHandle(job);
    return;
  }
  mProcessJobHandle = reinterpret_cast<quintptr>(job);
#endif
}

bool ProcessSupervisor::attachProcessToJob() {
#ifndef Q_OS_WIN
  return true;
#else
  if (mProcessJobHandle == 0 || mProcess.processId() <= 0) {
    return false;
  }

  HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                               static_cast<DWORD>(mProcess.processId()));
  if (process == nullptr) {
    terminateAndReleaseProcessJob();
    return false;
  }
  const BOOL assigned = AssignProcessToJobObject(
      reinterpret_cast<HANDLE>(mProcessJobHandle), process);
  CloseHandle(process);
  if (!assigned) {
    terminateAndReleaseProcessJob();
    return false;
  }
  return true;
#endif
}

void ProcessSupervisor::terminateAndReleaseProcessJob() {
#ifdef Q_OS_WIN
  if (mProcessJobHandle == 0) {
    return;
  }
  HANDLE job = reinterpret_cast<HANDLE>(mProcessJobHandle);
  mProcessJobHandle = 0;
  TerminateJobObject(job, ERROR_CANCELLED);
  WaitForSingleObject(job, 3000);
  CloseHandle(job);
#endif
}

void ProcessSupervisor::drainOutput() {
  const QByteArray bytes = mProcess.readAllStandardOutput();
  if (bytes.isEmpty()) {
    return;
  }

  mOutputBuffer.append(bytes);
  processBufferedOutput(false);
}

void ProcessSupervisor::processBufferedOutput(const bool flushTail) {
  qsizetype newlineIndex = mOutputBuffer.indexOf('\n');
  while (newlineIndex >= 0) {
    const qsizetype lineLength = newlineIndex + 1;
    const QByteArray line = mOutputBuffer.first(lineLength);
    mOutputBuffer.remove(0, lineLength);
    handleOutputLine(line);
    newlineIndex = mOutputBuffer.indexOf('\n');
  }

  if (flushTail && !mOutputBuffer.isEmpty()) {
    const QByteArray tail = std::exchange(mOutputBuffer, {});
    handleOutputLine(tail);
  }
}

void ProcessSupervisor::handleOutputLine(const QByteArray &line) {
  QByteArray content = line;
  if (content.endsWith('\n')) {
    content.chop(1);
  }
  if (content.endsWith('\r')) {
    content.chop(1);
  }

  if (content.startsWith(kWorkerEventPrefix)) {
    const QByteArray payload = content.mid(kWorkerEventPrefix.size());
    if (const auto status = decodeStatusEvent(payload)) {
      if(mPersistentTraining && QStringList{QStringLiteral("done"),QStringLiteral("failed"),
          QStringLiteral("paused"),QStringLiteral("cancelled")}.contains(status->state))
        mPersistentFinalState=status->state;
      emit workerStatusReady(*status);
      return;
    }
  }

  if (content.startsWith(kTrainingGpuPreviewPrefix)) {
    TrainingGpuPreviewDescriptor descriptor;
    const QByteArray payload = jsonObjectPrefix(
        content.mid(kTrainingGpuPreviewPrefix.size()));
    if (parseTrainingGpuPreviewDescriptor(payload, &descriptor)) {
      emit trainingGpuPreviewReady(descriptor);
      return;
    }
  }

  emit outputReady(QString::fromUtf8(line));
}

} // namespace gsw

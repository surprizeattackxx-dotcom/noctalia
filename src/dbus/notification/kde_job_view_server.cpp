#include "dbus/notification/kde_job_view_server.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "net/uri.h"
#include "notification/job_progress.h"
#include "notification/notification_manager.h"
#include "system/process.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

namespace {
  constexpr Logger kLog("kde-job");

  const sdbus::ServiceName kBusName{"org.kde.kuiserver"};
  // KF6's KUiServerV2JobTracker sends requestView to this name, so both must be owned.
  const sdbus::ServiceName kJobViewBusName{"org.kde.JobViewServer"};
  const sdbus::ObjectPath kServerPath{"/JobViewServer"};
  constexpr auto kJobViewV3 = "org.kde.JobViewV3";
  constexpr auto kJobViewV2 = "org.kde.JobViewV2";
  constexpr auto kCancelAction = "kde-job-cancel";
  constexpr auto kPauseAction = "kde-job-pause";
  constexpr auto kResumeAction = "kde-job-resume";
  constexpr int kCapabilityCancel = 0x1;       // KJob::Killable
  constexpr int kCapabilitySuspend = 0x2;      // KJob::Suspendable
  constexpr std::uint32_t kKilledJobError = 1; // KJob::KilledJobError: the job was cancelled
  constexpr auto kShowDelay = std::chrono::milliseconds(1000);
  constexpr auto kTick = std::chrono::milliseconds(500);

  std::optional<std::string> asString(const sdbus::Variant& value) {
    try {
      if (value.containsValueOfType<std::string>()) {
        return value.get<std::string>();
      }
    } catch (const sdbus::Error&) {
    }
    return std::nullopt;
  }

  std::optional<bool> asBool(const sdbus::Variant& value) {
    try {
      if (value.containsValueOfType<bool>()) {
        return value.get<bool>();
      }
    } catch (const sdbus::Error&) {
    }
    return std::nullopt;
  }

  std::string humanBytes(std::uint64_t bytes) {
    constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
      value /= 1024.0;
      ++unit;
    }
    return unit == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", value, units[unit]);
  }

  // file:///home/me/a%20b.iso -> ~/a b.iso; anything else is returned as given.
  std::string displayLocation(const std::string& location) {
    std::string path = location;
    if (location.starts_with("file:")) {
      path = uri::normalizeFileUrl(location);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      const std::string_view prefix(home);
      if (path == prefix) {
        return "~";
      }
      if (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/') {
        path = "~" + path.substr(prefix.size());
      }
    }
    return path;
  }

  std::string baseName(const std::string& location) {
    std::string path = location;
    while (path.size() > 1 && path.back() == '/') {
      path.pop_back();
    }
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  std::string parentOf(const std::string& displayPath) {
    const auto slash = displayPath.rfind('/');
    return slash == std::string::npos || slash == 0 ? displayPath : displayPath.substr(0, slash);
  }

  // The local folder a finished job wrote into: the destination itself when it is a directory, else its parent.
  // Empty for remote destinations. Only ever a directory, so "Open folder" cannot launch a file.
  std::string localFolderOf(const std::string& location) {
    std::string path;
    if (location.starts_with("file:")) {
      path = uri::normalizeFileUrl(location);
    } else if (location.starts_with('/')) {
      path = location;
    }
    if (path.empty()) {
      return {};
    }
    std::error_code ec;
    std::filesystem::path candidate{path};
    if (!std::filesystem::is_directory(candidate, ec)) {
      candidate = candidate.parent_path();
    }
    return std::filesystem::is_directory(candidate, ec) ? candidate.string() : std::string{};
  }

  std::string etaText(std::uint64_t seconds) {
    if (seconds < 60) {
      return i18n::tr(
          "notifications.internal.kde-job-eta-seconds", "n", std::to_string(std::max<std::uint64_t>(1, seconds))
      );
    }
    if (seconds < 3600) {
      return i18n::tr("notifications.internal.kde-job-eta-minutes", "n", std::to_string((seconds + 30) / 60));
    }
    return i18n::tr(
        "notifications.internal.kde-job-eta-hours", "h", std::to_string(seconds / 3600), "m",
        std::to_string((seconds % 3600) / 60)
    );
  }

  std::string joinNonEmpty(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (const auto& part : parts) {
      if (part.empty()) {
        continue;
      }
      if (!out.empty()) {
        out += separator;
      }
      out += part;
    }
    return out;
  }
} // namespace

std::optional<std::uint64_t> KdeJobViewServer::asU64(const sdbus::Variant& value) {
  try {
    if (value.containsValueOfType<std::uint64_t>()) {
      return value.get<std::uint64_t>();
    }
    if (value.containsValueOfType<std::uint32_t>()) {
      return value.get<std::uint32_t>();
    }
    if (value.containsValueOfType<std::int64_t>()) {
      return static_cast<std::uint64_t>(std::max<std::int64_t>(0, value.get<std::int64_t>()));
    }
    if (value.containsValueOfType<std::int32_t>()) {
      return static_cast<std::uint64_t>(std::max<std::int32_t>(0, value.get<std::int32_t>()));
    }
    if (value.containsValueOfType<double>()) {
      return amountFromDouble(value.get<double>());
    }
  } catch (const sdbus::Error&) {
  }
  return std::nullopt;
}

KdeJobViewServer::KdeJobViewServer(SessionBus& bus, NotificationManager& manager) : m_bus(bus), m_manager(manager) {
  try {
    m_serverObject = sdbus::createObject(m_bus.connection(), kServerPath);
    m_serverObject
        ->addVTable(
            sdbus::registerMethod("registerService")
                .withInputParamNames("service", "objectPath")
                .implementedAs([](const std::string&, const std::string&) {}),
            sdbus::registerMethod("emitJobUrlsChanged").implementedAs([] {}),
            sdbus::registerMethod("requiresJobTracker").withOutputParamNames("required").implementedAs([] {
              return false;
            }),
            sdbus::registerMethod("registeredJobContacts").withOutputParamNames("contacts").implementedAs([] {
              return std::vector<std::string>{};
            }),
            sdbus::registerSignal("jobUrlsChanged").withParameters<std::vector<std::string>>("urls"),
            sdbus::registerSignal("requiresJobTrackerChanged").withParameters<bool>("required")
        )
        .forInterface("org.kde.kuiserver");

    m_serverObject
        ->addVTable(
            sdbus::registerMethod("requestView")
                .withInputParamNames("appName", "appIconName", "capabilities")
                .withOutputParamNames("trackerPath")
                .implementedAs([this](const std::string& appName, const std::string& appIcon, std::int32_t caps) {
                  return requestView({}, appName, appIcon, caps, false);
                })
        )
        .forInterface("org.kde.JobViewServer");

    m_serverObject
        ->addVTable(
            sdbus::registerMethod("requestView")
                .withInputParamNames("desktopEntry", "capabilities", "hints")
                .withOutputParamNames("trackerPath")
                .implementedAs([this](
                                   const std::string& desktopEntry, std::int32_t caps,
                                   const std::map<std::string, sdbus::Variant>& hints
                               ) {
                  bool immediate = false;
                  if (const auto it = hints.find("immediate"); it != hints.end()) {
                    immediate = asBool(it->second).value_or(false);
                  }
                  return requestView(desktopEntry, {}, {}, caps, immediate);
                })
        )
        .forInterface("org.kde.JobViewServerV2");

    m_bus.connection().requestName(kBusName);
    m_nameAcquired = true;
    m_bus.connection().requestName(kJobViewBusName);
    m_jobViewNameAcquired = true;

    m_nameWatch = sdbus::createProxy(
        m_bus.connection(), sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
    );
    m_nameWatch->uponSignal("NameOwnerChanged")
        .onInterface("org.freedesktop.DBus")
        .call([this](const std::string& name, const std::string& /*oldOwner*/, const std::string& newOwner) {
          onNameOwnerChanged(name, newOwner);
        });

    m_eventToken = m_manager.addEventCallback([this](const Notification& n, NotificationEvent event) {
      onNotificationEvent(n, event);
    });
    m_actionToken = m_manager.addInternalActionHandler(
        [this](std::uint32_t id, const std::string& key, const std::string& /*token*/, const std::string& /*sender*/) {
          onAction(id, key);
        }
    );
    JobProgress::instance().setShowHandler([this] { showDismissed(); });
    m_showHandlerRegistered = true;
    kLog.info("serving KDE job progress on org.kde.kuiserver");
  } catch (...) {
    cleanupRegistrations();
    throw;
  }
}

void KdeJobViewServer::cleanupRegistrations() noexcept {
  if (m_eventToken >= 0) {
    m_manager.removeEventCallback(m_eventToken);
    m_eventToken = -1;
  }
  if (m_actionToken >= 0) {
    m_manager.removeInternalActionHandler(m_actionToken);
    m_actionToken = -1;
  }
  if (m_showHandlerRegistered) {
    JobProgress::instance().setShowHandler(nullptr);
    m_showHandlerRegistered = false;
  }
  // Release each name independently, including when the second acquisition failed.
  for (auto [acquired, name] :
       {std::pair{&m_jobViewNameAcquired, &kJobViewBusName}, std::pair{&m_nameAcquired, &kBusName}}) {
    if (*acquired) {
      try {
        m_bus.connection().releaseName(*name);
      } catch (...) {
        // Best effort during unwinding; one failure must not skip the other name.
      }
      *acquired = false;
    }
  }
}

KdeJobViewServer::~KdeJobViewServer() {
  cleanupRegistrations();
  m_timer.stop();
  JobProgress::instance().set({});
  for (auto& [serial, job] : m_jobs) {
    if (job->notificationId != 0) {
      m_manager.close(job->notificationId, CloseReason::ClosedByCall);
    }
  }
  m_jobs.clear();
}

sdbus::ObjectPath KdeJobViewServer::requestView(
    const std::string& desktopEntry, const std::string& appName, const std::string& appIcon, int capabilities,
    bool immediate
) {
  auto job = std::make_unique<Job>();
  job->serial = m_nextSerial++;
  job->path = "/org/kde/notificationmanager/jobs/JobView_" + std::to_string(job->serial);
  job->sender = m_serverObject->getCurrentlyProcessedMessage().getSender();
  job->desktopEntry = desktopEntry;
  job->appName = appName.empty() ? desktopEntry : appName;
  job->appIcon = appIcon;
  job->capabilities = capabilities;
  job->immediate = immediate;
  registerJobObject(*job);

  const sdbus::ObjectPath path{job->path};
  m_jobs.emplace(job->serial, std::move(job));
  ensureTimer();
  return path;
}

void KdeJobViewServer::registerJobObject(Job& job) {
  const std::uint32_t serial = job.serial;
  job.object = sdbus::createObject(m_bus.connection(), sdbus::ObjectPath{job.path});

  job.object
      ->addVTable(
          sdbus::registerMethod("terminate")
              .withInputParamNames("errorCode", "errorMessage", "hints")
              .implementedAs([this, serial](
                                 std::uint32_t code, const std::string& message,
                                 const std::map<std::string, sdbus::Variant>&
                             ) { terminate(serial, code, message); }),
          sdbus::registerMethod("update")
              .withInputParamNames("properties")
              .implementedAs([this, serial](const std::map<std::string, sdbus::Variant>& properties) {
                update(serial, properties);
              }),
          sdbus::registerSignal("updateRequested"), sdbus::registerSignal("suspendRequested"),
          sdbus::registerSignal("resumeRequested"), sdbus::registerSignal("cancelRequested")
      )
      .forInterface(kJobViewV3);

  // JobViewV2 is what the older org.kde.JobViewServer API hands out; the setters map onto the same state.
  job.object
      ->addVTable(
          sdbus::registerMethod("terminate")
              .withInputParamNames("errorMessage")
              .implementedAs([this, serial](const std::string& message) {
                terminate(serial, authorizedJob(serial).errorCode, message);
              }),
          sdbus::registerMethod("setSuspended").withInputParamNames("suspended").implementedAs([this, serial](bool s) {
            update(serial, {{"suspended", sdbus::Variant{s}}});
          }),
          sdbus::registerMethod("setTotalAmount")
              .withInputParamNames("amount", "unit")
              .implementedAs([this, serial](std::uint64_t amount, const std::string& unit) {
                const char* key = unit == "bytes" ? "totalBytes" : unit == "files" ? "totalFiles" : "totalDirectories";
                update(serial, {{key, sdbus::Variant{amount}}});
              }),
          sdbus::registerMethod("setProcessedAmount")
              .withInputParamNames("amount", "unit")
              .implementedAs([this, serial](std::uint64_t amount, const std::string& unit) {
                const char* key = unit == "bytes" ? "processedBytes"
                    : unit == "files"             ? "processedFiles"
                                                  : "processedDirectories";
                update(serial, {{key, sdbus::Variant{amount}}});
              }),
          sdbus::registerMethod("setPercent")
              .withInputParamNames("percent")
              .implementedAs([this, serial](std::uint32_t percent) {
                update(serial, {{"percent", sdbus::Variant{percent}}});
              }),
          sdbus::registerMethod("setSpeed")
              .withInputParamNames("bytesPerSecond")
              .implementedAs([this, serial](std::uint64_t speed) {
                update(serial, {{"speed", sdbus::Variant{speed}}});
              }),
          sdbus::registerMethod("setInfoMessage")
              .withInputParamNames("message")
              .implementedAs([this, serial](const std::string& message) {
                update(serial, {{"infoMessage", sdbus::Variant{message}}});
              }),
          sdbus::registerMethod("setDescriptionField")
              .withInputParamNames("number", "name", "value")
              .withOutputParamNames("res")
              .implementedAs([this, serial](std::uint32_t number, const std::string& name, const std::string& value) {
                return authorizedJob(serial).setDescriptionField(number, name, value);
              }),
          sdbus::registerMethod("clearDescriptionField")
              .withInputParamNames("number")
              .implementedAs([this, serial](std::uint32_t number) {
                if (!authorizedJob(serial).setDescriptionField(number, {}, {})) {
                  throw sdbus::Error(
                      sdbus::Error::Name{"org.freedesktop.DBus.Error.InvalidArgs"}, "Unsupported description index"
                  );
                }
              }),
          sdbus::registerMethod("setDestUrl")
              .withInputParamNames("destUrl")
              .implementedAs([this, serial](sdbus::Variant url) { update(serial, {{"destUrl", std::move(url)}}); }),
          sdbus::registerMethod("setError")
              .withInputParamNames("errorCode")
              .implementedAs([this, serial](std::uint32_t code) { authorizedJob(serial).errorCode = code; }),
          sdbus::registerSignal("suspendRequested"), sdbus::registerSignal("resumeRequested"),
          sdbus::registerSignal("cancelRequested")
      )
      .forInterface(kJobViewV2);
}

KdeJobViewServer::Job* KdeJobViewServer::findJob(std::uint32_t serial) {
  const auto it = m_jobs.find(serial);
  return it == m_jobs.end() ? nullptr : it->second.get();
}

KdeJobViewServer::Job& KdeJobViewServer::authorizedJob(std::uint32_t serial) {
  Job* job = findJob(serial);
  if (job == nullptr) {
    throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.UnknownObject"}, "Unknown job");
  }
  job->authorizeSender(job->object->getCurrentlyProcessedMessage().getSender());
  if (job->dead) {
    throw sdbus::Error(sdbus::Error::Name{"org.freedesktop.DBus.Error.UnknownObject"}, "Job has finished");
  }
  return *job;
}

void KdeJobViewServer::update(std::uint32_t serial, const std::map<std::string, sdbus::Variant>& properties) {
  Job* job = &authorizedJob(serial);
  for (const auto& [key, value] : properties) {
    if (key == "title") {
      job->title = asString(value).value_or(job->title);
    } else if (key == "infoMessage") {
      job->infoMessage = asString(value).value_or(job->infoMessage);
    } else if (key == "appName") {
      if (const auto name = asString(value); name.has_value() && !name->empty()) {
        job->appName = *name;
      }
    } else if (key == "appIconName") {
      job->appIcon = asString(value).value_or(job->appIcon);
    } else if (key == "percent") {
      if (const auto percent = asU64(value); percent.has_value()) {
        job->percent = static_cast<int>(std::min<std::uint64_t>(*percent, 100));
      }
    } else if (key == "suspended") {
      job->suspended = asBool(value).value_or(job->suspended);
    } else if (key == "processedBytes") {
      job->processedBytes = asU64(value).value_or(job->processedBytes);
    } else if (key == "totalBytes") {
      job->totalBytes = asU64(value).value_or(job->totalBytes);
    } else if (key == "processedFiles") {
      job->processedFiles = asU64(value).value_or(job->processedFiles);
    } else if (key == "totalFiles") {
      job->totalFiles = asU64(value).value_or(job->totalFiles);
    } else if (key == "processedItems") {
      job->processedItems = asU64(value).value_or(job->processedItems);
    } else if (key == "totalItems") {
      job->totalItems = asU64(value).value_or(job->totalItems);
    } else if (key == "speed") {
      job->speed = asU64(value).value_or(job->speed);
    } else if (key == "descriptionLabel1") {
      job->label1 = asString(value).value_or(job->label1);
    } else if (key == "descriptionValue1") {
      job->value1 = asString(value).value_or(job->value1);
    } else if (key == "descriptionLabel2") {
      job->label2 = asString(value).value_or(job->label2);
    } else if (key == "descriptionValue2") {
      job->value2 = asString(value).value_or(job->value2);
    } else if (key == "destUrl") {
      job->destUrl = asString(value).value_or(job->destUrl);
    }
  }
  job->dirty = true;
}

void KdeJobViewServer::terminate(std::uint32_t serial, std::uint32_t errorCode, const std::string& errorMessage) {
  Job* job = &authorizedJob(serial);
  finish(*job, errorCode, errorMessage);
}

void KdeJobViewServer::finish(Job& job, std::uint32_t errorCode, const std::string& errorMessage) {
  job.dead = true;
  const bool shown = job.notificationId != 0;
  const bool cancelled = errorCode == kKilledJobError;

  if (cancelled || job.dismissed) {
    if (shown && !job.dismissed) {
      m_manager.close(job.notificationId, CloseReason::ClosedByCall);
    }
    job.notificationId = 0;
    return;
  }

  const std::string label = job.title.empty() ? i18n::tr("notifications.internal.kde-job-default-title") : job.title;
  const std::string where = job.destUrl.empty() ? (job.value2.empty() ? std::string{} : displayLocation(job.value2))
                                                : displayLocation(job.destUrl);

  if (errorCode != 0) {
    // A failure is worth showing even when the job ended before the progress popup would have appeared.
    m_manager.addOrReplace(
        NotificationRequest{
            .replacesId = job.notificationId,
            .appName = job.appName,
            .summary = i18n::tr("notifications.internal.kde-job-failed-title", "title", label),
            .body = errorMessage.empty() ? joinNonEmpty({job.infoMessage, where}, "\n") : errorMessage,
            .urgency = Urgency::Normal,
            .timeout = 10000,
            .origin = NotificationOrigin::Internal,
            .persistInHistory = true,
            .icon = job.appIcon.empty() ? std::optional<std::string>{"dialog-error"} : job.appIcon,
            .iconIsAppIcon = !job.appIcon.empty(),
            .desktopEntry = job.desktopEntry.empty() ? std::nullopt : std::optional<std::string>{job.desktopEntry},
            .coalesce = false,
        }
    );
    return;
  }

  if (!shown) {
    return; // finished before it was worth a popup
  }
  const std::string folder = localFolderOf(job.destUrl.empty() ? job.value2 : job.destUrl);
  std::vector<std::string> actions;
  if (!folder.empty()) {
    // Keep the payload with its bounded notification/history record, not a second lifetime map.
    actions = {makeOpenFolderAction(folder), i18n::tr("notifications.internal.kde-job-open-folder")};
  }
  m_manager.addOrReplace(
      NotificationRequest{
          .replacesId = job.notificationId,
          .appName = job.appName,
          .summary = i18n::tr("notifications.internal.kde-job-finished-title", "title", label),
          .body = joinNonEmpty({job.infoMessage, where}, "\n"),
          .urgency = Urgency::Normal,
          .timeout = 8000,
          .origin = NotificationOrigin::Internal,
          .persistInHistory = true,
          .actions = std::move(actions),
          .icon = job.appIcon.empty() ? std::nullopt : std::optional<std::string>{job.appIcon},
          .iconIsAppIcon = !job.appIcon.empty(),
          .desktopEntry = job.desktopEntry.empty() ? std::nullopt : std::optional<std::string>{job.desktopEntry},
          .progress = std::nullopt,
          .coalesce = false,
      }
  );
}

void KdeJobViewServer::emitRequest(Job& job, const char* signal) {
  if (job.object == nullptr) {
    return;
  }
  for (const char* interface : {kJobViewV3, kJobViewV2}) {
    try {
      job.object->emitSignal(sdbus::SignalName{signal}).onInterface(sdbus::InterfaceName{interface});
    } catch (const sdbus::Error& e) {
      kLog.debug("emit {} failed: {}", signal, e.what());
    }
  }
}

void KdeJobViewServer::pushNotification(Job& job) {
  const std::optional<int> percent = job.effectivePercent();

  std::vector<std::string> stats;
  if (percent.has_value()) {
    stats.push_back(std::format("{}%", *percent));
  }
  if (job.totalFiles > 1) {
    stats.push_back(
        i18n::tr(
            "notifications.internal.kde-job-files", "done", std::to_string(job.processedFiles), "total",
            std::to_string(job.totalFiles)
        )
    );
  }
  if (job.totalBytes > 0) {
    stats.push_back(std::format("{} / {}", humanBytes(job.processedBytes), humanBytes(job.totalBytes)));
  }
  if (job.speed > 0 && !job.suspended) {
    stats.push_back(std::format("{}/s", humanBytes(job.speed)));
    if (job.totalBytes > job.processedBytes) {
      stats.push_back(etaText((job.totalBytes - job.processedBytes) / job.speed));
    }
  }
  if (job.suspended) {
    stats.push_back(i18n::tr("notifications.internal.kde-job-paused"));
  }

  std::vector<std::string> lines;
  if (!job.infoMessage.empty() && job.infoMessage != job.title) {
    lines.push_back(job.infoMessage);
  }
  // KIO reports the file being processed; show its name, and the destination folder rather than another long path.
  if (!job.value1.empty()) {
    lines.push_back((job.label1.empty() ? std::string{} : job.label1 + ": ") + baseName(displayLocation(job.value1)));
  }
  if (!job.value2.empty()) {
    std::string target = displayLocation(job.value2);
    if (!job.value1.empty() && baseName(target) == baseName(displayLocation(job.value1))) {
      target = parentOf(target);
    }
    lines.push_back((job.label2.empty() ? std::string{} : job.label2 + ": ") + target);
  }
  lines.push_back(joinNonEmpty(stats, "  ·  "));

  std::vector<std::string> actions;
  if ((job.capabilities & kCapabilitySuspend) != 0) {
    actions.emplace_back(job.suspended ? kResumeAction : kPauseAction);
    actions.push_back(
        i18n::tr(job.suspended ? "notifications.internal.kde-job-resume" : "notifications.internal.kde-job-pause")
    );
  }
  if ((job.capabilities & kCapabilityCancel) != 0) {
    actions.emplace_back(kCancelAction);
    actions.push_back(i18n::tr("notifications.internal.kde-job-cancel"));
  }

  const std::string label = job.title.empty() ? i18n::tr("notifications.internal.kde-job-default-title") : job.title;
  job.notificationId = m_manager.addOrReplace(
      NotificationRequest{
          .replacesId = job.notificationId,
          .appName = job.appName,
          .summary = label,
          .body = joinNonEmpty(lines, "\n"),
          .urgency = Urgency::Normal,
          .timeout = 0, // stays until the job ends
          .origin = NotificationOrigin::Internal,
          .actions = std::move(actions),
          .icon = job.appIcon.empty() ? std::nullopt : std::optional<std::string>{job.appIcon},
          .iconIsAppIcon = !job.appIcon.empty(),
          .desktopEntry = job.desktopEntry.empty() ? std::nullopt : std::optional<std::string>{job.desktopEntry},
          .progress = percent.has_value() ? percent : std::optional<int>{0},
          .coalesce = false,
      }
  );
  job.dirty = false;
}

void KdeJobViewServer::tick() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = m_jobs.begin(); it != m_jobs.end();) {
    Job& job = *it->second;
    if (job.dead) {
      it = m_jobs.erase(it);
      continue;
    }
    job.reopen = false; // a close caused by an action button has been handled by now
    const bool due = job.immediate || now - job.created >= kShowDelay;
    if (due && !job.dismissed && (job.dirty || job.notificationId == 0)) {
      pushNotification(job);
    }
    ++it;
  }
  publishState();
  if (m_jobs.empty()) {
    m_timer.stop();
  }
}

void KdeJobViewServer::publishState() {
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::pair<std::uint32_t, JobProgressEntry>> entries;
  for (const auto& [serial, job] : m_jobs) {
    if (job->dead || (!job->immediate && now - job->created < kShowDelay)) {
      continue;
    }
    const std::optional<int> percent = job->effectivePercent();
    std::vector<std::string> detail;
    if (percent.has_value()) {
      detail.push_back(std::format("{}%", *percent));
    }
    if (job->suspended) {
      detail.push_back(i18n::tr("notifications.internal.kde-job-paused"));
    }
    entries.emplace_back(
        serial,
        JobProgressEntry{
            .title = job->title.empty() ? i18n::tr("notifications.internal.kde-job-default-title") : job->title,
            .detail = joinNonEmpty(detail, "  ·  "),
            .percent = percent,
        }
    );
  }
  std::ranges::sort(entries, {}, [](const auto& entry) { return entry.first; });
  std::vector<JobProgressEntry> jobs;
  jobs.reserve(entries.size());
  for (auto& entry : entries) {
    jobs.push_back(std::move(entry.second));
  }
  JobProgress::instance().set(std::move(jobs));
}

void KdeJobViewServer::showDismissed() {
  for (auto& [serial, job] : m_jobs) {
    if (!job->dead && job->dismissed) {
      job->dismissed = false;
      job->notificationId = 0;
      job->dirty = true;
    }
  }
}

void KdeJobViewServer::ensureTimer() {
  if (!m_timer.active()) {
    m_timer.startRepeating(kTick, [this] { tick(); });
  }
}

void KdeJobViewServer::onNotificationEvent(const Notification& n, NotificationEvent event) {
  if (event != NotificationEvent::Closed) {
    return;
  }
  // The close callback does not carry the reason; a popup that closes while its job still runs was dismissed by
  // the user (our own closes only happen when the job is already finished).
  for (auto& [serial, job] : m_jobs) {
    if (job->notificationId != n.id || job->dead) {
      continue;
    }
    if (job->reopen) {
      // Pause/Resume closed the popup like any action button does; the next tick shows it again.
      job->reopen = false;
      job->notificationId = 0;
      job->dirty = true;
    } else {
      job->dismissed = true;
    }
  }
}

void KdeJobViewServer::onAction(std::uint32_t id, const std::string& actionKey) {
  if (const auto folder = folderFromAction(actionKey); folder.has_value()) {
    if (!process::runAsync({"xdg-open", folder->c_str()})) {
      kLog.warn("could not open {}", *folder);
    }
    return;
  }
  const bool pause = actionKey == kPauseAction;
  const bool resume = actionKey == kResumeAction;
  if (actionKey != kCancelAction && !pause && !resume) {
    return;
  }
  for (auto& [serial, job] : m_jobs) {
    if (job->notificationId != id || job->dead) {
      continue;
    }
    if (pause || resume) {
      kLog.info("{} requested for job {}", pause ? "pause" : "resume", job->serial);
      job->reopen = true;
      emitRequest(*job, pause ? "suspendRequested" : "resumeRequested");
    } else {
      kLog.info("cancel requested for job {}", job->serial);
      emitRequest(*job, "cancelRequested");
    }
    return;
  }
}

void KdeJobViewServer::onNameOwnerChanged(const std::string& name, const std::string& newOwner) {
  if (!newOwner.empty() || name.empty() || name.front() != ':') {
    return;
  }
  for (auto& [serial, job] : m_jobs) {
    if (!job->dead && job->sender == name) {
      kLog.info("job {} owner {} left the bus", job->serial, name);
      finish(*job, kKilledJobError, {});
    }
  }
}

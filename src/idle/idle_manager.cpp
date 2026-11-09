#include "idle/idle_manager.h"

#include "cli/schema_msg.h"
#include "config/config_types.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "shell/wayland/shell_globals.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace {

  constexpr Logger kLog("idle");

  constexpr std::uint32_t kHeartbeatTimeoutMs = 1000;
  constexpr double kMaxIdleTimeoutMs = static_cast<double>(std::numeric_limits<std::uint32_t>::max());

  /// Browsers renew their screensaver inhibit during long playback by dropping it and immediately
  /// taking a new one (Firefox, roughly once a minute, with gaps measured at 370-660ms). Treating such
  /// a gap as a release would restart every idle countdown on each renewal, so a release has to outlive
  /// this window. Kept just above the observed gaps: it is added latency on every genuine pause, and
  /// letting a renewal slip through only costs a countdown restart that playback would suppress anyway.
  constexpr auto kInhibitReleaseDebounce = std::chrono::milliseconds(1000);

  const ext_idle_notification_v1_listener kIdleNotificationListener = {
      .idled = &IdleManager::handleIdled,
      .resumed = &IdleManager::handleResumed,
  };

  const ext_idle_notification_v1_listener kHeartbeatListener = {
      .idled = &IdleManager::handleHeartbeatIdled,
      .resumed = &IdleManager::handleHeartbeatResumed,
  };

  std::uint32_t timeoutSecondsToMilliseconds(double timeoutSeconds) {
    const double timeoutMs = std::clamp(std::ceil(timeoutSeconds * 1000.0), 1.0, kMaxIdleTimeoutMs);
    return static_cast<std::uint32_t>(timeoutMs);
  }

} // namespace

IdleManager::IdleManager() = default;

IdleManager::~IdleManager() {
  clearBehaviors();
  destroyHeartbeat();
}

bool IdleManager::initialize(WaylandConnection& wayland, GraceBeginCallback onBegin, GraceEndCallback onEnd) {
  m_wayland = &wayland;
  m_shellGlobals = &wayland.extension<ShellGlobals>();
  m_onGraceBegin = std::move(onBegin);
  m_onGraceEnd = std::move(onEnd);
  if (!m_shellGlobals->hasIdleNotifier()) {
    kLog.info("idle notify protocol unavailable");
  }
  return true;
}

void IdleManager::setActionRunner(ActionRunner runner) { m_actionRunner = std::move(runner); }

void IdleManager::setLiveIdleChangeCallback(std::function<void()> callback) {
  m_onLiveIdleChange = std::move(callback);
}

void IdleManager::notifyLiveIdleChanged() {
  if (m_onLiveIdleChange) {
    m_onLiveIdleChange();
  }
}

void IdleManager::setScreenSaverInhibitLocks(std::int64_t locks) {
  const std::int64_t next = std::max<std::int64_t>(0, locks);
  if (m_screenSaverInhibitLocks == next) {
    return;
  }
  const bool wasInhibited = screenSaverInhibited();
  m_screenSaverInhibitLocks = next;

  if (m_screenSaverInhibitLocks > 0) {
    if (m_inhibitReleasePending) {
      kLog.debug("screensaver inhibit renewed within debounce; idle countdowns kept running");
      m_inhibitReleasePending = false;
      m_inhibitReleaseTimer.stop();
    }
    if (!wasInhibited) {
      // Playback started mid-fade: abort it rather than blanking the screen over a fresh video. The
      // cancelled behaviors return to Waiting and are re-armed when the inhibit is released.
      cancelActiveGrace(true);
      if (m_heartbeatCompositorIdle) {
        notifyLiveIdleChanged();
      }
    }
    return;
  }

  if (!wasInhibited || m_inhibitReleasePending) {
    return;
  }
  m_inhibitReleasePending = true;
  m_inhibitReleaseTimer.start(kInhibitReleaseDebounce, [this] { applyScreenSaverInhibitRelease(); });
}

void IdleManager::applyScreenSaverInhibitRelease() {
  if (!m_inhibitReleasePending) {
    return;
  }
  m_inhibitReleasePending = false;
  if (m_screenSaverInhibitLocks > 0) {
    return;
  }

  // Notifications that idled while inhibited cannot report idle again before activity resumes, so
  // restart every waiting countdown from the release. Idled behaviors keep waiting for real input to
  // run their resume action.
  kLog.debug("screensaver inhibit released; restarting idle countdowns");
  for (auto& behavior : m_behaviors) {
    if (behavior->phase == BehaviorPhase::Waiting) {
      recreateBehaviorNotification(*behavior);
    }
  }

  // The heartbeat keeps tracking the compositor while inhibited; count live idle from the release so
  // it matches the restarted countdowns.
  if (m_heartbeatCompositorIdle) {
    m_heartbeatIdleSince = std::chrono::steady_clock::now() - std::chrono::milliseconds(kHeartbeatTimeoutMs);
    notifyLiveIdleChanged();
  }
}

void IdleManager::setSessionLocked(bool locked) {
  if (m_sessionLocked == locked) {
    return;
  }
  m_sessionLocked = locked;
  if (!locked) {
    recreateBehaviorNotifications();
    return;
  }

  // Completed actions retain their effects and resume listeners; fades finish on their current schedule.
  for (auto& behavior : m_behaviors) {
    syncBehaviorNotification(*behavior);
  }
}

double IdleManager::effectiveTimeoutSeconds(const IdleBehaviorConfig& config) const {
  if (m_sessionLocked && std::isfinite(config.lockedTimeoutSeconds) && config.lockedTimeoutSeconds > 0.0) {
    return config.lockedTimeoutSeconds;
  }
  return config.timeoutSeconds;
}

std::uint32_t IdleManager::effectiveTimeoutMilliseconds(const IdleBehaviorConfig& config) const {
  const double timeout = effectiveTimeoutSeconds(config);
  if (!config.enabled || !std::isfinite(timeout) || timeout <= 0.0) {
    return 0;
  }
  return timeoutSecondsToMilliseconds(timeout);
}

void IdleManager::reload(const IdleConfig& config) {
  clearBehaviors();
  m_baseIdleConfig = config;
  m_idleConfig = config;
  for (auto& behavior : m_idleConfig.behaviors) {
    if (const auto it = m_behaviorOverrides.find(behavior.name); it != m_behaviorOverrides.end()) {
      behavior.enabled = it->second;
    }
  }

  if (m_wayland == nullptr) {
    destroyHeartbeat();
    return;
  }
  if (!m_shellGlobals->hasIdleNotifier()) {
    kLog.info("idle notify protocol unavailable; idle behaviors not registered");
    destroyHeartbeat();
    return;
  }
  if (m_wayland->seat() == nullptr) {
    kLog.warn("cannot register idle behaviors without a Wayland seat");
    destroyHeartbeat();
    return;
  }

  for (const auto& behavior : config.behaviors) {
    createBehavior(behavior);
  }
  syncHeartbeat();
}

std::int64_t IdleManager::liveIdleSeconds() const noexcept {
  if (!m_heartbeatCompositorIdle || screenSaverInhibited()) {
    return 0;
  }
  const auto elapsed = std::chrono::steady_clock::now() - m_heartbeatIdleSince;
  return std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

void IdleManager::syncHeartbeat() {
  destroyHeartbeat();
  if (m_wayland == nullptr || !m_shellGlobals->hasIdleNotifier() || m_wayland->seat() == nullptr) {
    m_heartbeatCompositorIdle = false;
    return;
  }

  m_heartbeatNotification = m_shellGlobals->createIdleNotification(kHeartbeatTimeoutMs);
  if (m_heartbeatNotification == nullptr) {
    kLog.warn("failed to register idle heartbeat for live status");
    return;
  }

  ext_idle_notification_v1_add_listener(m_heartbeatNotification, &kHeartbeatListener, this);
  kLog.debug("registered idle heartbeat ({}ms)", kHeartbeatTimeoutMs);
}

void IdleManager::destroyHeartbeat() {
  if (m_heartbeatNotification != nullptr) {
    ext_idle_notification_v1_destroy(m_heartbeatNotification);
    m_heartbeatNotification = nullptr;
  }
  m_heartbeatCompositorIdle = false;
}

void IdleManager::clearBehaviors() {
  cancelActiveGrace(false);
  for (auto& behavior : m_behaviors) {
    if (behavior->notification != nullptr) {
      ext_idle_notification_v1_destroy(behavior->notification);
      behavior->notification = nullptr;
    }
  }
  m_behaviors.clear();
}

void IdleManager::recreateBehaviorNotification(BehaviorState& behavior) {
  if (m_wayland == nullptr || !m_shellGlobals->hasIdleNotifier() || m_wayland->seat() == nullptr) {
    return;
  }

  // Re-arming an idled behavior (e.g. on unlock) has no natural resume event, so run it here —
  // otherwise screen_off's display-off is never undone and the screen stays black.
  if (behavior.phase == BehaviorPhase::Idled) {
    runResumeBehavior(behavior);
  }

  if (behavior.notification != nullptr) {
    ext_idle_notification_v1_destroy(behavior.notification);
    behavior.notification = nullptr;
  }
  behavior.notificationTimeoutMs = 0;
  behavior.phase = BehaviorPhase::Waiting;

  const auto timeoutMs = effectiveTimeoutMilliseconds(behavior.config);
  if (timeoutMs == 0) {
    return;
  }

  if (behavior.config.ignoreInhibitors) {
    // Input-only notification: the compositor ignores idle inhibitors for it (ext-idle-notify v2).
    behavior.notification = m_shellGlobals->createInputIdleNotification(timeoutMs);
    if (behavior.notification == nullptr) {
      kLog.warn("compositor lacks ext-idle-notify v2; '{}' cannot ignore inhibitors", behavior.config.name);
    }
  }
  if (behavior.notification == nullptr) {
    behavior.notification = m_shellGlobals->createIdleNotification(timeoutMs);
  }
  if (behavior.notification == nullptr) {
    kLog.warn("failed to re-register idle behavior '{}'", behavior.config.name);
    return;
  }
  behavior.notificationTimeoutMs = timeoutMs;
  ext_idle_notification_v1_add_listener(behavior.notification, &kIdleNotificationListener, &behavior);
}

void IdleManager::syncBehaviorNotification(BehaviorState& behavior) {
  if (behavior.phase == BehaviorPhase::Waiting
      && behavior.notificationTimeoutMs != effectiveTimeoutMilliseconds(behavior.config)) {
    recreateBehaviorNotification(behavior);
  }
}

void IdleManager::recreateBehaviorNotifications() {
  if (m_wayland == nullptr || !m_shellGlobals->hasIdleNotifier() || m_wayland->seat() == nullptr) {
    return;
  }

  cancelActiveGrace(false);
  for (auto& behavior : m_behaviors) {
    recreateBehaviorNotification(*behavior);
  }
  kLog.debug("idle behavior notifications re-armed");
}

void IdleManager::createBehavior(const IdleBehaviorConfig& config) {
  if (!config.enabled) {
    return;
  }
  if (!std::isfinite(config.timeoutSeconds) || config.timeoutSeconds < 0.0) {
    kLog.warn("idle behavior '{}' ignored: timeout must be >= 0 seconds", config.name);
    return;
  }
  if (!std::isfinite(config.lockedTimeoutSeconds) || config.lockedTimeoutSeconds < 0.0) {
    kLog.warn("idle behavior '{}' ignored: locked timeout must be >= 0 seconds", config.name);
    return;
  }
  if (config.timeoutSeconds == 0.0 && config.lockedTimeoutSeconds == 0.0) {
    kLog.debug("idle behavior '{}' disabled by zero timeout", config.name);
    return;
  }
  const ResolvedIdleBehavior resolved = resolveIdleBehaviorActions(config);
  if (resolved.idleAction.kind == IdleActionKind::None) {
    kLog.warn("idle behavior '{}' ignored: needs an action", config.name);
    return;
  }

  auto behavior = std::make_unique<BehaviorState>();
  behavior->owner = this;
  behavior->config = config;
  recreateBehaviorNotification(*behavior);
  kLog.info(
      "registered idle behavior '{}' timeout={}s locked_timeout={}s", config.name, config.timeoutSeconds,
      config.lockedTimeoutSeconds
  );
  m_behaviors.push_back(std::move(behavior));
}

bool IdleManager::runBehavior(BehaviorState& behavior) {
  const ResolvedIdleBehavior resolved = resolveIdleBehaviorActions(behavior.config);
  const bool ok = runAction(behavior.config, resolved.idleAction);
  if (!ok) {
    kLog.warn("idle behavior '{}' action failed", behavior.config.name);
  }
  return ok;
}

void IdleManager::runResumeBehavior(BehaviorState& behavior) {
  const ResolvedIdleBehavior resolved = resolveIdleBehaviorActions(behavior.config);
  if (resolved.resumeAction.kind != IdleActionKind::None && !runAction(behavior.config, resolved.resumeAction)) {
    kLog.warn("idle behavior '{}' native resume action failed", behavior.config.name);
  }
  if (!resolved.resumeCommand.empty()
      && !runAction(
          behavior.config, IdleActionRequest{.kind = IdleActionKind::Command, .command = resolved.resumeCommand}
      )) {
    kLog.warn("idle behavior '{}' resume command failed", behavior.config.name);
  }
}

bool IdleManager::runAction(const IdleBehaviorConfig& behavior, const IdleActionRequest& action) const {
  if (action.kind == IdleActionKind::None) {
    return true;
  }
  if (!m_actionRunner) {
    return false;
  }
  return m_actionRunner(behavior, action);
}

void IdleManager::cancelActiveGrace(bool userCancelled) {
  if (!hasActiveGrace()) {
    return;
  }
  const bool willLockSession = m_activeGraceWillLock;
  for (auto* behavior : m_graceBehaviors) {
    if (behavior != nullptr) {
      behavior->phase = BehaviorPhase::Waiting;
    }
  }
  m_graceBehaviors.clear();
  m_activeGraceWillLock = false;
  m_graceFallbackTimer.stop();
  ++m_graceGeneration;
  if (m_onGraceEnd) {
    m_onGraceEnd(userCancelled, willLockSession);
  }
}

void IdleManager::graceFadeComplete() {
  if (!hasActiveGrace()) {
    return;
  }
  auto behaviors = std::move(m_graceBehaviors);
  m_graceBehaviors.clear();
  m_graceFallbackTimer.stop();

  // End grace (and tear down the overlay) before idle actions. Suspend can freeze the process
  // before a deferred hide runs, leaving the opaque layer surface stuck after resume.
  bool willDispatchLockAction = false;
  for (auto* behavior : behaviors) {
    if (behavior == nullptr || behavior->phase != BehaviorPhase::Fading) {
      continue;
    }
    const IdleActionKind idleKind = resolveIdleBehaviorActions(behavior->config).idleAction.kind;
    if (idleKind == IdleActionKind::Lock || idleKind == IdleActionKind::LockAndSuspend) {
      willDispatchLockAction = true;
    }
  }
  m_activeGraceWillLock = false;
  if (m_onGraceEnd) {
    m_onGraceEnd(false, willDispatchLockAction);
  }

  for (auto* behavior : behaviors) {
    if (behavior == nullptr || behavior->phase != BehaviorPhase::Fading) {
      continue;
    }
    behavior->phase = BehaviorPhase::Idled;
    kLog.info("idle behavior '{}' triggered after pre-action fade", behavior->config.name);
    runBehavior(*behavior);
  }
}

void IdleManager::joinActiveGrace(BehaviorState& behavior) {
  if (std::ranges::contains(m_graceBehaviors, &behavior)) {
    return;
  }
  const IdleActionKind idleKind = resolveIdleBehaviorActions(behavior.config).idleAction.kind;
  if (idleKind == IdleActionKind::Lock || idleKind == IdleActionKind::LockAndSuspend) {
    m_activeGraceWillLock = true;
  }
  behavior.phase = BehaviorPhase::Fading;
  m_graceBehaviors.push_back(&behavior);
}

void IdleManager::handleIdled(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* behavior = static_cast<BehaviorState*>(data);
  if (behavior == nullptr || behavior->owner == nullptr) {
    return;
  }
  if (behavior->phase != BehaviorPhase::Waiting) {
    return;
  }

  IdleManager& self = *behavior->owner;
  if (self.m_lastExternalActivity != std::chrono::steady_clock::time_point{}) {
    const auto timeout = std::chrono::milliseconds(self.effectiveTimeoutMilliseconds(behavior->config));
    const auto since = std::chrono::steady_clock::now() - self.m_lastExternalActivity;
    if (since < timeout) {
      behavior->deferred = true;
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(timeout - since) + std::chrono::milliseconds(100);
      kLog.info(
          "idle behavior '{}' postponed: controller input {}s ago", behavior->config.name,
          std::chrono::duration_cast<std::chrono::seconds>(since).count()
      );
      behavior->deferTimer.start(remaining, [behavior] {
        if (!behavior->deferred) {
          return;
        }
        behavior->deferred = false;
        handleIdled(behavior, nullptr);
      });
      return;
    }
  }
  if (self.screenSaverInhibited() && !behavior->config.ignoreInhibitors) {
    kLog.info(
        "idle behavior '{}' suppressed (screensaver inhibit locks={}{})", behavior->config.name,
        self.m_screenSaverInhibitLocks, self.m_inhibitReleasePending ? ", release pending" : ""
    );
    // Leave the notification idled: real input re-arms it in the compositor, and the inhibit release
    // restarts it. Recreating it here would restart the countdown on every inhibit renewal.
    return;
  }

  const float fadeSec = self.m_idleConfig.preActionFadeSeconds;
  if (fadeSec > 0.0005F) {
    assert(self.m_onGraceBegin);
    if (self.hasActiveGrace()) {
      self.joinActiveGrace(*behavior);
      kLog.info("idle behavior '{}' joined active pre-action fade", behavior->config.name);
      return;
    }

    const int fadeMs = static_cast<int>(std::lround(static_cast<double>(fadeSec) * 1000.0));
    const auto fade = std::chrono::milliseconds(std::clamp(fadeMs, 1, 600000));
    kLog.info("idle behavior '{}' pre-action fade {}ms", behavior->config.name, fade.count());
    const IdleActionKind idleKind = resolveIdleBehaviorActions(behavior->config).idleAction.kind;
    const bool willLockSession = idleKind == IdleActionKind::Lock || idleKind == IdleActionKind::LockAndSuspend;
    self.joinActiveGrace(*behavior);
    const std::uint64_t generation = ++self.m_graceGeneration;
    self.m_graceFallbackTimer.start(fade + std::chrono::milliseconds(250), [&self, generation] {
      if (self.m_graceGeneration != generation || !self.hasActiveGrace()) {
        return;
      }
      kLog.debug("idle pre-action fade fallback completed");
      self.graceFadeComplete();
    });
    self.m_onGraceBegin(behavior->config.name, fade, willLockSession, [ptr = &self, generation] {
      DeferredCall::callLater([ptr, generation] {
        if (ptr->m_graceGeneration != generation || !ptr->hasActiveGrace()) {
          return;
        }
        ptr->graceFadeComplete();
      });
    });
    return;
  }

  behavior->phase = BehaviorPhase::Idled;
  kLog.info("idle behavior '{}' triggered", behavior->config.name);
  self.runBehavior(*behavior);
}

bool IdleManager::setBehaviorEnabled(const std::string& name, bool enabled) {
  const bool known = std::ranges::any_of(m_baseIdleConfig.behaviors, [&name](const IdleBehaviorConfig& behavior) {
    return behavior.name == name;
  });
  if (!known) {
    return false;
  }
  m_behaviorOverrides[name] = enabled;
  reload(m_baseIdleConfig);
  return true;
}

void IdleManager::clearBehaviorOverrides() {
  if (m_behaviorOverrides.empty()) {
    return;
  }
  m_behaviorOverrides.clear();
  reload(m_baseIdleConfig);
}

std::string IdleManager::describeBehaviors() const {
  std::string out;
  for (const auto& behavior : m_idleConfig.behaviors) {
    const bool on = behavior.enabled && behavior.timeoutSeconds > 0.0;
    out += std::format(
        "{}: {} ({}s, {}){}\n", behavior.name, on ? "on" : "off", static_cast<int>(behavior.timeoutSeconds),
        behavior.action.empty() ? "command" : behavior.action,
        m_behaviorOverrides.contains(behavior.name) ? " [override]" : ""
    );
  }
  return out.empty() ? "no idle behaviors\n" : out;
}

void IdleManager::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::idleBehavior, [this](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.empty() || parts[0] == "list" || parts[0] == "status") {
      return describeBehaviors();
    }
    if (parts[0] == "reset") {
      clearBehaviorOverrides();
      return "ok\n";
    }
    const std::string& name = parts[0];
    const std::string state = parts.size() > 1 ? parts[1] : std::string("status");
    const auto current = std::ranges::find_if(m_idleConfig.behaviors, [&name](const IdleBehaviorConfig& behavior) {
      return behavior.name == name;
    });
    if (current == m_idleConfig.behaviors.end()) {
      return "error: unknown idle behavior \"" + name + "\"\n";
    }
    if (state == "status") {
      return std::string(current->enabled ? "on\n" : "off\n");
    }
    if (state == "trigger" || state == "resume") {
      // Runs the behavior's action (or its resume command) right now, whether or not the behavior is enabled.
      // Triggers skip the grace fade; they are explicit requests.
      const auto base = std::ranges::find_if(m_baseIdleConfig.behaviors, [&name](const IdleBehaviorConfig& behavior) {
        return behavior.name == name;
      });
      const IdleBehaviorConfig& config = base != m_baseIdleConfig.behaviors.end() ? *base : *current;
      const ResolvedIdleBehavior resolved = resolveIdleBehaviorActions(config);
      if (state == "trigger") {
        if (resolved.idleAction.kind == IdleActionKind::None) {
          return "error: behavior \"" + name + "\" has no action\n";
        }
        return runAction(config, resolved.idleAction) ? "ok\n" : "error: action failed\n";
      }
      bool ok = true;
      if (resolved.resumeAction.kind != IdleActionKind::None) {
        ok = runAction(config, resolved.resumeAction) && ok;
      }
      if (!resolved.resumeCommand.empty()) {
        ok = runAction(config, IdleActionRequest{.kind = IdleActionKind::Command, .command = resolved.resumeCommand})
            && ok;
      }
      return ok ? "ok\n" : "error: resume failed\n";
    }
    bool next = false;
    if (state == "on" || state == "enable") {
      next = true;
    } else if (state == "off" || state == "disable") {
      next = false;
    } else if (state == "toggle") {
      next = !current->enabled;
    } else {
      return "error: expected on, off, toggle, status, trigger or resume\n";
    }
    (void)setBehaviorEnabled(name, next);
    return "ok\n";
  });
}

void IdleManager::noteExternalActivity() { m_lastExternalActivity = std::chrono::steady_clock::now(); }

void IdleManager::handleResumed(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* behavior = static_cast<BehaviorState*>(data);
  if (behavior == nullptr || behavior->owner == nullptr) {
    return;
  }

  IdleManager& self = *behavior->owner;
  behavior->deferred = false;
  behavior->deferTimer.stop();
  if (behavior->phase == BehaviorPhase::Fading) {
    kLog.info("idle behavior '{}' pre-action fade cancelled (user active)", behavior->config.name);
    self.cancelActiveGrace(true);
    for (auto& pending : self.m_behaviors) {
      self.syncBehaviorNotification(*pending);
    }
    return;
  }

  if (behavior->phase != BehaviorPhase::Idled) {
    return;
  }

  behavior->phase = BehaviorPhase::Waiting;
  kLog.info("idle behavior '{}' resumed", behavior->config.name);
  self.runResumeBehavior(*behavior);
  self.syncBehaviorNotification(*behavior);
}

void IdleManager::handleHeartbeatIdled(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* self = static_cast<IdleManager*>(data);
  if (self == nullptr) {
    return;
  }
  // Record idle even while inhibited: ext-idle-notify cannot resend `idled` without an intervening
  // `resumed`, so dropping it here would strand the live status at active until real input.
  self->m_heartbeatCompositorIdle = true;
  // The compositor reports idle only once the heartbeat timeout has already elapsed.
  self->m_heartbeatIdleSince = std::chrono::steady_clock::now() - std::chrono::milliseconds(kHeartbeatTimeoutMs);
  if (!self->screenSaverInhibited()) {
    self->notifyLiveIdleChanged();
  }
}

void IdleManager::handleHeartbeatResumed(void* data, ext_idle_notification_v1* /*notification*/) {
  auto* self = static_cast<IdleManager*>(data);
  if (self == nullptr) {
    return;
  }
  self->m_heartbeatCompositorIdle = false;
  self->notifyLiveIdleChanged();
}

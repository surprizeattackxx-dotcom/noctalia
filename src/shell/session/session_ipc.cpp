#include "shell/session/session_ipc.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "shell/lockscreen/lock_screen.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"

#include <string>
#include <string_view>

namespace {

  SessionPanelActionConfig builtinAction(std::string_view action) {
    return SessionPanelActionConfig{.action = std::string(action)};
  }

  std::string unknownSessionActionError(std::string_view action) {
    return "error: unknown session action \""
        + std::string(action)
        + "\" (try: lock, suspend, lock-and-suspend, logout, reboot, reboot-uefi, shutdown)\n";
  }

} // namespace

void registerSessionIpc(IpcService& ipc, SessionActionRunner& runner, LockScreen& lockScreen, ConfigService& config) {
  const auto dispatch = [&runner, &lockScreen, &config](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.empty()) {
      return "error: session requires <lock|suspend|lock-and-suspend|logout|reboot|reboot-uefi|shutdown>\n";
    }

    const std::string& ipcAction = parts[0];
    if (ipcAction == "lock-prompt") {
      // Display-only mirror of KDE's greeter (which owns the keyboard): "chars N", "auth", "fail", "clear".
      return lockScreen.mirrorKdePrompt(
                 parts.size() > 1 ? parts[1] : std::string{}, parts.size() > 2 ? parts[2] : std::string{}
             )
          ? "ok\n"
          : "error: no lock overlay active or bad argument\n";
    }
    const auto action = session_action::canonicalActionName(ipcAction);
    if (!action.has_value()) {
      return unknownSessionActionError(ipcAction);
    }

    if (*action == "lock") {
      if (!config.isLockScreenEnabled()) {
        return "error: lock screen disabled\n";
      }
      if (lockScreen.lock()) {
        return "ok\n";
      }
      return "error: lock screen unavailable\n";
    }
    if (*action == "lock_and_suspend") {
      if (!config.isLockScreenEnabled()) {
        runner.invoke(builtinAction("suspend"));
        return "ok\n";
      }
      runner.invoke(builtinAction(*action));
      return "ok\n";
    }

    runner.invoke(builtinAction(*action));
    return "ok\n";
  };

  ipc.bind(noctalia::cli::msg::session, dispatch);
}

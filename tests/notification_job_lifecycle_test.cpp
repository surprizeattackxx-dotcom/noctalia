#include "dbus/notification/kde_job_view_server.h"
#include "notification/notification_history_store.h"
#include "notification/notification_manager.h"
#include "tests/test_check.h"

#include <iostream>
#include <string>
#include <vector>

struct KdeJobViewServerTestAccess {
  static auto makeFolderAction(const std::string& folder) { return KdeJobViewServer::makeOpenFolderAction(folder); }
  static auto folderFromAction(std::string_view key) { return KdeJobViewServer::folderFromAction(key); }
};

int main() {
  NotificationManager manager;
  NotificationRequest request{
      .appName = "job-test",
      .summary = "Copying",
      .origin = NotificationOrigin::Internal,
      .actions = {"kde-job-cancel", "Cancel"},
      .coalesce = false,
  };
  const auto first = manager.addOrReplace(request);
  const auto second = manager.addOrReplace(request);
  TEST_CHECK(first != second);
  TEST_CHECK(manager.all().size() == 2);
  request.replacesId = first;
  request.summary = "First job updated";
  TEST_CHECK(manager.addOrReplace(request) == first);
  TEST_CHECK(manager.all().size() == 2);
  TEST_CHECK(manager.all().back().id == second);

  // Default duplicate suppression remains available to non-job callers.
  request.replacesId = 0;
  request.summary = "ordinary duplicate";
  request.coalesce = true;
  TEST_CHECK(manager.addOrReplace(request) == manager.addOrReplace(request));

  // The folder payload lives in the notification's own action. Expiry must preserve it until the
  // manager stops accepting history actions; no separate unbounded id-to-folder map is needed.
  const std::string folder = "/home/example/target folder";
  const std::string folderAction = KdeJobViewServerTestAccess::makeFolderAction(folder);
  TEST_CHECK(KdeJobViewServerTestAccess::folderFromAction(folderAction) == folder);
  TEST_CHECK(!KdeJobViewServerTestAccess::folderFromAction("kde-job-cancel").has_value());
  TEST_CHECK(!KdeJobViewServerTestAccess::folderFromAction("kde-job-open-folder:").has_value());
  request.summary = "Finished";
  request.persistInHistory = true;
  request.actions = {folderAction, "Open folder"};
  const auto finished = manager.addOrReplace(request);
  std::vector<std::string> dispatched;
  int calendarCalls = 0;
  manager.setInternalActionCallback([&](auto, const auto&, const auto&, const auto&) { ++calendarCalls; });
  const int handler = manager.addInternalActionHandler([&](auto, const auto& key, const auto&, const auto&) {
    dispatched.push_back(key);
  });
  TEST_CHECK(manager.close(finished, CloseReason::Expired));
  TEST_CHECK(manager.hasPendingDBusClose(finished));
  TEST_CHECK(
      KdeJobViewServerTestAccess::folderFromAction(manager.findHistoryEntry(finished)->notification.actions.front())
      == folder
  );
  TEST_CHECK(manager.invokeAction(finished, folderAction, false));
  TEST_CHECK(dispatched == std::vector<std::string>{folderAction});
  TEST_CHECK(calendarCalls == 1);
  TEST_CHECK(manager.invokeAction(finished, folderAction));
  TEST_CHECK(!manager.invokeAction(finished, folderAction));
  TEST_CHECK(!manager.hasPendingDBusClose(finished));
  manager.removeInternalActionHandler(handler);
  manager.removeHistoryEntry(finished);
  TEST_CHECK(manager.findHistoryEntry(finished) == nullptr);
  TEST_CHECK(!manager.invokeAction(finished, folderAction));

  // History removal, clearing, size eviction and retention expiry must end invokability and release
  // the action payload together. None requires the job server to track a second lifetime.
  request.coalesce = false;
  const auto removed = manager.addOrReplace(request);
  TEST_CHECK(manager.close(removed, CloseReason::Expired));
  manager.removeHistoryEntry(removed);
  TEST_CHECK(!manager.hasPendingDBusClose(removed));
  TEST_CHECK(!manager.invokeAction(removed, folderAction));

  const auto cleared = manager.addOrReplace(request);
  TEST_CHECK(manager.close(cleared, CloseReason::Expired));
  manager.clearHistory();
  TEST_CHECK(!manager.hasPendingDBusClose(cleared));
  TEST_CHECK(!manager.invokeAction(cleared, folderAction));

  const auto evicted = manager.addOrReplace(request);
  TEST_CHECK(manager.close(evicted, CloseReason::Expired));
  for (std::size_t i = 0; i < kNotificationHistoryMaxEntries; ++i) {
    const auto id = manager.addOrReplace(request);
    TEST_CHECK(manager.close(id, CloseReason::Expired));
  }
  TEST_CHECK(manager.history().size() == kNotificationHistoryMaxEntries);
  TEST_CHECK(manager.findHistoryEntry(evicted) == nullptr);
  TEST_CHECK(!manager.hasPendingDBusClose(evicted));
  TEST_CHECK(!manager.invokeAction(evicted, folderAction));
  const auto retained = manager.history().back().notification.id;
  TEST_CHECK(manager.invokeAction(retained, folderAction, false));
  auto* oldEntry = const_cast<NotificationHistoryEntry*>(manager.findHistoryEntry(retained));
  oldEntry->notification.receivedWallClock = WallClock::now() - std::chrono::hours(2);
  manager.setHistoryRetentionHours(1);
  TEST_CHECK(manager.findHistoryEntry(retained) == nullptr);
  TEST_CHECK(!manager.hasPendingDBusClose(retained));
  TEST_CHECK(!manager.invokeAction(retained, folderAction));

  std::cout << "PASS: independent job ids, explicit replacement, default coalescing, expired internal history action "
               "and cleanup\n";
}

#include "capture/kwin_window_capture.h"
#include "core/deferred_call.h"
#include "dbus/session_bus.h"
#include "tests/test_check.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  using Options = std::map<std::string, sdbus::Variant>;
  using namespace std::chrono_literals;

  // Tiny opaque RGBA fixture with distinct pixels and padded rows.
  const std::vector<std::uint8_t> kPixels{
      255, 0,  0,  255, 0,  255, 0,  255, 0,  0,  255, 255, 255, 255, 255, 255,
      10,  20, 30, 255, 40, 50,  60, 255, 70, 80, 90,  255, 100, 110, 120, 255,
  };

  class FakeKwin {
  public:
    struct Request {
      std::string method;
      std::string target;
      bool cursor;
      bool native;
      bool decoration;
      bool shadow;
    };
    FakeKwin()
        : connection(sdbus::createSessionBusConnection(sdbus::ServiceName{"org.kde.KWin"})),
          object(sdbus::createObject(*connection, sdbus::ObjectPath{"/org/kde/KWin/ScreenShot2"})) {
      object
          ->addVTable(
              sdbus::registerMethod("CaptureScreen")
                  .implementedAs([this](const std::string& target, const Options& options, const sdbus::UnixFd& fd) {
                    return reply("CaptureScreen", target, options, fd);
                  }),
              sdbus::registerMethod("CaptureWindow")
                  .implementedAs([this](const std::string& target, const Options& options, const sdbus::UnixFd& fd) {
                    return reply("CaptureWindow", target, options, fd);
                  })
          )
          .forInterface("org.kde.KWin.ScreenShot2");
      connection->enterEventLoopAsync();
    }
    ~FakeKwin() { connection->leaveEventLoop(); }

    std::atomic<int> calls{0};
    Request lastRequest() {
      const std::lock_guard lock(mutex);
      return requests.back();
    }

  private:
    Options
    reply(const std::string& method, const std::string& target, const Options& options, const sdbus::UnixFd& fd) {
      {
        const std::lock_guard lock(mutex);
        requests.push_back(
            {method, target, options.at("include-cursor").get<bool>(), options.at("native-resolution").get<bool>(),
             options.at("include-decoration").get<bool>(), options.at("include-shadow").get<bool>()}
        );
      }
      ++calls;
      if (target == "DENIED") {
        throw sdbus::Error(sdbus::Error::Name{"org.kde.KWin.ScreenShot2.Error.NoAuthorized"}, "Capture denied");
      }
      std::vector<std::uint8_t> padded;
      for (std::size_t row = 0; row < 2; ++row) {
        padded.insert(
            padded.end(), kPixels.begin() + static_cast<std::ptrdiff_t>(row * 16),
            kPixels.begin() + static_cast<std::ptrdiff_t>((row + 1) * 16)
        );
        padded.insert(padded.end(), 4, 0xaa);
      }
      const auto written = ::write(fd.get(), padded.data(), padded.size());
      // The client is allowed to close its pipe while the already-sent request is being served.
      TEST_CHECK(written == static_cast<ssize_t>(padded.size()) || (written < 0 && errno == EPIPE));
      return {
          {"width", sdbus::Variant{std::uint32_t{4}}},
          {"height", sdbus::Variant{std::uint32_t{2}}},
          {"stride", sdbus::Variant{std::uint32_t{20}}},
          {"format", sdbus::Variant{std::uint32_t{17}}}
      };
    }
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IObject> object;
    std::mutex mutex;
    std::vector<Request> requests;
  };

  void pump(SessionBus& bus) {
    bus.processPendingEvents();
    for (auto& callback : DeferredCall::takePending()) {
      callback();
    }
  }

  template <typename Predicate> bool until(SessionBus& bus, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      pump(bus);
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }
} // namespace

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  // Meson sets this marker only inside dbus-run-session. Refuse direct invocation
  // before opening any connection, so the fake cannot acquire a live KWin name.
  TEST_CHECK(std::getenv("NOCTALIA_KWIN_TEST_ISOLATED") != nullptr);
  TEST_CHECK(std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr);
  FakeKwin kwin;
  SessionBus bus;
  KwinWindowCapture capture(bus);
  const auto mainThread = std::this_thread::get_id();
  bool completed = false;
  capture.captureScreen("DP-FAKE-1", true, [&](std::optional<ScreencopyImage> image, std::string error) {
    TEST_CHECK(std::this_thread::get_id() == mainThread);
    TEST_CHECK(error.empty());
    TEST_CHECK(image.has_value());
    TEST_CHECK(image->width == 4 && image->height == 2);
    TEST_CHECK(image->rgba == kPixels);
    completed = true;
  });
  TEST_CHECK(capture.busy());
  TEST_CHECK(until(bus, [&] { return completed; }));
  TEST_CHECK(!capture.busy());
  TEST_CHECK(kwin.calls.load() == 1);
  auto request = kwin.lastRequest();
  TEST_CHECK(request.method == "CaptureScreen" && request.target == "DP-FAKE-1");
  TEST_CHECK(request.cursor && request.native);
  std::cout << "PASS screen connector/cursor/native dimensions/pixels/main-thread callback\n";

  completed = false;
  capture.captureWindow("{fake-window-id}", false, [&](std::optional<ScreencopyImage> image, std::string error) {
    TEST_CHECK(std::this_thread::get_id() == mainThread);
    TEST_CHECK(error.empty() && image.has_value());
    TEST_CHECK(image->width == 4 && image->height == 2);
    TEST_CHECK(image->rgba == kPixels);
    completed = true;
  });
  TEST_CHECK(until(bus, [&] { return completed; }));
  request = kwin.lastRequest();
  TEST_CHECK(request.method == "CaptureWindow" && request.target == "{fake-window-id}");
  TEST_CHECK(!request.cursor && request.native && request.decoration && !request.shadow);
  std::cout << "PASS full native window capture/no upscaling/cursor disabled\n";
  // No upscaling when bounds are 0: capture(windowId, 0, 0) must deliver native
  // pixels (4x2), never scaled up; positive bounds only downscale.
  bool boundsCompleted = false;
  capture.capture("DP-FAKE-2", 0, 0, [&](std::optional<ScreencopyImage> image, std::string error) {
    TEST_CHECK(error.empty());
    TEST_CHECK(image.has_value());
    TEST_CHECK(image->width == 4 && image->height == 2);
    TEST_CHECK(image->rgba == kPixels);
    boundsCompleted = true;
  });
  TEST_CHECK(until(bus, [&] { return boundsCompleted; }));
  TEST_CHECK(kwin.calls.load() == 3);
  request = kwin.lastRequest();
  TEST_CHECK(request.method == "CaptureWindow" && request.target == "DP-FAKE-2");
  TEST_CHECK(!request.cursor && request.native && request.decoration && !request.shadow);
  std::cout << "PASS bounds0/no-upscale/cursor-disabled flag propagation\n";

  // Cancellation: an in-flight screen capture whose job is cancelled must not
  // deliver a callback and must not block a follow-up capture.
  bool boomFired = false;
  capture.captureScreen("DP-FAKE-1", true, [&](std::optional<ScreencopyImage>, std::string) { boomFired = true; });
  TEST_CHECK(capture.busy());
  capture.cancelInFlight();
  TEST_CHECK(!capture.busy());
  bool retriedFired = false;
  capture.captureScreen("DP-FAKE-3", true, [&](std::optional<ScreencopyImage>, std::string) { retriedFired = true; });
  TEST_CHECK(until(bus, [&] { return retriedFired; }));
  TEST_CHECK(!boomFired);
  // Cancelling suppresses delivery, not an already-sent D-Bus request.
  TEST_CHECK(kwin.lastRequest().target == "DP-FAKE-3");

  // Destruction: a capture object torn down mid-flight must not deliver a
  // callback, and the capture API must remain usable afterwards.
  bool deadFired = false;
  {
    KwinWindowCapture deadCapture(bus);
    deadCapture.captureScreen("DP-FAKE-4", true, [&](std::optional<ScreencopyImage>, std::string) {
      deadFired = true;
    });
  }
  TEST_CHECK(!deadFired);
  bool finalFired = false;
  capture.captureScreen("DP-FAKE-5", true, [&](std::optional<ScreencopyImage>, std::string) { finalFired = true; });
  TEST_CHECK(until(bus, [&] { return finalFired; }));
  TEST_CHECK(!deadFired);

  completed = false;
  capture.capture("{bounded-window}", 2, 1, [&](std::optional<ScreencopyImage> image, std::string error) {
    TEST_CHECK(error.empty() && image.has_value());
    TEST_CHECK(image->width == 2 && image->height == 1);
    completed = true;
  });
  TEST_CHECK(until(bus, [&] { return completed; }));

  completed = false;
  capture.captureScreen("DENIED", false, [&](std::optional<ScreencopyImage> image, std::string error) {
    TEST_CHECK(!image.has_value());
    TEST_CHECK(error == "Capture denied");
    completed = true;
  });
  TEST_CHECK(until(bus, [&] { return completed; }));
  TEST_CHECK(!capture.available());
  TEST_CHECK(!capture.busy());

  std::cout << "PASS kwin bounds0/no-upscale flag propagation cancel no-callback destruction\n";
}

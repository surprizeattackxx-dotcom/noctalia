#include "capture/screenshot_capture.h"
#include "dbus/session_bus.h"
#include "tests/test_check.h"
#include "wayland/wayland_connection.h"

#include <cstdlib>
#include <optional>
#include <string>

int main() {
  // Meson runs this executable through dbus-run-session; it must not use the desktop bus.
  TEST_CHECK(std::getenv("NOCTALIA_TEST_PRIVATE_BUS") != nullptr);
  WaylandConnection wayland;
  ScreenshotCapture capture(wayland);
  TEST_CHECK(!capture.available());
  SessionBus bus;
  capture.setKwinSessionBus(&bus);
  TEST_CHECK(capture.available());
  bool completed = false;
  capture.capture(
      nullptr, std::nullopt, false, false, [&](std::optional<capture::ScreenshotImage> image, std::string error) {
        TEST_CHECK(!image.has_value());
        TEST_CHECK(!error.empty());
        completed = true;
      }
  );
  TEST_CHECK(completed);
  TEST_CHECK(!capture.busy());
  capture.setKwinSessionBus(nullptr);
  TEST_CHECK(!capture.available());
  return EXIT_SUCCESS;
}

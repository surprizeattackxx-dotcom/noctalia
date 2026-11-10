#include "capture/screenshot_service.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "dbus/session_bus.h"
#include "notification/notification_manager.h"
#include "render/render_context.h"
#include "tests/test_check.h"
#include "ui/controls/flex.h"
#include "wayland/wayland_connection.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

  void drainDeferred() {
    for (;;) {
      auto pending = DeferredCall::takePending();
      if (pending.empty()) {
        return;
      }
      for (auto& call : pending) {
        call();
      }
    }
  }

  class Sandbox {
  public:
    Sandbox() {
      std::string pattern = (std::filesystem::temp_directory_path() / "noctalia-screenshot-settings-XXXXXX").string();
      TEST_CHECK(::mkdtemp(pattern.data()) != nullptr);
      root = pattern;
      for (const auto* dir : {"config", "state", "data"}) {
        std::filesystem::create_directories(root / dir / "noctalia");
      }
      TEST_CHECK(::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1) == 0);
      TEST_CHECK(::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1) == 0);
      TEST_CHECK(::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1) == 0);
      TEST_CHECK(::setenv("XDG_CURRENT_DESKTOP", "NoctaliaTest", 1) == 0);
      TEST_CHECK(::unsetenv("WAYLAND_DISPLAY") == 0);
      TEST_CHECK(::unsetenv("WAYLAND_SOCKET") == 0);
    }
    ~Sandbox() { std::filesystem::remove_all(root); }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    std::filesystem::path root;
  };

  struct Fixture {
    WaylandConnection wayland;
    CompositorPlatform platform{wayland};
    ConfigService config;
    NotificationManager notifications;
    RenderContext renderer;
    SessionBus bus;
    ScreenshotService service{wayland, platform, config, notifications};
  };

} // namespace

class ScreenshotServiceTestAccess {
public:
  static void checkRecoverableExportFailure(const std::filesystem::path& root) {
    Fixture f;
    f.service.m_regionRenderContext = &f.renderer;
    f.service.ensureAnnotationOverlay();
    auto& overlay = *f.service.m_annotationOverlay;
    overlay.m_active = true;
    overlay.m_mode = capture::AnnotationMode::Image;
    ScreenshotService::OutputOptions options;
    const auto destination = root / "recovered.png";
    f.service.m_pendingDelivery = ScreenshotService::PendingDelivery{.options = options, .destPath = destination};
    // This is the same callback used by requestExport for missing backgrounds/empty composition.
    overlay.m_onFailure("Recoverable export composition failure");
    TEST_CHECK(overlay.isActive());
    TEST_CHECK(f.service.m_pendingDelivery.has_value());
    TEST_CHECK(f.service.m_pendingDelivery->destPath == destination);
    TEST_CHECK(f.notifications.all().back().body == "Recoverable export composition failure");
    overlay.dispatchExport(
        {.width = 2, .height = 2, .rgba = std::vector<std::uint8_t>(16, 255)}, capture::AnnotationExport::Done
    );
    drainDeferred();
    TEST_CHECK(!overlay.isActive());
    TEST_CHECK(!f.service.m_pendingDelivery.has_value());
    TEST_CHECK(std::filesystem::is_regular_file(destination));
    // Fatal overlay errors still cancel the session and discard its pending delivery.
    overlay.m_active = true;
    f.service.m_pendingDelivery = ScreenshotService::PendingDelivery{.options = options, .destPath = destination};
    overlay.abortWithError("Fatal overlay failure");
    drainDeferred();
    TEST_CHECK(!overlay.isActive());
    TEST_CHECK(!f.service.m_pendingDelivery.has_value());
  }

  static void checkRememberedRegionHint() {
    Fixture f;
    capture::ScreenshotRegionOverlay overlay;
    overlay.initialize(f.wayland, &f.renderer);
    overlay.begin(false, false, false, LogicalRect{.x = 0, .y = 0, .width = 10, .height = 10});
    TEST_CHECK(overlay.m_confirming);
    TEST_CHECK(!overlay.m_confirmRegion);
    Label* label = nullptr;
    auto hint = overlay.buildSelectionHint(label);
    TEST_CHECK(hint != nullptr);
    TEST_CHECK(label != nullptr);
    overlay.cancel();
    overlay.begin(false, true, false);
    TEST_CHECK(overlay.buildSelectionHint(label) == nullptr);
  }

  static void checkDoneDelivery(const std::filesystem::path& root) {
    Fixture f;
    f.service.m_regionRenderContext = &f.renderer;
    f.service.ensureAnnotationOverlay();
    auto& overlay = *f.service.m_annotationOverlay;
    overlay.m_active = true;
    overlay.m_mode = capture::AnnotationMode::Image;
    ScreenshotService::OutputOptions options;
    const auto blocker = root / "not-a-directory";
    const auto destination = blocker / "edited.png";
    {
      std::ofstream file(blocker);
      file << "block parent directory creation";
    }
    f.service.m_pendingDelivery = ScreenshotService::PendingDelivery{.options = options, .destPath = destination};
    const auto image = [] {
      return ScreencopyImage{.width = 2, .height = 2, .rgba = std::vector<std::uint8_t>(16, 255)};
    };
    overlay.dispatchExport(image(), capture::AnnotationExport::Done);
    drainDeferred();
    TEST_CHECK(overlay.isActive());
    TEST_CHECK(f.service.m_pendingDelivery.has_value());
    TEST_CHECK(f.service.m_pendingDelivery->destPath == destination);
    TEST_CHECK(!std::filesystem::exists(destination));
    std::filesystem::remove(blocker);
    overlay.dispatchExport(image(), capture::AnnotationExport::Done);
    drainDeferred();
    TEST_CHECK(!overlay.isActive());
    TEST_CHECK(!f.service.m_pendingDelivery.has_value());
    TEST_CHECK(std::filesystem::is_regular_file(destination));
  }

  static void checkFrozenDisplayFilename(const std::filesystem::path& root) {
    Fixture f;
    // Disconnected metadata only: the token is never used as a Wayland proxy.
    int outputToken = 0;
    auto* output = reinterpret_cast<wl_output*>(&outputToken);
    auto& outputs = const_cast<std::vector<WaylandOutput>&>(f.wayland.outputs());
    WaylandOutput metadata;
    metadata.output = output;
    metadata.logicalWidth = 2;
    metadata.logicalHeight = 2;
    outputs.push_back(metadata);
    const auto freeze = [&] {
      f.service.m_frozenScreenshots.push_back(
          capture::FrozenScreenshot{
              .output = output,
              .image = {.image = {.width = 2, .height = 2, .rgba = std::vector<std::uint8_t>(16, 255)}}
          }
      );
    };
    ScreenshotService::OutputOptions options;
    options.directory = root.string();
    options.filenamePattern = "display";
    options.freezeScreen = true;
    freeze();
    f.service.completeFullscreenSelection(output, options);
    TEST_CHECK(std::filesystem::is_regular_file(root / "display.png"));
    TEST_CHECK(!std::filesystem::exists(root / "display-region.png"));
    options.filenamePattern = "region";
    freeze();
    f.service.deliverFrozenRegion({.x = 0, .y = 0, .width = 2, .height = 2}, output, options);
    TEST_CHECK(std::filesystem::is_regular_file(root / "region-region.png"));
    outputs.clear();
  }

  static void checkFullscreenAnnotateOnly() {
    ScreenshotService::OutputOptions options;
    options.saveToFile = false;
    options.copyToClipboard = false;
    options.annotate = true;
    options.freezeScreen = true;
    for (int path = 0; path < 3; ++path) {
      Fixture f;
      f.service.m_capture.setKwinSessionBus(&f.bus);
      TEST_CHECK(f.service.available());
      if (path == 0) {
        f.service.captureFullscreen(options);
      } else if (path == 1) {
        f.service.captureFullscreenInteractive(f.renderer, options);
      } else {
        f.service.beginFullscreenCapture(f.renderer, options);
      }
      drainDeferred();
      TEST_CHECK(f.notifications.all().size() == 1);
      // A disconnected fixture has no outputs, but annotation is a valid destination.
      TEST_CHECK(f.notifications.all().back().body == "No outputs available");
    }
    Fixture f;
    f.service.m_capture.setKwinSessionBus(&f.bus);
    options.annotate = false;
    f.service.captureFullscreen(options);
    TEST_CHECK(f.notifications.all().back().body == "No screenshot output enabled");
  }
};

int main(int argc, char** argv) {
  TEST_CHECK(std::getenv("NOCTALIA_TEST_PRIVATE_BUS") != nullptr);
  Sandbox sandbox;
  TEST_CHECK(argc == 2);
  const std::string_view test = argv[1];
  if (test == "fullscreen") {
    ScreenshotServiceTestAccess::checkFullscreenAnnotateOnly();
  } else if (test == "filename") {
    ScreenshotServiceTestAccess::checkFrozenDisplayFilename(sandbox.root);
  } else if (test == "done") {
    ScreenshotServiceTestAccess::checkDoneDelivery(sandbox.root);
  } else if (test == "hint") {
    ScreenshotServiceTestAccess::checkRememberedRegionHint();
  } else if (test == "recoverable") {
    ScreenshotServiceTestAccess::checkRecoverableExportFailure(sandbox.root);
  } else {
    TEST_CHECK(false);
  }
  return EXIT_SUCCESS;
}

#include "pipewire/pipewire_service.h"
#include "shell/bar/widgets/taskbar_preview_policy.h"
#include "test_check.h"

#include <vector>

namespace {
  AudioNode stream(std::uint32_t id, std::string name, std::string appId, std::string binary) {
    AudioNode node;
    node.id = id;
    node.applicationName = std::move(name);
    node.applicationId = std::move(appId);
    node.applicationBinary = std::move(binary);
    return node;
  }
} // namespace

int main() {
  using noctalia::taskbar::matchAudioStreams;
  const std::vector<AudioNode> streams{
      stream(10, "Brave", "", "brave"),
      stream(11, "Brave", "", "brave"),
      stream(12, "Firefox", "org.mozilla.firefox", "firefox"),
      stream(13, "Spotify", "", "spotify"),
      stream(14, "ALSA plug-in [wine64-preloader]", "", "wine64-preloader"),
      stream(15, "", "", ""),
  };
  // Window app ids from the compositor differ in form from PipeWire identities.
  TEST_CHECK((matchAudioStreams(streams, "brave-browser") == std::vector<std::uint32_t>{10, 11}));
  TEST_CHECK((matchAudioStreams(streams, "Brave-Browser.desktop") == std::vector<std::uint32_t>{10, 11}));
  TEST_CHECK((matchAudioStreams(streams, "firefox") == std::vector<std::uint32_t>{12}));
  TEST_CHECK((matchAudioStreams(streams, "org.mozilla.firefox") == std::vector<std::uint32_t>{12}));
  TEST_CHECK((matchAudioStreams(streams, "spotify") == std::vector<std::uint32_t>{13}));
  // No false positives: unrelated, empty, too-short or runtime-launcher identities never match.
  TEST_CHECK(matchAudioStreams(streams, "kate").empty());
  TEST_CHECK(matchAudioStreams(streams, "").empty());
  TEST_CHECK(matchAudioStreams(streams, "br").empty());
  TEST_CHECK(matchAudioStreams(streams, "steam_app_123").empty());
  TEST_CHECK(matchAudioStreams(streams, "wine64-preloader").empty());
  using noctalia::taskbar::PreviewHover;
  PreviewHover hover;
  TEST_CHECK(!hover.shouldShow());
  TEST_CHECK(hover.shouldClose());
  hover.enterAnchor();
  TEST_CHECK(hover.shouldShow());
  TEST_CHECK(!hover.shouldClose());
  hover.leaveAnchor();
  TEST_CHECK(!hover.shouldShow());
  TEST_CHECK(hover.shouldClose());
  hover.enterPopup();
  TEST_CHECK(!hover.shouldClose());
  hover.enterAnchor();
  hover.leavePopup();
  TEST_CHECK(!hover.shouldClose());
  hover.leaveAnchor();
  TEST_CHECK(hover.shouldClose());
  hover.enterPopup();
  hover.cancel();
  TEST_CHECK(hover.shouldClose());
  TEST_CHECK(!hover.shouldShow());
  return 0;
}
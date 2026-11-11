#pragma once

#include "pipewire/pipewire_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::taskbar {

  namespace detail {
    inline std::string normalizeAudioIdentity(std::string_view value) {
      std::string out(value);
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      constexpr std::string_view kDesktop = ".desktop";
      if (out.size() > kDesktop.size() && out.ends_with(kDesktop)) {
        out.resize(out.size() - kDesktop.size());
      }
      return out;
    }

    // Wine/Proton and similar runtimes report a shared launcher, never the app itself.
    inline bool isRuntimeLauncher(std::string_view id) {
      return id.starts_with("wine")
          || id.starts_with("proton")
          || id.starts_with("steam_app_")
          || id.starts_with("pressure-vessel");
    }

    inline std::string lastDottedComponent(const std::string& id) {
      const auto dot = id.rfind('.');
      return dot == std::string::npos ? id : id.substr(dot + 1);
    }

    inline bool sameApp(const std::string& window, const std::string& stream) {
      constexpr std::size_t kMinLength = 3;
      if (window.size() < kMinLength
          || stream.size() < kMinLength
          || isRuntimeLauncher(window)
          || isRuntimeLauncher(stream)) {
        return false;
      }
      const auto leaf = [](const std::string& s) { return lastDottedComponent(s); };
      if (window == stream || leaf(window) == leaf(stream)) {
        return true;
      }
      // "brave-browser" (window) is the same app as "brave" (stream binary), and vice versa.
      const auto prefixed = [](const std::string& longer, const std::string& shorter) {
        return longer.size() > shorter.size()
            && longer.starts_with(shorter)
            && (longer[shorter.size()] == '-' || longer[shorter.size()] == '_' || longer[shorter.size()] == '.');
      };
      return prefixed(window, stream) || prefixed(stream, window);
    }
  } // namespace detail

  // Ids of the playback streams that belong to the application behind a window, so one preview
  // slider can drive all of them. Empty when the app is silent or its identity is ambiguous.
  inline std::vector<std::uint32_t> matchAudioStreams(const std::vector<AudioNode>& streams, std::string_view appId) {
    std::vector<std::uint32_t> ids;
    const std::string window = detail::normalizeAudioIdentity(appId);
    if (window.empty()) {
      return ids;
    }
    for (const auto& stream : streams) {
      const std::array<const std::string*, 3> fields{
          &stream.applicationId, &stream.applicationBinary, &stream.applicationName
      };
      const bool match = std::ranges::any_of(fields, [&](const std::string* field) {
        return !field->empty() && detail::sameApp(window, detail::normalizeAudioIdentity(*field));
      });
      if (match) {
        ids.push_back(stream.id);
      }
    }
    return ids;
  }

  // The icon and popup have separate Wayland pointer surfaces. A delayed close
  // checks both after the handoff grace period, rather than dismissing on leave.
  class PreviewHover {
  public:
    void enterAnchor() noexcept { m_anchor = true; }
    void leaveAnchor() noexcept { m_anchor = false; }
    void enterPopup() noexcept { m_popup = true; }
    void leavePopup() noexcept { m_popup = false; }
    void cancel() noexcept { m_anchor = m_popup = false; }
    bool shouldShow() const noexcept { return m_anchor; }
    bool shouldClose() const noexcept { return !m_anchor && !m_popup; }

  private:
    bool m_anchor = false;
    bool m_popup = false;
  };

} // namespace noctalia::taskbar

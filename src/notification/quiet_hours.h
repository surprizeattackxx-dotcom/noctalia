#pragma once

#include <charconv>
#include <optional>
#include <string_view>

/// Scheduled Do Not Disturb: pure helpers, so the schedule can be tested without a clock.
namespace quiet_hours {

  /// "HH:MM" (24-hour, also "H:MM") to minutes after midnight; empty when malformed or out of range.
  [[nodiscard]] inline std::optional<int> parseClockMinutes(std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 2 || text.size() != colon + 3) {
      return std::nullopt;
    }
    int hours = 0;
    int minutes = 0;
    const auto h = std::from_chars(text.data(), text.data() + colon, hours);
    const auto m = std::from_chars(text.data() + colon + 1, text.data() + text.size(), minutes);
    if (h.ec != std::errc{}
        || h.ptr != text.data() + colon
        || m.ec != std::errc{}
        || m.ptr != text.data() + text.size()
        || hours < 0
        || hours > 23
        || minutes < 0
        || minutes > 59) {
      return std::nullopt;
    }
    return hours * 60 + minutes;
  }

  /// True when `now` falls in [start, end). A window whose end is before its start runs past midnight; equal
  /// start and end is an empty window.
  [[nodiscard]] constexpr bool contains(int startMinutes, int endMinutes, int nowMinutes) noexcept {
    if (startMinutes == endMinutes) {
      return false;
    }
    if (startMinutes < endMinutes) {
      return nowMinutes >= startMinutes && nowMinutes < endMinutes;
    }
    return nowMinutes >= startMinutes || nowMinutes < endMinutes;
  }

} // namespace quiet_hours

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct AudioCardProfile {
  std::string name;
  std::string description;
  bool available = true;
  bool active = false;
};

struct AudioCardInfo {
  std::string name; // e.g. alsa_card.pci-0000_00_1f.3
  std::string description;
  std::vector<AudioCardProfile> profiles;
};

// Looks up the sound card (PipeWire device) with the given object id and lists its profiles.
// Uses `pactl -f json list cards` (pipewire-pulse); returns nullopt when pactl is missing or the card
// is unknown. Profiles that are unavailable (e.g. an unplugged HDMI port) and "off" are left out, the
// active profile is always kept.
[[nodiscard]] std::optional<AudioCardInfo> queryAudioCardProfiles(std::uint32_t deviceId);

// `pactl set-card-profile`; runs detached.
[[nodiscard]] bool setAudioCardProfile(const std::string& cardName, const std::string& profileName);

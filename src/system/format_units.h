#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace FormatUnits {

  /// Shell-wide temperature unit for hardware readouts (set from system.monitor.temperature_unit).
  void setTemperatureFahrenheit(bool fahrenheit);
  /// "65°C" or "149°F", rounded to whole degrees.
  [[nodiscard]] std::string formatTemperatureFromCelsius(double celsius);

  inline constexpr int kMinCompactByteRateDecimalPlaces = 0;
  inline constexpr int kMaxCompactByteRateDecimalPlaces = 3;

  enum class DecimalByteRateUnit {
    Auto,
    Kilobytes,
    Megabytes,
  };
  enum class ByteRateLabelStyle {
    Full,
    Compact,
  };

  std::string formatBinaryMib(std::uint64_t mib);
  std::string formatBinaryMibAsGib(std::uint64_t mib);
  std::string formatBinaryMibUsageAsGib(std::uint64_t usedMib, std::uint64_t totalMib);
  std::string formatBinaryBytesAsGib(std::uint64_t bytes);
  std::string formatDecimalBytesUsage(double usedBytes, double totalBytes);
  std::string formatDecimalBytesAsGb(double bytes);
  DecimalByteRateUnit decimalByteRateUnitFromString(std::string_view value);
  std::string formatDecimalBytesPerSecond(
      double bytesPerSec, DecimalByteRateUnit unit = DecimalByteRateUnit::Auto,
      ByteRateLabelStyle labelStyle = ByteRateLabelStyle::Full, int compactDecimalPlaces = 1
  );

} // namespace FormatUnits

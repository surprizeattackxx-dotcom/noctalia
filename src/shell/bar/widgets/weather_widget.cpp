#include "shell/bar/widgets/weather_widget.h"

#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/tooltip_content.h"
#include "system/weather_service.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <optional>
#include <string>

namespace {

  std::string windDirectionLabel(int degrees) { return WeatherService::compassLabel(degrees); }

  // Expands {token} placeholders from the current snapshot. Unknown tokens are left as written so a typo
  // is visible in the bar instead of silently vanishing; unavailable values read "--".
  std::string expandFormat(const std::string& format, const WeatherService& weather) {
    const auto& snapshot = weather.snapshot();
    const auto& cur = snapshot.current;
    const auto today = snapshot.forecastDays.empty() ? nullptr : &snapshot.forecastDays.front();
    const auto value = [&](const std::string& token) -> std::optional<std::string> {
      if (token == "temp") {
        return weather.formatTemperature(cur.temperatureC);
      }
      if (token == "feels") {
        return cur.apparentTemperatureC ? weather.formatTemperature(*cur.apparentTemperatureC) : "--";
      }
      if (token == "high") {
        return today != nullptr ? weather.formatTemperature(today->temperatureMaxC) : "--";
      }
      if (token == "low") {
        return today != nullptr ? weather.formatTemperature(today->temperatureMinC) : "--";
      }
      if (token == "dew") {
        return cur.dewPointC ? weather.formatTemperature(*cur.dewPointC) : "--";
      }
      if (token == "humidity") {
        return std::format("{}%", cur.relativeHumidityPercent);
      }
      if (token == "wind") {
        return std::format("{} {}", weather.formatWind(cur.windSpeedKmh), windDirectionLabel(cur.windDirectionDeg));
      }
      if (token == "gust") {
        return cur.windGustKmh ? weather.formatWind(*cur.windGustKmh) : "--";
      }
      if (token == "pressure") {
        return cur.pressureHpa ? weather.formatPressure(*cur.pressureHpa) : "--";
      }
      if (token == "uv") {
        return std::format("{:.1F}", cur.uvIndex);
      }
      if (token == "aqi") {
        return snapshot.airQuality.valid ? std::to_string(snapshot.airQuality.usAqi) : "--";
      }
      if (token == "cloud") {
        return cur.cloudCoverPercent >= 0 ? std::format("{}%", cur.cloudCoverPercent) : "--";
      }
      if (token == "visibility") {
        return cur.visibilityM ? weather.formatVisibility(*cur.visibilityM) : "--";
      }
      if (token == "rain") {
        return today != nullptr && today->precipitationProbabilityMaxPercent >= 0
            ? std::format("{}%", today->precipitationProbabilityMaxPercent)
            : "--";
      }
      if (token == "moon") {
        return WeatherService::moonPhase(std::chrono::system_clock::now()).name;
      }
      if (token == "condition") {
        return WeatherService::shortDescriptionForCode(cur.weatherCode);
      }
      if (token == "location") {
        return snapshot.locationName;
      }
      return std::nullopt;
    };

    std::string out;
    for (std::size_t i = 0; i < format.size(); ++i) {
      if (format[i] == '{') {
        if (const auto close = format.find('}', i); close != std::string::npos) {
          const std::string token = format.substr(i + 1, close - i - 1);
          if (const auto expanded = value(token); expanded.has_value()) {
            out += *expanded;
            i = close;
            continue;
          }
        }
      }
      out += format[i];
    }
    return out;
  }

} // namespace

WeatherWidget::WeatherWidget(WeatherService* weather, wl_output* /*output*/, Options options)
    : m_weather(weather), m_maxWidth(static_cast<float>(options.maxWidth)), m_showCondition(options.showCondition),
      m_showTemperature(options.showTemperature), m_glyphPosition(options.glyphPosition),
      m_format(std::move(options.format)) {
  setPeek(options.peek, options.peekDelayMs);
}

void WeatherWidget::create() {
  auto area = ui::inputArea({});
  m_area = area.get();

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "weather-cloud",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .maxWidth = m_maxWidth * m_contentScale,
          .maxLines = 1,
      })
  );

  setRoot(std::move(area));
}

void WeatherWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_glyph == nullptr || m_label == nullptr || root() == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  sync(renderer);

  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_glyph->measure(renderer);
  m_label->setTextAlign(m_isVertical ? TextAlign::Center : TextAlign::Start);
  m_label->setMaxWidth(m_isVertical ? containerWidth : (m_maxWidth * m_contentScale));
  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  m_label->measure(renderer);

  const float spacing = m_label->text().empty() ? 0.0F : (Style::spaceXs * m_contentScale);
  layoutGlyphValue({
      .glyph = m_glyph,
      .value = m_label->text().empty() ? nullptr : m_label,
      .glyphPosition = m_glyphPosition,
      .vertical = m_isVertical,
      .horizontalGap = spacing,
      .verticalGap = spacing,
  });
}

void WeatherWidget::doUpdate(Renderer& renderer) { sync(renderer); }

void WeatherWidget::sync(Renderer& renderer) {
  if (m_glyph == nullptr || m_label == nullptr) {
    return;
  }

  constexpr const char* kVerticalUnavailable = "-";
  constexpr const char* kVerticalLoading = "...";

  auto verticalTemperature = [](int temp) { return std::format("{}\xC2\xB0", temp); };

  std::string glyph = "weather-cloud";
  std::string text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.weather.default");

  if (m_weather == nullptr || !m_weather->enabled()) {
    text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.weather.off");
  } else if (!m_weather->locationConfigured()) {
    text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.weather.no-location");
  } else if (m_weather->hasData()) {
    const auto& snapshot = m_weather->snapshot();
    glyph = WeatherService::glyphForCode(snapshot.current.weatherCode, snapshot.current.isDay);
    const int temp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
    const std::string unit = m_weather->displayTemperatureUnit();
    if (!m_format.empty() && !m_isVertical) {
      text = expandFormat(m_format, *m_weather);
    } else if (m_showTemperature) {
      if (m_isVertical) {
        text = verticalTemperature(temp);
      } else {
        text = std::format("{}{}", temp, unit);
      }
    } else {
      text.clear();
    }
    if (m_format.empty() && m_showCondition && !m_isVertical) {
      text += text.empty() ? "" : " ";
      text += WeatherService::shortDescriptionForCode(snapshot.current.weatherCode);
    }
  } else if (m_weather->loading()) {
    text = m_isVertical ? kVerticalLoading : i18n::tr("bar.widgets.weather.loading");
  } else if (!m_weather->error().empty()) {
    text = m_isVertical ? kVerticalUnavailable : i18n::tr("bar.widgets.weather.error");
  }

  bool changed = false;

  if (glyph != m_lastGlyph) {
    m_lastGlyph = glyph;
    m_glyph->setGlyph(glyph);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
    changed = true;
  }

  if (text != m_lastText) {
    m_lastText = text;
    m_label->setText(text);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
    changed = true;
  }

  if (changed) {
    requestRedraw();
  }

  if (m_area == nullptr) {
    return;
  }

  if (m_weather == nullptr || !m_weather->hasData()) {
    m_area->clearTooltip();
    return;
  }

  const auto& snapshot = m_weather->snapshot();
  const char* tempUnit = m_weather->displayTemperatureUnit();

  const int currentTemp = static_cast<int>(std::lround(m_weather->displayTemperature(snapshot.current.temperatureC)));
  const auto& apparentTemp = snapshot.current.apparentTemperatureC;

  std::vector<TooltipRow> rows;
  rows.push_back(
      {i18n::tr("bar.widgets.weather.tooltip.condition"),
       WeatherService::shortDescriptionForCode(snapshot.current.weatherCode)}
  );
  rows.push_back({i18n::tr("bar.widgets.weather.tooltip.temperature"), std::format("{}{}", currentTemp, tempUnit)});
  if (apparentTemp.has_value()) {
    rows.push_back(
        {i18n::tr("bar.widgets.weather.tooltip.feels-like"),
         std::format("{}{}", static_cast<int>(std::lround(m_weather->displayTemperature(*apparentTemp))), tempUnit)}
    );
  }

  if (!snapshot.forecastDays.empty()) {
    const auto& today = snapshot.forecastDays.front();
    const int high = static_cast<int>(std::lround(m_weather->displayTemperature(today.temperatureMaxC)));
    const int low = static_cast<int>(std::lround(m_weather->displayTemperature(today.temperatureMinC)));
    rows.push_back({i18n::tr("bar.widgets.weather.tooltip.low"), std::format("{}{}", low, tempUnit)});
    rows.push_back({i18n::tr("bar.widgets.weather.tooltip.high"), std::format("{}{}", high, tempUnit)});
  }

  if (snapshot.current.dewPointC.has_value()) {
    rows.push_back(
        {i18n::tr("bar.widgets.weather.tooltip.dew-point"),
         std::format(
             "{} ({})", m_weather->formatTemperature(*snapshot.current.dewPointC),
             WeatherService::dewPointComfort(*snapshot.current.dewPointC)
         )}
    );
  }
  rows.push_back(
      {i18n::tr("bar.widgets.weather.tooltip.humidity"), std::format("{}%", snapshot.current.relativeHumidityPercent)}
  );
  rows.push_back(
      {i18n::tr("bar.widgets.weather.tooltip.wind"),
       std::format(
           "{} {}", m_weather->formatWind(snapshot.current.windSpeedKmh),
           windDirectionLabel(snapshot.current.windDirectionDeg)
       )}
  );
  if (snapshot.current.windGustKmh.has_value()) {
    rows.push_back(
        {i18n::tr("bar.widgets.weather.tooltip.gusts"), m_weather->formatWind(*snapshot.current.windGustKmh)}
    );
  }
  if (snapshot.current.pressureHpa.has_value()) {
    rows.push_back(
        {i18n::tr("bar.widgets.weather.tooltip.pressure"), m_weather->formatPressure(*snapshot.current.pressureHpa)}
    );
  }
  if (snapshot.current.cloudCoverPercent >= 0) {
    rows.push_back(
        {i18n::tr("bar.widgets.weather.tooltip.cloud-cover"), std::format("{}%", snapshot.current.cloudCoverPercent)}
    );
  }
  if (snapshot.current.visibilityM.has_value()) {
    rows.push_back(
        {i18n::tr("bar.widgets.weather.tooltip.visibility"), m_weather->formatVisibility(*snapshot.current.visibilityM)}
    );
  }
  if (!snapshot.forecastDays.empty() && snapshot.forecastDays.front().precipitationProbabilityMaxPercent >= 0) {
    rows.push_back(
        {i18n::tr("bar.widgets.weather.tooltip.rain-chance"),
         std::format("{}%", snapshot.forecastDays.front().precipitationProbabilityMaxPercent)}
    );
  }
  rows.push_back({i18n::tr("bar.widgets.weather.tooltip.uv"), std::format("{:.1F}", snapshot.current.uvIndex)});
  if (snapshot.airQuality.valid) {
    rows.push_back(
        {i18n::tr("bar.widgets.weather.tooltip.air-quality"),
         std::format("{} ({})", snapshot.airQuality.usAqi, WeatherService::airQualityLabel(snapshot.airQuality.usAqi))}
    );
  }

  if (!snapshot.forecastDays.empty()) {
    const auto& today = snapshot.forecastDays.front();
    if (!today.sunriseIso.empty()) {
      rows.push_back({i18n::tr("bar.widgets.weather.tooltip.sunrise"), formatIsoTime(today.sunriseIso, "%H:%M")});
    }
    if (!today.sunsetIso.empty()) {
      rows.push_back({i18n::tr("bar.widgets.weather.tooltip.sunset"), formatIsoTime(today.sunsetIso, "%H:%M")});
    }
  }

  m_area->setTooltip(std::move(rows));
}

#include "shell/bar/widgets/custom_button_widget.h"

#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "system/process.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

CustomButtonWidget::CustomButtonWidget(const Options& options)
    : m_glyphName(StringUtils::trim(options.glyph)), m_labelText(StringUtils::trim(options.label)),
      m_tooltip(StringUtils::trim(options.tooltip)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)),
      m_glyphPosition(options.glyphPosition), m_labelCommand(StringUtils::trim(options.labelCommand)),
      m_labelInterval(std::chrono::seconds(static_cast<long>(std::max(2.0F, options.labelInterval)))) {
  setPeek(options.peek, options.peekDelayMs);
}

void CustomButtonWidget::create() {
  auto area = ui::inputArea({});

  if (!m_tooltip.empty()) {
    area->setTooltip(m_tooltip);
  }

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_glyphName,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
            .visible = !m_glyphName.empty(),
        })
    );
  }

  area->addChild(
      ui::label({
          .out = &m_label,
          .text = m_labelText,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxLines = 1,
          .visible = !m_labelText.empty(),
      })
  );

  m_area = area.get();
  setRoot(std::move(area));

  if (!m_labelCommand.empty()) {
    // Results arrive on a worker thread; the repeating main-thread timer applies them.
    m_labelTimer.startRepeating(std::chrono::seconds(1), [this] { pollLabelCommand(); });
  }
}

void CustomButtonWidget::pollLabelCommand() {
  std::string text;
  bool haveText = false;
  bool idle = false;
  {
    std::scoped_lock lock(m_labelShared->mutex);
    if (m_labelShared->hasPending) {
      text = std::move(m_labelShared->pending);
      m_labelShared->hasPending = false;
      haveText = true;
    }
    idle = !m_labelShared->running;
  }
  if (haveText && text != m_labelText && m_label != nullptr) {
    m_labelText = text;
    m_label->setText(m_labelText);
    requestUpdate();
  }

  const auto now = std::chrono::steady_clock::now();
  if (!idle || (m_lastLabelRun != std::chrono::steady_clock::time_point{} && now - m_lastLabelRun < m_labelInterval)) {
    return;
  }
  m_lastLabelRun = now;
  {
    std::scoped_lock lock(m_labelShared->mutex);
    m_labelShared->running = true;
  }
  process::RunCallbacks callbacks;
  callbacks.onExit = [shared = m_labelShared](process::RunResult result) {
    std::string line;
    if (result.exitCode == 0 && !result.timedOut) {
      line = StringUtils::trim(result.out.substr(0, result.out.find('\n')));
    }
    std::scoped_lock lock(shared->mutex);
    shared->pending = std::move(line);
    shared->hasPending = true;
    shared->running = false;
  };
  process::RunOptions options;
  options.timeout = std::chrono::milliseconds(5000);
  if (!process::runAsync(m_labelCommand, std::move(callbacks), std::move(options))) {
    std::scoped_lock lock(m_labelShared->mutex);
    m_labelShared->running = false;
  }
}

void CustomButtonWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_area == nullptr || m_label == nullptr) {
    return;
  }

  const bool isVertical = containerHeight > containerWidth;
  const bool showImage = m_image != nullptr;
  const bool showGlyph = !showImage && m_glyph != nullptr && !m_glyphName.empty();
  const bool showIcon = showImage || showGlyph;
  const bool showLabel = !m_labelText.empty();
  const float spacing = (showIcon && showLabel) ? Style::spaceXs * m_contentScale : 0.0F;

  if (m_glyph != nullptr) {
    m_glyph->setVisible(showGlyph);
  }
  m_label->setVisible(showLabel);

  if (showImage) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
  } else if (showGlyph) {
    m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
  }

  if (showLabel) {
    m_label->setFontSize((isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
    m_label->setFontWeight(labelFontWeight());
    m_label->setTextAlign(isVertical ? TextAlign::Center : TextAlign::Start);
    m_label->setMaxWidth(isVertical ? containerWidth : 0.0F);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
  }

  Node* icon = showImage ? static_cast<Node*>(m_image) : (showGlyph ? static_cast<Node*>(m_glyph) : nullptr);
  layoutGlyphValue({
      .glyph = icon,
      .value = m_label,
      .glyphPosition = m_glyphPosition,
      .vertical = isVertical,
      .horizontalGap = spacing,
      .verticalGap = spacing,
  });
}

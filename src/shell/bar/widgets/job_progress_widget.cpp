#include "shell/bar/widgets/job_progress_widget.h"

#include "notification/job_progress.h"
#include "render/scene/input_area.h"
#include "render/scene/tooltip_content.h"
#include "shell/render/countdown_ring_node.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>
#include <memory>
#include <vector>

JobProgressWidget::JobProgressWidget(Options options) : m_config(options) {}

void JobProgressWidget::create() {
  auto area = ui::inputArea({
      .out = &m_area,
      .tooltipProvider = [] -> TooltipContent {
        std::vector<TooltipRow> rows;
        for (const auto& job : JobProgress::instance().jobs()) {
          rows.push_back(TooltipRow{.key = job.title, .value = job.detail});
        }
        if (rows.empty()) {
          return std::monostate{};
        }
        return TooltipContent{std::move(rows)};
      },
  });
  area->setOnClick([](const InputArea::PointerData& data) {
    if (!data.pressed && data.button == BTN_LEFT) {
      JobProgress::instance().show();
    }
  });

  for (CountdownRingNode** slot : {&m_ringTrack, &m_ringFill}) {
    auto ring = std::make_unique<CountdownRingNode>();
    ring->setParticipatesInLayout(false);
    ring->setHitTestVisible(false);
    *slot = static_cast<CountdownRingNode*>(area->addChild(std::move(ring)));
  }
  m_ringTrack->setProgress(1.0F);

  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "download",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = m_config.activeColor,
      })
  );

  setRoot(std::move(area));
}

void JobProgressWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  if (m_glyph == nullptr || m_ringTrack == nullptr || m_ringFill == nullptr) {
    return;
  }
  syncState();

  // The glyph sits inside the ring, so it is drawn smaller than a bare bar icon.
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale * 0.7F);
  m_glyph->measure(renderer);

  const float pad = std::round(Style::fontSizeBody * m_contentScale * 0.3F);
  const float side = std::max(m_glyph->width(), m_glyph->height()) + 2.0F * pad;
  const float thickness = std::max(1.5F, std::round(Style::fontSizeBody * m_contentScale * 0.14F * 2.0F) * 0.5F);
  for (CountdownRingNode* ring : {m_ringTrack, m_ringFill}) {
    ring->setThickness(thickness);
    ring->setFrameSize(side, side);
    ring->setPosition(0.0F, 0.0F);
  }
  m_glyph->setPosition((side - m_glyph->width()) * 0.5F, (side - m_glyph->height()) * 0.5F);

  if (auto* node = root(); node != nullptr) {
    node->setSize(side, side);
  }
}

void JobProgressWidget::doUpdate(Renderer& /*renderer*/) { syncState(); }

void JobProgressWidget::syncState() {
  const JobProgress& state = JobProgress::instance();
  if (state.serial() == m_lastSerial) {
    return;
  }
  m_lastSerial = state.serial();

  const bool active = !state.jobs().empty();
  if (auto* node = root(); node != nullptr) {
    node->setVisible(active);
    node->setParticipatesInLayout(active);
  }

  const ColorSpec fillSpec = widgetIconColorOr(m_config.activeColor);
  ColorSpec trackSpec = fillSpec;
  trackSpec.alpha *= 0.3F;
  if (m_glyph != nullptr) {
    m_glyph->setColor(fillSpec);
  }
  if (m_ringTrack != nullptr) {
    m_ringTrack->setColor(resolveColorSpec(trackSpec));
  }
  if (m_ringFill != nullptr) {
    m_ringFill->setColor(resolveColorSpec(fillSpec));
    m_ringFill->setProgress(static_cast<float>(state.overallPercent().value_or(0)) / 100.0F);
  }
  requestRedraw();
}

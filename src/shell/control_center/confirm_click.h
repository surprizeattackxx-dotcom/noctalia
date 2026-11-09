#pragma once

#include "core/timer_manager.h"
#include "ui/controls/button.h"

#include <chrono>
#include <functional>
#include <string>
#include <utility>

// Two-step click for destructive icon buttons (forget a device or network): the first click arms the button
// (icon and tooltip change, destructive color) and the second click within a few seconds runs the action.
// Without a second click the button disarms itself, so a stray click never deletes anything.
class ConfirmClick {
public:
  using Action = std::function<void()>;

  // `button` must outlive this object; both are normally members of the same row.
  void bind(
      Button* button, std::string normalGlyph, ButtonVariant normalVariant, std::string normalTooltip,
      std::string armedTooltip, Action onConfirm
  ) {
    m_button = button;
    m_normalGlyph = std::move(normalGlyph);
    m_normalVariant = normalVariant;
    m_normalTooltip = std::move(normalTooltip);
    m_armedTooltip = std::move(armedTooltip);
    m_onConfirm = std::move(onConfirm);
    if (m_button == nullptr) {
      return;
    }
    m_button->setTooltip(m_normalTooltip);
    m_button->setOnClick([this] { onClick(); });
  }

private:
  static constexpr auto kArmedFor = std::chrono::seconds(3);

  void onClick() {
    if (!m_armed) {
      m_armed = true;
      apply();
      m_timer.start(kArmedFor, [this] {
        m_armed = false;
        apply();
      });
      return;
    }
    m_timer.stop();
    m_armed = false;
    apply();
    if (m_onConfirm) {
      m_onConfirm();
    }
  }

  void apply() {
    if (m_button == nullptr) {
      return;
    }
    m_button->setGlyph(m_armed ? "alert-triangle" : m_normalGlyph);
    m_button->setVariant(m_armed ? ButtonVariant::Destructive : m_normalVariant);
    m_button->setTooltip(m_armed ? m_armedTooltip : m_normalTooltip);
  }

  Button* m_button = nullptr;
  bool m_armed = false;
  std::string m_normalGlyph;
  ButtonVariant m_normalVariant = ButtonVariant::Ghost;
  std::string m_normalTooltip;
  std::string m_armedTooltip;
  Action m_onConfirm;
  Timer m_timer;
};

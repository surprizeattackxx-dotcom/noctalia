#pragma once

namespace noctalia::taskbar {

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

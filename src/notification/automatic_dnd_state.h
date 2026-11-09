#pragma once

/// Combines automatic DND owners without saving/restoring each other's transient state.
/// Feed every actual DND change to observe(), including manual changes. setOwner() records
/// its expected result before the caller writes it, so synchronous change callbacks do not
/// mistake automatic writes for manual ones. A no-op setter (e.g. manual "on" while already
/// automatically on) produces no NotificationManager callback and cannot convey new intent here.
class AutomaticDndState {
public:
  enum class Owner : unsigned { QuietHours = 1U, GameMode = 2U, LockScreen = 4U };

  void observe(bool current) noexcept {
    if (current == m_expected) {
      return;
    }
    m_manual = current;
    // Turning DND off overrides all current activations. In particular, quiet hours must
    // not reassert DND on a timer/reload, or after another automatic owner ends.
    m_suppressed = current ? 0U : m_active;
    m_expected = current;
  }

  [[nodiscard]] bool setOwner(Owner owner, bool active, bool current) noexcept {
    observe(current);
    const auto bit = static_cast<unsigned>(owner);
    if (active) {
      m_active |= bit;
    } else {
      m_active &= ~bit;
      m_suppressed &= ~bit;
    }
    m_expected = m_manual || (m_active & ~m_suppressed) != 0U;
    return m_expected;
  }

private:
  bool m_manual = false;
  bool m_expected = false;
  unsigned m_active = 0U;
  unsigned m_suppressed = 0U;
};

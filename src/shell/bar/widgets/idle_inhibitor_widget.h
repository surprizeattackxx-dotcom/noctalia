#pragma once

#include "shell/bar/widget.h"

class Glyph;
class IdleInhibitor;
class InputArea;

class IdleInhibitorWidget : public Widget {
public:
  struct Options {
    /// Hide the widget while caffeine is off (toggle it with `noctalia msg caffeine-toggle` or the control center).
    bool hideWhenOff = false;
  };

  IdleInhibitorWidget(IdleInhibitor* inhibitor, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);

  IdleInhibitor* m_inhibitor = nullptr;
  bool m_hideWhenOff = false;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  bool m_lastEnabled = false;
  bool m_lastAvailable = false;
};

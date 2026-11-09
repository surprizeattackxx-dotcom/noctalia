#pragma once

#include "shell/bar/widget.h"
#include "ui/palette.h"

#include <cstdint>

class CountdownRingNode;
class Glyph;
class InputArea;

/// Appears while a background job (a KDE file copy, move, delete...) is running: a ring that fills with the
/// overall progress. Hover lists the jobs; click brings back progress popups that were closed.
class JobProgressWidget : public Widget {
public:
  struct Options {
    ColorSpec activeColor = colorSpecFromRole(ColorRole::Primary);
  };

  explicit JobProgressWidget(Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState();

  Options m_config;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  CountdownRingNode* m_ringTrack = nullptr;
  CountdownRingNode* m_ringFill = nullptr;
  std::uint64_t m_lastSerial = UINT64_MAX;
};

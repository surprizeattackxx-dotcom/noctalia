#pragma once

#include "core/timer_manager.h"
#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>

class Glyph;
class Image;
class InputArea;
class Label;

class CustomButtonWidget : public Widget {
public:
  struct Options {
    std::string glyph = "heart";
    std::string customImage;
    bool customImageColorize = false;
    std::string label;
    std::string tooltip;
    GlyphPosition glyphPosition = GlyphPosition::Before;
    bool peek = false;
    int peekDelayMs = kBarRevealDelayMs;
    std::string labelCommand;    // shell command whose first output line becomes the label
    float labelInterval = 30.0F; // seconds between runs of labelCommand
  };

  explicit CustomButtonWidget(const Options& options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void pollLabelCommand();

  struct LabelShared {
    std::mutex mutex;
    std::string pending;
    bool hasPending = false;
    bool running = false;
  };

  std::string m_glyphName;
  std::string m_labelText;
  std::string m_tooltip;
  WidgetCustomImage m_customImage;
  GlyphPosition m_glyphPosition = GlyphPosition::Before;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Label* m_label = nullptr;
  std::string m_labelCommand;
  std::chrono::seconds m_labelInterval{30};
  std::chrono::steady_clock::time_point m_lastLabelRun{};
  std::shared_ptr<LabelShared> m_labelShared = std::make_shared<LabelShared>();
  Timer m_labelTimer;
};

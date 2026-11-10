#pragma once

#include "capture/pointer_cursor_capture.h"
#include "capture/screenshot_image.h"
#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

class KwinWindowCapture;
class SessionBus;

class ScreenshotCapture {
public:
  using CompletionCallback = std::function<void(std::optional<capture::ScreenshotImage>, std::string error)>;

  explicit ScreenshotCapture(WaylandConnection& wayland);
  ~ScreenshotCapture();

  bool available() const noexcept;
  bool busy() const noexcept { return m_busy; }
  void setKwinSessionBus(SessionBus* bus);

  void capture(
      wl_output* output, std::optional<LogicalRect> region, bool showCursor, bool retainCursor,
      CompletionCallback onComplete
  );
  void cancelInFlight();

private:
  void captureBackground();
  void captureKwinBackground();
  void finish(std::optional<ScreencopyImage> image, std::string error);

  WaylandConnection& m_wayland;
  ScreencopyCapture m_backgroundCapture;
  PointerCursorCapture m_cursorCapture;
  std::unique_ptr<KwinWindowCapture> m_kwinCapture;
  Timer m_timeout;
  wl_output* m_output = nullptr;
  std::optional<LogicalRect> m_region;
  std::optional<capture::CapturedCursor> m_cursor;
  capture::CursorToggleStatus m_cursorStatus = capture::CursorToggleStatus::NotCaptured;
  bool m_busy = false;
  bool m_showCursor = false;
  CompletionCallback m_onComplete;
};

#pragma once

#include "capture/kwin_window_capture.h"
#include "compositors/compositor_platform.h"
#include "core/timer_manager.h"
#include "render/scene/input_dispatcher.h"
#include "shell/bar/widgets/taskbar_preview_policy.h"
#include "system/icon_resolver.h"
#include "ui/popup_parent.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class PopupSurface;
class RenderContext;
class Node;
class Image;
class SessionBus;

struct TaskbarPreviewEntry {
  ToplevelInfo window;
  ResolvedIcon icon;
};

// Non-grabbing, one-shot previews. Owns only the visible popup's bounded
// snapshots; all window actions resolve the current compositor identity again.
class TaskbarPreviewPopup {
public:
  TaskbarPreviewPopup(CompositorPlatform& platform, RenderContext& renderer, SessionBus& bus);
  ~TaskbarPreviewPopup();
  bool available() const noexcept { return m_capture.available(); }
  bool isOpen() const noexcept { return m_surface != nullptr; }
  void open(
      std::vector<TaskbarPreviewEntry> entries, PopupSurfaceParent parent, PopupAnchorRect anchor,
      std::string_view barPosition, wl_output* outputFilter
  );
  void close();
  bool matches(const std::vector<TaskbarPreviewEntry>& entries) const;
  void anchorEntered();
  void anchorLeft();
  bool onPointerEvent(const PointerEvent& event);

private:
  void scheduleClose();
  void captureNext();
  void prepareFrame();
  void activate(std::string id, std::string appId);

  CompositorPlatform& m_platform;
  RenderContext& m_renderer;
  KwinWindowCapture m_capture;
  std::unique_ptr<PopupSurface> m_surface;
  std::unique_ptr<Node> m_root;
  InputDispatcher m_input;
  noctalia::taskbar::PreviewHover m_hover;
  Timer m_closeTimer;
  std::vector<TaskbarPreviewEntry> m_entries;
  std::vector<std::optional<ScreencopyImage>> m_pixels;
  std::vector<Image*> m_images;
  std::size_t m_nextCapture = 0;
  wl_output* m_outputFilter = nullptr;
  float m_cardWidth = 260.0F;
  float m_viewHeight = 180.0F;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  std::uint64_t m_epoch = 0;
};

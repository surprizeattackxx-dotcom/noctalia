#include "shell/bar/widgets/taskbar_preview_popup.h"

#include "core/deferred_call.h"
#include "core/ui_phase.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <linux/input-event-codes.h>

TaskbarPreviewPopup::TaskbarPreviewPopup(CompositorPlatform& platform, RenderContext& renderer, SessionBus& bus)
    : m_platform(platform), m_renderer(renderer), m_capture(bus) {}

TaskbarPreviewPopup::~TaskbarPreviewPopup() {
  *m_alive = false;
  close();
}

bool TaskbarPreviewPopup::matches(const std::vector<TaskbarPreviewEntry>& entries) const {
  if (entries.size() != m_entries.size()) {
    return false;
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].window.identifier != m_entries[i].window.identifier
        || entries[i].window.minimizeState != m_entries[i].window.minimizeState) {
      return false;
    }
  }
  return true;
}

void TaskbarPreviewPopup::close() {
  ++m_epoch;
  m_closeTimer.stop();
  m_capture.cancelInFlight();
  m_hover.cancel();
  m_input.setSceneRoot(nullptr);
  // Release image textures while their renderer and surface are still alive.
  if (m_surface && m_root) {
    m_renderer.makeCurrent(m_surface->renderTarget());
    m_surface->setSceneRoot(nullptr);
  }
  m_images.clear();
  m_root.reset();
  m_surface.reset();
  m_pixels.clear();
  m_entries.clear();
  m_nextCapture = 0;
}

void TaskbarPreviewPopup::open(
    std::vector<TaskbarPreviewEntry> entries, PopupSurfaceParent parent, PopupAnchorRect anchor,
    std::string_view barPosition, wl_output* outputFilter
) {
  close();
  if (entries.empty()) {
    return;
  }
  m_entries = std::move(entries);
  m_outputFilter = outputFilter;
  m_pixels.resize(m_entries.size());
  const auto* output = m_platform.wayland().findOutputByWl(parent.output);
  const int outputWidth = output ? output->effectiveLogicalWidth() : 640;
  const int outputHeight = output ? output->effectiveLogicalHeight() : 480;
  m_cardWidth = static_cast<float>(std::clamp(outputWidth - 48, 80, 260));
  m_viewHeight =
      std::min(190.0F * static_cast<float>(m_entries.size()), static_cast<float>(std::max(100, outputHeight / 2)));
  PopupSurfaceConfig config{
      .anchorX = anchor.x,
      .anchorY = anchor.y,
      .anchorWidth = std::max(1, anchor.width),
      .anchorHeight = std::max(1, anchor.height),
      .width = static_cast<std::uint32_t>(m_cardWidth + 16),
      .height = static_cast<std::uint32_t>(m_viewHeight + 16),
      .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetY = 4,
      .grab = false,
  };
  if (barPosition == "bottom") {
    config.anchor = XDG_POSITIONER_ANCHOR_TOP;
    config.gravity = XDG_POSITIONER_GRAVITY_TOP;
    config.offsetY = -4;
  } else if (barPosition == "left" || barPosition == "right") {
    const bool left = barPosition == "left";
    config.anchor = left ? XDG_POSITIONER_ANCHOR_RIGHT : XDG_POSITIONER_ANCHOR_LEFT;
    config.gravity = left ? XDG_POSITIONER_GRAVITY_RIGHT : XDG_POSITIONER_GRAVITY_LEFT;
    config.offsetX = left ? 4 : -4;
    config.offsetY = 0;
  }
  m_surface = std::make_unique<PopupSurface>(m_platform.wayland());
  m_surface->setRenderContext(&m_renderer);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool, bool) { prepareFrame(); });
  const auto epoch = m_epoch;
  const auto alive = m_alive;
  m_surface->setDismissedCallback([this, alive, epoch] {
    DeferredCall::callLater([this, alive, epoch] {
      if (*alive && epoch == m_epoch) {
        close();
      }
    });
  });
  // Initialization dispatches events. Seed hover before its roundtrip so a
  // real leave delivered during initialization is not overwritten afterwards.
  anchorEntered();
  const bool initialized = parent.xdgSurface ? m_surface->initializeAsChild(parent.xdgSurface, parent.output, config)
                                             : m_surface->initialize(parent.attach, parent.output, config);
  if (!*alive || epoch != m_epoch) {
    return; // the owner or this opening may have been destroyed during dispatch
  }
  if (!initialized) {
    close();
    return;
  }
  captureNext();
}

void TaskbarPreviewPopup::anchorEntered() {
  m_hover.enterAnchor();
  m_closeTimer.stop();
}

void TaskbarPreviewPopup::anchorLeft() {
  m_hover.leaveAnchor();
  scheduleClose();
}

void TaskbarPreviewPopup::scheduleClose() {
  m_closeTimer.start(std::chrono::milliseconds(220), [this] {
    if (m_hover.shouldClose()) {
      close();
    }
  });
}

void TaskbarPreviewPopup::captureNext() {
  if (!m_surface || m_capture.busy()) {
    return;
  }
  // One snapshot per represented window per opening; bound thumbnail memory to
  // 32 images. Additional cards retain their app icon and remain clickable.
  while (m_nextCapture < std::min<std::size_t>(32, m_entries.size())) {
    const auto index = m_nextCapture++;
    const auto& window = m_entries[index].window;
    if (window.identifier.empty() || (window.minimizeState && window.minimizeState->minimized)) {
      continue;
    }
    const auto epoch = m_epoch;
    m_capture.capture(
        window.identifier, 520, 300, [this, epoch, index](std::optional<ScreencopyImage> image, std::string) {
          if (epoch != m_epoch || !m_surface) {
            return;
          }
          m_pixels[index] = std::move(image);
          // Pixel uploads happen in prepareFrame; a paint-only redraw skips it.
          m_surface->requestUpdateOnly();
          captureNext();
        }
    );
    return;
  }
}

void TaskbarPreviewPopup::prepareFrame() {
  if (!m_surface) {
    return;
  }
  m_renderer.makeCurrent(m_surface->renderTarget());
  auto& renderer = m_surface->renderTarget().renderer();
  UiPhaseScope layoutPhase(UiPhase::Layout);
  if (!m_root) {
    auto root = ui::box(
        {.fill = colorSpecFromRole(ColorRole::Surface),
         .border = colorSpecFromRole(ColorRole::OutlineVariant),
         .borderWidth = 1.0F,
         .radius = Style::scaledRadiusLg(),
         .width = static_cast<float>(m_surface->width()),
         .height = static_cast<float>(m_surface->height())}
    );
    auto scroll = std::make_unique<ScrollView>();
    scroll->setPosition(8, 8);
    scroll->setSize(m_cardWidth, m_viewHeight);
    scroll->setViewportPaddingH(0);
    scroll->setViewportPaddingV(0);
    scroll->clearFill();
    scroll->clearBorder();
    scroll->content()->setDirection(FlexDirection::Vertical);
    scroll->content()->setGap(8);
    for (const auto& entry : m_entries) {
      auto card = ui::inputArea({});
      card->setFrameSize(m_cardWidth, 182);
      const auto id = entry.window.identifier;
      const auto app = entry.window.appId;
      card->setOnClick([this, id, app](const InputArea::PointerData&) { activate(id, app); });
      auto image = ui::image(
          {.fit = ImageFit::Contain, .radius = Style::scaledRadiusMd(), .width = m_cardWidth - 8, .height = 150.0F}
      );
      image->setPosition(4, 0);
      if (!entry.icon.path.empty()) {
        image->setSourceFile(renderer, entry.icon.path, 64);
      }
      m_images.push_back(image.get());
      card->addChild(std::move(image));
      auto label = std::make_unique<Label>();
      label->setText(entry.window.title.empty() ? entry.window.appId : entry.window.title);
      label->setFontSize(Style::fontSizeCaption);
      label->setMaxWidth(m_cardWidth - 16);
      label->setColor(colorSpecFromRole(ColorRole::OnSurface));
      label->setPosition(8, 158);
      label->layout(renderer);
      card->addChild(std::move(label));
      scroll->content()->addChild(std::move(card));
    }
    scroll->layout(renderer);
    root->addChild(std::move(scroll));
    m_root = std::move(root);
    m_input.setSceneRoot(m_root.get());
    m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform.wayland().setCursorShape(serial, shape);
    });
    m_surface->setSceneRoot(m_root.get());
  }
  for (std::size_t i = 0; i < m_pixels.size(); ++i) {
    if (!m_pixels[i] || i >= m_images.size()) {
      continue;
    }
    const auto& image = *m_pixels[i];
    m_images[i]->setSourceRaw(
        renderer, image.rgba.data(), image.rgba.size(), image.width, image.height, image.width * 4, PixmapFormat::RGBA
    );
    m_pixels[i].reset();
  }
  m_root->layout(renderer);
}

void TaskbarPreviewPopup::activate(std::string id, std::string appId) {
  const auto alive = m_alive;
  const auto epoch = m_epoch;
  DeferredCall::callLater([this, alive, epoch, id = std::move(id), appId = std::move(appId)] {
    if (!*alive || epoch != m_epoch) {
      return;
    }
    const auto lower = StringUtils::toLower(appId);
    const auto windows = appId.empty() ? m_platform.enrichedWindowsWithoutAppId(m_outputFilter)
                                       : m_platform.enrichedWindowsForApp(lower, lower, m_outputFilter);
    const auto it = std::ranges::find(windows, id, &ToplevelInfo::identifier);
    if (it == windows.end()) {
      close();
      return;
    }
    const auto window = *it;
    close();
    m_platform.activateToplevelInfo(window);
  });
}

bool TaskbarPreviewPopup::onPointerEvent(const PointerEvent& event) {
  if (!m_surface) {
    return false;
  }
  if (event.surface != m_surface->wlSurface()) {
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      close();
    }
    return false;
  }
  const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_hover.enterPopup();
    m_closeTimer.stop();
    m_input.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_hover.leavePopup();
    m_input.pointerLeave();
    scheduleClose();
    break;
  case PointerEvent::Type::Motion:
    m_input.pointerMotion(x, y, 0);
    break;
  case PointerEvent::Type::Button:
    m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  case PointerEvent::Type::Axis:
    m_input.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  if (m_root && m_root->layoutDirty()) {
    m_surface->requestLayout();
  } else {
    m_surface->requestRedraw();
  }
  return true;
}

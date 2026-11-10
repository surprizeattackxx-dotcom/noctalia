#pragma once

#include "capture/screencopy_capture.h"

#include <functional>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <thread>

class SessionBus;

// One-shot window snapshots on KDE Plasma through KWin's `org.kde.KWin.ScreenShot2` D-Bus interface. KWin does not
// offer ext-image-copy-capture, so this is what window thumbnails use there. A window is named by its KWin internal
// id (the `{uuid}` the KWin bridge reports). Positive bounds downscale; the image is delivered on the main thread.
class KwinWindowCapture {
public:
  using CompletionCallback = std::function<void(std::optional<ScreencopyImage>, std::string error)>;

  explicit KwinWindowCapture(SessionBus& bus);
  ~KwinWindowCapture();

  KwinWindowCapture(const KwinWindowCapture&) = delete;
  KwinWindowCapture& operator=(const KwinWindowCapture&) = delete;

  // False once KWin has refused this process (the interface is restricted to authorized desktop files).
  bool available() const noexcept { return m_available; }
  bool busy() const noexcept { return m_busy; }
  void capture(const std::string& windowId, int maxWidth, int maxHeight, CompletionCallback onComplete);
  // Native-resolution output snapshot; connectorName is KWin's output connector (e.g. DP-1).
  void captureScreen(const std::string& connectorName, bool showCursor, CompletionCallback onComplete);
  // Native-resolution window snapshot, without thumbnail bounds.
  void captureWindow(const std::string& windowId, bool showCursor, CompletionCallback onComplete);
  void cancelInFlight();

private:
  struct Job;

  void joinWorker();
  void startCapture(
      const char* method, const std::string& target, bool showCursor, int maxWidth, int maxHeight,
      CompletionCallback onComplete
  );

  std::unique_ptr<sdbus::IProxy> m_proxy;
  std::shared_ptr<Job> m_job;
  std::thread m_worker;
  CompletionCallback m_onComplete;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
  bool m_available = true;
  bool m_busy = false;
};

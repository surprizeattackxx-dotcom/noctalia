#include "capture/kwin_window_capture.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "render/core/image_resize.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  constexpr Logger kLog("kwin-capture");

  constexpr auto kInterface = "org.kde.KWin.ScreenShot2";
  constexpr auto kCaptureTimeout = std::chrono::seconds(3);
  constexpr std::uint64_t kMaxPixels = 16384ULL * 16384ULL;

  // KWin describes the pixels with a QImage::Format number.
  std::optional<ResizeSourceLayout> layoutForQImageFormat(std::uint32_t format) noexcept {
    switch (format) {
    case 4: // RGB32: B, G, R, x in memory
      return ResizeSourceLayout::Bgrx;
    case 5: // ARGB32 (straight alpha); window captures are opaque enough that treating it as premultiplied is fine
    case 6: // ARGB32_Premultiplied
      return ResizeSourceLayout::BgraPremultiplied;
    case 16: // RGBX8888
      return ResizeSourceLayout::Rgbx;
    case 17: // RGBA8888
      return ResizeSourceLayout::Rgba;
    case 18: // RGBA8888_Premultiplied
      return ResizeSourceLayout::RgbaPremultiplied;
    default:
      return std::nullopt;
    }
  }

  std::uint32_t variantU32(const std::map<std::string, sdbus::Variant>& map, const char* key) {
    const auto it = map.find(key);
    if (it == map.end()) {
      return 0;
    }
    try {
      if (it->second.containsValueOfType<std::uint32_t>()) {
        return it->second.get<std::uint32_t>();
      }
      if (it->second.containsValueOfType<std::int32_t>()) {
        return static_cast<std::uint32_t>(std::max(0, it->second.get<std::int32_t>()));
      }
    } catch (const sdbus::Error&) {
    }
    return 0;
  }
} // namespace

struct KwinWindowCapture::Job {
  std::mutex mutex;
  std::condition_variable changed;
  bool haveMeta = false;
  bool failed = false;
  std::string error;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::uint32_t format = 0;
  std::atomic<bool> cancelled{false};
  int readFd = -1;
  int maxWidth = 0;
  int maxHeight = 0;
};

KwinWindowCapture::KwinWindowCapture(SessionBus& bus) {
  m_proxy = sdbus::createProxy(
      bus.connection(), sdbus::ServiceName{"org.kde.KWin"}, sdbus::ObjectPath{"/org/kde/KWin/ScreenShot2"}
  );
}

KwinWindowCapture::~KwinWindowCapture() {
  *m_alive = false;
  cancelInFlight();
  joinWorker();
}

void KwinWindowCapture::joinWorker() {
  if (m_worker.joinable()) {
    m_worker.join();
  }
}

void KwinWindowCapture::cancelInFlight() {
  if (m_job != nullptr) {
    m_job->cancelled.store(true);
    m_job->changed.notify_all();
    m_job.reset();
  }
  m_onComplete = nullptr;
  m_busy = false;
}

void KwinWindowCapture::capture(
    const std::string& windowId, int maxWidth, int maxHeight, CompletionCallback onComplete
) {
  if (m_busy || !m_available || windowId.empty()) {
    if (onComplete) {
      onComplete(std::nullopt, m_busy ? "capture already in flight" : "KWin window capture unavailable");
    }
    return;
  }
  joinWorker(); // the previous worker has already delivered or been cancelled

  int fds[2] = {-1, -1};
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    if (onComplete) {
      onComplete(std::nullopt, "pipe failed");
    }
    return;
  }

  auto job = std::make_shared<Job>();
  job->readFd = fds[0];
  job->maxWidth = maxWidth;
  job->maxHeight = maxHeight;
  m_job = job;
  m_onComplete = std::move(onComplete);
  m_busy = true;

  const std::map<std::string, sdbus::Variant> options{
      {"include-decoration", sdbus::Variant{true}},
      {"include-cursor", sdbus::Variant{false}},
      {"include-shadow", sdbus::Variant{false}},
      {"native-resolution", sdbus::Variant{true}},
  };
  try {
    m_proxy->callMethodAsync("CaptureWindow")
        .onInterface(kInterface)
        .withArguments(windowId, options, sdbus::UnixFd{fds[1]})
        .uponReplyInvoke([job, alive = m_alive, this](
                             std::optional<sdbus::Error> error, const std::map<std::string, sdbus::Variant>& result
                         ) {
          const std::lock_guard lock(job->mutex);
          if (error.has_value()) {
            job->failed = true;
            job->error = error->getMessage();
            if (*alive && std::string(error->getName()).find("NoAuthorized") != std::string::npos) {
              // KWin only serves callers whose desktop file lists the interface in X-KDE-DBUS-Restricted-Interfaces.
              m_available = false;
              kLog.warn("KWin refused window capture (not authorized); window thumbnails disabled");
            }
          } else {
            job->width = variantU32(result, "width");
            job->height = variantU32(result, "height");
            job->stride = variantU32(result, "stride");
            job->format = variantU32(result, "format");
            job->haveMeta = true;
          }
          job->changed.notify_all();
        });
  } catch (const sdbus::Error& e) {
    ::close(fds[0]);
    ::close(fds[1]);
    auto callback = std::move(m_onComplete);
    cancelInFlight();
    if (callback) {
      callback(std::nullopt, e.getMessage());
    }
    return;
  }
  ::close(fds[1]); // sdbus keeps its own duplicate for the message

  m_worker = std::thread([job, alive = m_alive, this] {
    std::vector<std::uint8_t> pixels;
    std::string error;
    const auto deadline = std::chrono::steady_clock::now() + kCaptureTimeout;
    std::uint64_t needed = 0; // unknown until the reply arrives
    bool eof = false;

    while (!job->cancelled.load()) {
      {
        const std::lock_guard lock(job->mutex);
        if (job->failed) {
          error = job->error.empty() ? "KWin capture failed" : job->error;
          break;
        }
        if (job->haveMeta && needed == 0) {
          needed = static_cast<std::uint64_t>(job->stride) * job->height;
          if (job->width == 0
              || job->height == 0
              || job->stride < job->width * 4ULL
              || static_cast<std::uint64_t>(job->width) * job->height > kMaxPixels) {
            error = "KWin returned an unusable image size";
            break;
          }
        }
      }
      if (needed != 0 && pixels.size() >= needed) {
        break;
      }
      if (eof) {
        // All data is in; only the reply is missing. Wait for it instead of spinning on a closed pipe.
        std::unique_lock lock(job->mutex);
        job->changed.wait_until(lock, deadline, [&] { return job->haveMeta || job->failed || job->cancelled.load(); });
        if (!job->haveMeta && !job->failed) {
          error = "KWin capture timed out";
          break;
        }
        if (job->haveMeta && pixels.size() < static_cast<std::uint64_t>(job->stride) * job->height) {
          error = "KWin sent a short image";
          break;
        }
        continue;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        error = "KWin capture timed out";
        break;
      }

      pollfd pfd{.fd = job->readFd, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&pfd, 1, 50);
      if (ready < 0 && errno != EINTR) {
        error = "poll failed";
        break;
      }
      if (ready <= 0) {
        continue;
      }
      const std::size_t offset = pixels.size();
      constexpr std::size_t chunk = 1U << 20U;
      pixels.resize(offset + chunk);
      const ssize_t n = ::read(job->readFd, pixels.data() + offset, chunk);
      if (n < 0) {
        pixels.resize(offset);
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        error = "read failed";
        break;
      }
      pixels.resize(offset + static_cast<std::size_t>(n));
      if (n == 0) {
        eof = true;
      }
    }
    ::close(job->readFd);

    std::optional<ScreencopyImage> image;
    if (!job->cancelled.load() && error.empty()) {
      const auto layout = layoutForQImageFormat(job->format);
      if (!layout.has_value()) {
        error = "unsupported KWin image format " + std::to_string(job->format);
      } else {
        const int width = static_cast<int>(job->width);
        const int height = static_cast<int>(job->height);
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
        if (job->stride != rowBytes) {
          // The resampler wants tightly packed rows.
          std::vector<std::uint8_t> packed(rowBytes * static_cast<std::size_t>(height));
          for (int y = 0; y < height; ++y) {
            std::copy_n(
                pixels.data() + static_cast<std::size_t>(y) * job->stride, rowBytes,
                packed.data() + static_cast<std::size_t>(y) * rowBytes
            );
          }
          pixels = std::move(packed);
        }
        const int maxWidth = job->maxWidth > 0 ? job->maxWidth : width;
        const int maxHeight = job->maxHeight > 0 ? job->maxHeight : height;
        const ImageExtent target = fitImageWithin(width, height, maxWidth, maxHeight);
        ScreencopyImage out;
        out.width = target.width;
        out.height = target.height;
        out.rgba = resizeToRgba(pixels.data(), width, height, *layout, target.width, target.height);
        if (out.rgba.empty()) {
          error = "failed to scale KWin capture";
        } else {
          image = std::move(out);
        }
      }
    }

    DeferredCall::callLater([job, alive, this, image = std::move(image), error = std::move(error)]() mutable {
      // `this` is only touched while the owner is alive and still waiting on this very job.
      if (!*alive || m_job != job) {
        return;
      }
      auto callback = std::move(m_onComplete);
      m_onComplete = nullptr;
      m_job.reset();
      m_busy = false;
      if (callback) {
        callback(std::move(image), std::move(error));
      }
    });
  });
}

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct JobProgressEntry {
  std::string title;
  std::string detail;
  std::optional<int> percent;

  bool operator==(const JobProgressEntry&) const = default;
};

/// Running background jobs (KDE file operations), shared between the service that tracks them and the bar widget
/// that shows them. Main thread only. Empty when nothing is running or no job service is active.
class JobProgress {
public:
  static JobProgress& instance() {
    static JobProgress state;
    return state;
  }

  [[nodiscard]] const std::vector<JobProgressEntry>& jobs() const noexcept { return m_jobs; }
  [[nodiscard]] std::uint64_t serial() const noexcept { return m_serial; }

  /// Average of the jobs that report a percentage; empty when none do.
  [[nodiscard]] std::optional<int> overallPercent() const {
    int sum = 0;
    int count = 0;
    for (const auto& job : m_jobs) {
      if (job.percent.has_value()) {
        sum += std::clamp(*job.percent, 0, 100);
        ++count;
      }
    }
    return count == 0 ? std::nullopt : std::optional<int>{sum / count};
  }

  void set(std::vector<JobProgressEntry> jobs) {
    if (jobs == m_jobs) {
      return;
    }
    m_jobs = std::move(jobs);
    ++m_serial;
    if (m_changed) {
      m_changed();
    }
  }

  void setChangeCallback(std::function<void()> callback) { m_changed = std::move(callback); }

  /// Bring back the popups of running jobs the user closed.
  void setShowHandler(std::function<void()> handler) { m_show = std::move(handler); }
  void show() const {
    if (m_show) {
      m_show();
    }
  }

private:
  std::vector<JobProgressEntry> m_jobs;
  std::uint64_t m_serial = 0;
  std::function<void()> m_changed;
  std::function<void()> m_show;
};

#include "dbus/notification/kde_job_view_server.h"
#include "tests/test_check.h"

#include <iostream>
#include <limits>

struct KdeJobViewServerTestAccess {
  using Job = KdeJobViewServer::Job;
  static auto fromDouble(double value) { return KdeJobViewServer::amountFromDouble(value); }
};

int main() {
  KdeJobViewServerTestAccess::Job job;
  job.sender = ":1.42";
  job.authorizeSender(":1.42");
  for (const auto* sender : {"", ":1.43", "org.kde.JobViewServer"}) {
    bool denied = false;
    try {
      job.authorizeSender(sender);
    } catch (const sdbus::Error& error) {
      denied = error.getName() == "org.freedesktop.DBus.Error.AccessDenied";
    }
    TEST_CHECK(denied);
  }
  TEST_CHECK(job.setDescriptionField(0, "Source", "file:///source"));
  TEST_CHECK(job.setDescriptionField(1, "Destination", "file:///destination"));
  TEST_CHECK(job.label1 == "Source" && job.value1 == "file:///source");
  TEST_CHECK(job.label2 == "Destination" && job.value2 == "file:///destination");
  TEST_CHECK(!job.setDescriptionField(2, "Invalid", "invalid"));
  TEST_CHECK(!job.setDescriptionField(std::numeric_limits<std::uint32_t>::max(), "Invalid", "invalid"));
  TEST_CHECK(job.setDescriptionField(0, {}, {}));
  TEST_CHECK(job.label1.empty() && job.value1.empty());
  TEST_CHECK(job.value2 == "file:///destination");
  TEST_CHECK(job.setDescriptionField(1, {}, {}));
  TEST_CHECK(job.label2.empty() && job.value2.empty());
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  job.processedBytes = maximum - 1;
  job.totalBytes = maximum;
  TEST_CHECK(job.effectivePercent() == 99);
  job.processedBytes = maximum;
  TEST_CHECK(job.effectivePercent() == 100);
  job.totalBytes = 1;
  TEST_CHECK(job.effectivePercent() == 100);
  job.totalBytes = 0;
  TEST_CHECK(!job.effectivePercent().has_value());
  job.percent = 42;
  TEST_CHECK(job.effectivePercent() == 42);
  // Exhaust small ratios against the straightforward (safe at these sizes) reference formula.
  job.percent.reset();
  for (std::uint64_t total = 1; total <= 255; ++total) {
    job.totalBytes = total;
    for (std::uint64_t done = 0; done <= total; ++done) {
      job.processedBytes = done;
      TEST_CHECK(job.effectivePercent() == static_cast<int>(done * 100 / total));
    }
  }
  TEST_CHECK(!KdeJobViewServerTestAccess::fromDouble(-std::numeric_limits<double>::infinity()));
  TEST_CHECK(!KdeJobViewServerTestAccess::fromDouble(std::numeric_limits<double>::infinity()));
  TEST_CHECK(!KdeJobViewServerTestAccess::fromDouble(std::numeric_limits<double>::quiet_NaN()));
  TEST_CHECK(!KdeJobViewServerTestAccess::fromDouble(18446744073709551616.0));
  TEST_CHECK(KdeJobViewServerTestAccess::fromDouble(-1.0) == 0);
  TEST_CHECK(KdeJobViewServerTestAccess::fromDouble(42.9) == 42);
  std::cout << "PASS: unique sender authorization, V2 description mapping/clear, safe numeric progress\n";
}

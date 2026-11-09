#include "notification/automatic_dnd_state.h"
#include "tests/test_check.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {
  using Owner = AutomaticDndState::Owner;

  struct Dnd {
    AutomaticDndState state;
    bool enabled = false;

    void set(Owner owner, bool active) {
      enabled = state.setOwner(owner, active, enabled);
      // NotificationManager invokes its state callback synchronously for automatic writes too.
      state.observe(enabled);
    }

    void manual(bool value) {
      enabled = value;
      state.observe(enabled);
    }
  };

  void reportedOverlaps() {
    Dnd dnd;
    dnd.set(Owner::QuietHours, true);
    dnd.set(Owner::GameMode, true);
    dnd.set(Owner::LockScreen, true);
    dnd.set(Owner::QuietHours, false);
    dnd.set(Owner::GameMode, false);
    TEST_CHECK(dnd.enabled);
    dnd.set(Owner::LockScreen, false);
    TEST_CHECK(!dnd.enabled);

    dnd.set(Owner::LockScreen, true);
    dnd.set(Owner::GameMode, true);
    dnd.set(Owner::QuietHours, true);
    dnd.set(Owner::GameMode, false);
    dnd.set(Owner::LockScreen, false);
    TEST_CHECK(dnd.enabled);
    dnd.set(Owner::QuietHours, false);
    TEST_CHECK(!dnd.enabled);
  }

  // Every valid interleaving of each owner's start/end, not just all-starts then all-ends.
  unsigned allOrderings(bool manualInitially) {
    constexpr std::array owners{Owner::QuietHours, Owner::GameMode, Owner::LockScreen};
    std::array events{0, 1, 2, 3, 4, 5};
    unsigned checked = 0;
    do {
      unsigned started = 0U;
      bool valid = true;
      for (int event : events) {
        const unsigned bit = 1U << (event / 2);
        if (event % 2 == 0) {
          started |= bit;
        } else if ((started & bit) == 0U) {
          valid = false;
          break;
        }
      }
      if (!valid) {
        continue;
      }
      Dnd dnd;
      dnd.manual(manualInitially);
      unsigned active = 0U;
      for (int event : events) {
        const unsigned bit = 1U << (event / 2);
        const bool starting = event % 2 == 0;
        active = starting ? active | bit : active & ~bit;
        dnd.set(owners[event / 2], starting);
        TEST_CHECK(dnd.enabled == (manualInitially || active != 0U));
        // Repeated syncs/reloads must be idempotent and never acquire manual ownership.
        dnd.set(owners[event / 2], starting);
        TEST_CHECK(dnd.enabled == (manualInitially || active != 0U));
      }
      ++checked;
    } while (std::next_permutation(events.begin(), events.end()));
    return checked;
  }

  void singleOwners() {
    for (Owner owner : {Owner::QuietHours, Owner::GameMode, Owner::LockScreen}) {
      Dnd dnd;
      dnd.set(owner, false); // An unmatched exit must not clear manual state.
      TEST_CHECK(!dnd.enabled);
      dnd.set(owner, true);
      TEST_CHECK(dnd.enabled);
      dnd.set(owner, false);
      TEST_CHECK(!dnd.enabled);
      dnd.manual(true);
      dnd.set(owner, false);
      TEST_CHECK(dnd.enabled);
      dnd.set(owner, true);
      dnd.set(owner, false);
      TEST_CHECK(dnd.enabled);
    }
  }

  void manualOverrides() {
    Dnd dnd;
    dnd.set(Owner::QuietHours, true);
    dnd.manual(false);
    dnd.set(Owner::QuietHours, true); // Timer/config reload in the same interval.
    TEST_CHECK(!dnd.enabled);
    dnd.set(Owner::GameMode, true);
    TEST_CHECK(dnd.enabled);
    dnd.set(Owner::LockScreen, true);
    dnd.set(Owner::GameMode, false);
    TEST_CHECK(dnd.enabled);
    dnd.set(Owner::LockScreen, false);
    TEST_CHECK(!dnd.enabled); // Ending another owner must not revive overridden quiet hours.
    dnd.set(Owner::QuietHours, false);
    dnd.set(Owner::QuietHours, true); // Next interval may enable DND again.
    TEST_CHECK(dnd.enabled);
    dnd.manual(false);
    dnd.manual(true);
    dnd.set(Owner::QuietHours, false);
    TEST_CHECK(dnd.enabled); // Explicit manual on survives schedule end.

    for (Owner owner : {Owner::GameMode, Owner::LockScreen}) {
      Dnd alone;
      alone.set(owner, true);
      alone.manual(false);
      alone.set(owner, true);
      TEST_CHECK(!alone.enabled);
      alone.set(owner, false);
      TEST_CHECK(!alone.enabled);
      alone.set(owner, true);
      TEST_CHECK(alone.enabled);
      alone.manual(false);
      alone.manual(true);
      alone.set(owner, false);
      TEST_CHECK(alone.enabled);
    }

    Dnd overlapping;
    overlapping.set(Owner::QuietHours, true);
    overlapping.set(Owner::GameMode, true);
    overlapping.set(Owner::LockScreen, true);
    overlapping.manual(false);
    overlapping.set(Owner::GameMode, false);
    overlapping.set(Owner::LockScreen, false);
    overlapping.set(Owner::QuietHours, true);
    TEST_CHECK(!overlapping.enabled);
    overlapping.manual(true);
    overlapping.set(Owner::QuietHours, false);
    TEST_CHECK(overlapping.enabled);
  }
} // namespace

int main() {
  reportedOverlaps();
  singleOwners();
  manualOverrides();
  const unsigned automatic = allOrderings(false);
  const unsigned manual = allOrderings(true);
  TEST_CHECK(automatic == 90U);
  TEST_CHECK(manual == automatic);
  std::printf(
      "DND ownership passed: %u automatic and %u manual-on orderings, overlaps, single owners, manual overrides\n",
      automatic, manual
  );
  return 0;
}

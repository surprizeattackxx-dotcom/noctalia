#include "shell/bar/widgets/taskbar_preview_policy.h"
#include "test_check.h"

int main() {
  using noctalia::taskbar::PreviewHover;
  PreviewHover hover;
  TEST_CHECK(!hover.shouldShow());
  TEST_CHECK(hover.shouldClose());
  hover.enterAnchor();
  TEST_CHECK(hover.shouldShow());
  TEST_CHECK(!hover.shouldClose());
  hover.leaveAnchor();
  TEST_CHECK(!hover.shouldShow());
  TEST_CHECK(hover.shouldClose());
  hover.enterPopup();
  TEST_CHECK(!hover.shouldClose());
  hover.enterAnchor();
  hover.leavePopup();
  TEST_CHECK(!hover.shouldClose());
  hover.leaveAnchor();
  TEST_CHECK(hover.shouldClose());
  hover.enterPopup();
  hover.cancel();
  TEST_CHECK(hover.shouldClose());
  TEST_CHECK(!hover.shouldShow());
  return 0;
}

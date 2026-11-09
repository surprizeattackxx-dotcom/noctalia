#include "shell/bar/widgets/caffeine_widget_definition.h"

const noctalia::bar::WidgetDefinition<IdleInhibitorWidget::Options>& caffeineWidgetDefinition() {
  using noctalia::bar::field;
  using Options = IdleInhibitorWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "caffeine",
      .fields = {
          field<&Options::hideWhenOff>({
              .key = "hide_when_off",
          }),
      },
  };
  return definition;
}

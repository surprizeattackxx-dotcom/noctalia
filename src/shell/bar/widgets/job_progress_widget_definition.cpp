#include "shell/bar/widgets/job_progress_widget_definition.h"

const noctalia::bar::WidgetDefinition<JobProgressWidget::Options>& jobProgressWidgetDefinition() {
  using noctalia::bar::field;
  using Options = JobProgressWidget::Options;

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "job_progress",
      .fields = {
          field<&Options::activeColor>({
              .key = "active_color",
          }),
      },
  };
  return definition;
}

#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/idle_inhibitor_widget.h"

[[nodiscard]] const noctalia::bar::WidgetDefinition<IdleInhibitorWidget::Options>& caffeineWidgetDefinition();

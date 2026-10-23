// Hajinmon native renderer - graphics system.

#include "plume_graphics_system.h"

#include <rex/logging/macros.h>

#include "plume_command_processor.h"

namespace hajinmon::renderer {

using rex::X_STATUS;  // For X_STATUS_SUCCESS.

PlumeGraphicsSystem::PlumeGraphicsSystem() = default;

PlumeGraphicsSystem::~PlumeGraphicsSystem() = default;

rex::X_STATUS PlumeGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  app_context_ = app_context;
  REXLOG_INFO("Hajinmon: native Plume renderer selected (no Xenos emulation)");
  return X_STATUS_SUCCESS;
}

std::unique_ptr<rex::graphics::CommandProcessor> PlumeGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<PlumeCommandProcessor>(this, kernel_state_);
}

}  // namespace hajinmon::renderer

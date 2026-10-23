// doa4 - choice of renderer (see renderer_select.h).

#include "renderer_select.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/ui/window.h>

#if HAJINMON_PLUME_RENDERER
#include "plume_graphics_system.h"
#endif

REXCVAR_DEFINE_STRING(renderer, "xenia", "GPU",
                      "Renderer: xenia (Xenos emulation) or plume (experimental native renderer)")
    .allowed({"xenia", "plume"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace doa4 {

std::unique_ptr<rex::system::IGraphicsSystem> CreateSelectedGraphicsSystem() {
  if (REXCVAR_GET(renderer) != "plume") {
    return nullptr;
  }
#if HAJINMON_PLUME_RENDERER
  return std::make_unique<hajinmon::renderer::PlumeGraphicsSystem>();
#else
  REXLOG_ERROR("doa4: this build has no native renderer, using Xenos emulation");
  return nullptr;
#endif
}

void AttachWindowToRenderer(rex::system::IGraphicsSystem* graphics, rex::ui::Window* window) {
#if HAJINMON_PLUME_RENDERER
  auto* plume = dynamic_cast<hajinmon::renderer::PlumeGraphicsSystem*>(graphics);
  if (plume && window) {
    plume->AttachWindow(window->GetNativeWindowHandle());
  }
#else
  (void)graphics;
  (void)window;
#endif
}

}  // namespace doa4

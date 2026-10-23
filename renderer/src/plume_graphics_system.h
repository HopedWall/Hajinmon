// Hajinmon native renderer - graphics system.
//
// Replaces the SDK's Xenos emulation (rexgpu-xenos) when Hajinmon runs with
// --renderer=plume. The SDK base class keeps doing what is not rendering: GPU
// registers (MMIO), the guest vblank thread, interrupts and the command
// processor thread that parses the GPU command stream. Rendering goes through
// PlumeCommandProcessor and PlumeRenderer.
//
// There is no SDK presenter: the renderer owns the window's swap chain, so the
// SDK overlays (ImGui) are not available in this mode yet.

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/graphics_system.h>

#include "plume_renderer.h"

namespace hajinmon::renderer {

class PlumeGraphicsSystem : public rex::graphics::GraphicsSystem {
 public:
  PlumeGraphicsSystem();
  ~PlumeGraphicsSystem() override;

  std::string name() const override { return "Plume"; }

  // Nothing to set up before the window exists: the swap chain is created
  // once the app hands over the window (AttachWindow).
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  bool has_presentation() const override { return true; }

  // Native window handle (HWND on Windows). Called by the app once the window exists.
  void AttachWindow(void* native_window) { renderer_.AttachWindow(native_window); }

  PlumeRenderer& renderer() { return renderer_; }

 protected:
  // No SDK graphics provider: Plume owns the device.
  void CreateProvider(bool with_presentation) override { (void)with_presentation; }
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override;

 private:
  PlumeRenderer renderer_;
};

}  // namespace hajinmon::renderer

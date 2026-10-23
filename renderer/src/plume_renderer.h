// Hajinmon native renderer - Plume device, swap chain and frame submission.
//
// Owns the host GPU objects. All methods except AttachWindow run on the GPU
// command processor thread.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "plume_render_interface.h"

namespace hajinmon::renderer {

class PlumeRenderer {
 public:
  PlumeRenderer();
  ~PlumeRenderer();

  // Creates the render interface (D3D12 or Vulkan, see the plume_backend
  // option) and the device. Returns false if no usable GPU was found.
  bool Initialize();
  void Shutdown();

  // Window to present to (an HWND on Windows). Called from the UI thread once
  // the window exists; the swap chain is created on the next frame.
  void AttachWindow(void* native_window);

  // Milestone 1: clears the next swap chain image and presents it. Returns
  // false when nothing could be presented (no window yet, minimized...).
  bool PresentClear(float r, float g, float b);

  const char* backend_name() const { return backend_name_; }

 private:
  bool EnsureSwapChain();
  void DestroySwapChain();

  const char* backend_name_ = "none";
  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_;
  std::unique_ptr<plume::RenderCommandList> command_list_;
  std::unique_ptr<plume::RenderCommandFence> fence_;
  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphore_;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release_semaphores_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;

  std::atomic<void*> pending_window_{nullptr};
  void* window_ = nullptr;
};

}  // namespace hajinmon::renderer

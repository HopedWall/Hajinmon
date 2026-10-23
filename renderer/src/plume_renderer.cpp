// Hajinmon native renderer - Plume device, swap chain and frame submission.

#include "plume_renderer.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

REXCVAR_DEFINE_STRING(plume_backend, "auto", "GPU/Plume",
                      "Graphics API of the native renderer: auto, d3d12, vulkan")
    .allowed({"auto", "d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace plume {
#if defined(_WIN32)
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
extern std::unique_ptr<RenderInterface> CreateVulkanInterface();
}  // namespace plume

namespace hajinmon::renderer {

namespace {
constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr uint32_t kSwapChainImages = 3;
}  // namespace

PlumeRenderer::PlumeRenderer() = default;

PlumeRenderer::~PlumeRenderer() { Shutdown(); }

bool PlumeRenderer::Initialize() {
  std::string backend = REXCVAR_GET(plume_backend);
#if defined(_WIN32)
  if (backend == "auto" || backend == "d3d12") {
    interface_ = plume::CreateD3D12Interface();
    backend_name_ = "Direct3D 12";
  }
#endif
  if (!interface_ && (backend == "auto" || backend == "vulkan")) {
    interface_ = plume::CreateVulkanInterface();
    backend_name_ = "Vulkan";
  }
  if (!interface_) {
    REXLOG_ERROR("Plume: could not create the {} render interface", backend);
    return false;
  }
  device_ = interface_->createDevice();
  if (!device_) {
    REXLOG_ERROR("Plume: could not create a {} device", backend_name_);
    return false;
  }
  queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  command_list_ = queue_->createCommandList();
  fence_ = device_->createCommandFence();
  acquire_semaphore_ = device_->createCommandSemaphore();
  REXLOG_INFO("Plume: {} device created", backend_name_);
  return true;
}

void PlumeRenderer::Shutdown() {
  if (queue_ && fence_) {
    // Nothing must be in flight when the objects below are destroyed.
    DestroySwapChain();
  }
  acquire_semaphore_.reset();
  fence_.reset();
  command_list_.reset();
  queue_.reset();
  device_.reset();
  interface_.reset();
}

void PlumeRenderer::AttachWindow(void* native_window) { pending_window_.store(native_window); }

void PlumeRenderer::DestroySwapChain() {
  framebuffers_.clear();
  release_semaphores_.clear();
  swap_chain_.reset();
}

bool PlumeRenderer::EnsureSwapChain() {
  void* window = pending_window_.load();
  if (!window) {
    return false;
  }
  if (window != window_) {
    DestroySwapChain();
    window_ = window;
  }
  if (!swap_chain_) {
    swap_chain_ = queue_->createSwapChain(plume::RenderSwapChainDesc(
        static_cast<plume::RenderWindow>(window_), kSwapChainFormat, kSwapChainImages));
    if (!swap_chain_) {
      REXLOG_ERROR("Plume: could not create the swap chain");
      return false;
    }
    swap_chain_->setVsyncEnabled(true);
    swap_chain_->resize();
  } else if (swap_chain_->needsResize()) {
    framebuffers_.clear();
    swap_chain_->resize();
  }
  if (swap_chain_->isEmpty()) {
    return false;  // Minimized.
  }
  if (framebuffers_.size() != swap_chain_->getTextureCount()) {
    framebuffers_.clear();
    for (uint32_t i = 0; i < swap_chain_->getTextureCount(); ++i) {
      const plume::RenderTexture* color = swap_chain_->getTexture(i);
      plume::RenderFramebufferDesc desc;
      desc.colorAttachments = &color;
      desc.colorAttachmentsCount = 1;
      framebuffers_.push_back(device_->createFramebuffer(desc));
    }
  }
  while (release_semaphores_.size() < swap_chain_->getTextureCount()) {
    release_semaphores_.push_back(device_->createCommandSemaphore());
  }
  return true;
}

bool PlumeRenderer::PresentClear(float r, float g, float b) {
  if (!device_ || !EnsureSwapChain()) {
    return false;
  }
  uint32_t image = 0;
  if (!swap_chain_->acquireTexture(acquire_semaphore_.get(), &image)) {
    return false;
  }
  plume::RenderTexture* target = swap_chain_->getTexture(image);
  command_list_->begin();
  command_list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                          plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
  command_list_->setFramebuffer(framebuffers_[image].get());
  uint32_t width = swap_chain_->getWidth(), height = swap_chain_->getHeight();
  command_list_->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  command_list_->setScissors(plume::RenderRect(0, 0, int32_t(width), int32_t(height)));
  command_list_->clearColor(0, plume::RenderColor(r, g, b, 1.0f));
  command_list_->barriers(plume::RenderBarrierStage::NONE,
                          plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
  command_list_->end();

  const plume::RenderCommandList* lists = command_list_.get();
  plume::RenderCommandSemaphore* wait = acquire_semaphore_.get();
  plume::RenderCommandSemaphore* signal = release_semaphores_[image].get();
  queue_->executeCommandLists(&lists, 1, &wait, 1, &signal, 1, fence_.get());
  swap_chain_->present(image, &signal, 1);
  queue_->waitForCommandFence(fence_.get());
  return true;
}

}  // namespace hajinmon::renderer

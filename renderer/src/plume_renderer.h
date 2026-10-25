// Hajinmon native renderer - Plume device, swap chain and frames.
//
// Owns the host GPU objects. All methods except AttachWindow run on the GPU
// command processor thread.
//
// The guest's GPU commands of a frame are recorded into one command list,
// submitted when the guest swaps. kFramesInFlight frames can be in flight;
// each has its own command list, fence, upload memory (for data copied to GPU
// resources) and constant memory (the per-draw constant blocks the translated
// shaders read).

#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "plume_render_interface.h"
#include "shader_compiler.h"

namespace hajinmon::renderer {

// A texture with the layout it was last transitioned to.
struct TrackedTexture {
  std::unique_ptr<plume::RenderTexture> texture;
  plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t samples = 1;

  void Transition(plume::RenderCommandList* command_list, plume::RenderTextureLayout new_layout) {
    if (layout != new_layout) {
      command_list->barriers(plume::RenderBarrierStage::ALL,
                             plume::RenderTextureBarrier(texture.get(), new_layout));
      layout = new_layout;
    }
  }
};

class PlumeRenderer {
 public:
  static constexpr uint32_t kFramesInFlight = 2;
  // Per frame, for the draw constant blocks.
  static constexpr uint32_t kConstantMemorySize = 32 << 20;

  PlumeRenderer();
  ~PlumeRenderer();

  // Creates the render interface (D3D12 or Vulkan, see the plume_backend
  // option), the device and the per-frame resources. Returns false if no
  // usable GPU was found.
  bool Initialize();
  void Shutdown();

  // Window to present to (an HWND on Windows, an SDL_Window elsewhere). Called from the UI thread once
  // the window exists; the swap chain is created on the next frame.
  void AttachWindow(void* native_window);

  const char* backend_name() const { return backend_name_; }
  // Vulkan takes SPIR-V shaders, Direct3D 12 DXIL.
  bool is_vulkan() const { return std::string_view(backend_name_) == "Vulkan"; }
  ShaderCompiler::Target shader_target() const {
    return is_vulkan() ? ShaderCompiler::Target::kSpirv : ShaderCompiler::Target::kDxil;
  }
  plume::RenderShaderFormat shader_format() const {
    return is_vulkan() ? plume::RenderShaderFormat::SPIRV : plume::RenderShaderFormat::DXIL;
  }
  plume::RenderDevice* device() const { return device_.get(); }
  // Host pixels per guest pixel along each axis (plume_resolution_scale).
  uint32_t resolution_scale() const { return resolution_scale_; }

  // Destroys GPU objects once the frames that may use them have completed.
  void Retire(std::unique_ptr<plume::RenderTexture> texture);
  void Retire(std::unique_ptr<plume::RenderFramebuffer> framebuffer);

  // The command list of the current frame, opened on first use (waiting for
  // the GPU to finish the frame that last used its resources).
  plume::RenderCommandList* command_list();
  // A command list of the current frame that the GPU executes before
  // command_list (for uploads that must come before all of the frame's draws).
  // Record into it only at the end of the frame, after command_list, so the
  // resource states tracked by Plume are those at the start of the frame.
  plume::RenderCommandList* upload_command_list();
  uint32_t frame_slot() const { return frame_slot_; }
  // Time spent waiting for the GPU to finish earlier frames since the last
  // call (GPU-bound when large).
  std::chrono::nanoseconds TakeGpuWaitTime() {
    return std::exchange(gpu_wait_time_, std::chrono::nanoseconds(0));
  }
  // GPU time measured with timestamps, for the frames finished since the last
  // call: whole frames, resolves and presentation.
  struct GpuTimes {
    uint64_t frames = 0;
    uint64_t frame_ns = 0;
    uint64_t resolve_ns = 0;
    uint64_t present_ns = 0;
    // Wall time from submission to the fence, for the frames that were waited
    // for (the others had finished before being checked).
    uint64_t latency_frames = 0;
    uint64_t latency_ns = 0;
  };
  GpuTimes TakeGpuTimes() { return std::exchange(gpu_times_, GpuTimes()); }
  // A timestamp in the current frame's command list (UINT32_MAX if the frame
  // has no room left), and a resolve measured between two of them.
  uint32_t WriteTimestamp();
  // plume_gpu_profile: a timestamp, after waiting for all the work before it
  // (exact on every driver, but slower), starting a segment of GPU time
  // counted under `label` until the next one.
  bool profiling() const { return profiling_; }
  void ProfileMark(std::string_view label);
  // The labels with the most GPU time since the last call, per frame.
  std::string TakeProfile();
  void AddResolveTime(uint32_t begin, uint32_t end);
  uint64_t frame_number() const { return frame_number_; }

  struct Allocation {
    plume::RenderBuffer* buffer = nullptr;
    uint64_t offset = 0;
    uint8_t* data = nullptr;
  };
  // CPU-written memory the GPU can read until the current frame completes.
  Allocation Upload(uint64_t size, uint64_t alignment = 16);
  // Draw constant memory of the current frame; data is null when full.
  Allocation AllocateConstants(uint32_t size);
  plume::RenderBuffer* constant_buffer(uint32_t slot) const {
    return frames_[slot].constants.get();
  }

  // Submits the frame, showing `source` (a region of `width` x `height` from
  // its top left corner, letterboxed to the window, optionally with red and
  // blue swapped) or black. Returns whether something was presented.
  bool EndFrame(TrackedTexture* source, uint32_t width, uint32_t height, bool swap_red_blue);

  // Saves the next presented frame as a PNG in ScreenshotDirectory(). Thread-safe.
  void RequestScreenshot();
  static std::filesystem::path ScreenshotDirectory();

  // Compiles a built-in HLSL shader for the current backend.
  std::unique_ptr<plume::RenderShader> CompileBuiltinShader(const char* hlsl,
                                                            ShaderCompiler::Stage stage);

 private:
  struct UploadChunk {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint8_t* data = nullptr;
    uint64_t size = 0;
  };
  struct Frame {
    std::unique_ptr<plume::RenderCommandList> command_list;
    // Recorded at the end of the frame, executed before command_list.
    std::unique_ptr<plume::RenderCommandList> upload_list;
    bool upload_list_open = false;
    std::unique_ptr<plume::RenderCommandFence> fence;
    // GPU timestamps: 0 at the start of the frame, then pairs around resolves,
    // then the start and the end of the presentation.
    std::unique_ptr<plume::RenderQueryPool> queries;
    uint32_t query_count = 0;
    std::vector<std::pair<uint32_t, uint32_t>> resolve_queries;
    uint32_t present_query = UINT32_MAX;
    // With plume_gpu_profile: timestamps starting labelled segments.
    std::vector<std::pair<uint32_t, uint32_t>> profile_marks;
    bool queries_pending = false;
    std::chrono::steady_clock::time_point submit_time;
    bool submitted = false;
    std::vector<UploadChunk> upload_chunks;
    size_t upload_chunk = 0;
    uint64_t upload_offset = 0;
    std::unique_ptr<plume::RenderBuffer> constants;
    uint8_t* constants_data = nullptr;
    uint32_t constants_offset = 0;
    std::vector<std::unique_ptr<plume::RenderTexture>> retired_textures;
    std::vector<std::unique_ptr<plume::RenderFramebuffer>> retired_framebuffers;
  };

  bool EnsureSwapChain();
  void DestroySwapChain();
  bool CreatePresentPipeline();
  void WaitForIdle();

  const char* backend_name_ = "none";
  uint32_t resolution_scale_ = 1;
  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_;
  Frame frames_[kFramesInFlight];
  uint32_t frame_slot_ = 0;
  std::chrono::nanoseconds gpu_wait_time_{0};
  bool timer_period_set_ = false;
  GpuTimes gpu_times_;
  bool profiling_ = false;
  std::unique_ptr<plume::RenderBuffer> profile_buffer_;
  std::unordered_map<std::string, uint32_t> profile_label_ids_;
  std::vector<std::string> profile_labels_;
  std::vector<uint64_t> profile_ns_;
  std::vector<uint64_t> profile_counts_;
  uint64_t profile_frames_ = 0;
  void ProfileBarrier(plume::RenderCommandList* list);
  void CollectGpuTimes(Frame& frame);
  uint64_t frame_number_ = 0;
  bool frame_open_ = false;

  std::unique_ptr<plume::RenderSwapChain> swap_chain_;
  // One per frame in flight: a semaphore must not be signalled again by an
  // acquire before the submission waiting for its previous signal has run.
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_semaphores_[kFramesInFlight];
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release_semaphores_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;

  // Present: a full screen triangle sampling the source texture.
  std::unique_ptr<plume::RenderPipelineLayout> present_layout_;
  std::unique_ptr<plume::RenderPipeline> present_pipeline_;
  std::unique_ptr<plume::RenderSampler> present_sampler_;
  std::unique_ptr<plume::RenderDescriptorSet> present_sets_[kFramesInFlight];

  // Debugging frame captures.
  std::unique_ptr<plume::RenderBuffer> capture_buffer_;
  uint64_t capture_buffer_size_ = 0;

  std::atomic<bool> screenshot_requested_{false};
  std::atomic<void*> pending_window_{nullptr};
  void* window_ = nullptr;
};

}  // namespace hajinmon::renderer

// Hajinmon native renderer - Plume device, swap chain and frames.

#include "plume_renderer.h"

#include "png_writer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#if defined(__linux__)
#include <SDL3/SDL.h>
#endif

REXCVAR_DEFINE_STRING(plume_backend, "auto", "GPU/Plume",
                      "Graphics API of the native renderer: auto, d3d12, vulkan")
    .allowed({"auto", "d3d12", "vulkan"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(plume_vsync, true, "GPU/Plume",
                    "Native renderer: present on vertical blank (no tearing)");
REXCVAR_DEFINE_BOOL(plume_letterbox, true, "GPU/Plume",
                    "Native renderer: keep the 16:9 aspect ratio (black bars), else stretch");
REXCVAR_DEFINE_INT32(plume_resolution_scale, 2, "GPU/Plume",
                     "Native renderer resolution scale (1 = 1280x720, 2 = 2560x1440...)")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(plume_capture_path, "", "GPU/Plume",
                      "Debugging: directory to save presented frames to as PNG (empty: off)");
REXCVAR_DEFINE_INT32(plume_capture_interval, 300, "GPU/Plume",
                     "Debugging: save every this many frames to plume_capture_path");

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
constexpr uint64_t kUploadChunkSize = 16 << 20;

const char kPresentShaders[] = R"(
struct PresentConstants {
  float2 uv_scale;
  // 1 to swap red and blue.
  float swap_red_blue;
  float padding;
};
[[vk::push_constant]] ConstantBuffer<PresentConstants> present_constants : register(b0, space1);
Texture2D present_texture : register(t0, space0);
SamplerState present_sampler : register(s1, space0);

struct VertexOutput {
  float2 uv : TEXCOORD0;
  float4 position : SV_Position;
};

#ifdef VERTEX
VertexOutput main(uint vertex_id : SV_VertexID) {
  VertexOutput output;
  float2 uv = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
  output.uv = uv;
  output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  return output;
}
#else
float4 main(VertexOutput input) : SV_Target0 {
  float3 color =
      present_texture.SampleLevel(present_sampler, input.uv * present_constants.uv_scale, 0.0f).rgb;
  return float4(present_constants.swap_red_blue != 0.0f ? color.bgr : color, 1.0f);
}
#endif
)";

#if defined(_WIN32)
bool ToRenderWindow(void* window, plume::RenderWindow& render_window) {
  render_window = static_cast<HWND>(window);
  return true;
}
#else
// Plume presents to an X11 window: the game's SDL window under X11 (or XWayland).
bool ToRenderWindow(void* window, plume::RenderWindow& render_window) {
  SDL_PropertiesID properties = SDL_GetWindowProperties(static_cast<SDL_Window*>(window));
  render_window.display = static_cast<Display*>(
      SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  render_window.window =
      static_cast<Window>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!render_window.display || !render_window.window) {
    const char* driver = SDL_GetCurrentVideoDriver();
    REXLOG_ERROR("Plume: the window is not an X11 window (SDL video driver: {}), set SDL_VIDEO_DRIVER=x11",
                 driver ? driver : "none");
    return false;
  }
  return true;
}
#endif

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
  resolution_scale_ = uint32_t(std::clamp(REXCVAR_GET(plume_resolution_scale), 1, 4));
  queue_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
  for (Frame& frame : frames_) {
    frame.command_list = queue_->createCommandList();
    frame.fence = device_->createCommandFence();
    frame.constants = device_->createBuffer(
        plume::RenderBufferDesc::UploadBuffer(kConstantMemorySize, plume::RenderBufferFlag::STORAGE));
    frame.constants_data = static_cast<uint8_t*>(frame.constants->map());
  }
  acquire_semaphore_ = device_->createCommandSemaphore();
  if (!CreatePresentPipeline()) {
    REXLOG_ERROR("Plume: could not create the present pipeline");
    return false;
  }
  REXLOG_INFO("Plume: {} device created", backend_name_);
  return true;
}

void PlumeRenderer::WaitForIdle() {
  for (Frame& frame : frames_) {
    if (frame.submitted) {
      queue_->waitForCommandFence(frame.fence.get());
      frame.submitted = false;
    }
  }
}

void PlumeRenderer::Shutdown() {
  if (!device_) {
    return;
  }
  // Nothing must be in flight when the objects below are destroyed.
  WaitForIdle();
  DestroySwapChain();
  for (auto& set : present_sets_) {
    set.reset();
  }
  present_pipeline_.reset();
  present_layout_.reset();
  present_sampler_.reset();
  for (Frame& frame : frames_) {
    if (frame.constants) {
      frame.constants->unmap();
    }
    for (UploadChunk& chunk : frame.upload_chunks) {
      chunk.buffer->unmap();
    }
    frame = Frame();
  }
  acquire_semaphore_.reset();
  queue_.reset();
  device_.reset();
  interface_.reset();
}

void PlumeRenderer::AttachWindow(void* native_window) { pending_window_.store(native_window); }

std::unique_ptr<plume::RenderShader> PlumeRenderer::CompileBuiltinShader(
    const char* hlsl, ShaderCompiler::Stage stage) {
  thread_local ShaderCompiler compiler;
  std::vector<uint8_t> binary;
  std::string errors;
  std::string source = std::string(stage == ShaderCompiler::Stage::kVertex   ? "#define VERTEX 1\n"
                                   : stage == ShaderCompiler::Stage::kPixel  ? "#define PIXEL 1\n"
                                                                             : "#define GEOMETRY 1\n") +
                       hlsl;
  if (!compiler.Initialize() || !compiler.Compile(source, stage, shader_target(), binary, errors)) {
    REXLOG_ERROR("Plume: built-in shader failed to compile: {}", errors);
    return nullptr;
  }
  return device_->createShader(binary.data(), binary.size(), "main", shader_format());
}

bool PlumeRenderer::CreatePresentPipeline() {
  plume::RenderSamplerDesc sampler_desc;
  sampler_desc.addressU = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressV = plume::RenderTextureAddressMode::CLAMP;
  sampler_desc.addressW = plume::RenderTextureAddressMode::CLAMP;
  present_sampler_ = device_->createSampler(sampler_desc);

  plume::RenderDescriptorRange ranges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::TEXTURE, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::SAMPLER, 1, 1),
  };
  plume::RenderDescriptorSetDesc set_desc(ranges, 2);
  plume::RenderPushConstantRange push_constants(0, 1, 0, 16, plume::RenderShaderStageFlag::PIXEL);
  plume::RenderPipelineLayoutDesc layout_desc(&push_constants, 1, &set_desc, 1);
  present_layout_ = device_->createPipelineLayout(layout_desc);
  for (auto& set : present_sets_) {
    set = device_->createDescriptorSet(set_desc);
    set->setSampler(1, present_sampler_.get());
  }

  auto vertex_shader = CompileBuiltinShader(kPresentShaders, ShaderCompiler::Stage::kVertex);
  auto pixel_shader = CompileBuiltinShader(kPresentShaders, ShaderCompiler::Stage::kPixel);
  if (!present_layout_ || !vertex_shader || !pixel_shader) {
    return false;
  }
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = present_layout_.get();
  desc.vertexShader = vertex_shader.get();
  desc.pixelShader = pixel_shader.get();
  desc.renderTargetFormat[0] = kSwapChainFormat;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.renderTargetCount = 1;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  present_pipeline_ = device_->createGraphicsPipeline(desc);
  return present_pipeline_ != nullptr;
}

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
    WaitForIdle();
    DestroySwapChain();
    window_ = window;
  }
  if (!swap_chain_) {
    plume::RenderWindow render_window{};
    if (!ToRenderWindow(window_, render_window)) {
      pending_window_.store(nullptr);
      return false;
    }
    swap_chain_ = queue_->createSwapChain(
        plume::RenderSwapChainDesc(render_window, kSwapChainFormat, kSwapChainImages));
    if (!swap_chain_) {
      REXLOG_ERROR("Plume: could not create the swap chain");
      return false;
    }
    swap_chain_->setVsyncEnabled(REXCVAR_GET(plume_vsync));
    swap_chain_->resize();
  } else if (swap_chain_->needsResize()) {
    WaitForIdle();
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

plume::RenderCommandList* PlumeRenderer::command_list() {
  Frame& frame = frames_[frame_slot_];
  if (!frame_open_) {
    if (frame.submitted) {
      queue_->waitForCommandFence(frame.fence.get());
      frame.submitted = false;
    }
    frame.retired_textures.clear();
    frame.retired_framebuffers.clear();
    frame.upload_chunk = 0;
    frame.upload_offset = 0;
    frame.constants_offset = 0;
    frame.command_list->begin();
    frame_open_ = true;
  }
  return frame.command_list.get();
}

void PlumeRenderer::Retire(std::unique_ptr<plume::RenderTexture> texture) {
  frames_[frame_slot_].retired_textures.push_back(std::move(texture));
}

void PlumeRenderer::Retire(std::unique_ptr<plume::RenderFramebuffer> framebuffer) {
  frames_[frame_slot_].retired_framebuffers.push_back(std::move(framebuffer));
}

std::filesystem::path PlumeRenderer::ScreenshotDirectory() {
#ifdef _WIN32
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  return std::filesystem::path(home ? home : ".") / "Pictures" / "Hajinmon";
}

void PlumeRenderer::RequestScreenshot() { screenshot_requested_ = true; }

PlumeRenderer::Allocation PlumeRenderer::Upload(uint64_t size, uint64_t alignment) {
  command_list();
  Frame& frame = frames_[frame_slot_];
  while (true) {
    if (frame.upload_chunk >= frame.upload_chunks.size()) {
      UploadChunk chunk;
      chunk.size = std::max(kUploadChunkSize, size);
      chunk.buffer = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(
          chunk.size, plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::STORAGE));
      chunk.data = static_cast<uint8_t*>(chunk.buffer->map());
      frame.upload_chunks.push_back(std::move(chunk));
    }
    UploadChunk& chunk = frame.upload_chunks[frame.upload_chunk];
    uint64_t offset = (frame.upload_offset + alignment - 1) / alignment * alignment;
    if (offset + size <= chunk.size) {
      frame.upload_offset = offset + size;
      return {chunk.buffer.get(), offset, chunk.data + offset};
    }
    ++frame.upload_chunk;
    frame.upload_offset = 0;
  }
}

PlumeRenderer::Allocation PlumeRenderer::AllocateConstants(uint32_t size) {
  command_list();
  Frame& frame = frames_[frame_slot_];
  uint32_t offset = (frame.constants_offset + 255) & ~uint32_t(255);
  if (uint64_t(offset) + size > kConstantMemorySize) {
    return {};
  }
  frame.constants_offset = offset + size;
  return {frame.constants.get(), offset, frame.constants_data + offset};
}

bool PlumeRenderer::EndFrame(TrackedTexture* source, uint32_t width, uint32_t height,
                             bool swap_red_blue) {
  plume::RenderCommandList* list = command_list();
  Frame& frame = frames_[frame_slot_];
  bool presenting = EnsureSwapChain();
  uint32_t image = 0;
  if (presenting && !swap_chain_->acquireTexture(acquire_semaphore_.get(), &image)) {
    presenting = false;
  }
  if (presenting) {
    plume::RenderTexture* target = swap_chain_->getTexture(image);
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
    if (source) {
      source->Transition(list, plume::RenderTextureLayout::SHADER_READ);
    }
    list->setFramebuffer(framebuffers_[image].get());
    uint32_t target_width = swap_chain_->getWidth(), target_height = swap_chain_->getHeight();
    list->setScissors(plume::RenderRect(0, 0, int32_t(target_width), int32_t(target_height)));
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    if (source && width && height) {
      // Letterboxed to the aspect ratio of the source, or stretched.
      float scale = std::min(float(target_width) / float(width), float(target_height) / float(height));
      float view_width = float(width) * scale, view_height = float(height) * scale;
      if (!REXCVAR_GET(plume_letterbox)) {
        view_width = float(target_width);
        view_height = float(target_height);
      }
      list->setViewports(plume::RenderViewport((float(target_width) - view_width) * 0.5f,
                                               (float(target_height) - view_height) * 0.5f,
                                               view_width, view_height));
      plume::RenderDescriptorSet* set = present_sets_[frame_slot_].get();
      set->setTexture(0, source->texture.get(), plume::RenderTextureLayout::SHADER_READ);
      list->setPipeline(present_pipeline_.get());
      list->setGraphicsPipelineLayout(present_layout_.get());
      list->setGraphicsDescriptorSet(set, 0);
      float constants[4] = {float(width) / float(source->width),
                            float(height) / float(source->height), swap_red_blue ? 1.0f : 0.0f, 0.0f};
      list->setGraphicsPushConstants(0, constants);
      list->drawInstanced(3, 1, 0, 0);
    }
    list->barriers(plume::RenderBarrierStage::NONE,
                   plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
  }
  // Screenshots (F3), and the periodic debugging capture of the presented image.
  std::string capture_path = REXCVAR_GET(plume_capture_path);
  int32_t capture_interval = std::max(REXCVAR_GET(plume_capture_interval), 1);
  bool capturable = source && width && height && source->format == plume::RenderFormat::R8G8B8A8_UNORM;
  bool screenshot = capturable && screenshot_requested_.exchange(false);
  bool capture = capturable && (screenshot || (!capture_path.empty() &&
                                               frame_number_ % uint64_t(capture_interval) == 0));
  uint32_t capture_row = (width * 4 + 255) & ~255u;
  if (capture) {
    uint64_t size = uint64_t(capture_row) * height;
    if (!capture_buffer_ || capture_buffer_size_ < size) {
      capture_buffer_ = device_->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(size));
      capture_buffer_size_ = size;
    }
    source->Transition(list, plume::RenderTextureLayout::COPY_SOURCE);
    plume::RenderBox box(0, 0, int32_t(width), int32_t(height));
    list->copyTextureRegion(
        plume::RenderTextureCopyLocation::PlacedFootprint(capture_buffer_.get(), source->format, width,
                                                          height, 1, capture_row / 4),
        plume::RenderTextureCopyLocation::Subresource(source->texture.get()), 0, 0, 0, &box);
  }
  list->end();

  const plume::RenderCommandList* lists = list;
  if (presenting) {
    plume::RenderCommandSemaphore* wait = acquire_semaphore_.get();
    plume::RenderCommandSemaphore* signal = release_semaphores_[image].get();
    queue_->executeCommandLists(&lists, 1, &wait, 1, &signal, 1, frame.fence.get());
    swap_chain_->present(image, &signal, 1);
  } else {
    queue_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, frame.fence.get());
  }
  frame.submitted = true;
  if (capture) {
    queue_->waitForCommandFence(frame.fence.get());
    frame.submitted = false;
    const auto* data = static_cast<const uint8_t*>(capture_buffer_->map());
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
      uint8_t* row = pixels.data() + size_t(y) * width * 4;
      std::memcpy(row, data + size_t(y) * capture_row, size_t(width) * 4);
      for (uint32_t x = 0; x < width; ++x) {
        if (swap_red_blue) {
          std::swap(row[x * 4], row[x * 4 + 2]);
        }
        row[x * 4 + 3] = 255;
      }
    }
    capture_buffer_->unmap();
    std::filesystem::path path;
    if (screenshot) {
      std::time_t now = std::time(nullptr);
      char name[64];
      std::strftime(name, sizeof(name), "Hajinmon %Y-%m-%d %H-%M-%S.png", std::localtime(&now));
      std::error_code error;
      std::filesystem::create_directories(ScreenshotDirectory(), error);
      path = ScreenshotDirectory() / name;
    } else {
      path = std::filesystem::path(capture_path) / fmt::format("plume_frame_{:06}.png", frame_number_);
    }
    // Compressing takes a moment: not on the GPU thread.
    std::thread([path, width, height, pixels = std::move(pixels)]() {
      if (WritePng(path, width, height, pixels.data())) {
        REXLOG_INFO("Plume: saved {}", path.string());
      } else {
        REXLOG_ERROR("Plume: could not write {}", path.string());
      }
    }).detach();
  }
  frame_open_ = false;
  frame_slot_ = (frame_slot_ + 1) % kFramesInFlight;
  ++frame_number_;
  return presenting;
}

}  // namespace hajinmon::renderer

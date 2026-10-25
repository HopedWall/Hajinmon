// Hajinmon native renderer - graphics pipelines.

#include "pipeline_cache.h"

#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>

#include <rex/logging/macros.h>
#include <xxhash.h>

#include "texture_cache.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hajinmon::renderer {

namespace {

struct PipelineCreation {
  plume::RenderDevice* device;
  const plume::RenderGraphicsPipelineDesc* desc;
  std::unique_ptr<plume::RenderPipeline> pipeline;
};

void CreatePipeline(void* context) {
  auto* creation = static_cast<PipelineCreation*>(context);
  creation->pipeline = creation->device->createGraphicsPipeline(*creation->desc);
}

// Runs `function`, returning false if it crashed. Some drivers crash while
// compiling certain shaders when the pipeline is created (the AMD Direct3D 12
// driver on Radeon 890M): the draws using it are skipped instead of the game
// closing. No C++ objects here, as structured exception handling requires.
bool CallGuarded(void (*function)(void*), void* context) {
#ifdef _WIN32
  __try {
    function(context);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
#else
  function(context);
#endif
  return true;
}

}  // namespace

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;

namespace {

// Rectangle lists: each triangle is 3 corners of a rectangle, the fourth is
// the corner opposite to the right angle, at v1 + v2 - v0 for all outputs.
const char kRectangleListShader[] = R"(
struct XeVertexOutput {
  float4 interpolators[16] : TEXCOORD0;
  float4 position : SV_Position;
};

[maxvertexcount(4)]
void main(triangle XeVertexOutput input[3], inout TriangleStream<XeVertexOutput> output) {
  // The right angle is opposite to the longest edge.
  float2 p0 = input[0].position.xy / input[0].position.w;
  float2 p1 = input[1].position.xy / input[1].position.w;
  float2 p2 = input[2].position.xy / input[2].position.w;
  float d01 = dot(p1 - p0, p1 - p0), d02 = dot(p2 - p0, p2 - p0), d12 = dot(p2 - p1, p2 - p1);
  uint corner = 0u, a = 1u, b = 2u;
  if (d02 > d12 && d02 >= d01) {
    corner = 1u; a = 0u; b = 2u;
  } else if (d01 > d12 && d01 > d02) {
    corner = 2u; a = 0u; b = 1u;
  }
  XeVertexOutput opposite;
  [unroll] for (uint i = 0u; i < 16u; ++i) {
    opposite.interpolators[i] = input[a].interpolators[i] + input[b].interpolators[i] -
                                input[corner].interpolators[i];
  }
  opposite.position = input[a].position + input[b].position - input[corner].position;
  output.Append(input[corner]);
  output.Append(input[a]);
  output.Append(input[b]);
  output.Append(opposite);
}
)";

plume::RenderBlend ConvertBlend(xenos::BlendFactor factor, bool is_alpha) {
  switch (factor) {
    case xenos::BlendFactor::kZero:
      return plume::RenderBlend::ZERO;
    case xenos::BlendFactor::kOne:
      return plume::RenderBlend::ONE;
    case xenos::BlendFactor::kSrcColor:
      return is_alpha ? plume::RenderBlend::SRC_ALPHA : plume::RenderBlend::SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return is_alpha ? plume::RenderBlend::INV_SRC_ALPHA : plume::RenderBlend::INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha:
      return plume::RenderBlend::SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return plume::RenderBlend::INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor:
      return is_alpha ? plume::RenderBlend::DEST_ALPHA : plume::RenderBlend::DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor:
      return is_alpha ? plume::RenderBlend::INV_DEST_ALPHA : plume::RenderBlend::INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha:
      return plume::RenderBlend::DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return plume::RenderBlend::INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha:
      return plume::RenderBlend::BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return plume::RenderBlend::INV_BLEND_FACTOR;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return is_alpha ? plume::RenderBlend::ONE : plume::RenderBlend::SRC_ALPHA_SAT;
    default:
      return plume::RenderBlend::ONE;
  }
}

plume::RenderBlendOperation ConvertBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract:
      return plume::RenderBlendOperation::SUBTRACT;
    case xenos::BlendOp::kMin:
      return plume::RenderBlendOperation::MIN;
    case xenos::BlendOp::kMax:
      return plume::RenderBlendOperation::MAX;
    case xenos::BlendOp::kRevSubtract:
      return plume::RenderBlendOperation::REV_SUBTRACT;
    default:
      return plume::RenderBlendOperation::ADD;
  }
}

plume::RenderComparisonFunction ConvertCompare(xenos::CompareFunction function) {
  // Same order, starting from NEVER.
  return plume::RenderComparisonFunction(uint32_t(function) + 1);
}

plume::RenderStencilOp ConvertStencilOp(xenos::StencilOp op) {
  return plume::RenderStencilOp(uint32_t(op) + 1);
}

}  // namespace

plume::RenderDescriptorSetDesc PipelineCache::GetFrameSetDesc() {
  static const plume::RenderDescriptorRange kRanges[] = {
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 0, 1),
      plume::RenderDescriptorRange(plume::RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER, 1, 1),
  };
  return plume::RenderDescriptorSetDesc(kRanges, 2);
}

bool PipelineCache::Initialize() {
  plume::RenderDescriptorSetDesc sets[1 + TextureCache::kSetCount];
  sets[0] = GetFrameSetDesc();
  for (uint32_t i = 0; i < TextureCache::kSetCount; ++i) {
    sets[1 + i] = TextureCache::GetDescriptorSetDesc(TextureCache::DescriptorSet(i));
  }
  plume::RenderPushConstantRange push_constants(
      0, 8, 0, 32,
      plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::GEOMETRY |
          plume::RenderShaderStageFlag::PIXEL);
  plume::RenderPipelineLayoutDesc desc(&push_constants, 1, sets, 1 + TextureCache::kSetCount);
  layout_ = renderer_.device()->createPipelineLayout(desc);
  rectangle_list_shader_ =
      renderer_.CompileBuiltinShader(kRectangleListShader, ShaderCompiler::Stage::kGeometry);
  return layout_ && rectangle_list_shader_;
}

PipelineCache::~PipelineCache() { StopPrewarm(); }

void PipelineCache::StopPrewarm() {
  prewarm_stop_ = true;
  if (prewarm_thread_.joinable()) {
    prewarm_thread_.join();
  }
}

void PipelineCache::StartPrewarm(const std::filesystem::path& directory, ShaderCache& shader_cache) {
  StopPrewarm();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    record_path_ = directory.empty() ? std::filesystem::path() : directory / "pipelines.bin";
  }
  // Records: version, size, description. Descriptions of another layout
  // (an older build) are skipped.
  std::vector<Description> recorded;
  std::ifstream file(record_path_, std::ios::binary);
  uint32_t header[2];
  while (file.read(reinterpret_cast<char*>(header), sizeof(header))) {
    if (header[1] > 4096) {
      break;
    }
    Description description = {};
    std::vector<char> data(header[1]);
    if (!file.read(data.data(), header[1])) {
      break;
    }
    if (header[0] == kRecordVersion && header[1] == sizeof(Description)) {
      std::memcpy(&description, data.data(), sizeof(Description));
      recorded.push_back(description);
    }
  }
  REXLOG_INFO("Plume: {} pipelines recorded in previous sessions", recorded.size());
  if (recorded.empty()) {
    return;
  }
  prewarm_stop_ = false;
  prewarm_thread_ = std::thread([this, recorded = std::move(recorded), &shader_cache] {
    auto start = std::chrono::steady_clock::now();
    uint32_t created = 0, skipped = 0;
    for (const Description& description : recorded) {
      if (prewarm_stop_) {
        return;
      }
      // The shader prewarm (running at the same time) provides the shaders.
      const CompiledShader* vertex = shader_cache.Wait(description.vertex_shader, std::chrono::seconds(20));
      const CompiledShader* pixel =
          description.pixel_shader ? shader_cache.Wait(description.pixel_shader, std::chrono::seconds(20))
                                   : nullptr;
      if (!vertex || !vertex->is_valid || (description.pixel_shader && (!pixel || !pixel->is_valid))) {
        ++skipped;
        continue;
      }
      if (GetPipeline(description, *vertex, pixel)) {
        ++created;
      }
    }
    REXLOG_INFO("Plume: pipeline prewarm done in {:.1f} s ({} ready, {} without shaders)",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), created,
                skipped);
  });
}

void PipelineCache::Shutdown() {
  StopPrewarm();
  pipelines_.clear();
  shaders_.clear();
  rectangle_list_shader_.reset();
  layout_.reset();
}

// Called with mutex_ held.
plume::RenderShader* PipelineCache::GetShader(uint64_t hash, const CompiledShader& compiled) {
  std::unique_ptr<plume::RenderShader>& shader = shaders_[hash];
  if (!shader) {
    shader = renderer_.device()->createShader(compiled.binary.data(), compiled.binary.size(), "main",
                                              renderer_.shader_format());
  }
  return shader.get();
}

plume::RenderPipeline* PipelineCache::GetPipeline(const Description& d,
                                                  const CompiledShader& vertex_shader,
                                                  const CompiledShader* pixel_shader) {
  uint64_t fields[] = {
      d.vertex_shader,
      d.pixel_shader,
      uint64_t(d.primitive_type) | (uint64_t(d.color_count) << 8) | (uint64_t(d.has_depth) << 16) |
          (uint64_t(d.cull_mode) << 24) | (uint64_t(d.color_mask) << 32),
      uint64_t(d.color_formats[0]) | (uint64_t(d.color_formats[1]) << 16) |
          (uint64_t(d.color_formats[2]) << 32) | (uint64_t(d.color_formats[3]) << 48),
      uint64_t(d.blend_control[0]) | (uint64_t(d.blend_control[1]) << 32),
      uint64_t(d.blend_control[2]) | (uint64_t(d.blend_control[3]) << 32),
      uint64_t(d.depth_control) | (uint64_t(d.stencil_ref_mask) << 32),
      uint64_t(d.stencil_ref_mask_bf) | (uint64_t(d.sample_count) << 32) |
          (uint64_t(d.alpha_to_coverage) << 40),
      uint64_t(uint32_t(d.depth_bias)) | (uint64_t(std::bit_cast<uint32_t>(d.depth_bias_slope_scaled)) << 32),
  };
  uint64_t key = XXH3_64bits(fields, sizeof(fields));
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) {
      return it->second.get();
    }
    if (!creating_.count(key)) {
      break;
    }
    // Being created by another thread (the prewarm).
    created_condition_.wait(lock);
  }
  creating_.insert(key);

  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = layout_.get();
  desc.vertexShader = GetShader(d.vertex_shader, vertex_shader);
  desc.pixelShader = pixel_shader ? GetShader(d.pixel_shader, *pixel_shader) : nullptr;
  switch (d.primitive_type) {
    case xenos::PrimitiveType::kPointList:
      desc.primitiveTopology = plume::RenderPrimitiveTopology::POINT_LIST;
      break;
    case xenos::PrimitiveType::kLineList:
      desc.primitiveTopology = plume::RenderPrimitiveTopology::LINE_LIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      desc.primitiveTopology = plume::RenderPrimitiveTopology::LINE_STRIP;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
      break;
    case xenos::PrimitiveType::kRectangleList:
      desc.geometryShader = rectangle_list_shader_.get();
      desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      break;
    default:
      desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      break;
  }

  desc.multisampling.sampleCount = d.sample_count;
  desc.alphaToCoverageEnabled = d.alpha_to_coverage;
  desc.renderTargetCount = d.color_count;
  for (uint32_t i = 0; i < d.color_count; ++i) {
    desc.renderTargetFormat[i] = d.color_formats[i];
    auto blend = rex::memory::Reinterpret<reg::RB_BLENDCONTROL>(d.blend_control[i]);
    plume::RenderBlendDesc& b = desc.renderTargetBlend[i];
    b.srcBlend = ConvertBlend(blend.color_srcblend, false);
    b.dstBlend = ConvertBlend(blend.color_destblend, false);
    b.blendOp = ConvertBlendOp(blend.color_comb_fcn);
    b.srcBlendAlpha = ConvertBlend(blend.alpha_srcblend, true);
    b.dstBlendAlpha = ConvertBlend(blend.alpha_destblend, true);
    b.blendOpAlpha = ConvertBlendOp(blend.alpha_comb_fcn);
    b.blendEnabled =
        !(b.srcBlend == plume::RenderBlend::ONE && b.dstBlend == plume::RenderBlend::ZERO &&
          b.blendOp == plume::RenderBlendOperation::ADD && b.srcBlendAlpha == plume::RenderBlend::ONE &&
          b.dstBlendAlpha == plume::RenderBlend::ZERO &&
          b.blendOpAlpha == plume::RenderBlendOperation::ADD);
    b.renderTargetWriteMask = uint8_t((d.color_mask >> (i * 4)) & 0xF);
  }

  if (d.has_depth) {
    desc.depthTargetFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
    auto depth = rex::memory::Reinterpret<reg::RB_DEPTHCONTROL>(d.depth_control);
    desc.depthEnabled = depth.z_enable;
    desc.depthWriteEnabled = depth.z_enable && depth.z_write_enable;
    desc.depthFunction = ConvertCompare(depth.zfunc);
    desc.stencilEnabled = depth.stencil_enable;
    auto ref_mask = rex::memory::Reinterpret<reg::RB_STENCILREFMASK>(d.stencil_ref_mask);
    desc.stencilReadMask = ref_mask.stencilmask;
    desc.stencilWriteMask = ref_mask.stencilwritemask;
    desc.stencilReference = ref_mask.stencilref;
    desc.stencilFrontFace.compareFunction = ConvertCompare(depth.stencilfunc);
    desc.stencilFrontFace.failOp = ConvertStencilOp(depth.stencilfail);
    desc.stencilFrontFace.passOp = ConvertStencilOp(depth.stencilzpass);
    desc.stencilFrontFace.depthFailOp = ConvertStencilOp(depth.stencilzfail);
    if (depth.backface_enable) {
      desc.stencilBackFace.compareFunction = ConvertCompare(depth.stencilfunc_bf);
      desc.stencilBackFace.failOp = ConvertStencilOp(depth.stencilfail_bf);
      desc.stencilBackFace.passOp = ConvertStencilOp(depth.stencilzpass_bf);
      desc.stencilBackFace.depthFailOp = ConvertStencilOp(depth.stencilzfail_bf);
    } else {
      desc.stencilBackFace = desc.stencilFrontFace;
    }
  }

  // Bits 0-1: cull front / back, bit 2: clockwise front faces.
  desc.cullMode = (d.cull_mode & 1)   ? plume::RenderCullMode::FRONT
                  : (d.cull_mode & 2) ? plume::RenderCullMode::BACK
                                      : plume::RenderCullMode::NONE;
  desc.frontFace = (d.cull_mode & 4) ? plume::RenderFrontFace::CLOCKWISE
                                     : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  desc.depthBias = d.depth_bias;
  desc.slopeScaledDepthBias = d.depth_bias_slope_scaled;

  lock.unlock();
  PipelineCreation creation{renderer_.device(), &desc, nullptr};
  if (!CallGuarded(CreatePipeline, &creation)) {
    // Remembered as failed (null) so it is not tried again.
    REXLOG_ERROR("Plume: the graphics driver crashed creating a pipeline for shaders {:016X} / "
                 "{:016X} ({} samples, primitive {}), its draws are skipped",
                 d.vertex_shader, d.pixel_shader, d.sample_count, uint32_t(d.primitive_type));
  } else if (!creation.pipeline) {
    REXLOG_ERROR("Plume: could not create a pipeline for shaders {:016X} / {:016X}", d.vertex_shader,
                 d.pixel_shader);
  }
  plume::RenderPipeline* result = creation.pipeline.get();
  lock.lock();
  if (result && !record_path_.empty()) {
    // Recorded for the prewarm of later sessions.
    std::ofstream file(record_path_, std::ios::binary | std::ios::app);
    uint32_t header[2] = {kRecordVersion, uint32_t(sizeof(Description))};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&d), sizeof(Description));
  }
  pipelines_.emplace(key, std::move(creation.pipeline));
  creating_.erase(key);
  lock.unlock();
  created_condition_.notify_all();
  return result;
}

}  // namespace hajinmon::renderer

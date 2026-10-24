// Hajinmon native renderer - translated shader cache.

#include "shader_cache.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

#include <fmt/format.h>
#include <rex/logging/macros.h>
#include <rex/string/buffer.h>
#include <xxhash.h>

namespace hajinmon::renderer {

namespace {

// Bump when the DXC options change, as they are not part of the HLSL.
constexpr uint64_t kCompilerOptionsVersion = 2;

ShaderCompiler& ThreadCompiler() {
  // DXC compiler objects are not thread-safe.
  thread_local ShaderCompiler compiler;
  compiler.Initialize();
  return compiler;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return !data.empty();
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
  // Written to a temporary name first, so a crash never leaves a truncated
  // binary under the final name.
  std::filesystem::path temporary = path;
  temporary += fmt::format(".{}.tmp", std::hash<std::thread::id>()(std::this_thread::get_id()));
  {
    std::ofstream file(temporary, std::ios::binary);
    if (!file) {
      return;
    }
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
  }
}

}  // namespace

ShaderCache::ShaderCache(ShaderCompiler::Target target) : target_(target) {
  for (uint32_t i = 0; i < 2; ++i) {
    build_threads_.emplace_back(&ShaderCache::BuildWorker, this);
  }
}

ShaderCache::~ShaderCache() {
  StopPrewarm();
  {
    std::lock_guard<std::mutex> lock(build_mutex_);
    build_stop_ = true;
  }
  build_condition_.notify_all();
  for (std::thread& thread : build_threads_) {
    thread.join();
  }
}

void ShaderCache::RequestBuild(uint64_t hash, rex::graphics::xenos::ShaderType type,
                               const uint32_t* ucode, uint32_t dword_count) {
  {
    std::lock_guard<std::mutex> lock(build_mutex_);
    build_queue_.push_back({hash, type, std::vector<uint32_t>(ucode, ucode + dword_count)});
    ++pending_builds_;
  }
  build_condition_.notify_one();
}

const CompiledShader* ShaderCache::Find(uint64_t hash) const {
  std::lock_guard<std::mutex> lock(build_mutex_);
  auto it = built_.find(hash);
  return it != built_.end() ? it->second.get() : nullptr;
}

void ShaderCache::BuildWorker() {
  while (true) {
    BuildRequest request;
    {
      std::unique_lock<std::mutex> lock(build_mutex_);
      build_condition_.wait(lock, [this] { return build_stop_ || !build_queue_.empty(); });
      if (build_stop_) {
        return;
      }
      request = std::move(build_queue_.front());
      build_queue_.pop_front();
    }
    rex::graphics::Shader shader(request.type, request.hash, request.ucode.data(),
                                 request.ucode.size());
    auto compiled = std::make_unique<CompiledShader>();
    Build(shader, *compiled);
    {
      std::lock_guard<std::mutex> lock(build_mutex_);
      built_[request.hash] = std::move(compiled);
    }
    --pending_builds_;
  }
}

void ShaderCache::SetStorageDirectory(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  directory_ = error ? std::filesystem::path() : directory;
}

bool ShaderCache::Build(rex::graphics::Shader& shader, CompiledShader& out) {
  out = CompiledShader();
  if (!shader.is_ucode_analyzed()) {
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
  }
  HlslShaderTranslator translator;
  rex::graphics::Shader::Translation& translation = *shader.GetOrCreateTranslation(0);
  if (!translator.TranslateAnalyzedShader(translation)) {
    ++stats_.failed;
    return false;
  }
  out.texture_slots = translator.texture_slots();
  const std::vector<uint8_t>& hlsl = translation.translated_binary();
  bool is_vertex_shader = shader.type() == rex::graphics::xenos::ShaderType::kVertex;

  uint64_t key = XXH3_64bits_withSeed(hlsl.data(), hlsl.size(),
                                      kCompilerOptionsVersion * 2 + uint64_t(target_));
  std::filesystem::path path;
  if (!directory_.empty()) {
    path = directory_ / fmt::format("{:016X}.{}", key, target_ == ShaderCompiler::Target::kSpirv
                                                            ? "spv"
                                                            : "dxil");
    if (ReadFile(path, out.binary)) {
      ++stats_.loaded;
      out.is_valid = true;
      return true;
    }
  }

  std::string errors;
  ShaderCompiler& compiler = ThreadCompiler();
  if (!compiler.initialized() ||
      !compiler.Compile(std::string(hlsl.begin(), hlsl.end()), is_vertex_shader ? ShaderCompiler::Stage::kVertex : ShaderCompiler::Stage::kPixel, target_,
                        out.binary, errors)) {
    REXLOG_ERROR("Plume: {} shader {:016X} failed to compile: {}",
                 is_vertex_shader ? "vertex" : "pixel", shader.ucode_data_hash(),
                 compiler.initialized() ? errors : "dxcompiler not found");
    ++stats_.failed;
    return false;
  }
  ++stats_.compiled;
  if (!path.empty()) {
    WriteFile(path, out.binary);
  }
  out.is_valid = true;
  return true;
}

void ShaderCache::StartPrewarm(std::vector<PrewarmShader> shaders) {
  StopPrewarm();
  prewarm_shaders_ = std::move(shaders);
  prewarm_next_ = 0;
  prewarm_stop_ = false;
  prewarm_skipped_ = 0;
  // Leave most cores to the game, which is starting at the same time.
  uint32_t threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
  prewarm_remaining_workers_ = threads;
  for (uint32_t i = 0; i < threads; ++i) {
    prewarm_threads_.emplace_back(&ShaderCache::PrewarmWorker, this);
  }
}

void ShaderCache::StopPrewarm() {
  prewarm_stop_ = true;
  for (std::thread& thread : prewarm_threads_) {
    thread.join();
  }
  prewarm_threads_.clear();
}

void ShaderCache::PrewarmWorker() {
  auto start = std::chrono::steady_clock::now();
  while (!prewarm_stop_) {
    size_t index = prewarm_next_++;
    if (index >= prewarm_shaders_.size()) {
      break;
    }
    const PrewarmShader& entry = prewarm_shaders_[index];
    rex::graphics::Shader shader(entry.is_vertex_shader ? rex::graphics::xenos::ShaderType::kVertex
                                                        : rex::graphics::xenos::ShaderType::kPixel,
                                 XXH3_64bits(entry.ucode, entry.dword_count * sizeof(uint32_t)),
                                 entry.ucode, entry.dword_count);
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    if (!shader.vertex_bindings().empty()) {
      // Patched by the game before use.
      ++prewarm_skipped_;
      continue;
    }
    CompiledShader compiled;
    Build(shader, compiled);
  }
  if (--prewarm_remaining_workers_ == 0 && !prewarm_stop_) {
    double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REXLOG_INFO(
        "Plume: shader prewarm done in {:.1f} s ({} compiled, {} cached, {} failed, {} left for "
        "run time)",
        seconds, stats_.compiled.load(), stats_.loaded.load(), stats_.failed.load(),
        prewarm_skipped_.load());
  }
}

}  // namespace hajinmon::renderer

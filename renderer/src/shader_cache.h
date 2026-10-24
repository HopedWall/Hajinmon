// Hajinmon native renderer - translated shader cache.
//
// Shaders are translated to HLSL whenever they are needed (it is fast), and the
// compiled DXIL/SPIR-V is stored on disk, keyed by a hash of the HLSL, so a
// change of the translator never reuses stale binaries. DXC is the slow part
// (~150 ms per shader), so it always runs on worker threads: at startup for the
// shaders of the executable, and in the background for the shaders the game
// loads that are not on disk yet (they are skipped until ready).
//
// Vertex shaders that fetch vertices are not compiled ahead of time: the game's
// D3D runtime patches their vfetch instructions (buffer, format, stride) when a
// vertex declaration is bound, so only the patched microcode the game loads is
// meaningful.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>

#include "hlsl_translator.h"
#include "shader_compiler.h"

namespace hajinmon::renderer {

struct CompiledShader {
  bool is_valid = false;
  std::vector<uint8_t> binary;
  std::vector<HlslShaderTranslator::TextureSlot> texture_slots;
};

class ShaderCache {
 public:
  struct Stats {
    std::atomic<uint32_t> compiled{0};
    std::atomic<uint32_t> loaded{0};
    std::atomic<uint32_t> failed{0};
  };

  explicit ShaderCache(ShaderCompiler::Target target);
  ~ShaderCache();

  void SetStorageDirectory(const std::filesystem::path& directory);

  // Translates the shader and compiles it, or loads the binary from disk.
  // Thread-safe, as long as each thread uses its own Shader objects.
  bool Build(rex::graphics::Shader& shader, CompiledShader& out);

  // Compiles the given shaders (big-endian microcode) to the disk cache on
  // background threads.
  struct PrewarmShader {
    std::vector<uint32_t> ucode;
    bool is_vertex_shader;
    // From the executable: vertex shaders that fetch vertices are skipped, as
    // the game patches them before use.
    bool from_executable;
  };
  // `threads` 0: half of the cores, leaving the others to the game.
  void StartPrewarm(std::vector<PrewarmShader> shaders, uint32_t threads = 0);
  void StopPrewarm();
  // Waits for the prewarm started by StartPrewarm to finish.
  void WaitForPrewarm();

  // The shaders the game loaded in previous sessions (recorded in the storage
  // directory), to prewarm: after a translator change they are all compiled
  // at startup instead of when first drawn.
  std::vector<PrewarmShader> LoadRecordedShaders();

  // Builds a shader the game loaded on worker threads (several shaders build in
  // parallel). `ucode` is the guest microcode (big-endian).
  void RequestBuild(uint64_t hash, rex::graphics::xenos::ShaderType type, const uint32_t* ucode,
                    uint32_t dword_count);
  // The result of RequestBuild, or null while it is still being built.
  const CompiledShader* Find(uint64_t hash) const;
  // Like Find, but waits up to `timeout` for a requested build to finish.
  const CompiledShader* Wait(uint64_t hash, std::chrono::milliseconds timeout);
  uint32_t pending_builds() const { return pending_builds_; }

  const Stats& stats() const { return stats_; }

 private:
  void PrewarmWorker();
  void BuildWorker();

  struct BuildRequest {
    uint64_t hash;
    rex::graphics::xenos::ShaderType type;
    std::vector<uint32_t> ucode;
  };
  mutable std::mutex build_mutex_;
  std::condition_variable build_condition_;
  std::condition_variable built_condition_;
  std::deque<BuildRequest> build_queue_;
  std::unordered_map<uint64_t, std::unique_ptr<CompiledShader>> built_;
  std::atomic<uint32_t> pending_builds_{0};
  bool build_stop_ = false;
  std::vector<std::thread> build_threads_;

  ShaderCompiler::Target target_;
  std::filesystem::path directory_;
  Stats stats_;

  std::vector<PrewarmShader> prewarm_shaders_;
  std::filesystem::path recorded_path_;
  std::mutex recorded_mutex_;
  std::unordered_set<uint64_t> recorded_hashes_;
  std::atomic<size_t> prewarm_next_{0};
  std::atomic<uint32_t> prewarm_remaining_workers_{0};
  std::atomic<bool> prewarm_stop_{false};
  std::atomic<uint32_t> prewarm_skipped_{0};
  std::vector<std::thread> prewarm_threads_;
};

}  // namespace hajinmon::renderer

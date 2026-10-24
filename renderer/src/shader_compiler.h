// Hajinmon native renderer - HLSL compilation with DXC.
//
// dxcompiler (and dxil, which signs DXIL) are loaded at run time from the
// executable's directory, so the game starts without them when the native
// renderer is not used.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hajinmon::renderer {

class ShaderCompiler {
 public:
  enum class Target {
    kDxil,   // Direct3D 12.
    kSpirv,  // Vulkan.
  };

  enum class Stage {
    kVertex,
    kPixel,
    kGeometry,
  };

  ShaderCompiler();
  ~ShaderCompiler();
  ShaderCompiler(const ShaderCompiler&) = delete;
  ShaderCompiler& operator=(const ShaderCompiler&) = delete;

  bool Initialize();
  bool initialized() const { return compiler_ != nullptr; }

  // Compiles `hlsl` (entry point `main`). On failure, `errors` holds the
  // compiler messages.
  bool Compile(const std::string& hlsl, Stage stage, Target target,
               std::vector<uint8_t>& binary, std::string& errors);

 private:
  void* library_ = nullptr;
  // IDxcUtils and IDxcCompiler3.
  void* utils_ = nullptr;
  void* compiler_ = nullptr;
};

}  // namespace hajinmon::renderer

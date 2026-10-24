// Hajinmon native renderer - offline shader translation check.
//
// Translates every shader of the game executable to HLSL and compiles it with
// DXC for both Direct3D 12 and Vulkan, reporting the shaders that fail. Used to
// develop the translator without starting the game.
//
// Usage: hajinmon_shader_tool <decrypted executable image> [output directory]
// The image is the XEX's basefile, loaded at 0x82000000. Failing shaders (or
// all of them with --all) are written to the output directory as .hlsl files
// with the microcode disassembly and the compiler messages.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

#include "hlsl_translator.h"
#include "shader_catalog.h"
#include "shader_compiler.h"

using namespace hajinmon::renderer;
namespace graphics = rex::graphics;

int main(int argc, char** argv) {
  std::vector<std::string> args;
  bool dump_all = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--all")) {
      dump_all = true;
    } else {
      args.push_back(argv[i]);
    }
  }
  if (args.empty()) {
    std::fprintf(stderr, "Usage: hajinmon_shader_tool [--all] <executable image> [output dir]\n");
    return 1;
  }
  std::ifstream file(args[0], std::ios::binary);
  if (!file) {
    std::fprintf(stderr, "Cannot open %s\n", args[0].c_str());
    return 1;
  }
  std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::filesystem::path out_dir = args.size() > 1 ? args[1] : "shader_dump";
  std::filesystem::create_directories(out_dir);

  constexpr uint32_t kImageBase = 0x82000000;
  ShaderCatalog catalog;
  catalog.Build(image.data(), kImageBase, uint32_t(image.size()));

  ShaderCompiler compiler;
  if (!compiler.Initialize()) {
    std::fprintf(stderr, "Cannot load dxcompiler\n");
    return 1;
  }

  uint32_t translated = 0, translation_errors = 0, dxil_ok = 0, spirv_ok = 0, memexport = 0;
  for (const CatalogShader& entry : catalog.shaders()) {
    const auto* ucode =
        reinterpret_cast<const uint32_t*>(image.data() + (entry.ucode_address - kImageBase));
    graphics::Shader shader(
        entry.is_vertex_shader ? graphics::xenos::ShaderType::kVertex : graphics::xenos::ShaderType::kPixel,
        entry.ucode_address, ucode, entry.ucode_dword_count);
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    HlslShaderTranslator translator;
    graphics::Shader::Translation& translation = *shader.GetOrCreateTranslation(0);
    bool valid = translator.TranslateAnalyzedShader(translation);
    ++translated;
    translation_errors += !valid;
    memexport += translator.skipped_memexports() != 0;
    std::string hlsl(translation.translated_binary().begin(), translation.translated_binary().end());

    std::string messages;
    for (const auto& error : translation.errors()) {
      messages += fmt::format("translation {}: {}\n", error.is_fatal ? "error" : "warning", error.message);
    }
    auto stage = entry.is_vertex_shader ? ShaderCompiler::Stage::kVertex : ShaderCompiler::Stage::kPixel;
    std::vector<uint8_t> binary;
    std::string errors;
    bool dxil = compiler.Compile(hlsl, stage, ShaderCompiler::Target::kDxil, binary, errors);
    if (!errors.empty()) {
      messages += "DXIL:\n" + errors;
    }
    bool spirv =
        compiler.Compile(hlsl, stage, ShaderCompiler::Target::kSpirv, binary, errors);
    if (!errors.empty()) {
      messages += "SPIR-V:\n" + errors;
    }
    dxil_ok += dxil;
    spirv_ok += spirv;

    if (dump_all || !valid || !dxil || !spirv) {
      std::string name = fmt::format("{}_{:08X}", entry.is_vertex_shader ? "vs" : "ps", entry.ucode_address);
      std::ofstream out(out_dir / (name + ".hlsl"), std::ios::binary);
      out << "/*\n" << disassembly.to_string_view() << "\n" << messages << "*/\n" << hlsl;
      if (!valid || !dxil || !spirv) {
        std::printf("%s: %s%s%s\n", name.c_str(), valid ? "" : "translation ", dxil ? "" : "dxil ",
                    spirv ? "" : "spirv");
      }
    }
  }
  std::printf("%u shaders: %u translation errors, %u DXIL ok, %u SPIR-V ok, %u with memexport\n",
              translated, translation_errors, dxil_ok, spirv_ok, memexport);
  return (translation_errors || dxil_ok != translated || spirv_ok != translated) ? 2 : 0;
}

// Hajinmon native renderer - HLSL compilation with DXC.

#include "shader_compiler.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#else
#include <dlfcn.h>
#endif

#include <dxcapi.h>

namespace hajinmon::renderer {

namespace {

IDxcUtils* Utils(void* p) { return static_cast<IDxcUtils*>(p); }
IDxcCompiler3* Compiler(void* p) { return static_cast<IDxcCompiler3*>(p); }

}  // namespace

ShaderCompiler::ShaderCompiler() = default;

ShaderCompiler::~ShaderCompiler() {
  if (compiler_) {
    Compiler(compiler_)->Release();
  }
  if (utils_) {
    Utils(utils_)->Release();
  }
  // The library stays loaded: DXC does not support being unloaded cleanly.
}

bool ShaderCompiler::Initialize() {
  if (compiler_) {
    return true;
  }
#ifdef _WIN32
  HMODULE library = LoadLibraryW(L"dxcompiler.dll");
  auto create = library ? reinterpret_cast<DxcCreateInstanceProc>(
                              GetProcAddress(library, "DxcCreateInstance"))
                        : nullptr;
#else
  void* library = dlopen("libdxcompiler.so", RTLD_NOW | RTLD_LOCAL);
  auto create = library ? reinterpret_cast<DxcCreateInstanceProc>(dlsym(library, "DxcCreateInstance"))
                        : nullptr;
#endif
  if (!create) {
    return false;
  }
  library_ = library;
  IDxcUtils* utils = nullptr;
  IDxcCompiler3* compiler = nullptr;
  if (FAILED(create(CLSID_DxcUtils, __uuidof(IDxcUtils), reinterpret_cast<void**>(&utils))) ||
      FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3),
                    reinterpret_cast<void**>(&compiler)))) {
    if (utils) {
      utils->Release();
    }
    return false;
  }
  utils_ = utils;
  compiler_ = compiler;
  return true;
}

bool ShaderCompiler::Compile(const std::string& hlsl, bool is_vertex_shader, Target target,
                             std::vector<uint8_t>& binary, std::string& errors) {
  binary.clear();
  errors.clear();
  if (!compiler_) {
    errors = "DXC is not loaded";
    return false;
  }
  std::vector<LPCWSTR> arguments = {
      L"-E", L"main", L"-T", is_vertex_shader ? L"vs_6_0" : L"ps_6_0",
      // HLSL 2018: vector ?: and ||, as written by the translator.
      L"-HV", L"2018", L"-O3",
  };
  if (target == Target::kSpirv) {
    arguments.insert(arguments.end(), {
        L"-spirv", L"-fspv-target-env=vulkan1.1",
        // Separate binding ranges per register type within each space.
        L"-fvk-t-shift", L"16", L"0", L"-fvk-t-shift", L"16", L"1", L"-fvk-t-shift", L"16", L"2",
        L"-fvk-s-shift", L"48", L"0", L"-fvk-s-shift", L"48", L"1", L"-fvk-s-shift", L"48", L"2",
    });
    // Direct3D conventions for the position.
    arguments.push_back(is_vertex_shader ? L"-fvk-invert-y" : L"-fvk-use-dx-position-w");
  }

  DxcBuffer source = {};
  source.Ptr = hlsl.data();
  source.Size = hlsl.size();
  source.Encoding = DXC_CP_UTF8;
  IDxcResult* result = nullptr;
  if (FAILED(Compiler(compiler_)->Compile(&source, arguments.data(), UINT32(arguments.size()),
                                          nullptr, __uuidof(IDxcResult),
                                          reinterpret_cast<void**>(&result)))) {
    errors = "DXC Compile call failed";
    return false;
  }
  IDxcBlobUtf8* messages = nullptr;
  if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8),
                                  reinterpret_cast<void**>(&messages), nullptr)) &&
      messages) {
    if (messages->GetStringLength()) {
      errors.assign(messages->GetStringPointer(), messages->GetStringLength());
    }
    messages->Release();
  }
  HRESULT status = E_FAIL;
  result->GetStatus(&status);
  bool ok = SUCCEEDED(status);
  if (ok) {
    IDxcBlob* object = nullptr;
    if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob),
                                    reinterpret_cast<void**>(&object), nullptr)) &&
        object) {
      const auto* data = static_cast<const uint8_t*>(object->GetBufferPointer());
      binary.assign(data, data + object->GetBufferSize());
      object->Release();
    }
    ok = !binary.empty();
  }
  result->Release();
  return ok;
}

}  // namespace hajinmon::renderer

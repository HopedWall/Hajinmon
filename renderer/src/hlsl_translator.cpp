// Hajinmon native renderer - Xenos microcode to HLSL translator.
//
// Instruction semantics follow the SDK's shader interpreter and SPIR-V
// translator: Direct3D 9 multiplication (0 * anything = +0), max/min as
// `a >= b ? a : b` / `a < b ? a : b`, a single predicate register, and the
// "previous scalar" register shared by consecutive scalar operations.

#include "hlsl_translator.h"

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

namespace hajinmon::renderer {

using namespace rex::graphics;

namespace {

constexpr uint32_t kInterpolatorCount = 16;

const char kHlslPrologue[] = R"(// Translated from Xenos microcode by Hajinmon.
#define XE_FLT_MAX 3.402823466e+38f

cbuffer XeSystemConstants : register(b0, space0) {
  // Bit 0: position XY is divided by W, bit 1: Z is divided by W, bit 2: W is
  // 1/W (PA_CL_VTE_CNTL).
  uint xe_flags;
  int xe_vertex_index_offset;
  // Interpolator receiving the pixel parameters, plus 1 (0 - none).
  uint xe_ps_param_gen;
  // Alpha test function (0 - never ... 7 - always).
  uint xe_alpha_test;
  float4 xe_ndc_scale;
  float4 xe_ndc_offset;
  float xe_alpha_ref;
  float3 xe_system_padding;
};
cbuffer XeFloatConstants : register(b1, space0) { float4 xe_c[256]; };
// Dwords 0-7: bool constants, 8-39: loop constants.
cbuffer XeBoolLoopConstants : register(b2, space0) { uint4 xe_bool_loop[10]; };
cbuffer XeFetchConstants : register(b3, space0) { uint4 xe_fetch[48]; };
ByteAddressBuffer xe_shared_memory : register(t0, space0);

float xe_mul(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
float4 xe_mul(float4 a, float4 b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
float xe_max(float a, float b) { return a >= b ? a : b; }
float4 xe_max(float4 a, float4 b) { return a >= b ? a : b; }
float xe_min(float a, float b) { return a < b ? a : b; }
float4 xe_min(float4 a, float4 b) { return a < b ? a : b; }
float xe_dp(float4 a, float4 b, uint count) {
  float4 m = xe_mul(a, b);
  float r = m.x + m.y;
  if (count > 2u) r += m.z;
  if (count > 3u) r += m.w;
  return r;
}
float4 xe_cube(float4 s) {
  // The operand is .zzxy: X, Y and Z of the direction are in s.z, s.w, s.x.
  float x = s.z, y = s.w, z = s.x;
  float4 r;
  if (abs(z) >= abs(x) && abs(z) >= abs(y)) {
    r = float4(-y, z < 0.0f ? -x : x, z, z < 0.0f ? 5.0f : 4.0f);
  } else if (abs(y) >= abs(x)) {
    r = float4(y < 0.0f ? -z : z, x, y, y < 0.0f ? 3.0f : 2.0f);
  } else {
    r = float4(-y, x < 0.0f ? z : -z, x, x < 0.0f ? 1.0f : 0.0f);
  }
  r.z *= 2.0f;
  return r;
}
float xe_max4(float4 s) {
  if (s.x >= s.y && s.x >= s.z && s.x >= s.w) return s.x;
  if (s.y >= s.z && s.y >= s.w) return s.y;
  return s.z >= s.w ? s.z : s.w;
}
int xe_address(float v, bool round) {
  return int(floor(clamp(v, -256.0f, 255.0f) + (round ? 0.5f : 0.0f)));
}
uint xe_bool(uint index) {
  uint d = index >> 5u;
  return (xe_bool_loop[d >> 2u][d & 3u] >> (index & 31u)) & 1u;
}
uint xe_loop_constant(uint index) {
  uint d = 8u + index;
  return xe_bool_loop[d >> 2u][d & 3u];
}
uint xe_fetch_dword(uint index) { return xe_fetch[index >> 2u][index & 3u]; }
uint xe_swap(uint v, uint endian) {
  if (endian == 1u || endian == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
  if (endian == 2u || endian == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
// A dword of a vertex buffer, 0 outside of it.
uint xe_vfetch_dword(uint fetch_constant, int dword_offset) {
  uint fc0 = xe_fetch_dword(fetch_constant * 2u), fc1 = xe_fetch_dword(fetch_constant * 2u + 1u);
  uint base = fc0 >> 2u, size = (fc1 >> 2u) & 0xFFFFFFu;
  uint address = uint(int(base) + dword_offset);
  if (address - base >= size) return 0u;
  return xe_swap(xe_shared_memory.Load(address << 2u), fc1 & 3u);
}
float xe_unpack_unsigned(uint v, uint offset, uint width, bool normalize) {
  uint mask = (1u << width) - 1u;
  float x = float((v >> offset) & mask);
  return normalize ? x / float(mask) : x;
}
float xe_unpack_signed(uint v, uint offset, uint width, bool normalize, bool no_zero) {
  float x = float(int(v << (32u - offset - width)) >> (32u - width));
  if (!normalize) return x;
  if (no_zero) return (x + 0.5f) * 2.0f / float((1u << width) - 1u);
  return max(-1.0f, x / float((1u << (width - 1u)) - 1u));
}
// Cube map fetch coordinates (SC + 1, TC + 1, face) to a direction.
float3 xe_cube_direction(float3 c) {
  float2 st = c.xy * 2.0f - 3.0f;
  uint face = min(uint(max(c.z, 0.0f)), 5u);
  float sign = (face & 1u) != 0u ? -1.0f : 1.0f;
  uint axis = face >> 1u;
  if (axis == 0u) return float3(sign, -st.y, -sign * st.x);
  if (axis == 1u) return float3(st.x, sign, sign * st.y);
  return float3(sign * st.x, -st.y, sign);
}

static int xe_a0;
static int xe_aL;
static bool xe_p0;
static float xe_ps;
static uint xe_loop_depth;
static uint xe_loop_iteration[4];
static uint xe_loop_count[4];
static int xe_loop_saved_aL[4];
static uint xe_call_depth;
static uint xe_call_stack[4];
static float xe_lod;
static float3 xe_gradient_h;
static float3 xe_gradient_v;
)";

std::string FloatLiteral(float value) {
  std::string text = fmt::format("{:.9g}", value);
  if (text.find_first_of(".einf") == std::string::npos) {
    text += ".0";
  }
  return text + "f";
}

const char kComponents[] = "xyzw";

}  // namespace

void HlslShaderTranslator::Reset() {
  ShaderTranslator::Reset();
  body_.clear();
  indent_ = 0;
  labels_.clear();
  uses_state_machine_ = false;
  texture_slots_.clear();
  skipped_memexports_ = 0;
  exec_condition_open_ = false;
}

void HlslShaderTranslator::Line(const std::string& text) {
  body_.append(size_t(indent_) * 2, ' ');
  body_ += text;
  body_ += '\n';
}

void HlslShaderTranslator::PreProcessControlFlowInstructions(
    std::vector<ucode::ControlFlowInstruction> instrs) {
  labels_ = current_shader().label_addresses();
  for (uint32_t i = 0; i < uint32_t(instrs.size()); ++i) {
    if (instrs[i].opcode() == ucode::ControlFlowOpcode::kCondCall) {
      labels_.insert(i + 1);
    }
  }
  uses_state_machine_ = !labels_.empty();
}

void HlslShaderTranslator::StartTranslation() {
  // The state machine header, if needed, is added in CompleteTranslation, as
  // PreProcessControlFlowInstructions runs after this.
  indent_ = 1;
}

void HlslShaderTranslator::ProcessControlFlowInstructionBegin(uint32_t cf_index) {
  if (!uses_state_machine_) {
    return;
  }
  if (cf_index == 0) {
    Line("case 0u: {");
    Indent();
    return;
  }
  if (labels_.count(cf_index)) {
    // Falling through from the previous block.
    Line(fmt::format("xe_pc = {}u;", cf_index));
    Line("continue;");
    Unindent();
    Line("}");
    Line(fmt::format("case {}u: {{", cf_index));
    Indent();
  }
}

std::string HlslShaderTranslator::BoolConstantCondition(uint32_t index, bool condition) {
  return fmt::format("xe_bool({}u) {} 0u", index, condition ? "!=" : "==");
}

std::string HlslShaderTranslator::PredicateCondition(bool condition) {
  return condition ? "xe_p0" : "!xe_p0";
}

bool HlslShaderTranslator::BeginCondition(bool is_unconditional, bool is_predicated,
                                          uint32_t bool_index, bool condition) {
  if (is_unconditional) {
    return false;
  }
  Line(fmt::format("if ({}) {{", is_predicated ? PredicateCondition(condition)
                                               : BoolConstantCondition(bool_index, condition)));
  Indent();
  return true;
}

void HlslShaderTranslator::EmitJump(uint32_t target) {
  Line(fmt::format("xe_pc = {}u;", target));
  Line("continue;");
}

void HlslShaderTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& instr) {
  Line(fmt::format("// {} {} instructions at {}", instr.opcode_name, instr.instruction_count,
                   instr.instruction_address));
  exec_condition_open_ =
      BeginCondition(instr.type == ParsedExecInstruction::Type::kUnconditional,
                     instr.type == ParsedExecInstruction::Type::kPredicated,
                     instr.bool_constant_index, instr.condition);
}

void HlslShaderTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& instr) {
  if (instr.is_end) {
    Line("return;");
  }
  if (exec_condition_open_) {
    Unindent();
    Line("}");
    exec_condition_open_ = false;
  }
}

void HlslShaderTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) {
  Line(fmt::format("{{  // loop_start{}", instr.is_repeat ? " (repeat)" : ""));
  Indent();
  Line("uint xe_d = min(xe_loop_depth, 3u);");
  Line(fmt::format("uint xe_lc = xe_loop_constant({}u);", instr.loop_constant_index));
  if (!instr.is_repeat) {
    Line("xe_loop_iteration[xe_d] = 0u;");
  }
  Line("xe_loop_count[xe_d] = xe_lc & 0xFFu;");
  Line("xe_loop_saved_aL[xe_d] = xe_aL;");
  Line(
      "xe_aL = int((xe_lc >> 8u) & 0xFFu) + int(xe_loop_iteration[xe_d]) * (int(xe_lc << 8u) >> "
      "24);");
  Line("if (xe_loop_iteration[xe_d] >= xe_loop_count[xe_d]) {");
  Indent();
  Line("xe_aL = xe_loop_saved_aL[xe_d];");
  EmitJump(instr.loop_skip_address);
  Unindent();
  Line("}");
  Line("xe_loop_depth = xe_d + 1u;");
  Unindent();
  Line("}");
}

void HlslShaderTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) {
  Line("{  // loop_end");
  Indent();
  Line("uint xe_d = max(xe_loop_depth, 1u) - 1u;");
  Line(fmt::format("uint xe_lc = xe_loop_constant({}u);", instr.loop_constant_index));
  Line("++xe_loop_iteration[xe_d];");
  Line("xe_aL += int(xe_lc << 8u) >> 24;");
  std::string condition = "xe_loop_iteration[xe_d] < xe_loop_count[xe_d]";
  if (instr.is_predicated_break) {
    condition += fmt::format(" && !({})", PredicateCondition(instr.predicate_condition));
  }
  Line(fmt::format("if ({}) {{", condition));
  Indent();
  EmitJump(instr.loop_body_address);
  Unindent();
  Line("}");
  Line("xe_aL = xe_loop_saved_aL[xe_d];");
  Line("xe_loop_depth = xe_d;");
  Unindent();
  Line("}");
}

void HlslShaderTranslator::ProcessCallInstruction(const ParsedCallInstruction& instr) {
  Line("// call");
  bool open = BeginCondition(instr.type == ParsedCallInstruction::Type::kUnconditional,
                             instr.type == ParsedCallInstruction::Type::kPredicated,
                             instr.bool_constant_index, instr.condition);
  Line("if (xe_call_depth < 4u) {");
  Indent();
  Line(fmt::format("xe_call_stack[xe_call_depth++] = {}u;", instr.dword_index + 1));
  EmitJump(instr.target_address);
  Unindent();
  Line("}");
  if (open) {
    Unindent();
    Line("}");
  }
}

void HlslShaderTranslator::ProcessReturnInstruction(const ParsedReturnInstruction& instr) {
  (void)instr;
  if (!uses_state_machine_) {
    // Without calls, a return does nothing.
    return;
  }
  Line("if (xe_call_depth != 0u) {  // return");
  Indent();
  Line("xe_pc = xe_call_stack[--xe_call_depth];");
  Line("continue;");
  Unindent();
  Line("}");
}

void HlslShaderTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& instr) {
  Line("// jump");
  bool open = BeginCondition(instr.type == ParsedJumpInstruction::Type::kUnconditional,
                             instr.type == ParsedJumpInstruction::Type::kPredicated,
                             instr.bool_constant_index, instr.condition);
  EmitJump(instr.target_address);
  if (open) {
    Unindent();
    Line("}");
  }
}

std::string HlslShaderTranslator::RegisterExpression(uint32_t index,
                                                     InstructionStorageAddressingMode mode) const {
  uint32_t count = std::max(register_count(), uint32_t(1));
  switch (mode) {
    case InstructionStorageAddressingMode::kAddressRegisterRelative:
      return fmt::format("r[clamp({} + xe_a0, 0, {})]", index, count - 1);
    case InstructionStorageAddressingMode::kLoopRelative:
      return fmt::format("r[clamp({} + xe_aL, 0, {})]", index, count - 1);
    default:
      return fmt::format("r[{}]", std::min(index, count - 1));
  }
}

std::string HlslShaderTranslator::OperandExpression(const InstructionOperand& operand,
                                                    uint32_t component_count) const {
  std::string value;
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      value = RegisterExpression(operand.storage_index, operand.storage_addressing_mode);
      break;
    case InstructionStorageSource::kConstantFloat:
      switch (operand.storage_addressing_mode) {
        case InstructionStorageAddressingMode::kAddressRegisterRelative:
          value = fmt::format("xe_c[clamp({} + xe_a0, 0, 255)]", operand.storage_index);
          break;
        case InstructionStorageAddressingMode::kLoopRelative:
          value = fmt::format("xe_c[clamp({} + xe_aL, 0, 255)]", operand.storage_index);
          break;
        default:
          value = fmt::format("xe_c[{}]", operand.storage_index & 255);
          break;
      }
      break;
    default:
      value = "float4(0.0f, 0.0f, 0.0f, 0.0f)";
      break;
  }
  value += '.';
  for (uint32_t i = 0; i < component_count; ++i) {
    SwizzleSource component = operand.GetComponent(i);
    value += component <= SwizzleSource::kW ? kComponents[uint32_t(component)] : 'x';
  }
  if (operand.is_absolute_value) {
    value = "abs(" + value + ")";
  }
  if (operand.is_negated) {
    value = "-" + value;
  }
  return value;
}

std::string HlslShaderTranslator::OperandComponent(const InstructionOperand& operand,
                                                   uint32_t component) const {
  InstructionOperand single = operand;
  single.components[0] = operand.GetComponent(component);
  single.component_count = 1;
  return OperandExpression(single, 1);
}

void HlslShaderTranslator::StoreResult(const InstructionResult& result, const std::string& value) {
  uint32_t write_mask = result.GetUsedWriteMask();
  if (!write_mask) {
    return;
  }
  std::string dest;
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      dest = RegisterExpression(result.storage_index, result.storage_addressing_mode);
      break;
    case InstructionStorageTarget::kInterpolator:
      if (result.storage_index >= kInterpolatorCount) {
        return;
      }
      dest = fmt::format("xe_o_interpolators[{}]", result.storage_index);
      break;
    case InstructionStorageTarget::kPosition:
      dest = "xe_o_position";
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      dest = "xe_o_point_size_edge_kill";
      break;
    case InstructionStorageTarget::kColor:
      dest = fmt::format("xe_o_color[{}]", result.storage_index & 3);
      break;
    case InstructionStorageTarget::kDepth:
      dest = "xe_o_depth";
      break;
    case InstructionStorageTarget::kExportAddress:
    case InstructionStorageTarget::kExportData:
      // Memory export (eA/eM#) is not supported yet.
      ++skipped_memexports_;
      return;
    default:
      return;
  }
  Line(fmt::format("xe_r = {};", value));
  if (result.is_clamped) {
    Line("xe_r = saturate(xe_r);");
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(write_mask & (1u << i))) {
      continue;
    }
    SwizzleSource component = result.components[i];
    std::string source = component == SwizzleSource::k0   ? "0.0f"
                         : component == SwizzleSource::k1 ? "1.0f"
                                                          : fmt::format("xe_r.{}", kComponents[uint32_t(component)]);
    Line(fmt::format("{}.{} = {};", dest, kComponents[i], source));
  }
}

void HlslShaderTranslator::ProcessAluInstruction(const ParsedAluInstruction& instr,
                                                 uint8_t memexport_eM_potentially_written_before) {
  (void)memexport_eM_potentially_written_before;
  if (instr.IsNop()) {
    return;
  }
  bool vector_used = !instr.IsVectorOpDefaultNop();
  bool scalar_used = !instr.IsScalarOpDefaultNop();
  Line(fmt::format("{{  // {} / {}", vector_used ? instr.vector_opcode_name : "nop",
                   scalar_used ? instr.scalar_opcode_name : "nop"));
  Indent();
  if (instr.is_predicated) {
    Line(fmt::format("if ({}) {{", PredicateCondition(instr.predicate_condition)));
    Indent();
  }
  Line("float4 xe_r;");

  // Load all operands before anything is written.
  for (uint32_t i = 0; vector_used && i < instr.vector_operand_count; ++i) {
    Line(fmt::format("float4 xe_s{} = {};", i, OperandExpression(instr.vector_operands[i], 4)));
  }
  if (scalar_used && instr.scalar_operand_count) {
    const InstructionOperand& a = instr.scalar_operands[0];
    Line(fmt::format("float xe_sa = {};", OperandComponent(a, 0)));
    if (instr.scalar_operand_count >= 2) {
      Line(fmt::format("float xe_sb = {};", OperandComponent(instr.scalar_operands[1], 0)));
    } else if (a.component_count >= 2) {
      Line(fmt::format("float xe_sb = {};", OperandComponent(a, 1)));
    } else {
      Line("float xe_sb = xe_sa;");
    }
  }

  bool kills = is_pixel_shader();
  bool vector_sets_a0 = false, vector_sets_p0 = false;
  if (vector_used) {
    std::string v;
    switch (instr.vector_opcode) {
      case ucode::AluVectorOpcode::kAdd:
        v = "xe_s0 + xe_s1";
        break;
      case ucode::AluVectorOpcode::kMul:
        v = "xe_mul(xe_s0, xe_s1)";
        break;
      case ucode::AluVectorOpcode::kMax:
        v = "xe_max(xe_s0, xe_s1)";
        break;
      case ucode::AluVectorOpcode::kMin:
        v = "xe_min(xe_s0, xe_s1)";
        break;
      case ucode::AluVectorOpcode::kSeq:
        v = "float4(xe_s0 == xe_s1)";
        break;
      case ucode::AluVectorOpcode::kSgt:
        v = "float4(xe_s0 > xe_s1)";
        break;
      case ucode::AluVectorOpcode::kSge:
        v = "float4(xe_s0 >= xe_s1)";
        break;
      case ucode::AluVectorOpcode::kSne:
        v = "float4(xe_s0 != xe_s1)";
        break;
      case ucode::AluVectorOpcode::kFrc:
        v = "xe_s0 - floor(xe_s0)";
        break;
      case ucode::AluVectorOpcode::kTrunc:
        v = "trunc(xe_s0)";
        break;
      case ucode::AluVectorOpcode::kFloor:
        v = "floor(xe_s0)";
        break;
      case ucode::AluVectorOpcode::kMad:
        v = "xe_mul(xe_s0, xe_s1) + xe_s2";
        break;
      case ucode::AluVectorOpcode::kCndEq:
        v = "xe_s0 == 0.0f ? xe_s1 : xe_s2";
        break;
      case ucode::AluVectorOpcode::kCndGe:
        v = "xe_s0 >= 0.0f ? xe_s1 : xe_s2";
        break;
      case ucode::AluVectorOpcode::kCndGt:
        v = "xe_s0 > 0.0f ? xe_s1 : xe_s2";
        break;
      case ucode::AluVectorOpcode::kDp4:
        v = "xe_dp(xe_s0, xe_s1, 4u).xxxx";
        break;
      case ucode::AluVectorOpcode::kDp3:
        v = "xe_dp(xe_s0, xe_s1, 3u).xxxx";
        break;
      case ucode::AluVectorOpcode::kDp2Add:
        v = "(xe_dp(xe_s0, xe_s1, 2u) + xe_s2.x).xxxx";
        break;
      case ucode::AluVectorOpcode::kCube:
        v = "xe_cube(xe_s0)";
        break;
      case ucode::AluVectorOpcode::kMax4:
        v = "xe_max4(xe_s0).xxxx";
        break;
      case ucode::AluVectorOpcode::kSetpEqPush:
      case ucode::AluVectorOpcode::kSetpNePush:
      case ucode::AluVectorOpcode::kSetpGtPush:
      case ucode::AluVectorOpcode::kSetpGePush: {
        const char* compare = instr.vector_opcode == ucode::AluVectorOpcode::kSetpEqPush   ? "== 0.0f"
                              : instr.vector_opcode == ucode::AluVectorOpcode::kSetpNePush ? "!= 0.0f"
                              : instr.vector_opcode == ucode::AluVectorOpcode::kSetpGtPush ? "> 0.0f"
                                                                                            : ">= 0.0f";
        Line(fmt::format("bool xe_vp = xe_s0.w == 0.0f && xe_s1.w {};", compare));
        v = fmt::format("((xe_s0.x == 0.0f && xe_s1.x {}) ? 0.0f : xe_s0.x + 1.0f).xxxx", compare);
        vector_sets_p0 = true;
      } break;
      case ucode::AluVectorOpcode::kKillEq:
      case ucode::AluVectorOpcode::kKillGt:
      case ucode::AluVectorOpcode::kKillGe:
      case ucode::AluVectorOpcode::kKillNe: {
        const char* compare = instr.vector_opcode == ucode::AluVectorOpcode::kKillEq   ? "=="
                              : instr.vector_opcode == ucode::AluVectorOpcode::kKillGt ? ">"
                              : instr.vector_opcode == ucode::AluVectorOpcode::kKillGe ? ">="
                                                                                        : "!=";
        Line(fmt::format("bool xe_vkill = any(xe_s0 {} xe_s1);", compare));
        v = "float(xe_vkill).xxxx";
      } break;
      case ucode::AluVectorOpcode::kDst:
        v = "float4(1.0f, xe_mul(xe_s0.y, xe_s1.y), xe_s0.z, xe_s1.w)";
        break;
      case ucode::AluVectorOpcode::kMaxA:
        Line("int xe_va0 = xe_address(xe_s0.w, true);");
        v = "xe_max(xe_s0, xe_s1)";
        vector_sets_a0 = true;
        break;
      default:
        EmitTranslationError(
            fmt::format("Unsupported vector opcode {}", uint32_t(instr.vector_opcode)).c_str());
        v = "0.0f.xxxx";
        break;
    }
    Line(fmt::format("float4 xe_v = {};", v));
  }

  if (scalar_used) {
    std::string s;
    std::string kill;
    switch (instr.scalar_opcode) {
      case ucode::AluScalarOpcode::kAdds:
      case ucode::AluScalarOpcode::kAddsc0:
      case ucode::AluScalarOpcode::kAddsc1:
        s = "xe_sa + xe_sb";
        break;
      case ucode::AluScalarOpcode::kAddsPrev:
        s = "xe_sa + xe_ps";
        break;
      case ucode::AluScalarOpcode::kMuls:
      case ucode::AluScalarOpcode::kMulsc0:
      case ucode::AluScalarOpcode::kMulsc1:
        s = "xe_mul(xe_sa, xe_sb)";
        break;
      case ucode::AluScalarOpcode::kMulsPrev:
        s = "xe_mul(xe_sa, xe_ps)";
        break;
      case ucode::AluScalarOpcode::kMulsPrev2:
        s = "(xe_ps == -XE_FLT_MAX || !isfinite(xe_ps) || !isfinite(xe_sb) || xe_sb <= 0.0f) ? "
            "-XE_FLT_MAX : xe_mul(xe_sa, xe_ps)";
        break;
      case ucode::AluScalarOpcode::kMaxs:
        s = "xe_max(xe_sa, xe_sb)";
        break;
      case ucode::AluScalarOpcode::kMins:
        s = "xe_min(xe_sa, xe_sb)";
        break;
      case ucode::AluScalarOpcode::kSeqs:
        s = "float(xe_sa == 0.0f)";
        break;
      case ucode::AluScalarOpcode::kSgts:
        s = "float(xe_sa > 0.0f)";
        break;
      case ucode::AluScalarOpcode::kSges:
        s = "float(xe_sa >= 0.0f)";
        break;
      case ucode::AluScalarOpcode::kSnes:
        s = "float(xe_sa != 0.0f)";
        break;
      case ucode::AluScalarOpcode::kFrcs:
        s = "xe_sa - floor(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kTruncs:
        s = "trunc(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kFloors:
        s = "floor(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kExp:
        s = "exp2(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kLogc:
        s = "max(log2(xe_sa), -XE_FLT_MAX)";
        break;
      case ucode::AluScalarOpcode::kLog:
        s = "log2(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kRcpc:
        s = "clamp(1.0f / xe_sa, -XE_FLT_MAX, XE_FLT_MAX)";
        break;
      case ucode::AluScalarOpcode::kRcpf:
        s = "isinf(1.0f / xe_sa) ? (xe_sa < 0.0f ? -0.0f : 0.0f) : 1.0f / xe_sa";
        break;
      case ucode::AluScalarOpcode::kRcp:
        s = "1.0f / xe_sa";
        break;
      case ucode::AluScalarOpcode::kRsqc:
        s = "clamp(rsqrt(xe_sa), -XE_FLT_MAX, XE_FLT_MAX)";
        break;
      case ucode::AluScalarOpcode::kRsqf:
        s = "isinf(rsqrt(xe_sa)) ? 0.0f : rsqrt(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kRsq:
        s = "rsqrt(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kMaxAs:
        Line("xe_a0 = xe_address(xe_sa, true);");
        s = "xe_max(xe_sa, xe_sb)";
        break;
      case ucode::AluScalarOpcode::kMaxAsf:
        Line("xe_a0 = xe_address(xe_sa, false);");
        s = "xe_max(xe_sa, xe_sb)";
        break;
      case ucode::AluScalarOpcode::kSubs:
      case ucode::AluScalarOpcode::kSubsc0:
      case ucode::AluScalarOpcode::kSubsc1:
        s = "xe_sa - xe_sb";
        break;
      case ucode::AluScalarOpcode::kSubsPrev:
        s = "xe_sa - xe_ps";
        break;
      case ucode::AluScalarOpcode::kSetpEq:
        Line("xe_p0 = xe_sa == 0.0f;");
        s = "float(!xe_p0)";
        break;
      case ucode::AluScalarOpcode::kSetpNe:
        Line("xe_p0 = xe_sa != 0.0f;");
        s = "float(!xe_p0)";
        break;
      case ucode::AluScalarOpcode::kSetpGt:
        Line("xe_p0 = xe_sa > 0.0f;");
        s = "float(!xe_p0)";
        break;
      case ucode::AluScalarOpcode::kSetpGe:
        Line("xe_p0 = xe_sa >= 0.0f;");
        s = "float(!xe_p0)";
        break;
      case ucode::AluScalarOpcode::kSetpInv:
        Line("xe_p0 = xe_sa == 1.0f;");
        s = "xe_p0 ? 0.0f : (xe_sa == 0.0f ? 1.0f : xe_sa)";
        break;
      case ucode::AluScalarOpcode::kSetpPop:
        Line("xe_p0 = xe_sa - 1.0f <= 0.0f;");
        s = "xe_p0 ? 0.0f : xe_sa - 1.0f";
        break;
      case ucode::AluScalarOpcode::kSetpClr:
        Line("xe_p0 = false;");
        s = "XE_FLT_MAX";
        break;
      case ucode::AluScalarOpcode::kSetpRstr:
        Line("xe_p0 = xe_sa == 0.0f;");
        s = "xe_p0 ? 0.0f : xe_sa";
        break;
      case ucode::AluScalarOpcode::kKillsEq:
        kill = "xe_sa == 0.0f";
        break;
      case ucode::AluScalarOpcode::kKillsGt:
        kill = "xe_sa > 0.0f";
        break;
      case ucode::AluScalarOpcode::kKillsGe:
        kill = "xe_sa >= 0.0f";
        break;
      case ucode::AluScalarOpcode::kKillsNe:
        kill = "xe_sa != 0.0f";
        break;
      case ucode::AluScalarOpcode::kKillsOne:
        kill = "xe_sa == 1.0f";
        break;
      case ucode::AluScalarOpcode::kSqrt:
        s = "sqrt(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kSin:
        s = "sin(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kCos:
        s = "cos(xe_sa)";
        break;
      case ucode::AluScalarOpcode::kRetainPrev:
        break;
      default:
        EmitTranslationError(
            fmt::format("Unsupported scalar opcode {}", uint32_t(instr.scalar_opcode)).c_str());
        s = "0.0f";
        break;
    }
    if (!kill.empty()) {
      Line(fmt::format("bool xe_skill = {};", kill));
      s = "float(xe_skill)";
      if (kills) {
        Line("if (xe_skill) discard;");
      }
    }
    if (!s.empty()) {
      Line(fmt::format("xe_ps = {};", s));
    }
  }

  if (vector_used) {
    if (vector_sets_a0) {
      Line("xe_a0 = xe_va0;");
    }
    if (vector_sets_p0) {
      Line("xe_p0 = xe_vp;");
    }
    if (kills && instr.vector_opcode >= ucode::AluVectorOpcode::kKillEq &&
        instr.vector_opcode <= ucode::AluVectorOpcode::kKillNe) {
      Line("if (xe_vkill) discard;");
    }
    StoreResult(instr.vector_and_constant_result, "xe_v");
  } else {
    // Constant-only writes (the vector operation is a no-op).
    StoreResult(instr.vector_and_constant_result, "0.0f.xxxx");
  }
  if (scalar_used || instr.scalar_result.GetUsedWriteMask()) {
    StoreResult(instr.scalar_result, "xe_ps.xxxx");
  }

  if (instr.is_predicated) {
    Unindent();
    Line("}");
  }
  Unindent();
  Line("}");
}

void HlslShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  if (instr.result.storage_target == InstructionStorageTarget::kNone ||
      !instr.result.GetUsedWriteMask()) {
    return;
  }
  const auto& attributes = instr.attributes;
  Line(fmt::format("{{  // {} format {} offset {} stride {}", instr.opcode_name,
                   uint32_t(attributes.data_format), attributes.offset, attributes.stride));
  Indent();
  if (instr.is_predicated) {
    Line(fmt::format("if ({}) {{", PredicateCondition(instr.predicate_condition)));
    Indent();
  }
  Line("float4 xe_r;");
  uint32_t fetch_constant = instr.operands[1].storage_index;
  Line(fmt::format("int xe_vi = int(floor({}{}));", OperandComponent(instr.operands[0], 0),
                   attributes.is_index_rounded ? " + 0.5f" : ""));
  Line(fmt::format("int xe_vd = xe_vi * {} + {};", attributes.stride, attributes.offset));

  uint32_t dword_count;
  switch (attributes.data_format) {
    case xenos::VertexFormat::k_16_16_16_16:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_FLOAT:
      dword_count = 2;
      break;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      dword_count = 3;
      break;
    case xenos::VertexFormat::k_32_32_32_32:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      dword_count = 4;
      break;
    default:
      dword_count = 1;
      break;
  }
  for (uint32_t i = 0; i < dword_count; ++i) {
    Line(fmt::format("uint xe_d{} = xe_vfetch_dword({}u, xe_vd + {});", i, fetch_constant, i));
  }
  Line("float4 xe_f = float4(0.0f, 0.0f, 0.0f, 1.0f);");

  bool normalize = !attributes.is_integer;
  bool no_zero = attributes.signed_rf_mode == xenos::SignedRepeatingFractionMode::kNoZero;
  // Packed formats: (dword, offset, width) per component.
  struct Packed {
    uint32_t dword, offset, width;
  };
  auto emit_packed = [&](std::initializer_list<Packed> components) {
    uint32_t i = 0;
    for (const Packed& c : components) {
      if (attributes.is_signed) {
        Line(fmt::format("xe_f.{} = xe_unpack_signed(xe_d{}, {}u, {}u, {}, {});", kComponents[i],
                         c.dword, c.offset, c.width, normalize, no_zero));
      } else {
        Line(fmt::format("xe_f.{} = xe_unpack_unsigned(xe_d{}, {}u, {}u, {});", kComponents[i],
                         c.dword, c.offset, c.width, normalize));
      }
      ++i;
    }
  };
  uint32_t fetched_components = 4;
  switch (attributes.data_format) {
    case xenos::VertexFormat::k_8_8_8_8:
      emit_packed({{0, 0, 8}, {0, 8, 8}, {0, 16, 8}, {0, 24, 8}});
      break;
    case xenos::VertexFormat::k_2_10_10_10:
      emit_packed({{0, 0, 10}, {0, 10, 10}, {0, 20, 10}, {0, 30, 2}});
      break;
    case xenos::VertexFormat::k_10_11_11:
      emit_packed({{0, 0, 11}, {0, 11, 11}, {0, 22, 10}});
      fetched_components = 3;
      break;
    case xenos::VertexFormat::k_11_11_10:
      emit_packed({{0, 0, 10}, {0, 10, 11}, {0, 21, 11}});
      fetched_components = 3;
      break;
    case xenos::VertexFormat::k_16_16:
      emit_packed({{0, 0, 16}, {0, 16, 16}});
      fetched_components = 2;
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      emit_packed({{0, 0, 16}, {0, 16, 16}, {1, 0, 16}, {1, 16, 16}});
      break;
    case xenos::VertexFormat::k_16_16_FLOAT:
      Line("xe_f.xy = f16tof32(uint2(xe_d0, xe_d0 >> 16u));");
      fetched_components = 2;
      break;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      Line("xe_f = f16tof32(uint4(xe_d0, xe_d0 >> 16u, xe_d1, xe_d1 >> 16u));");
      break;
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32: {
      fetched_components = attributes.data_format == xenos::VertexFormat::k_32     ? 1
                           : attributes.data_format == xenos::VertexFormat::k_32_32 ? 2
                                                                                    : 4;
      for (uint32_t i = 0; i < fetched_components; ++i) {
        std::string x = attributes.is_signed ? fmt::format("float(int(xe_d{}))", i)
                                             : fmt::format("float(xe_d{})", i);
        if (normalize) {
          x = attributes.is_signed
                  ? (no_zero ? fmt::format("({} + 0.5f) / 2147483647.5f", x)
                             : fmt::format("{} / 2147483647.0f", x))
                  : fmt::format("{} / 4294967295.0f", x);
        }
        Line(fmt::format("xe_f.{} = {};", kComponents[i], x));
      }
    } break;
    case xenos::VertexFormat::k_32_FLOAT:
      Line("xe_f.x = asfloat(xe_d0);");
      fetched_components = 1;
      break;
    case xenos::VertexFormat::k_32_32_FLOAT:
      Line("xe_f.xy = asfloat(uint2(xe_d0, xe_d1));");
      fetched_components = 2;
      break;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      Line("xe_f.xyz = asfloat(uint3(xe_d0, xe_d1, xe_d2));");
      fetched_components = 3;
      break;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      Line("xe_f = asfloat(uint4(xe_d0, xe_d1, xe_d2, xe_d3));");
      break;
    default:
      EmitTranslationError(
          fmt::format("Unsupported vertex format {}", uint32_t(attributes.data_format)).c_str(),
          false);
      fetched_components = 0;
      break;
  }
  if (attributes.exp_adjust && fetched_components) {
    std::string mask(kComponents, fetched_components);
    Line(fmt::format("xe_f.{} *= {};", mask, FloatLiteral(std::ldexp(1.0f, attributes.exp_adjust))));
  }
  StoreResult(instr.result, "xe_f");

  if (instr.is_predicated) {
    Unindent();
    Line("}");
  }
  Unindent();
  Line("}");
}

uint32_t HlslShaderTranslator::TextureSlotIndex(uint32_t fetch_constant,
                                                xenos::FetchOpDimension dimension) {
  for (uint32_t i = 0; i < uint32_t(texture_slots_.size()); ++i) {
    if (texture_slots_[i].fetch_constant == fetch_constant &&
        texture_slots_[i].dimension == dimension) {
      return i;
    }
  }
  texture_slots_.push_back({fetch_constant, dimension});
  return uint32_t(texture_slots_.size() - 1);
}

void HlslShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  const auto& attributes = instr.attributes;
  bool writes = instr.has_result() && instr.result.GetUsedWriteMask();
  switch (instr.opcode) {
    case ucode::FetchOpcode::kSetTextureLod:
    case ucode::FetchOpcode::kSetTextureGradientsHorz:
    case ucode::FetchOpcode::kSetTextureGradientsVert:
      break;
    default:
      if (!writes) {
        return;
      }
      break;
  }
  Line(fmt::format("{{  // {}", instr.opcode_name));
  Indent();
  if (instr.is_predicated) {
    Line(fmt::format("if ({}) {{", PredicateCondition(instr.predicate_condition)));
    Indent();
  }
  Line("float4 xe_r;");
  Line(fmt::format("float4 xe_tc = {};", OperandExpression(instr.operands[0], 4)));

  switch (instr.opcode) {
    case ucode::FetchOpcode::kSetTextureLod:
      Line("xe_lod = xe_tc.x;");
      break;
    case ucode::FetchOpcode::kSetTextureGradientsHorz:
      Line("xe_gradient_h = xe_tc.xyz;");
      break;
    case ucode::FetchOpcode::kSetTextureGradientsVert:
      Line("xe_gradient_v = xe_tc.xyz;");
      break;
    case ucode::FetchOpcode::kGetTextureBorderColorFrac:
      StoreResult(instr.result, "0.0f.xxxx");
      break;
    case ucode::FetchOpcode::kGetTextureGradients:
      StoreResult(instr.result, is_pixel_shader()
                                    ? "float4(ddx(xe_tc.x), ddy(xe_tc.x), ddx(xe_tc.y), ddy(xe_tc.y))"
                                    : "0.0f.xxxx");
      break;
    case ucode::FetchOpcode::kTextureFetch:
    case ucode::FetchOpcode::kGetTextureComputedLod:
    case ucode::FetchOpcode::kGetTextureWeights: {
      uint32_t slot = TextureSlotIndex(instr.operands[1].storage_index, instr.dimension);
      std::string texture = fmt::format("xe_texture{}", slot);
      std::string sampler = fmt::format("xe_sampler{}", slot);
      bool is_cube = instr.dimension == xenos::FetchOpDimension::kCube;
      bool is_3d = instr.dimension == xenos::FetchOpDimension::k3DOrStacked;
      bool is_1d = instr.dimension == xenos::FetchOpDimension::k1D;
      // Texture size, for unnormalized coordinates, offsets and weights.
      bool has_offset = !is_cube && (attributes.offset_x || attributes.offset_y ||
                                     (is_3d && attributes.offset_z));
      bool needs_size = !is_cube && (attributes.unnormalized_coordinates || has_offset ||
                                     instr.opcode == ucode::FetchOpcode::kGetTextureWeights);
      if (needs_size) {
        if (is_3d) {
          Line("uint3 xe_ts;");
          Line(fmt::format("{}.GetDimensions(xe_ts.x, xe_ts.y, xe_ts.z);", texture));
        } else {
          Line("uint3 xe_ts = uint3(0u, 0u, 1u);");
          Line(fmt::format("{}.GetDimensions(xe_ts.x, xe_ts.y);", texture));
        }
        Line("float3 xe_size = float3(max(xe_ts, 1u));");
      }
      std::string coord;
      if (is_cube) {
        Line("float3 xe_coord = xe_cube_direction(xe_tc.xyz);");
        coord = "xe_coord";
      } else {
        const char* mask = is_3d ? "xyz" : is_1d ? "x" : "xy";
        const char* type = is_3d ? "float3" : is_1d ? "float" : "float2";
        Line(fmt::format("{} xe_coord = xe_tc.{};", type, mask));
        if (has_offset) {
          Line(fmt::format("xe_coord += {}({}, {}, {}).{};", "float3", FloatLiteral(attributes.offset_x),
                           FloatLiteral(attributes.offset_y), FloatLiteral(attributes.offset_z),
                           mask) +
               (attributes.unnormalized_coordinates ? "" : fmt::format(" / xe_size.{}", mask)));
        }
        if (attributes.unnormalized_coordinates) {
          Line(fmt::format("xe_coord /= xe_size.{};", mask));
        }
        if (is_1d) {
          Line("float2 xe_coord2 = float2(xe_coord, 0.5f);");
          coord = "xe_coord2";
        } else {
          coord = "xe_coord";
        }
      }
      if (instr.opcode == ucode::FetchOpcode::kGetTextureWeights) {
        if (is_cube) {
          StoreResult(instr.result, "0.0f.xxxx");
        } else {
          Line(fmt::format("float3 xe_w = frac({} * xe_size{} - 0.5f);",
                           is_3d ? "xe_coord" : fmt::format("float3({}, 0.0f)", coord),
                           is_3d ? "" : ""));
          StoreResult(instr.result, "float4(xe_w, 0.0f)");
        }
        break;
      }
      if (instr.opcode == ucode::FetchOpcode::kGetTextureComputedLod) {
        StoreResult(instr.result,
                    is_pixel_shader()
                        ? fmt::format("{}.CalculateLevelOfDetail({}, {}).xxxx", texture, sampler, coord)
                        : std::string("0.0f.xxxx"));
        break;
      }
      std::string bias = FloatLiteral(attributes.lod_bias);
      if (attributes.use_register_lod) {
        bias = "(xe_lod + " + bias + ")";
      }
      std::string sample;
      if (attributes.use_register_gradients) {
        const char* gmask = is_cube || is_3d ? "xyz" : "xy";
        std::string gh = fmt::format("xe_gradient_h.{}", gmask);
        std::string gv = fmt::format("xe_gradient_v.{}", gmask);
        if (is_1d) {
          gh = "float2(xe_gradient_h.x, 0.0f)";
          gv = "float2(xe_gradient_v.x, 0.0f)";
        }
        sample = fmt::format("{}.SampleGrad({}, {}, {}, {})", texture, sampler, coord, gh, gv);
      } else if (is_pixel_shader() && attributes.use_computed_lod) {
        if (!attributes.use_register_lod && attributes.lod_bias == 0.0f) {
          sample = fmt::format("{}.Sample({}, {})", texture, sampler, coord);
        } else {
          sample = fmt::format("{}.SampleBias({}, {}, {})", texture, sampler, coord, bias);
        }
      } else {
        sample = fmt::format("{}.SampleLevel({}, {}, {})", texture, sampler, coord, bias);
      }
      StoreResult(instr.result, sample);
    } break;
    default:
      EmitTranslationError(
          fmt::format("Unsupported fetch opcode {}", uint32_t(instr.opcode)).c_str(), false);
      break;
  }

  if (instr.is_predicated) {
    Unindent();
    Line("}");
  }
  Unindent();
  Line("}");
}

std::vector<uint8_t> HlslShaderTranslator::CompleteTranslation() {
  const Shader& shader = current_shader();
  uint32_t register_count = std::max(this->register_count(), uint32_t(1));
  std::string hlsl = kHlslPrologue;

  hlsl += fmt::format("static float4 r[{}];\n", register_count);
  if (is_vertex_shader()) {
    hlsl += fmt::format("static float4 xe_o_interpolators[{}];\n", kInterpolatorCount);
    hlsl += "static float4 xe_o_position;\n";
    hlsl += "static float4 xe_o_point_size_edge_kill;\n";
  } else {
    hlsl += "static float4 xe_o_color[4];\n";
    hlsl += "static float4 xe_o_depth;\n";
  }

  // Textures and samplers of this stage.
  uint32_t space = is_vertex_shader() ? 1 : 2;
  for (uint32_t i = 0; i < uint32_t(texture_slots_.size()); ++i) {
    const char* type;
    switch (texture_slots_[i].dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        type = "Texture3D";
        break;
      case xenos::FetchOpDimension::kCube:
        type = "TextureCube";
        break;
      default:
        type = "Texture2D";
        break;
    }
    hlsl += fmt::format("{} xe_texture{} : register(t{}, space{});  // fetch constant {}\n", type,
                        i, i, space, texture_slots_[i].fetch_constant);
    hlsl += fmt::format("SamplerState xe_sampler{} : register(s{}, space{});\n", i, i, space);
  }

  // The translated program.
  hlsl += "\nvoid xe_execute() {\n";
  if (uses_state_machine_) {
    hlsl += "  uint xe_pc = 0u;\n";
    hlsl += "  [loop] while (true) {\n";
    hlsl += "    switch (xe_pc) {\n";
    // Re-indent the body into the switch.
    std::string indented;
    size_t pos = 0;
    while (pos < body_.size()) {
      size_t end = body_.find('\n', pos);
      if (end == std::string::npos) {
        end = body_.size();
      }
      indented += "    ";
      indented.append(body_, pos, end - pos);
      indented += '\n';
      pos = end + 1;
    }
    hlsl += indented;
    hlsl += "        return;\n";
    hlsl += "      }\n";
    hlsl += "      default:\n";
    hlsl += "        return;\n";
    hlsl += "    }\n";
    hlsl += "  }\n";
  } else {
    hlsl += body_;
  }
  hlsl += "}\n\n";

  // Entry point.
  hlsl += "struct XeVertexOutput {\n";
  hlsl += fmt::format("  float4 interpolators[{}] : TEXCOORD0;\n", kInterpolatorCount);
  hlsl += "  float4 position : SV_Position;\n";
  hlsl += "};\n\n";
  if (is_vertex_shader()) {
    hlsl += "XeVertexOutput main(uint xe_vertex_id : SV_VertexID) {\n";
    hlsl += "  r[0].x = float(int(xe_vertex_id) + xe_vertex_index_offset);\n";
    hlsl += "  xe_execute();\n";
    hlsl += "  XeVertexOutput xe_output;\n";
    hlsl += "  xe_output.interpolators = xe_o_interpolators;\n";
    hlsl += "  float4 xe_position = xe_o_position;\n";
    hlsl += "  if ((xe_flags & 4u) != 0u) xe_position.w = 1.0f / xe_position.w;\n";
    hlsl += "  if ((xe_flags & 1u) != 0u) xe_position.xy *= xe_position.w;\n";
    hlsl += "  if ((xe_flags & 2u) != 0u) xe_position.z *= xe_position.w;\n";
    hlsl +=
        "  xe_position.xyz = xe_position.xyz * xe_ndc_scale.xyz + xe_ndc_offset.xyz * "
        "xe_position.w;\n";
    hlsl += "  xe_output.position = xe_position;\n";
    hlsl += "  return xe_output;\n";
    hlsl += "}\n";
  } else {
    bool has_outputs = shader.writes_depth();
    for (uint32_t i = 0; i < 4; ++i) {
      has_outputs |= shader.writes_color_target(i);
    }
    if (has_outputs) {
      hlsl += "struct XePixelOutput {\n";
      for (uint32_t i = 0; i < 4; ++i) {
        if (shader.writes_color_target(i)) {
          hlsl += fmt::format("  float4 color{} : SV_Target{};\n", i, i);
        }
      }
      if (shader.writes_depth()) {
        hlsl += "  float depth : SV_Depth;\n";
      }
      hlsl += "};\n\n";
    }
    hlsl += fmt::format("{} main(XeVertexOutput xe_input, bool xe_front_facing : SV_IsFrontFace) {{\n",
                        has_outputs ? "XePixelOutput" : "void");
    uint32_t interpolators = std::min(register_count, kInterpolatorCount);
    hlsl += fmt::format("  for (uint xe_i = 0u; xe_i < {}u; ++xe_i) {{\n", interpolators);
    hlsl += "    r[xe_i] = xe_input.interpolators[xe_i];\n";
    hlsl += "  }\n";
    hlsl += "  if (xe_ps_param_gen != 0u) {\n";
    hlsl += "    float2 xe_pixel = floor(xe_input.position.xy);\n";
    hlsl += "    float xe_x = xe_front_facing ? xe_pixel.x : asfloat(asuint(xe_pixel.x) | 0x80000000u);\n";
    hlsl += fmt::format("    r[min(xe_ps_param_gen - 1u, {}u)] = float4(xe_x, xe_pixel.y, 0.0f, 0.0f);\n",
                        register_count - 1);
    hlsl += "  }\n";
    hlsl += "  xe_execute();\n";
    if (!has_outputs) {
      // Depth, stencil or kill only.
      hlsl += "}\n";
      return std::vector<uint8_t>(hlsl.begin(), hlsl.end());
    }
    hlsl += "  XePixelOutput xe_output = (XePixelOutput)0;\n";
    if (shader.writes_color_target(0)) {
      hlsl += "  float xe_alpha = xe_o_color[0].a;\n";
      hlsl += "  uint xe_func = xe_alpha_test & 7u;\n";
      hlsl += "  bool xe_alpha_pass = xe_func == 7u ||\n";
      hlsl += "      (xe_func == 1u && xe_alpha < xe_alpha_ref) || (xe_func == 2u && xe_alpha == xe_alpha_ref) ||\n";
      hlsl += "      (xe_func == 3u && xe_alpha <= xe_alpha_ref) || (xe_func == 4u && xe_alpha > xe_alpha_ref) ||\n";
      hlsl += "      (xe_func == 5u && xe_alpha != xe_alpha_ref) || (xe_func == 6u && xe_alpha >= xe_alpha_ref);\n";
      hlsl += "  if (!xe_alpha_pass) discard;\n";
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (shader.writes_color_target(i)) {
        hlsl += fmt::format("  xe_output.color{} = xe_o_color[{}];\n", i, i);
      }
    }
    if (shader.writes_depth()) {
      hlsl += "  xe_output.depth = xe_o_depth.x;\n";
    }
    hlsl += "  return xe_output;\n";
    hlsl += "}\n";
  }
  return std::vector<uint8_t>(hlsl.begin(), hlsl.end());
}

}  // namespace hajinmon::renderer

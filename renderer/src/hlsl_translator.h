// Hajinmon native renderer - Xenos microcode to HLSL translator.
//
// The SDK parses the microcode and calls the Process* hooks in program order;
// this translator turns each hook into HLSL text. The result is compiled with
// DXC to DXIL (Direct3D 12) or SPIR-V (Vulkan).
//
// Control flow: shaders without jumps, calls or loops become straight-line
// code. The others become a state machine, `while (true) { switch (pc) ... }`,
// with a case per branch target, since Xenos control flow is not structured.
//
// Resource interface, shared by all translated shaders (see kHlslPrologue in
// the .cpp for the exact layout):
//   b0 space0 - system constants, set by the command processor.
//   b1 space0 - float constants of the stage (256 float4).
//   b2 space0 - bool (256 bits) and loop (32 dwords) constants.
//   b3 space0 - fetch constants (32 x 6 dwords).
//   t0 space0 - shared memory (guest physical memory) for vertex fetches.
//   tN/sN space1 (vertex) or space2 (pixel) - textures and samplers, one per
//                                             entry of texture_slots().

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/translator.h>

namespace hajinmon::renderer {

class HlslShaderTranslator : public rex::graphics::ShaderTranslator {
 public:
  // A texture the shader samples: register tN / sN of the stage space.
  struct TextureSlot {
    uint32_t fetch_constant;
    rex::graphics::xenos::FetchOpDimension dimension;
  };

  HlslShaderTranslator() = default;

  // Valid after TranslateAnalyzedShader.
  const std::vector<TextureSlot>& texture_slots() const { return texture_slots_; }
  // Number of memory export instructions skipped (not supported yet).
  uint32_t skipped_memexports() const { return skipped_memexports_; }

 protected:
  void Reset() override;

  void StartTranslation() override;
  std::vector<uint8_t> CompleteTranslation() override;

  void PreProcessControlFlowInstructions(
      std::vector<rex::graphics::ucode::ControlFlowInstruction> instrs) override;
  void ProcessControlFlowInstructionBegin(uint32_t cf_index) override;

  void ProcessExecInstructionBegin(const rex::graphics::ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const rex::graphics::ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(const rex::graphics::ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(const rex::graphics::ParsedLoopEndInstruction& instr) override;
  void ProcessCallInstruction(const rex::graphics::ParsedCallInstruction& instr) override;
  void ProcessReturnInstruction(const rex::graphics::ParsedReturnInstruction& instr) override;
  void ProcessJumpInstruction(const rex::graphics::ParsedJumpInstruction& instr) override;

  void ProcessVertexFetchInstruction(
      const rex::graphics::ParsedVertexFetchInstruction& instr) override;
  void ProcessTextureFetchInstruction(
      const rex::graphics::ParsedTextureFetchInstruction& instr) override;
  void ProcessAluInstruction(const rex::graphics::ParsedAluInstruction& instr,
                             uint8_t memexport_eM_potentially_written_before) override;

 private:
  void Line(const std::string& text);
  void Indent() { ++indent_; }
  void Unindent() { --indent_; }

  // HLSL for a bool constant test, or a predicate test.
  static std::string BoolConstantCondition(uint32_t index, bool condition);
  static std::string PredicateCondition(bool condition);
  // Opens `if (...) {` for conditional control flow; returns whether it did.
  bool BeginCondition(bool is_unconditional, bool is_predicated, uint32_t bool_index,
                      bool condition);
  // Jumps to a cf index through the state machine.
  void EmitJump(uint32_t target);

  std::string RegisterExpression(uint32_t index,
                                 rex::graphics::InstructionStorageAddressingMode mode) const;
  // The operand, swizzled to `component_count` components (abs/neg applied).
  std::string OperandExpression(const rex::graphics::InstructionOperand& operand,
                                uint32_t component_count) const;
  // A single component of the operand.
  std::string OperandComponent(const rex::graphics::InstructionOperand& operand,
                               uint32_t component) const;
  // Stores `value` (a float4 expression) to the result, honoring its swizzle,
  // write mask and saturation.
  void StoreResult(const rex::graphics::InstructionResult& result, const std::string& value);

  uint32_t TextureSlotIndex(uint32_t fetch_constant, rex::graphics::xenos::FetchOpDimension dim);

  std::string body_;
  int indent_ = 0;
  // Branch targets, including the return points of calls.
  std::set<uint32_t> labels_;
  bool uses_state_machine_ = false;
  std::vector<TextureSlot> texture_slots_;
  uint32_t skipped_memexports_ = 0;
  // Whether an exec block, or an instruction predicate block, is open.
  bool exec_condition_open_ = false;
};

}  // namespace hajinmon::renderer

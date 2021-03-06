#pragma once

#include <any>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "../../arm_code/arm.hpp"
#include "../../include/aixlog.hpp"
#include "../../prelude/prelude.hpp"
#include "../backend.hpp"
#include "../optimization/optimization.hpp"

namespace backend::codegen {

inline std::string format_const_label(std::string_view function_name,
                                      uint32_t label_id) {
  auto s = std::stringstream();
  s << ".const_" << function_name << "$" << label_id;
  return s.str();
}

inline std::string format_load_pc_label(std::string_view function_name,
                                        uint32_t label_id) {
  auto s = std::stringstream();
  s << ".ld_pc_" << function_name << "$" << label_id;
  return s.str();
}

inline std::string format_bb_label(std::string_view function_name,
                                   uint32_t label_id) {
  auto s = std::stringstream();
  s << ".bb_" << function_name << "$" << label_id;
  return s.str();
}

inline std::string format_fn_end_label(std::string_view function_name) {
  auto s = std::stringstream();
  s << ".end_" << function_name << "$";
  return s.str();
}

struct LastJump {
  arm::ConditionCode cond;
  mir::types::LabelId bb_true;
  mir::types::LabelId bb_false;
};

class Codegen final {
 public:
  Codegen(mir::inst::MirFunction& func, mir::inst::MirPackage& package,
          std::map<std::string, std::any>& extra_data,
          bool enable_cond_exec = true)
      : func(func),
        package(package),
        extra_data(extra_data),
        inst(),
        reg_map(),
        fixed_vars(),
        var_collapse(),
        bb_ordering(),
        enable_cond_exec(enable_cond_exec) {
    {
      // Calculate ordering
      auto any_ordering =
          extra_data.find(optimization::BASIC_BLOCK_ORDERING_DATA_NAME);
      if (any_ordering != extra_data.end()) {
        auto& order_map = std::any_cast<optimization::BasicBlockOrderingType&>(
            any_ordering->second);
        auto o = order_map.find(func.name);
        if (o != order_map.end()) {
          LOG(TRACE) << "Found order map for " << func.name << " with "
                     << o->second.size() << " elements" << std::endl;
          auto& ordering = o->second;
          this->bb_ordering.insert(bb_ordering.end(), ordering.begin(),
                                   ordering.end());
        } else {
          LOG(TRACE) << "Cannot find order map for " << func.name << " (1)"
                     << std::endl;
          for (auto& a : func.basic_blks) {
            bb_ordering.push_back(a.first);
          }
        }
      } else {
        LOG(TRACE) << "Cannot find order map for " << func.name << " (2)"
                   << std::endl;
        for (auto& a : func.basic_blks) {
          bb_ordering.push_back(a.first);
        }
      }
    }
    auto inline_hint = extra_data.find(optimization::inline_blks);
    if (inline_hint != extra_data.end()) {
      LOG(TRACE) << "Found inline hint in extra data" << std::endl;

      this->inline_hint =
          std::any_cast<optimization::InlineBlksType&>(inline_hint->second)
              .at(func.name);

      LOG(TRACE) << "Inline hint has " << this->inline_hint.size()
                 << " items: ";
      for (auto i : this->inline_hint) LOG(TRACE) << i.first << " ";
      LOG(TRACE) << std::endl;
    }
    param_size = func.type->params.size();
  }

  /// Translate MirPackage into ArmCode
  arm::Function translate_function();

 private:
  mir::inst::MirFunction& func;
  mir::inst::MirPackage& package;

  std::map<std::string, std::any>& extra_data;

  std::vector<std::unique_ptr<arm::Inst>> inst;
  std::map<mir::inst::VarId, arm::Reg> reg_map;
  std::map<arm::Label, arm::ConstValue> consts;

  std::unordered_map<mir::types::LabelId, std::set<mir::inst::VarId>> var_use;
  std::unordered_map<mir::inst::VarId, mir::inst::VarId> var_collapse;
  std::unordered_map<mir::inst::VarId, arm::Reg> fixed_vars;
  std::unordered_map<mir::inst::VarId, arm::Reg> phi_reg;

  std::map<mir::inst::VarId, int32_t> stack_space_allocation;

  std::vector<uint32_t> bb_ordering;
  std::map<uint32_t, arm::ConditionCode> inline_hint;

  std::optional<arm::ConditionCode> second_last_condition_code;
  std::optional<LastJump> last_jump;
  std::optional<LastJump> this_jump;

  uint32_t vreg_gp_counter = 0;
  uint32_t vreg_vd_counter = 0;
  uint32_t vreg_vq_counter = 0;
  uint32_t const_counter = 0;
  bool enable_cond_exec;

  int param_size;
  uint32_t stack_size = 0;

  arm::Reg get_or_alloc_vgp(mir::inst::VarId v);
  arm::Reg get_or_alloc_vd(mir::inst::VarId v);
  arm::Reg get_or_alloc_vq(mir::inst::VarId v);
  arm::Reg alloc_vgp();
  arm::Reg alloc_vd();
  arm::Reg alloc_vq();

  arm::Operand2 translate_value_to_operand2(mir::inst::Value& v);
  arm::Reg translate_value_to_reg(mir::inst::Value& v);
  arm::Reg translate_var_reg(mir::inst::VarId v);

  void make_number(arm::Reg reg, uint32_t num);

  arm::MemoryOperand translate_var_to_memory_arg(mir::inst::VarId v);
  arm::MemoryOperand translate_var_to_memory_arg(mir::inst::Value& v);
  arm::MemoryOperand translate_var_to_memory_arg(mir::inst::Value& v,
                                                 mir::inst::Value& offset);
  arm::MemoryOperand translate_var_to_memory_arg(mir::inst::VarId v,
                                                 mir::inst::Value& offset);

  void scan();
  void scan_stack();
  void init_reg_map();
  void deal_call(mir::inst::CallInst& call);
  void deal_phi(mir::inst::PhiInst& phi);
  void add_collapsed_var(mir::inst::VarId, mir::inst::VarId);
  mir::inst::VarId get_collapsed_var(mir::inst::VarId i);
  void generate_startup();
  void generate_return_and_cleanup();

  // arm::Function translate_function(mir::inst::MirFunction& f);
  void translate_basic_block(mir::inst::BasicBlk& blk);
  void translate_inst(mir::inst::AssignInst& i);
  void translate_inst(mir::inst::PhiInst& i);
  void translate_inst(mir::inst::CallInst& i);
  void translate_inst(mir::inst::StoreInst& i);
  void translate_inst(mir::inst::LoadInst& i);
  void translate_inst(mir::inst::StoreOffsetInst& i);
  void translate_inst(mir::inst::LoadOffsetInst& i);
  void translate_inst(mir::inst::RefInst& i);
  void translate_inst(mir::inst::PtrOffsetInst& i);
  void translate_inst(mir::inst::OpInst& i);
  void translate_inst(mir::inst::OpAccInst& i);

  void emit_phi_move(std::unordered_set<mir::inst::VarId>& i);
  void emit_compare(mir::inst::VarId& dest, mir::inst::Value& lhs,
                    mir::inst::Value& rhs, arm::ConditionCode cond,
                    bool reversed);

  void translate_branch(mir::inst::JumpInstruction& j);
};
}  // namespace backend::codegen

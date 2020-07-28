#include "codegen.hpp"

#include <any>
#include <cassert>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <typeinfo>
#include <unordered_set>

#include "../../include/aixlog.hpp"
#include "err.hpp"

namespace backend::codegen {

using namespace arm;

arm::Function Codegen::translate_function() {
  init_reg_map();
  scan_stack();
  scan();
  generate_startup();
  for (auto bb_id : bb_ordering) {
    auto& bb = func.basic_blks.at(bb_id);
    translate_basic_block(bb);
  }
  generate_return_and_cleanup();

  {
#pragma region passShow
    LOG(TRACE) << "VariableToReg: " << std::endl;
    for (auto& v : reg_map) {
      LOG(TRACE) << v.first << " -> ";
      display_reg_name(LOG(TRACE), v.second);
      LOG(TRACE) << std::endl;
    }
    LOG(TRACE) << std::endl;
#pragma endregion
  }

  {
    // Put variable to vreg map in extra data
    auto data =
        extra_data.find(optimization::MIR_VARIABLE_TO_ARM_VREG_DATA_NAME);
    if (data == extra_data.end()) {
      extra_data.insert({optimization::MIR_VARIABLE_TO_ARM_VREG_DATA_NAME,
                         optimization::MirVariableToArmVRegType()});
      data = extra_data.find(optimization::MIR_VARIABLE_TO_ARM_VREG_DATA_NAME);
    }
    auto& map =
        std::any_cast<optimization::MirVariableToArmVRegType&>(data->second);
    map.insert({func.name, std::move(this->reg_map)});
  }

  return arm::Function(func.name, func.type, std::move(this->inst),
                       std::move(this->consts), stack_size);
}

bool is_comparison(mir::inst::Op op) {
  const std::set<mir::inst::Op> comp = {mir::inst::Op::Gt,  mir::inst::Op::Lt,
                                        mir::inst::Op::Gte, mir::inst::Op::Lte,
                                        mir::inst::Op::Eq,  mir::inst::Op::Neq};
  return comp.find(op) != comp.end();
}

void Codegen::translate_basic_block(mir::inst::BasicBlk& blk) {
  auto label = format_bb_label(func.name, blk.id);
  if (inst.size() > 0 && inst.back()->op == arm::OpCode::_Label) {
    auto& i = static_cast<arm::LabelInst&>(*inst.back());
    if (i.label == label) {
      inst.pop_back();
    }
  }
  inst.push_back(std::make_unique<LabelInst>(label));
  // HACK: To make comparison closer to branch, `emit_phi_move` runs before the
  // HACK: first comparison
  bool met_cmp = false;
  std::unordered_set<mir::inst::VarId> use_vars;

  for (auto& inst : blk.inst) {
    auto& i = *inst;
    if (auto x = dynamic_cast<mir::inst::OpInst*>(&i)) {
      if (is_comparison(x->op) && !met_cmp) {
        emit_phi_move(std::move(use_vars));
        met_cmp = true;
      }
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::CallInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::AssignInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::LoadInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::StoreInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::LoadOffsetInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::StoreOffsetInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::RefInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::PhiInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else if (auto x = dynamic_cast<mir::inst::PtrOffsetInst*>(&i)) {
      met_cmp = false;
      translate_inst(*x);
    } else {
      throw new std::bad_cast();
    }
    use_vars.insert(i.dest);
  }
  if (!met_cmp) emit_phi_move(use_vars);
  translate_branch(blk.jump);
}

void Codegen::init_reg_map() {
  // reg_map.insert({mir::inst::VarId(0), Reg(0)});
  for (int i = 0; i < 4 && i < param_size; i++) {
    reg_map.insert({mir::inst::VarId(i + 1), Reg(i)});
  }
}

void Codegen::generate_return_and_cleanup() {
  inst.push_back(std::make_unique<LabelInst>(format_fn_end_label(func.name)));
  // NOTE: Register counting is done AFTER register allocation, so there's not
  // much cleanup here. Insert register counting after end label.
  inst.push_back(std::make_unique<Arith2Inst>(OpCode::Mov, REG_SP,
                                              RegisterOperand(REG_FP)));
  inst.push_back(
      std::make_unique<PushPopInst>(OpCode::Pop, std::set{REG_FP, REG_PC}));
  // ^ this final pop sets PC and returns the function
}

void Codegen::generate_startup() {
  inst.push_back(
      std::make_unique<PushPopInst>(OpCode::Push, std::set{REG_FP, REG_LR}));
  // TODO: Push all used register to save them, probably after register alloc
  inst.push_back(std::make_unique<Arith2Inst>(OpCode::Mov, REG_FP,
                                              RegisterOperand(REG_SP)));
  // TODO: Expand stack here; we haven't allocated the stack for now!
}

arm::Reg Codegen::get_or_alloc_vgp(mir::inst::VarId v) {
  // auto v = get_collapsed_var(v_);
  // If it's param, load before use
  if (v > 4 && v <= param_size) {
    auto reg = alloc_vgp();
    inst.push_back(std::make_unique<LoadStoreInst>(
        OpCode::LdR, reg, MemoryOperand(Reg(REG_FP), (v - 5) * 4)));
    return reg;
  } else {
    auto x = stack_space_allocation.find(v);
    if (x != stack_space_allocation.end()) {
      auto reg = alloc_vgp();
      inst.push_back(std::make_unique<Arith3Inst>(OpCode::Add, reg, Reg(REG_SP),
                                                  x->second));
      return reg;
    } else {
      auto found = reg_map.find(v);
      if (found != reg_map.end()) {
        // assert(arm::register_type(found->second) ==
        //        arm::RegisterKind::VirtualGeneralPurpose);
        return found->second;
      } else {
        auto reg = alloc_vgp();
        reg_map.insert({v, reg});
        return reg;
      }
    }
  }
}

arm::Reg Codegen::get_or_alloc_vd(mir::inst::VarId v) {
  // auto v = get_collapsed_var(v_);
  auto found = reg_map.find(v);
  if (found != reg_map.end()) {
    assert(arm::register_type(found->second) ==
           arm::RegisterKind::VirtualDoubleVector);
    return found->second;
  } else {
    auto reg = alloc_vd();
    reg_map.insert({v, reg});
    return reg;
  }
}

arm::Reg Codegen::get_or_alloc_vq(mir::inst::VarId v) {
  // auto v = get_collapsed_var(v_);
  auto found = reg_map.find(v);
  if (found != reg_map.end()) {
    assert(arm::register_type(found->second) ==
           arm::RegisterKind::VirtualQuadVector);
    return found->second;
  } else {
    return alloc_vq();
  }
}

void Codegen::scan() {
  for (auto& bb : func.basic_blks) {
    std::set<mir::inst::VarId> bb_var_use;
    for (auto& inst : bb.second.inst) {
      if (auto x = dynamic_cast<mir::inst::PhiInst*>(&*inst)) {
        deal_phi(*x);
      } else if (auto x = dynamic_cast<mir::inst::CallInst*>(&*inst)) {
        deal_call(*x);
      }
      bb_var_use.insert(inst->dest);
    }
    var_use.insert({bb.first, std::move(bb_var_use)});
  }

  // #pragma region CollapseShow
  //   LOG(TRACE) << "collapsing: " << std::endl;
  //   for (auto& v : var_collapse) {
  //     LOG(TRACE) << v.first << " -> " << v.second << std::endl;
  //   }
  //   LOG(TRACE) << std::endl;
  // #pragma endregion
}

void Codegen::deal_call(mir::inst::CallInst& call) {}

void Codegen::deal_phi(mir::inst::PhiInst& phi) {
  // auto set = std::set<mir::inst::VarId>();
  // auto dest = get_collapsed_var(phi.dest);
  // set.insert(dest);
  // for (auto& id : phi.vars) {
  // auto x = get_collapsed_var(id);
  // set.insert(x);
  // }
  auto dest = phi.dest;
  LOG(TRACE) << dest << " <- ";
  for (auto& x : phi.vars) {
    LOG(TRACE) << x << " ";
    var_collapse.insert({x, dest});
  }
  LOG(TRACE) << std::endl;
}

void Codegen::scan_stack() {
  for (auto& var_ : func.variables) {
    auto& id = var_.first;
    auto& var = var_.second;
    if (var.is_memory_var && var.ty->kind() != mir::types::TyKind::RestParam) {
      stack_space_allocation.insert({id, stack_size});
      stack_size += var.size();
    }
  }
}

arm::Reg Codegen::alloc_vgp() {
  return arm::make_register(arm::RegisterKind::VirtualGeneralPurpose,
                            vreg_gp_counter++);
}

arm::Reg Codegen::alloc_vd() {
  return arm::make_register(arm::RegisterKind::VirtualDoubleVector,
                            vreg_vd_counter++);
}

arm::Reg Codegen::alloc_vq() {
  return arm::make_register(arm::RegisterKind::VirtualQuadVector,
                            vreg_vq_counter++);
}

arm::Operand2 Codegen::translate_value_to_operand2(mir::inst::Value& v) {
  if (auto x = v.get_if<int32_t>()) {
    auto int_value = *x;
    if (is_valid_immediate(int_value)) {
      return Operand2(int_value);
    } else {
      auto reg = alloc_vgp();
      uint32_t imm_u = int_value;
      make_number(reg, imm_u);
      return Operand2(RegisterOperand(reg));
    }
  } else if (auto x = v.get_if<mir::inst::VarId>()) {
    return RegisterOperand(Reg(get_or_alloc_vgp(*x)));
  } else {
    throw new prelude::UnreachableException();
  }
}

arm::Reg Codegen::translate_value_to_reg(mir::inst::Value& v) {
  if (auto x = v.get_if<int32_t>()) {
    auto int_value = *x;
    auto reg = alloc_vgp();
    uint32_t imm_u = int_value;
    make_number(reg, imm_u);
    return reg;
  } else if (auto x = v.get_if<mir::inst::VarId>()) {
    return get_or_alloc_vgp(*x);
  } else {
    throw new prelude::UnreachableException();
  }
}

arm::Reg Codegen::translate_var_reg(mir::inst::VarId v) {
  auto r = get_or_alloc_vgp(v);
  return r;
}

arm::MemoryOperand Codegen::translate_var_to_memory_arg(mir::inst::Value& v_) {
  if (auto x = v_.get_if<int32_t>()) {
    throw new prelude::NotImplementedException();
  } else {
    auto x_ = v_.get_if<mir::inst::VarId>();
    return translate_var_to_memory_arg(*x_);
  }
}

arm::MemoryOperand Codegen::translate_var_to_memory_arg(mir::inst::VarId v_) {
  // auto v = get_collapsed_var(v_);
  auto v = v_;
  // If it's param, load before use
  if (v >= 4 && v <= param_size) {
    auto reg = alloc_vgp();
    return MemoryOperand(REG_FP, (v - 4) * 4);
  } else {
    auto x = stack_space_allocation.find(v);
    if (x != stack_space_allocation.end()) {
      auto reg = alloc_vgp();
      return MemoryOperand(REG_SP, x->second);
    } else {
      // If this is just an ordinary pointer type
      auto found = reg_map.find(v);
      if (found != reg_map.end()) {
        // assert(arm::register_type(found->second) ==
        //        arm::RegisterKind::VirtualGeneralPurpose);
        return found->second;
      } else {
        auto reg = alloc_vgp();
        reg_map.insert({v, reg});
        return reg;
      }
    }
  }
}

arm::MemoryOperand Codegen::translate_var_to_memory_arg(
    mir::inst::VarId v_, mir::inst::Value& offset) {
  // auto v = get_collapsed_var(v_);
  auto v = v_;
  // If it's param, load before use
  if (v >= 4 && v <= param_size) {
    auto reg = alloc_vgp();
    if (auto o = offset.get_if<int32_t>()) {
      return MemoryOperand(REG_FP, (v - 4) * 4 + *o);
    } else {
      auto o_ = offset.get_if<mir::inst::VarId>();
      throw prelude::NotImplementedException();
    }
  } else {
    auto x = stack_space_allocation.find(v);
    if (x != stack_space_allocation.end()) {
      auto reg = alloc_vgp();
      if (auto o = offset.get_if<int32_t>()) {
        return MemoryOperand(REG_SP, x->second + *o);
      } else {
        auto o_ = offset.get_if<mir::inst::VarId>();
        throw prelude::NotImplementedException();
      }
    } else {
      // If this is just an ordinary pointer type
      Reg reg = get_or_alloc_vgp(v);
      if (auto o = offset.get_if<int32_t>()) {
        return MemoryOperand(reg, *o);
      } else {
        auto o_ = offset.get_if<mir::inst::VarId>();
        return MemoryOperand(reg, RegisterOperand(get_or_alloc_vgp(*o_)));
      }
    }
  }
}

void Codegen::make_number(Reg reg, uint32_t num) {
  if ((~num) <= 0xffff) {
    inst.push_back(std::make_unique<Arith2Inst>(arm::OpCode::Mvn, reg, ~num));
  } else {
    inst.push_back(
        std::make_unique<Arith2Inst>(arm::OpCode::Mov, reg, num & 0xffff));
    if (num > 0xffff) {
      inst.push_back(
          std::make_unique<Arith2Inst>(arm::OpCode::MovT, reg, num >> 16));
    }
  }
}

void Codegen::translate_inst(mir::inst::AssignInst& i) {
  if (i.src.is_immediate()) {
    auto imm = *i.src.get_if<int32_t>();
    uint32_t imm_u = imm;
    make_number(translate_var_reg(i.dest), imm_u);
  } else {
    inst.push_back(std::make_unique<Arith2Inst>(
        arm::OpCode::Mov, translate_var_reg(i.dest),
        translate_value_to_operand2(i.src)));
  }
}

void Codegen::translate_inst(mir::inst::PhiInst& i) {
  // auto phi_reg = get_or_alloc_phi_reg(i.dest);
  // inst.push_back(std::make_unique<Arith2Inst>(
  //     arm::OpCode::Mov, translate_var_reg(i.dest),
  //     RegisterOperand(phi_reg)));
}

void Codegen::translate_inst(mir::inst::CallInst& i) {
  // TODO: make call stuff
  auto fn_id = i.func;
  auto f_ = package.functions.find(fn_id);
  if (f_ == package.functions.end()) {
    throw new FunctionNotFoundError{fn_id};
  }
  auto& f = *f_;
  auto params = f.second.type->params;
  uint32_t param_count = params.size();

  if (params.size() > 0 &&
      params.back().get()->kind() == mir::types::TyKind::RestParam) {
    // Deal with variable length params
    param_count = i.params.size();
  }

  auto stack_size = param_count > 4 ? param_count - 4 : 0;
  auto reg_size = param_count > 4 ? 4 : param_count;

  // TODO: Will not work with more than 4 params
  // Expand stack
  if (stack_size > 0) {
    inst.push_back(std::make_unique<Arith3Inst>(OpCode::Sub, REG_SP, REG_SP,
                                                stack_size * 4));
    inst.push_back(std::make_unique<CtrlInst>(
        STACK_OFFSET_CTRL, std::make_any<int32_t>(stack_size * 4)));
  }

  {
    // Push params
    for (int idx = i.params.size() - 1; idx >= 0; idx--) {
      auto& param = i.params[idx];
      if (idx < 4) {
        inst.push_back(std::make_unique<Arith2Inst>(
            OpCode::Mov, Reg(idx), translate_value_to_operand2(param)));
      } else {
        inst.push_back(std::make_unique<LoadStoreInst>(
            OpCode::StR, translate_value_to_reg(param),
            MemoryOperand(REG_SP, (int16_t)((idx - 4) * 4),
                          MemoryAccessKind::None)));
      }
    }
  }
  // Call instruction
  inst.push_back(
      std::make_unique<BrInst>(OpCode::Bl, f.second.name, param_count));

  // Shrink stack
  if (stack_size > 0) {
    inst.push_back(std::make_unique<CtrlInst>(
        STACK_OFFSET_CTRL, std::make_any<int32_t>(-(stack_size * 4))));
    inst.push_back(std::make_unique<Arith3Inst>(OpCode::Add, REG_SP, REG_SP,
                                                stack_size * 4));
  }

  // Move return value
  if (!(f.second.type->ret->kind() == mir::types::TyKind::Void))
    inst.push_back(std::make_unique<Arith2Inst>(
        OpCode::Mov, translate_var_reg(i.dest), RegisterOperand(0)));
}

void Codegen::translate_inst(mir::inst::StoreInst& i) {
  auto ins = std::make_unique<LoadStoreInst>(
      arm::OpCode::StR, translate_value_to_reg(i.val),
      translate_var_to_memory_arg(i.dest));
  inst.push_back(std::move(ins));
}

void Codegen::translate_inst(mir::inst::LoadInst& i) {
  auto ins = std::make_unique<LoadStoreInst>(
      arm::OpCode::LdR, translate_var_reg(i.dest),
      translate_var_to_memory_arg(i.src));
  inst.push_back(std::move(ins));
}

void Codegen::translate_inst(mir::inst::StoreOffsetInst& i) {
  auto ins = std::make_unique<LoadStoreInst>(
      arm::OpCode::StR, translate_value_to_reg(i.val),
      translate_var_to_memory_arg(i.dest));
  inst.push_back(std::move(ins));
}

void Codegen::translate_inst(mir::inst::LoadOffsetInst& i) {
  auto ins = std::make_unique<LoadStoreInst>(
      arm::OpCode::LdR, translate_var_reg(i.dest),
      translate_var_to_memory_arg(i.src));
  inst.push_back(std::move(ins));
}

void Codegen::translate_inst(mir::inst::RefInst& i) {
  if (auto x = std::get_if<std::string>(&i.val)) {
    auto global = package.global_values.find(*x);

    /*
     *    ldr	r0, .LCPI0_0
     *    add	r0, pc, r0
     *  .LPC0_0:             <- load_pc_label
     *  ...
     *  .LCPI0_0:            <- offset_from_pc_label
     *    .long	n-(.LPC0_0)
     */

    auto offset_from_pc_label = format_const_label(func.name, const_counter++);
    auto load_pc_label = format_load_pc_label(func.name, const_counter++);
    std::string local_const_definition;

    local_const_definition.append(*x);
    local_const_definition.append("-(");
    local_const_definition.append(load_pc_label);
    local_const_definition.append("+4)");

    consts.insert(
        {offset_from_pc_label,
         std::move(ConstValue(local_const_definition, ConstType::Word))});

    auto reg = get_or_alloc_vgp(i.dest);
    inst.push_back(std::make_unique<LoadStoreInst>(OpCode::LdR, reg,
                                                   offset_from_pc_label));
    inst.push_back(std::make_unique<Arith3Inst>(OpCode::Add, reg, REG_PC,
                                                RegisterOperand(reg)));
    inst.push_back(std::make_unique<LabelInst>(load_pc_label));

  } else if (auto x = std::get_if<mir::inst::VarId>(&i.val)) {
    auto reg = get_or_alloc_vgp(*x);
    auto reg1 = get_or_alloc_vgp(i.dest);
    inst.push_back(
        std::make_unique<Arith2Inst>(OpCode::Mov, reg1, RegisterOperand(reg)));
  }
}

void Codegen::translate_inst(mir::inst::PtrOffsetInst& i) {
  auto& ptr_ty = func.variables.at(i.ptr).ty;
  auto ptr_ty_ = static_cast<mir::types::PtrTy*>(&*ptr_ty);
  auto item_size = ptr_ty_->item->size().value();

  auto offset_value = i.offset;
  if (auto int_val = offset_value.get_if<int32_t>()) {
    *int_val *= item_size;
    inst.push_back(std::make_unique<Arith3Inst>(
        arm::OpCode::Add, translate_var_reg(i.dest), translate_var_reg(i.ptr),
        *int_val));
  } else if (auto var_val = offset_value.get_if<mir::inst::VarId>()) {
    auto num_reg = alloc_vgp();
    auto reg = alloc_vgp();
    inst.push_back(
        std::make_unique<Arith2Inst>(OpCode::Mov, num_reg, item_size));
    inst.push_back(std::make_unique<Arith3Inst>(OpCode::Mul, reg,
                                                translate_var_reg(*var_val),
                                                RegisterOperand(num_reg)));
    inst.push_back(std::make_unique<Arith3Inst>(
        arm::OpCode::Add, translate_var_reg(i.dest), translate_var_reg(i.ptr),
        RegisterOperand(reg)));
  }
}

inline bool can_reverse_param(mir::inst::Op op) {
  const std::set<mir::inst::Op> not_reversible = {
      mir::inst::Op::Div, mir::inst::Op::Rem, mir::inst::Op::Shl,
      mir::inst::Op::Shr, mir::inst::Op::ShrA};
  return not_reversible.find(op) == not_reversible.end();
}

void Codegen::translate_inst(mir::inst::OpInst& i) {
  bool reverse_params =
      i.lhs.is_immediate() && !i.rhs.is_immediate() && can_reverse_param(i.op);

  mir::inst::Value& lhs = reverse_params ? i.rhs : i.lhs;
  mir::inst::Value& rhs = reverse_params ? i.lhs : i.rhs;

  switch (i.op) {
    case mir::inst::Op::Add:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Add, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Sub:
      if (reverse_params) {
        inst.push_back(std::make_unique<Arith3Inst>(
            arm::OpCode::Rsb, translate_var_reg(i.dest),
            translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      } else {
        inst.push_back(std::make_unique<Arith3Inst>(
            arm::OpCode::Sub, translate_var_reg(i.dest),
            translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      }
      break;

    case mir::inst::Op::Mul:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Mul, translate_var_reg(i.dest),
          translate_value_to_reg(lhs),
          RegisterOperand(translate_value_to_reg(rhs))));
      break;

    case mir::inst::Op::Div:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::SDiv, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::And:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::And, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Or:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Orr, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Shl:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Lsl, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Shr:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Lsr, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::ShrA:
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::Asr, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;

    case mir::inst::Op::Rem:
      // WARN: Mod is a pseudo-instruction!
      // Mod operations needs to be eliminated in later passes.
      inst.push_back(std::make_unique<Arith3Inst>(
          arm::OpCode::_Mod, translate_var_reg(i.dest),
          translate_value_to_reg(lhs), translate_value_to_operand2(rhs)));
      break;
    case mir::inst::Op::Gt:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Gt, reverse_params);
      break;
    case mir::inst::Op::Lt:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Lt, reverse_params);
      break;
    case mir::inst::Op::Gte:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Ge, reverse_params);
      break;
    case mir::inst::Op::Lte:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Le, reverse_params);
      break;
    case mir::inst::Op::Eq:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::Equal, reverse_params);
      break;
    case mir::inst::Op::Neq:
      emit_compare(i.dest, i.lhs, i.rhs, ConditionCode::NotEqual,
                   reverse_params);
      break;
  }
}

void Codegen::emit_compare(mir::inst::VarId& dest, mir::inst::Value& lhs,
                           mir::inst::Value& rhs, arm::ConditionCode cond,
                           bool reversed) {
  auto lhsv = translate_value_to_reg(lhs);
  auto rhsv = translate_value_to_operand2(rhs);
  if (reversed) cond = reverse_cond(cond);

  inst.push_back(
      std::make_unique<Arith2Inst>(OpCode::Cmp, translate_value_to_reg(lhs),
                                   translate_value_to_operand2(rhs)));

  inst.push_back(std::make_unique<Arith2Inst>(
      OpCode::Mov, translate_var_reg(dest), Operand2(0)));
  inst.push_back(std::make_unique<Arith2Inst>(
      OpCode::Mov, translate_var_reg(dest), Operand2(1), cond));
}

void Codegen::emit_phi_move(std::unordered_set<mir::inst::VarId> i) {
  for (auto id : i) {
    auto collapsed = var_collapse.equal_range(id);
    if (collapsed.first == collapsed.second) {
      LOG(DEBUG) << id << " skipped" << std::endl;
      continue;
    }
    LOG(DEBUG) << id << std::endl;
    for (auto it = collapsed.first; it != collapsed.second; it++) {
      auto dest_reg = get_or_alloc_vgp(it->second);
      inst.push_back(std::make_unique<Arith2Inst>(
          OpCode::Mov, dest_reg, RegisterOperand(translate_var_reg(id))));
    }
  }
}

void Codegen::translate_branch(mir::inst::JumpInstruction& j) {
  switch (j.kind) {
    case mir::inst::JumpInstructionKind::Br:
      inst.push_back(std::make_unique<BrInst>(
          OpCode::B, format_bb_label(func.name, j.bb_true)));
      break;
    case mir::inst::JumpInstructionKind::BrCond: {
      auto back = inst.end() - 2;

      // Find if a comparison exists
      std::optional<ConditionCode> cond = std::nullopt;
      if (inst.size() >= 2) {
        auto b1 = back->get();
        auto b2 = (back + 1)->get();
        if (b1->op == arm::OpCode::Mov && b2->op == arm::OpCode::Mov) {
          auto b1m = dynamic_cast<Arith2Inst*>(b1);
          auto b2m = dynamic_cast<Arith2Inst*>(b2);
          if (b1m->r1 == b2m->r1 && b1m->r2 == 0 && b2m->r2 == 1 &&
              b1m->cond == arm::ConditionCode::Always &&
              b2m->cond != arm::ConditionCode::Always) {
            cond = {b2m->cond};
          }
        }
      }
      // TODO: Omit the second jump argument if label is right after it
      // TODO: Move this^ to peephole optimization
      if (cond) {
        inst.pop_back();
        inst.pop_back();
        inst.push_back(std::make_unique<BrInst>(
            OpCode::B, format_bb_label(func.name, j.bb_false),
            invert_cond(cond.value())));
        inst.push_back(std::make_unique<BrInst>(
            OpCode::B, format_bb_label(func.name, j.bb_true)));
      } else {
        inst.push_back(std::make_unique<Arith2Inst>(
            OpCode::Cmp, translate_var_reg(j.cond_or_ret.value()),
            Operand2(0)));
        inst.push_back(std::make_unique<BrInst>(
            OpCode::B, format_bb_label(func.name, j.bb_false),
            ConditionCode::Equal));
        inst.push_back(std::make_unique<BrInst>(
            OpCode::B, format_bb_label(func.name, j.bb_true)));
      }
    } break;
    case mir::inst::JumpInstructionKind::Return:
      if (j.cond_or_ret.has_value()) {
        // Move return value to its register
        // if (j.cond_or_ret.value().id != 0) {
        inst.push_back(std::make_unique<Arith2Inst>(
            OpCode::Mov, make_register(arm::RegisterKind::GeneralPurpose, 0),
            RegisterOperand(translate_var_reg(j.cond_or_ret.value()))));
        // }
      }
      inst.push_back(
          std::make_unique<BrInst>(OpCode::B, format_fn_end_label(func.name)));
      // Jumps to function end, the rest is cleanup generation in function
      // `generate_return_and_cleanup`
      break;
    case mir::inst::JumpInstructionKind::Undefined:
      // WARN: Error about undefined blocks
      throw new std::exception();
      break;
    case mir::inst::JumpInstructionKind::Unreachable:
      // TODO: Discard unreachable blocks beforhand
      break;
  }
}

}  // namespace backend::codegen

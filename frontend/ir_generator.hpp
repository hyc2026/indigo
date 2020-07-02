#pragma once

#include <vector>

#include "../mir/mir.hpp"
#include "symbol.hpp"

#ifndef COMPILER_FRONT_IR_GENERATOR_H_
#define COMPILER_FRONT_IR_GENERATOR_H_

namespace front::irGenerator
{
    using std::string;
    using std::get;
    using std::variant;
    using std::shared_ptr;
    using std::unique_ptr;
    using std::move;

    using mir::types::SharedTyPtr;
    using mir::types::LabelId;
    using mir::types::IntTy;
    using mir::types::VoidTy;
    using mir::types::ArrayTy;
    using mir::types::PtrTy;
    using mir::types::FunctionTy;

    using mir::inst::GlobalValue;
    using mir::inst::Variable;
    using mir::inst::VarId;
    using mir::inst::Value;

    typedef variant<int, LabelId, string> RightVal;
    typedef variant<LabelId, string> LeftVal;

    class WhileLabels
    {
    public:
        LabelId _beginLabel;
        LabelId _endLabel;

        WhileLabels()
        {
            _beginLabel = 0;
            _endLabel = 0;
        }
        WhileLabels(LabelId beginLabel, LabelId endLabel)
            : _beginLabel(beginLabel), _endLabel(endLabel) {}
    };

    class JumpLabelId
    {
    public:
        LabelId _jumpLabelId;

        JumpLabelId(LabelId id)
            : _jumpLabelId(id) {}
    };

    typedef std::variant<unique_ptr<mir::inst::Inst>, unique_ptr<mir::inst::JumpInstruction>, unique_ptr<JumpLabelId>> Instruction;

    class irGenerator
    {
    public:
        irGenerator();

        LabelId getNewLabelId();
        // as of now, the tmp for global scope is inserted into global_values of _package
        LabelId getNewTmpValueId();

        void ir_declare_value(string name, symbol::SymbolKind kind, int len = 0);
        void ir_declare_function(string name, symbol::SymbolKind kind);
        void ir_leave_function();
        void ir_declare_param(string name, symbol::SymbolKind kind);

        /* src type mean (src.index())
         * 0: jumplabel
         * 1: {global_values, local_values}
         */
        void ir_ref(LeftVal dest, LeftVal src);
        void ir_offset(LeftVal dest, LeftVal ptr, RightVal offset);
        void ir_load(LeftVal dest, RightVal src);
        void ir_store(LeftVal dest, RightVal src);
        void ir_op(LeftVal dest, RightVal op1, RightVal op2, mir::inst::Op op);
        void ir_assign(LeftVal dest, RightVal src);
        void ir_function_call(string retName, symbol::SymbolKind kind, string funcName, std::vector<RightVal> params);
        void ir_jump(mir::inst::JumpInstructionKind kind, LabelId bbTrue, LabelId bbFalse, 
            std::optional<string> condRetName, mir::inst::JumpKind jumpKind);
        void ir_label(LabelId label);

        void pushWhile(WhileLabels wl);
        WhileLabels checkWhile();
        void popWhile();

    private:
        mir::inst::MirPackage _package;
        std::map<LabelId, std::vector<Instruction>> _funcIdToInstructions;

        const LabelId _GlobalInitFuncId = 0;
        const LabelId _VoidVarId = (1 << 20);

        LabelId _nowFuncId;
        LabelId _nowLabelId;
        LabelId _nowGlobalValueId;
        LabelId _nowLocalValueId;

        std::map<string, LabelId> _globalValueNameToId;
        std::map<string, LabelId> _funcNameToId;
        std::map<string, LabelId> _localValueNameToId;

        std::vector<WhileLabels> _whileStack;
        std::vector<LabelId> _funcStack;

        // name of {local value, global value}
        LabelId nameToLabelId(string name);
        LabelId LeftValueToLabelId(LeftVal leftVal);
        shared_ptr<Value> rightValueToValue(RightVal& rightValue);
    };
}

#endif // !COMPILER_FRONT_IR_GENERATOR_H_

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTNativeTest.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <initializer_list>

namespace JSC { namespace AOT { namespace Typed {

template<typename Range, typename T>
bool hasElements(const Range& range, std::initializer_list<T> expected)
{
    return std::ranges::equal(range, expected);
}

inline ValueID addUnlistedValue(Function& function, Opcode opcode)
{
    Value value(opcode);
    value.type = TypeTable::anything;
    function.values.append(WTF::move(value));
    return ValueID(function.values.size() - 1);
}

inline ValueID addValue(Function& function, BlockID block, Opcode opcode, std::initializer_list<ValueID> operands = { })
{
    ValueID id = addUnlistedValue(function, opcode);
    function.at(block).values.append(id);
    uint32_t operandIndex = 0;
    for (ValueID operand : operands) {
        if (operandIndex < function.at(id).numberOfNamedOperands())
            function.at(id).setOperand(operandIndex, operand);
        else
            function.at(id).appendVariadicOperand(operand);
        ++operandIndex;
    }
    return id;
}

inline ValueID addParameter(Function& function, BlockID block, TypeID type = TypeTable::anything)
{
    ValueID id = addUnlistedValue(function, Opcode::Parameter);
    function.at(id).type = type;
    function.at(block).parameters.append(id);
    return id;
}

inline BlockID addBlock(Function& function)
{
    function.blocks.append(Block { });
    return BlockID(function.blocks.size() - 1);
}

inline void jump(Function& function, BlockID from, BlockID to, std::initializer_list<ValueID> arguments = { })
{
    Terminator& terminator = function.at(from).terminator;
    terminator.become(TerminatorKind::Jump);
    terminator.targets.append(Target { to, Arguments(arguments), { } });
}

inline void branch(Function& function, BlockID from, ValueID condition, BlockID taken, BlockID notTaken)
{
    Terminator& terminator = function.at(from).terminator;
    terminator.become(TerminatorKind::Branch);
    terminator.setOperand(0, condition);
    terminator.targets.append(Target { taken, { }, { } });
    terminator.targets.append(Target { notTaken, { }, { } });
}

inline Function createFunction(std::initializer_list<Opcode> opcodes)
{
    Function function;
    BlockID entry = addBlock(function);
    for (Opcode opcode : opcodes)
        addValue(function, entry, opcode);
    return function;
}

inline void typeFunctionAsRead(Program& program, FunctionID id, Function& function, BuildContext& build)
{
    TypeArena types(program.types);
    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, function);
    typeValuesFromNever({ program, id, function, analyses, types, build });
}

inline void typeProgramAsRead(Program& program, BuildContext& build)
{
    for (size_t index = 0; index < program.functions.size(); ++index) {
        Function& function = program.functions[index];
        if (function.hasBody())
            typeFunctionAsRead(program, FunctionID(index), function, build);
    }
}

struct TestProgram {
    TestProgram() { AOT_TEST_CHECK(program.strings.initialize(BuiltinTable::singleton(), loops, { }).error == StringTable::Error::None); }

    FunctionID addFunction(Function&& function)
    {
        program.functions.append(WTF::move(function));
        return FunctionID(program.functions.size() - 1);
    }

    ParallelLoops loops { 1 };
    Program program { BuiltinTable::singleton() };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

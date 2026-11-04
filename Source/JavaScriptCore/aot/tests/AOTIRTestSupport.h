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
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTTypeTable.h"
#include "AOTValueSubstitution.h"
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
    function.add(WTF::move(value));
    return ValueID(function.values.size() - 1);
}

inline ValueID addValue(Function& function, BlockID block, Opcode opcode, std::initializer_list<ValueID> operands = { })
{
    ValueID id = addUnlistedValue(function, opcode);
    function.edit(block).values.append(id);
    uint32_t operandIndex = 0;
    for (ValueID operand : operands) {
        if (operandIndex < function.at(id).numberOfNamedOperands())
            function.edit(id).setOperand(operandIndex, operand);
        else
            function.edit(id).appendVariadicOperand(operand);
        ++operandIndex;
    }
    return id;
}

inline ValueID addParameter(Function& function, BlockID block, TypeID type = TypeTable::anything)
{
    ValueID id = addUnlistedValue(function, Opcode::Parameter);
    function.edit(id).type = type;
    function.edit(block).parameters.append(id);
    return id;
}

inline BlockID addBlock(Function& function)
{
    function.addBlock(Block { });
    return BlockID(function.blocks.size() - 1);
}

inline void jump(Function& function, BlockID from, BlockID to, std::initializer_list<ValueID> arguments = { })
{
    Terminator& terminator = function.edit(from).terminator;
    terminator.become(TerminatorKind::Jump);
    terminator.targets.append(Target { to, Arguments(arguments), { } });
}

inline void branch(Function& function, BlockID from, ValueID condition, BlockID taken, BlockID notTaken)
{
    Terminator& terminator = function.edit(from).terminator;
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

// What TypeValues does with the requests of a typed function. Where the pass takes out a request that changes nothing, it stays here as a cast to the type it has: no value that a test names changes its number.
inline void resolveRequestsInPlace(Program& program, FunctionID id, Function& function, BuildContext& build)
{
    TypeArena types(program.types);
    TypingContext typing { program, function, types };
    bool didResolve = false;
    for (const Block& block : function.blocks) {
        for (ValueID request : block.values) {
            if (function.at(request).opcode != Opcode::Convert)
                continue;
            ValueID operand = function.at(request).valueOperand();
            if (!operand || !function.at(operand).type)
                continue;
            DeclaredType declared = function.at(request).declared();
            if (!becomeResolved(function, request, resolveConvert(typing, typing.typeOf(operand), declared)))
                becomeUpcast(function, request, operand);
            didResolve = true;
        }
    }
    if (!didResolve)
        return;
    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, function);
    finishTypedValues({ program, id, function, analyses, types, build });
}

inline void typeProgramAsRead(Program& program, BuildContext& build)
{
    for (size_t index = 0; index < program.functions.size(); ++index) {
        Function& function = program.functions[index];
        if (!function.hasBody())
            continue;
        typeFunctionAsRead(program, FunctionID(index), function, build);
        resolveRequestsInPlace(program, FunctionID(index), function, build);
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

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

enum class IterationHelper : uint8_t {
    OpenIterator,
    OpenWrappedIterator,
    StepIterator,
    WayToStepIterator,
    StepIteratorInWay,
    WayToStepMatches,
    StepMatchesInWay,
    CloseIterator,
    CloseIteratorAfterThrow,
    AppendGenerator,
    AppendIterable,
    TakeFromIterable,
    NextOfStepsOverText,
};

inline BuiltinID builtinOfIterationHelper(IterationHelper helper)
{
    switch (helper) {
    case IterationHelper::OpenIterator:
        return Builtins::CompilerHelpersOpenIterator;
    case IterationHelper::OpenWrappedIterator:
        return Builtins::CompilerHelpersOpenWrappedIterator;
    case IterationHelper::StepIterator:
        return Builtins::CompilerHelpersStepIterator;
    case IterationHelper::WayToStepIterator:
        return Builtins::CompilerHelpersWayToStepIterator;
    case IterationHelper::StepIteratorInWay:
        return Builtins::CompilerHelpersStepIteratorInWay;
    case IterationHelper::WayToStepMatches:
        return Builtins::CompilerHelpersWayToStepMatches;
    case IterationHelper::StepMatchesInWay:
        return Builtins::CompilerHelpersStepMatchesInWay;
    case IterationHelper::CloseIterator:
        return Builtins::CompilerHelpersCloseIterator;
    case IterationHelper::CloseIteratorAfterThrow:
        return Builtins::CompilerHelpersCloseIteratorAfterThrow;
    case IterationHelper::AppendGenerator:
        return Builtins::CompilerHelpersAppendGenerator;
    case IterationHelper::AppendIterable:
        return Builtins::CompilerHelpersAppendIterable;
    case IterationHelper::TakeFromIterable:
        return Builtins::CompilerHelpersTakeFromIterable;
    case IterationHelper::NextOfStepsOverText:
        return Builtins::CompilerHelpersNextOfStepsOverText;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline bool isWrittenMatchAll(const Program& program, const BodyValue& written)
{
    BuiltinName matchAll = written && written.opcode() == Opcode::Call ? program.builtins().nameOf(byteCast<uint8_t>(("matchAll"_s).span())) : BuiltinName();
    return matchAll && written.calleeKey() == program.keyOf(matchAll);
}

inline FunctionID functionOfIterationHelper(const Program& program, IterationHelper helper)
{
    for (const BuiltinOverload& overload : program.builtins().overloadsOf(builtinOfIterationHelper(helper))) {
        RuntimeFunctionID id = overload.libraryFunction();
        if (overload.mode != BuiltinCallMode::Call || !id)
            continue;
        const RuntimeFunctionEntry* held = findRuntimeFunction(program, id);
        return held && held->function && program.at(held->function).hasBody() ? held->function : FunctionID();
    }
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

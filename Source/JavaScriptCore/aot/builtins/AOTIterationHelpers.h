/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

// The functions of the library that do a part of the protocol of iteration for every loop of which nothing is known: src/aot/prelude/iterator-consumers.ts in Bun.
// The walk calls them in place of writing the protocol out, and the summary of every IteratorOpen, IteratorResume and IteratorClose states that they are entered.
enum class IterationHelper : uint8_t {
    // (iterable): the iterator, or undefined where the iterable has no method to call.
    OpenIterator,
    // (iterable): the wrapper that `for await` steps where the iterable has a synchronous iterator alone.
    OpenWrappedIterator,
    // (iterator, next): what the iterator hands out, or CollectionIteratorClosedMark where it is done.
    StepIterator,
    // (iterator)
    CloseIterator,
    CloseIteratorAfterThrow,
    // (list, generator): whatever the generator hands out goes behind the elements of the list, which a spread fills and no code of the program has seen.
    AppendGenerator,
    // (list, iterable): the same, of an object of the program's own, by the protocol as a spread goes by it. `iterable` is neither undefined nor null.
    AppendIterable,
    // (iterable, count): a list of what the first `count` steps hand out, fewer where the iterator is done before; what is not done is closed. As a pattern of `count` places goes by the protocol. `count` is not 0.
    TakeFromIterable,
    // (this: what openIterator() makes in the place of the iterator of a text): the `next` of that. No program ever holds it, so whatever has it under `next` is what openIterator() has made.
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

// Null: the program does not hold the function.
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

inline bool isIterationHelper(const Program& program, FunctionID function)
{
    for (IterationHelper helper : { IterationHelper::OpenIterator, IterationHelper::OpenWrappedIterator, IterationHelper::StepIterator, IterationHelper::CloseIterator, IterationHelper::CloseIteratorAfterThrow, IterationHelper::AppendGenerator, IterationHelper::AppendIterable, IterationHelper::TakeFromIterable, IterationHelper::NextOfStepsOverText }) {
        if (functionOfIterationHelper(program, helper) == function)
            return true;
    }
    return false;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

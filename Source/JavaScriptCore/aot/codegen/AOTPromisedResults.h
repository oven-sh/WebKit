/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTThreads.h"
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class CallingConventions;

// The type of a result is declared, and nothing checks what the rare paths of a function return, so every caller tests what it gets.
// A function that promises its result tests for itself what it cannot prove, on the paths that return it. What fails leaves through the stub ReturnMisfit,
// which goes on at the landing that the call site has for it. So a return says that the result has the type, and a caller that relies on it tests nothing.
class PromisedResults {
public:
    PromisedResults() = default;
    PromisedResults(Vector<TypeID>&& types, BitVector&& functionsWithExactMisfits)
        : m_types(WTF::move(types))
        , m_functionsWithExactMisfits(WTF::move(functionsWithExactMisfits))
    {
    }

    // No type where the function promises nothing.
    TypeID of(FunctionID function) const { return function.index() < m_types.size() ? m_types[function.index()] : TypeID(); }
    // A function that ends in a tail call leaves its return to a callee, which may promise less than the function does. Where none does, what arrives at a landing is not of the type.
    bool misfitsAreNeverOfType(FunctionID function) const { return m_functionsWithExactMisfits.get(function.index()); }

    // The call, where the block ends with a direct call and a test of its result for a type that the callee promises.
    ValueID findCallThatStandsForTest(const Program&, const Function&, const Block&) const;

private:
    Vector<TypeID> m_types;
    BitVector m_functionsWithExactMisfits;
};

PromisedResults findPromisedResults(const Program&, const CallingConventions&, const ParallelLoops&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

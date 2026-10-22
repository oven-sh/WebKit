/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowResultsInternal.h"

namespace JSC { namespace AOT { namespace Typed {

// Where the program names a built-in, and through which of its statements it comes to a place in the library. It walks every statement of the reached functions, so it is made for a report alone.
class ValueFlowPlacesInProgram {
    WTF_MAKE_NONCOPYABLE(ValueFlowPlacesInProgram);
public:
    // By canonical entry.
    ValueFlowPlacesInProgram(const ValueFlowResultInputs&, const BitVector& builtinsAskedAbout);

    // The first statement in the text of the program that has the built-in as its result or as a constant operand, or that asks for the module whose namespace object it is.
    // Where only the library names it, the way to that place. A built-in that has escaped and that nothing names came out of a read under a key nobody knows: the first such read. None for any other.
    Witness whereNamed(BuiltinID) const;
    // The place itself if it is written in the program. Of a place in the library the nearest call, import or require of the program that leads there, the first in the text among equals. None where nobody spells the way out.
    Witness wayTo(const Witness&) const;

private:
    const Program& m_program;
    Vector<Witness> m_namedInProgram;
    Vector<Witness> m_namedInLibrary;
    BitVector m_hasEscaped;
    Witness m_readUnderKeyNobodyKnows { noWitness };
    // By function of the library.
    Vector<Witness> m_wayInto;
};

// The locales that the calls of the built-ins whose code runs name. One that is no constant, and one that is left to the host, keeps the data of every language.
Vector<FlowLocaleUse> collectLocaleUses(const ValueFlowResultInputs&, ValueFlowHeapView&, const BitVector& builtinsWhoseCodeRuns, std::span<const UnseenCodeRecord>);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

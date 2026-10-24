/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowResultsInternal.h"

namespace JSC { namespace AOT { namespace Typed {

class ValueFlowPlacesInProgram {
    WTF_MAKE_NONCOPYABLE(ValueFlowPlacesInProgram);
public:
    ValueFlowPlacesInProgram(const ValueFlowResultInputs&, const BitVector& builtinsAskedAbout);

    Witness whereNamed(BuiltinID) const;
    Witness wayTo(const Witness&) const;

private:
    const Program& m_program;
    Vector<Witness> m_namedInProgram;
    Vector<Witness> m_namedInLibrary;
    BitVector m_hasEscaped;
    Witness m_readUnderKeyNobodyKnows { noWitness };
    Vector<Witness> m_wayInto;
};

class ValueFlowNamedBuiltins {
    WTF_MAKE_NONCOPYABLE(ValueFlowNamedBuiltins);
public:
    struct Member {
        BuiltinID builtin;
        BuiltinID candidate;
    };

    ValueFlowNamedBuiltins(const ValueFlowResultInputs&, const BitVector& candidates, std::span<const Member>);

    bool contains(BuiltinID builtin) const { return m_named.quickGet(builtin.index()); }

private:
    BitVector m_named;
};

Vector<FlowLocaleUse> collectLocaleUses(const ValueFlowResultInputs&, ValueFlowHeapView&, const BitVector& builtinsWhoseCodeRuns, std::span<const UnseenCodeRecord>);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <wtf/Lock.h>

namespace JSC { namespace AOT { namespace Typed {

class Arrivals {
    WTF_MAKE_NONCOPYABLE(Arrivals);
public:
    Arrivals(const Program&, const Facts&, TypeTable&);

    void leave(TypeArena&, FunctionID callee, uint32_t entryParameter, TypeID proven);
    void leaveUndefinedFrom(TypeArena&, FunctionID callee, uint32_t firstMissingArgument);
    void leaveAnything(FunctionID callee);

    TypeID provenAt(FunctionID, uint32_t entryParameter) const;
    TypeID askedByDoorsOfCapture(TypeArena&, FunctionID, uint32_t entryParameter) const;
    TypeID provenAt(TypeArena&, FunctionID, uint32_t entryParameter) const;
    TypeID leftAt(TypeArena&, FunctionID, uint32_t entryParameter) const;
    bool takesWhatWasLeft(FunctionID) const;
    void closeEntriesOf(FunctionID function) { m_entriesAreClosed[function.index()] = 1; }
    bool entriesAreClosed(FunctionID function) const { return m_entriesAreClosed[function.index()]; }
    const Facts& facts() const { return m_facts; }

private:
    const Program& m_program;
    const Facts& m_facts;
    TypeTable& m_types;
    Vector<TypeID> m_joined;
    Vector<uint8_t> m_entriesAreClosed;
    mutable Vector<Lock> m_locks;
    mutable TypeArena m_joins;
    mutable Lock m_joinsLock;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

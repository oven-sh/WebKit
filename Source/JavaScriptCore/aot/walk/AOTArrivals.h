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

// The walk is the pass that goes down. A walk leaves, at each call that Link lists with one callee, what it had proven of each argument.
// A function is walked behind all who may name it, and takes the join of what they left as proven.
//
// IT IS SOUND ONLY BECAUSE EVERY CALL THAT MAY ENTER THE FUNCTION LEAVES SOMETHING:
//   a call built as a direct call leaves the types of its arguments . a listed call built in any other form leaves anything (leaveAnything()) .
//   a call whose callee was copied in line, and a call in a block that is never walked, enter nothing and leave nothing.
// Walker::walkOperation() sees to it in one place. No handler has to remember.
//
// A join is commutative and a function starts only when its referrers are done, so the answer depends on no order of threads.
class Arrivals {
    WTF_MAKE_NONCOPYABLE(Arrivals);
public:
    Arrivals(const Program&, const Facts&, TypeTable&);

    // `proven` may lie in the arena of the calling thread: it is committed to the program's table here. Any thread.
    void leave(TypeArena&, FunctionID callee, uint32_t entryParameter, TypeID proven);
    // A caller passes fewer arguments than the callee has parameters: undefined arrives at the rest.
    // That holds of the positional parameters. A list arrives at a rest parameter, so of whatever else is missing nothing is proven.
    void leaveUndefinedFrom(TypeArena&, FunctionID callee, uint32_t firstMissingArgument);
    // The call may pass anything at all: nothing is proven of any parameter of the callee from here on.
    void leaveAnything(FunctionID callee);

    // Asked only when the cycles of all its referrers are done. Null: nothing is proven.
    //   LinkedFunction::everyCallerIsKnown and everyCallHasOneCallee, no referrer in its own cycle, and something was left: the join of what was left.
    //   everyCallerIsKnown otherwise: LinkedParameter::proven, which holds of every caller.
    //   Else null.
    TypeID provenAt(FunctionID, uint32_t entryParameter) const;
    // The same for a thread that has an arena: the join as it stands, made in that arena where the program's table lacks it.
    // Code takes the type of an entry parameter when the parameter is made and never again, so whoever makes it asks here.
    TypeID provenAt(TypeArena&, FunctionID, uint32_t entryParameter) const;
    // The first case of provenAt(). False: the code of the function hangs on nothing that a walk leaves, so it may be walked at any time. The queue of Compile goes by that.
    bool takesWhatWasLeft(FunctionID) const;

private:
    const Program& m_program;
    const Facts& m_facts;
    TypeTable& m_types;
    // By LinkedFunction::firstParameter + the parameter. Null: nothing was left. TypeTable::anything: whatever is left from here on changes nothing.
    // An id that liesInThread() lies in m_joins.
    Vector<TypeID> m_joined;
    // By function: its part of m_joined.
    Vector<Lock> m_locks;
    // The program's table is frozen in front of Compile and takes no type. A join that it lacks is kept here, whole, so that the join of all that is left follows from what is left and from no order.
    // It is never reset. Taken behind a lock of m_locks, never in front of one.
    mutable TypeArena m_joins;
    mutable Lock m_joinsLock;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

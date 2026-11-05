/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSlotPlan.h"
#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// ---- AOTWalkBirthsListed.cpp
// Two links of the chain in front of buildFreshObject(), each behind `has(Mechanism::EscapeAnalysis) && m_opensGuesses`. The allocation lists what the stores behind it lay, and the stores build nothing. False: nothing was built.
// `new Node(kind, pos, end)` of a constructor function whose body is copied.
inline constexpr OpcodeSet opcodesThatListStoresOfCopiedConstructorTakes { Opcode::Construct };
AOT_LINK_OF_WALK(listStoresOfCopiedConstructor, opcodesThatListStoresOfCopiedConstructorTakes)
// `const o = { a }; o.b = b;`
inline constexpr OpcodeSet opcodesThatListStoresBehindLiteralTakes { Opcode::NewObject };
AOT_LINK_OF_WALK(listStoresBehindLiteral, opcodesThatListStoresBehindLiteralTakes)

// For buildStoreForTypedAccess(), of a store into a filled slot. `held`: what the state has for the slot, null where it has nothing. True: nothing but this code has the object, and the store would lay the very value that lies there.
bool storeLaysWhatSlotOfFreshObjectHolds(Walker&, Reference object, Reference held, Reference stored);

// For whoever has made an object and has run its constructor. False: the mark answers for no field of any layout that the object may have. It is then set only where a field has left its slot for good, and no look clears it.
bool lookMayFinishBirthOf(Walker&, Reference object);
// A birth ends at each Return of the constructor of a class, in the unit of the constructor and where its body is copied. Nothing where the walk has seen the birth end.
void finishBirthAtReturnOfClassConstructor(Walker&, Reference returned, Origin);

// ---- AOTWalkTypedAccesses.cpp, which has the questions that a store by name asks.
struct StoresListedAtBirth {
    // As many as may be listed, from the first store on. Empty: none.
    Vector<FieldIndex, 16> fields;
    // What a cell that was handed its prototype has to show behind the allocation.
    bool asksForLayoutPrototype { false };
    Vector<MemberConditionRecord, 8> conditions;
    // PlacedAllocation::isBornUnfinished of the allocation that lists them.
    bool isBornUnfinished { false };
};
// `keys`: the names under which the first assignments to a cell of the layout that is made here go, one behind the other. `stored`: the proven type of what each brings.
// Listed, each is what the assignment leaves: a plain property of the object's own in its slot in the cell, in the order of the assignments, for which the image has a word.
// That holds while the cell stands below the prototype object of its layout and the conditions hold. It builds nothing.
// `keysStoredFurtherOn`: names under which the object is stored into later, in any order. The conditions speak of those too, wherever a guard of a class answers for one.
StoresListedAtBirth findStoresThatAllocationMayList(Walker&, LayoutID, std::span<const PropertyKeyID> keys, std::span<const TypeID> stored, std::span<const PropertyKeyID> keysStoredFurtherOn);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

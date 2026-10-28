/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTExpandOwnKeysOrders.h"
#include "AOTPass.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesOfWalks { Opcode::CopyDataProperties, Opcode::CopyObject, Opcode::CallBuiltin };
inline constexpr OpcodeSet opcodesOfKeyOrderTests { Opcode::HasKeyOrder, Opcode::HasNoExtraProperties };
// ExpandOwnKeys has nothing to do in a function without any of these.
inline constexpr OpcodeSet ownKeyExpansionOpcodes = opcodesOfWalks | opcodesOfKeyOrderTests | OpcodeSet { Opcode::ForInStep };

enum class OwnKeyWalk : uint8_t { Keys, Values, Entries, Assign, Spread, Copy, Stringify, StringifyIntoText };

struct OwnKeyWalkSite {
    OwnKeyWalk walk { OwnKeyWalk::Values };
    BlockID block;
    uint32_t index { 0 };
    ValueID value;
    // What flow says of the list that the walk makes, where it says something.
    TypeID provenTypeOfList;
    ValueID source;
    ValueID target;
    uint32_t sourceArgument { 0 };
    BuiltinID textForm;
    Vector<ValueID, 1> laterSources;
};

struct OwnKeyWalkArm {
    LayoutID layout;
    KeyOrderFields order;
    // The target of the copy is a new object of this layout that has no property yet: it takes the slots and the key order of the source as they are, whatever the order. The copy tests the source itself.
    bool copiesFieldsAtOnce { false };
};

struct OwnKeyWalkPlan {
    Vector<OwnKeyWalkArm, 4> arms;
    bool needsShapeTest { true };
    bool needsKeyOrderTest { true };
    bool needsOriginalCall { true };
    // Set when the plan needs no test because the source is this allocation and nothing has changed it. It is the guard of the reads.
    ValueID unchangedAllocation;
};

struct AllocationAtPoint {
    ValueID allocation;
    ProgramPoint point;
};

// Answers for the sources and targets of all the sites of a function with one solve.
class UnchangedAllocations {
public:
    UnchangedAllocations(FunctionPassContext&, std::span<const OwnKeyWalkSite>);

    bool contains(ValueID allocation, ProgramPoint) const;

private:
    FunctionPassContext& m_context;
    Vector<AllocationAtPoint, 8> m_uses;
    mutable std::optional<BitVector> m_unchanged;
};

std::optional<OwnKeyWalkPlan> planOwnKeyWalk(FunctionPassContext&, const UnchangedAllocations&, const OwnKeyWalkSite&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

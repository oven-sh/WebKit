/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "AOTStubsPropertyProbes.h"
#include <algorithm>
#include <array>
#include <span>

namespace JSC { namespace AOT {

inline constexpr std::array mostCommonFieldRepresentations { SlotRepresentation::JSValue, SlotRepresentation::Cell32, SlotRepresentation::OptionalInt32, SlotRepresentation::OptionalCell32 };

inline constexpr auto representationsInTestOrder = [] {
    std::array<SlotRepresentation, numberOfEnumerators<SlotRepresentation>> order { };
    size_t count = 0;
    for (SlotRepresentation representation : mostCommonFieldRepresentations)
        order[count++] = representation;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        if (std::ranges::find(mostCommonFieldRepresentations, representation) == mostCommonFieldRepresentations.end())
            order[count++] = representation;
    }
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(count == order.size());
    return order;
}();

template<typename EmitBody>
void emitSearchForRepresentation(StubAssembler& jit, GPRReg representation, std::span<const SlotRepresentation> ascending, const EmitBody& emitBody)
{
    if (ascending.size() == 1) {
        emitBody(ascending[0]);
        return;
    }
    size_t middle = ascending.size() / 2;
    CCallHelpers::Jump isInUpperHalf = jit.branch32(CCallHelpers::AboveOrEqual, representation, CCallHelpers::TrustedImm32(static_cast<int32_t>(ascending[middle])));
    emitSearchForRepresentation(jit, representation, ascending.first(middle), emitBody);
    isInUpperHalf.link(&jit);
    emitSearchForRepresentation(jit, representation, ascending.subspan(middle), emitBody);
}

// emitBody emits the code for one representation, which never falls through. The most common ones cost one comparison each, any other one four or five more.
template<typename EmitBody>
void emitDispatchOnRepresentation(StubAssembler& jit, GPRReg representation, const EmitBody& emitBody)
{
    constexpr size_t numberOfCommon = mostCommonFieldRepresentations.size();
    static_assert(std::ranges::is_sorted(std::span(representationsInTestOrder).subspan(numberOfCommon)));
    for (SlotRepresentation common : mostCommonFieldRepresentations) {
        CCallHelpers::Jump isOther = jit.branch32(CCallHelpers::NotEqual, representation, CCallHelpers::TrustedImm32(static_cast<int32_t>(common)));
        emitBody(common);
        isOther.link(&jit);
    }
    emitSearchForRepresentation(jit, representation, std::span(representationsInTestOrder).subspan(numberOfCommon), emitBody);
}

// extensionWordOffset may be address. slotData is dead after computeFieldAddress: a caller may pass it again as a scratch register.
struct FieldAddressRegisters {
    GPRReg object;
    GPRReg slotData;
    GPRReg extensionWordOffset;
    GPRReg address;
    GPRReg representation;
};

// result may be field.object: it is written after the last exit.
struct FieldReadRegisters {
    FieldAddressRegisters field;
    GPRReg value;
    GPRReg result;
    FPRReg number;
    FPRReg numberScratch;
};

struct FieldReadExits {
    JumpList isAbsent;
    JumpList cannot;
};

struct FieldStoreRegisters {
    FieldAddressRegisters field;
    GPRReg value;
    GPRReg type;
    GPRReg scratch;
    GPRReg secondScratch;
    FPRReg number;
};

struct FieldStoreExits {
    JumpList wasAbsent;
    JumpList wasAbsentAndIsNotAccepted;
    JumpList cannot;
};

enum class FieldStoreEnd : uint8_t { ReturnAfterBarrier, JoinList };
enum class FieldTypeTest : bool { IsEmitted, HasPassed };

struct TypeTestRegisters {
    GPRReg value;
    GPRReg type;
    GPRReg test;
    GPRReg scratch;
    GPRReg secondScratch;
};

struct KeyOrderRegisters {
    GPRReg object;
    GPRReg slotKey;
    GPRReg entry;
    GPRReg words;
    GPRReg scratch;
};

void loadExtensionWordOffset(StubAssembler&, GPRReg object, GPRReg result);
void computeFieldAddress(StubAssembler&, const FieldAddressRegisters&, bool isOutOfLine, JumpList& hasNoStorage);
void readFieldAndReturn(StubAssembler&, const FieldReadRegisters&, FieldReadExits&);
// Nothing falls through. Keeps address and representation.
void branchOnFieldPresence(StubAssembler&, const FieldAddressRegisters&, GPRReg scratch, JumpList& isPresent, JumpList& isAbsent);
// Returns 1 for a field in its slot and 0 for an absent one under Slot::absentIsUndefined. Keeps object at isUnknown.
void returnFieldPresence(StubAssembler&, const CachedSlot&, const FieldAddressRegisters&, JumpList& isUnknown);
// Falls through when accepted. Reads bits 0 to 31 of type. Keeps type and value on every path.
void branchUnlessTypeAccepts(StubAssembler&, const TypeTestRegisters&, JumpList& isRejected, JumpList& cannot);
// At wasAbsent the type accepts the value, and every register but scratch and secondScratch is as on entry. Under ReturnAfterBarrier cannot means the field is in its slot.
void storeField(StubAssembler&, const FieldStoreRegisters&, FieldStoreEnd, FieldTypeTest, JumpList* stored, FieldStoreExits&);
// Falls through with the new order word stored. Keeps object and slotKey, and writes nothing to memory before it jumps to hasNoTransition.
void appendFieldToKeyOrder(StubAssembler&, const KeyOrderRegisters&, JumpList& hasNoTransition);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

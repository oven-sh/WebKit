/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSlotRepresentation.h"
#include "AOTStubFrontEnds.h"
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

// A read does not tell apart what it reads with the same code: whole words that are encoded values, narrow references, and 32-bit integers. They come first in the enumeration, so each pair costs one comparison.
inline constexpr std::array representationsReadAsEncodedValues { SlotRepresentation::JSValue, SlotRepresentation::Cell };
inline constexpr std::array representationsReadAsNarrowReferences { SlotRepresentation::Cell32, SlotRepresentation::OptionalCell32 };
inline constexpr std::array representationsReadAsInt32 { SlotRepresentation::Int32, SlotRepresentation::OptionalInt32 };
static_assert(static_cast<unsigned>(representationsReadAsEncodedValues[0]) == 0 && static_cast<unsigned>(representationsReadAsEncodedValues[1]) == 1);
static_assert(static_cast<unsigned>(representationsReadAsNarrowReferences[0]) == 2 && static_cast<unsigned>(representationsReadAsNarrowReferences[1]) == 3);
static_assert(static_cast<unsigned>(representationsReadAsInt32[0]) == 4 && static_cast<unsigned>(representationsReadAsInt32[1]) == 5);

inline constexpr auto representationsReadOneByOne = [] {
    std::array<SlotRepresentation, numberOfEnumerators<SlotRepresentation> - 6> rest { };
    size_t count = 0;
    for (SlotRepresentation representation : allMembersOf<SlotRepresentation>) {
        if (representation > representationsReadAsInt32.back())
            rest[count++] = representation;
    }
    RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(count == rest.size());
    return rest;
}();

// emitBody is given the member of a pair whose code reads both.
template<typename EmitBody>
void emitDispatchOnRepresentationForRead(StubAssembler& jit, GPRReg representation, const EmitBody& emitBody)
{
    static_assert(std::ranges::is_sorted(representationsReadOneByOne));
    auto immediate = [](SlotRepresentation value) { return CCallHelpers::TrustedImm32(static_cast<int32_t>(value)); };
    CCallHelpers::Jump isNoEncodedValue = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsEncodedValues.back()));
    emitBody(SlotRepresentation::JSValue);
    isNoEncodedValue.link(&jit);
    CCallHelpers::Jump isNoNarrowReference = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsNarrowReferences.back()));
    emitBody(SlotRepresentation::OptionalCell32);
    isNoNarrowReference.link(&jit);
    CCallHelpers::Jump isNoInt32 = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsInt32.back()));
    emitBody(SlotRepresentation::OptionalInt32);
    isNoInt32.link(&jit);
    emitSearchForRepresentation(jit, representation, std::span(representationsReadOneByOne), emitBody);
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

struct FieldOfLayout {
    GPRReg layout;
    GPRReg fieldIndex;
};

void loadExtensionWordOffset(StubAssembler&, GPRReg object, GPRReg result);
// Keeps object and slotData, at hasNoStorage too. Out of line it also takes the scratch register of the assembler and fpTempRegister.
void computeFieldAddress(StubAssembler&, const FieldAddressRegisters&, bool isOutOfLine, JumpList& hasNoStorage);
void readFieldAndReturn(StubAssembler&, const FieldReadRegisters&, FieldReadExits&);
// The slot of an accessor field holds its GetterSetter, as a boxed value, a cell or a narrow reference. address and representation are what computeFieldAddress gave. result may be either.
void loadGetterSetterInField(StubAssembler&, GPRReg address, GPRReg representation, GPRReg result, JumpList& isAbsent);
// Nothing falls through. Keeps address and representation.
void branchOnFieldPresence(StubAssembler&, const FieldAddressRegisters&, GPRReg scratch, JumpList& isPresent, JumpList& isAbsent);
// Returns 1 for a field in its slot and 0 for an absent one under Slot::absentIsUndefined. Keeps object at isUnknown.
void returnFieldPresence(StubAssembler&, const CachedSlot&, const FieldAddressRegisters&, JumpList& isUnknown);
// Falls through when accepted. Reads bits 0 to 31 of type. Keeps type and value on every path.
void branchUnlessTypeAccepts(StubAssembler&, const TypeTestRegisters&, JumpList& isRejected, JumpList& cannot);
// At wasAbsent the slot admits the value: the type accepts it, or it is undefined or null and the representation has a marker for it.
void storeField(StubAssembler&, const FieldStoreRegisters&, FieldStoreEnd, FieldTypeTest, JumpList* stored, FieldStoreExits&);
// Falls through with the new order word stored. Keeps object and slotKey, and writes nothing to memory before it jumps to hasNoTransition. Of a known field fieldIndex is kept and layout is written.
void appendFieldToKeyOrder(StubAssembler&, const KeyOrderRegisters&, JumpList& hasNoTransition, std::optional<FieldOfLayout> known = std::nullopt);
// Behind the store that creates a field: lists its key, in the stub or by the run time, and returns behind the barrier.
void listCreatedFieldAndReturn(StubAssembler&, const KeyOrderRegisters&, GPRReg value, std::optional<FieldOfLayout> known = std::nullopt);
// Gives the field of slotData its granule, in the stub or by the run time, and goes on at findStorage. Writes address, representation and secondScratch.
void createGranuleOfField(StubAssembler&, const FieldStoreRegisters&, const ScopedLambda<void()>& loadOffsetOfExtensionWord, Label findStorage);
PreludeExit emitPrivateDefinePrelude(StubAssembler&);
PreludeExit emitPrivateSetPrelude(StubAssembler&);

inline constexpr uint32_t offsetBitsWithoutSlotKey = SlotData::maxOffset & ~(outOfLineSlotKey - 1);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

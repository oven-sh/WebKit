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

constexpr SlotRepresentation representationWithCodeForRead(SlotRepresentation representation)
{
    if (representation <= representationsReadAsEncodedValues.back())
        return SlotRepresentation::JSValue;
    if (representation <= representationsReadAsNarrowReferences.back())
        return SlotRepresentation::OptionalCell32;
    if (representation <= representationsReadAsInt32.back())
        return SlotRepresentation::OptionalInt32;
    return representation;
}

// AreReadElsewhere: the caller has told apart the encoded values, the narrow references and the 32-bit integers, and none comes here.
enum class CommonRepresentations : bool { AreRead, AreReadElsewhere };

// emitBody is given the member of a pair whose code reads both.
template<typename EmitBody>
void emitDispatchOnRepresentationForRead(StubAssembler& jit, GPRReg representation, CommonRepresentations commonRepresentations, const EmitBody& emitBody)
{
    static_assert(std::ranges::is_sorted(representationsReadOneByOne));
    auto immediate = [](SlotRepresentation value) { return CCallHelpers::TrustedImm32(static_cast<int32_t>(value)); };
    if (commonRepresentations == CommonRepresentations::AreRead) {
        CCallHelpers::Jump isNoEncodedValue = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsEncodedValues.back()));
        emitBody(SlotRepresentation::JSValue);
        isNoEncodedValue.link(&jit);
        CCallHelpers::Jump isNoNarrowReference = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsNarrowReferences.back()));
        emitBody(SlotRepresentation::OptionalCell32);
        isNoNarrowReference.link(&jit);
        CCallHelpers::Jump isNoInt32 = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsInt32.back()));
        emitBody(SlotRepresentation::OptionalInt32);
        isNoInt32.link(&jit);
    }
    // Most of what is left is numbers and booleans: `rank: n * 3`.
    static_assert(SlotRepresentation::Double < SlotRepresentation::OptionalDouble && SlotRepresentation::OptionalDouble < SlotRepresentation::Float && SlotRepresentation::Float < SlotRepresentation::Boolean && SlotRepresentation::Boolean < SlotRepresentation::OptionalBoolean);
    auto positionOf = [](SlotRepresentation value) {
        return static_cast<size_t>(std::ranges::find(representationsReadOneByOne, value) - representationsReadOneByOne.begin());
    };
    const std::span<const SlotRepresentation> rest(representationsReadOneByOne);
    CCallHelpers::Jump isOptionalBooleanOrAbove = jit.branch32(CCallHelpers::AboveOrEqual, representation, immediate(SlotRepresentation::OptionalBoolean));
    CCallHelpers::Jump isNoOptionalDouble = jit.branch32(CCallHelpers::NotEqual, representation, immediate(SlotRepresentation::OptionalDouble));
    emitBody(SlotRepresentation::OptionalDouble);
    isNoOptionalDouble.link(&jit);
    // Behind the comparison that failed its flags say on which side the rest lies.
#if CPU(X86_64)
    CCallHelpers::Jump isFloatOrBoolean(jit.m_assembler.jCC(X86Assembler::ConditionA));
#else
    CCallHelpers::Jump isFloatOrBoolean = jit.branch32(CCallHelpers::Above, representation, immediate(SlotRepresentation::OptionalDouble));
#endif
    CCallHelpers::Jump isDouble = jit.branch32(CCallHelpers::Equal, representation, immediate(SlotRepresentation::Double));
    emitSearchForRepresentation(jit, representation, rest.first(positionOf(SlotRepresentation::Double)), emitBody);
    isDouble.link(&jit);
    emitBody(SlotRepresentation::Double);
    isFloatOrBoolean.link(&jit);
    CCallHelpers::Jump isFloat = jit.branch32(CCallHelpers::NotEqual, representation, immediate(SlotRepresentation::Boolean));
    emitBody(SlotRepresentation::Boolean);
    isFloat.link(&jit);
    emitBody(SlotRepresentation::Float);
    isOptionalBooleanOrAbove.link(&jit);
    emitSearchForRepresentation(jit, representation, rest.subspan(positionOf(SlotRepresentation::OptionalBoolean)), emitBody);
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
    // Holds the instance through whose table a word of literal text is read: that of the image of a variable. None: the instance of the stub.
    FPRReg instanceOfLiteralStrings { InvalidFPRReg };
    // Where the slot lies, in place of field.address, which is then a scratch register. Only for a known representation that is read as an encoded value, a narrow reference or a 32-bit integer.
    std::optional<CCallHelpers::BaseIndex> slot { };
    CommonRepresentations commonRepresentations { CommonRepresentations::AreRead };
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
// IsEmittedForSlot: the type is the word of a Slot, which may be bits (Slot::typeIsGivenByBits).
enum class FieldTypeTest : uint8_t { IsEmitted, IsEmittedForSlot, HasPassed };

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
// With a known representation, field.representation is a scratch register.
void readFieldAndReturn(StubAssembler&, const FieldReadRegisters&, FieldReadExits&, std::optional<SlotRepresentation> known = std::nullopt);
// The slot of an accessor field holds its GetterSetter, as a boxed value, a cell or a narrow reference. address and representation are what computeFieldAddress gave. result may be either.
void loadGetterSetterInField(StubAssembler&, GPRReg address, GPRReg representation, GPRReg result, JumpList& isAbsent);
// Nothing falls through. Keeps address and representation.
void branchOnFieldPresence(StubAssembler&, const FieldAddressRegisters&, GPRReg scratch, JumpList& isPresent, JumpList& isAbsent);
// Returns 1 for a field in its slot and 0 for an absent one under Slot::absentIsUndefined. Keeps object at isUnknown.
void returnFieldPresence(StubAssembler&, const CachedSlot&, const FieldAddressRegisters&, JumpList& isUnknown);
// Falls through when accepted. Reads bits 0 to 31 of type. Keeps type and value on every path.
void branchUnlessTypeAccepts(StubAssembler&, const TypeTestRegisters&, JumpList& isRejected, JumpList& cannot);
// At wasAbsent the slot admits the value: the type accepts it, or it is undefined or null and the representation has a marker for it.
// rememberStore emits what comes between a store to a slot of one of AcceptedKindOfValue::ClassOfSlot and the barrier. It is given secondScratch, with that class, finds the type in its register, or zero where the type has not accepted the value,
// and may write every register but the object and the value.
void storeField(StubAssembler&, const FieldStoreRegisters&, FieldStoreEnd, FieldTypeTest, JumpList* stored, FieldStoreExits&, const ScopedLambda<void(GPRReg classOfSlot)>* rememberStore = nullptr);
// Falls through with the new order word stored. Keeps object and slotKey, and writes nothing to memory before it jumps to hasNoTransition. Of a known field fieldIndex is kept and layout is written.
void appendFieldToKeyOrder(StubAssembler&, const KeyOrderRegisters&, JumpList& hasNoTransition, std::optional<FieldOfLayout> known = std::nullopt);
// Behind the store that creates a field: lists its key, in the stub or by the run time, and returns behind the barrier.
void listCreatedFieldAndReturn(StubAssembler&, const KeyOrderRegisters&, GPRReg value, std::optional<FieldOfLayout> known = std::nullopt);
// Gives the field of slotData its granule, in the stub or by the run time, and goes on at findStorage. Writes address, representation and secondScratch.
void createGranuleOfField(StubAssembler&, const FieldStoreRegisters&, const ScopedLambda<void()>& loadOffsetOfExtensionWord, Label findStorage);
PreludeExit emitPrivateDefinePrelude(StubAssembler&);
PreludeExit emitPrivateSetPrelude(StubAssembler&);

inline constexpr uint32_t offsetBitsWithoutSlotKey = SlotData::maxOffset & ~(outOfLineSlotKey - 1);

template<typename AddressOfSlot>
void loadSlotBits(StubAssembler& jit, unsigned width, AddressOfSlot slot, GPRReg bits)
{
    switch (width) {
    case 1:
        jit.load8(slot, bits);
        return;
    case 2:
        jit.load16(slot, bits);
        return;
    case 4:
        jit.load32(slot, bits);
        return;
    case 8:
        jit.load64(slot, bits);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Jump branchIfBitsEqual(StubAssembler&, unsigned width, GPRReg bits, uint64_t expected);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

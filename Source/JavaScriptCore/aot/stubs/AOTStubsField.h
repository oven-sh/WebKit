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

enum class CommonRepresentations : uint8_t { AreRead, EncodedValuesAreReadElsewhere, AreReadElsewhere };

template<typename EmitBody>
void emitDispatchOnRepresentationForRead(StubAssembler& jit, GPRReg representation, CommonRepresentations commonRepresentations, const EmitBody& emitBody)
{
    static_assert(std::ranges::is_sorted(representationsReadOneByOne));
    auto immediate = [](SlotRepresentation value) { return CCallHelpers::TrustedImm32(static_cast<int32_t>(value)); };
    if (commonRepresentations == CommonRepresentations::AreRead) {
        CCallHelpers::Jump isNotEncodedValue = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsEncodedValues.back()));
        emitBody(SlotRepresentation::JSValue);
        isNotEncodedValue.link(&jit);
    }
    if (commonRepresentations != CommonRepresentations::AreReadElsewhere) {
        CCallHelpers::Jump isNotNarrowReference = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsNarrowReferences.back()));
        emitBody(SlotRepresentation::OptionalCell32);
        isNotNarrowReference.link(&jit);
        CCallHelpers::Jump isNotInt32 = jit.branch32(CCallHelpers::Above, representation, immediate(representationsReadAsInt32.back()));
        emitBody(SlotRepresentation::OptionalInt32);
        isNotInt32.link(&jit);
    }
    static_assert(SlotRepresentation::Double < SlotRepresentation::OptionalDouble && SlotRepresentation::OptionalDouble < SlotRepresentation::Float && SlotRepresentation::Float < SlotRepresentation::Boolean && SlotRepresentation::Boolean < SlotRepresentation::OptionalBoolean);
    auto positionOf = [](SlotRepresentation value) {
        return static_cast<size_t>(std::ranges::find(representationsReadOneByOne, value) - representationsReadOneByOne.begin());
    };
    const std::span<const SlotRepresentation> rest(representationsReadOneByOne);
    CCallHelpers::Jump isOptionalBooleanOrAbove = jit.branch32(CCallHelpers::AboveOrEqual, representation, immediate(SlotRepresentation::OptionalBoolean));
    CCallHelpers::Jump isNotOptionalDouble = jit.branch32(CCallHelpers::NotEqual, representation, immediate(SlotRepresentation::OptionalDouble));
    emitBody(SlotRepresentation::OptionalDouble);
    isNotOptionalDouble.link(&jit);
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

struct FieldAddressRegisters {
    GPRReg object;
    GPRReg slotData;
    GPRReg extensionWordOffset;
    GPRReg address;
    GPRReg representation;
};

struct FieldReadRegisters {
    FieldAddressRegisters field;
    GPRReg value;
    GPRReg result;
    FPRReg number;
    FPRReg numberScratch;
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
enum class FieldTypeTest : uint8_t { IsEmitted, IsEmittedForSlot, HasPassed };

struct TypeTestRegisters {
    GPRReg value;
    GPRReg type;
    GPRReg test;
    GPRReg scratch;
    GPRReg secondScratch;
    GPRReg holder { InvalidGPRReg };
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
    GPRReg offsetOfRecord;
};

inline void loadOffsetOfFieldRecord(StubAssembler& jit, GPRReg index, GPRReg result)
{
    static_assert(sizeof(FieldData) == 3 * sizeof(uint32_t) && !(sizeof(LayoutData) % sizeof(uint32_t)));
    jit.getEffectiveAddress(BaseIndex(index, index, CCallHelpers::TimesTwo, sizeof(LayoutData) / sizeof(uint32_t)), result);
    jit.lshift32(TrustedImm32(2), result);
}

inline void loadTypeOfFieldRecord(StubAssembler& jit, GPRReg record, int32_t offset, GPRReg result)
{
    jit.load16(Address(record, offset + OBJECT_OFFSETOF(FieldData, typeNumber)), result);
    jit.lshift32(TrustedImm32(ProgramTables::shiftOfNearTypeNumbers), result);
}

inline void addOffsetOfField(StubAssembler& jit, GPRReg layout, GPRReg index, GPRReg result, int32_t offset = 0)
{
    static_assert(sizeof(FieldData) == 3 * sizeof(uint32_t));
    RELEASE_ASSERT(noOverlap(layout, index));
    jit.getEffectiveAddress(BaseIndex(index, index, CCallHelpers::TimesTwo), index);
    jit.getEffectiveAddress(BaseIndex(layout, index, CCallHelpers::TimesFour, offset), result);
}

void loadExtensionWordOffset(StubAssembler&, GPRReg object, GPRReg result);
struct WaysToFieldInExtension {
    Label fromExtension;
    Label fromGranule;
};
void computeFieldAddress(StubAssembler&, const FieldAddressRegisters&, bool isOutOfLine, JumpList& hasNoStorage, WaysToFieldInExtension* = nullptr, std::optional<SlotRepresentation> known = std::nullopt);
void readFieldAndReturn(StubAssembler&, const FieldReadRegisters&, FieldReadExits&, std::optional<SlotRepresentation> known = std::nullopt);
void loadGetterSetterInField(StubAssembler&, GPRReg address, GPRReg representation, GPRReg result, JumpList& isAbsent);
void branchOnFieldPresence(StubAssembler&, const FieldAddressRegisters&, GPRReg scratch, JumpList& isPresent, JumpList& isAbsent, JumpList& keyOrderDecides);
void returnFieldPresence(StubAssembler&, const CachedSlot&, const FieldAddressRegisters&, JumpList& isUnknown, JumpList* keyOrderSays = nullptr);
void branchUnlessTypeAccepts(StubAssembler&, const TypeTestRegisters&, JumpList& isRejected, JumpList& cannot);
void storeField(StubAssembler&, const FieldStoreRegisters&, FieldStoreEnd, FieldTypeTest, JumpList* stored, FieldStoreExits&, const ScopedLambda<void(GPRReg classOfSlot)>* rememberStore = nullptr);
void appendFieldToKeyOrder(StubAssembler&, const KeyOrderRegisters&, JumpList& hasNoTransition, std::optional<FieldOfLayout> known = std::nullopt);
void listCreatedField(StubAssembler&, const KeyOrderRegisters&, JumpList& hasNoTransition, std::optional<FieldOfLayout> known = std::nullopt);
enum class CreationChecksChain : bool { No, Yes };
void listCreatedFieldAndReturn(StubAssembler&, const KeyOrderRegisters&, GPRReg value, std::optional<FieldOfLayout> known = std::nullopt, CreationChecksChain = CreationChecksChain::No);
void loadOrderAfterRememberedCreation(StubAssembler&, const KeyOrderRegisters&, JumpList& isNotRemembered);
enum class FirstExtension : bool { IsLeftToStub, IsAllocatedInLine };
void createGranuleOfField(StubAssembler&, const FieldStoreRegisters&, const WaysToFieldInExtension&, FirstExtension = FirstExtension::IsLeftToStub);
void allocateFirstExtensionWithOneGranule(StubAssembler&, GPRReg object, GPRReg granuleBit, GPRReg extensionAndGranule, GPRReg scratch, GPRReg secondScratch, Label hasGranule, JumpList& isLeftToStub);
PreludeExit emitPrivateDefinePrelude(StubAssembler&);
PreludeExit emitPrivateHasPrelude(StubAssembler&);
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

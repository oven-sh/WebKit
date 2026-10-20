/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsPropertyKeys.h"
#include <optional>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

inline constexpr StubRegister slotHeaderRegister = StubRegister::T12;
inline constexpr StubRegister slotPayloadRegister = StubRegister::T13;
inline constexpr StubRegister slotNameRegister = stubImmediateRegister;
inline constexpr StubRegister storedValueByIdRegister = StubRegister::A1;
inline constexpr StubRegister keyCellRegister = StubRegister::A1;
inline constexpr StubRegister storedValueByValRegister = StubRegister::A2;
#if CPU(X86_64)
inline constexpr StubRegister readSiteCacheRegister = StubRegister::A1;
inline constexpr StubRegister storeSiteCacheRegister = StubRegister::A2;
#elif CPU(ARM64)
inline constexpr StubRegister readSiteCacheRegister = StubRegister::T15;
inline constexpr StubRegister storeSiteCacheRegister = StubRegister::T15;
#endif

static_assert(noOverlap(gprOf(firstStubOperandRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister), gprOf(slotNameRegister), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::T10), gprOf(StubRegister::T11), gprOf(StubRegister::T14)));
static_assert(noOverlap(gprOf(readSiteCacheRegister), gprOf(firstStubOperandRegister), gprOf(slotNameRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister)));
static_assert(noOverlap(gprOf(storeSiteCacheRegister), gprOf(firstStubOperandRegister), gprOf(storedValueByIdRegister), gprOf(slotNameRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister)));

constexpr RuntimeCounterID reasonOf(SlotFinisher finisher)
{
    switch (finisher) {
    case SlotFinisher::ReadField:
        return RuntimeCounterID::StubCannotReadField;
    case SlotFinisher::CallGetter:
        return RuntimeCounterID::StubCannotCallGetter;
    case SlotFinisher::StoreField:
        return RuntimeCounterID::StubCannotStoreField;
    case SlotFinisher::CreateField:
        return RuntimeCounterID::StubCannotCreateField;
    case SlotFinisher::CallSetterStrict:
    case SlotFinisher::CallSetterSloppy:
        return RuntimeCounterID::StubCannotCallSetter;
    case SlotFinisher::GrowStorage:
        return RuntimeCounterID::StubCannotGrowStorage;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct SlotHandlerOperands {
    GPRReg receiver;
    GPRReg header;
    GPRReg payload { InvalidGPRReg };
    GPRReg value { InvalidGPRReg };
    GPRReg keyCell { InvalidGPRReg };
    GPRReg identifierNumber { InvalidGPRReg };
};

StubSignature signatureOfSlotHandler(SlotHandlerTable);
SlotHandlerOperands operandsOfSlotHandler(SlotHandlerTable);

class CachedSlot {
public:
    CachedSlot(SlotForm form, Slot::Kind kind, GPRReg header, GPRReg payload)
        : m_form(form)
        , m_kind(kind)
        , m_header(header)
        , m_payload(payload)
    {
    }

    SlotForm form() const { return m_form; }
    Slot::Kind kind() const { return m_kind; }
    GPRReg header() const { return m_header; }

    Jump branchIfFlag(StubAssembler&, uint8_t slotFlag) const;
    Jump branchUnlessFlag(StubAssembler&, uint8_t slotFlag) const;
    void loadOwnWordIndex(StubAssembler&, GPRReg result) const;
    void loadInheritedWordIndex(StubAssembler&, GPRReg result) const;
    void loadHolder(StubAssembler&, GPRReg result) const;
    void loadHiddenWordOffset(StubAssembler&, GPRReg result) const;
    void loadFirstPayloadWord(StubAssembler&, GPRReg result) const;
    void loadSecondPayloadWord(StubAssembler&, GPRReg result) const;
    void loadPayloadBits(StubAssembler&, GPRReg result) const;

private:
    SlotForm m_form;
    Slot::Kind m_kind;
    GPRReg m_header;
    GPRReg m_payload;
};

struct ProbedSlot {
    SlotForm form;
    GPRReg header;
    GPRReg payload { InvalidGPRReg };
};

struct FrontProbeRegisters {
    GPRReg structureID;
    GPRReg column;
    GPRReg row;
    GPRReg cell;
    GPRReg scratch;
};

struct HashedProbeRegisters {
    GPRReg structureID;
    GPRReg uid;
    GPRReg entry;
    GPRReg header;
    GPRReg payload;
    GPRReg scratch;
};

// Adds to hasNoStructure in an out-of-line part: the list has to outlive the generator, and an out-of-line part added after this call links it.
void loadStructureIDOfReceiver(StubAssembler&, GPRReg receiver, GPRReg structureID, JumpList& hasNoStructure);
// What probeCell emits runs for a receiver that is a cell, before its StructureID is loaded.
void loadStructureIDOfReceiver(StubAssembler&, GPRReg receiver, GPRReg structureID, JumpList& hasNoStructure, const ScopedLambda<void()>& probeCell);
void loadFrontRow(CCallHelpers&, SlotAccess, GPRReg structureID, GPRReg row, GPRReg scratch);
void loadFrontCell(StubAssembler&, SlotAccess, const FrontProbeRegisters&);
Jump branchUnlessOwnInlineValue(StubAssembler&, GPRReg column, GPRReg cell, GPRReg scratch);
Jump branchIfCellNameDiffers(StubAssembler&, GPRReg column, GPRReg cell, GPRReg scratch);
void loadIdentifier(StubAssembler&, GPRReg identifierNumber, GPRReg uid, JumpList& isNotCreated);

constexpr uint32_t integrityBitsBlockingStore(SlotOperation operation)
{
    switch (operation) {
    case SlotOperation::StoreStrict:
    case SlotOperation::StoreSloppy:
        return orderWordIsFrozen;
    case SlotOperation::Define:
        // A definition asks for a configurable property, which a field of a sealed object is not.
        return orderWordIsFrozen | orderWordIsSealed;
    case SlotOperation::Read:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct FieldTableProbe {
    GPRReg receiver;
    // Not read where boundName is set.
    GPRReg name;
    GPRReg index;
    GPRReg table;
    std::optional<uint32_t> boundName { };
    bool nameHasColumn { true };
    bool receiverIsCell { false };
};

// Reads a field of a compiled object that is in its slot in the cell into result, which may be any of the probe's registers, and goes on with finish, which leaves the stub.
// Changes neither receiver nor name where it leaves for otherwise.
void readFieldByTable(StubAssembler&, const FieldTableProbe&, GPRReg result, JumpList& otherwise, const ScopedLambda<void()>& finish);
void returnFieldByTable(StubAssembler&, const FieldTableProbe&, JumpList& otherwise);
// Stores value in a field of a compiled object that is in its slot, has one of the entry's own forms and needs no type test, and returns.
void storeFieldByTable(StubAssembler&, const FieldTableProbe&, GPRReg value, GPRReg scratch, SlotOperation, JumpList& otherwise);
void findHashedSlot(StubAssembler&, SlotAccess, const HashedProbeRegisters&, JumpList& miss);

inline Jump branchIfSlotKindDiffers(StubAssembler& jit, const ProbedSlot& slot, Slot::Kind kind, GPRReg scratch)
{
    switch (slot.form) {
    case SlotForm::Slot:
        extractBits(jit, slot.header, Slot::kindShiftInHeader, 8 * sizeof(Slot::Kind), scratch);
        return jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<uint8_t>(kind)));
    case SlotForm::PackedSlot: {
        constexpr unsigned kindShiftInCell = 8 * PackedSlot::offsetOfBiasedKindAndFlags();
        const uint8_t biasedKind = (static_cast<uint8_t>(kind) ^ PackedSlot::ownInlineValueBits) & PackedSlot::kindMask;
        if (!biasedKind)
            return jit.branchTest64(CCallHelpers::NonZero, slot.header, TrustedImm64(static_cast<int64_t>(PackedSlot::kindMask) << kindShiftInCell));
        extractBits(jit, slot.header, kindShiftInCell, PackedSlot::kindBits, scratch);
        return jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(biasedKind));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void loadSlotKind(StubAssembler&, const ProbedSlot&, GPRReg kind);
// On x86-64 cell is zero-extended in place.
void loadOwnInlineValue(StubAssembler&, GPRReg object, GPRReg cell, GPRReg result);

void jumpToSlotHandler(StubAssembler&, SlotHandlerTable, GPRReg header, GPRReg scratch);
void jumpToSlotHandlerOfKind(StubAssembler&, SlotHandlerTable, GPRReg header, GPRReg kind);
// kind is what loadSlotKind gave.
void callSlotHandler(StubAssembler&, SlotHandlerTable, GPRReg kind);
void linkToSlotExit(StubAssembler&, JumpList&, SlotHandlerTable, SlotExit);
void linkToSlotCannot(StubAssembler&, JumpList&, SlotHandlerTable, SlotFinisher, const CachedSlot&);

// result may be receiver or storage. storage may be the register that holds the payload of the slot.
void loadValueBySlot(StubAssembler&, const CachedSlot&, GPRReg receiver, GPRReg storage, GPRReg wordIndex, GPRReg result);
void loadInheritedValueBySlot(StubAssembler&, const CachedSlot&, GPRReg storage, GPRReg wordIndex, GPRReg result);
void returnPresenceBySlot(StubAssembler&, PresenceQuestion, const ProbedSlot&, GPRReg receiver, JumpList& isUnknown);

void loadStaticsBase(StubAssembler&, GPRReg staticsBase, GPRReg roundedSizeOfStatics);
void markStaticsCard(StubAssembler&, GPRReg staticsBase, GPRReg roundedSizeOfStatics, GPRReg offsetInStatics);

// What filled a site was not in the field table: an access that the table answers never gets as far as a fill.
Jump branchIfSiteIsFilled(StubAssembler&, GPRReg site);
void countSiteCacheMiss(StubAssembler&, GPRReg site, GPRReg scratch, JumpList& isDueForRefill);
void countMissOfEmptySite(StubAssembler&, GPRReg site, GPRReg scratch, JumpList& isDueForFill);
void refillSiteCache(StubAssembler&, GPRReg receiver, GPRReg identifierNumber, GPRReg site, SlotAccess);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

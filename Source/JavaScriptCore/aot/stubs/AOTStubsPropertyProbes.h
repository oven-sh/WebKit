/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFieldTable.h"
#include "AOTStubAssembler.h"
#include "AOTStubHelpers.h"
#include "AOTStubRegisters.h"
#include "AOTStubsAccessTable.h"
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
inline constexpr StubRegister valueStoredThroughProxyRegister = StubRegister::T10;

void loadProxySlotKey(StubAssembler&, SlotForm, GPRReg header, GPRReg key, JumpList& isForNoProxies);
#if CPU(X86_64)
inline constexpr StubRegister readSiteCacheRegister = StubRegister::A1;
inline constexpr StubRegister storeSiteCacheRegister = StubRegister::A2;
#elif CPU(ARM64)
inline constexpr StubRegister readSiteCacheRegister = StubRegister::T15;
inline constexpr StubRegister storeSiteCacheRegister = StubRegister::T15;
#endif

static_assert(noOverlap(gprOf(firstStubOperandRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister), gprOf(slotNameRegister), gprOf(StubRegister::A1), gprOf(StubRegister::A2), gprOf(StubRegister::T10), gprOf(StubRegister::T11), gprOf(StubRegister::T14)));
static_assert(noOverlap(gprOf(readSiteCacheRegister), gprOf(firstStubOperandRegister), gprOf(slotNameRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister), gprOf(StubRegister::T10), gprOf(StubRegister::T11), gprOf(StubRegister::T14)));
static_assert(noOverlap(gprOf(storeSiteCacheRegister), gprOf(firstStubOperandRegister), gprOf(storedValueByIdRegister), gprOf(slotNameRegister), gprOf(slotHeaderRegister), gprOf(slotPayloadRegister), gprOf(StubRegister::T10), gprOf(StubRegister::T11)));

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
    GPRReg registerOfFirstPayloadWord() const;
    void loadSecondPayloadWord(StubAssembler&, GPRReg result) const;
    void loadVariableBase(StubAssembler&, GPRReg receiver, GPRReg result, GPRReg scratch) const;
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
    GPRReg kind { InvalidGPRReg };
};

void loadStructureIDOfReceiver(StubAssembler&, GPRReg receiver, GPRReg structureID, JumpList& hasNoStructure, const ScopedLambda<void()>& probeCell = [] { });
void loadFrontCell(StubAssembler&, SlotAccess, const FrontProbeRegisters&);
Jump branchUnlessOwnInlineValue(StubAssembler&, GPRReg column, GPRReg cell, GPRReg scratch);
Jump branchIfCellNameDiffers(StubAssembler&, GPRReg column, GPRReg cell, GPRReg scratch);
#if CPU(X86_64)
void locateFrontCell(StubAssembler&, SlotAccess, GPRReg structureIDAndIndex, GPRReg column, GPRReg plane);
#endif

enum class ReceiverOfFrontProbe : bool { IsAnyCell, IsCompiledObject };

struct FrontTableRead {
    GPRReg receiver;
    GPRReg column;
    GPRReg structureID;
    GPRReg cell;
    GPRReg scratch;
    GPRReg secondScratch;
    ReceiverOfFrontProbe receiverIs { ReceiverOfFrontProbe::IsAnyCell };
    bool filesInAccessTable { false };
};

struct LoadedFrontCell {
    GPRReg receiver;
    GPRReg cell;
    GPRReg scratch;
    GPRReg secondScratch;
    ReceiverOfFrontProbe receiverIs { ReceiverOfFrontProbe::IsAnyCell };
};

void readByFrontTable(StubAssembler&, const FrontTableRead&, GPRReg result, JumpList& hasOtherName, std::optional<StubKey> entersBehindAnswer = std::nullopt);
void readByLoadedFrontCell(StubAssembler&, const LoadedFrontCell&, GPRReg result, const ScopedLambda<void()>& finish);
void loadIdentifier(StubAssembler&, GPRReg identifierNumber, GPRReg uid, JumpList& isNotCreated);
void loadProgramSymbol(StubAssembler&, GPRReg identifierNumber, GPRReg uid, JumpList& isNotCreated);

constexpr uint32_t integrityBitsBlockingStore(SlotOperation operation)
{
    switch (operation) {
    case SlotOperation::StoreStrict:
    case SlotOperation::StoreSloppy:
        return orderWordIsFrozen;
    case SlotOperation::Define:
        return orderWordIsFrozen | orderWordIsSealed;
    case SlotOperation::Read:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct FieldTableProbe {
    GPRReg receiver;
    GPRReg name;
    GPRReg index;
    GPRReg table;
    bool nameHasColumn { true };
    bool receiverIsCell { false };
    bool answersAbsent { false };
    std::optional<CallerFrame> answersMissingFields { };
    JumpList* isNotCompiledObject { nullptr };
    GPRReg keyInAccessTable { InvalidGPRReg };
    GPRReg placeInAccessTable { InvalidGPRReg };
    JumpList* hasFormAndOperand { nullptr };
    bool receiverIsCompiledObject { false };
    bool waysOnwardAreCalls { false };
};

void readFieldByTable(StubAssembler&, const FieldTableProbe&, GPRReg result, JumpList& otherwise, const ScopedLambda<void()>& finish, const ScopedLambda<void()>* finishWhereAbsent = nullptr);
void returnFieldByTable(StubAssembler&, const FieldTableProbe&, JumpList& otherwise);
void answerMissingField(StubAssembler&, CallerFrame, GPRReg object, GPRReg name, GPRReg T10, GPRReg T11, GPRReg T12, GPRReg T13, JumpList& otherwise);
void storeFieldByTable(StubAssembler&, const FieldTableProbe&, GPRReg value, GPRReg scratch, SlotOperation, JumpList& otherwise);
void loadIndexInFieldTable(StubAssembler&, GPRReg object, GPRReg name, GPRReg index, GPRReg table, JumpList& hasNone);
void findHashedSlot(StubAssembler&, SlotAccess, const HashedProbeRegisters&, JumpList& miss);

enum class ObjectLookupQuestion : uint8_t { Read, In, HasOwn };
enum class ObjectLookupName : bool { IsGivenByNumber, IsGivenByUID };

struct ObjectLookupRegisters {
    GPRReg receiver;
    GPRReg name;
    GPRReg uid;
    GPRReg object;
    GPRReg structure;
    GPRReg size;
    GPRReg vector;
    GPRReg index;
};

class IndexRegisterOfObjectLookup {
public:
#if CPU(X86_64)
    explicit IndexRegisterOfObjectLookup(StubAssembler& jit)
        : m_register(jit)
    {
    }

    operator GPRReg() const { return m_register; }

private:
    OwnedScratchRegister m_register;
#else
    explicit IndexRegisterOfObjectLookup(StubAssembler& jit)
        : m_register(jit.reuseScratch(StubRegister::T15))
    {
    }

    operator GPRReg() const { return m_register; }

private:
    GPRReg m_register;
#endif
};

void answerFromObjectsAndReturn(StubAssembler&, ObjectLookupQuestion, const ObjectLookupRegisters&, ObjectLookupName, JumpList& isUnknown, const ScopedLambda<void()>* willReturnValue = nullptr);
struct EmptyWordsOfPropertyTable {
    JumpList inCompactIndex;
    JumpList inWideIndex;
};

void findInPropertyTable(StubAssembler&, const ObjectLookupRegisters&, JumpList& isAbsent, JumpList& isUnknown, EmptyWordsOfPropertyTable* = nullptr);
void locateFieldOfObject(StubAssembler&, const ObjectLookupRegisters&, ObjectLookupName, JumpList& hasNoField, JumpList& isUnknown);
void replaceOwnValueAndReturn(StubAssembler&, SlotOperation, const ObjectLookupRegisters&, ObjectLookupName, GPRReg value, JumpList& cannot, GPRReg spare = InvalidGPRReg, const ScopedLambda<void()>* willReplaceValue = nullptr);
void countMissOfPair(StubAssembler&, GPRReg structureID, GPRReg uid, GPRReg counter, JumpList& isDueForFiling);

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

inline Jump branchUnlessSlotNamesField(StubAssembler& jit, const ProbedSlot& slot, GPRReg scratch)
{
    switch (slot.form) {
    case SlotForm::Slot:
        extractBits(jit, slot.header, Slot::kindShiftInHeader + 1, 8 * sizeof(Slot::Kind) - 1, scratch);
        return jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(static_cast<uint8_t>(Slot::Kind::Field) >> 1));
    case SlotForm::PackedSlot: {
        constexpr unsigned kindShiftInCell = 8 * PackedSlot::offsetOfBiasedKindAndFlags();
        constexpr uint8_t biasedKind = (static_cast<uint8_t>(Slot::Kind::Field) ^ PackedSlot::ownInlineValueBits) & PackedSlot::kindMask;
        extractBits(jit, slot.header, kindShiftInCell + 1, PackedSlot::kindBits - 1, scratch);
        return jit.branch32(CCallHelpers::NotEqual, scratch, TrustedImm32(biasedKind >> 1));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void loadSlotKind(StubAssembler&, const ProbedSlot&, GPRReg kind);
void loadOwnInlineValue(StubAssembler&, GPRReg object, GPRReg cell, GPRReg result);

void jumpToSlotHandler(StubAssembler&, SlotHandlerTable, GPRReg header, GPRReg scratch);
#if CPU(X86_64)
void callSlotHandler(StubAssembler&, SlotHandlerTable, GPRReg header, GPRReg scratch);
#endif
inline constexpr std::array<GPRReg, 2> registersThatWaysOnwardOfGetByIdLose { gprOf(StubRegister::T12), gprOf(StubRegister::T14) };
void jumpToSlotHandlerOfKind(StubAssembler&, SlotHandlerTable, GPRReg header, GPRReg kind);
void linkToSlotExit(StubAssembler&, JumpList&, SlotHandlerTable, SlotExit);
void linkToSlotCannot(StubAssembler&, JumpList&, SlotHandlerTable, SlotFinisher, const CachedSlot&);
struct FieldExitBySlotRegisters {
    GPRReg object;
    GPRReg value;
    GPRReg addressOfSlot;
    GPRReg exit;
    GPRReg scratch;
    GPRReg extra;
};
void repeatFieldExitFoundBySlotAndReturn(StubAssembler&, const FieldExitBySlotRegisters&, JumpList& cannot);
void storeToReplacedMemberAndReturn(StubAssembler&, GPRReg receiver, GPRReg identifierNumber, GPRReg value, GPRReg place, GPRReg word);

void loadValueBySlot(StubAssembler&, const CachedSlot&, GPRReg receiver, GPRReg storage, GPRReg wordIndex, GPRReg result);
void loadOwnValueBySlot(StubAssembler&, const CachedSlot&, GPRReg receiver, GPRReg storage, GPRReg wordIndex, GPRReg result);
void loadInheritedValueBySlot(StubAssembler&, const CachedSlot&, GPRReg storage, GPRReg wordIndex, GPRReg result);
enum class KindOfProbedSlot : uint8_t { IsInHeader, IsInT10 };
void returnPresenceBySlot(StubAssembler&, PresenceQuestion, const ProbedSlot&, GPRReg receiver, Label asksReceiver, JumpList& isUnknown, KindOfProbedSlot = KindOfProbedSlot::IsInHeader, const ScopedLambda<void()>* rememberThatNothingIsOwn = nullptr);

void loadStaticsBase(StubAssembler&, GPRReg staticsBase, GPRReg roundedSizeOfStatics);

void countSiteCacheMiss(StubAssembler&, GPRReg site, GPRReg scratch, JumpList& isDueForRefill);
void countMissOfEmptySite(StubAssembler&, GPRReg site, GPRReg scratch, JumpList& isDueForFill);
void countMissOfPlace(StubAssembler&, GPRReg distance, GPRReg scratch, JumpList& isSample);
void goOnWhileSamplesOfPlaceAreLeftOut(StubAssembler&, GPRReg scratchOfCount, CCallHelpers::Label isCounted);
void refillSiteCache(StubAssembler&, GPRReg receiver, GPRReg identifierNumber, GPRReg site, SlotAccess);

inline constexpr unsigned receiverOperand = 0;
inline constexpr unsigned headerOperand = 1;
inline constexpr unsigned payloadOperand = 2;
inline constexpr unsigned locationShiftInCell = 8 * PackedSlot::offsetOfLocation();

inline constexpr PutMode putModeOf(SlotOperation operation)
{
    for (PutMode mode : { PutMode::Strict, PutMode::Sloppy, PutMode::Define }) {
        if (operationFor(mode) == operation)
            return mode;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

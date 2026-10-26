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
    // Its low half is the first payload word. What is above is not zero.
    GPRReg registerOfFirstPayloadWord() const;
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
    // Gets what loadSlotKind gives. May be uid or structureID.
    GPRReg kind { InvalidGPRReg };
};

// Adds to hasNoStructure in an out-of-line part: the list has to outlive the generator, and an out-of-line part added after this call links it.
// What probeCell emits runs for a receiver that is a cell, before its StructureID is loaded.
void loadStructureIDOfReceiver(StubAssembler&, GPRReg receiver, GPRReg structureID, JumpList& hasNoStructure, const ScopedLambda<void()>& probeCell = [] { });
void loadFrontRow(CCallHelpers&, SlotAccess, GPRReg structureID, GPRReg row, GPRReg scratch);
void loadFrontCell(StubAssembler&, SlotAccess, const FrontProbeRegisters&);
Jump branchUnlessOwnInlineValue(StubAssembler&, GPRReg column, GPRReg cell, GPRReg scratch);
Jump branchIfCellNameDiffers(StubAssembler&, GPRReg column, GPRReg cell, GPRReg scratch);
#if CPU(X86_64)
void loadFrontRowOfCell(CCallHelpers&, SlotAccess, GPRReg cell, GPRReg row, GPRReg scratch);
// The cell of the front table for a Structure and a name lies at plane + index * 8. structureIDAndIndex holds the StructureID.
void locateFrontCell(StubAssembler&, SlotAccess, GPRReg structureIDAndIndex, GPRReg column, GPRReg plane);
// Answers what most reads find, each by a comparison with the cell where it lies: a value of the receiver's own in its cell, then that nothing has the name.
void returnByFrontCell(StubAssembler&, GPRReg receiver, GPRReg column, GPRReg plane, GPRReg index, GPRReg cell, JumpList& hasOtherName);
#endif
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
    // Where the layout has no field of the name, and neither the object nor what is above it can have the name, the read gives undefined.
    bool answersAbsent { false };
    // For a probe that returns what it reads.
    std::optional<CallerFrame> answersMissingFields { };
};

// Reads a field of a compiled object that is in its slot in the cell into result, which may be any of the probe's registers, and goes on with finish, which leaves the stub.
void readFieldByTable(StubAssembler&, const FieldTableProbe&, GPRReg result, JumpList& otherwise, const ScopedLambda<void()>& finish, const ScopedLambda<void()>* finishWhereAbsent = nullptr);
void returnFieldByTable(StubAssembler&, const FieldTableProbe&, JumpList& otherwise);
// Binds GetByIdOfSilentSlot and GetByIdOfEmptySlot.
void answerMissingField(StubAssembler&, CallerFrame, GPRReg object, GPRReg name, GPRReg T10, GPRReg T11, GPRReg T12, JumpList& otherwise);
// Stores value in a field of a compiled object that is in its slot, has one of the entry's own forms and needs no type test, and returns.
void storeFieldByTable(StubAssembler&, const FieldTableProbe&, GPRReg value, GPRReg scratch, SlotOperation, JumpList& otherwise, JumpList* isSampled = nullptr);
void findHashedSlot(StubAssembler&, SlotAccess, const HashedProbeRegisters&, JumpList& miss);

enum class ObjectLookupQuestion : uint8_t { Read, In, HasOwn };
// By number: name holds the number of the name and is kept. By uid: name is one more scratch register.
enum class ObjectLookupName : bool { IsGivenByNumber, IsGivenByUID };

// receiver is kept on every way out.
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

// The eighth register of a lookup. x86-64 has no stub register left for it. Elsewhere the macro assembler writes its scratch registers itself.
inline GPRReg indexRegisterOfObjectLookup(StubAssembler& jit)
{
#if CPU(X86_64)
    return jit.scratchRegister();
#else
    return jit.reuseScratch(StubRegister::T15);
#endif
}

// Asks the receiver and then the objects above it, each for what it has at this moment.
// willReturnValue: what to do in front of the return of a plain value that the property table of object has listed. structure has the offset above the eight bits of the attributes and is kept. size, vector and index are free.
void answerFromObjectsAndReturn(StubAssembler&, ObjectLookupQuestion, const ObjectLookupRegisters&, ObjectLookupName, JumpList& isUnknown, const ScopedLambda<void()>* willReturnValue = nullptr);
// Where the search for a name that the table lacks ends: at the word of the index that the name would take. vector and size are as the table has them, and index is the number of the word.
struct EmptyWordsOfPropertyTable {
    JumpList inCompactIndex;
    JumpList inWideIndex;
};

// Takes the Structure of an ordinary object in structure and leaves there what its property table has for uid: the offset, above the eight bits of the attributes. Writes size, vector and index.
// With emptyWords every search goes as far as the index, and nothing is added to isAbsent.
void findInPropertyTable(StubAssembler&, const ObjectLookupRegisters&, JumpList& isAbsent, JumpList& isUnknown, EmptyWordsOfPropertyTable* = nullptr);
// object is a compiled object. Leaves in index where the row of its shape has the entry of the field with the name. Writes name, where the name is given by its uid, and vector.
void locateFieldOfObject(StubAssembler&, const ObjectLookupRegisters&, ObjectLookupName, JumpList& hasNoField, JumpList& isUnknown);
// Where receiver itself has the property as a plain value that may be written, puts value in its place and returns, and nothing is filed.
// spare is a register that nobody reads once the store cannot fail. With one, and the name given by its uid, a dictionary is also given a name that it lacks, where neither its table nor its storage has to grow.
// willReplaceValue: what to do in front of the store to a property that the table of object has listed. The registers are as for willReturnValue.
void replaceOwnValueAndReturn(StubAssembler&, SlotOperation, const ObjectLookupRegisters&, ObjectLookupName, GPRReg value, JumpList& cannot, GPRReg spare = InvalidGPRReg, const ScopedLambda<void()>* willReplaceValue = nullptr);
// What the objects answer is never filed, so a pair that comes again and again has to leave for the operation that files: one miss in MissesOfPairs::missesBetweenFilings.
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

// Taken unless the slot is of kind Field or FieldGetter, which differ in the lowest bit alone.
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
// On x86-64 cell is zero-extended in place.
void loadOwnInlineValue(StubAssembler&, GPRReg object, GPRReg cell, GPRReg result);

void jumpToSlotHandler(StubAssembler&, SlotHandlerTable, GPRReg header, GPRReg scratch);
void jumpToSlotHandlerOfKind(StubAssembler&, SlotHandlerTable, GPRReg header, GPRReg kind);
// kind is what loadSlotKind gave.
void callSlotHandler(StubAssembler&, SlotHandlerTable, GPRReg kind);
void linkToSlotExit(StubAssembler&, JumpList&, SlotHandlerTable, SlotExit);
void linkToSlotCannot(StubAssembler&, JumpList&, SlotHandlerTable, SlotFinisher, const CachedSlot&);

// result may be receiver or storage. storage may be the register that holds the payload of the slot. wordIndex may be the register that holds the header of a Slot.
void loadValueBySlot(StubAssembler&, const CachedSlot&, GPRReg receiver, GPRReg storage, GPRReg wordIndex, GPRReg result);
void loadInheritedValueBySlot(StubAssembler&, const CachedSlot&, GPRReg storage, GPRReg wordIndex, GPRReg result);
enum class KindOfProbedSlot : uint8_t { IsInHeader, IsInT10 };
void returnPresenceBySlot(StubAssembler&, PresenceQuestion, const ProbedSlot&, GPRReg receiver, JumpList& isUnknown, KindOfProbedSlot = KindOfProbedSlot::IsInHeader);

void loadStaticsBase(StubAssembler&, GPRReg staticsBase, GPRReg roundedSizeOfStatics);
void markStaticsCard(StubAssembler&, GPRReg staticsBase, GPRReg roundedSizeOfStatics, GPRReg offsetInStatics);

// What filled a site was not in the field table: an access that the table answers never gets as far as a fill.
Jump branchIfSiteIsFilled(StubAssembler&, GPRReg site);
void countSiteCacheMiss(StubAssembler&, GPRReg site, GPRReg scratch, JumpList& isDueForRefill);
void countMissOfEmptySite(StubAssembler&, GPRReg site, GPRReg scratch, JumpList& isDueForFill);
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

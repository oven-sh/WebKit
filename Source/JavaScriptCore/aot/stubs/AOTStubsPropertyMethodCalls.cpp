/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubFrames.h"
#include "AOTStubsCalls.h"
#include "AOTStubsField.h"
#include "JSCell.h"

namespace JSC { namespace AOT {

static_assert(sizeof(PackedSlot) == sizeof(uint64_t));
static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfPayload() == sizeof(uint64_t));
static_assert(gprOf(slotNameRegister) == countGPR && noOverlap(gprOf(slotNameRegister), thisGPR, calleeGPR, gprOf(closureWordRegister)));
static_assert(descriptorOf(StubFamily::CallMethodByIdLending).numberOfForms == numberOfSetsOfLentArguments);

StubSignature signatureOfCallMethodById(StubKey)
{
    StubSignature signature = callConventionSignature({ });
    signature.immediate = gprOf(slotNameRegister);
    return signature;
}

StubSignature signatureOfCallMethodByIdLending(StubKey key) { return signatureOfCallMethodById(key); }

static void emitCallMethodById(StubAssembler& jit, unsigned lentArguments)
{
    const unsigned count = jit.key().count();
    const StubKey callsValue = callValueLending(count, lentArguments);
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg name = jit.immediate();
    const GPRReg key = registers.word;
    const GPRReg place = registers.scratch;
#if CPU(ARM64)
    const GPRReg spare = jit.allocateScratch(StubRegister::T14);
    const GPRReg secondSpare = jit.allocateScratch(StubRegister::T15);
#else
    std::optional<OwnedScratchRegister> ownedScratch;
    ownedScratch.emplace(jit);
    const GPRReg spare = ownedScratch->gpr();
    const GPRReg secondSpare = InvalidGPRReg;
#endif
    const AccessTableRegisters inTable { thisGPR, name, key, place };
    RELEASE_ASSERT(noOverlap(thisGPR, name, key, place, spare, calleeGPR, instanceGPR));
    JumpList isNotInPlaceOfCall;
    JumpList isNoOwnValue;
    JumpList isNotInAccessTable;
    JumpList readsByStub;
    JumpList filesNothing;
    JumpList isNotInFrontTable;
    JumpList isInNoTableByShape;

    jit.checkStackBeforeEnteringCallee();
    readsByStub.append(jit.branchIfNotCell(thisGPR));
    isInNoTableByShape.append(jit.branchTest8(CCallHelpers::Zero, Address(thisGPR, offsetOfShapeWord + 3), TrustedImm32(compiledObjectShapeBit >> 24)));
    isInNoTableByShape.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)));
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), key);
    readByFrontTable(jit, { thisGPR, name, key, spare, place, secondSpare, ReceiverOfFrontProbe::IsCompiledObject, false }, calleeGPR, isNotInFrontTable, callsValue);
    readsByStub.append(jit.jump());
    isNotInFrontTable.link(&jit);
    {
#if CPU(X86_64)
        ownedScratch.reset();
#endif
        FieldTableProbe probe { thisGPR, name, key, place, true, true };
        probe.receiverIsCompiledObject = true;
        readFieldByTable(jit, probe, calleeGPR, isInNoTableByShape, [&] {
            jit.jumpToStub(callsValue);
        });
#if CPU(X86_64)
        ownedScratch.emplace(jit);
#endif
    }
    isInNoTableByShape.link(&jit);
    locatePlaceInAccessTable(jit, PartOfAccessTable::Calls, inTable, count);
    loadWordBehindKeyInAccessTable(jit, inTable, OBJECT_OFFSETOF(AccessTable::Call, key), calleeGPR, spare, isNotInPlaceOfCall);
    jit.load64(Address(calleeGPR), calleeGPR);
    const GPRReg code = key;
#if CPU(ARM64)
    jit.loadPair64(place, TrustedImm32(OBJECT_OFFSETOF(AccessTable::Call, callee)), spare, code);
    isNotInPlaceOfCall.append(jit.branch64(CCallHelpers::NotEqual, calleeGPR, spare));
#else
    isNotInPlaceOfCall.append(jit.branch64(CCallHelpers::NotEqual, calleeGPR, Address(place, OBJECT_OFFSETOF(AccessTable::Call, callee))));
    jit.load64(Address(place, OBJECT_OFFSETOF(AccessTable::Call, closureWord)), code);
#endif
    if (lentArguments)
        jit.linkToStub(branchUnlessWordBorrows(jit, code, spare, lentArguments), callsValue);
    jit.move(TrustedImm32(count), countGPR);
    toCodeAddress(jit, code, place, StackIsTested::Yes);
    jit.farJump(code, JSEntryPtrTag);

    isNotInPlaceOfCall.link(&jit);
    locatePlaceInAccessTable(jit, PartOfAccessTable::Reads, inTable);
    loadWordBehindKeyInAccessTable(jit, inTable, OBJECT_OFFSETOF(AccessTable::Read, keyOfOwnValue), calleeGPR, spare, isNoOwnValue);
    jit.load64(BaseIndex(thisGPR, calleeGPR, CCallHelpers::TimesOne), calleeGPR);
    readsByStub.append(jit.branchTest64(CCallHelpers::Zero, calleeGPR));
    jit.jumpToStub(callsValue);

    isNoOwnValue.link(&jit);
    loadWordBehindKeyInAccessTable(jit, inTable, OBJECT_OFFSETOF(AccessTable::Read, keyOfOtherAnswer), calleeGPR, spare, isNotInAccessTable);
    Jump hasForm = jit.branchTest64(CCallHelpers::NonZero, calleeGPR, TrustedImm32(AccessTable::answerHasForm));
    jit.load64(Address(calleeGPR), calleeGPR);
    readsByStub.append(jit.branchTest64(CCallHelpers::Zero, calleeGPR));
    filesNothing.append(jit.branchIfNotCell(calleeGPR));
    loadClosureWordOfThisRealm(jit, calleeGPR, registers, filesNothing);
    jit.move(TrustedImm64(ClosureWord::minWordOfCalleeTaking(count)), registers.scratch);
    filesNothing.append(jit.branch64(CCallHelpers::LessThan, registers.word, registers.scratch));
    fileCallInAccessTable(jit, { thisGPR, name, spare, place }, count, calleeGPR, registers.word);
    filesNothing.link(&jit);
    jit.jumpToStub(callsValue);

    hasForm.link(&jit);
    readByFormOfAnswerInAccessTable(jit, thisGPR, calleeGPR, place, calleeGPR, readsByStub, [&] {
        jit.jumpToStub(callsValue);
    });

    isNotInAccessTable.link(&jit);
    readsByStub.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)));
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), key);
    readByFrontTable(jit, { thisGPR, name, key, spare, place, secondSpare, ReceiverOfFrontProbe::IsCompiledObject, false }, calleeGPR, readsByStub, callsValue);

    readsByStub.link(&jit);
    Vector<GPRReg, 1 + numberOfArgumentGPRs> kept;
    kept.append(thisGPR);
    for (unsigned index = 0; index < count; ++index)
        kept.append(argumentGPR(index));
    jit.emitFunctionPrologue();
    jit.preserving(std::span<const GPRReg>(kept.span()), [&] {
        jit.move(thisGPR, gprOf(firstStubOperandRegister));
        jit.callStub(getByIdCheckingColumn().withCallerFrame(CallerFrame::StubFrame));
        jit.bindReturnLabel(StubFrameKind::MethodRead);
        jit.move(GPRInfo::returnValueGPR, calleeGPR);
    });
    jit.emitFunctionEpilogue();
    jit.jumpToStub(callsValue);
}

void generateCallMethodById(StubAssembler& jit)
{
    emitCallMethodById(jit, 0);
}

void generateCallMethodByIdLending(StubAssembler& jit)
{
    emitCallMethodById(jit, jit.key().form() + 1);
}

void generateTryGetSuperById(StubAssembler& jit)
{
    const GPRReg R0 = jit.operand(0);
    const GPRReg name = jit.immediate();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T10);
    const GPRReg structureID = jit.allocateScratch(StubRegister::T11);
    const GPRReg cell = jit.allocateScratch(slotHeaderRegister);
    const GPRReg row = jit.allocateScratch(StubRegister::T13);
    const ProbedSlot slot { SlotForm::PackedSlot, cell };
    JumpList needsGenericRead;

    needsGenericRead.append(jit.branchIfNotCell(R0));
    needsGenericRead.append(jit.branch32(CCallHelpers::Above, name, TrustedImm32(RowDisplacementTable::maxColumn)));
    Label hasHolder = jit.label();
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    loadFrontCell(jit, SlotAccess::Read, { structureID, name, row, cell, scratch });
    needsGenericRead.append(branchIfCellNameDiffers(jit, name, cell, scratch));
    Jump isNotValue = branchIfSlotKindDiffers(jit, slot, Slot::Kind::Value, scratch);
    loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, cell, InvalidGPRReg), R0, row, scratch, R0);
    jit.returnToCaller();

    isNotValue.link(&jit);
    Jump isNotAbsent = branchIfSlotKindDiffers(jit, slot, Slot::Kind::Absent, scratch);
    Label returnsUndefined = jit.label();
    jit.move(TrustedImm64(JSValue::ValueUndefined), R0);
    jit.returnToCaller();

    const FPRReg number = jit.allocateScratch(FPRInfo::fpRegT0);
    const FPRReg numberScratch = jit.allocateScratch(FPRInfo::fpRegT1);
    auto fieldReadRegisters = [&](GPRReg object, GPRReg slotData) -> FieldReadRegisters {
        return {
            .field = { .object = object, .slotData = slotData, .extensionWordOffset = structureID, .address = structureID, .representation = row },
            .value = scratch,
            .result = R0,
            .number = number,
            .numberScratch = numberScratch,
        };
    };
    isNotAbsent.link(&jit);
    Jump isNotField = branchIfSlotKindDiffers(jit, slot, Slot::Kind::Field, scratch);
    {
        const CachedSlot field(SlotForm::PackedSlot, Slot::Kind::Field, cell, InvalidGPRReg);
        const FieldReadRegisters registers = fieldReadRegisters(R0, field.registerOfFirstPayloadWord());
        FieldReadExits exits;
        Jump isOutOfLine = field.branchIfFlag(jit, Slot::isOutOfLine);
        computeFieldAddress(jit, registers.field, false, exits.isAbsent);
        Label hasAddress = jit.label();
        readFieldAndReturn(jit, registers, exits);

        isOutOfLine.link(&jit);
        field.loadHiddenWordOffset(jit, registers.field.extensionWordOffset);
        computeFieldAddress(jit, registers.field, true, exits.isAbsent);
        jit.jump().linkTo(hasAddress, &jit);

        exits.isAbsent.link(&jit);
        field.branchIfFlag(jit, Slot::absentIsUndefined).linkTo(returnsUndefined, &jit);
        needsGenericRead.append(jit.jump());
        needsGenericRead.append(exits.cannot);
    }

    isNotField.link(&jit);
    Jump isNotVariable = branchIfSlotKindDiffers(jit, slot, Slot::Kind::Variable, scratch);
    {
        const CachedSlot variable(SlotForm::PackedSlot, Slot::Kind::Variable, cell, InvalidGPRReg);
        const FieldReadRegisters registers = fieldReadRegisters(cell, scratch);
        FieldReadExits exits;
        variable.loadFirstPayloadWord(jit, registers.field.slotData);
        variable.loadVariableBase(jit, R0, cell, structureID);
        computeFieldAddress(jit, registers.field, false, exits.isAbsent);
        readFieldAndReturn(jit, registers, exits);
        needsGenericRead.append(exits.isAbsent);
        needsGenericRead.append(exits.cannot);
    }

    isNotVariable.link(&jit);
    needsGenericRead.append(branchIfSlotKindDiffers(jit, slot, Slot::Kind::InheritedField, scratch));
    CachedSlot(SlotForm::PackedSlot, Slot::Kind::InheritedField, cell, InvalidGPRReg).loadHolder(jit, R0);
    jit.jump().linkTo(hasHolder, &jit);

    needsGenericRead.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.returnToCaller();
}

static void loadPrototypeAboveAndReturn(StubAssembler& jit, GPRReg home, GPRReg homeKind, JumpList& callsOperation)
{
    const GPRReg result = jit.result();
    callsOperation.append(jit.branchTest64(CCallHelpers::Zero, result));
    callsOperation.append(jit.branchTest8(CCallHelpers::NonZero, Address(result, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    loadStructure(jit, result, result);
    jit.load64(Address(result, Structure::prototypeOffset()), result);
    callsOperation.append(jit.branchTest64(CCallHelpers::Zero, result));
    jit.returnToCaller();

    callsOperation.link(&jit);
    RELEASE_ASSERT(home == operationArgumentGPR(1) && homeKind == operationArgumentGPR(2));
    jit.move(TrustedImm32(static_cast<int32_t>(Typed::HomeObjectKind::ClassPrototype)), homeKind);
    jit.tailCallOperation(Entry::operationAOTGetSuperBase, OperationCall::FirstArgument::Instance);
}

void generateGetSuperBaseOfClass(StubAssembler& jit)
{
    const GPRReg rank = jit.immediate();
    const GPRReg result = jit.result();
    RELEASE_ASSERT(noOverlap(jit.operand(0), rank, result, instanceGPR));
    JumpList callsOperation;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.classPrototypes)), result);
    jit.load64(BaseIndex(result, rank, CCallHelpers::TimesEight), result);
    loadPrototypeAboveAndReturn(jit, jit.operand(0), rank, callsOperation);
}

void generateGetSuperBaseOfClassObject(StubAssembler& jit)
{
    const GPRReg classObject = jit.operand(0);
    const GPRReg homeKind = jit.allocateScratch(StubRegister::A2);
    const GPRReg result = jit.result();
    RELEASE_ASSERT(noOverlap(classObject, homeKind, result, instanceGPR));
    JumpList callsOperation;
    jit.loadPtr(Address(classObject, JSObject::butterflyOffset()), result);
    callsOperation.append(jit.branchTestPtr(CCallHelpers::Zero, result));
    jit.load64(Address(result, offsetInButterfly(firstOutOfLineOffset) * static_cast<int32_t>(sizeof(EncodedJSValue))), result);
    loadPrototypeAboveAndReturn(jit, classObject, homeKind, callsOperation);
}

StubSignature signatureOfGetSuperBaseOfClassObject(StubKey)
{
    return StubSignature::withGPRResult({ operationArgumentGPR(1) });
}

StubSignature signatureOfGetSuperBaseOfClass(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ operationArgumentGPR(1) });
    signature.immediate = operationArgumentGPR(2);
    return signature;
}

StubSignature signatureOfTryGetSuperById(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::R0) });
    signature.immediate = gprOf(StubRegister::A1);
    return signature;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

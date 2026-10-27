/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosureWord.h"
#include "AOTCompiledStructure.h"
#include "AOTConvention.h"
#include "AOTFieldTable.h"
#include "AOTFunctionData.h"
#include "AOTLayoutData.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubFrames.h"
#include "AOTStubProgramTables.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsField.h"
#include "AOTStubsPropertyExtraProperties.h"
#include "AOTStubsCalls.h"
#include "JSCell.h"
#include "StackAlignment.h"
#include <bit>
#include <wtf/Box.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

static constexpr bool isFreeBeforeCall(StubRegister stubRegister)
{
    const GPRReg reg = gprOf(stubRegister);
    for (unsigned index = 0; index < numberOfArgumentGPRs; ++index) {
        if (reg == argumentGPR(index))
            return false;
    }
    return noOverlap(reg, thisGPR, calleeGPR);
}

static_assert(!exists(callMethodById(numberOfArgumentGPRs + 1)));
static_assert(isFreeBeforeCall(StubRegister::T9) && isFreeBeforeCall(StubRegister::T11) && isFreeBeforeCall(StubRegister::T13));
static_assert(sizeof(PackedSlot) == sizeof(uint64_t));
static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfPayload() == sizeof(uint64_t));

// The name, this and the arguments, and above them room for a slim closure with one capture.
static constexpr unsigned sizeOfClosureInFrame = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(static_cast<unsigned>(offsetOfSlimClosureCaptures + sizeof(EncodedJSValue)));
static constexpr unsigned offsetOfClosureInFrame(unsigned count) { return WTF::roundUpToMultipleOf<stackAlignmentBytes()>((count + 2) * sizeof(EncodedJSValue)); }
static constexpr unsigned bytesSavedForCallMethod(unsigned count) { return offsetOfClosureInFrame(count) + sizeOfClosureInFrame; }

static void readMethodByCell(StubAssembler& jit, GPRReg cell, GPRReg method, GPRReg scratch, JumpList& otherwise)
{
    RELEASE_ASSERT(noOverlap(thisGPR, calleeGPR, cell, method, scratch));
    otherwise.append(branchIfCellNameDiffers(jit, calleeGPR, cell, scratch));
    Jump isNoValue = branchIfSlotKindDiffers(jit, { SlotForm::PackedSlot, cell }, Slot::Kind::Value, scratch);
    loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, cell, InvalidGPRReg), thisGPR, method, scratch, method);
    Jump hasMethod = jit.jump();

    isNoValue.link(&jit);
    otherwise.append(branchIfSlotKindDiffers(jit, { SlotForm::PackedSlot, cell }, Slot::Kind::ExtraProperty, scratch));
    loadExtraPropertyBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::ExtraProperty, cell, InvalidGPRReg), thisGPR, method, scratch, otherwise);
    hasMethod.link(&jit);
}

// The code that calls a method does not know the callee, so what it expects of the call waits for the callee's name.
static void callFoundMethod(StubAssembler& jit, unsigned count)
{
    if (jit.options().validates)
        jit.storePtr(calleeGPR, Address(instanceGPR, AOT_INSTANCE_OFFSET(scratch.expectedCall.callee)));
    jit.jumpToStub(callValueCell(count));
}

// Calls a cell that an own field of the receiver holds. Falls through with every operand as it was where there is none.
static void emitCallMethodInField(StubAssembler& jit, GPRReg index, GPRReg entry, std::optional<uint32_t> boundName)
{
    JumpList otherwise;
    const GPRReg method = index;
    readFieldByTable(jit, { thisGPR, calleeGPR, index, entry, boundName }, method, otherwise, [&] {
        otherwise.append(jit.branchIfNotCell(method));
        jit.move(method, calleeGPR);
        callFoundMethod(jit, jit.key().count());
    });
    otherwise.link(&jit);
}

// A field that defers its closure makes it when the field is read.
static void emitCallOfUncreatedClosure(StubAssembler& jit, unsigned count)
{
    using Form = FieldTableEntry::Form;
    constexpr unsigned fieldSizeShift = WTF::fastLog2(static_cast<unsigned>(sizeof(FieldData)));
    static_assert(sizeof(FieldData) == 1u << fieldSizeShift && sizeof(FieldTableEntry) == 1u << CCallHelpers::TimesFour && sizeof(FieldRecordIndex) == 1u << CCallHelpers::TimesTwo);
    static_assert(sizeof(FunctionKindData) == 1u << CCallHelpers::TimesTwo && offsetOfSlimClosureWord == sizeof(uint64_t) && numberOfSlimClosureKinds == 2);
    static_assert(!uncreatedDeferredClosure && gprOf(StubRegister::A0) == argumentGPR(0) && gprOf(StubRegister::A1) == argumentGPR(1));
    static_assert([] {
        FunctionKindData kind { };
        kind.isAsync = 1;
        return kind.slimClosureKind() == 1 && !FunctionKindData { }.slimClosureKind();
    }());

    // A validating build allocates nothing in frames, and with wide references no slot defers a closure.
    if (jit.options().validates || jit.options().useWideReferences)
        return;

    const GPRReg index = jit.reuseScratch(StubRegister::T13);
    const GPRReg table = jit.reuseScratch(StubRegister::T11);
    const GPRReg word = jit.reuseScratch(StubRegister::T9);
    const GPRReg capture = jit.reuseScratch(StubRegister::A0);
    const GPRReg kind = jit.reuseScratch(StubRegister::A1);
    RELEASE_ASSERT(noOverlap(thisGPR, calleeGPR, index, table, word, capture, kind));
    JumpList otherwise;

    otherwise.append(jit.branchIfNotCell(thisGPR));
    jit.load32SignedExtendTo64(Address(thisGPR, offsetOfShapeWord), index);
    otherwise.append(jit.branchTest32(CCallHelpers::PositiveOrZero, index));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldTableEntries)), table);
    jit.load32(BaseIndex(table, index, CCallHelpers::TimesFour), index);
    jit.add32(calleeGPR, index);
    jit.load32(BaseIndex(table, index, CCallHelpers::TimesFour), word);
    jit.lshift32(calleeGPR, TrustedImm32(FieldTableEntry::nameShift), table);
    jit.xor32(table, word);
    jit.sub32(TrustedImm32(static_cast<int32_t>(Form::InlineNarrowCell) << FieldTableEntry::formShift), word);
    otherwise.append(jit.branch32(CCallHelpers::Above, word, TrustedImm32(FieldTableEntry::maxOperand)));
    jit.load32(BaseIndex(thisGPR, word, CCallHelpers::TimesFour), word);
    otherwise.append(jit.branchTest32(CCallHelpers::NonZero, word));

    // Zero in the slot of any other reference says that the slot is empty.
    const GPRReg field = index;
    const GPRReg layout = table;
    auto inField = [&](ptrdiff_t offset) {
        return Address(field, static_cast<int32_t>(sizeof(LayoutData) + offset));
    };
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.fieldRecordIndices)), table);
    jit.load16(BaseIndex(table, index, CCallHelpers::TimesTwo), index);
    jit.and32(TrustedImm32(FieldRecordIndex::indexMask), index);
    jit.lshift32(TrustedImm32(fieldSizeShift), index);
    loadStructure(jit, thisGPR, layout);
    jit.loadPtr(Address(layout, CompiledStructure::offsetOfLayout()), layout);
    jit.addPtr(layout, field);
    jit.load32(inField(OBJECT_OFFSETOF(FieldData, slot)), word);
    jit.urshift32(TrustedImm32(SlotData::offsetBits), word);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, word, TrustedImm32(static_cast<int32_t>(SlotRepresentation::DeferredClosure))));

    const GPRReg operands = layout;
    const GPRReg function = word;
    jit.load16(Address(layout, OBJECT_OFFSETOF(LayoutData, numberOfFields)), word);
    jit.lshift32(TrustedImm32(fieldSizeShift), word);
    jit.addPtr(word, layout);
    jit.load16(inField(OBJECT_OFFSETOF(FieldData, operandIndex)), word);
    jit.getEffectiveAddress(BaseIndex(layout, word, CCallHelpers::TimesFour, sizeof(LayoutData)), operands);
    jit.load32(Address(operands), function);
    otherwise.append(jit.branch32(CCallHelpers::Above, function, TrustedImm32(ClosureWord::functionMask)));
    loadKindOfClosureFunction(jit, function, kind);
    otherwise.append(jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(slimFunctionKindBit())));

    // The object itself, or the environment in its hidden field. A function without captures reads neither.
    const GPRReg flags = field;
    jit.load16(inField(OBJECT_OFFSETOF(FieldData, flags)), flags);
    jit.move(thisGPR, capture);
    Jump capturesNoEnvironment = jit.branchTest32(CCallHelpers::Zero, flags, TrustedImm32(fieldDeferredClosureCapturesEnvironment));
    jit.load32(Address(operands, sizeof(uint32_t)), capture);
    jit.urshift32(capture, TrustedImm32(SlotData::offsetBits), flags);
    otherwise.append(jit.branch32(CCallHelpers::NotEqual, flags, TrustedImm32(static_cast<int32_t>(SlotRepresentation::Cell32))));
    jit.and32(TrustedImm32(SlotData::maxOffset), capture);
    jit.load32(BaseIndex(thisGPR, capture, CCallHelpers::TimesOne), capture);
    otherwise.append(jit.branch32(CCallHelpers::Below, capture, TrustedImm32(minCell32)));
    decodeCell32(jit, capture, capture);
    capturesNoEnvironment.link(&jit);

    const GPRReg header = operands;
    const GPRReg closureWord = function;
    const int32_t offsetOfClosure = offsetOfClosureInFrame(count);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), header);
    jit.load64(BaseIndex(header, function, CCallHelpers::TimesEight), closureWord);
    otherwise.append(jit.branchTest64(CCallHelpers::Zero, closureWord));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders)), header);
    Jump isNotAsync = jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(asyncFunctionKindBit()));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders) + sizeof(uint64_t)), header);
    isNotAsync.link(&jit);
    jit.storePair64(header, closureWord, Address(sp, offsetOfClosure));
    jit.store64(capture, Address(sp, offsetOfClosure + static_cast<int32_t>(offsetOfSlimClosureCaptures)));
    bumpRuntimeCounter(jit, RuntimeCounterID::CalledClosuresNotCreated, kind);

    // The callee is done with the cell when it returns or leaves by a tail call, which is what holds for the words of a list of arguments.
    for (unsigned i = 0; i < count; ++i)
        jit.load64(Address(sp, (i + 2) * sizeof(EncodedJSValue)), argumentGPR(i));
    jit.addPtr(TrustedImm32(offsetOfClosure), sp, calleeGPR);
    jit.move(TrustedImm32(count), countGPR);
    jit.loadStubAddress(callValueCell(count), jit.clobberRegister(callTargetRegister));
    jit.jumpToStub(callFromArgumentListFrame());

    otherwise.link(&jit);
}

static void emitReadMethodAndCall(StubAssembler& jit, unsigned count)
{
    const GPRReg R0 = jit.result();
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(bytesSavedForCallMethod(count)), sp);
    jit.store64(calleeGPR, Address(sp));
    jit.store64(thisGPR, Address(sp, sizeof(EncodedJSValue)));
    for (unsigned i = 0; i < count; ++i)
        jit.store64(argumentGPR(i), Address(sp, (i + 2) * sizeof(EncodedJSValue)));
    emitCallOfUncreatedClosure(jit, count);
    jit.move(calleeGPR, jit.reuseScratch(slotNameRegister));
    jit.move(thisGPR, R0);
    jit.callStub(getById());
    // Whatever the read throws or runs is shown where the read is written. The call comes behind.
    jit.bindReturnLabel(StubFrameKind::MethodRead);
    Jump notCell = jit.branchIfNotCell(R0);
    jit.move(R0, calleeGPR);
    jit.load64(Address(sp, sizeof(EncodedJSValue)), thisGPR);
    for (unsigned i = 0; i < count; ++i)
        jit.load64(Address(sp, (i + 2) * sizeof(EncodedJSValue)), argumentGPR(i));
    jit.emitFunctionEpilogue();
    callFoundMethod(jit, count);

    notCell.link(&jit);
    jit.loadPtr(Address(sp), jit.reuseScratch(StubRegister::A2));
    jit.emitFunctionEpilogue();
    jit.callOperationAndReturn(Entry::operationAOTThrowUnlessCallableNamed, { .returns = OperationCall::Returns::Void, .secondArgumentIn = R0 });
}

void generateCallMethodById(StubAssembler& jit)
{
    const unsigned count = jit.key().count();
    const GPRReg cell = jit.allocateScratch(StubRegister::T13);
    const GPRReg method = jit.allocateScratch(StubRegister::T11);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T9);
    const std::optional<uint32_t> boundName = jit.key().immediate();
    RELEASE_ASSERT(!boundName || RowDisplacementTable::hasColumn(*boundName));
    JumpList otherwise;
    Box<JumpList> hasNoStructure = Box<JumpList>::create();

    emitCallMethodInField(jit, cell, scratch, boundName);
    if (boundName) {
        jit.move(TrustedImm32(*boundName), calleeGPR);
        otherwise.append(jit.branchIfNotCell(thisGPR));
        jit.load32(Address(thisGPR, JSCell::structureIDOffset()), method);
        loadFrontRow(jit, SlotAccess::Read, method, cell, scratch);
        jit.load64(Address(cell, static_cast<int32_t>(*boundName * sizeof(PackedSlot))), cell);
    } else {
        loadStructureIDOfReceiver(jit, thisGPR, method, *hasNoStructure);
#if CPU(ARM64)
        static_assert(isFreeBeforeCall(StubRegister::T14));
        loadFrontCell(jit, SlotAccess::Read, { method, calleeGPR, scratch, cell, jit.allocateScratch(StubRegister::T14) });
#else
        // The calling convention leaves three registers free, one fewer than loadFrontCell takes.
        loadFrontRow(jit, SlotAccess::Read, method, cell, scratch);
        jit.load64(BaseIndex(cell, calleeGPR, CCallHelpers::TimesEight), cell);
#endif
    }
    readMethodByCell(jit, cell, method, scratch, otherwise);
    otherwise.append(jit.branchIfNotCell(method));
    jit.move(method, calleeGPR);
    callFoundMethod(jit, count);

    if (boundName) {
        jit.linkToStub(otherwise, jit.key().withoutImmediate());
        return;
    }
    otherwise.link(&jit);
    // loadStructureIDOfReceiver adds to its list in an out-of-line part, which runs after the generator has returned.
    jit.addOutOfLinePart([hasNoStructure, target = jit.label()](StubAssembler& jit) {
        hasNoStructure->linkTo(target, &jit);
    });
    emitReadMethodAndCall(jit, count);
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

    // A data property is the same for every receiver, so whatever is filed for the object that the search starts at answers, but an accessor.
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
        loadStaticsBase(jit, cell, structureID);
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

// What is above the prototype object of a class that is evaluated once: where `super.name` in a method of the class begins to look.
void generateGetSuperBaseOfClass(StubAssembler& jit)
{
    const GPRReg rank = jit.immediate();
    const GPRReg result = jit.result();
    RELEASE_ASSERT(jit.operand(0) == operationArgumentGPR(1) && rank == operationArgumentGPR(2) && noOverlap(jit.operand(0), rank, result, instanceGPR));
    JumpList callsOperation;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(classes.classPrototypes)), result);
    jit.load64(BaseIndex(result, rank, CCallHelpers::TimesEight), result);
    callsOperation.append(jit.branchTest64(CCallHelpers::Zero, result));
    callsOperation.append(jit.branchTest8(CCallHelpers::NonZero, Address(result, JSCell::typeInfoFlagsOffset()), TrustedImm32(OverridesGetPrototype)));
    loadStructure(jit, result, result);
    jit.load64(Address(result, Structure::prototypeOffset()), result);
    callsOperation.append(jit.branchTest64(CCallHelpers::Zero, result));
    jit.returnToCaller();

    callsOperation.link(&jit);
    jit.move(TrustedImm32(static_cast<int32_t>(Typed::HomeObjectKind::ClassPrototype)), rank);
    jit.tailCallOperation(Entry::operationAOTGetSuperBase, OperationCall::FirstArgument::Instance);
}

StubSignature signatureOfCallMethodById(StubKey)
{
    StubSignature signature = callConventionSignature({ });
    signature.immediate = calleeGPR;
    signature.boundImmediates = StubBoundImmediates::NamesRead;
    return signature;
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

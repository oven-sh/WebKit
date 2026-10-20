/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsPropertyProbes.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTPropertyCachesFront.h"
#include "AOTStubsCalls.h"
#include "JSCell.h"
#include "StackAlignment.h"
#include <wtf/Box.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

#if CPU(X86_64)
static constexpr StubRegister methodSiteCacheRegister = StubRegister::T11;
#else
static constexpr StubRegister methodSiteCacheRegister = readSiteCacheRegister;
#endif

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
static_assert(isFreeBeforeCall(StubRegister::T9) && isFreeBeforeCall(StubRegister::T11) && isFreeBeforeCall(StubRegister::T13) && isFreeBeforeCall(methodSiteCacheRegister));
static_assert(sizeof(PackedSlot) == sizeof(uint64_t));
static_assert(!Slot::offsetOfStructureID() && Slot::offsetOfPayload() == sizeof(uint64_t));

struct MethodProbeRegisters {
    static MethodProbeRegisters allocate(StubAssembler& jit)
    {
        return { jit.allocateScratch(StubRegister::T13), jit.allocateScratch(StubRegister::T11), jit.allocateScratch(StubRegister::T9) };
    }

    GPRReg cell;
    GPRReg method;
    GPRReg scratch;
};

struct MethodProbeExits {
    JumpList otherwise;
    Box<JumpList> hasNoStructure { Box<JumpList>::create() };
};

static constexpr unsigned bytesSavedForCallMethod(unsigned count) { return WTF::roundUpToMultipleOf<stackAlignmentBytes()>((count + 2) * sizeof(EncodedJSValue)); }

static constexpr StubKey getByIdInStubFrame()
{
    return getById().withCallerFrame(CallerFrame::StubFrame);
}

template<typename Functor>
static void preservingThisAndArguments(StubAssembler& jit, GPRReg extra, const Functor& functor)
{
#if CPU(ARM64)
    static_assert(numberOfArgumentGPRs == 8);
    jit.preserving({ thisGPR, extra, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3), argumentGPR(4), argumentGPR(5), argumentGPR(6), argumentGPR(7) }, functor);
#else
    static_assert(numberOfArgumentGPRs == 4);
    jit.preserving({ thisGPR, extra, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3) }, functor);
#endif
}

static void readMethodByCell(StubAssembler& jit, GPRReg cell, GPRReg method, GPRReg scratch, JumpList& otherwise)
{
    RELEASE_ASSERT(noOverlap(thisGPR, calleeGPR, cell, method, scratch));
    otherwise.append(branchIfCellNameDiffers(jit, calleeGPR, cell, scratch));
    otherwise.append(branchIfSlotKindDiffers(jit, { SlotForm::PackedSlot, cell }, Slot::Kind::Value, scratch));
    loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, cell, InvalidGPRReg), thisGPR, method, scratch, method);
}

// finish takes the register with a cell that an own field of the receiver holds. Falls through with every operand as it was where there is none.
static void emitProbeMethodInField(StubAssembler& jit, GPRReg index, GPRReg entry, std::optional<uint32_t> boundName, bool requiresCallable, const ScopedLambda<void(GPRReg method)>& finish, GPRReg site = InvalidGPRReg)
{
    JumpList otherwise;
    if (site != InvalidGPRReg)
        otherwise.append(branchIfSiteIsFilled(jit, site));
    const GPRReg method = index;
    readFieldByTable(jit, { thisGPR, calleeGPR, index, entry, boundName }, method, otherwise, [&] {
        otherwise.append(jit.branchIfNotCell(method));
        if (requiresCallable)
            otherwise.append(branchIfNotFunctionType(jit, method, entry));
        finish(method);
    });
    otherwise.link(&jit);
}

static void returnMethodInField(StubAssembler& jit, GPRReg index, GPRReg entry, bool requiresCallable, GPRReg site = InvalidGPRReg)
{
    emitProbeMethodInField(jit, index, entry, std::nullopt, requiresCallable, [&](GPRReg method) {
        jit.move(method, calleeGPR);
        jit.returnToCaller();
    }, site);
}

static void emitProbeMethod(StubAssembler& jit, const MethodProbeRegisters& registers, MethodProbeExits& exits)
{
    loadStructureIDOfReceiver(jit, thisGPR, registers.method, *exits.hasNoStructure);
#if CPU(ARM64)
    static_assert(isFreeBeforeCall(StubRegister::T14));
    loadFrontCell(jit, SlotAccess::Read, { registers.method, calleeGPR, registers.scratch, registers.cell, jit.allocateScratch(StubRegister::T14) });
#else
    // The calling convention leaves three registers free, one fewer than loadFrontCell takes.
    loadFrontRow(jit, SlotAccess::Read, registers.method, registers.cell, registers.scratch);
    jit.load64(BaseIndex(registers.cell, calleeGPR, CCallHelpers::TimesEight), registers.cell);
#endif
    readMethodByCell(jit, registers.cell, registers.method, registers.scratch, exits.otherwise);
    exits.otherwise.append(jit.branchIfNotCell(registers.method));
}

static void linkMethodProbeExits(StubAssembler& jit, const MethodProbeExits& exits)
{
    exits.otherwise.link(&jit);
    // loadStructureIDOfReceiver adds to its list in an out-of-line part, which runs after the generator has returned.
    jit.addOutOfLinePart([hasNoStructure = exits.hasNoStructure, target = jit.label()](StubAssembler& jit) {
        hasNoStructure->linkTo(target, &jit);
    });
}

static void emitMethodValueCheck(StubAssembler& jit, GPRReg value)
{
    Jump exception;
    jit.emitFunctionPrologue();
    preservingThisAndArguments(jit, value, [&] {
        const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
        const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
        const GPRReg A2 = jit.reuseScratch(StubRegister::A2);
        jit.move(calleeGPR, A2);
        jit.move(value, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTThrowUnlessCallableNamed, jit.reuseScratch(StubRegister::Operation));
        exception = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR);
    });
    jit.emitFunctionEpilogue();
    jit.move(value, calleeGPR);
    jit.returnToCaller();

    exception.link(&jit);
    jit.emitFunctionEpilogue();
    jit.jumpToExceptionHandler();
}

void generateFindMethodById(StubAssembler& jit)
{
    const bool requiresCallable = static_cast<FoundMethod>(jit.key().form()) == FoundMethod::Callable;
    const MethodProbeRegisters registers = MethodProbeRegisters::allocate(jit);
    const GPRReg method = registers.method;
    MethodProbeExits exits;
    JumpList needsCheck;

    returnMethodInField(jit, registers.cell, registers.scratch, requiresCallable);
    emitProbeMethod(jit, registers, exits);
    if (requiresCallable)
        needsCheck.append(branchIfNotFunctionType(jit, method, registers.scratch));
    jit.move(method, calleeGPR);
    jit.returnToCaller();

    linkMethodProbeExits(jit, exits);
    jit.emitFunctionPrologue();
    preservingThisAndArguments(jit, calleeGPR, [&] {
        const GPRReg R0 = jit.reuseScratch(firstStubOperandRegister);
        jit.move(calleeGPR, jit.reuseScratch(slotNameRegister));
        jit.move(thisGPR, R0);
        jit.callStub(getByIdInStubFrame());
        jit.move(R0, method);
    });
    jit.emitFunctionEpilogue();
    needsCheck.append(jit.branchIfNotCell(method));
    if (requiresCallable)
        needsCheck.append(branchIfNotFunctionType(jit, method, registers.scratch));
    jit.move(method, calleeGPR);
    jit.returnToCaller();

    needsCheck.link(&jit);
    if (requiresCallable) {
        emitMethodValueCheck(jit, method);
        return;
    }
    // The error belongs to the call, which has a site of its own and no name.
    jit.add32(TrustedImm32(1), calleeGPR);
    jit.store32(calleeGPR, Address(instanceGPR, AOT_INSTANCE_OFFSET(runtime.exceptions.nameOfUncallableMethod)));
    jit.move(method, calleeGPR);
    jit.returnToCaller();
}

void generateFindMethodByIdAtSite(StubAssembler& jit)
{
    const GPRReg site = jit.clobberOperand(2);
    const GPRReg header = jit.allocateScratch(StubRegister::T13);
    const GPRReg method = jit.allocateScratch(StubRegister::T9);
    const StubKey findMethod = findMethodById(FoundMethod::Cell);
    JumpList isDue;

    returnMethodInField(jit, header, method, false, site);
    jit.linkToStub(jit.branchIfNotCell(thisGPR), findMethod);
    jit.loadPair64(site, header, method);
    Jump hasOtherStructure = jit.branch32(CCallHelpers::NotEqual, header, Address(thisGPR, JSCell::structureIDOffset()));
    jit.linkToStub(branchIfSlotKindDiffers(jit, { SlotForm::Slot, header, method }, Slot::Kind::Value, site), findMethod);
    loadValueBySlot(jit, CachedSlot(SlotForm::Slot, Slot::Kind::Value, header, method), thisGPR, method, site, method);
    jit.linkToStub(jit.branchIfNotCell(method), findMethod);
    jit.move(method, calleeGPR);
    jit.returnToCaller();

    hasOtherStructure.link(&jit);
    Jump isEmpty = jit.branchTest64(CCallHelpers::Zero, header);
    countSiteCacheMiss(jit, site, header, isDue);
    jit.jumpToStub(findMethod);

    isEmpty.link(&jit);
    countMissOfEmptySite(jit, site, header, isDue);
    jit.jumpToStub(findMethod);

    isDue.link(&jit);
    refillSiteCache(jit, thisGPR, calleeGPR, site, SlotAccess::Read);
    jit.jumpToStub(findMethod);
}

// The code that calls a method does not know the callee, so what it expects of the call waits for the callee's name.
static void callFoundMethod(StubAssembler& jit, unsigned count)
{
    if (jit.options().validates)
        jit.storePtr(calleeGPR, Address(instanceGPR, AOT_INSTANCE_OFFSET(scratch.expectedCall.callee)));
    jit.jumpToStub(callValueCell(count));
}

static void emitCallMethodWithBoundName(StubAssembler& jit, uint32_t number, const MethodProbeRegisters& registers)
{
    RELEASE_ASSERT(RowDisplacementTable::hasColumn(number));
    JumpList otherwise;

    emitProbeMethodInField(jit, registers.cell, registers.scratch, number, false, [&](GPRReg method) {
        jit.move(method, calleeGPR);
        callFoundMethod(jit, jit.key().count());
    });
    jit.move(TrustedImm32(number), calleeGPR);
    otherwise.append(jit.branchIfNotCell(thisGPR));
    jit.load32(Address(thisGPR, JSCell::structureIDOffset()), registers.method);
    loadFrontRow(jit, SlotAccess::Read, registers.method, registers.cell, registers.scratch);
    jit.load64(Address(registers.cell, static_cast<int32_t>(number * sizeof(PackedSlot))), registers.cell);
    readMethodByCell(jit, registers.cell, registers.method, registers.scratch, otherwise);
    otherwise.append(jit.branchIfNotCell(registers.method));
    jit.move(registers.method, calleeGPR);
    callFoundMethod(jit, jit.key().count());

    jit.linkToStub(otherwise, jit.key().withoutImmediate());
}

static void emitReadMethodAndCall(StubAssembler& jit, unsigned count)
{
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    const GPRReg R0 = jit.result();
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(bytesSavedForCallMethod(count)), sp);
    jit.store64(calleeGPR, Address(sp));
    jit.store64(thisGPR, Address(sp, sizeof(EncodedJSValue)));
    for (unsigned i = 0; i < count; ++i)
        jit.store64(argumentGPR(i), Address(sp, (i + 2) * sizeof(EncodedJSValue)));
    jit.move(calleeGPR, jit.reuseScratch(slotNameRegister));
    jit.move(thisGPR, R0);
    jit.callStub(getByIdInStubFrame());
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
    const MethodProbeRegisters registers = MethodProbeRegisters::allocate(jit);
    if (std::optional<uint32_t> number = jit.key().immediate()) {
        emitCallMethodWithBoundName(jit, *number, registers);
        return;
    }

    MethodProbeExits exits;
    emitProbeMethodInField(jit, registers.cell, registers.scratch, std::nullopt, false, [&](GPRReg method) {
        jit.move(method, calleeGPR);
        callFoundMethod(jit, count);
    });
    emitProbeMethod(jit, registers, exits);
    jit.move(registers.method, calleeGPR);
    callFoundMethod(jit, count);

    linkMethodProbeExits(jit, exits);
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
    jit.load32(Address(R0, JSCell::structureIDOffset()), structureID);
    loadFrontCell(jit, SlotAccess::Read, { structureID, name, row, cell, scratch });
    needsGenericRead.append(branchIfCellNameDiffers(jit, name, cell, scratch));
    Jump isNotValue = branchIfSlotKindDiffers(jit, slot, Slot::Kind::Value, scratch);
    loadValueBySlot(jit, CachedSlot(SlotForm::PackedSlot, Slot::Kind::Value, cell, InvalidGPRReg), R0, row, scratch, R0);
    jit.returnToCaller();

    isNotValue.link(&jit);
    needsGenericRead.append(branchIfSlotKindDiffers(jit, slot, Slot::Kind::Absent, scratch));
    jit.move(TrustedImm64(JSValue::ValueUndefined), R0);
    jit.returnToCaller();

    needsGenericRead.link(&jit);
    jit.move(TrustedImm32(0), R0);
    jit.returnToCaller();
}

static StubSignature findMethodSignature()
{
    StubSignature signature;
    signature.addOperand(thisGPR);
    signature.addOperand(calleeGPR);
    signature.result = StubResult::GPR;
    signature.firstResult = calleeGPR;
    return signature;
}

StubSignature signatureOfFindMethodById(StubKey) { return findMethodSignature(); }

StubSignature signatureOfFindMethodByIdAtSite(StubKey)
{
    StubSignature signature = findMethodSignature();
    signature.addOperand(gprOf(methodSiteCacheRegister));
    return signature;
}

StubSignature signatureOfCallMethodById(StubKey)
{
    StubSignature signature = callConventionSignature({ });
    signature.immediate = calleeGPR;
    signature.boundImmediates = StubBoundImmediates::NamesRead;
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

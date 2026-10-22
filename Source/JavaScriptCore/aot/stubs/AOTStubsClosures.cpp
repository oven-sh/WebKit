/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsClosures.h"

#if ENABLE(AOT)

#include "AOTAllocationSiteRecord.h"
#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTSlimClosure.h"
#include "AOTStubHelpers.h"
#include "AOTStubProgramTables.h"
#include "AOTStubRegisters.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsCalls.h"
#include "CellState.h"
#include "JSFunction.h"
#include <optional>

namespace JSC { namespace AOT {

static_assert(ClosureRequest::maxSlimClosureCaptures == 4);
static_assert(ClosureRequest::maxNumberOfCaptures <= ClosureRequest::maxSlimClosureCaptures);
static_assert(frameCellState > tautologicalThreshold);

struct ClosureRegisters {
    GPRReg cell;
    GPRReg request;
    GPRReg firstScratch;
    GPRReg secondScratch;
};

static constexpr GPRReg captureGPR(unsigned slot)
{
    return GPRInfo::toArgumentRegister(2 + slot);
}

static_assert([] {
    for (unsigned slot = 0; slot < ClosureRequest::maxSlimClosureCaptures; ++slot) {
        if (captureGPR(slot) != operationArgumentGPR(2 + slot))
            return false;
    }
    return true;
}());

static ClosureRegisters allocateClosureRegisters(StubAssembler& jit)
{
    return {
        .cell = jit.result(),
        .request = jit.operand(0),
        .firstScratch = jit.allocateScratch(StubRegister::T12),
        .secondScratch = jit.allocateScratch(StubRegister::T13),
    };
}

static StubSignature closureRequestSignature(unsigned numberOfCaptures)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T9) });
    for (unsigned slot = 0; slot < numberOfCaptures; ++slot)
        signature.addOperand(captureGPR(slot));
    return signature;
}

// Above maxNumberOfCaptures every request is slim, and on x86-64 T11 is the fourth capture.
static std::optional<Jump> branchIfFunctionObject(StubAssembler& jit, GPRReg request)
{
    const TrustedImm32 slimBit(static_cast<int32_t>(1u << ClosureRequest::isSlimBit));
    if (jit.key().count() <= ClosureRequest::maxNumberOfCaptures)
        return jit.branchTest32(CCallHelpers::Zero, request, slimBit);
    if (jit.options().validates) {
        Jump isSlim = jit.branchTest32(CCallHelpers::NonZero, request, slimBit);
        jit.breakpoint();
        isSlim.link(&jit);
    }
    return std::nullopt;
}

static void storeCaptures(StubAssembler& jit, GPRReg cell, ptrdiff_t offsetOfCaptures)
{
    const unsigned numberOfCaptures = jit.key().count();
    auto addressOf = [&](unsigned slot) {
        return Address(cell, offsetOfCaptures + slot * sizeof(uint64_t));
    };
    for (unsigned slot = 0; slot + 1 < numberOfCaptures; slot += 2)
        jit.storePair64(captureGPR(slot), captureGPR(slot + 1), addressOf(slot));
    if (numberOfCaptures % 2)
        jit.store64(captureGPR(numberOfCaptures - 1), addressOf(numberOfCaptures - 1));
}

static void loadClosureWord(StubAssembler& jit, GPRReg request, GPRReg word, GPRReg scratch)
{
    ASSERT(noOverlap(request, word, scratch));
    jit.and32(TrustedImm32(ClosureWord::functionMask), request, word);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureWords)), scratch);
    jit.load64(BaseIndex(scratch, word, CCallHelpers::TimesEight), word);
}

static void initializeSlimClosure(StubAssembler& jit, const ClosureRegisters& registers)
{
    const GPRReg cell = registers.cell;
    const GPRReg request = registers.request;
    const GPRReg word = registers.firstScratch;
    const GPRReg header = registers.secondScratch;
    static_assert(offsetOfSlimClosureWord == sizeof(uint64_t));
    // The slim bit is part of the index, and the displacement takes it out again.
    constexpr ptrdiff_t slimBitInIndex = sizeof(uint64_t) << (ClosureRequest::isSlimBit - ClosureRequest::executableShift);
    constexpr ptrdiff_t offsetOfHeaders = static_cast<ptrdiff_t>(AOT_INSTANCE_OFFSET(slimClosureHeaders)) - slimBitInIndex;

    loadClosureWord(jit, request, word, header);
    jit.urshift32(request, TrustedImm32(ClosureRequest::executableShift), header);
    jit.load64(BaseIndex(instanceGPR, header, CCallHelpers::TimesEight, offsetOfHeaders), header);
    jit.storePair64(header, word, Address(cell));
    storeCaptures(jit, cell, offsetOfSlimClosureCaptures);
}

static void initializeFunctionObjectFields(StubAssembler& jit, const ClosureRegisters& registers)
{
    const GPRReg cell = registers.cell;
    const GPRReg request = registers.request;
    const GPRReg index = registers.firstScratch;
    const GPRReg scratch = registers.secondScratch;
    static_assert(JSFunction::offsetOfExecutableOrRareData() == JSCallee::offsetOfScopeChain() + static_cast<ptrdiff_t>(sizeof(void*)));

    jit.store64(TrustedImm64(0), Address(cell, JSObject::butterflyOffset()));
    jit.urshift32(request, TrustedImm32(ClosureRequest::executableShift), index);
    jit.loadPtr(BaseIndex(instanceGPR, index, CCallHelpers::TimesEight, AOT_INSTANCE_OFFSET(closures.executables)), index);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), scratch);
    jit.storePair64(scratch, index, Address(cell, JSCallee::offsetOfScopeChain()));
    loadClosureWord(jit, request, index, scratch);
    jit.store64(index, Address(cell, offsetOfClosureEntry));
    storeCaptures(jit, cell, offsetOfClosureCaptures);
}

static void allocateFunctionObjectAndReturn(StubAssembler& jit, const ClosureRegisters& registers, GPRReg record, JumpList& slow)
{
    const GPRReg cell = registers.cell;
    const GPRReg request = registers.request;
    const GPRReg allocator = registers.firstScratch;
    const GPRReg scratch = registers.secondScratch;

    // A request from code has no bit at or above countShift.
    jit.urshift64(request, TrustedImm32(ClosureRequest::allocationShift), allocator);
    loadAllocationSiteRecord(jit, allocator, record, allocator);
    jit.loadPtr(Address(record, AllocationSiteRecord::offsetOfAllocator()), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, scratch, slow);
    jit.load64(Address(record, AllocationSiteRecord::offsetOfHeader()), scratch);
    jit.store64(scratch, Address(cell));
    initializeFunctionObjectFields(jit, registers);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slow);
}

static void createClosureInOperation(StubAssembler& jit, GPRReg request, uint64_t restOfRequest, GPRReg wholeRequest)
{
    jit.or64(TrustedImm64(static_cast<int64_t>(restOfRequest)), request, wholeRequest);
    jit.callOperationAndReturn(Entry::operationAOTNewClosureSlow, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = wholeRequest,
    });
}

StubSignature signatureOfNewClosure(StubKey key)
{
    return closureRequestSignature(key.count());
}

void generateNewClosure(StubAssembler& jit)
{
    const ClosureRegisters registers = allocateClosureRegisters(jit);
    const GPRReg request = registers.request;

    JumpList slow;
    allocateFunctionObjectAndReturn(jit, registers, jit.allocateScratch(StubRegister::T11), slow);

    slow.link(&jit);
    createClosureInOperation(jit, request, static_cast<uint64_t>(jit.key().count()) << ClosureRequest::countShift, registers.firstScratch);
}

StubSignature signatureOfNewSlimClosure(StubKey key)
{
    return closureRequestSignature(key.count());
}

void generateNewSlimClosure(StubAssembler& jit)
{
    const ClosureRegisters registers = allocateClosureRegisters(jit);
    const GPRReg cell = registers.cell;
    const GPRReg function = registers.request;
    const GPRReg allocator = registers.firstScratch;
    const GPRReg word = registers.firstScratch;
    const GPRReg header = registers.secondScratch;
    const unsigned kind = jit.key().form();
    const unsigned count = jit.key().count();
    // Allocator i serves cells of 16 * (i + 1) bytes.
    const unsigned allocatorIndex = (count + 1) / 2;
    static_assert((ClosureRequest::maxSlimClosureCaptures + 1) / 2 < ClosureWords::numberOfSlimClosureAllocators);
    static_assert(offsetOfSlimClosureWord == sizeof(uint64_t));

    JumpList slow;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.slimClosureAllocators) + allocatorIndex * sizeof(void*)), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, header, slow, MarkedSpace::sizeStep * (allocatorIndex + 1));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureWords)), word);
    jit.load64(BaseIndex(word, function, CCallHelpers::TimesEight), word);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind * sizeof(uint64_t)), header);
    jit.storePair64(header, word, Address(cell));
    storeCaptures(jit, cell, offsetOfSlimClosureCaptures);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slow);
    slow.link(&jit);
    uint64_t restOfRequest = static_cast<uint64_t>(kind) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(count) << ClosureRequest::countShift;
    createClosureInOperation(jit, function, restOfRequest, registers.firstScratch);
}

StubSignature signatureOfClosureOfTable(StubKey)
{
    return closureRequestSignature(0);
}

void generateClosureOfTable(StubAssembler& jit)
{
    static_assert(sizeof(Number24) == 3 && std::endian::native == std::endian::little);
    const ClosureRegisters registers = allocateClosureRegisters(jit);
    const GPRReg cell = registers.cell;
    const GPRReg function = registers.request;
    const GPRReg tables = registers.firstScratch;
    const GPRReg numbers = registers.secondScratch;
    const GPRReg number = cell;
    const GPRReg cells = registers.firstScratch;

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    loadProgramTable(jit, tables, ProgramTableIndex::closureCellNumbers, numbers);
    jit.move(function, number);
    jit.sub32(Address(tables, OBJECT_OFFSETOF(ProgramTables, closureFunctions) + OBJECT_OFFSETOF(ClosureFunctionRangeData, first)), number);
    jit.getEffectiveAddress(BaseIndex(number, number, CCallHelpers::TimesTwo), number);
    const GPRReg lowBytes = tables;
    jit.load16(BaseIndex(numbers, number, CCallHelpers::TimesOne), lowBytes);
    jit.load8(BaseIndex(numbers, number, CCallHelpers::TimesOne, 2), number);
    jit.lshift32(TrustedImm32(16), number);
    jit.or32(lowBytes, number);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closureCells)), cells);
    Jump isNotCreated;
    if (jit.options().useWideReferences) {
        jit.load64(BaseIndex(cells, number, CCallHelpers::TimesEight), cell);
        isNotCreated = jit.branchTest64(CCallHelpers::Zero, cell);
    } else {
        jit.load32(BaseIndex(cells, number, CCallHelpers::TimesFour), cell);
        isNotCreated = jit.branchTest32(CCallHelpers::Zero, cell);
        jit.add32(TrustedImm32(1), cell);
        decodeCell32(jit, cell, cell);
    }
    jit.returnToCaller();

    isNotCreated.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTSingletonClosure, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = function,
    });
}

StubSignature signatureOfInitializeClosure(StubKey key)
{
    StubSignature signature = closureRequestSignature(key.count());
    signature.addOperand(signature.firstResult);
    return signature;
}

void generateInitializeClosure(StubAssembler& jit)
{
    const ClosureRegisters registers = allocateClosureRegisters(jit);
    const GPRReg cell = registers.cell;
    const GPRReg scratch = registers.secondScratch;

    std::optional<Jump> isFunctionObject = branchIfFunctionObject(jit, registers.request);
    initializeSlimClosure(jit, registers);
    jit.returnToCaller();
    if (!isFunctionObject)
        return;

    isFunctionObject->link(&jit);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0])), scratch);
    jit.store64(scratch, Address(cell));
    initializeFunctionObjectFields(jit, registers);
    jit.returnToCaller();
}

StubSignature signatureOfMaterializeEnvironment(StubKey)
{
    return StubSignature::withGPRResult({ GPRInfo::returnValueGPR, gprOf(StubRegister::T9) });
}

void generateMaterializeEnvironment(StubAssembler& jit)
{
    const GPRReg cell = jit.result();
    const GPRReg word = jit.operand(1);
    const GPRReg source = jit.allocateScratch(StubRegister::T14);
    const GPRReg record = jit.allocateScratch(StubRegister::T11);
    const GPRReg allocator = jit.allocateScratch(StubRegister::T12);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);

    Jump isOnHeap = jit.branch8(CCallHelpers::NotEqual, Address(cell, JSCell::cellStateOffset()), TrustedImm32(frameCellState));
    jit.move(cell, source);
    JumpList slow;
    jit.zeroExtend32ToWord(word, allocator);
    loadAllocationSiteRecord(jit, allocator, record, allocator);
    jit.loadPtr(Address(record, AllocationSiteRecord::offsetOfAllocator()), allocator);
    allocateCell(jit, cell, allocator, scratch, slow);
    jit.load64(Address(record, AllocationSiteRecord::offsetOfHeader()), scratch);
    jit.store64(scratch, Address(cell));

    const GPRReg size = allocator;
    const GPRReg offset = record;
    jit.urshift64(word, TrustedImm32(32), size);
    jit.move(TrustedImm32(sizeof(JSCell)), offset);
    Label copyWord = jit.label();
    jit.load64(BaseIndex(source, offset, CCallHelpers::TimesOne), scratch);
    jit.store64(scratch, BaseIndex(cell, offset, CCallHelpers::TimesOne));
    jit.add64(TrustedImm32(sizeof(uint64_t)), offset);
    jit.branch64(CCallHelpers::Below, offset, size).linkTo(copyWord, &jit);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    slow.link(&jit);
    const GPRReg allocation = allocator;
    jit.zeroExtend32ToWord(word, allocation);
    jit.callOperationAndReturn(Entry::operationAOTMaterializeEnvironment, {
        .returns = OperationCall::Returns::Value,
        .firstArgument = OperationCall::FirstArgument::Instance,
        .saves = OperationCall::Saves::CallerSavedRegisters,
        .checksException = false,
        .secondArgumentIn = allocation,
        .thirdArgumentIn = source,
    });

    isOnHeap.link(&jit);
    jit.returnToCaller();
}

void loadSlimClosureWord(StubAssembler& jit, GPRReg closure, GPRReg word)
{
    ASSERT(noOverlap(closure, word, instanceGPR));
    jit.load64(Address(closure, offsetOfSlimClosureWord), word);
    Jump isSlimClosure = jit.branchIfType(closure, SlimClosureType);
    jit.load32(Address(word, ReifiedSlimClosure::offsetOfFunctionIndexInButterfly()), word);
    jit.lshift64(TrustedImm32(3), word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureWords)), word);
    jit.load64(Address(word), word);
    isSlimClosure.link(&jit);
}

StubSignature signatureOfValueEndsItsTailCalls(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::R0));
    signature.result = StubResult::Flags;
    signature.trueCondition = MacroAssembler::NonZero;
    return signature;
}

void generateValueEndsItsTailCalls(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg word = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);

    static_assert(ClosureWord::functionShift == 32 && ClosureWord::endsItsTailCallsBit >= ClosureWord::functionShift);
    Jump isNotCell = jit.branchIfNotCell(value);
    jit.load32(Address(value, JSCell::structureIDOffset()), word);
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, word, scratch);
    jit.load32(Address(value, offsetOfClosureEntry + sizeof(uint32_t)), word);
    jit.returnFlagsTest32(word, TrustedImm32(1 << (ClosureWord::endsItsTailCallsBit - ClosureWord::functionShift)));

    isSlim.link(&jit);
    jit.load32(Address(value, offsetOfSlimClosureWord + sizeof(uint32_t)), word);
    jit.returnFlagsTest32(word, TrustedImm32(1 << (ClosureWord::endsItsTailCallsBit - ClosureWord::functionShift)));

    isNotCell.link(&jit);
    isNeither.link(&jit);
    jit.returnFlagsConstant(false);
}

StubSignature signatureOfValueIsClosureOf(StubKey)
{
    StubSignature signature;
    signature.addOperand(gprOf(StubRegister::R0));
    signature.addOperand(gprOf(StubRegister::T9));
    signature.result = StubResult::Flags;
    signature.trueCondition = MacroAssembler::Zero;
    return signature;
}

void generateValueIsClosureOf(StubAssembler& jit)
{
    const GPRReg value = jit.operand(0);
    const GPRReg expectedUpperHalf = jit.operand(1);
    const GPRReg word = jit.allocateScratch(StubRegister::T10);
    const GPRReg scratch = jit.allocateScratch(StubRegister::T11);

    static_assert(ClosureWord::functionShift == 32 && !JSCell::structureIDOffset());
    JumpList isNotClosure;
    isNotClosure.append(jit.branchIfNotCell(value));

    // Only a slim closure of this realm that is as it was created has one of these Structures.
    JumpList isSlimClosureAsCreated;
    jit.load32(Address(value, JSCell::structureIDOffset()), word);
    for (unsigned kind = 0; kind < numberOfSlimClosureKinds; ++kind) {
        Address header(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind * sizeof(uint64_t));
#if CPU(ARM64)
        jit.load32(header, scratch);
        isSlimClosureAsCreated.append(jit.branch32(CCallHelpers::Equal, word, scratch));
#else
        isSlimClosureAsCreated.append(jit.branch32(CCallHelpers::Equal, word, header));
#endif
    }

    loadClosureWordOfThisRealm(jit, value, { word, scratch, word, scratch }, isNotClosure);
    jit.urshift64(TrustedImm32(ClosureWord::functionShift), word);
    jit.returnFlags32(CCallHelpers::Equal, word, expectedUpperHalf);

    isSlimClosureAsCreated.link(&jit);
    jit.load32(Address(value, offsetOfSlimClosureWord + sizeof(uint32_t)), word);
    jit.returnFlags32(CCallHelpers::Equal, word, expectedUpperHalf);

    isNotClosure.link(&jit);
    jit.returnFlagsConstant(false);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

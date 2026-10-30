/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsClosures.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTClosureWord.h"
#include "AOTConvention.h"
#include "AOTInstance.h"
#include "AOTObjectWords.h"
#include "AOTSlimClosure.h"
#include "AOTStubHelpers.h"
#include "AOTStubProgramTables.h"
#include "AOTStubRegisters.h"
#include "AOTStubsAllocation.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsCalls.h"
#include "AOTStubsValues.h"
#include "CellState.h"
#include "JSFunction.h"
#include "RememberedSlot.h"
#include <optional>

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

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

static_assert(captureGPR(0) == operationArgumentGPR(2) && captureGPR(1) == operationArgumentGPR(3) && captureGPR(2) == operationArgumentGPR(4) && captureGPR(3) == operationArgumentGPR(5));

static ClosureRegisters allocateClosureRegisters(StubAssembler& jit)
{
    return { jit.result(), jit.operand(0), jit.allocateScratch(StubRegister::T12), jit.allocateScratch(StubRegister::T13) };
}

static StubSignature closureRequestSignature(unsigned numberOfCaptures)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(StubRegister::T9) });
    for (unsigned slot = 0; slot < numberOfCaptures; ++slot)
        signature.addOperand(captureGPR(slot));
    return signature;
}

static void storeCaptures(StubAssembler& jit, GPRReg cell, ptrdiff_t offsetOfCaptures)
{
    const unsigned numberOfCaptures = jit.key().count();
    auto addressOf = [&](unsigned slot) { return Address(cell, offsetOfCaptures + slot * sizeof(uint64_t)); };
    for (unsigned slot = 0; slot + 1 < numberOfCaptures; slot += 2)
        jit.storePair64(captureGPR(slot), captureGPR(slot + 1), addressOf(slot));
    if (numberOfCaptures % 2)
        jit.store64(captureGPR(numberOfCaptures - 1), addressOf(numberOfCaptures - 1));
}

static void loadClosureWord(StubAssembler& jit, GPRReg request, GPRReg word, GPRReg scratch)
{
    ASSERT(noOverlap(request, word, scratch));
    jit.and32(TrustedImm32(ClosureWord::functionMask), request, word);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), scratch);
    jit.load64(BaseIndex(scratch, word, CCallHelpers::TimesEight), word);
}

static void initializeFunctionObjectFields(StubAssembler& jit, const ClosureRegisters& registers)
{
    const auto [cell, request, index, scratch] = registers;
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

static void createClosureInOperation(StubAssembler& jit, GPRReg request, uint64_t restOfRequest, GPRReg wholeRequest)
{
    jit.or64(TrustedImm64(static_cast<int64_t>(restOfRequest)), request, wholeRequest);
    jit.callOperationAndReturn(Entry::operationAOTNewClosureSlow, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = wholeRequest });
}

StubSignature signatureOfNewClosure(StubKey key)
{
    return closureRequestSignature(key.count());
}

void generateNewClosure(StubAssembler& jit)
{
    const ClosureRegisters registers = allocateClosureRegisters(jit);
    const auto [cell, request, allocator, scratch] = registers;
    const GPRReg record = jit.allocateScratch(StubRegister::T11);

    JumpList slow;
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
    slow.link(&jit);
    createClosureInOperation(jit, request, static_cast<uint64_t>(jit.key().count()) << ClosureRequest::countShift, allocator);
}

StubSignature signatureOfNewSlimClosure(StubKey key)
{
    return closureRequestSignature(key.count());
}

void generateNewSlimClosure(StubAssembler& jit)
{
    const auto [cell, function, allocator, header] = allocateClosureRegisters(jit);
    const GPRReg word = allocator;
    const unsigned kind = jit.key().form();
    const unsigned count = jit.key().count();
    // Allocator i serves cells of 16 * (i + 1) bytes.
    const unsigned allocatorIndex = (count + 1) / 2;
    static_assert((ClosureRequest::maxSlimClosureCaptures + 1) / 2 < ClosureWords::numberOfSlimClosureAllocators);
    static_assert(offsetOfSlimClosureWord == sizeof(uint64_t));

    JumpList slow;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.slimClosureAllocators) + allocatorIndex * sizeof(void*)), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, header, slow, MarkedSpace::sizeStep * (allocatorIndex + 1));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.load64(BaseIndex(word, function, CCallHelpers::TimesEight), word);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind * sizeof(uint64_t)), header);
    jit.storePair64(header, word, Address(cell));
    storeCaptures(jit, cell, offsetOfSlimClosureCaptures);
    fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slow);
    slow.link(&jit);
    uint64_t restOfRequest = static_cast<uint64_t>(kind) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(count) << ClosureRequest::countShift;
    createClosureInOperation(jit, function, restOfRequest, allocator);
}

StubSignature signatureOfMakeClosureOfField(StubKey)
{
    return closureRequestSignature(1);
}

void generateMakeClosureOfField(StubAssembler& jit)
{
    const auto [cell, request, allocator, header] = allocateClosureRegisters(jit);
    const GPRReg object = jit.operand(1);
    const GPRReg word = allocator;
    const GPRReg offset = allocator;
    const unsigned kind = jit.key().form();
    const unsigned count = jit.key().count();
    const unsigned allocatorIndex = (count + 1) / 2;
    RELEASE_ASSERT(object == captureGPR(0) && noOverlap(cell, request, allocator, header, object));

    JumpList slow;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.slimClosureAllocators) + allocatorIndex * sizeof(void*)), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, header, slow, MarkedSpace::sizeStep * (allocatorIndex + 1));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.zeroExtend32ToWord(request, header);
    jit.load64(BaseIndex(word, header, CCallHelpers::TimesEight), word);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind * sizeof(uint64_t)), header);
    jit.storePair64(header, word, Address(cell));
    if (count)
        jit.store64(object, Address(cell, offsetOfSlimClosureCaptures));
    fenceAfterAllocation(jit);

    Label isMade = jit.label();
    jit.urshift64(request, TrustedImm32(32), offset);
    jit.urshift64(cell, TrustedImm32(cell32Shift), header);
    jit.store32(header, BaseIndex(object, offset, CCallHelpers::TimesOne));
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, object, header);
    jit.returnToCaller();

    // The operation preserves the request and the object.
    emitLateRefill(jit, refill, slow);
    slow.link(&jit);
    const GPRReg wholeRequest = allocator;
    uint64_t restOfRequest = static_cast<uint64_t>(kind) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(count) << ClosureRequest::countShift;
    jit.zeroExtend32ToWord(request, wholeRequest);
    jit.or64(TrustedImm64(static_cast<int64_t>(restOfRequest)), wholeRequest);
    jit.callOperationAndReturn(Entry::operationAOTNewClosureSlow, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = wholeRequest }, [&] {
        jit.jump().linkTo(isMade, &jit);
    });

    // The barrier stub takes the object in the register that holds the closure. The closure is already in the slot.
    needsBarrier.link(&jit);
    checkStubOperands(writeBarrier(), { cell });
    jit.clobberOperand(0);
    jit.emitFunctionPrologue();
    jit.move(object, cell);
    jit.callStub(writeBarrier());
    jit.load32(BaseIndex(object, offset, CCallHelpers::TimesOne), cell);
    decodeCell32(jit, cell, cell);
    jit.emitFunctionEpilogue();
    jit.ret();
}

// The caller loses the registers that a call of the operation which keeps registers would cost it.
StubSignature signatureOfClosureOfTable(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    signature.immediate = operationArgumentGPR(1);
    return signature;
}

// The number of the cell that the closure table has for the function. The two registers may hold anything.
static void loadNumberOfClosureCell(StubAssembler& jit, GPRReg function, GPRReg number, GPRReg scratch)
{
    static_assert(sizeof(Number24) == 3 && std::endian::native == std::endian::little);
    ASSERT(noOverlap(function, number, scratch));
    const GPRReg tables = scratch;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    jit.getEffectiveAddress(BaseIndex(function, function, CCallHelpers::TimesTwo), number);
    jit.add32(Address(tables, offsetOfProgramTableRange(ProgramTableIndex::closureCellNumbers) + OBJECT_OFFSETOF(TableRange, offset)), number);
    jit.addPtr(tables, number);
    const GPRReg lowBytes = scratch;
    jit.load16(Address(number), lowBytes);
    jit.load8(Address(number, 2), number);
    jit.lshift32(TrustedImm32(16), number);
    jit.or32(lowBytes, number);
}

// ClosureTable::closure(), for a slim closure: two words, the cell of the table, and the card that has the collector visit that region of the table.
static void emitCreateSlimClosureOfTable(StubAssembler& jit, const ClosureRegisters& registers, GPRReg number, JumpList& slow, JumpList& isCreated)
{
    const auto [cell, function, first, second] = registers;
    ASSERT(noOverlap(cell, function, first, second, number));
    static_assert(offsetOfSlimClosureWord == sizeof(uint64_t) && numberOfSlimClosureKinds == 2);
    static_assert(sizeof(RememberedSlot<uint32_t>) == 1u << CCallHelpers::TimesFour);
    constexpr unsigned cellsOfRegionShift = ClosureTable::regionShift - CCallHelpers::TimesFour;

    const GPRReg cards = cell;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closureTable.cards)), cards);
    slow.append(jit.branchTestPtr(CCallHelpers::Zero, cards));
    slow.append(jit.branchTest32(CCallHelpers::Zero, number));
    const GPRReg kind = first;
    loadKindOfClosureFunction(jit, function, kind);
    slow.append(jit.branchTest32(CCallHelpers::Zero, kind, TrustedImm32(slimFunctionKindBit())));
    jit.urshift32(TrustedImm32(cellsOfRegionShift), number);
    jit.store8(TrustedImm32(RememberedSlotSet::writtenCard), BaseIndex(cards, number, CCallHelpers::TimesOne));

    const GPRReg allocator = first;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.slimClosureAllocators)), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, second, slow, MarkedSpace::sizeStep, CountedUnder::Allocation);
    countAllocatedCellOfStub(jit, first, MarkedSpace::sizeStep);
    const GPRReg header = first;
    const GPRReg word = second;
    loadKindOfClosureFunction(jit, function, header);
    Jump isAsync = jit.branchTest32(CCallHelpers::NonZero, header, TrustedImm32(asyncFunctionKindBit()));
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders)), header);
    Jump hasHeader = jit.jump();
    isAsync.link(&jit);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders) + sizeof(uint64_t)), header);
    hasHeader.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.load64(BaseIndex(word, function, CCallHelpers::TimesEight), word);
    jit.storePair64(header, word, Address(cell));
    fenceAfterAllocation(jit);

    const GPRReg slot = first;
    loadNumberOfClosureCell(jit, function, second, first);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closureCells)), slot);
    jit.getEffectiveAddress(BaseIndex(slot, second, CCallHelpers::TimesFour), slot);
    encodeCell32(jit, cell, second);
    jit.sub32(TrustedImm32(1), second);
    jit.store32(second, Address(slot));
    isCreated.append(jit.jump());

    emitLateRefill(jit, refill, slow);
}

void generateClosureOfTable(StubAssembler& jit)
{
    const GPRReg cell = jit.result();
    const GPRReg function = jit.immediate();
    const GPRReg cells = cell;
#if CPU(X86_64)
    constexpr GPRReg number = CCallHelpers::s_scratchRegister;
#else
    const GPRReg number = jit.allocateScratch(StubRegister::T9);
#endif

    loadNumberOfClosureCell(jit, function, number, cells);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closureCells)), cells);
    JumpList slow;
    if (jit.options().useWideReferences) {
        jit.load64(BaseIndex(cells, number, CCallHelpers::TimesEight), cell);
        slow.append(jit.branchTest64(CCallHelpers::Zero, cell));
        jit.returnToCaller();
    } else {
        jit.load32(BaseIndex(cells, number, CCallHelpers::TimesFour), cell);
        Jump isNotCreated = jit.branchTest32(CCallHelpers::Zero, cell);
        jit.add32(TrustedImm32(1), cell);
        decodeCell32(jit, cell, cell);
        jit.returnToCaller();

        isNotCreated.link(&jit);
        jit.preserving({ gprOf(StubRegister::T12), gprOf(StubRegister::T13) }, [&] {
            JumpList cannot;
            JumpList isCreated;
            emitCreateSlimClosureOfTable(jit, { cell, function, jit.allocateScratch(StubRegister::T12), jit.allocateScratch(StubRegister::T13) }, number, cannot, isCreated);
            cannot.link(&jit);
            jit.move(TrustedImm32(0), cell);
            isCreated.link(&jit);
        });
        slow.append(jit.branchTest64(CCallHelpers::Zero, cell));
        jit.returnToCaller();
    }

    slow.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTSingletonClosure, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = function });
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
    const auto [cell, request, word, header] = registers;
    const TrustedImm32 slimBit(static_cast<int32_t>(1u << ClosureRequest::isSlimBit));
    static_assert(offsetOfSlimClosureWord == sizeof(uint64_t));
    // The slim bit is part of the index, and the displacement takes it out again.
    constexpr ptrdiff_t slimBitInIndex = sizeof(uint64_t) << (ClosureRequest::isSlimBit - ClosureRequest::executableShift);
    constexpr ptrdiff_t offsetOfHeaders = static_cast<ptrdiff_t>(AOT_INSTANCE_OFFSET(slimClosureHeaders)) - slimBitInIndex;

    // Above maxNumberOfCaptures every request is slim, and on x86-64 T11 is the fourth capture.
    std::optional<Jump> isFunctionObject;
    if (jit.key().count() <= ClosureRequest::maxNumberOfCaptures)
        isFunctionObject = jit.branchTest32(CCallHelpers::Zero, request, slimBit);
    else if (jit.options().validates) {
        Jump isSlim = jit.branchTest32(CCallHelpers::NonZero, request, slimBit);
        jit.breakpoint();
        isSlim.link(&jit);
    }
    loadClosureWord(jit, request, word, header);
    jit.urshift32(request, TrustedImm32(ClosureRequest::executableShift), header);
    jit.load64(BaseIndex(instanceGPR, header, CCallHelpers::TimesEight, offsetOfHeaders), header);
    jit.storePair64(header, word, Address(cell));
    storeCaptures(jit, cell, offsetOfSlimClosureCaptures);
    jit.returnToCaller();
    if (!isFunctionObject)
        return;

    isFunctionObject->link(&jit);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0])), header);
    jit.store64(header, Address(cell));
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
    jit.callOperationAndReturn(Entry::operationAOTMaterializeEnvironment, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = allocation, .thirdArgumentIn = source });

    isOnHeap.link(&jit);
    jit.returnToCaller();
}

void loadSlimClosureWord(StubAssembler& jit, GPRReg closure, GPRReg word, JumpList* isRenamed)
{
    ASSERT(noOverlap(closure, word, instanceGPR));
    jit.load64(Address(closure, offsetOfSlimClosureWord), word);
    Jump isSlimClosure = jit.branchIfType(closure, SlimClosureType);
    if (isRenamed)
        isRenamed->append(jit.branchTest8(CCallHelpers::NonZero, Address(closure, JSCell::typeInfoFlagsOffset()), TrustedImm32(TypeInfoPerCellBit)));
    jit.load32(Address(word, ReifiedSlimClosure::offsetOfFunctionIndexInButterfly()), word);
    jit.lshift64(TrustedImm32(3), word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.load64(Address(word), word);
    isSlimClosure.link(&jit);
}

StubSignature signatureOfValueEndsItsTailCalls(StubKey)
{
    return signatureWithFlagsResult({ gprOf(StubRegister::R0) }, MacroAssembler::NonZero);
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

StubSignature signatureOfRecordCallee(StubKey)
{
    return StubSignature::withoutResult({ calleeGPR });
}

void generateRecordCallee(StubAssembler& jit)
{
    const GPRReg closure = jit.operand(0);
    const GPRReg stackPointerOfCaller = jit.allocateScratch(StubRegister::T11);
    const GPRReg entries = jit.allocateScratch(StubRegister::T13);
    const GPRReg scratch = jit.scratchRegister();

    constexpr ptrdiff_t offsetOfMask = OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller) - static_cast<ptrdiff_t>(sizeof(RecordedCallee));
    jit.addPtr(TrustedImm32(sizeof(CallerFrameAndPC)), GPRInfo::callFrameRegister, stackPointerOfCaller);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(heap.recordedCallees)), entries);
    // The call that was made from further out may not have returned.
#if CPU(X86_64)
    jit.move(stackPointerOfCaller, scratch);
    jit.and64(Address(entries, offsetOfMask), scratch);
    Jump isTaken = jit.branchPtr(CCallHelpers::Above, BaseIndex(entries, scratch, CCallHelpers::TimesOne, OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller)), stackPointerOfCaller);
    jit.storePtr(stackPointerOfCaller, BaseIndex(entries, scratch, CCallHelpers::TimesOne, OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller)));
    jit.storePtr(closure, BaseIndex(entries, scratch, CCallHelpers::TimesOne, OBJECT_OFFSETOF(RecordedCallee, closure)));
#else
    jit.loadPtr(Address(entries, offsetOfMask), scratch);
    jit.andPtr(stackPointerOfCaller, scratch);
    jit.addPtr(scratch, entries);
    jit.loadPtr(Address(entries, OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller)), scratch);
    Jump isTaken = jit.branchPtr(CCallHelpers::Above, scratch, stackPointerOfCaller);
    jit.storePtr(stackPointerOfCaller, Address(entries, OBJECT_OFFSETOF(RecordedCallee, stackPointerOfCaller)));
    jit.storePtr(closure, Address(entries, OBJECT_OFFSETOF(RecordedCallee, closure)));
#endif
    jit.returnToCaller();

    isTaken.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTRecordCallee, { .returns = OperationCall::Returns::Void, .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = closure, .thirdArgumentIn = stackPointerOfCaller });
}

StubSignature signatureOfValueIsClosureOf(StubKey)
{
    return signatureWithFlagsResult({ gprOf(StubRegister::R0), gprOf(StubRegister::T9) }, MacroAssembler::Zero);
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

    loadClosureWordOfThisRealm(jit, value, { word, scratch, word, scratch }, isNotClosure, RenamedClosures::AreLikeOthers);
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

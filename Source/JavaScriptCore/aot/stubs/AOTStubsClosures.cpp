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
#include "AOTStubsValues.h"
#include "CellState.h"
#include "JSFunction.h"
#include "RememberedSlot.h"

namespace JSC { namespace AOT {

using Saves = OperationCall::Saves;

static_assert(ClosureRequest::maxSlimClosureCapturesInRegisters == 4);
static_assert(ClosureRequest::maxNumberOfCaptures <= ClosureRequest::maxSlimClosureCapturesInRegisters && ClosureRequest::maxSlimClosureCapturesInRegisters <= ClosureRequest::maxSlimClosureCaptures);
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

static constexpr unsigned numberOfCapturesInRegisters(StubKey key)
{
    return std::min(key.count(), ClosureRequest::maxSlimClosureCapturesInRegisters);
}

static ClosureRegisters allocateClosureRegisters(StubAssembler& jit)
{
    return { jit.result(), jit.operand(0), jit.allocateScratch(scratchBesideShortStubImmediate), jit.allocateScratch(StubRegister::T13) };
}

static StubSignature closureRequestSignature(unsigned numberOfCaptures)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(shortStubImmediateRegister) });
    for (unsigned slot = 0; slot < numberOfCaptures; ++slot)
        signature.addOperand(captureGPR(slot));
    return signature;
}

static void storeCaptures(StubAssembler& jit, GPRReg cell, ptrdiff_t offsetOfCaptures)
{
    const unsigned numberOfCaptures = numberOfCapturesInRegisters(jit.key());
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

static void initializeSlimClosure(StubAssembler& jit, const ClosureRegisters& registers)
{
    const auto [cell, function, word, header] = registers;
    static_assert(offsetOfSlimClosureWord == sizeof(uint64_t));

    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.load64(BaseIndex(word, function, CCallHelpers::TimesEight), word);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders) + jit.key().form() * sizeof(uint64_t)), header);
    jit.storePair64(header, word, Address(cell));
    storeCaptures(jit, cell, offsetOfSlimClosureCaptures);
}

static void createClosureInOperation(StubAssembler& jit, GPRReg request, uint64_t restOfRequest, GPRReg wholeRequest)
{
    jit.or64(TrustedImm64(static_cast<int64_t>(restOfRequest)), request, wholeRequest);
    jit.callOperationAndReturn(Entry::operationAOTNewClosureSlow, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = wholeRequest });
}

StubSignature signatureOfNewClosure(StubKey key)
{
    return closureRequestSignature(key.count()).withResultIn(key.operandRegister());
}

void generateNewClosure(StubAssembler& jit)
{
    const ClosureRegisters registers = allocateClosureRegisters(jit);
    const auto [cell, request, allocator, scratch] = registers;
    const GPRReg record = jit.allocateScratch(StubRegister::T11);

    JumpList slow;
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
    return closureRequestSignature(numberOfCapturesInRegisters(key)).withResultIn(key.operandRegister());
}

void generateNewSlimClosure(StubAssembler& jit)
{
    const ClosureRegisters registers = allocateClosureRegisters(jit);
    const auto [cell, function, allocator, header] = registers;
    const unsigned kind = jit.key().form();
    const unsigned count = jit.key().count();
    const unsigned allocatorIndex = (count + 1) / 2;
    static_assert((ClosureRequest::maxSlimClosureCaptures + 1) / 2 < ClosureWords::numberOfSlimClosureAllocators);
    static_assert(MarkedSpace::sizeStep * ClosureWords::numberOfSlimClosureAllocators <= MarkedSpace::preciseCutoff);

    JumpList slow;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.slimClosureAllocators) + allocatorIndex * sizeof(void*)), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, header, slow, MarkedSpace::sizeStep * (allocatorIndex + 1));
    initializeSlimClosure(jit, registers);
    if (count == numberOfCapturesInRegisters(jit.key()))
        fenceAfterAllocation(jit);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slow);
    slow.link(&jit);
    uint64_t restOfRequest = static_cast<uint64_t>(kind) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(count) << ClosureRequest::countShift;
    createClosureInOperation(jit, function, restOfRequest, allocator);
}

StubSignature signatureOfNewSlimClosureInFrame(StubKey key)
{
    StubSignature signature = StubSignature::withGPRResult({ gprOf(shortStubImmediateRegister) });
    signature.addOperand(signature.firstResult);
    for (unsigned slot = 0; slot < numberOfCapturesInRegisters(key); ++slot)
        signature.addOperand(captureGPR(slot));
    return signature;
}

void generateNewSlimClosureInFrame(StubAssembler& jit)
{
    const ClosureRegisters registers = allocateClosureRegisters(jit);
    const auto [cell, function, word, header] = registers;
    RELEASE_ASSERT(cell == jit.operand(1) && noOverlap(cell, function, word, header, captureGPR(0), captureGPR(1), captureGPR(2), captureGPR(3), instanceGPR));

    initializeSlimClosure(jit, registers);
    jit.store8(TrustedImm32(frameCellState), Address(cell, JSCell::cellStateOffset()));
    jit.store64(TrustedImm64(0), Address(cell, offsetOfCopyInHeapOfClosureInFrame(offsetOfSlimClosureCaptures, jit.key().count())));
    jit.returnToCaller();
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
    const bool capturesEnvironment = jit.key() == makeClosureOfFieldOverEnvironment(kind);
    const unsigned count = jit.key().count() ? 1 : 0;
    const unsigned allocatorIndex = (count + 1) / 2;
    RELEASE_ASSERT(object == captureGPR(0) && noOverlap(cell, request, allocator, header, object));
    auto loadEnvironment = [&](GPRReg environment) {
        jit.urshift64(request, TrustedImm32(48), environment);
        jit.load32(BaseIndex(object, environment, CCallHelpers::TimesOne), environment);
        decodeCell32(jit, environment, environment);
    };

    JumpList slow;
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.slimClosureAllocators) + allocatorIndex * sizeof(void*)), allocator);
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, header, slow, MarkedSpace::sizeStep * (allocatorIndex + 1));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.zeroExtend32ToWord(request, header);
    jit.load64(BaseIndex(word, header, CCallHelpers::TimesEight), word);
    jit.load64(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders) + kind * sizeof(uint64_t)), header);
    jit.storePair64(header, word, Address(cell));
    if (capturesEnvironment) {
        loadEnvironment(header);
        jit.store64(header, Address(cell, offsetOfSlimClosureCaptures));
    } else if (count)
        jit.store64(object, Address(cell, offsetOfSlimClosureCaptures));
    fenceAfterAllocation(jit);

    Label isMade = jit.label();
    jit.urshift64(request, TrustedImm32(32), offset);
    if (capturesEnvironment)
        jit.and32(TrustedImm32(0xffff), offset);
    encodeCell32(jit, cell, header);
    jit.store32(header, BaseIndex(object, offset, CCallHelpers::TimesOne));
    Jump needsBarrier = branchIfBarrierIsNeeded(jit, object, header);
    jit.returnToCaller();

    emitLateRefill(jit, refill, slow);
    slow.link(&jit);
    const GPRReg wholeRequest = allocator;
    uint64_t restOfRequest = static_cast<uint64_t>(kind) << ClosureRequest::executableShift | 1ULL << ClosureRequest::isSlimBit | static_cast<uint64_t>(count) << ClosureRequest::countShift;
    if (capturesEnvironment) {
        loadEnvironment(allocator);
        jit.move(object, header);
        jit.move(allocator, object);
    }
    jit.zeroExtend32ToWord(request, wholeRequest);
    jit.or64(TrustedImm64(static_cast<int64_t>(restOfRequest)), wholeRequest);
    jit.callOperationAndReturn(Entry::operationAOTNewClosureSlow, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = wholeRequest }, [&] {
        if (capturesEnvironment)
            jit.move(header, object);
        jit.jump().linkTo(isMade, &jit);
    });

    needsBarrier.link(&jit);
    checkStubOperands(writeBarrier(), { cell });
    jit.clobberOperand(0);
    jit.emitFunctionPrologue();
    jit.move(cell, header);
    jit.move(object, cell);
    jit.callStub(writeBarrier());
    jit.move(header, cell);
    jit.emitFunctionEpilogue();
    jit.ret();
}

StubSignature signatureOfClosureOfTable(StubKey)
{
    StubSignature signature = StubSignature::withGPRResult({ });
    signature.immediate = operationArgumentGPR(1);
#if CPU(X86_64)
    return signature.fillingKeptRegisterInFront(X86Registers::ecx);
#else
    return signature;
#endif
}

static void loadNumberOfClosureCell(StubAssembler& jit, GPRReg function, GPRReg number, GPRReg scratch)
{
    static_assert(NumberGroup::size == 32 && NumberGroup::numberOfWords * sizeof(uint32_t) == sizeof(uint64_t) && NumberGroup::isListedFlag == 1u << 31 && std::endian::native == std::endian::little);
    ASSERT(noOverlap(function, number, scratch));
    auto loadTable = [&](GPRReg table, GPRReg offset) {
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), table);
        jit.load32(Address(table, offsetOfProgramTableRange(ProgramTableIndex::closureCells) + OBJECT_OFFSETOF(TableRange, offset)), offset);
        jit.addPtr(offset, table);
    };
    const GPRReg group = number;
    loadTable(scratch, group);
    jit.urshift32(function, TrustedImm32(std::countr_zero(NumberGroup::size)), group);
    jit.load64(BaseIndex(scratch, group, CCallHelpers::TimesEight), group);
    const GPRReg bit = scratch;
    jit.move(TrustedImm32(1), bit);
    jit.lshift32(function, bit);
    Jump hasCell = jit.branchTest32(CCallHelpers::NonZero, group, bit);
    jit.move(TrustedImm32(0), number);
    Jump hasNoCell = jit.jump();

    hasCell.link(&jit);
    const GPRReg rank = scratch;
    jit.sub32(TrustedImm32(1), bit);
    jit.and32(group, rank);
    jit.countPopulation64(rank, rank, CCallHelpers::fpTempRegister);
    jit.urshift64(TrustedImm32(32), group);
    jit.add32(rank, number);
    Jump follows = jit.branchTest32(CCallHelpers::PositiveOrZero, number, number);

    jit.and32(TrustedImm32(~NumberGroup::isListedFlag), number);
    jit.lshift32(TrustedImm32(std::countr_zero(sizeof(uint32_t))), number);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), scratch);
    jit.add32(Address(scratch, offsetOfProgramTableRange(ProgramTableIndex::closureCells) + OBJECT_OFFSETOF(TableRange, offset)), number);
    jit.load32(BaseIndex(scratch, number, CCallHelpers::TimesOne), number);

    follows.link(&jit);
    hasNoCell.link(&jit);
}

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
    AssemblyHelpers::FreeListRefill refill = allocateCellWithLateRefill(jit, cell, allocator, second, slow, MarkedSpace::sizeStep);
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
        slow.link(&jit);
        jit.callOperationAndReturn(Entry::operationAOTSingletonClosure, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = function });
        return;
    }
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

    slow.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTSingletonClosure, { .saves = Saves::CallerSavedRegisters, .checksException = false, .secondArgumentIn = function });
}

void loadSlimClosureWord(StubAssembler& jit, GPRReg closure, GPRReg word)
{
    ASSERT(noOverlap(closure, word, instanceGPR));
    jit.load64(Address(closure, offsetOfSlimClosureWord), word);
    Jump isSlimClosure = jit.branchIfType(closure, SlimClosureType);
    jit.load32(Address(word, ReifiedSlimClosure::offsetOfFunctionIndexInButterfly()), word);
    jit.lshift64(TrustedImm32(3), word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.load64(Address(word), word);
    isSlimClosure.link(&jit);
}

static_assert(noClosureNumber > ClosureWord::functionMask && ClosureWord::functionShift == 32);

static void compareRealmOnFlags(StubAssembler& jit, GPRReg cell, GPRReg scratch, [[maybe_unused]] GPRReg secondScratch)
{
    jit.load32(Address(cell, JSCell::structureIDOffset()), scratch);
#if CPU(X86_64)
    decodeStructureID(jit, scratch);
    jit.loadPtr(Address(scratch, Structure::realmOffset()), scratch);
    jit.compareOnFlags64(scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)));
#else
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_PREFIX_OFFSET(structureIDBase)), secondScratch);
    jit.add64(secondScratch, scratch);
    jit.loadPtr(Address(scratch, Structure::realmOffset()), scratch);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(globalObject)), secondScratch);
    jit.compareOnFlags64(scratch, secondScratch);
#endif
}

static void loadNumberOfFullClosure(StubAssembler& jit, GPRReg cell, GPRReg number)
{
    jit.load32(Address(cell, offsetOfClosureEntry + sizeof(uint32_t)), number);
    jit.and32(TrustedImm32(ClosureWord::functionMask), number);
}

static void loadNumberOfSlimClosureWithProperties(StubAssembler& jit, GPRReg cell, GPRReg number)
{
    jit.loadPtr(Address(cell, JSObject::butterflyOffset()), number);
    jit.load32(Address(number, ReifiedSlimClosure::offsetOfFunctionIndexInButterfly()), number);
}

static void compareCellTypeOnFlags(StubAssembler& jit, GPRReg cell, JSType type, [[maybe_unused]] GPRReg scratch)
{
#if CPU(X86_64)
    jit.m_assembler.cmpb_im(type, JSCell::typeInfoTypeOffset(), cell);
#else
    jit.load8(Address(cell, JSCell::typeInfoTypeOffset()), scratch);
    jit.compareOnFlags32(scratch, TrustedImm32(type));
#endif
}

StubSignature signatureOfChangedClosureHasNumber(StubKey key)
{
    StubSignature signature = signatureWithFlagsResult({ key.operandRegister().value_or(gprOf(firstStubOperandRegister)) }, MacroAssembler::Zero);
    signature.immediate = scratchImmediateGPR;
    return signature;
}

void generateChangedClosureHasNumber(StubAssembler& jit)
{
    const GPRReg cell = jit.operand(0);
    const GPRReg asked = jit.immediate();
    const bool isSlim = jit.key().form() == static_cast<uint8_t>(ChangedClosure::Slim);
    const bool asksForRealm = !isSlim || !jit.options().hasOneInstance;
    constexpr bool numberWaitsOnStack = isX86_64();
#if CPU(X86_64)
    const GPRReg found = asked;
#else
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg found = ownedScratch;
#endif

    compareCellTypeOnFlags(jit, cell, isSlim ? SlimClosureWithPropertiesType : CompiledClosureType, found);
    Jump hasOtherType = jit.branchOnFlags(CCallHelpers::NotEqual);
    if (numberWaitsOnStack)
        jit.pushToSave(asked);

    Jump isOfOtherRealm;
    if (asksForRealm) {
        if (!numberWaitsOnStack)
            jit.pushToSave(asked);
        compareRealmOnFlags(jit, cell, found, asked);
        if (!numberWaitsOnStack)
            jit.popToRestore(asked);
        isOfOtherRealm = jit.branchOnFlags(CCallHelpers::NotEqual);
    }

    if (isSlim)
        loadNumberOfSlimClosureWithProperties(jit, cell, found);
    else
        loadNumberOfFullClosure(jit, cell, found);
#if CPU(X86_64)
    jit.compareOnFlags32(found, Address(sp));
#else
    jit.compareOnFlags32(found, asked);
#endif

    if (asksForRealm)
        isOfOtherRealm.link(&jit);
    if (numberWaitsOnStack)
        jit.popToRestore(asked);
    hasOtherType.link(&jit);
    jit.returnToCaller();
}

StubSignature signatureOfCheckProofOfClosure(StubKey key)
{
    return StubSignature::withoutResult({ key.operandRegister().value_or(gprOf(firstStubOperandRegister)), throwWordGPR });
}

void generateCheckProofOfClosure(StubAssembler& jit)
{
    static_assert(KindsOfProof::shiftOfNumber == 32 && ClosureWord::functionShift == 32);
    const GPRReg value = jit.operand(0);
    OwnedScratchRegister words(jit, jit.operand(1));
    const GPRReg scratch = isX86_64() ? InvalidGPRReg : scratchImmediateGPR;
    auto swapHalves = [&] {
        jit.rotateRight64(TrustedImm32(32), words);
    };

    JumpList isNoCell;
    isNoCell.append(jit.branchIfNotCell(value));
    isNoCell.append(jit.branchTest64(CCallHelpers::Zero, value));
    swapHalves();
    Jump hasOtherWord = jit.branch32(CCallHelpers::NotEqual, words, Address(value, offsetOfSlimClosureWord + sizeof(uint32_t)));
    compareCellTypeOnFlags(jit, value, ObjectType, scratch);
    Jump isNoObject = jit.branchOnFlags(CCallHelpers::Below);
    jit.returnToCaller();

    hasOtherWord.link(&jit);
    compareCellTypeOnFlags(jit, value, SlimClosureWithPropertiesType, scratch);
    Jump isNotChanged = jit.branchOnFlags(CCallHelpers::NotEqual);
#if CPU(X86_64)
    jit.pushToSave(words);
    loadNumberOfSlimClosureWithProperties(jit, value, words);
    jit.xor32(Address(sp), words);
    jit.and32(TrustedImm32(ClosureWord::functionMask), words);
    jit.compareOnFlags32(words, TrustedImm32(0));
    jit.popToRestore(words);
    Jump hasOtherNumber = jit.branchOnFlags(CCallHelpers::NotEqual);
#else
    loadNumberOfSlimClosureWithProperties(jit, value, scratch);
    jit.xor32(words, scratch);
    Jump hasOtherNumber = jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(ClosureWord::functionMask));
#endif
    jit.returnToCaller();

    isNotChanged.link(&jit);
    compareCellTypeOnFlags(jit, value, CompiledClosureType, scratch);
    Jump isNoClosure = jit.branchOnFlags(CCallHelpers::NotEqual);
    jit.pushToSave(words);
#if CPU(X86_64)
    compareRealmOnFlags(jit, value, words, InvalidGPRReg);
    Jump isOfOtherRealm = jit.branchOnFlags(CCallHelpers::NotEqual);
    loadNumberOfFullClosure(jit, value, words);
    jit.xor32(Address(sp), words);
    jit.and32(TrustedImm32(ClosureWord::functionMask), words);
    jit.compareOnFlags32(words, TrustedImm32(0));
    isOfOtherRealm.link(&jit);
    jit.popToRestore(words);
    Jump isOtherFullClosure = jit.branchOnFlags(CCallHelpers::NotEqual);
#else
    compareRealmOnFlags(jit, value, scratch, words);
    jit.popToRestore(words);
    JumpList isOtherFullClosure;
    isOtherFullClosure.append(jit.branchOnFlags(CCallHelpers::NotEqual));
    loadNumberOfFullClosure(jit, value, scratch);
    jit.xor32(words, scratch);
    isOtherFullClosure.append(jit.branchTest32(CCallHelpers::NonZero, scratch, TrustedImm32(ClosureWord::functionMask)));
#endif
    jit.returnToCaller();

    isNoObject.link(&jit);
    isNoClosure.link(&jit);
    isOtherFullClosure.link(&jit);
    hasOtherNumber.link(&jit);
    swapHalves();
    isNoCell.link(&jit);
    jit.jumpToStub(jit.key().operandRegister() ? checkProofOfKinds().withOperandRegister(value) : checkProofOfKinds());
}

StubSignature signatureOfNumberOfChangedClosure(StubKey key)
{
    return StubSignature::withGPRResult({ key.operandRegister().value_or(gprOf(firstStubOperandRegister)) });
}

void generateNumberOfChangedClosure(StubAssembler& jit)
{
    const GPRReg cell = jit.operand(0);
    const GPRReg number = jit.result();
    const OwnedScratchRegister ownedScratch(jit);
    const GPRReg scratch = ownedScratch;

    compareCellTypeOnFlags(jit, cell, SlimClosureWithPropertiesType, scratch);
    Jump isSlim = jit.branchOnFlags(CCallHelpers::Equal);
    compareCellTypeOnFlags(jit, cell, CompiledClosureType, scratch);
    Jump isFull = jit.branchOnFlags(CCallHelpers::Equal);
    Label isNoClosureOfInstance = jit.label();
    jit.move(TrustedImm32(static_cast<int32_t>(noClosureNumber)), number);
    jit.returnToCaller();

    auto leaveUnlessOfThisRealm = [&] {
        compareRealmOnFlags(jit, cell, scratch, scratchImmediateGPR);
        jit.branchOnFlags(CCallHelpers::NotEqual).linkTo(isNoClosureOfInstance, &jit);
    };

    isFull.link(&jit);
    leaveUnlessOfThisRealm();
    loadNumberOfFullClosure(jit, cell, number);
    jit.returnToCaller();

    isSlim.link(&jit);
    if (!jit.options().hasOneInstance)
        leaveUnlessOfThisRealm();
    loadNumberOfSlimClosureWithProperties(jit, cell, number);
    jit.returnToCaller();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

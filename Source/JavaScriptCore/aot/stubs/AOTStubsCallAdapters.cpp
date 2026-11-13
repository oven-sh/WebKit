/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCallAdapters.h"

#if ENABLE(AOT)

#include "AOTAsyncAwaitConditions.h"
#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTFunctionTables.h"
#include "AOTImageFormat.h"
#include "AOTInstance.h"
#include "AOTProgramTables.h"
#include "AOTStubAssembler.h"
#include "AOTStubFrames.h"
#include "AOTStubHelpers.h"
#include "AOTStubProgramTables.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsClosures.h"
#include "AOTStubsExceptions.h"
#include "CallFrame.h"
#include "JSGlobalObject.h"
#include "MicrotaskQueue.h"
#include "StackAlignment.h"
#include "StackManager.h"
#include "VM.h"
#include <limits>

namespace JSC { namespace AOT {

static constexpr ptrdiff_t offsetOfFirstArgument = static_cast<int>(CallFrameSlot::firstArgument) * static_cast<int>(sizeof(Register));
static_assert(sizeof(Register) == sizeOfStackWord);

static constexpr unsigned constructSlotsSize = 16;
static constexpr ptrdiff_t offsetOfConstructThis = -static_cast<ptrdiff_t>(AdapterFrame::saveAreaSize) - 8;
static constexpr ptrdiff_t offsetOfConstructEntryWord = -static_cast<ptrdiff_t>(AdapterFrame::saveAreaSize) - 16;
static_assert(!(constructSlotsSize % stackAlignmentBytes()));

#if CPU(X86_64)
static constexpr StubRegister constructArgumentBaseRegister = StubRegister::T13;
#else
static constexpr StubRegister constructArgumentBaseRegister = StubRegister::T12;
#endif

struct EntryCall {
    EntryCall(Address firstArgument, StubFrameKind kind)
        : arguments(firstArgument)
        , isInAdapterFrame(kind == StubFrameKind::Adapter || kind == StubFrameKind::ConstructAdapter)
        , bytesBelowFramePointer(AdapterFrame::saveAreaSize + (kind == StubFrameKind::ConstructAdapter ? constructSlotsSize : 0))
    {
    }

    Address arguments;
    bool isInAdapterFrame;
    unsigned bytesBelowFramePointer;
    JumpList passesFewer;
    JumpList passesMore;
    Label ready;
    Label readyWithTestedStack;
    Label afterCall;
    Jump stackMayNotFit;
    JumpList stackDoesNotFit;
};

#if CPU(ARM64)
static constexpr ptrdiff_t savedOffsetOf(GPRReg reg)
{
    for (const AdapterFrame::SavedRegister& saved : AdapterFrame::savedRegisters) {
        if (saved.reg == reg)
            return saved.offset;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr ptrdiff_t offsetOfNumberTagRegisterInAdapter = savedOffsetOf(GPRInfo::numberTagRegister);
static constexpr ptrdiff_t offsetOfNotCellMaskRegisterInAdapter = savedOffsetOf(GPRInfo::notCellMaskRegister);
static constexpr ptrdiff_t offsetOfInstanceRegisterInAdapter = savedOffsetOf(instanceGPR);
static constexpr ptrdiff_t offsetOfCellCageBaseRegisterInAdapter = savedOffsetOf(cellCageBaseGPR);
static_assert(AdapterFrame::savedRegisters.size() == 4);
static_assert(offsetOfCellCageBaseRegisterInAdapter == AdapterFrame::offsetOfInstance + 8 && offsetOfNotCellMaskRegisterInAdapter == offsetOfNumberTagRegisterInAdapter + 8);
#endif

static TrustedImm64 bitMask(unsigned bit)
{
    return TrustedImm64(static_cast<int64_t>(1ULL << bit));
}

static void emitSaveRegistersInAdapter(StubAssembler& jit, GPRReg instance)
{
#if CPU(ARM64)
    jit.storePair64(instance, cellCageBaseGPR, fp, TrustedImm32(AdapterFrame::offsetOfInstance));
    jit.storePtr(instanceGPR, Address(fp, offsetOfInstanceRegisterInAdapter));
    jit.storePair64(GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister, fp, TrustedImm32(offsetOfNumberTagRegisterInAdapter));
#else
    for (const AdapterFrame::SavedRegister& saved : AdapterFrame::savedRegisters)
        jit.storePtr(saved.reg, Address(fp, static_cast<int32_t>(saved.offset)));
    jit.storePtr(instance, Address(fp, AdapterFrame::offsetOfInstance));
#endif
}

void emitEnterAdapter(StubAssembler& jit, unsigned extraBytes)
{
    const GPRReg instance = jit.reuseScratch(StubRegister::EntryT12);
    const GPRReg scratch = jit.reuseScratch(StubRegister::T9);
    constexpr GPRReg callee = calleeGPR;
    RELEASE_ASSERT(noOverlap(instance, scratch, thisGPR, calleeGPR) && noOverlap(instance, countGPR));
    jit.loadPtr(incomingFrameSlot(CallFrameSlot::callee), callee);
    jit.move(callee, scratch);
    loadCellVM(jit, scratch);
    jit.loadPtr(Address(scratch, VM::offsetOfAOTState() + VMState::offsetOfOnlyInstance()), instance);
    Jump hasSeveralInstances = jit.branchTestPtr(CCallHelpers::Zero, instance);
    Label hasInstance = jit.label();
    jit.addOutOfLinePart([=](StubAssembler& jit) {
        hasSeveralInstances.link(&jit);
        jit.load64(Address(scratch, VM::offsetOfAOTState() + VMState::offsetOfEntriesWithSeveralInstances()), instance);
        jit.add64(TrustedImm32(1), instance);
        jit.store64(instance, Address(scratch, VM::offsetOfAOTState() + VMState::offsetOfEntriesWithSeveralInstances()));
        jit.load32(Address(callee, JSCell::structureIDOffset()), instance);
        jit.add64(Address(scratch, VM::offsetOfAOTState() + VMState::offsetOfStructureIDBase()), instance);
        jit.loadPtr(Address(instance, Structure::realmOffset()), instance);
        jit.loadPtr(Address(instance, JSGlobalObject::offsetOfAOTInstance()), instance);
        jit.store8(TrustedImm32(1), Address(instance, AOT_INSTANCE_OFFSET(propertyCaches.hasRunSinceClear)));
        jit.jump().linkTo(hasInstance, &jit);
    });

    jit.emitFunctionPrologue();
    static_assert(!(AdapterFrame::saveAreaSize % stackAlignmentBytes()));
    jit.subPtr(TrustedImm32(AdapterFrame::saveAreaSize + extraBytes), sp);
    emitSaveRegistersInAdapter(jit, instance);
    jit.move(instance, instanceGPR);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(cellCageBase)), cellCageBaseGPR);
#if !CPU(X86_64)
    jit.emitMaterializeTagCheckRegisters();
#endif
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));
}

void emitRestoreAdapterCallerRegisters(StubAssembler& jit, GPRReg adapterFrame)
{
#if CPU(ARM64)
    jit.loadPtr(Address(adapterFrame, offsetOfInstanceRegisterInAdapter), instanceGPR);
    jit.loadPtr(Address(adapterFrame, offsetOfCellCageBaseRegisterInAdapter), cellCageBaseGPR);
    jit.loadPair64(adapterFrame, TrustedImm32(offsetOfNumberTagRegisterInAdapter), GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister);
#else
    for (const AdapterFrame::SavedRegister& saved : AdapterFrame::savedRegisters)
        jit.loadPtr(Address(adapterFrame, static_cast<int32_t>(saved.offset)), saved.reg);
#endif
}

void emitAdapterEpilogue(StubAssembler& jit)
{
    emitRestoreAdapterCallerRegisters(jit, fp);
    jit.emitFunctionEpilogue();
    jit.returnToCaller();
}

static void emitLoadThisAndCount(StubAssembler& jit)
{
#if CPU(ARM64)
    static_assert(static_cast<int>(CallFrameSlot::thisArgument) == static_cast<int>(CallFrameSlot::argumentCountIncludingThis) + 1);
    jit.loadPair64(fp, TrustedImm32(static_cast<int>(CallFrameSlot::argumentCountIncludingThis) * static_cast<int>(sizeof(Register))), countGPR, thisGPR);
#else
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::thisArgument), thisGPR);
    jit.load32(CCallHelpers::lowWordFor(CallFrameSlot::argumentCountIncludingThis), countGPR);
#endif
    jit.sub32(TrustedImm32(1), countGPR);
    Jump isNotCell = jit.branchIfNotCell(thisGPR);
    jit.load8(Address(thisGPR, JSCell::typeInfoTypeOffset()), argumentGPR(0));
    jit.sub32(TrustedImm32(FirstScopeType), argumentGPR(0));
    Jump isNotScope = jit.branch32(CCallHelpers::Above, argumentGPR(0), TrustedImm32(LastScopeType - FirstScopeType));
    jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
    isNotScope.link(&jit);
    isNotCell.link(&jit);
}

static void emitLoadArgumentRegisters(StubAssembler& jit, Address arguments)
{
#if CPU(ARM64)
    static_assert(!(numberOfArgumentGPRs % 2));
    for (unsigned i = 0; i < numberOfArgumentGPRs; i += 2)
        jit.loadPair64(arguments.base, TrustedImm32(static_cast<int32_t>(arguments.offset + i * sizeof(Register))), argumentGPR(i), argumentGPR(i + 1));
#else
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        jit.load64(arguments.withOffset(i * sizeof(Register)), argumentGPR(i));
#endif
}

[[nodiscard]] static EntryCall emitLoadArgumentsAndCallEntry(StubAssembler& jit, GPRReg word, Address arguments, StubFrameKind kind, std::optional<StubKey> entryWithLoadedArguments = std::nullopt)
{
    EntryCall call(arguments, kind);
    call.passesMore.append(jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(numberOfArgumentGPRs)));
    constexpr GPRReg numberOfParameters = argumentGPR(numberOfArgumentGPRs - 1);
    ASSERT(noOverlap(word, numberOfParameters, arguments.base));
    extractBits(jit, word, ClosureWord::numberOfParametersShift, ClosureWord::numberOfParametersBits, numberOfParameters);
    jit.xor32(TrustedImm32(ClosureWord::maxNumberOfParameters), numberOfParameters);
    call.passesFewer.append(jit.branch32(CCallHelpers::Below, countGPR, numberOfParameters));
    emitLoadArgumentRegisters(jit, arguments);

    if (entryWithLoadedArguments)
        jit.bindEntry(*entryWithLoadedArguments);
    call.ready = jit.label();
    call.stackMayNotFit = jit.branchPtr(CCallHelpers::Below, sp, trapAwareSoftStackLimit());
    call.readyWithTestedStack = jit.label();
    toCodeAddress(jit, word, jit.reuseScratch(StubRegister::T13), StackIsTested::Yes);
    jit.call(word, JSEntryPtrTag);
    call.afterCall = jit.bindReturnLabel(kind);
    return call;
}

static void emitLocateOutOfLineArguments(StubAssembler& jit, GPRReg source, GPRReg count, GPRReg scratch)
{
    constexpr ptrdiff_t offsetOfTaggedCount = offsetOfFirstArgument + static_cast<ptrdiff_t>(numberOfArgumentGPRs * sizeof(Register));
    static_assert(outOfLineArgumentCountTagShift == 48);
    jit.load16(Address(fp, offsetOfTaggedCount + 6), count);
    Jump areInFrame = jit.branch32(CCallHelpers::NotEqual, count, TrustedImm32(outOfLineArgumentCountTag));
    jit.load32(Address(fp, offsetOfTaggedCount), count);
    jit.add32(count, countGPR);
    jit.sub32(TrustedImm32(adapterArgumentCountWithOutOfLineArguments), countGPR);
    jit.loadPtr(Address(fp, offsetOfTaggedCount + static_cast<ptrdiff_t>(sizeof(Register))), scratch);
    jit.subPtr(fp, source);
    jit.addPtr(scratch, source);
    jit.subPtr(TrustedImm32(offsetOfFirstArgument), source);
    areInFrame.link(&jit);
}

struct StackWordCopy {
    GPRReg index;
    GPRReg value;
    GPRReg destination;
    GPRReg source;
};

static void emitCopyStackWords(StubAssembler& jit, const StackWordCopy& copy, JumpList& stackDoesNotFit)
{
    constexpr int32_t numberOfRegisters = numberOfArgumentGPRs;
    const GPRReg size = copy.value;
    emitAlignedSizeOfWords(jit, countGPR, -numberOfRegisters, size);
    jit.move(sp, copy.destination);
    jit.subPtr(size, copy.destination);
    stackDoesNotFit.append(jit.branchPtr(CCallHelpers::Below, copy.destination, softStackLimit()));
    jit.move(copy.destination, sp);

    jit.add32(TrustedImm32(-numberOfRegisters), countGPR, copy.index);
    emitCopyFromLastWord(jit, Address(copy.source), Address(copy.destination), copy.index, copy.value);
}

static void emitRelocateFrameAboveStackWords(StubAssembler& jit, const EntryCall& call, const StackWordCopy& copy)
{
    constexpr int32_t numberOfRegisters = numberOfArgumentGPRs;
    constexpr int32_t bytesAboveFramePointer = offsetOfFirstArgument + numberOfRegisters * static_cast<int32_t>(sizeof(Register));
    const int32_t bytesBelowFramePointer = call.bytesBelowFramePointer;
    const int32_t frameSize = bytesBelowFramePointer + bytesAboveFramePointer;
    const TrustedImm32 frameSizeInWords(frameSize / static_cast<int32_t>(sizeof(Register)));
    const GPRReg argumentBase = call.arguments.base;
    const GPRReg numberOfStackWords = countGPR;
    RELEASE_ASSERT(!(frameSize % stackAlignmentBytes()));

    jit.sub32(TrustedImm32(numberOfRegisters), numberOfStackWords);
    jit.getEffectiveAddress(BaseIndex(copy.source, numberOfStackWords, CCallHelpers::TimesEight, sizeof(Register)), copy.destination);
    jit.andPtr(TrustedImm32(~static_cast<int32_t>(stackAlignmentBytes() - 1)), copy.destination);
    emitAlignedSizeOfWords(jit, numberOfStackWords, 0, copy.value);
    jit.subPtr(copy.value, copy.destination);
    jit.subPtr(TrustedImm32(frameSize), copy.destination);
    jit.addPtr(TrustedImm32(-frameSize), copy.destination, sp);

    jit.move(frameSizeInWords, copy.index);
    emitCopyFromLastWord(jit, Address(fp, -bytesBelowFramePointer), Address(sp), copy.index, copy.value);
    if (argumentBase != fp)
        jit.subPtr(fp, argumentBase);
    jit.loadPtr(Address(fp, CallFrame::callerFrameOffset()), fp);

    emitCopyFromFirstWord(jit, copy.source, copy.destination, numberOfStackWords, copy.index, copy.value);
    jit.store64(TrustedImm32(0), BaseIndex(copy.destination, copy.index, CCallHelpers::TimesEight));

    const GPRReg frame = copy.source;
    emitAlignedSizeOfWords(jit, numberOfStackWords, 0, frame);
    jit.addPtr(copy.destination, frame);
    jit.move(frameSizeInWords, copy.index);
    emitCopyFromLastWord(jit, Address(sp), Address(frame), copy.index, copy.value);
    jit.addPtr(TrustedImm32(bytesBelowFramePointer), frame, fp);
    jit.move(copy.destination, sp);
    if (argumentBase != fp)
        jit.addPtr(fp, argumentBase);
    jit.add32(TrustedImm32(numberOfRegisters), countGPR);
    jit.store64(TrustedImm32(numberOfRegisters + 1), CCallHelpers::addressFor(CallFrameSlot::argumentCountIncludingThis));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), copy.value);
    jit.storePtr(fp, Address(copy.value, VM::offsetOfTopCallFrame()));
}

static void emitRareArgumentPaths(StubAssembler& jit, EntryCall& call)
{
    call.passesFewer.link(&jit);
    emitLoadArgumentRegisters(jit, call.arguments);
    emitFillMissingArgumentRegisters(jit, countGPR, jit.reuseScratch(StubRegister::T13));
    jit.jump().linkTo(call.ready, &jit);

    call.passesMore.link(&jit);
    static_assert(numberOfArgumentGPRs >= 4 && gprOf(StubRegister::A3) == argumentGPR(3));
    static_assert(adapterArgumentCountWithOutOfLineArguments < maxListLengthCopiedTwice);
    const StackWordCopy copy { jit.reuseScratch(StubRegister::A0), jit.reuseScratch(StubRegister::A1), jit.reuseScratch(StubRegister::A2), jit.reuseScratch(StubRegister::A3) };
    const TrustedImm32 offsetOfStackWords(static_cast<int32_t>(call.arguments.offset + numberOfArgumentGPRs * sizeof(Register)));
    ASSERT(noOverlap(call.arguments.base, copy.index, copy.value, copy.destination, copy.source));
    Jump isLong;
    if (call.isInAdapterFrame)
        isLong = jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(maxListLengthCopiedTwice));
    jit.addPtr(offsetOfStackWords, call.arguments.base, copy.source);
    if (call.isInAdapterFrame)
        emitLocateOutOfLineArguments(jit, copy.source, copy.value, copy.index);
    emitCopyStackWords(jit, copy, call.stackDoesNotFit);
    emitLoadArgumentRegisters(jit, call.arguments);
    jit.jump().linkTo(call.ready, &jit);
    if (!call.isInAdapterFrame)
        return;

    isLong.link(&jit);
    jit.addPtr(offsetOfStackWords, call.arguments.base, copy.source);
    emitRelocateFrameAboveStackWords(jit, call, copy);
    emitLoadArgumentRegisters(jit, call.arguments);
    jit.jump().linkTo(call.ready, &jit);
}

static void emitPollPath(StubAssembler& jit, EntryCall& call)
{
    call.stackMayNotFit.link(&jit);
    call.stackDoesNotFit.append(jit.branchPtr(CCallHelpers::Below, sp, softStackLimit()));
    jit.pollBeforeCall(call.afterCall);
    jit.jump().linkTo(call.readyWithTestedStack, &jit);
}

static void emitThrowFromAdapter(StubAssembler& jit, Label afterCall, StubKey target)
{
#if CPU(ARM64)
    jit.loadLabelAddress(afterCall, CCallHelpers::linkRegister);
#else
    jit.loadLabelAddress(afterCall, CCallHelpers::s_scratchRegister);
    jit.push(CCallHelpers::s_scratchRegister);
#endif
    jit.jumpToStub(target);
}

static void emitStackOverflowPath(StubAssembler& jit, const EntryCall& entryCall)
{
    entryCall.stackDoesNotFit.link(&jit);
    emitThrowFromAdapter(jit, entryCall.afterCall, throwStackOverflow());
}

StubSignature signatureOfEnterClosure(StubKey) { return { }; }
StubSignature signatureOfEnterSlimClosure(StubKey) { return { }; }
StubSignature signatureOfEnterModuleBody(StubKey) { return { }; }
StubSignature signatureOfEnterClosureWithLoadedArguments(StubKey) { return { }; }
StubSignature signatureOfEnterClosureForConstruct(StubKey) { return { }; }

void generateEnterClosure(StubAssembler& jit)
{
    const GPRReg word = jit.reuseScratch(StubRegister::T11);
    JumpList hasWord;
    emitEnterAdapter(jit);
    jit.load64(Address(calleeGPR, offsetOfClosureEntry), word);
    hasWord.append(jit.jump());

    jit.bindEntry(enterModuleBody());
    emitEnterAdapter(jit);
    jit.load32(Address(fp, offsetOfFirstArgument), word);
    jit.lshift64(TrustedImm32(3), word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.load64(Address(word), word);
    hasWord.append(jit.jump());

    jit.bindEntry(enterSlimClosure());
    emitEnterAdapter(jit);
    loadSlimClosureWord(jit, calleeGPR, word);

    hasWord.link(&jit);
    emitLoadThisAndCount(jit);
    EntryCall entryCall = emitLoadArgumentsAndCallEntry(jit, word, Address(fp, offsetOfFirstArgument), StubFrameKind::Adapter, enterClosureWithLoadedArguments());
    emitAdapterEpilogue(jit);

    emitRareArgumentPaths(jit, entryCall);
    emitPollPath(jit, entryCall);
    emitStackOverflowPath(jit, entryCall);
}

StubSignature signatureOfStartModuleThatRunTimeNames(StubKey) { return { }; }

void generateStartModuleThatRunTimeNames(StubAssembler& jit)
{
    const GPRReg start = jit.reuseScratch(StubRegister::A0);
    const Address named(instanceGPR, AOT_INSTANCE_OFFSET(modules.moduleThatRunTimeStartsPlusOne));
    jit.load32(named, start);
    jit.store32(TrustedImm32(0), named);
    Jump isNamed = jit.branchTest32(CCallHelpers::NonZero, start);
    jit.breakpoint();
    isNamed.link(&jit);
    jit.sub32(TrustedImm32(1), start);
    jit.jumpToStub(startModule());
}

StubSignature signatureOfStartModule(StubKey) { return StubSignature::withGPRResult({ gprOf(StubRegister::A0) }); }

void generateStartModule(StubAssembler& jit)
{
    using Operation = ModuleStartStepData::Operation;
    static_assert(StoredModuleStatus::Unlinked < StoredModuleStatus::InRecord && StoredModuleStatus::Linked < StoredModuleStatus::InRecord);
    static_assert(StoredModuleStatus::InRecord < StoredModuleStatus::Evaluated && StoredModuleStatus::Evaluated < StoredModuleStatus::Evaluating);
    static_assert(sizeof(StoredModuleStatus) == 1 && sizeof(ModuleStartStepData) == 4 && !(ModuleStartFrame::size % stackAlignmentBytes()));
    static_assert(areDistinctOnEveryISA(StubRegister::A0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::T10, StubRegister::T11));
    const GPRReg start = jit.clobberOperand(0);
    const GPRReg operand = jit.allocateScratch(StubRegister::A1);
    const GPRReg operation = jit.allocateScratch(StubRegister::A2);
    const GPRReg table = jit.allocateScratch(StubRegister::A3);
    const GPRReg nextStep = jit.allocateScratch(StubRegister::T10);
    const GPRReg step = jit.allocateScratch(StubRegister::T11);
    const GPRReg status = start;
    const GPRReg target = operation;
    const Address nextStepInFrame(fp, static_cast<int32_t>(ModuleStartFrame::offsetOfNextStep));
    const Address moduleToAbandon(fp, static_cast<int32_t>(ModuleStartFrame::offsetOfModuleToAbandon));
    const TrustedImm32 inRecord(static_cast<int32_t>(StoredModuleStatus::InRecord));
    const TrustedImm32 flagOfStep(static_cast<int32_t>(ModuleStartStepData::flagBit));
    RELEASE_ASSERT(start == directArgumentGPR(0) && start == operationArgumentGPR(0) && operand == operationArgumentGPR(1));
    RELEASE_ASSERT(noOverlap(start, operand, operation, table, nextStep, step, instanceGPR));

    auto callStatusOperation = [&](Entry entry, JumpList& hasThrown) {
        jit.storePtr(nextStep, nextStepInFrame);
        jit.move(instanceGPR, operationArgumentGPR(0));
        jit.callOperation(entry, jit.reuseScratch(StubRegister::Operation));
        hasThrown.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2));
        jit.loadPtr(nextStepInFrame, nextStep);
    };

    jit.zeroExtend32ToWord(start, start);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(modules.status)), table);
    jit.load8(BaseIndex(table, start, CCallHelpers::TimesOne), operand);
    Jump hasBegunBeforeCall = jit.branch32(CCallHelpers::Above, operand, inRecord);

    Label entry = jit.label();
    jit.checkStackBeforeEntering();
    jit.emitFunctionPrologue();
    jit.subPtr(TrustedImm32(ModuleStartFrame::size), sp);
    jit.store64(TrustedImm32(0), moduleToAbandon);
    jit.zeroExtend32ToWord(start, start);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), table);
    loadProgramTable(jit, table, ProgramTableIndex::moduleStarts, step);
    jit.load32(BaseIndex(step, start, CCallHelpers::TimesFour), step);
    loadProgramTable(jit, table, ProgramTableIndex::moduleStartSteps, nextStep);
    jit.getEffectiveAddress(BaseIndex(nextStep, step, CCallHelpers::TimesFour), nextStep);

    Label takesNextStep = jit.label();
    jit.load32(Address(nextStep), step);
    jit.addPtr(TrustedImm32(sizeof(ModuleStartStepData)), nextStep);
    jit.urshift32(step, TrustedImm32(ModuleStartStepData::operandShift), operand);
    jit.and32(TrustedImm32(static_cast<int32_t>(ModuleStartStepData::operationMask)), step, operation);
    auto branchIfStepIs = [&](Operation tested) {
        return jit.branch32(CCallHelpers::Equal, operation, TrustedImm32(static_cast<int32_t>(tested)));
    };
    JumpList begins;
    JumpList returns;
    JumpList hasThrown;
    Jump callsStart = branchIfStepIs(Operation::CallStart);
    Jump callsBody = branchIfStepIs(Operation::CallBody);
    begins.append(branchIfStepIs(Operation::BeginOrReturn));
    Jump finishesAndReturns = branchIfStepIs(Operation::FinishAndReturn);
    Jump callsStartUnlessStopped = branchIfStepIs(Operation::CallStartUnlessStopped);
    returns.append(branchIfStepIs(Operation::Return));
    Jump abandonsOnThrow = branchIfStepIs(Operation::AbandonOnThrow);
    begins.append(branchIfStepIs(Operation::Begin));
    Jump mayStopLoading = branchIfStepIs(Operation::ReturnIfLoadingStops);
    Jump finishes = branchIfStepIs(Operation::Finish);
    jit.breakpoint();

    callsStart.link(&jit);
    Label startsModule = jit.label();
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(modules.status)), table);
    jit.load8(BaseIndex(table, operand, CCallHelpers::TimesOne), status);
    jit.branch32(CCallHelpers::Above, status, inRecord).linkTo(takesNextStep, &jit);
    jit.move(operand, start);
    jit.loadLabelAddress(entry, target);
    Jump hasTarget = jit.jump();

    callsBody.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.functionStartGroups)), table);
    jit.urshift32(operand, TrustedImm32(FunctionStarts::groupShift), step);
    jit.load32(BaseIndex(table, step, CCallHelpers::TimesFour), target);
    Jump isInNarrowGroup = jit.branch32(CCallHelpers::GreaterThanOrEqual, target, TrustedImm32(0));
    jit.and32(TrustedImm32(static_cast<int32_t>(~FunctionStarts::wideGroup)), target);
    jit.and32(TrustedImm32(FunctionStarts::functionsInGroup - 1), operand, step);
    jit.add32(step, target);
    jit.load32(BaseIndex(table, target, CCallHelpers::TimesFour), target);
    Jump hasStart = jit.jump();
    isInNarrowGroup.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.functionSteps)), table);
    jit.load16(BaseIndex(table, operand, CCallHelpers::TimesTwo), step);
    jit.add32(step, target);
    hasStart.link(&jit);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.code)), target);
    jit.move(TrustedImm64(JSValue::ValueUndefined), directArgumentGPR(0));

    hasTarget.link(&jit);
    jit.storePtr(nextStep, nextStepInFrame);
    jit.call(target, JSEntryPtrTag);
    Label afterCall = jit.bindReturnLabel(StubFrameKind::ModuleStart);
    jit.addPtr(TrustedImm32(-static_cast<int32_t>(ModuleStartFrame::size)), fp, sp);
    jit.loadPtr(nextStepInFrame, nextStep);
    jit.branchTestPtr(CCallHelpers::NonZero, nextStep).linkTo(takesNextStep, &jit);

    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTCatchException, jit.reuseScratch(StubRegister::Operation));
    jit.store64(GPRInfo::returnValueGPR, nextStepInFrame);
    jit.move(GPRInfo::returnValueGPR, operationArgumentGPR(2));
    jit.load32(moduleToAbandon, operationArgumentGPR(1));
    jit.sub32(TrustedImm32(1), operationArgumentGPR(1));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTAbandonModuleEvaluation, jit.reuseScratch(StubRegister::Operation));
    jit.load64(nextStepInFrame, operationArgumentGPR(1));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTThrow, jit.reuseScratch(StubRegister::Operation));
    jit.emitFunctionEpilogue();
    jit.jumpToExceptionHandler();

    begins.link(&jit);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(modules.status)), table);
    jit.load8(BaseIndex(table, operand, CCallHelpers::TimesOne), status);
    Jump hasBegun = jit.branch32(CCallHelpers::Above, status, inRecord);
    JumpList asksRunTime;
    asksRunTime.append(jit.branchTest32(CCallHelpers::NonZero, step, flagOfStep));
    asksRunTime.append(jit.branch32(CCallHelpers::Equal, status, inRecord));
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(modules.evaluatingMark)), status);
    jit.store8(status, BaseIndex(table, operand, CCallHelpers::TimesOne));
    jit.jump().linkTo(takesNextStep, &jit);

    asksRunTime.link(&jit);
    callStatusOperation(Entry::operationAOTBeginModuleEvaluation, hasThrown);
    jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR).linkTo(takesNextStep, &jit);
    jit.load32(Address(nextStep, -static_cast<int32_t>(sizeof(ModuleStartStepData))), operation);
    jit.and32(TrustedImm32(static_cast<int32_t>(ModuleStartStepData::operationMask)), operation);
    hasBegun.link(&jit);
    branchIfStepIs(Operation::Begin).linkTo(takesNextStep, &jit);
    returns.append(jit.jump());

    auto finish = [&] {
        JumpList hasFinished;
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(modules.status)), table);
        Jump storesOverAnyStatus = jit.branchTest32(CCallHelpers::Zero, step, flagOfStep);
        jit.load8(BaseIndex(table, operand, CCallHelpers::TimesOne), status);
        hasFinished.append(jit.branch32(CCallHelpers::Equal, status, inRecord));
        storesOverAnyStatus.link(&jit);
        jit.move(TrustedImm32(static_cast<int32_t>(StoredModuleStatus::Evaluated)), status);
        jit.store8(status, BaseIndex(table, operand, CCallHelpers::TimesOne));
        hasFinished.append(jit.jump());
        return hasFinished;
    };
    finishes.link(&jit);
    finish().linkTo(takesNextStep, &jit);
    finishesAndReturns.link(&jit);
    returns.append(finish());

    abandonsOnThrow.link(&jit);
    jit.add32(TrustedImm32(1), operand);
    jit.store64(operand, moduleToAbandon);
    jit.jump().linkTo(takesNextStep, &jit);

    auto branchUnlessLoadingStops = [&](Label continuation) {
        jit.branchTest32(CCallHelpers::Zero, Address(instanceGPR, AOT_INSTANCE_OFFSET(modules.runsImportedFilesAlone))).linkTo(continuation, &jit);
        jit.loadPtr(Address(instanceGPR, offsetOfMicrotaskQueue), table);
        jit.load64(Address(table, MicrotaskQueue::offsetOfStart()), status);
        jit.branch64(CCallHelpers::Equal, status, Address(table, MicrotaskQueue::offsetOfEnd())).linkTo(continuation, &jit);
    };
    auto loadOperandOfStep = [&](int32_t stepsBack) {
        jit.load32(Address(nextStep, -stepsBack * static_cast<int32_t>(sizeof(ModuleStartStepData))), operand);
        jit.and32(TrustedImm32(static_cast<int32_t>(ModuleStartStepData::operationMask)), operand, operation);
        jit.urshift32(TrustedImm32(ModuleStartStepData::operandShift), operand);
    };
    mayStopLoading.link(&jit);
    branchUnlessLoadingStops(takesNextStep);
    Jump asksAboutOperand = jit.jump();
    callsStartUnlessStopped.link(&jit);
    branchUnlessLoadingStops(startsModule);
    loadOperandOfStep(2);
    asksAboutOperand.link(&jit);
    callStatusOperation(Entry::operationAOTStopLoadingBehindFile, hasThrown);
    returns.append(jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR));
    loadOperandOfStep(1);
    branchIfStepIs(Operation::CallStartUnlessStopped).linkTo(startsModule, &jit);
    jit.jump().linkTo(takesNextStep, &jit);

    returns.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.emitFunctionEpilogue();
    jit.returnToCaller();

    hasBegunBeforeCall.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), jit.result());
    jit.returnToCaller();

    hasThrown.link(&jit);
    emitThrowFromAdapter(jit, afterCall, handleException(false));
}

Address emitLocateRememberedConstructWord(StubAssembler& jit, GPRReg wordOfCall, GPRReg entry)
{
    extractBits(jit, wordOfCall, ClosureWord::functionShift, ConstructWordCache::numberOfEntriesLog2, entry);
    jit.lshiftPtr(TrustedImm32(4), entry);
    jit.addPtr(instanceGPR, entry);
    return Address(entry, AOT_INSTANCE_OFFSET(constructWords.entries));
}

static void emitFindConstructWord(StubAssembler& jit, GPRReg word, Jump isNeeded, Label continuation, Address newTarget, GPRReg candidate)
{
    static_assert(sizeof(ConstructEntrypointData) == 8);
    constexpr ptrdiff_t offsetOfFunction = OBJECT_OFFSETOF(ConstructEntrypointData, function);
    const GPRReg function = jit.reuseScratch(StubRegister::A0);
    const GPRReg first = jit.reuseScratch(StubRegister::A1);
    const GPRReg count = jit.reuseScratch(StubRegister::A2);
    const GPRReg half = jit.reuseScratch(StubRegister::A3);
    const GPRReg tables = candidate;
    RELEASE_ASSERT(noOverlap(word, function, first, count, half, candidate, calleeGPR));
    JumpList isNotListed;

    isNeeded.link(&jit);
    Address rememberedEntry = emitLocateRememberedConstructWord(jit, word, half);
    jit.move(bitMask(ClosureWord::isNotCallableBit), candidate);
    jit.or64(word, candidate);
    Jump isNotRemembered = jit.branch64(CCallHelpers::NotEqual, rememberedEntry.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfCall)), candidate);
    jit.load64(rememberedEntry.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfConstruct)), word);
    jit.jump().linkTo(continuation, &jit);

    isNotRemembered.link(&jit);
    extractBits(jit, word, ClosureWord::functionShift, ClosureWord::numberOfFunctionBits, function);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    jit.load32(programTableCount(tables, ProgramTableIndex::constructEntrypoints), count);
    loadProgramTable(jit, tables, ProgramTableIndex::constructEntrypoints, first);
    isNotListed.append(jit.branchTest32(CCallHelpers::Zero, count));

    Label next = jit.label();
    Jump hasOneCandidate = jit.branch32(CCallHelpers::Equal, count, TrustedImm32(1));
    jit.urshift32(count, TrustedImm32(1), half);
    jit.load32(BaseIndex(first, half, CCallHelpers::TimesEight, offsetOfFunction), candidate);
    Jump isInLowerHalf = jit.branch32(CCallHelpers::Above, candidate, function);
    jit.getEffectiveAddress(BaseIndex(first, half, CCallHelpers::TimesEight), first);
    jit.sub32(half, count);
    jit.jump().linkTo(next, &jit);
    isInLowerHalf.link(&jit);
    jit.move(half, count);
    jit.jump().linkTo(next, &jit);

    hasOneCandidate.link(&jit);
    isNotListed.append(jit.branch32(CCallHelpers::NotEqual, function, Address(first, offsetOfFunction)));
    jit.load32(Address(first, OBJECT_OFFSETOF(ConstructEntrypointData, constructEntrypointAndFlag)), function);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), first);
    Jump allocatesThis = jit.branchTest32(CCallHelpers::NonZero, function, TrustedImm32(static_cast<int32_t>(ConstructEntrypointData::allocatesThis)));
    jit.load64(BaseIndex(first, function, CCallHelpers::TimesEight), candidate);
    isNotListed.append(jit.branchTest64(CCallHelpers::Zero, candidate));
    Address entryTakingThis = emitLocateRememberedConstructWord(jit, word, half);
    jit.or64(bitMask(ClosureWord::isNotCallableBit), word);
    jit.store64(word, entryTakingThis.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfCall)));
    jit.store64(candidate, entryTakingThis.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfConstruct)));
    jit.move(candidate, word);
    jit.jump().linkTo(continuation, &jit);

    allocatesThis.link(&jit);
    Jump hasOtherNewTarget = jit.branch64(CCallHelpers::NotEqual, newTarget, calleeGPR);
    jit.and32(TrustedImm32(static_cast<int32_t>(~ConstructEntrypointData::allocatesThis)), function);
    Address remembered = emitLocateRememberedConstructWord(jit, word, half);
    jit.store64(word, remembered.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfCall)));
    jit.load64(BaseIndex(first, function, CCallHelpers::TimesEight), word);
    isNotListed.append(jit.branchTest64(CCallHelpers::Zero, word));
    jit.store64(word, remembered.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfConstruct)));
    jit.or64(TrustedImm64(static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit)), word);
    jit.jump().linkTo(continuation, &jit);

    hasOtherNewTarget.link(&jit);
    const GPRReg wordOfConstruct = candidate;
    jit.and32(TrustedImm32(static_cast<int32_t>(~ConstructEntrypointData::allocatesThis)), function);
    Address place = emitLocateRememberedConstructWord(jit, word, half);
    jit.load64(BaseIndex(first, function, CCallHelpers::TimesEight), wordOfConstruct);
    Jump isNotCreated = jit.branchTest64(CCallHelpers::Zero, wordOfConstruct);
    jit.store64(word, place.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfCall)));
    jit.store64(wordOfConstruct, place.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfConstruct)));
    isNotCreated.link(&jit);
    jit.and64(TrustedImm64(static_cast<int64_t>(~(1ULL << ClosureWord::takesNewTargetBit))), word);
    jit.jump().linkTo(continuation, &jit);

    isNotListed.link(&jit);
    jit.breakpoint();
}

static void emitLoadConstructReceiver(StubAssembler& jit, GPRReg word, GPRReg base)
{
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), calleeGPR);
    jit.load64(Address(fp, offsetOfConstructEntryWord), word);
    jit.load64(Address(fp, offsetOfConstructThis), thisGPR);
    jit.load32(CCallHelpers::lowWordFor(CallFrameSlot::argumentCountIncludingThis), countGPR);
    jit.sub32(TrustedImm32(1), countGPR);
    jit.addPtr(TrustedImm32(offsetOfFirstArgument), fp, base);
    Jump doesNotTakeNewTarget = jit.branchTest64(CCallHelpers::Zero, word, bitMask(ClosureWord::takesNewTargetBit));
    static_assert(static_cast<int>(CallFrameSlot::firstArgument) == static_cast<int>(CallFrameSlot::thisArgument) + 1);
    jit.subPtr(TrustedImm32(sizeof(Register)), base);
    jit.add32(TrustedImm32(1), countGPR);
    doesNotTakeNewTarget.link(&jit);
}

static void emitSelectConstructResult(StubAssembler& jit, ptrdiff_t offsetOfThis, StubFrameKind frameKind)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    const GPRReg shapeWord = operationArgumentGPR(1);
    const Address thisInFrame(fp, static_cast<int32_t>(offsetOfThis));
    RELEASE_ASSERT(noOverlap(result, shapeWord, instanceGPR));
    JumpList isNotObject;
    JumpList done;
    isNotObject.append(jit.branchIfNotCell(result, DoNotHaveTagRegisters));
    isNotObject.append(jit.branchIfNotObject(result));
    done.append(jit.jump());
    isNotObject.link(&jit);
    jit.load64(thisInFrame, result);
    done.append(jit.branchIfNotCell(result, DoNotHaveTagRegisters));
    jit.load32(Address(result, offsetOfShapeWord), shapeWord);
    jit.and32(TrustedImm32(static_cast<int32_t>(compiledObjectShapeBit | shapeWordIsUnmarked)), shapeWord);
    done.append(jit.branch32(CCallHelpers::NotEqual, shapeWord, TrustedImm32(static_cast<int32_t>(compiledObjectShapeBit))));
    jit.move(result, operationArgumentGPR(1));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTTryFinishBirth, jit.reuseScratch(StubRegister::Operation));
    jit.bindReturnLabel(frameKind);
    jit.load64(thisInFrame, result);
    done.link(&jit);
}

void generateEnterClosureForConstruct(StubAssembler& jit)
{
    const GPRReg word = jit.reuseScratch(StubRegister::T11);
    const GPRReg base = jit.reuseScratch(constructArgumentBaseRegister);
    emitEnterAdapter(jit, constructSlotsSize);
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::thisArgument), base);
    jit.store64(base, Address(fp, AdapterFrame::offsetOfNewTarget));

    Jump isNotFullClosure = jit.branchIfNotType(calleeGPR, CompiledClosureType);
    jit.load64(Address(calleeGPR, offsetOfClosureEntry), word);
    Jump hasWord = jit.jump();
    isNotFullClosure.link(&jit);
    loadSlimClosureWord(jit, calleeGPR, word);
    hasWord.link(&jit);

    Jump isNotPlainFunction = jit.branchTest64(CCallHelpers::NonZero, word, TrustedImm64((1ULL << ClosureWord::isNotCallableBit) | (1ULL << ClosureWord::takesNewTargetBit)));
    jit.store64(word, Address(fp, offsetOfConstructEntryWord));

    Label makesThis = jit.label();
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), operationArgumentGPR(1));
    jit.load64(Address(fp, AdapterFrame::offsetOfNewTarget), operationArgumentGPR(2));
    jit.callOperation(Entry::operationAOTCreateThis, jit.reuseScratch(StubRegister::Operation));
    jit.bindReturnLabel(StubFrameKind::ConstructAdapterMakesThis);
    Jump createThisThrew = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.store64(GPRInfo::returnValueGPR, Address(fp, offsetOfConstructThis));

    Label hasThis = jit.label();
    emitLoadConstructReceiver(jit, word, base);

    EntryCall entryCall = emitLoadArgumentsAndCallEntry(jit, word, Address(base), StubFrameKind::ConstructAdapter);
    emitSelectConstructResult(jit, offsetOfConstructThis, StubFrameKind::ConstructAdapterMakesThis);
    emitAdapterEpilogue(jit);

    createThisThrew.link(&jit);
    emitThrowFromAdapter(jit, entryCall.afterCall, handleException(false));
    emitRareArgumentPaths(jit, entryCall);
    emitPollPath(jit, entryCall);
    emitStackOverflowPath(jit, entryCall);

    isNotPlainFunction.link(&jit);
    Jump isNotCallable = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::isNotCallableBit));
    Jump needsConstructWord = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::takesNewTargetBit));
    isNotCallable.link(&jit);
    Label hasConstructWord = jit.label();
    jit.store64(word, Address(fp, offsetOfConstructEntryWord));
    jit.branchTest64(CCallHelpers::Zero, word, bitMask(ClosureWord::isNotCallableBit)).linkTo(makesThis, &jit);
    jit.store64(TrustedImm64(JSValue::ValueUndefined), Address(fp, offsetOfConstructThis));
    jit.jump().linkTo(hasThis, &jit);

    emitFindConstructWord(jit, word, needsConstructWord, hasConstructWord, Address(fp, AdapterFrame::offsetOfNewTarget), jit.reuseScratch(StubRegister::T9));
}

static uint32_t nonPlainFunctionKindMask()
{
    static_assert(!static_cast<unsigned>(CallableKind::Plain));
    FunctionKindData others { };
    others.isAsync = 1;
    others.isGenerator = 1;
    others.isConstructor = 1;
    others.callableKind = 3;
    uint32_t mask = std::bit_cast<uint16_t>(others);
    for (uint32_t bits = 0; bits <= std::numeric_limits<uint16_t>::max(); ++bits) {
        auto kind = std::bit_cast<FunctionKindData>(static_cast<uint16_t>(bits));
        bool hasCallableKind = kind.callableKind <= static_cast<unsigned>(CallableKind::Method);
        RELEASE_ASSERT(!(bits & mask) == (hasCallableKind && FunctionTables::executableKind(kind) == FunctionTables::ExecutableKind::Constructible));
    }
    return mask;
}

void emitConstructWithPlainFunction(StubAssembler& jit, GPRReg word, JumpList& isNotPlainFunction)
{
    constexpr ptrdiff_t offsetOfThis = PlainFunctionConstructFrame::offsetOfThis;
    constexpr ptrdiff_t offsetOfCallee = PlainFunctionConstructFrame::offsetOfCallee;
    constexpr ptrdiff_t offsetOfWord = PlainFunctionConstructFrame::offsetOfWord;
    constexpr ptrdiff_t offsetOfCount = PlainFunctionConstructFrame::offsetOfCount;
    static_assert(!(-offsetOfCount % stackAlignmentBytes()));
    const GPRReg function = jit.reuseScratch(StubRegister::A0);
    const GPRReg kind = jit.reuseScratch(StubRegister::A2);
    const GPRReg base = jit.reuseScratch(constructArgumentBaseRegister);
    const Address newTarget(fp, offsetOfStackWordFromFrame(0));
    RELEASE_ASSERT(noOverlap(word, function, kind, calleeGPR, countGPR));
    RELEASE_ASSERT(noOverlap(word, base, calleeGPR, countGPR, thisGPR));

    extractBits(jit, word, ClosureWord::functionShift, ClosureWord::numberOfFunctionBits, function);
    loadKindOfClosureFunction(jit, function, kind);
    isNotPlainFunction.append(jit.branchTest32(CCallHelpers::NonZero, kind, TrustedImm32(nonPlainFunctionKindMask())));
    isNotPlainFunction.append(jit.branchPtr(CCallHelpers::Below, sp, StubAssembler::trapAwareSoftStackLimitWord()));

    jit.subPtr(TrustedImm32(static_cast<int32_t>(-offsetOfCount)), sp);
    jit.store64(calleeGPR, Address(fp, offsetOfCallee));
    jit.store64(countGPR, Address(fp, offsetOfCount));
    Jump needsConstructWord = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::takesNewTargetBit));
    Label hasConstructWord = jit.label();
    jit.store64(word, Address(fp, offsetOfWord));
    Jump allocatesThisItself = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::isNotCallableBit));
    jit.move(calleeGPR, operationArgumentGPR(1));
    jit.load64(newTarget, operationArgumentGPR(2));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(Entry::operationAOTCreateThis, jit.reuseScratch(StubRegister::Operation));
    jit.bindReturnLabel(StubFrameKind::PlainFunctionConstructMakesThis);
    Jump createThisThrew = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.move(GPRInfo::returnValueGPR, thisGPR);
    Jump hasThis = jit.jump();
    allocatesThisItself.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
    hasThis.link(&jit);
    jit.store64(thisGPR, Address(fp, offsetOfThis));
    jit.load64(Address(fp, offsetOfCallee), calleeGPR);
    jit.load64(Address(fp, offsetOfWord), word);
    jit.load32(Address(fp, offsetOfCount), countGPR);
    jit.addPtr(TrustedImm32(static_cast<int32_t>(offsetOfStackWordFromFrame(0))), fp, base);
    Jump takesNewTarget = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::takesNewTargetBit));
    jit.addPtr(TrustedImm32(sizeof(Register)), base);
    jit.sub32(TrustedImm32(1), countGPR);
    takesNewTarget.link(&jit);

    EntryCall entryCall = emitLoadArgumentsAndCallEntry(jit, word, Address(base), StubFrameKind::PlainFunctionConstruct);
    emitSelectConstructResult(jit, offsetOfThis, StubFrameKind::PlainFunctionConstructMakesThis);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    jit.returnToCaller();

    createThisThrew.link(&jit);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    jit.jumpToExceptionHandler();

    emitRareArgumentPaths(jit, entryCall);
    emitPollPath(jit, entryCall);
    entryCall.stackDoesNotFit.link(&jit);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    emitThrowStackOverflow(jit);
    emitFindConstructWord(jit, word, needsConstructWord, hasConstructWord, newTarget, base);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

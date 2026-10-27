/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCallAdapters.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTConvention.h"
#include "AOTFunctionTables.h"
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
#include "EntryFrame.h"
#include "JSGlobalObject.h"
#include "StackAlignment.h"
#include "StackManager.h"
#include "VM.h"
#include "VMEntryRecord.h"
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

// vmEntryToJavaScript() and its kin store only to the head of the record in their frame. The frame pointer is still the caller's.
static void emitFillFrameOfEntryFromNativeCode(StubAssembler& jit, GPRReg vm, AdapterCaller caller)
{
    const ptrdiff_t begin = EntryFrame::vmEntryRecordOffset() + OBJECT_OFFSETOF(VMEntryRecord, calleeSaveRegistersBuffer);
    RELEASE_ASSERT(begin < 0 && !(begin % static_cast<ptrdiff_t>(stackAlignmentBytes())));
    Jump isCalledByInterpreter;
    if (caller == AdapterCaller::NativeCodeOrInterpreter)
        isCalledByInterpreter = jit.branchPtr(CCallHelpers::NotEqual, Address(vm, VM::topEntryFrameOffset()), fp);
    else if (jit.options().validates) {
        Jump isCalledByNativeCode = jit.branchPtr(CCallHelpers::Equal, Address(vm, VM::topEntryFrameOffset()), fp);
        jit.breakpoint();
        isCalledByNativeCode.link(&jit);
    }
#if CPU(ARM64)
    for (ptrdiff_t offset = begin; offset < 0; offset += 2 * sizeof(CPURegister))
        jit.storePair64(ARM64Registers::zr, ARM64Registers::zr, fp, TrustedImm32(static_cast<int32_t>(offset)));
#else
    for (ptrdiff_t offset = begin; offset < 0; offset += sizeof(CPURegister))
        jit.store64(TrustedImm32(0), Address(fp, static_cast<int32_t>(offset)));
    if (caller == AdapterCaller::NativeCodeOrInterpreter) {
        // An even number of arguments, this among them, leaves one word between the arguments and the record.
        Jump argumentsFillFrame = jit.branchTest32(CCallHelpers::NonZero, incomingFrameSlot(CallFrameSlot::argumentCountIncludingThis), TrustedImm32(1));
        jit.store64(TrustedImm32(JSValue::ValueUndefined), Address(fp, static_cast<int32_t>(EntryFrame::vmEntryRecordOffset() - sizeof(Register))));
        argumentsFillFrame.link(&jit);
    }
#endif
    if (isCalledByInterpreter.isSet())
        isCalledByInterpreter.link(&jit);
}

void emitEnterAdapter(StubAssembler& jit, AdapterCaller caller, unsigned extraBytes)
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
        jit.load32(Address(callee, JSCell::structureIDOffset()), instance);
        jit.add64(Address(scratch, VM::offsetOfAOTState() + VMState::offsetOfStructureIDBase()), instance);
        jit.loadPtr(Address(instance, Structure::realmOffset()), instance);
        jit.loadPtr(Address(instance, JSGlobalObject::offsetOfAOTInstance()), instance);
        jit.jump().linkTo(hasInstance, &jit);
    });

    emitFillFrameOfEntryFromNativeCode(jit, scratch, caller);
    jit.emitFunctionPrologue();
    static_assert(!(AdapterFrame::saveAreaSize % stackAlignmentBytes()));
    jit.subPtr(TrustedImm32(AdapterFrame::saveAreaSize + extraBytes), sp);
    emitSaveRegistersInAdapter(jit, instance);
    // The word that only aligns the area.
    if constexpr (-AdapterFrame::offsetOfNewTarget < static_cast<ptrdiff_t>(AdapterFrame::saveAreaSize))
        jit.store64(TrustedImm32(0), Address(fp, AdapterFrame::offsetOfNewTarget - static_cast<ptrdiff_t>(sizeof(CPURegister))));
    jit.move(instance, instanceGPR);
    if (jit.options().validates) {
        jit.store64(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(scratch.didEnterCompiledCode)));
        // A job and a step of an async generator come here without an entry scope of the engine, and the checks of operations ask the VM whether code ran.
        jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(vm)), cellCageBaseGPR);
        jit.store8(TrustedImm32(1), Address(cellCageBaseGPR, OBJECT_OFFSETOF(VM, didEnterVM)));
    }
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(cellCageBase)), cellCageBaseGPR);
#if !CPU(X86_64)
    jit.emitMaterializeTagCheckRegisters();
#endif
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));
}

void emitRestoreAdapterCallerRegisters(StubAssembler& jit, GPRReg adapterFrame)
{
    // What a caller expects of a method waits in these words while the method is looked up. A getter or a trap that the lookup ran has left its own calls in them.
    if (jit.options().validates)
        jit.store64(TrustedImm32(-1), Address(instanceGPR, AOT_INSTANCE_OFFSET(scratch.expectedCall.site)));
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

[[nodiscard]] static EntryCall emitLoadArgumentsAndCallEntry(StubAssembler& jit, GPRReg word, Address arguments, StubFrameKind kind)
{
    EntryCall call(arguments, kind);
    call.passesMore.append(jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(numberOfArgumentGPRs)));
    constexpr GPRReg numberOfParameters = argumentGPR(numberOfArgumentGPRs - 1);
    ASSERT(noOverlap(word, numberOfParameters, arguments.base));
    extractBits(jit, word, ClosureWord::numberOfParametersShift, ClosureWord::numberOfParametersBits, numberOfParameters);
    jit.xor32(TrustedImm32(ClosureWord::maxNumberOfParameters), numberOfParameters);
    call.passesFewer.append(jit.branch32(CCallHelpers::Below, countGPR, numberOfParameters));
    emitLoadArgumentRegisters(jit, arguments);

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

// The caller does not read the callee frame again. The stack words slide down over the adapter frame, which is rebuilt above them with the arguments that go in registers.
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
StubSignature signatureOfEnterClosureForConstruct(StubKey) { return { }; }

void generateEnterClosure(StubAssembler& jit)
{
    const GPRReg word = jit.reuseScratch(StubRegister::T11);
    JumpList hasWord;
    emitEnterAdapter(jit, AdapterCaller::NativeCodeOrInterpreter);
    jit.load64(Address(calleeGPR, offsetOfClosureEntry), word);
    hasWord.append(jit.jump());

    // The callee only says which realm is entered. The first argument is the number of the function, an int32.
    jit.bindEntry(enterModuleBody());
    emitEnterAdapter(jit, AdapterCaller::NativeCodeOrInterpreter);
    jit.load32(Address(fp, offsetOfFirstArgument), word);
    jit.lshift64(TrustedImm32(3), word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(functionTables.closureWords)), word);
    jit.load64(Address(word), word);
    hasWord.append(jit.jump());

    jit.bindEntry(enterSlimClosure());
    emitEnterAdapter(jit, AdapterCaller::NativeCodeOrInterpreter);
    loadSlimClosureWord(jit, calleeGPR, word);

    hasWord.link(&jit);
    emitLoadThisAndCount(jit);
    EntryCall entryCall = emitLoadArgumentsAndCallEntry(jit, word, Address(fp, offsetOfFirstArgument), StubFrameKind::Adapter);
    emitAdapterEpilogue(jit);

    emitRareArgumentPaths(jit, entryCall);
    emitPollPath(jit, entryCall);
    emitStackOverflowPath(jit, entryCall);
}

Address emitLocateRememberedConstructWord(StubAssembler& jit, GPRReg wordOfCall, GPRReg entry)
{
    extractBits(jit, wordOfCall, ClosureWord::functionShift, ConstructWordCache::numberOfEntriesLog2, entry);
    jit.lshiftPtr(TrustedImm32(4), entry);
    jit.addPtr(instanceGPR, entry);
    return Address(entry, AOT_INSTANCE_OFFSET(constructWords.entries));
}

// A binary search of ProgramTables::constructEntrypoints, which is ascending by function and lists every function that comes here.
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
    jit.load64(BaseIndex(first, function, CCallHelpers::TimesEight), word);
    jit.branchTest64(CCallHelpers::NonZero, word).linkTo(continuation, &jit);
    isNotListed.append(jit.jump());

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
    jit.and64(TrustedImm64(static_cast<int64_t>(~(1ULL << ClosureWord::takesNewTargetBit))), word);
    jit.jump().linkTo(continuation, &jit);

    isNotListed.link(&jit);
    jit.breakpoint();
}

[[nodiscard]] static Jump emitCreateThis(StubAssembler& jit, GPRReg word)
{
    Jump isClassConstructor = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::isNotCallableBit));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), operationArgumentGPR(1));
    jit.load64(Address(fp, AdapterFrame::offsetOfNewTarget), operationArgumentGPR(2));
    jit.callOperation(Entry::operationAOTCreateThis, jit.reuseScratch(StubRegister::Operation));
    Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.store64(GPRInfo::returnValueGPR, Address(fp, offsetOfConstructThis));
    Jump hasThis = jit.jump();
    isClassConstructor.link(&jit);
    jit.store64(TrustedImm64(JSValue::ValueUndefined), Address(fp, offsetOfConstructThis));
    hasThis.link(&jit);
    return threw;
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

static void emitSelectConstructResult(StubAssembler& jit, ptrdiff_t offsetOfThis)
{
    constexpr GPRReg result = GPRInfo::returnValueGPR;
    JumpList isNotObject;
    isNotObject.append(jit.branchIfNotCell(result, DoNotHaveTagRegisters));
    isNotObject.append(jit.branchIfNotObject(result));
    Jump isObject = jit.jump();
    isNotObject.link(&jit);
    jit.load64(Address(fp, static_cast<int32_t>(offsetOfThis)), result);
    isObject.link(&jit);
}

void generateEnterClosureForConstruct(StubAssembler& jit)
{
    const GPRReg word = jit.reuseScratch(StubRegister::T11);
    const GPRReg base = jit.reuseScratch(constructArgumentBaseRegister);
    emitEnterAdapter(jit, AdapterCaller::NativeCodeOrInterpreter, constructSlotsSize);
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::thisArgument), base);
    jit.store64(base, Address(fp, AdapterFrame::offsetOfNewTarget));

    Jump isNotFullClosure = jit.branchIfNotType(calleeGPR, CompiledClosureType);
    jit.load64(Address(calleeGPR, offsetOfClosureEntry), word);
    Jump hasWord = jit.jump();
    isNotFullClosure.link(&jit);
    loadSlimClosureWord(jit, calleeGPR, word);
    hasWord.link(&jit);

    // The word of a plain function that reads new.target has the address of its [[Call]] entry, which takes no new.target.
    Jump isNotCallable = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::isNotCallableBit));
    Jump needsConstructWord = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::takesNewTargetBit));
    isNotCallable.link(&jit);
    Label hasConstructWord = jit.label();
    jit.store64(word, Address(fp, offsetOfConstructEntryWord));

    Jump createThisThrew = emitCreateThis(jit, word);
    emitLoadConstructReceiver(jit, word, base);

    EntryCall entryCall = emitLoadArgumentsAndCallEntry(jit, word, Address(base), StubFrameKind::ConstructAdapter);
    emitSelectConstructResult(jit, offsetOfConstructThis);
    emitAdapterEpilogue(jit);

    createThisThrew.link(&jit);
    emitThrowFromAdapter(jit, entryCall.afterCall, handleException(false));
    emitRareArgumentPaths(jit, entryCall);
    emitPollPath(jit, entryCall);
    emitStackOverflowPath(jit, entryCall);
    emitFindConstructWord(jit, word, needsConstructWord, hasConstructWord, Address(fp, AdapterFrame::offsetOfNewTarget), jit.reuseScratch(StubRegister::T9));
}

// No bit of the mask is set in the kind of a function that may be called and constructed.
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
        RELEASE_ASSERT(!(bits & mask) == (FunctionTables::executableKind(kind) == FunctionTables::ExecutableKind::Constructible));
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
    // The call below comes by no stub that records its callee.
    isNotPlainFunction.append(jit.branchPtr(CCallHelpers::Below, sp, StubAssembler::limitOfCallsByValue()));

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
    jit.bindReturnLabel(StubFrameKind::BelowSpilledArguments);
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
    // new.target lies in front of the arguments and is counted with them.
    jit.addPtr(TrustedImm32(static_cast<int32_t>(offsetOfStackWordFromFrame(0))), fp, base);
    Jump takesNewTarget = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::takesNewTargetBit));
    jit.addPtr(TrustedImm32(sizeof(Register)), base);
    jit.sub32(TrustedImm32(1), countGPR);
    takesNewTarget.link(&jit);

    EntryCall entryCall = emitLoadArgumentsAndCallEntry(jit, word, Address(base), StubFrameKind::PlainFunctionConstruct);
    emitSelectConstructResult(jit, offsetOfThis);
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

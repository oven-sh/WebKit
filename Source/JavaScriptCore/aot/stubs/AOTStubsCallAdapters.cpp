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
#include "AOTStubsCallFrames.h"
#include "AOTStubsCalls.h"
#include "AOTStubsClosures.h"
#include "AOTStubsExceptions.h"
#include "CallFrame.h"
#include "JSGlobalObject.h"
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

struct ArgumentArea {
    GPRReg base;
    ptrdiff_t offset;
};

struct ArgumentPaths {
    JumpList passesFewer;
    JumpList passesMore;
};

struct EntryCall {
    Label afterCall;
    JumpList stackDoesNotFit;
};

struct ConstructWordSearch {
    Jump isNeeded;
    Label continuation;
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

static void loadCalleeAndVM(StubAssembler& jit, GPRReg callee, GPRReg vm)
{
    jit.loadPtr(incomingFrameSlot(CallFrameSlot::callee), callee);
    jit.move(callee, vm);
    loadCellVM(jit, vm);
}

static void emitFindInstance(StubAssembler& jit, GPRReg callee, GPRReg instance, GPRReg scratch)
{
    loadCalleeAndVM(jit, callee, instance);
    jit.load32(Address(callee, JSCell::structureIDOffset()), scratch);
    jit.add64(Address(instance, VM::offsetOfAOTState() + VMState::offsetOfStructureIDBase()), scratch);
    jit.loadPtr(Address(scratch, Structure::realmOffset()), instance);
    jit.loadPtr(Address(instance, JSGlobalObject::offsetOfAOTInstance()), instance);
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

static void emitAdapterPrologue(StubAssembler& jit, GPRReg instance, unsigned extraBytes)
{
    ASSERT(noOverlap(instance, thisGPR, countGPR, calleeGPR));
    jit.emitFunctionPrologue();
    static_assert(!(AdapterFrame::saveAreaSize % stackAlignmentBytes()));
    jit.subPtr(TrustedImm32(AdapterFrame::saveAreaSize + extraBytes), sp);
    emitSaveRegistersInAdapter(jit, instance);
    jit.move(instance, instanceGPR);
    if (jit.options().validates)
        jit.store64(TrustedImm32(1), Address(instanceGPR, AOT_INSTANCE_OFFSET(scratch.didEnterCompiledCode)));
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(cellCageBase)), cellCageBaseGPR);
#if !CPU(X86_64)
    jit.emitMaterializeTagCheckRegisters();
#endif
    jit.storePtr(CCallHelpers::TrustedImmPtr(nullptr), CCallHelpers::addressFor(CallFrameSlot::codeBlock));
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::callee), calleeGPR);
}

void emitAdapterEpilogue(StubAssembler& jit)
{
    // What a caller expects of a method waits in these words while the method is looked up. A getter or a trap that the lookup ran has left its own calls in them.
    if (jit.options().validates)
        jit.store64(TrustedImm32(-1), Address(instanceGPR, AOT_INSTANCE_OFFSET(scratch.expectedCall.site)));
#if CPU(ARM64)
    jit.loadPtr(Address(fp, offsetOfInstanceRegisterInAdapter), instanceGPR);
    jit.loadPtr(Address(fp, offsetOfCellCageBaseRegisterInAdapter), cellCageBaseGPR);
    jit.loadPair64(fp, TrustedImm32(offsetOfNumberTagRegisterInAdapter), GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister);
#else
    for (const AdapterFrame::SavedRegister& saved : AdapterFrame::savedRegisters)
        jit.loadPtr(Address(fp, static_cast<int32_t>(saved.offset)), saved.reg);
#endif
    jit.emitFunctionEpilogue();
    jit.returnToCaller();
}

void emitEnterAdapter(StubAssembler& jit, GPRReg calleeScratch, unsigned extraBytes)
{
    const GPRReg instance = jit.reuseScratch(StubRegister::EntryT12);
    emitFindInstance(jit, calleeScratch, instance, jit.reuseScratch(StubRegister::T9));
    emitAdapterPrologue(jit, instance, extraBytes);
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

static void emitLoadArgumentRegisters(StubAssembler& jit, const ArgumentArea& area)
{
#if CPU(ARM64)
    static_assert(!(numberOfArgumentGPRs % 2));
    for (unsigned i = 0; i < numberOfArgumentGPRs; i += 2)
        jit.loadPair64(area.base, TrustedImm32(static_cast<int32_t>(area.offset + i * sizeof(Register))), argumentGPR(i), argumentGPR(i + 1));
#else
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        jit.load64(Address(area.base, static_cast<int32_t>(area.offset + i * sizeof(Register))), argumentGPR(i));
#endif
}

[[nodiscard]] static ArgumentPaths emitLoadArguments(StubAssembler& jit, GPRReg word, const ArgumentArea& area)
{
    ArgumentPaths paths;
    paths.passesMore.append(jit.branch32(CCallHelpers::Above, countGPR, TrustedImm32(numberOfArgumentGPRs)));
    constexpr GPRReg numberOfParameters = argumentGPR(numberOfArgumentGPRs - 1);
    ASSERT(noOverlap(word, numberOfParameters, area.base));
    extractBits(jit, word, ClosureWord::numberOfParametersShift, ClosureWord::numberOfParametersBits, numberOfParameters);
    jit.xor32(TrustedImm32(ClosureWord::maxNumberOfParameters), numberOfParameters);
    paths.passesFewer.append(jit.branch32(CCallHelpers::Below, countGPR, numberOfParameters));
    emitLoadArgumentRegisters(jit, area);
    return paths;
}

static void emitCopyStackWords(StubAssembler& jit, const ArgumentArea& area, JumpList& stackDoesNotFit)
{
    static_assert(stackAlignmentRegisters() == 2);
    static_assert(numberOfArgumentGPRs >= 4 && gprOf(StubRegister::A3) == argumentGPR(3));
    constexpr int32_t numberOfRegisters = numberOfArgumentGPRs;
    const GPRReg index = jit.reuseScratch(StubRegister::A0);
    const GPRReg value = jit.reuseScratch(StubRegister::A1);
    const GPRReg destination = jit.reuseScratch(StubRegister::A2);
    const GPRReg source = jit.reuseScratch(StubRegister::A3);
    const GPRReg size = value;
    ASSERT(noOverlap(area.base, index, value, destination, source));

    jit.add32(TrustedImm32(1 - numberOfRegisters), countGPR, size);
    jit.and32(TrustedImm32(~1), size);
    jit.lshiftPtr(TrustedImm32(3), size);
    jit.move(sp, destination);
    jit.subPtr(size, destination);
    stackDoesNotFit.append(jit.branchPtr(CCallHelpers::Below, destination, softStackLimit()));
    jit.move(destination, sp);

    jit.add32(TrustedImm32(-numberOfRegisters), countGPR, index);
    jit.addPtr(TrustedImm32(static_cast<int32_t>(area.offset + numberOfArgumentGPRs * sizeof(Register))), area.base, source);
    Label next = jit.label();
    jit.sub32(TrustedImm32(1), index);
    jit.load64(BaseIndex(source, index, CCallHelpers::TimesEight), value);
    jit.store64(value, BaseIndex(destination, index, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, index).linkTo(next, &jit);
}

static void emitRareArgumentPaths(StubAssembler& jit, const ArgumentArea& area, const ArgumentPaths& paths, Label ready, JumpList& stackDoesNotFit)
{
    paths.passesFewer.link(&jit);
    emitLoadArgumentRegisters(jit, area);
    emitFillMissingArgumentRegisters(jit, countGPR, jit.reuseScratch(StubRegister::T13));
    jit.jump().linkTo(ready, &jit);

    paths.passesMore.link(&jit);
    emitCopyStackWords(jit, area, stackDoesNotFit);
    emitLoadArgumentRegisters(jit, area);
    jit.jump().linkTo(ready, &jit);
}

[[nodiscard]] static EntryCall emitCallOfEntry(StubAssembler& jit, GPRReg word, StubFrameKind kind)
{
    EntryCall entryCall;
    entryCall.stackDoesNotFit.append(jit.branchPtr(CCallHelpers::Below, sp, softStackLimit()));
    toCodeAddress(jit, word, jit.reuseScratch(StubRegister::T13), StackIsTested::Yes);
    jit.call(word, JSEntryPtrTag);
    entryCall.afterCall = kind == StubFrameKind::Plain ? jit.label() : jit.bindReturnLabel(kind);
    return entryCall;
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
    emitEnterAdapter(jit, word);
    jit.load64(Address(calleeGPR, offsetOfClosureEntry), word);
    hasWord.append(jit.jump());

    // The callee only says which realm is entered. The first argument is the number of the function, an int32.
    jit.bindEntry(enterModuleBody());
    emitEnterAdapter(jit, word);
    jit.load32(Address(fp, offsetOfFirstArgument), word);
    jit.lshift64(TrustedImm32(3), word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureWords)), word);
    jit.load64(Address(word), word);
    hasWord.append(jit.jump());

    jit.bindEntry(enterSlimClosure());
    emitEnterAdapter(jit, word);
    loadSlimClosureWord(jit, calleeGPR, word);

    hasWord.link(&jit);
    emitLoadThisAndCount(jit);
    const ArgumentArea arguments { fp, offsetOfFirstArgument };
    ArgumentPaths paths = emitLoadArguments(jit, word, arguments);
    Label ready = jit.label();
    EntryCall entryCall = emitCallOfEntry(jit, word, StubFrameKind::Adapter);
    emitAdapterEpilogue(jit);

    emitRareArgumentPaths(jit, arguments, paths, ready, entryCall.stackDoesNotFit);
    emitStackOverflowPath(jit, entryCall);
}

[[nodiscard]] static ConstructWordSearch emitLoadConstructWord(StubAssembler& jit, GPRReg word)
{
    Jump isNotFullClosure = jit.branchIfNotType(calleeGPR, CompiledClosureType);
    jit.load64(Address(calleeGPR, offsetOfClosureEntry), word);
    Jump hasWord = jit.jump();
    isNotFullClosure.link(&jit);
    loadSlimClosureWord(jit, calleeGPR, word);
    hasWord.link(&jit);

    // The word of a plain function that reads new.target has the address of its [[Call]] entry, which takes no new.target.
    ConstructWordSearch search;
    Jump isNotCallable = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::isNotCallableBit));
    search.isNeeded = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::takesNewTargetBit));
    isNotCallable.link(&jit);
    search.continuation = jit.label();
    jit.store64(word, Address(fp, offsetOfConstructEntryWord));
    return search;
}

Address emitLocateRememberedConstructWord(StubAssembler& jit, GPRReg wordOfCall, GPRReg entry)
{
    extractBits(jit, wordOfCall, ClosureWord::functionShift, ConstructWordCache::numberOfEntriesLog2, entry);
    jit.lshiftPtr(TrustedImm32(4), entry);
    jit.addPtr(instanceGPR, entry);
    return Address(entry, AOT_INSTANCE_OFFSET(constructWords.entries));
}

// A binary search of ProgramTables::constructEntrypoints, which is ascending by function and lists every function that comes here.
static void emitFindConstructWord(StubAssembler& jit, GPRReg word, const ConstructWordSearch& search, Address newTarget, GPRReg candidate)
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

    search.isNeeded.link(&jit);
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
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureWords)), first);
    Jump allocatesThis = jit.branchTest32(CCallHelpers::NonZero, function, TrustedImm32(static_cast<int32_t>(ConstructEntrypointData::allocatesThis)));
    jit.load64(BaseIndex(first, function, CCallHelpers::TimesEight), word);
    jit.branchTest64(CCallHelpers::NonZero, word).linkTo(search.continuation, &jit);
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
    jit.jump().linkTo(search.continuation, &jit);

    hasOtherNewTarget.link(&jit);
    jit.and64(TrustedImm64(static_cast<int64_t>(~(1ULL << ClosureWord::takesNewTargetBit))), word);
    jit.jump().linkTo(search.continuation, &jit);

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
    emitEnterAdapter(jit, word, constructSlotsSize);
    jit.load64(CCallHelpers::addressFor(CallFrameSlot::thisArgument), base);
    jit.store64(base, Address(fp, AdapterFrame::offsetOfNewTarget));

    ConstructWordSearch search = emitLoadConstructWord(jit, word);
    Jump createThisThrew = emitCreateThis(jit, word);
    emitLoadConstructReceiver(jit, word, base);

    const ArgumentArea arguments { base, 0 };
    ArgumentPaths paths = emitLoadArguments(jit, word, arguments);
    Label ready = jit.label();
    EntryCall entryCall = emitCallOfEntry(jit, word, StubFrameKind::ConstructAdapter);
    emitSelectConstructResult(jit, offsetOfConstructThis);
    emitAdapterEpilogue(jit);

    createThisThrew.link(&jit);
    emitThrowFromAdapter(jit, entryCall.afterCall, handleException(false));
    emitRareArgumentPaths(jit, arguments, paths, ready, entryCall.stackDoesNotFit);
    emitStackOverflowPath(jit, entryCall);
    emitFindConstructWord(jit, word, search, Address(fp, AdapterFrame::offsetOfNewTarget), jit.reuseScratch(StubRegister::T9));
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
    constexpr ptrdiff_t offsetOfThis = -8;
    constexpr ptrdiff_t offsetOfCallee = -16;
    constexpr ptrdiff_t offsetOfWord = -24;
    constexpr ptrdiff_t offsetOfCount = -32;
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

    jit.subPtr(TrustedImm32(static_cast<int32_t>(-offsetOfCount)), sp);
    jit.store64(calleeGPR, Address(fp, offsetOfCallee));
    jit.store64(countGPR, Address(fp, offsetOfCount));
    ConstructWordSearch search;
    search.isNeeded = jit.branchTest64(CCallHelpers::NonZero, word, bitMask(ClosureWord::takesNewTargetBit));
    search.continuation = jit.label();
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

    const ArgumentArea arguments { base, 0 };
    ArgumentPaths paths = emitLoadArguments(jit, word, arguments);
    Label ready = jit.label();
    EntryCall entryCall = emitCallOfEntry(jit, word, StubFrameKind::BelowSpilledArguments);
    emitSelectConstructResult(jit, offsetOfThis);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    jit.returnToCaller();

    createThisThrew.link(&jit);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    jit.jumpToExceptionHandler();

    emitRareArgumentPaths(jit, arguments, paths, ready, entryCall.stackDoesNotFit);
    entryCall.stackDoesNotFit.link(&jit);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    emitThrowStackOverflow(jit);
    emitFindConstructWord(jit, word, search, newTarget, base);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

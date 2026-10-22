/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubsCalls.h"

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include "AOTInstance.h"
#include "AOTSlimClosure.h"
#include "AOTStubProgramTables.h"
#include "AOTStubsCallAdapters.h"
#include "AOTStubsCallFrames.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsClosures.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsOperations.h"
#include "FunctionRareData.h"
#include "JSBoundFunction.h"
#include "JSCellButterfly.h"
#include "JSFunction.h"
#include "LocalAllocator.h"
#include "MarkedSpace.h"
#include "NativeExecutable.h"
#include "StackAlignment.h"
#include "StackManager.h"

namespace JSC { namespace AOT {

static constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
static constexpr GPRReg fp = GPRInfo::callFrameRegister;

static constexpr int64_t isNotCallableMask = static_cast<int64_t>(1ULL << ClosureWord::isNotCallableBit);
static constexpr int32_t offsetOfStackWordsAtEntry = static_cast<int32_t>(offsetOfStackWordsFromFrame) - static_cast<int32_t>(prologueStackPointerDelta());

static constexpr Address boundTarget { calleeGPR, JSBoundFunction::offsetOfTargetFunction() };
static constexpr Address boundThis { calleeGPR, JSBoundFunction::offsetOfBoundThis() };
static constexpr Address numberOfBoundArguments { calleeGPR, JSBoundFunction::offsetOfBoundArgsLength() };

StubSignature callConventionSignature(std::initializer_list<StubRegister> operands)
{
    StubSignature signature;
    signature.usesCallConvention = true;
    for (StubRegister operand : operands)
        signature.addOperand(gprOf(operand));
    signature.result = StubResult::GPR;
    signature.firstResult = GPRInfo::returnValueGPR;
    return signature;
}

StubSignature callConventionSignatureWithCount()
{
    StubSignature signature = callConventionSignature({ });
    signature.addOperand(countGPR);
    return signature;
}

StubSignature signatureOfCallValue(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallValueWithoutThis(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallValueCell(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallValueNotClosure(StubKey) { return callConventionSignature({ closureWordRegister }); }
StubSignature signatureOfCallValueLending(StubKey) { return callConventionSignature({ }); }
StubSignature signatureOfCallValueWithCount(StubKey) { return callConventionSignatureWithCount(); }
StubSignature signatureOfCallCellWithCount(StubKey) { return callConventionSignatureWithCount(); }
StubSignature signatureOfCallNotClosureWithCount(StubKey) { return callConventionSignatureWithCount(); }
StubSignature signatureOfCallBoundFunction(StubKey) { return callConventionSignatureWithCount(); }
StubSignature signatureOfConstructValue(StubKey) { return callConventionSignatureWithCount(); }

StubSignature signatureOfCallResolvedValue(StubKey)
{
    StubSignature signature = callConventionSignature({ });
    signature.addOperand(valueEntryGPR);
    return signature;
}

StubSignature signatureOfReturnMisfit(StubKey) { return { }; }

StubSignature signatureOfResolveCallValue(StubKey)
{
    StubSignature signature;
    signature.addOperand(calleeGPR);
    signature.result = StubResult::GPR;
    signature.firstResult = gprOf(closureWordRegister);
    return signature;
}

CallRegisters CallRegisters::allocate(StubAssembler& jit)
{
    const GPRReg word = jit.clobberRegister(StubRegister::T11);
    const GPRReg scratch = jit.clobberRegister(StubRegister::T13);
    auto allocateUnlessTaken = [&](StubRegister stubRegister) {
        const GPRReg reg = gprOf(stubRegister);
        return reg == word || reg == scratch ? reg : jit.allocateScratch(stubRegister);
    };
    const GPRReg target = allocateUnlessTaken(callTargetRegister);
    const GPRReg temporary = allocateUnlessTaken(callTemporaryRegister);
    return { word, scratch, target, temporary };
}

ClosureHeaderBranches branchIfNotPlainClosureHeader(StubAssembler& jit, GPRReg structureID, GPRReg scratch)
{
    static_assert(!JSCell::structureIDOffset() && sizeof(StructureID) == sizeof(uint32_t));
#if CPU(ARM64)
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0])), scratch);
    Jump isSlim = jit.branch32(CCallHelpers::Equal, structureID, scratch);
    jit.load32(Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0])), scratch);
    return { isSlim, jit.branch32(CCallHelpers::NotEqual, structureID, scratch) };
#else
    UNUSED_PARAM(scratch);
    Jump isSlim = jit.branch32(CCallHelpers::Equal, structureID, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0])));
    return { isSlim, jit.branch32(CCallHelpers::NotEqual, structureID, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0]))) };
#endif
}

void loadClosureWordOfThisRealm(StubAssembler& jit, GPRReg closure, const CallRegisters& registers, JumpList& isNotClosureOfThisRealm)
{
    ASSERT(noOverlap(closure, registers.word, registers.scratch));
    jit.load8(Address(closure, JSCell::typeInfoTypeOffset()), registers.scratch);
    Jump isFull = jit.branch32(CCallHelpers::Equal, registers.scratch, TrustedImm32(CompiledClosureType));
    jit.sub32(TrustedImm32(SlimClosureType), registers.scratch);
    isNotClosureOfThisRealm.append(jit.branch32(CCallHelpers::Above, registers.scratch, TrustedImm32(SlimClosureWithPropertiesType - SlimClosureType)));
    loadStructure(jit, closure, registers.scratch);
    isNotClosureOfThisRealm.append(branchIfNotOfThisRealm(jit, registers.scratch));
    loadSlimClosureWord(jit, closure, registers.word);
    Jump hasWord = jit.jump();

    isFull.link(&jit);
    loadStructure(jit, closure, registers.scratch);
    isNotClosureOfThisRealm.append(branchIfNotOfThisRealm(jit, registers.scratch));
    jit.load64(Address(closure, offsetOfClosureEntry), registers.word);
    hasWord.link(&jit);
}

void emitFillMissingArgumentRegisters(StubAssembler& jit, unsigned count)
{
    for (unsigned i = count; i < numberOfArgumentGPRs; ++i)
        jit.move(TrustedImm64(JSValue::ValueUndefined), argumentGPR(i));
}

void emitFillMissingArgumentRegisters(StubAssembler& jit, GPRReg count, GPRReg scratch)
{
    jit.move(TrustedImm64(JSValue::ValueUndefined), scratch);
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i)
        jit.moveConditionally32(CCallHelpers::Above, count, TrustedImm32(i), argumentGPR(i), scratch, argumentGPR(i));
}

void emitEnterClosure(StubAssembler& jit, const CallRegisters& registers, std::optional<unsigned> count, JumpList& isNotCallable)
{
    static_assert(ClosureWord::isNotCallableBit > ClosureWord::numberOfParametersShift && ClosureWord::takesNewTargetBit < ClosureWord::numberOfParametersShift && ClosureWord::functionShift + ClosureWord::numberOfFunctionBits <= ClosureWord::numberOfParametersShift);
    if (!count) {
        isNotCallable.append(jit.branchTest64(CCallHelpers::NonZero, registers.word, TrustedImm64(isNotCallableMask)));
        emitFillMissingArgumentRegisters(jit, countGPR, registers.scratch);
        toCodeAddress(jit, registers.word, registers.scratch, StackIsTested::No);
        jit.farJump(registers.word, JSEntryPtrTag);
        return;
    }

    jit.move(TrustedImm64(ClosureWord::minWordOfCalleeTaking(*count)), registers.scratch);
    Jump notCommonCase = jit.branch64(CCallHelpers::LessThan, registers.word, registers.scratch);
    Label enter = jit.label();
    jit.move(TrustedImm32(*count), countGPR);
    toCodeAddress(jit, registers.word, registers.scratch, StackIsTested::No);
    jit.farJump(registers.word, JSEntryPtrTag);

    notCommonCase.link(&jit);
    isNotCallable.append(jit.branchTest64(CCallHelpers::NonZero, registers.word, TrustedImm64(isNotCallableMask)));
    emitFillMissingArgumentRegisters(jit, *count);
    jit.jump().linkTo(enter, &jit);
}

#if CPU(X86_64)
static Jump branchIfTopByteRefusesCount(StubAssembler& jit, ptrdiff_t offsetOfWord, unsigned count)
{
    return jit.branch8(CCallHelpers::LessThan, Address(calleeGPR, offsetOfWord + ClosureWord::topByteShift / 8), TrustedImm32(ClosureWord::minTopByteOfCalleeTaking(count)));
}

static void emitEnterCodeOfWordAt(StubAssembler& jit, const CallRegisters& registers, ptrdiff_t offsetOfWord, unsigned count)
{
    static_assert(ClosureWord::offsetUnit == 1 && ClosureWord::numberOfOffsetBits == 32);
    jit.checkStackBeforeEntering();
    jit.move(TrustedImm32(count), countGPR);
    jit.load32(Address(calleeGPR, offsetOfWord), registers.word);
    jit.add64(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.code)), registers.word);
    jit.farJump(registers.word, JSEntryPtrTag);
}
#endif

// The callee is a cell. Where it jumps, registers.word holds the StructureID of the callee.
static Jump emitEnterClosureCell(StubAssembler& jit, const CallRegisters& registers, unsigned count, JumpList& isNotCallable)
{
#if CPU(X86_64)
    // In an image with one instance every slim closure is a closure of this instance, and the top byte of the word at that place tells one that is as it was created from every other cell.
    if (jit.options().hasOneInstance) {
        Jump notCommonCase = branchIfTopByteRefusesCount(jit, offsetOfSlimClosureWord, count);
        emitEnterCodeOfWordAt(jit, registers, offsetOfSlimClosureWord, count);
        notCommonCase.link(&jit);
    }
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    Jump isNotSlim = jit.branch32(CCallHelpers::NotEqual, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(slimClosureHeaders[0])));
    if (!jit.options().hasOneInstance) {
        Jump notCommonCase = branchIfTopByteRefusesCount(jit, offsetOfSlimClosureWord, count);
        emitEnterCodeOfWordAt(jit, registers, offsetOfSlimClosureWord, count);
        notCommonCase.link(&jit);
    }
    // What may be called declares more parameters than the call passes arguments.
    isNotCallable.append(jit.branch8(CCallHelpers::LessThan, Address(calleeGPR, offsetOfSlimClosureWord + ClosureWord::topByteShift / 8), TrustedImm32(0)));
    emitFillMissingArgumentRegisters(jit, count);
    emitEnterCodeOfWordAt(jit, registers, offsetOfSlimClosureWord, count);

    isNotSlim.link(&jit);
    Jump isNeither = jit.branch32(CCallHelpers::NotEqual, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(closures.closureHeaders[0])));
    Jump notCommonCase = branchIfTopByteRefusesCount(jit, offsetOfClosureEntry, count);
    emitEnterCodeOfWordAt(jit, registers, offsetOfClosureEntry, count);
    notCommonCase.link(&jit);
    jit.loadPtr(Address(calleeGPR, offsetOfClosureEntry), registers.word);
    emitEnterClosure(jit, registers, count, isNotCallable);
    return isNeither;
#else
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, registers.word, registers.scratch);
    jit.loadPtr(Address(calleeGPR, offsetOfClosureEntry), registers.word);
    emitEnterClosure(jit, registers, count, isNotCallable);
    isSlim.link(&jit);
    jit.loadPtr(Address(calleeGPR, offsetOfSlimClosureWord), registers.word);
    emitEnterClosure(jit, registers, count, isNotCallable);
    return isNeither;
#endif
}

static void emitCallEngineCallee(StubAssembler& jit, unsigned count)
{
    jit.move(TrustedImm32(count), countGPR);
    jit.jumpToStub(callEngineCallee());
}

static Address boundArgument(unsigned index)
{
    ASSERT(index < JSBoundFunction::maxEmbeddedArgs);
    return Address(calleeGPR, JSBoundFunction::offsetOfBoundArgs() + index * sizeof(WriteBarrier<Unknown>));
}

static void emitReplaceBoundFunctionWithTarget(StubAssembler& jit)
{
    jit.load64(boundThis, thisGPR);
    jit.loadPtr(boundTarget, calleeGPR);
}

// The callee is Function.prototype.call bound to a value. Where it jumps, the callee is still the bound function.
static void emitReplaceBoundCallWithFunction(StubAssembler& jit, const CallRegisters& registers, JumpList& isEngineCallee)
{
    jit.load64(boundThis, registers.word);
    isEngineCallee.append(jit.branchIfNotCell(registers.word));
    isEngineCallee.append(branchIfNotFunctionType(jit, registers.word, registers.scratch));
    jit.move(registers.word, calleeGPR);
}

// No stack trace lists the frame of a bound function, so the target is entered in its place.
static void emitCallBoundFunction(StubAssembler& jit, const CallRegisters& registers, unsigned count, JumpList& isEngineCallee)
{
    jit.load32(numberOfBoundArguments, registers.scratch);
    Jump hasBoundArguments = jit.branchTest32(CCallHelpers::NonZero, registers.scratch);
    jit.loadPtr(boundTarget, registers.word);
    Jump bindsCall = jit.branchPtr(CCallHelpers::Equal, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.functionPrototypeCall)));
    jit.load64(boundThis, thisGPR);
    jit.move(registers.word, calleeGPR);
    jit.jumpToStub(callValueCell(count));

    bindsCall.link(&jit);
    emitReplaceBoundCallWithFunction(jit, registers, isEngineCallee);
    if (count) {
        jit.move(argumentGPR(0), thisGPR);
        for (unsigned i = 1; i < count; ++i)
            jit.move(argumentGPR(i), argumentGPR(i - 1));
    } else
        jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
    jit.jumpToStub(callValueCell(count ? count - 1 : 0));

    hasBoundArguments.link(&jit);
    for (unsigned bound = 1; bound <= JSBoundFunction::maxEmbeddedArgs && count + bound <= numberOfArgumentGPRs; ++bound) {
        Jump hasMore = jit.branch32(CCallHelpers::NotEqual, registers.scratch, TrustedImm32(bound));
        for (unsigned i = count; i--;)
            jit.move(argumentGPR(i), argumentGPR(i + bound));
        for (unsigned i = 0; i < bound; ++i)
            jit.load64(boundArgument(i), argumentGPR(i));
        emitReplaceBoundFunctionWithTarget(jit);
        jit.jumpToStub(callValueCell(count + bound));
        hasMore.link(&jit);
    }
    jit.move(TrustedImm32(count), countGPR);
    jit.jumpToStub(callBoundFunction());
}

static void emitCallNotPlainClosure(StubAssembler& jit, const CallRegisters& registers, unsigned count, JumpList& isEngineCallee)
{
    jit.linkToStub(jit.branch32(CCallHelpers::Equal, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.hostFunctionStructureID))), callHostFunction(count));
    JumpList isBoundFunction;
    isBoundFunction.append(jit.branch32(CCallHelpers::Equal, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionStructureID))));

    JumpList isNotCompiled;
    loadClosureWordOfThisRealm(jit, calleeGPR, registers, isNotCompiled);
    emitEnterClosure(jit, registers, count, isEngineCallee);

    isNotCompiled.link(&jit);
    // String, Boolean, Number, Object .. as a callback. Under validation the run time calls every native function that is called by value.
    if (!jit.options().validates)
        jit.linkToStub(jit.branchIfType(calleeGPR, InternalFunctionType), callInternalFunction(CodeSpecializationKind::CodeForCall, count));
    Jump isNoFunction = jit.branchIfNotType(calleeGPR, JSFunctionType);
    loadStructure(jit, calleeGPR, registers.word);
    jit.loadPtr(Address(registers.word, Structure::classInfoOffset()), registers.scratch);
    isBoundFunction.append(jit.branchPtr(CCallHelpers::Equal, registers.scratch, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionClassInfo))));
    if (jit.options().imageHasWebAssembly) {
        jit.loadRuntimePointer(Entry::WebAssemblyFunctionClassInfo, registers.word);
        isEngineCallee.append(jit.branchPtr(CCallHelpers::Equal, registers.scratch, registers.word));
    }
    jit.loadPtr(Address(calleeGPR, JSFunction::offsetOfExecutableOrRareData()), registers.scratch);
    Jump isTagged = jit.branchTestPtr(CCallHelpers::NonZero, registers.scratch, TrustedImm32(JSFunction::rareDataTag));
    jit.linkToStub(jit.branchIfType(registers.scratch, NativeExecutableType), callHostFunction(count));
    isEngineCallee.append(jit.jump());
    isTagged.link(&jit);
    jit.loadPtr(Address(registers.scratch, FunctionRareData::offsetOfExecutable() - JSFunction::rareDataTag), registers.scratch);
    jit.linkToStub(jit.branchIfType(registers.scratch, NativeExecutableType), callHostFunction(count));
    isEngineCallee.append(jit.jump());

    isNoFunction.link(&jit);
    jit.linkToStub(jit.branchIfType(calleeGPR, ProxyObjectType), callProxy(count));
    isEngineCallee.append(jit.jump());

    isBoundFunction.link(&jit);
    emitCallBoundFunction(jit, registers, count, isEngineCallee);
}

void generateCallValue(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const unsigned count = jit.key().count();
    constexpr unsigned sizeOfMove = isX86_64() ? 5 : 4;
    while ((jit.m_assembler.codeSize() + sizeOfMove) % 16)
        jit.nop();
    jit.bindEntry(callValueWithoutThis(count));
    jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
    RELEASE_ASSERT(!(jit.m_assembler.codeSize() % 16));
    jit.bindEntry(callValue(count));

    JumpList isEngineCallee;
    Jump isNotCell = jit.branchIfNotCell(calleeGPR);
    jit.bindEntry(callValueCell(count));
    Jump isNeither = emitEnterClosureCell(jit, registers, count, isEngineCallee);

    isNeither.link(&jit);
    jit.bindEntry(callValueNotClosure(count));
    emitCallNotPlainClosure(jit, registers, count, isEngineCallee);

    isEngineCallee.link(&jit);
    emitCallEngineCallee(jit, count);

    isNotCell.link(&jit);
    jit.callOperationAndReturn(Entry::operationAOTThrowUnlessCallable, { .returns = OperationCall::Returns::Void, .secondArgumentIn = calleeGPR });
}

template<typename Functor>
static void preservingOperandsOfCall(StubAssembler& jit, const Functor& functor)
{
#if CPU(ARM64)
    static_assert(numberOfArgumentGPRs == 8);
    jit.preserving({ thisGPR, calleeGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3), argumentGPR(4), argumentGPR(5), argumentGPR(6), argumentGPR(7) }, functor);
#else
    static_assert(numberOfArgumentGPRs == 4);
    jit.preserving({ thisGPR, calleeGPR, argumentGPR(0), argumentGPR(1), argumentGPR(2), argumentGPR(3) }, functor);
#endif
}

// The cell of the heap is as large as its size class. What lies behind the closure in the frame goes into the padding, which nobody reads.
static void emitCopyLentClosure(StubAssembler& jit, const CallRegisters& registers, GPRReg closure, JumpList& slow)
{
    static_assert(CaptureCounts { 0, 1 }.encoded() == 1 << 4);
    static_assert(SlimClosure::allocationSize(0) == MarkedSpace::sizeStep && SlimClosure::allocationSize(2) == 2 * MarkedSpace::sizeStep);
    const GPRReg cell = registers.word;
    const GPRReg allocator = registers.scratch;
    const GPRReg scratch = jit.clobberRegister(countGPR);
    ASSERT(noOverlap(closure, cell, allocator, scratch, instanceGPR));

    const GPRReg function = cell;
    const GPRReg tables = allocator;
    jit.load32(Address(closure, offsetOfSlimClosureWord + sizeof(uint32_t)), function);
    jit.and32(TrustedImm32(ClosureWord::functionMask), function);
    jit.loadPtr(Address(instanceGPR, AOT_INSTANCE_OFFSET(image.tables)), tables);
    loadProgramTable(jit, tables, ProgramTableIndex::closureCaptureCounts, scratch);
    jit.load32(Address(tables, OBJECT_OFFSETOF(ProgramTables, closureFunctions) + OBJECT_OFFSETOF(ClosureFunctionRangeData, first)), tables);
    jit.sub32(tables, function);
    jit.load8(BaseIndex(scratch, function, CCallHelpers::TimesOne), function);
    slow.append(jit.branch32(CCallHelpers::Equal, function, TrustedImm32(countsAreListed)));
    // Allocator i serves cells of 16 * (i + 1) bytes. The captures stand above the four bits of the references: (captures + 1) / 2.
    jit.add32(TrustedImm32(1 << 4), function);
    jit.urshift32(TrustedImm32(5), function);
    jit.getEffectiveAddress(BaseIndex(instanceGPR, function, CCallHelpers::TimesEight), allocator);
    jit.loadPtr(Address(allocator, AOT_INSTANCE_OFFSET(closures.slimClosureAllocators)), allocator);
    allocateCell(jit, cell, allocator, scratch, slow);

    const GPRReg offset = scratch;
    const GPRReg copied = allocator;
    jit.load32(Address(allocator, LocalAllocator::offsetOfCellSize()), offset);
    Label previousWord = jit.label();
    jit.sub32(TrustedImm32(sizeof(uint64_t)), offset);
    jit.load64(BaseIndex(closure, offset, CCallHelpers::TimesOne), copied);
    jit.store64(copied, BaseIndex(cell, offset, CCallHelpers::TimesOne));
    jit.branchTest32(CCallHelpers::NonZero, offset).linkTo(previousWord, &jit);
    fenceAfterAllocation(jit);
    jit.move(cell, closure);
}

static void emitMoveLentClosuresToHeap(StubAssembler& jit, const CallRegisters& registers, unsigned lentArguments)
{
    for (unsigned argument = 0; argument < ClosureWord::numberOfBorrowedArguments; ++argument) {
        if (!(lentArguments >> argument & 1))
            continue;
        JumpList slow;
        emitCopyLentClosure(jit, registers, argumentGPR(argument), slow);
        Jump isCopied = jit.jump();

        slow.link(&jit);
        jit.emitFunctionPrologue();
        preservingOperandsOfCall(jit, [&] {
            const GPRReg A0 = jit.reuseScratch(StubRegister::A0);
            const GPRReg A1 = jit.reuseScratch(StubRegister::A1);
            jit.move(argumentGPR(argument), A1);
            jit.move(instanceGPR, A0);
            jit.callOperation(Entry::operationAOTMoveClosureToHeap, jit.reuseScratch(StubRegister::Operation));
            jit.move(GPRInfo::returnValueGPR, registers.word);
        });
        jit.emitFunctionEpilogue();
        jit.move(registers.word, argumentGPR(argument));
        isCopied.link(&jit);
    }
}

void generateCallValueLending(StubAssembler& jit)
{
    static_assert(ClosureWord::functionShift == 32 && ClosureWord::borrowsArgumentShift >= ClosureWord::functionShift);
    const CallRegisters registers = CallRegisters::allocate(jit);
    const unsigned count = jit.key().count();
    const unsigned lentArguments = jit.key().form() + 1;
    const TrustedImm32 borrowsAll(static_cast<int32_t>(lentArguments << (ClosureWord::borrowsArgumentShift - ClosureWord::functionShift)));

    jit.linkToStub(jit.branchIfNotCell(calleeGPR), callValue(count));
    JumpList doesNotBorrow;
#if CPU(X86_64)
    // As in emitEnterClosureCell().
    if (jit.options().hasOneInstance) {
        Jump notCommonCase = branchIfTopByteRefusesCount(jit, offsetOfSlimClosureWord, count);
        jit.load32(Address(calleeGPR, offsetOfSlimClosureWord + sizeof(uint32_t)), registers.word);
        jit.and32(borrowsAll, registers.word);
        doesNotBorrow.append(jit.branch32(CCallHelpers::NotEqual, registers.word, borrowsAll));
        emitEnterCodeOfWordAt(jit, registers, offsetOfSlimClosureWord, count);
        notCommonCase.link(&jit);
    }
#endif
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, registers.word, registers.scratch);
    jit.load32(Address(calleeGPR, offsetOfClosureEntry + sizeof(uint32_t)), registers.word);
    Jump hasUpperHalf = jit.jump();
    isSlim.link(&jit);
    jit.load32(Address(calleeGPR, offsetOfSlimClosureWord + sizeof(uint32_t)), registers.word);
    hasUpperHalf.link(&jit);
    jit.and32(borrowsAll, registers.word);
    jit.linkToStub(jit.branch32(CCallHelpers::Equal, registers.word, borrowsAll), callValueCell(count));

    isNeither.link(&jit);
    doesNotBorrow.link(&jit);
    emitMoveLentClosuresToHeap(jit, registers, lentArguments);
    jit.jumpToStub(callValueCell(count));
}

void generateCallValueWithCount(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    JumpList isNotClosure;
    isNotClosure.append(jit.branchIfNotCell(calleeGPR));
    jit.bindEntry(callCellWithCount());
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, registers.word, registers.scratch);
    jit.load64(Address(calleeGPR, offsetOfClosureEntry), registers.word);
    emitEnterClosure(jit, registers, std::nullopt, isNotClosure);

    isSlim.link(&jit);
    jit.load64(Address(calleeGPR, offsetOfSlimClosureWord), registers.word);
    emitEnterClosure(jit, registers, std::nullopt, isNotClosure);

    isNeither.link(&jit);
    loadClosureWordOfThisRealm(jit, calleeGPR, registers, isNotClosure);
    emitEnterClosure(jit, registers, std::nullopt, isNotClosure);

    isNotClosure.link(&jit);
    jit.jumpToStub(callNotClosureWithCount());
}

static void emitCallFunctionBoundToCall(StubAssembler& jit, const CallRegisters& registers, GPRReg count, JumpList& isEngineCallee)
{
    emitReplaceBoundCallWithFunction(jit, registers, isEngineCallee);
    Jump hasNoArguments = jit.branchTest32(CCallHelpers::Zero, count);
    jit.move(argumentGPR(0), thisGPR);
    for (unsigned i = 1; i < numberOfArgumentGPRs; ++i)
        jit.move(argumentGPR(i), argumentGPR(i - 1));
    jit.sub32(TrustedImm32(1), count);
    Jump hasStackWords = jit.branch32(CCallHelpers::AboveOrEqual, count, TrustedImm32(numberOfArgumentGPRs));
    jit.jumpToStub(callCellWithCount());

    hasNoArguments.link(&jit);
    jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
    jit.jumpToStub(callCellWithCount());

    // The stack words are the callee's to change. The first one moves to the last register, the others down by one.
    hasStackWords.link(&jit);
    const GPRReg stackWord = registers.word;
    const GPRReg remaining = registers.scratch;
    const FPRReg moved = jit.allocateScratch(FPRInfo::fpRegT0);
    jit.addPtr(TrustedImm32(offsetOfStackWordsAtEntry), sp, stackWord);
    jit.load64(Address(stackWord), argumentGPR(numberOfArgumentGPRs - 1));
    jit.move(count, remaining);
    jit.sub32(TrustedImm32(numberOfArgumentGPRs), remaining);
    Jump hasMovedEveryWord = jit.branchTest32(CCallHelpers::Zero, remaining);
    Label next = jit.label();
    jit.loadDouble(Address(stackWord, sizeOfStackWord), moved);
    jit.storeDouble(moved, Address(stackWord));
    jit.addPtr(TrustedImm32(sizeOfStackWord), stackWord);
    jit.sub32(TrustedImm32(1), remaining);
    jit.branchTest32(CCallHelpers::NonZero, remaining).linkTo(next, &jit);
    hasMovedEveryWord.link(&jit);
    jit.jumpToStub(callCellWithCount());
}

// All arguments fit in the registers, and bound is 1 to JSBoundFunction::maxEmbeddedArgs.
static void emitPrependEmbeddedArguments(StubAssembler& jit, GPRReg bound)
{
    static_assert(JSBoundFunction::maxEmbeddedArgs < numberOfArgumentGPRs);
    JumpList areInPlace;
    for (unsigned number = 1; number <= JSBoundFunction::maxEmbeddedArgs; ++number) {
        Jump hasMore;
        if (number < JSBoundFunction::maxEmbeddedArgs)
            hasMore = jit.branch32(CCallHelpers::NotEqual, bound, TrustedImm32(number));
        for (unsigned i = numberOfArgumentGPRs; i-- > number;)
            jit.move(argumentGPR(i - number), argumentGPR(i));
        for (unsigned i = 0; i < number; ++i)
            jit.load64(boundArgument(i), argumentGPR(i));
        if (number < JSBoundFunction::maxEmbeddedArgs) {
            areInPlace.append(jit.jump());
            hasMore.link(&jit);
        }
    }
    areInPlace.link(&jit);
}

// index is the number of words, which is not zero.
static void emitCopyFromLastWord(StubAssembler& jit, Address source, Address destination, GPRReg index, GPRReg word)
{
    Label previous = jit.label();
    jit.sub32(TrustedImm32(1), index);
    jit.load64(source.indexedBy(index, CCallHelpers::TimesEight), word);
    jit.store64(word, destination.indexedBy(index, CCallHelpers::TimesEight));
    jit.branchTest32(CCallHelpers::NonZero, index).linkTo(previous, &jit);
}

// The list of all arguments gets a frame of its own, as the list of a spread call does. total is bound + count, and bound is not zero.
static void emitCallWithBoundArgumentsInListFrame(StubAssembler& jit, const CallRegisters& registers, GPRReg count, JumpList& isEngineCallee)
{
    static_assert(!(numberOfArgumentGPRs % 2) && stackAlignmentRegisters() == 2);
    constexpr GPRReg index = argumentGPR(0);
    constexpr GPRReg word = argumentGPR(1);
    constexpr GPRReg boundArguments = argumentGPR(2);
    const GPRReg bound = registers.scratch;
    const GPRReg total = registers.word;
    const GPRReg passedArguments = registers.word;
    const GPRReg list = jit.clobberRegister(thisGPR);
    ASSERT(noOverlap(index, word, boundArguments, bound, total, list, count, calleeGPR, instanceGPR));

    isEngineCallee.append(jit.branch32(CCallHelpers::Above, total, TrustedImm32(maxArgumentsOfBoundCall)));
    // Every argument register is stored behind the bound arguments, whatever the count is.
    jit.add32(TrustedImm32(numberOfArgumentGPRs + 1), total, list);
    jit.and32(TrustedImm32(~1), list);
    jit.lshiftPtr(TrustedImm32(3), list);
    jit.subPtr(sp, list, total);
    jit.subPtr(TrustedImm32(sizeof(CallerFrameAndPC)), total);
    Jump doesNotFitOnStack = jit.branchPtr(CCallHelpers::Below, total, Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfSoftStackLimit()));
    jit.emitFunctionPrologue();
    jit.subPtr(sp, list, list);
    jit.move(list, sp);

    jit.getEffectiveAddress(BaseIndex(list, bound, CCallHelpers::TimesEight), passedArguments);
    for (unsigned i = 0; i < numberOfArgumentGPRs; i += 2)
        jit.storePair64(argumentGPR(i), argumentGPR(i + 1), Address(passedArguments, i * sizeOfStackWord));
    Jump hasNoStackWords = jit.branch32(CCallHelpers::BelowOrEqual, count, TrustedImm32(numberOfArgumentGPRs));
    jit.move(count, index);
    jit.sub32(TrustedImm32(numberOfArgumentGPRs), index);
    emitCopyFromLastWord(jit, Address(fp, static_cast<int32_t>(offsetOfStackWordsFromFrame)), Address(passedArguments, sizeOfSpilledArgumentRegisters), index, word);
    hasNoStackWords.link(&jit);

    jit.addPtr(TrustedImm32(static_cast<int32_t>(JSBoundFunction::offsetOfBoundArgs())), calleeGPR, boundArguments);
    Jump areEmbedded = jit.branch32(CCallHelpers::BelowOrEqual, bound, TrustedImm32(JSBoundFunction::maxEmbeddedArgs));
    jit.loadPtr(Address(boundArguments), boundArguments);
    jit.addPtr(TrustedImm32(static_cast<int32_t>(JSCellButterfly::offsetOfData())), boundArguments);
    areEmbedded.link(&jit);
    jit.move(bound, index);
    emitCopyFromLastWord(jit, Address(boundArguments), Address(list), index, word);

    jit.add32(bound, count);
    for (unsigned i = 0; i < numberOfArgumentGPRs; i += 2)
        jit.loadPair64(Address(list, i * sizeOfStackWord), argumentGPR(i), argumentGPR(i + 1));
    jit.addPtr(TrustedImm32(sizeOfSpilledArgumentRegisters), list, sp);
    emitReplaceBoundFunctionWithTarget(jit);
    jit.loadStubAddress(callCellWithCount(), registers.target);
    jit.jumpToStub(callFromArgumentListFrame());

    doesNotFitOnStack.link(&jit);
    emitThrowStackOverflow(jit);
}

void generateCallNotClosureWithCount(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    const GPRReg count = jit.clobberOperand(0);
    const GPRReg bound = registers.scratch;
    const GPRReg total = registers.word;
    JumpList isEngineCallee;

    isEngineCallee.append(jit.branchIfNotCell(calleeGPR));
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), registers.word);
    Jump hasOriginalStructure = jit.branch32(CCallHelpers::Equal, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionStructureID)));
    isEngineCallee.append(jit.branchIfNotType(calleeGPR, JSFunctionType));
    decodeStructureID(jit, registers.word);
    jit.loadPtr(Address(registers.word, Structure::classInfoOffset()), registers.word);
    isEngineCallee.append(jit.branchPtr(CCallHelpers::NotEqual, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.boundFunctionClassInfo))));
    hasOriginalStructure.link(&jit);

    jit.bindEntry(callBoundFunction());
    jit.load32(numberOfBoundArguments, bound);
    Jump hasBoundArguments = jit.branchTest32(CCallHelpers::NonZero, bound);
    jit.loadPtr(boundTarget, registers.word);
    Jump bindsCall = jit.branchPtr(CCallHelpers::Equal, registers.word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.functionPrototypeCall)));
    jit.load64(boundThis, thisGPR);
    jit.move(registers.word, calleeGPR);
    jit.jumpToStub(callCellWithCount());

    bindsCall.link(&jit);
    emitCallFunctionBoundToCall(jit, registers, count, isEngineCallee);

    hasBoundArguments.link(&jit);
    JumpList needsListFrame;
    jit.add32(count, bound, total);
    needsListFrame.append(jit.branch32(CCallHelpers::Above, total, TrustedImm32(numberOfArgumentGPRs)));
    Jump areInButterfly = jit.branch32(CCallHelpers::Above, bound, TrustedImm32(JSBoundFunction::maxEmbeddedArgs));
    jit.move(total, count);
    emitPrependEmbeddedArguments(jit, bound);
    emitReplaceBoundFunctionWithTarget(jit);
    jit.jumpToStub(callCellWithCount());

    areInButterfly.link(&jit);
    needsListFrame.append(jit.branchTest32(CCallHelpers::NonZero, count));
    const ArgumentList boundArguments { count, registers.word };
    jit.loadPtr(boundArgument(0), boundArguments.words);
    jit.addPtr(TrustedImm32(static_cast<int32_t>(JSCellButterfly::offsetOfData())), boundArguments.words);
    jit.move(bound, boundArguments.count);
    emitLoadArgumentRegistersFromList(jit, boundArguments);
    emitReplaceBoundFunctionWithTarget(jit);
    jit.jumpToStub(callCellWithCount());

    needsListFrame.link(&jit);
    emitCallWithBoundArgumentsInListFrame(jit, registers, count, isEngineCallee);

    jit.linkToStub(isEngineCallee, callEngineCallee());
}

static void emitSpillArgumentsAndPushFrame(StubAssembler& jit)
{
    jit.recordClobbers(RegisterSet::macroClobberedGPRs());
    emitSpillArgumentRegisters(jit);
    jit.emitFunctionPrologue();
}

static Entry constructingOperationOf(ConstructSyntax syntax)
{
    switch (syntax) {
    case ConstructSyntax::NewExpression:
        return Entry::operationAOTConstruct;
    case ConstructSyntax::SuperCall:
        return Entry::operationAOTSuperConstruct;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void emitConstructByOperation(StubAssembler& jit, ConstructSyntax syntax)
{
    static_assert(noOverlap(countGPR, operationArgumentGPR(1), operationArgumentGPR(2)));
    jit.move(calleeGPR, operationArgumentGPR(1));
    jit.load64(Address(fp, offsetOfStackWordFromFrame(0)), operationArgumentGPR(2));
    jit.move(countGPR, operationArgumentGPR(3));
    jit.sub32(TrustedImm32(1), operationArgumentGPR(3));
    jit.addPtr(TrustedImm32(static_cast<int32_t>(offsetOfStackWordFromFrame(1))), fp, operationArgumentGPR(4));
    jit.move(instanceGPR, operationArgumentGPR(0));
    jit.callOperation(constructingOperationOf(syntax), jit.reuseScratch(StubRegister::Operation));
    jit.bindReturnLabel(StubFrameKind::BelowSpilledArguments);
    jit.emitFunctionEpilogue();
    emitDropSpilledArgumentRegisters(jit);
    Jump threw = jit.branchTestPtr(CCallHelpers::NonZero, GPRInfo::returnValueGPR2);
    jit.returnToCaller();

    threw.link(&jit);
    jit.jumpToExceptionHandler();
}

void generateConstructValue(StubAssembler& jit)
{
    const CallRegisters registers = CallRegisters::allocate(jit);
    JumpList isNotClosureOfThisRealm;
    isNotClosureOfThisRealm.append(jit.branchIfNotCell(calleeGPR));
    loadClosureWordOfThisRealm(jit, calleeGPR, registers, isNotClosureOfThisRealm);
    Jump isCallable = jit.branchTest64(CCallHelpers::Zero, registers.word, TrustedImm64(isNotCallableMask));

    JumpList never;
    jit.and64(TrustedImm64(~isNotCallableMask), registers.word);
    emitEnterClosure(jit, registers, std::nullopt, never);
    never.link(&jit);
    jit.breakpoint();

    isCallable.link(&jit);
    // A constructor function that is its own new.target, and whose entrypoint for that was found before. new.target is the first argument, there as here, and the entrypoint makes the object.
    JumpList isNotRemembered;
    isNotRemembered.append(jit.branchTest64(CCallHelpers::Zero, registers.word, TrustedImm64(static_cast<int64_t>(1ULL << ClosureWord::takesNewTargetBit))));
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, argumentGPR(0), calleeGPR));
    Address remembered = emitLocateRememberedConstructWord(jit, registers.word, registers.scratch);
    isNotRemembered.append(jit.branch64(CCallHelpers::NotEqual, remembered.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfCall)), registers.word));
    jit.load64(remembered.withOffset(OBJECT_OFFSETOF(ConstructWordCache::Entry, wordOfConstruct)), registers.word);
    jit.move(TrustedImm64(JSValue::ValueUndefined), thisGPR);
    emitEnterClosure(jit, registers, std::nullopt, never);

    isNotRemembered.link(&jit);
    JumpList isNotPlainFunction;
    emitSpillArgumentsAndPushFrame(jit);
    emitConstructWithPlainFunction(jit, registers.word, isNotPlainFunction);

    isNotClosureOfThisRealm.link(&jit);
    emitSpillArgumentsAndPushFrame(jit);
    isNotPlainFunction.link(&jit);
    emitConstructByOperation(jit, static_cast<ConstructSyntax>(jit.key().form()));
}

void generateResolveCallValue(StubAssembler& jit)
{
    const unsigned count = jit.key().count();
    const GPRReg word = jit.result();
    const GPRReg scratch = jit.allocateScratch(StubRegister::T13);
    JumpList isResolvedAtEachCall;
    isResolvedAtEachCall.append(jit.branchIfNotCell(calleeGPR));
    jit.load32(Address(calleeGPR, JSCell::structureIDOffset()), word);
    auto [isSlim, isNeither] = branchIfNotPlainClosureHeader(jit, word, scratch);
    jit.loadPtr(Address(calleeGPR, offsetOfClosureEntry), word);
    Jump hasWord = jit.jump();
    isSlim.link(&jit);
    jit.loadPtr(Address(calleeGPR, offsetOfSlimClosureWord), word);
    hasWord.link(&jit);
    jit.move(TrustedImm64(ClosureWord::minWordOfCalleeTaking(count)), scratch);
    isResolvedAtEachCall.append(jit.branch64(CCallHelpers::LessThan, word, scratch));
    isResolvedAtEachCall.append(jit.branchPtr(CCallHelpers::Below, CCallHelpers::stackPointerRegister, Address(instanceGPR, AOT_INSTANCE_OFFSET(stackMirror) + StackManager::Mirror::offsetOfTrapAwareSoftStackLimit())));
    toCodeAddress(jit, word, scratch, StackIsTested::Yes);
    jit.returnToCaller();

    isNeither.link(&jit);
    isResolvedAtEachCall.append(jit.branch32(CCallHelpers::NotEqual, word, Address(instanceGPR, AOT_INSTANCE_OFFSET(realm.hostFunctionStructureID))));
    jit.returnStubAddress(callHostFunction(count));

    isResolvedAtEachCall.link(&jit);
    jit.returnStubAddress(callValue(count));
}

void generateCallResolvedValue(StubAssembler& jit)
{
    jit.farJump(jit.operand(0), JSEntryPtrTag);
}

// The frame of the function is gone and everything is as at its return. The return goes to the landing that the caller has for such a value, if it has one.
void generateReturnMisfit(StubAssembler& jit)
{
    constexpr GPRReg A0 = gprOf(StubRegister::A0);
    constexpr GPRReg A1 = gprOf(StubRegister::A1);
    constexpr GPRReg sp = CCallHelpers::stackPointerRegister;
    jit.preservingCallerSavedRegisters(false, [&](unsigned bytes) {
        jit.move(GPRInfo::returnValueGPR, gprOf(StubRegister::A2));
        jit.loadCallerReturnAddress(bytes, A1);
        jit.move(instanceGPR, A0);
        jit.callOperation(Entry::operationAOTFindLandingOfMisfit, gprOf(StubRegister::T11));
#if CPU(X86_64)
        jit.storePtr(GPRInfo::returnValueGPR, Address(sp, bytes));
#else
        UNUSED_PARAM(bytes);
        jit.storePtr(GPRInfo::returnValueGPR, Address(sp));
#endif
    });
    jit.ret();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

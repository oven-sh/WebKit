/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTConvention.h"
#include "AOTStubRegisters.h"
#include "AOTStubTestSupport.h"
#include "CPU.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

namespace {

struct ReadOnlyOperand {
    StubKey body;
    unsigned index;
    GPRReg gpr;
    StubRegister stubRegister;
};

} // anonymous namespace

static constexpr StubKey calledStub { StubFamily::ToBoolean };
static constexpr StubKey bodyWithTwoEntries { StubFamily::StringEqualsLiteral };
static constexpr StubKey secondEntry { StubFamily::ValueEqualsLiteral };
static constexpr StubKey nonexistentStub = StubKey(StubFamily::StringsAreEqual).withCallerFrame(CallerFrame::Frameless);
static constexpr StubKey bodyEnteredAtFaults { StubFamily::ThrowAtFault };
static constexpr Entry preserveMostLeaf = Entry::operationAOTStringImplsAreEqual;
static constexpr Entry ordinaryLeaf = Entry::operationAOTToInt32;

static_assert(exists(calledStub) && !neverReturns(calledStub) && !exists(nonexistentStub));
static_assert(bodyOf(bodyWithTwoEntries) == bodyWithTwoEntries && bodyOf(secondEntry) == bodyWithTwoEntries && bodyOf(calledStub) != bodyWithTwoEntries);
static_assert(exists(bodyEnteredAtFaults) && exists(bodyEnteredAtFaults.withCallerFrame(CallerFrame::Frameless)));
static_assert(isLeaf(RuntimeOperation::StringImplsAreEqual) && preservesMostRegisters(RuntimeOperation::StringImplsAreEqual));
static_assert(isLeaf(RuntimeOperation::ToInt32) && !preservesMostRegisters(RuntimeOperation::ToInt32));

static constexpr StubRegister stubRegisters[] = {
    StubRegister::R0, StubRegister::A0, StubRegister::A1, StubRegister::A2, StubRegister::A3, StubRegister::A4, StubRegister::A5, StubRegister::A6, StubRegister::A7,
    StubRegister::T9, StubRegister::T10, StubRegister::T11, StubRegister::T12, StubRegister::T13, StubRegister::T14, StubRegister::T15,
    StubRegister::EntryT12, StubRegister::EntryT13, StubRegister::Operation,
};

static_assert(std::size(stubRegisters) == static_cast<size_t>(StubRegister::Operation) + 1);

static constexpr FPRReg borrowedFPR = FPRInfo::fpRegT0;

static void checkWrittenRegisters(const RegisterSet& clobbered, const StubSignature& signature)
{
    for (Reg result : { signature.firstResult, signature.secondResult })
        AOT_TEST_CHECK(!result.isSet() || containsRegister(clobbered, result));
    if (signature.immediate)
        AOT_TEST_CHECK(containsRegister(clobbered, *signature.immediate));
}

static void checkRecordedClobbers(const StubRecording& recording)
{
    StubKey body = recording.body;
    AOT_TEST_CHECK(pinnedRegisters().filter(recording.clobbered()).isEmpty());
    for (StubKey entry : recording.entries)
        checkWrittenRegisters(recording.clobberedInline, signatureFor(entry));
    if (!recording.leavesStubs && recording.symbols.isEmpty())
        AOT_TEST_CHECK(recording.clobberedByCallees.isEmpty());

    if (neverReturns(body))
        return;
    if (isARM64() && body.callerFrame() == CallerFrame::Frameless)
        AOT_TEST_CHECK(!containsRegister(recording.clobbered(), faultCallerLinkGPR));
}

static void checkRecording(const StubRecording& recording, StubKey body)
{
    AOT_TEST_CHECK(recording.body == body);

    auto entries = recording.entries;
    std::sort(entries.begin(), entries.end());
    AOT_TEST_CHECK(entries == entriesOf(body));

    for (const StubReference& reference : recording.references) {
        AOT_TEST_CHECK(exists(reference.target));
        AOT_TEST_CHECK(reference.kind != StubReference::Kind::Jump || reference.savedAround.isEmpty());
    }
    AOT_TEST_CHECK(!recording.returnLabels.contains(StubFrameKind::Plain));
    AOT_TEST_CHECK(recording.entriesUsed.subsumes(recording.operationsCalled));
    for (const StubExitRecord& exit : recording.exits) {
        AOT_TEST_CHECK(recording.operationsCalled.get(indexOf(exit.operation)));
        AOT_TEST_CHECK((exit.exit == StubExit::Miss) == (exit.reason == RuntimeCounterID::None));
    }
    for (CodeSymbol symbol : recording.symbols)
        AOT_TEST_CHECK(symbol.space == SymbolSpace::RegExp);
    checkRecordedClobbers(recording);
}

static void testStubBodyGeneration()
{
    Vector<StubKey> bodies = allStubBodies();
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options, StubPolicy { });
        StubAssembler jit(options, catalog);
        size_t numberOfBoundEntries = 0;
        for (StubKey body : bodies) {
            StubRecording recording = jit.generate(body);
            checkRecording(recording, body);
            numberOfBoundEntries += recording.entries.size();
        }
        AOT_TEST_CHECK(jit.boundEntries().size() == numberOfBoundEntries);
    }
}

static const StubCatalog& defaultCatalog()
{
    static NeverDestroyed<StubCatalog> catalog(StubOptions { }, StubPolicy { });
    return catalog.get();
}

template<typename Emit>
static StubRecording record(StubKey body, const Emit& emit)
{
    const StubCatalog& catalog = defaultCatalog();
    StubAssembler jit(catalog.options(), catalog);
    return jit.generateForTesting(body, emit);
}

template<typename Accepts>
static StubKey findBody(const Accepts& accepts)
{
    std::optional<StubKey> found;
    for (StubKey body : allStubBodies()) {
        if (body.operandRegister() || entriesOf(body).size() != 1)
            continue;
        StubSignature signature = signatureFor(body);
        if (signature.usesCallConvention || !accepts(body, signature))
            continue;
        found = body;
        break;
    }
    AOT_TEST_CHECK(found.has_value());
    return *found;
}

static std::optional<StubRegister> stubRegisterOf(GPRReg reg)
{
    if (reg == InvalidGPRReg)
        return std::nullopt;
    for (StubRegister candidate : stubRegisters) {
        if (gprOf(candidate) == reg)
            return candidate;
    }
    return std::nullopt;
}

static StubRegister freeRegisterOf(StubKey body)
{
    StubSignature signature = signatureFor(body);
    std::optional<StubRegister> found;
    for (StubRegister candidate : stubRegisters) {
        GPRReg reg = gprOf(candidate);
        if (reg == InvalidGPRReg || namesRegister(signature, reg))
            continue;
        found = candidate;
        break;
    }
    AOT_TEST_CHECK(found.has_value());
    return *found;
}

static bool isPlain(StubKey body, const StubSignature& signature)
{
    return body.callerFrame() == CallerFrame::HasFrame && !signature.immediate;
}

static StubKey findPlainBody() { return findBody(isPlain); }

static StubKey findPlainBodyWithout(Reg reg)
{
    return findBody([&](StubKey candidate, const StubSignature& signature) {
        return isPlain(candidate, signature) && !namesRegister(signature, reg);
    });
}

static std::optional<unsigned> findReadOnlyOperand(const StubSignature& signature)
{
    for (unsigned i = 0; i < signature.numberOfOperands; ++i) {
        Reg operand = signature.operands[i];
        if (!operand.isGPR() || !stubRegisterOf(operand.gpr()))
            continue;
        if (operand == signature.firstResult || operand == signature.secondResult)
            continue;
        if (signature.immediate && operand == Reg(*signature.immediate))
            continue;
        return i;
    }
    return std::nullopt;
}

static ReadOnlyOperand findReadOnlyOperand()
{
    StubKey body = findBody([](StubKey candidate, const StubSignature& signature) {
        return isPlain(candidate, signature) && findReadOnlyOperand(signature);
    });
    StubSignature signature = signatureFor(body);
    unsigned index = findReadOnlyOperand(signature).value();
    GPRReg gpr = signature.operands[index].gpr();
    return { body, index, gpr, stubRegisterOf(gpr).value() };
}

static void checkRecordedScratch(StubKey plain, StubRegister scratch)
{
    GPRReg scratchGPR = gprOf(scratch);

    StubRecording unsaved = record(plain, [&](StubAssembler& jit) { jit.allocateScratch(scratch); });
    AOT_TEST_CHECK(containsRegister(unsaved.clobberedInline, scratchGPR) && unsaved.clobberedByCallees.isEmpty());

    StubRecording preserved = record(plain, [&](StubAssembler& jit) {
        jit.preserving({ scratchGPR }, [&] { jit.allocateScratch(scratch); });
    });
    AOT_TEST_CHECK(!containsRegister(preserved.clobbered(), scratchGPR));

    StubRecording saved = record(plain, [&](StubAssembler& jit) {
        jit.withSavedRegisters(RegisterSet { scratchGPR }, [&] { jit.allocateScratch(scratch); });
    });
    AOT_TEST_CHECK(!containsRegister(saved.clobbered(), scratchGPR));
}

static void checkBorrowedRegister()
{
    StubKey body = findPlainBodyWithout(borrowedFPR);
    StubRegister scratch = freeRegisterOf(body);

    StubRecording allocated = record(body, [](StubAssembler& jit) { jit.allocateScratch(borrowedFPR); });
    AOT_TEST_CHECK(containsRegister(allocated.clobberedInline, borrowedFPR));

    StubRecording borrowed = record(body, [&](StubAssembler& jit) {
        GPRReg saved = jit.allocateScratch(scratch);
        jit.saveFPR(borrowedFPR, saved);
        jit.restoreFPR(borrowedFPR, saved);
    });
    AOT_TEST_CHECK(!containsRegister(borrowed.clobbered(), borrowedFPR));
    AOT_TEST_CHECK(containsRegister(borrowed.clobberedInline, gprOf(scratch)));
}

static void checkRecordedRegisterSet()
{
    StubKey body = findPlainBodyWithout(borrowedFPR);
    GPRReg scratch = gprOf(freeRegisterOf(body));
    RegisterSet registers { scratch };
    registers.add(borrowedFPR, IgnoreVectors);

    StubRecording unsaved = record(body, [&](StubAssembler& jit) { jit.recordClobbers(registers); });
    AOT_TEST_CHECK(unsaved.clobberedInline.subsumes(registers));

    StubRecording preserved = record(body, [&](StubAssembler& jit) {
        jit.preserving({ scratch }, [&] { jit.recordClobbers(registers); });
    });
    AOT_TEST_CHECK(!containsRegister(preserved.clobbered(), scratch) && containsRegister(preserved.clobbered(), borrowedFPR));
}

static void testRecordedStubRegisters()
{
    StubKey plain = findPlainBody();
    StubRegister scratch = freeRegisterOf(plain);
    checkRecordedScratch(plain, scratch);
    checkBorrowedRegister();
    checkRecordedRegisterSet();

    auto emitNothing = [](StubAssembler&) { };
    StubRecording empty = record(plain, emitNothing);
    checkWrittenRegisters(empty.clobberedInline, signatureFor(plain));
    AOT_TEST_CHECK(!containsRegister(empty.clobbered(), gprOf(scratch)));

    StubKey bodyWithImmediate = findBody([](StubKey, const StubSignature& signature) {
        return signature.firstResult.isSet() && signature.immediate;
    });
    checkWrittenRegisters(record(bodyWithImmediate, emitNothing).clobberedInline, signatureFor(bodyWithImmediate));

    ReadOnlyOperand operand = findReadOnlyOperand();
    AOT_TEST_CHECK(!containsRegister(record(operand.body, emitNothing).clobbered(), operand.gpr));
    StubRecording clobberedOperand = record(operand.body, [&](StubAssembler& jit) { jit.clobberOperand(operand.index); });
    AOT_TEST_CHECK(containsRegister(clobberedOperand.clobberedInline, operand.gpr));
}

static void checkScratchAllocation()
{
    StubKey plain = findPlainBody();
    StubRegister scratch = freeRegisterOf(plain);
    record(plain, [&](StubAssembler& jit) {
        AOT_TEST_CHECK(jit.canAllocate(scratch));
        jit.allocateScratch(scratch);
        AOT_TEST_CHECK(!jit.canAllocate(scratch));
    });
}

static void checkOperandAllocation()
{
    ReadOnlyOperand operand = findReadOnlyOperand();
    record(operand.body, [&](StubAssembler& jit) {
        AOT_TEST_CHECK(!jit.canAllocate(operand.stubRegister));
        jit.preserving({ operand.gpr }, [&] { AOT_TEST_CHECK(jit.canAllocate(operand.stubRegister)); });
        GPRReg clobbered = jit.clobberOperand(operand.index);
        AOT_TEST_CHECK(clobbered == operand.gpr);
        AOT_TEST_CHECK(!jit.canAllocate(operand.stubRegister));
        GPRReg reused = jit.reuseScratch(operand.stubRegister);
        AOT_TEST_CHECK(reused == operand.gpr);
    });
}

static void checkAliasedRegisterAllocation()
{
    StubKey body = findPlainBodyWithout(gprOf(StubRegister::A0));
    record(body, [](StubAssembler& jit) {
        AOT_TEST_CHECK(jit.canAllocate(StubRegister::T12));
        jit.allocateScratch(StubRegister::A0);
        AOT_TEST_CHECK(!jit.canAllocate(StubRegister::T12));
        AOT_TEST_CHECK(!jit.canAllocate(StubRegister::A6));
    });
}

static void testAllocatableStubRegisters()
{
    checkScratchAllocation();
    checkOperandAllocation();
    if (isX86_64())
        checkAliasedRegisterAllocation();
}

static void checkRecordedCalls(StubKey plain, StubRegister scratch)
{
    Entry entry = operationFrontEnds()[0].raw;

    StubRecording operationCall = record(plain, [&](StubAssembler& jit) { jit.callOperation(entry, jit.allocateScratch(scratch)); });
    AOT_TEST_CHECK(operationCall.entriesUsed.get(indexOf(entry)));
    AOT_TEST_CHECK(operationCall.operationsCalled.get(indexOf(entry)));
    AOT_TEST_CHECK(!operationCall.hasUnknownEffects && operationCall.leavesStubs);
    AOT_TEST_CHECK(operationCall.clobberedByCallees == RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));

    StubRecording pointerLoad = record(plain, [&](StubAssembler& jit) { jit.loadRuntimePointer(entry, jit.allocateScratch(scratch)); });
    AOT_TEST_CHECK(pointerLoad.entriesUsed.get(indexOf(entry)));
    AOT_TEST_CHECK(pointerLoad.operationsCalled.isEmpty());
    AOT_TEST_CHECK(!pointerLoad.leavesStubs && pointerLoad.clobberedByCallees.isEmpty());

    StubRecording compiledCall = record(plain, [&](StubAssembler& jit) { jit.call(jit.allocateScratch(scratch), JSEntryPtrTag); });
    AOT_TEST_CHECK(compiledCall.hasUnknownEffects && compiledCall.leavesStubs);
    AOT_TEST_CHECK(compiledCall.clobberedByCallees == registersClobberedByCompiledCalls());

    StubRecording hostCall = record(plain, [&](StubAssembler& jit) { jit.call(jit.allocateScratch(scratch), HostFunctionPtrTag); });
    AOT_TEST_CHECK(hostCall.hasUnknownEffects && hostCall.leavesStubs);
    AOT_TEST_CHECK(hostCall.clobberedByCallees == registersClobberedByOperations());
}

static void checkRecordedTailCall()
{
    constexpr GPRReg function = gprOf(StubRegister::Operation);
    Entry entry = operationFrontEnds()[0].raw;
    StubKey body = findBody([](StubKey candidate, const StubSignature& signature) {
        if (!isPlain(candidate, signature) || signature.firstResult == Reg(function) || signature.secondResult == Reg(function))
            return false;
        return namesRegister(signature, function);
    });

    StubRecording tailCall = record(body, [&](StubAssembler& jit) {
        jit.tailCallOperation(entry, OperationCall::FirstArgument::Instance);
    });
    AOT_TEST_CHECK(tailCall.operationsCalled.get(indexOf(entry)));
    AOT_TEST_CHECK(tailCall.leavesStubs && tailCall.clobberedByCallees == registersClobberedByOperations());
}

static void checkRecordedStubReferences(StubKey plain, StubRegister scratch)
{
    StubRecording everyKind = record(plain, [&](StubAssembler& jit) {
        GPRReg reg = jit.allocateScratch(scratch);
        jit.callStub(calledStub);
        jit.jumpToStub(calledStub);
        jit.loadStubAddress(calledStub, reg);
    });
    constexpr std::array kinds { StubReference::Kind::Call, StubReference::Kind::Jump, StubReference::Kind::Address };
    AOT_TEST_CHECK(everyKind.references.size() == kinds.size());
    for (size_t i = 0; i < kinds.size(); ++i) {
        AOT_TEST_CHECK(everyKind.references[i].target == calledStub);
        AOT_TEST_CHECK(everyKind.references[i].kind == kinds[i]);
        AOT_TEST_CHECK(everyKind.references[i].savedAround.isEmpty());
    }

    StubKey bodyWithResult = findBody([](StubKey candidate, const StubSignature& signature) {
        return isPlain(candidate, signature) && signature.result == StubResult::GPR;
    });
    StubRecording returnedAddress = record(bodyWithResult, [](StubAssembler& jit) { jit.returnStubAddress(calledStub); });
    AOT_TEST_CHECK(returnedAddress.references.size() == 1);
    AOT_TEST_CHECK(returnedAddress.references[0].target == calledStub && returnedAddress.references[0].kind == StubReference::Kind::ReturnedAddress);

    StubRecording preservingCall = record(plain, [&](StubAssembler& jit) {
        jit.preserving({ jit.allocateScratch(scratch) }, [&] { jit.callStub(calledStub); });
    });
    AOT_TEST_CHECK(preservingCall.references.size() == 1);
    AOT_TEST_CHECK(containsRegister(preservingCall.references[0].savedAround, gprOf(scratch)));

    StubRecording jumpPreservingNothing = record(plain, [](StubAssembler& jit) {
        size_t sizeBefore = jit.m_assembler.codeSize();
        jit.preserving(std::span<const GPRReg>(), [] { });
        AOT_TEST_CHECK(jit.m_assembler.codeSize() == sizeBefore);
        jit.preserving(std::span<const GPRReg>(), [&] { jit.jumpToStub(calledStub); });
    });
    AOT_TEST_CHECK(jumpPreservingNothing.references.size() == 1);
    AOT_TEST_CHECK(jumpPreservingNothing.references[0].kind == StubReference::Kind::Jump);

    StubRecording labeledCall = record(plain, [](StubAssembler& jit) {
        jit.callStub(calledStub);
        jit.bindReturnLabel(StubFrameKind::EngineCallee);
    });
    AOT_TEST_CHECK(labeledCall.returnLabels.size() == 1);
    AOT_TEST_CHECK(labeledCall.returnLabels[0] == StubFrameKind::EngineCallee);
}

static void testRecordedStubReferences()
{
    StubKey plain = findPlainBody();
    StubRegister scratch = freeRegisterOf(plain);
    checkRecordedCalls(plain, scratch);
    checkRecordedTailCall();
    checkRecordedStubReferences(plain, scratch);
}

static StubKey findBodyWithoutRegisters()
{
    return findBody([](StubKey candidate, const StubSignature& signature) {
        return isPlain(candidate, signature) && !signature.numberOfOperands && signature.result == StubResult::None;
    });
}

static void checkCallInSavingScope(StubKey body, StubRegister scratch, bool hasResult, const RegisterSet& expectedLoss)
{
    Entry entry = operationFrontEnds()[0].raw;
    StubRecording recording = record(body, [&](StubAssembler& jit) {
        jit.preservingCallerSavedRegisters(hasResult, [&](unsigned bytes) {
            AOT_TEST_CHECK(bytes);
            jit.callOperation(entry, jit.allocateScratch(scratch));
        });
    });
    AOT_TEST_CHECK(recording.clobberedByCallees == expectedLoss);
    AOT_TEST_CHECK(recording.leavesStubs && recording.operationsCalled.get(indexOf(entry)));
}

static void checkPreservingCall(StubKey body, StubRegister scratch, bool returnsValue, const RegisterSet& expectedLoss)
{
    for (StubCallee callee : { StubCallee::Operation, StubCallee::HostFunction, StubCallee::CompiledCode }) {
        PtrTag tag = callee == StubCallee::Operation ? OperationPtrTag : callee == StubCallee::HostFunction ? HostFunctionPtrTag : JSEntryPtrTag;
        StubRecording recording = record(body, [&](StubAssembler& jit) {
            jit.callAndReturnPreservingRegisters({ callee, returnsValue, true }, [&] {
                jit.call(jit.allocateScratch(scratch), tag);
            });
        });
        RegisterSet lostIntegerRegisters = recording.clobberedByCallees;
        lostIntegerRegisters.filter(RegisterSet::allGPRs());
        lostIntegerRegisters.exclude(RegisterSet::macroClobberedGPRs());
        AOT_TEST_CHECK(lostIntegerRegisters == expectedLoss);
        // Of the floating-point registers that the callee may write, two are stored and loaded.
        RegisterSet keptFloatingPointRegisters = registersClobberedBy(callee);
        keptFloatingPointRegisters.filter(RegisterSet::allFPRs());
        keptFloatingPointRegisters.exclude(recording.clobberedByCallees);
        AOT_TEST_CHECK(keptFloatingPointRegisters.numberOfSetRegisters() == 2);
        AOT_TEST_CHECK(recording.leavesStubs && recording.hasUnknownEffects == (callee != StubCallee::Operation));
        AOT_TEST_CHECK(!containsRegister(recording.clobbered(), gprOf(scratch)));
    }
}

static void testCallsInSavingScopes()
{
    StubKey body = findBodyWithoutRegisters();
    StubRegister scratch = StubRegister::A1;
    RegisterSet nothing;
    RegisterSet returnedValue { GPRInfo::returnValueGPR };

    checkCallInSavingScope(body, scratch, false, nothing);
    checkCallInSavingScope(body, scratch, true, returnedValue);
    checkPreservingCall(body, scratch, false, nothing);
    checkPreservingCall(body, scratch, true, returnedValue);

    Entry entry = operationFrontEnds()[0].raw;
    StubRecording unsaved = record(body, [&](StubAssembler& jit) { jit.callOperation(entry, jit.allocateScratch(scratch)); });
    AOT_TEST_CHECK(unsaved.clobberedByCallees == registersClobberedByOperations() && unsaved.leavesStubs);

    StubRecording partlySaved = record(body, [&](StubAssembler& jit) {
        jit.preserving({ gprOf(scratch) }, [&] { jit.callOperation(entry, jit.allocateScratch(scratch)); });
    });
    AOT_TEST_CHECK(partlySaved.clobberedByCallees == registersClobberedByOperations().remove(gprOf(scratch)));
}

static void checkLeafCall(StubKey body, Entry leaf, RegisterSet expectedLoss)
{
    expectedLoss.add(leafFunctionGPR, IgnoreVectors);
    StubRecording recording = record(body, [&](StubAssembler& jit) { jit.callLeaf(leaf); });
    AOT_TEST_CHECK(recording.clobberedInline == expectedLoss);
    AOT_TEST_CHECK(recording.clobberedByCallees.isEmpty());
    AOT_TEST_CHECK(!recording.leavesStubs && !recording.hasUnknownEffects);
    AOT_TEST_CHECK(recording.operationsCalled.get(indexOf(leaf)));
}

static void testLeafCalls()
{
    StubKey body = findBodyWithoutRegisters();
    checkLeafCall(body, preserveMostLeaf, registersClobberedByPreserveMostLeaves());
    checkLeafCall(body, ordinaryLeaf, registersClobberedByOperations());

    constexpr GPRReg saved = GPRInfo::returnValueGPR;
    StubRecording preserving = record(body, [&](StubAssembler& jit) {
        jit.preserving({ saved }, [&] { jit.callLeaf(preserveMostLeaf, stackAlignmentBytes()); });
    });
    AOT_TEST_CHECK(containsRegister(registersClobberedByPreserveMostLeaves(), saved) && !containsRegister(preserving.clobbered(), saved));
}

static void testRecordedExits()
{
    constexpr Entry cachingOperation = Entry::operationAOTGetByIdAndCache;
    constexpr Entry finisher = Entry::operationAOTCallGetterOfSlot;
    constexpr RuntimeCounterID reason = RuntimeCounterID::StubCannotCallGetter;
    constexpr GPRReg reasonArgument = operationArgumentGPR(3);

    StubRecording recording = record(findBodyWithoutRegisters(), [&](StubAssembler& jit) {
        jit.exitOnMiss(cachingOperation, { });
        jit.exitOnCannot(finisher, reason, reasonArgument, { });
    });
    AOT_TEST_CHECK(recording.exits.size() == 2);
    AOT_TEST_CHECK(recording.exits[0] == (StubExitRecord { StubExit::Miss, cachingOperation, RuntimeCounterID::None }));
    AOT_TEST_CHECK(recording.exits[1] == (StubExitRecord { StubExit::Cannot, finisher, reason }));
    AOT_TEST_CHECK(recording.operationsCalled.get(indexOf(cachingOperation)) && recording.operationsCalled.get(indexOf(finisher)));
    AOT_TEST_CHECK(containsRegister(recording.clobbered(), reasonArgument));
    AOT_TEST_CHECK(record(findBodyWithoutRegisters(), [](StubAssembler&) { }).exits.isEmpty());

    StubRecording savingRegisters = record(findBodyWithoutRegisters(), [&](StubAssembler& jit) {
        jit.exitOnCannot(finisher, reason, reasonArgument, { .saves = OperationCall::Saves::CallerSavedRegisters });
    });
    AOT_TEST_CHECK(savingRegisters.exits.size() == 1 && savingRegisters.exits[0] == recording.exits[1]);
    AOT_TEST_CHECK(!containsRegister(savingRegisters.clobbered(), reasonArgument));
}

static void testRecordedCodeSymbols()
{
    constexpr CodeSymbol narrow { SymbolSpace::RegExp, 3, 0 };
    constexpr CodeSymbol wide { SymbolSpace::RegExp, 3, 1 };

    StubRecording recording = record(findBodyWithoutRegisters(), [&](StubAssembler& jit) {
        jit.callCode(narrow);
        jit.callCode(narrow);
        jit.callCode(wide);
        AOT_TEST_CHECK(jit.relocations().numberOfRelocations() == 3);
    });
    AOT_TEST_CHECK(recording.symbols.size() == 2 && recording.symbols[0] == narrow && recording.symbols[1] == wide);
    AOT_TEST_CHECK(recording.clobberedByCallees == registersClobberedByOperations());
    AOT_TEST_CHECK(!recording.leavesStubs && !recording.hasUnknownEffects);

    StubRecording saved = record(findBodyWithoutRegisters(), [&](StubAssembler& jit) {
        jit.preservingCallerSavedRegisters(false, [&](unsigned) { jit.callCode(narrow); });
    });
    AOT_TEST_CHECK(saved.symbols.size() == 1 && saved.clobberedByCallees.isEmpty());
}

#if OS(UNIX)

template<typename Legal, typename Illegal>
static void checkAssertion(StubKey body, const Legal& legal, const Illegal& illegal)
{
    AOT_TEST_CHECK(runInChild([&] { record(body, legal); }) == ChildOutcome::Finished);
    AOT_TEST_CHECK(runInChild([&] { record(body, illegal); }) == ChildOutcome::Crashed);
}

static void checkRegisterAssertions(StubKey plain, StubRegister scratch)
{
    ReadOnlyOperand operand = findReadOnlyOperand();
    checkAssertion(operand.body, [&](StubAssembler& jit) {
        jit.clobberOperand(operand.index);
        jit.reuseScratch(operand.stubRegister);
    }, [&](StubAssembler& jit) {
        jit.allocateScratch(operand.stubRegister);
    });

    checkAssertion(plain, [&](StubAssembler& jit) {
        jit.allocateScratch(scratch);
        jit.reuseScratch(scratch);
    }, [&](StubAssembler& jit) {
        jit.allocateScratch(scratch);
        jit.allocateScratch(scratch);
    });
}

static void checkBorrowedRegisterAssertion()
{
    StubKey body = findPlainBodyWithout(borrowedFPR);
    StubRegister scratch = freeRegisterOf(body);
    checkAssertion(body, [&](StubAssembler& jit) {
        GPRReg saved = jit.allocateScratch(scratch);
        jit.saveFPR(borrowedFPR, saved);
        jit.restoreFPR(borrowedFPR, saved);
        jit.returnToCaller();
    }, [&](StubAssembler& jit) {
        jit.saveFPR(borrowedFPR, jit.allocateScratch(scratch));
        jit.returnToCaller();
    });
}

static void callThroughHandlerTable(StubAssembler& jit, GPRReg index)
{
    jit.callThroughHandlerTable(CCallHelpers::BaseIndex(instanceGPR, index, CCallHelpers::TimesEight), std::array { calledStub });
}

static void checkHandlerTableAssertions()
{
    ReadOnlyOperand operand = findReadOnlyOperand();
    checkAssertion(operand.body, [&](StubAssembler& jit) {
        callThroughHandlerTable(jit, jit.allocateScratch(freeRegisterOf(operand.body)));
    }, [&](StubAssembler& jit) {
        callThroughHandlerTable(jit, operand.gpr);
    });
}

static void checkEntryAssertions()
{
    auto bindSecondEntry = [](StubAssembler& jit) { jit.bindEntry(secondEntry); };

    checkAssertion(bodyWithTwoEntries, bindSecondEntry, [](StubAssembler& jit) {
        jit.bindEntry(secondEntry);
        jit.bindEntry(calledStub);
    });
    checkAssertion(bodyWithTwoEntries, bindSecondEntry, [](StubAssembler& jit) {
        jit.bindEntry(secondEntry);
        jit.bindEntry(secondEntry);
    });
    checkAssertion(bodyWithTwoEntries, bindSecondEntry, [](StubAssembler&) { });
}

static void checkReferenceAssertions(StubKey plain, StubRegister scratch)
{
    checkAssertion(plain, [](StubAssembler& jit) {
        jit.callStub(calledStub);
    }, [](StubAssembler& jit) {
        jit.callStub(nonexistentStub);
    });

    checkAssertion(plain, [](StubAssembler& jit) {
        jit.callStub(calledStub);
        jit.bindReturnLabel(StubFrameKind::EngineCallee);
    }, [](StubAssembler& jit) {
        jit.callStub(calledStub);
        jit.bindReturnLabel(StubFrameKind::Plain);
    });

    checkAssertion(plain, [&](StubAssembler& jit) {
        jit.preserving({ gprOf(scratch) }, [&] { jit.callStub(calledStub); });
    }, [&](StubAssembler& jit) {
        jit.preserving({ gprOf(scratch) }, [&] { jit.jumpToStub(calledStub); });
    });
}

static void checkFaultLinkAssertion()
{
    auto clobberLink = [](StubAssembler& jit) { jit.clobberRegister(faultCallerLinkGPR); };
    AOT_TEST_CHECK(runInChild([&] { record(bodyEnteredAtFaults, clobberLink); }) == ChildOutcome::Finished);
    AOT_TEST_CHECK(runInChild([&] { record(bodyEnteredAtFaults.withCallerFrame(CallerFrame::Frameless), clobberLink); }) == ChildOutcome::Crashed);
}

static void checkLeafAssertion(StubKey plain)
{
    checkAssertion(plain, [](StubAssembler& jit) {
        jit.callLeaf(ordinaryLeaf);
    }, [](StubAssembler& jit) {
        jit.callLeaf(operationFrontEnds()[0].operation);
    });
}

static void checkBodyAssertion()
{
    auto generate = [](StubKey key) {
        const StubCatalog& catalog = defaultCatalog();
        StubAssembler jit(catalog.options(), catalog);
        jit.generate(key);
    };
    AOT_TEST_CHECK(runInChild([&] { generate(bodyWithTwoEntries); }) == ChildOutcome::Finished);
    AOT_TEST_CHECK(runInChild([&] { generate(secondEntry); }) == ChildOutcome::Crashed);
}

static void testStubAssemblerAssertions()
{
    WTF::fastDisableScavenger();
    defaultCatalog();

    StubKey plain = findPlainBody();
    StubRegister scratch = freeRegisterOf(plain);
    checkRegisterAssertions(plain, scratch);
    checkBorrowedRegisterAssertion();
    checkHandlerTableAssertions();
    checkEntryAssertions();
    checkReferenceAssertions(plain, scratch);
    if (isARM64())
        checkFaultLinkAssertion();
    checkLeafAssertion(plain);
    checkBodyAssertion();
}

#endif // OS(UNIX)

std::span<const NativeTest> stubAssemblerTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/every-body-generates"_s, testStubBodyGeneration },
        { "stubs/recorded-registers"_s, testRecordedStubRegisters },
        { "stubs/allocatable-registers"_s, testAllocatableStubRegisters },
        { "stubs/recorded-references"_s, testRecordedStubReferences },
        { "stubs/calls-in-saving-scopes"_s, testCallsInSavingScopes },
        { "stubs/leaf-calls"_s, testLeafCalls },
        { "stubs/recorded-exits"_s, testRecordedExits },
        { "stubs/recorded-code-symbols"_s, testRecordedCodeSymbols },
#if OS(UNIX)
        { "stubs/assembler-assertions"_s, testStubAssemblerAssertions },
#endif
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

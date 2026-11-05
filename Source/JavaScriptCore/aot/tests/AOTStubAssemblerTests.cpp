/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTRelocation.h"
#include "AOTStubRegisters.h"
#include "AOTStubTestSupport.h"
#include "AOTVector.h"
#include "CPU.h"
#include "StackAlignment.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

namespace {

struct ReadOnlyStubOperand {
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
static constexpr Entry calledOperation = Entry::operationAOTValueAdd;
static constexpr Entry preserveMostLeaf = Entry::operationAOTStringImplsAreEqual;
static constexpr Entry ordinaryLeaf = Entry::operationAOTToInt32;

static_assert(exists(calledStub) && !neverReturns(calledStub) && !exists(nonexistentStub));
static_assert(bodyOf(bodyWithTwoEntries) == bodyWithTwoEntries && bodyOf(secondEntry) == bodyWithTwoEntries && bodyOf(calledStub) != bodyWithTwoEntries);
static_assert(exists(bodyEnteredAtFaults) && exists(bodyEnteredAtFaults.withCallerFrame(CallerFrame::Frameless)));
static_assert(!isLeaf(RuntimeOperation::ValueAdd) && effectsOf(calledOperation).mayThrow);
static_assert(isLeaf(RuntimeOperation::StringImplsAreEqual) && preservesMostRegisters(RuntimeOperation::StringImplsAreEqual));
static_assert(isLeaf(RuntimeOperation::ToInt32) && !preservesMostRegisters(RuntimeOperation::ToInt32));

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
    if (!recording.leavesStubs)
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
    checkRecordedClobbers(recording);
}

static void testStubBodyGeneration()
{
    Vector<StubKey> bodies = allStubBodies();
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options);
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
    static NeverDestroyed<StubCatalog> catalog(StubOptions { });
    return catalog.get();
}

template<typename Emit>
static StubRecording recordStubBody(StubKey body, const Emit& emit)
{
    const StubCatalog& catalog = defaultCatalog();
    StubAssembler jit(catalog.options(), catalog);
    return jit.generateForTesting(body, emit);
}

template<typename Accepts>
static std::optional<StubKey> tryFindStubBody(const Accepts& accepts)
{
    for (StubKey body : allStubBodies()) {
        if (entriesOf(body).size() != 1)
            continue;
        StubSignature signature = signatureFor(body);
        if (!signature.usesCallConvention && accepts(body, signature))
            return body;
    }
    return std::nullopt;
}

template<typename Accepts>
static StubKey findStubBody(const Accepts& accepts)
{
    std::optional<StubKey> found = tryFindStubBody(accepts);
    AOT_TEST_CHECK(found.has_value());
    return *found;
}

static std::optional<StubRegister> stubRegisterOf(GPRReg reg)
{
    if (reg == InvalidGPRReg)
        return std::nullopt;
    for (unsigned index = 0; index < numberOfStubRegisters; ++index) {
        StubRegister candidate = static_cast<StubRegister>(index);
        if (gprOf(candidate) == reg)
            return candidate;
    }
    return std::nullopt;
}

static StubRegister freeStubRegisterOf(StubKey body)
{
    StubSignature signature = signatureFor(body);
    std::optional<StubRegister> found;
    for (unsigned index = 0; index < numberOfStubRegisters; ++index) {
        StubRegister candidate = static_cast<StubRegister>(index);
        GPRReg reg = gprOf(candidate);
        if (reg == InvalidGPRReg || namesRegister(signature, reg))
            continue;
        found = candidate;
        break;
    }
    AOT_TEST_CHECK(found.has_value());
    return *found;
}

static bool isPlainStubBody(StubKey body, const StubSignature& signature)
{
    return body.callerFrame() == CallerFrame::HasFrame && !signature.immediate;
}

static StubKey findPlainStubBody() { return findStubBody(isPlainStubBody); }

static StubKey findPlainStubBodyWithout(Reg reg)
{
    return findStubBody([&](StubKey candidate, const StubSignature& signature) {
        return isPlainStubBody(candidate, signature) && !namesRegister(signature, reg);
    });
}

static StubKey findStubBodyWithoutRegisters()
{
    return findStubBody([](StubKey candidate, const StubSignature& signature) {
        return isPlainStubBody(candidate, signature) && !signature.numberOfOperands && signature.result == StubResult::None;
    });
}

static std::optional<unsigned> indexOfReadOnlyOperand(const StubSignature& signature)
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

static ReadOnlyStubOperand findReadOnlyStubOperand()
{
    StubKey body = findStubBody([](StubKey candidate, const StubSignature& signature) {
        return isPlainStubBody(candidate, signature) && indexOfReadOnlyOperand(signature);
    });
    StubSignature signature = signatureFor(body);
    unsigned index = indexOfReadOnlyOperand(signature).value();
    GPRReg gpr = signature.operands[index].gpr();
    return { body, index, gpr, stubRegisterOf(gpr).value() };
}

static void checkRecordedScratch(StubKey plain, StubRegister scratch)
{
    GPRReg scratchGPR = gprOf(scratch);

    StubRecording unsaved = recordStubBody(plain, [&](StubAssembler& jit) { jit.allocateScratch(scratch); });
    AOT_TEST_CHECK(containsRegister(unsaved.clobberedInline, scratchGPR) && unsaved.clobberedByCallees.isEmpty());

    StubRecording preserved = recordStubBody(plain, [&](StubAssembler& jit) {
        jit.preserving({ scratchGPR }, [&] { jit.allocateScratch(scratch); });
    });
    AOT_TEST_CHECK(!containsRegister(preserved.clobbered(), scratchGPR));

    StubRecording saved = recordStubBody(plain, [&](StubAssembler& jit) {
        jit.withSavedRegisters(RegisterSet { scratchGPR }, [&] { jit.allocateScratch(scratch); });
    });
    AOT_TEST_CHECK(!containsRegister(saved.clobbered(), scratchGPR));
}

static void checkBorrowedRegister()
{
    StubKey body = findPlainStubBodyWithout(borrowedFPR);
    StubRegister scratch = freeStubRegisterOf(body);

    StubRecording allocated = recordStubBody(body, [](StubAssembler& jit) { jit.allocateScratch(borrowedFPR); });
    AOT_TEST_CHECK(containsRegister(allocated.clobberedInline, borrowedFPR));

    StubRecording borrowed = recordStubBody(body, [&](StubAssembler& jit) {
        GPRReg saved = jit.allocateScratch(scratch);
        jit.saveFPR(borrowedFPR, saved);
        jit.restoreFPR(borrowedFPR, saved);
    });
    AOT_TEST_CHECK(!containsRegister(borrowed.clobbered(), borrowedFPR));
    AOT_TEST_CHECK(containsRegister(borrowed.clobberedInline, gprOf(scratch)));
}

static void checkRecordedRegisterSet()
{
    StubKey body = findPlainStubBodyWithout(borrowedFPR);
    GPRReg scratch = gprOf(freeStubRegisterOf(body));
    RegisterSet registers { scratch };
    registers.add(borrowedFPR, IgnoreVectors);

    StubRecording unsaved = recordStubBody(body, [&](StubAssembler& jit) { jit.recordClobbers(registers); });
    AOT_TEST_CHECK(unsaved.clobberedInline.subsumes(registers));

    StubRecording preserved = recordStubBody(body, [&](StubAssembler& jit) {
        jit.preserving({ scratch }, [&] { jit.recordClobbers(registers); });
    });
    AOT_TEST_CHECK(!containsRegister(preserved.clobbered(), scratch) && containsRegister(preserved.clobbered(), borrowedFPR));
}

static void testRecordedStubRegisters()
{
    StubKey plain = findPlainStubBody();
    StubRegister scratch = freeStubRegisterOf(plain);
    checkRecordedScratch(plain, scratch);
    checkBorrowedRegister();
    checkRecordedRegisterSet();

    auto emitNothing = [](StubAssembler&) { };
    StubRecording empty = recordStubBody(plain, emitNothing);
    checkWrittenRegisters(empty.clobberedInline, signatureFor(plain));
    AOT_TEST_CHECK(!containsRegister(empty.clobbered(), gprOf(scratch)));

    StubKey bodyWithImmediate = findStubBody([](StubKey, const StubSignature& signature) {
        return signature.firstResult.isSet() && signature.immediate;
    });
    checkWrittenRegisters(recordStubBody(bodyWithImmediate, emitNothing).clobberedInline, signatureFor(bodyWithImmediate));

    ReadOnlyStubOperand operand = findReadOnlyStubOperand();
    AOT_TEST_CHECK(!containsRegister(recordStubBody(operand.body, emitNothing).clobbered(), operand.gpr));
    StubRecording clobberedOperand = recordStubBody(operand.body, [&](StubAssembler& jit) { jit.clobberOperand(operand.index); });
    AOT_TEST_CHECK(containsRegister(clobberedOperand.clobberedInline, operand.gpr));
}

static void checkScratchAllocation()
{
    StubKey plain = findPlainStubBody();
    StubRegister scratch = freeStubRegisterOf(plain);
    recordStubBody(plain, [&](StubAssembler& jit) {
        AOT_TEST_CHECK(jit.canAllocate(scratch));
        jit.allocateScratch(scratch);
        AOT_TEST_CHECK(!jit.canAllocate(scratch));
    });
}

static void checkOperandAllocation()
{
    ReadOnlyStubOperand operand = findReadOnlyStubOperand();
    recordStubBody(operand.body, [&](StubAssembler& jit) {
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
    StubKey body = findPlainStubBodyWithout(gprOf(StubRegister::A0));
    recordStubBody(body, [](StubAssembler& jit) {
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
    StubRecording operationCall = recordStubBody(plain, [&](StubAssembler& jit) { jit.callOperation(calledOperation, jit.allocateScratch(scratch)); });
    AOT_TEST_CHECK(operationCall.entriesUsed.get(indexOf(calledOperation)));
    AOT_TEST_CHECK(operationCall.operationsCalled.get(indexOf(calledOperation)));
    AOT_TEST_CHECK(!operationCall.hasUnknownEffects && operationCall.leavesStubs);
    AOT_TEST_CHECK(operationCall.clobberedByCallees == RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));

    StubRecording pointerLoad = recordStubBody(plain, [&](StubAssembler& jit) { jit.loadRuntimePointer(calledOperation, jit.allocateScratch(scratch)); });
    AOT_TEST_CHECK(pointerLoad.entriesUsed.get(indexOf(calledOperation)));
    AOT_TEST_CHECK(pointerLoad.operationsCalled.isEmpty());
    AOT_TEST_CHECK(!pointerLoad.leavesStubs && pointerLoad.clobberedByCallees.isEmpty());

    StubRecording compiledCall = recordStubBody(plain, [&](StubAssembler& jit) { jit.call(jit.allocateScratch(scratch), JSEntryPtrTag); });
    AOT_TEST_CHECK(compiledCall.hasUnknownEffects && compiledCall.leavesStubs);
    AOT_TEST_CHECK(compiledCall.clobberedByCallees == registersClobberedByCompiledCalls());

    StubRecording hostCall = recordStubBody(plain, [&](StubAssembler& jit) { jit.call(jit.allocateScratch(scratch), HostFunctionPtrTag); });
    AOT_TEST_CHECK(hostCall.hasUnknownEffects && hostCall.leavesStubs);
    AOT_TEST_CHECK(hostCall.clobberedByCallees == registersClobberedByOperations());
}

static void checkRecordedTailCall()
{
    constexpr GPRReg function = gprOf(StubRegister::Operation);
    StubKey body = tryFindStubBody([](StubKey candidate, const StubSignature& signature) {
        if (!isPlainStubBody(candidate, signature) || signature.firstResult == Reg(function) || signature.secondResult == Reg(function))
            return false;
        return namesRegister(signature, function);
    }).value_or(findPlainStubBody());

    StubRecording tailCall = recordStubBody(body, [&](StubAssembler& jit) {
        jit.tailCallOperation(calledOperation, OperationCall::FirstArgument::Instance);
    });
    AOT_TEST_CHECK(tailCall.operationsCalled.get(indexOf(calledOperation)));
    AOT_TEST_CHECK(tailCall.leavesStubs && tailCall.clobberedByCallees == registersClobberedByOperations());
}

static void checkRecordedStubReferences(StubKey plain, StubRegister scratch)
{
    StubRecording everyKind = recordStubBody(plain, [&](StubAssembler& jit) {
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

    StubKey bodyWithResult = findStubBody([](StubKey candidate, const StubSignature& signature) {
        return isPlainStubBody(candidate, signature) && signature.result == StubResult::GPR;
    });
    StubRecording returnedAddress = recordStubBody(bodyWithResult, [](StubAssembler& jit) { jit.returnStubAddress(calledStub); });
    AOT_TEST_CHECK(returnedAddress.references.size() == 1);
    AOT_TEST_CHECK(returnedAddress.references[0].target == calledStub && returnedAddress.references[0].kind == StubReference::Kind::ReturnedAddress);

    StubRecording preservingCall = recordStubBody(plain, [&](StubAssembler& jit) {
        jit.preserving({ jit.allocateScratch(scratch) }, [&] { jit.callStub(calledStub); });
    });
    AOT_TEST_CHECK(preservingCall.references.size() == 1);
    AOT_TEST_CHECK(containsRegister(preservingCall.references[0].savedAround, gprOf(scratch)));

    StubRecording jumpPreservingNothing = recordStubBody(plain, [](StubAssembler& jit) {
        size_t sizeBefore = jit.m_assembler.codeSize();
        jit.preserving(std::span<const GPRReg>(), [] { });
        AOT_TEST_CHECK(jit.m_assembler.codeSize() == sizeBefore);
        jit.preserving(std::span<const GPRReg>(), [&] { jit.jumpToStub(calledStub); });
    });
    AOT_TEST_CHECK(jumpPreservingNothing.references.size() == 1);
    AOT_TEST_CHECK(jumpPreservingNothing.references[0].kind == StubReference::Kind::Jump);

    StubRecording labeledCall = recordStubBody(plain, [](StubAssembler& jit) {
        jit.callStub(calledStub);
        jit.bindReturnLabel(StubFrameKind::EngineCallee);
    });
    AOT_TEST_CHECK(labeledCall.returnLabels.size() == 1);
    AOT_TEST_CHECK(labeledCall.returnLabels[0] == StubFrameKind::EngineCallee);
}

static void testRecordedStubReferences()
{
    StubKey plain = findPlainStubBody();
    StubRegister scratch = freeStubRegisterOf(plain);
    checkRecordedCalls(plain, scratch);
    checkRecordedTailCall();
    checkRecordedStubReferences(plain, scratch);
}

static void checkCallInSavingScope(StubKey body, StubRegister scratch, bool hasResult, const RegisterSet& expectedLoss)
{
    StubRecording recording = recordStubBody(body, [&](StubAssembler& jit) {
        jit.preservingCallerSavedRegisters(hasResult, [&](unsigned bytes) {
            AOT_TEST_CHECK(bytes);
            jit.callOperation(calledOperation, jit.allocateScratch(scratch));
        });
    });
    AOT_TEST_CHECK(recording.clobberedByCallees == expectedLoss);
    AOT_TEST_CHECK(recording.leavesStubs && recording.operationsCalled.get(indexOf(calledOperation)));
}

static void checkPreservingCall(StubKey body, StubRegister scratch, bool returnsValue, const RegisterSet& expectedLoss)
{
    for (StubCallee callee : { StubCallee::Operation, StubCallee::HostFunction, StubCallee::CompiledCode }) {
        PtrTag tag = callee == StubCallee::Operation ? OperationPtrTag : callee == StubCallee::HostFunction ? HostFunctionPtrTag : JSEntryPtrTag;
        StubRecording recording = recordStubBody(body, [&](StubAssembler& jit) {
            jit.callAndReturnPreservingRegisters({ callee, returnsValue, true }, [&] {
                jit.call(jit.allocateScratch(scratch), tag);
            });
        });
        RegisterSet lostIntegerRegisters = recording.clobberedByCallees;
        lostIntegerRegisters.filter(RegisterSet::allGPRs());
        lostIntegerRegisters.exclude(RegisterSet::macroClobberedGPRs());
        AOT_TEST_CHECK(lostIntegerRegisters == expectedLoss);
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
    StubKey body = findStubBodyWithoutRegisters();
    StubRegister scratch = StubRegister::A1;
    RegisterSet nothing;
    RegisterSet returnedValue { GPRInfo::returnValueGPR };

    checkCallInSavingScope(body, scratch, false, nothing);
    checkCallInSavingScope(body, scratch, true, returnedValue);
    checkPreservingCall(body, scratch, false, nothing);
    checkPreservingCall(body, scratch, true, returnedValue);

    StubRecording unsaved = recordStubBody(body, [&](StubAssembler& jit) { jit.callOperation(calledOperation, jit.allocateScratch(scratch)); });
    AOT_TEST_CHECK(unsaved.clobberedByCallees == registersClobberedByOperations() && unsaved.leavesStubs);

    StubRecording partlySaved = recordStubBody(body, [&](StubAssembler& jit) {
        jit.preserving({ gprOf(scratch) }, [&] { jit.callOperation(calledOperation, jit.allocateScratch(scratch)); });
    });
    AOT_TEST_CHECK(partlySaved.clobberedByCallees == registersClobberedByOperations().remove(gprOf(scratch)));
}

static void checkLeafCall(StubKey body, Entry leaf, RegisterSet expectedLoss)
{
    expectedLoss.add(leafFunctionGPR, IgnoreVectors);
    StubRecording recording = recordStubBody(body, [&](StubAssembler& jit) { jit.callLeaf(leaf); });
    AOT_TEST_CHECK(recording.clobberedInline == expectedLoss);
    AOT_TEST_CHECK(recording.clobberedByCallees.isEmpty());
    AOT_TEST_CHECK(!recording.leavesStubs && !recording.hasUnknownEffects);
    AOT_TEST_CHECK(recording.operationsCalled.get(indexOf(leaf)));
}

static void testLeafCalls()
{
    StubKey body = findStubBodyWithoutRegisters();
    checkLeafCall(body, preserveMostLeaf, registersClobberedByPreserveMostLeaves());
    checkLeafCall(body, ordinaryLeaf, registersClobberedByOperations());

    constexpr GPRReg saved = GPRInfo::returnValueGPR;
    StubRecording preserving = recordStubBody(body, [&](StubAssembler& jit) {
        jit.preserving({ saved }, [&] { jit.callLeaf(preserveMostLeaf, stackAlignmentBytes()); });
    });
    AOT_TEST_CHECK(containsRegister(registersClobberedByPreserveMostLeaves(), saved) && !containsRegister(preserving.clobbered(), saved));
}

static constexpr StubKey bodyForEveryCaller { StubFamily::AllocateCell };
static constexpr StubKey stubMakingSameResult = bodyForEveryCaller.withForm(1);
static constexpr StubKey stubMakingNoResult { StubFamily::RememberElement };

static_assert(exists(bodyForEveryCaller.withCallerFrame(CallerFrame::KeepsNoFPRs)));
static_assert(exists(stubMakingSameResult.withCallerFrame(CallerFrame::KeepsNoFPRs)));
static_assert(exists(stubMakingNoResult.withCallerFrame(CallerFrame::KeepsNoFPRs)));
static_assert(!exists(StubKey(StubFamily::BuiltinCell).withCallerFrame(CallerFrame::KeepsNoFPRs)) && !exists(StubKey(StubFamily::Equal).withCallerFrame(CallerFrame::KeepsNoFPRs)) && !exists(bodyWithTwoEntries.withCallerFrame(CallerFrame::KeepsNoFPRs)));

static RegisterSet registersLostToOperationFor(CallerFrame callerFrame, SavedRegistersAreRead savedRegistersAreRead)
{
    return recordStubBody(bodyForEveryCaller.withCallerFrame(callerFrame), [&](StubAssembler& jit) {
        jit.callOperationAndReturn(calledOperation, { .saves = OperationCall::Saves::CallerSavedRegisters }, [] { }, savedRegistersAreRead);
    }).clobberedByCallees;
}

static StubKey targetOfJumpFor(CallerFrame callerFrame, StubKey target)
{
    StubRecording recording = recordStubBody(bodyForEveryCaller.withCallerFrame(callerFrame), [&](StubAssembler& jit) { jit.jumpToStub(target); });
    AOT_TEST_CHECK(recording.references.size() == 1 && recording.references[0].kind == StubReference::Kind::Jump);
    return recording.references[0].target;
}

static void testCallersThatKeepFewerRegisters()
{
    AOT_TEST_CHECK(signatureFor(bodyForEveryCaller).firstResult == Reg(GPRInfo::returnValueGPR) && signatureFor(stubMakingSameResult).firstResult == Reg(GPRInfo::returnValueGPR) && !signatureFor(stubMakingNoResult).firstResult.isSet());

    RegisterSet lostToCallerWithFrame = registersLostToOperationFor(CallerFrame::HasFrame, SavedRegistersAreRead::ByCallerAlone);
    AOT_TEST_CHECK(!lostToCallerWithFrame.numberOfSetFPRs());
    RegisterSet lostToCallerWithoutFPRs = registersLostToOperationFor(CallerFrame::KeepsNoFPRs, SavedRegistersAreRead::ByCallerAlone);
    AOT_TEST_CHECK(RegisterSet(lostToCallerWithoutFPRs).filter(RegisterSet::allGPRs()) == lostToCallerWithFrame);
    AOT_TEST_CHECK(RegisterSet(lostToCallerWithoutFPRs).filter(RegisterSet::allFPRs()) == registersClobberedByOperations().filter(RegisterSet::allFPRs()));

    AOT_TEST_CHECK(targetOfJumpFor(CallerFrame::HasFrame, stubMakingSameResult) == stubMakingSameResult);
    AOT_TEST_CHECK(registersLostToOperationFor(CallerFrame::KeepsNoFPRs, SavedRegistersAreRead::BeforeReturnToo) == lostToCallerWithFrame);
    AOT_TEST_CHECK(targetOfJumpFor(CallerFrame::KeepsNoFPRs, stubMakingSameResult) == stubMakingSameResult.withCallerFrame(CallerFrame::KeepsNoFPRs));
    AOT_TEST_CHECK(targetOfJumpFor(CallerFrame::KeepsNoFPRs, stubMakingNoResult) == stubMakingNoResult.withCallerFrame(CallerFrame::KeepsNoFPRs));
}

#if OS(UNIX)

template<typename Legal, typename Illegal>
static void checkAssemblerAssertion(StubKey body, const Legal& legal, const Illegal& illegal)
{
    AOT_TEST_CHECK(runInChild([&] { recordStubBody(body, legal); }) == ChildOutcome::Finished);
    AOT_TEST_CHECK(runInChild([&] { recordStubBody(body, illegal); }) == ChildOutcome::Crashed);
}

static void checkRegisterAssertions(StubKey plain, StubRegister scratch)
{
    ReadOnlyStubOperand operand = findReadOnlyStubOperand();
    checkAssemblerAssertion(operand.body, [&](StubAssembler& jit) {
        jit.clobberOperand(operand.index);
        jit.reuseScratch(operand.stubRegister);
    }, [&](StubAssembler& jit) {
        jit.allocateScratch(operand.stubRegister);
    });

    checkAssemblerAssertion(plain, [&](StubAssembler& jit) {
        jit.allocateScratch(scratch);
        jit.reuseScratch(scratch);
    }, [&](StubAssembler& jit) {
        jit.allocateScratch(scratch);
        jit.allocateScratch(scratch);
    });
}

static void checkBorrowedRegisterAssertion()
{
    StubKey body = findPlainStubBodyWithout(borrowedFPR);
    StubRegister scratch = freeStubRegisterOf(body);
    checkAssemblerAssertion(body, [&](StubAssembler& jit) {
        GPRReg saved = jit.allocateScratch(scratch);
        jit.saveFPR(borrowedFPR, saved);
        jit.restoreFPR(borrowedFPR, saved);
        jit.returnToCaller();
    }, [&](StubAssembler& jit) {
        jit.saveFPR(borrowedFPR, jit.allocateScratch(scratch));
        jit.returnToCaller();
    });
}

static void checkEntryAssertions()
{
    auto bindSecondEntry = [](StubAssembler& jit) { jit.bindEntry(secondEntry); };

    checkAssemblerAssertion(bodyWithTwoEntries, bindSecondEntry, [](StubAssembler& jit) {
        jit.bindEntry(secondEntry);
        jit.bindEntry(calledStub);
    });
    checkAssemblerAssertion(bodyWithTwoEntries, bindSecondEntry, [](StubAssembler& jit) {
        jit.bindEntry(secondEntry);
        jit.bindEntry(secondEntry);
    });
    checkAssemblerAssertion(bodyWithTwoEntries, bindSecondEntry, [](StubAssembler&) { });
}

static void checkReferenceAssertions(StubKey plain, StubRegister scratch)
{
    checkAssemblerAssertion(plain, [](StubAssembler& jit) {
        jit.callStub(calledStub);
    }, [](StubAssembler& jit) {
        jit.callStub(nonexistentStub);
    });

    checkAssemblerAssertion(plain, [](StubAssembler& jit) {
        jit.callStub(calledStub);
        jit.bindReturnLabel(StubFrameKind::EngineCallee);
    }, [](StubAssembler& jit) {
        jit.callStub(calledStub);
        jit.bindReturnLabel(StubFrameKind::Plain);
    });

    checkAssemblerAssertion(plain, [&](StubAssembler& jit) {
        jit.preserving({ gprOf(scratch) }, [&] { jit.callStub(calledStub); });
    }, [&](StubAssembler& jit) {
        jit.preserving({ gprOf(scratch) }, [&] { jit.jumpToStub(calledStub); });
    });
}

static void checkFaultLinkAssertion()
{
    auto clobberLink = [](StubAssembler& jit) { jit.clobberRegister(faultCallerLinkGPR); };
    AOT_TEST_CHECK(runInChild([&] { recordStubBody(bodyEnteredAtFaults, clobberLink); }) == ChildOutcome::Finished);
    AOT_TEST_CHECK(runInChild([&] { recordStubBody(bodyEnteredAtFaults.withCallerFrame(CallerFrame::Frameless), clobberLink); }) == ChildOutcome::Crashed);
}

static void checkLeafAssertion(StubKey plain)
{
    checkAssemblerAssertion(plain, [](StubAssembler& jit) {
        jit.callLeaf(ordinaryLeaf);
    }, [](StubAssembler& jit) {
        jit.callLeaf(calledOperation);
    });
}

static void checkScratchRegisterAssertions(StubKey plain)
{
    constexpr int32_t largest = std::numeric_limits<int32_t>::max();
    checkAssemblerAssertion(plain, [](StubAssembler& jit) {
        const OwnedScratchRegister length(jit);
        jit.load64(CCallHelpers::Address(instanceGPR), length);
        jit.branch64(CCallHelpers::Above, length, CCallHelpers::TrustedImm32(largest)).link(&jit);
    }, [](StubAssembler& jit) {
        const OwnedScratchRegister length(jit);
        jit.load64(CCallHelpers::Address(instanceGPR), length);
        jit.branch64(CCallHelpers::Above, length, CCallHelpers::TrustedImm64(largest)).link(&jit);
    });
    checkAssemblerAssertion(plain, [](StubAssembler& jit) {
        {
            const OwnedScratchRegister first(jit);
        }
        const OwnedScratchRegister second(jit);
    }, [](StubAssembler& jit) {
        const OwnedScratchRegister first(jit);
        const OwnedScratchRegister second(jit);
    });
    checkAssemblerAssertion(plain, [](StubAssembler& jit) {
        const OwnedScratchRegister handedOn(jit, instanceGPR);
        jit.branch64(CCallHelpers::Above, instanceGPR, CCallHelpers::TrustedImm64(largest)).link(&jit);
    }, [](StubAssembler& jit) {
        const OwnedScratchRegister handedOn(jit, OwnedScratchRegister::registerToHandOn);
        jit.branch64(CCallHelpers::Above, instanceGPR, CCallHelpers::TrustedImm64(largest)).link(&jit);
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

    StubKey plain = findPlainStubBody();
    StubRegister scratch = freeStubRegisterOf(plain);
    checkRegisterAssertions(plain, scratch);
    checkBorrowedRegisterAssertion();
    checkEntryAssertions();
    checkReferenceAssertions(plain, scratch);
    if (isARM64())
        checkFaultLinkAssertion();
    checkLeafAssertion(plain);
    if (isX86_64())
        checkScratchRegisterAssertions(plain);
    checkBodyAssertion();
}

#endif // OS(UNIX)

static bool canLinkAddressAcross(size_t distance, PCRelativeReach reach)
{
    const StubCatalog& catalog = defaultCatalog();
    StubAssembler jit(catalog.options(), catalog, reach);
    CCallHelpers::Label target = jit.label();
    while (jit.m_assembler.codeSize() < distance)
        jit.nop();
    jit.loadLabelAddress(target, gprOf(StubRegister::R0));
    ImageLinkBuffer buffer(jit, LinkBuffer::Profile::Thunk);
    AOT_TEST_CHECK(!buffer.didFailToAllocate());
    return jit.linkCallsAndAddresses(buffer.linkBuffer());
}

static void testAddressReach()
{
    AOT_TEST_CHECK(canLinkAddressAcross(2 * MB, PCRelativeReach::Near) == !isARM64());
    AOT_TEST_CHECK(canLinkAddressAcross(2 * MB, PCRelativeReach::Far));
}

std::span<const NativeTest> stubAssemblerTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/every-body-generates"_s, testStubBodyGeneration },
        { "stubs/recorded-registers"_s, testRecordedStubRegisters },
        { "stubs/allocatable-registers"_s, testAllocatableStubRegisters },
        { "stubs/recorded-references"_s, testRecordedStubReferences },
        { "stubs/calls-in-saving-scopes"_s, testCallsInSavingScopes },
        { "stubs/leaf-calls"_s, testLeafCalls },
        { "stubs/callers-that-keep-fewer-registers"_s, testCallersThatKeepFewerRegisters },
        { "stubs/address-reach"_s, testAddressReach },
#if OS(UNIX)
        { "stubs/assembler-assertions"_s, testStubAssemblerAssertions },
#endif
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

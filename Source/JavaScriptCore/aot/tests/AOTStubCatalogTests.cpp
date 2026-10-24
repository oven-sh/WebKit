/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeUnits.h"
#include "AOTCompileOptionWords.h"
#include "AOTCompiledCode.h"
#include "AOTStubCalls.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsCollections.h"
#include "AOTStubsConstants.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPropertyInlineCode.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsRegExp.h"
#include "AOTStubsValues.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

using EntrySet = WTF::BitSet<numberOfEntries>;

static constexpr uint32_t nameWithReadStub = 3;
static constexpr uint32_t nameWithStoreStub = 4;
static constexpr uint32_t nameWithDefineStub = 5;
static constexpr uint32_t nameWithoutStub = 7;
static constexpr uint32_t nameBeyondPolicy = 100000;
static constexpr uint32_t builtinCellNumber = 2;
static constexpr uint32_t literalMatchingBothWidths = 0;
static constexpr uint32_t literalMatchingAtCodePoints = 1;
static constexpr uint32_t literalWithNamedGroups = 2;
static constexpr uint32_t literalBeyondPolicy = 3;
static constexpr uint32_t minimumSizeOfLiteral = 3;
static constexpr uint32_t numberOfGroupsOfLiteral = 2;

static StubPolicy createBindingPolicy()
{
    StubPolicy policy;
    auto giveOwnStub = [&](Typed::Access access, uint32_t name) {
        policy.namesWithOwnStubByAccess[static_cast<size_t>(access)].set(name);
    };
    giveOwnStub(Typed::Access::Read, nameWithReadStub);
    giveOwnStub(Typed::Access::Store, nameWithStoreStub);
    giveOwnStub(Typed::Access::Define, nameWithDefineStub);
    policy.regExpLiterals.append(RegExpLiteralConstants { .minimumSize = minimumSizeOfLiteral, .numberOfGroups = numberOfGroupsOfLiteral, .usesLastIndex = false, .isGlobal = false, .matchesAtCodePointBoundaries = false, .recordsMatch = false, .hasNamedGroups = false });
    policy.regExpLiterals.append(RegExpLiteralConstants { .minimumSize = 0, .usesLastIndex = true, .isGlobal = false, .matchesAtCodePointBoundaries = true, .recordsMatch = true });
    policy.regExpLiterals.append(RegExpLiteralConstants { .numberOfGroups = numberOfGroupsOfLiteral, .usesLastIndex = false, .isGlobal = false, .matchesAtCodePointBoundaries = false, .recordsMatch = false, .hasNamedGroups = true });
    AOT_TEST_CHECK(policy.regExpLiterals.size() == literalBeyondPolicy);
    return policy;
}

static StubOptions createValidatingOptions()
{
    StubOptions options;
    options.validates = true;
    return options;
}

static bool areEqual(const StubSignature& a, const StubSignature& b)
{
    return a.operands == b.operands
        && a.numberOfOperands == b.numberOfOperands
        && a.usesCallConvention == b.usesCallConvention
        && a.result == b.result
        && a.firstResult == b.firstResult
        && a.secondResult == b.secondResult
        && a.immediate == b.immediate
        && a.boundImmediates == b.boundImmediates
        && a.trueCondition == b.trueCondition;
}

static bool haveEqualAnswers(const StubCatalog& a, const StubCatalog& b, StubKey key)
{
    const StubSummary& summary = a.summaryOf(key);
    const StubSummary& otherSummary = b.summaryOf(key);
    return summary.clobbered == otherSummary.clobbered
        && summary.clobberedInline == otherSummary.clobberedInline
        && a.effectsOf(key) == b.effectsOf(key)
        && a.isLeaf(key) == b.isLeaf(key);
}

static bool overlaps(const RegisterSet& a, const RegisterSet& b)
{
    RegisterSet common = a;
    common.filter(b);
    return !common.isEmpty();
}

static RegisterSet macroClobberedRegisters()
{
    return RegisterSet::macroClobberedGPRs().merge(RegisterSet::macroClobberedFPRs());
}

static RegisterSet resultAndImmediateRegisters(const StubSignature& signature)
{
    RegisterSet registers;
    for (Reg result : { signature.firstResult, signature.secondResult }) {
        if (result)
            registers.add(result, IgnoreVectors);
    }
    if (signature.immediate)
        registers.add(*signature.immediate, IgnoreVectors);
    return registers;
}

static RegisterSet integerRegistersLostToCallees(const RegisterSet& clobbered, const RegisterSet& clobberedInline)
{
    RegisterSet lost = clobbered;
    lost.exclude(clobberedInline);
    lost.exclude(RegisterSet::macroClobberedGPRs());
    lost.filter(RegisterSet::allGPRs());
    return lost;
}

template<typename Check>
static void checkEveryKeyOfEveryVariant(const Check& check)
{
    Vector<StubKey> keys = allStubKeys();
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options, StubPolicy { });
        for (StubKey key : keys)
            check(catalog, key);
    }
}

static void checkSummary(const StubCatalog& catalog, StubKey key)
{
    const StubSummary& summary = catalog.summaryOf(key);
    AOT_TEST_CHECK(&catalog.summaryOf(key) == &summary);
    AOT_TEST_CHECK(areEqual(summary.signature, signatureFor(key)));

    StubKey body = bodyOf(key);
    AOT_TEST_CHECK(catalog.hasRecordingOf(body));
    AOT_TEST_CHECK(summary.clobbered.subsumes(catalog.closedClobbersOf(catalog.recordingOf(body))));
    AOT_TEST_CHECK(summary.clobbered.subsumes(summary.clobberedInline));

    for (const RegisterSet* clobbered : { &summary.clobbered, &summary.clobberedInline }) {
        AOT_TEST_CHECK(clobbered->subsumes(macroClobberedRegisters()));
        AOT_TEST_CHECK(clobbered->subsumes(resultAndImmediateRegisters(summary.signature)));
    }
    AOT_TEST_CHECK(!overlaps(summary.clobbered, pinnedRegisters()));
}

static void testStubSummaries()
{
    checkEveryKeyOfEveryVariant(checkSummary);
}

static void checkDeclaredColumns(const StubCatalog& catalog, StubKey key)
{
    const StubDescriptor& descriptor = descriptorOf(key.family());
    const StubSummary& summary = catalog.summaryOf(key);
    Typed::Effects effects = catalog.effectsOf(key);

    AOT_TEST_CHECK(descriptor.ownEffects.toEffects().isSubsetOf(effects));
    if (descriptor.clobbers == StubClobbers::ClobbersOnlyScratch)
        AOT_TEST_CHECK(integerRegistersLostToCallees(summary.clobbered, summary.clobberedInline).isEmpty());
    if (descriptor.leafness == StubLeafness::Leaf) {
        AOT_TEST_CHECK(catalog.isLeaf(key));
        AOT_TEST_CHECK(!effects.mayThrow && !effects.mayCollect && !effects.mayCallUnknownCode);
    }
    if (summary.signature.usesCallConvention) {
        AOT_TEST_CHECK(!descriptorOf(bodyOf(key).family()).ownEffects.isDeclared);
        AOT_TEST_CHECK(!catalog.isLeaf(key));
        AOT_TEST_CHECK(Typed::Effects::unknownCode().isSubsetOf(effects));
        AOT_TEST_CHECK(summary.clobbered.subsumes(registersClobberedByCompiledCalls()));
    }
}

static void testDeclaredColumns()
{
    checkEveryKeyOfEveryVariant(checkDeclaredColumns);
}

static void checkEffectsCover(const StubCatalog& catalog, const StubRecording& recording, const Typed::Effects& effects)
{
    AOT_TEST_CHECK(!recording.hasUnknownEffects || Typed::Effects::unknownCode().isSubsetOf(effects));
    for (size_t index : recording.operationsCalled)
        AOT_TEST_CHECK(effectsOf(static_cast<Entry>(index)).isSubsetOf(effects));
    for (const StubReference& reference : recording.references) {
        if (reference.kind != StubReference::Kind::ReturnedAddress)
            AOT_TEST_CHECK(catalog.effectsOf(reference.target).isSubsetOf(effects));
    }
}

static RegisterSet withoutRestoredRegisters(RegisterSet clobberedByTarget, const StubReference& reference)
{
    if (reference.kind != StubReference::Kind::Address)
        clobberedByTarget.exclude(reference.savedAround);
    return clobberedByTarget;
}

static void checkClosureOverReferences(const StubCatalog& catalog, StubKey key)
{
    const StubRecording& recording = catalog.recordingOf(bodyOf(key));
    const StubSummary& summary = catalog.summaryOf(key);
    bool isLeaf = catalog.isLeaf(key);

    AOT_TEST_CHECK(summary.clobbered.subsumes(recording.clobbered()));
    AOT_TEST_CHECK(summary.clobberedInline.subsumes(recording.clobberedInline));
    AOT_TEST_CHECK(!recording.leavesStubs || !isLeaf);
    checkEffectsCover(catalog, recording, catalog.effectsOf(key));

    for (const StubReference& reference : recording.references) {
        if (reference.kind == StubReference::Kind::ReturnedAddress)
            continue;
        AOT_TEST_CHECK(catalog.isLeaf(reference.target) || !isLeaf);
        if (neverReturns(reference.target))
            continue;
        const StubSummary& target = catalog.summaryOf(reference.target);
        AOT_TEST_CHECK(summary.clobbered.subsumes(withoutRestoredRegisters(target.clobbered, reference)));
        AOT_TEST_CHECK(summary.clobberedInline.subsumes(withoutRestoredRegisters(target.clobberedInline, reference)));
    }
}

static void testClosureOverReferences()
{
    checkEveryKeyOfEveryVariant(checkClosureOverReferences);
}

static void checkReadSummaries(const StubCatalog& catalog)
{
    RegisterSet probeRegisters { gprOf(StubRegister::T10), gprOf(StubRegister::T11), gprOf(StubRegister::T12), gprOf(StubRegister::T13) };
    if (isARM64())
        probeRegisters.add(gprOf(stubImmediateRegister), IgnoreVectors);
    AOT_TEST_CHECK(!probeRegisters.isEmpty());
    for (StubKey key : { getById(), getByIdAfterFrontProbe(), putByIdAfterFrontProbe(PutMode::Strict), putByIdAfterFrontProbe(PutMode::Sloppy), putByIdAfterFrontProbe(PutMode::Define) })
        AOT_TEST_CHECK(catalog.summaryOf(key).clobbered.subsumes(probeRegisters));
    AOT_TEST_CHECK(&catalog.summaryOf(getById().withImmediate(nameWithReadStub)) == &catalog.summaryOf(getById()));

    // The way to a getter stores and loads two floating-point registers, not all: the others are lost to whoever reads or stores by name.
    for (StubKey key : { getById(), getByIdAtSite(), putById(PutMode::Strict) }) {
        RegisterSet kept = RegisterSet::allFPRs();
        kept.exclude(catalog.summaryOf(key).clobbered);
        AOT_TEST_CHECK(kept.numberOfSetRegisters() < RegisterSet::allFPRs().numberOfSetRegisters() / 2);
        if (isX86_64())
            AOT_TEST_CHECK(kept.numberOfSetRegisters() == 2);
    }
}

static void checkPreservingCallSummary(const StubCatalog& catalog, StubKey key)
{
    const StubSummary& summary = catalog.summaryOf(key);
    RegisterSet callProtocolRegisters = macroClobberedRegisters();
    callProtocolRegisters.merge(resultAndImmediateRegisters(summary.signature));
    if (preservingCallShapes[key.form()].saves == OperationCall::Saves::CallerSavedGPRs)
        callProtocolRegisters.merge(RegisterSet::allFPRs());
    AOT_TEST_CHECK(callProtocolRegisters.subsumes(summary.clobbered));
}

static void checkPreservingCallSummaries(const StubCatalog& catalog)
{
    for (uint8_t form = 0; form < std::size(preservingCallShapes); ++form)
        checkPreservingCallSummary(catalog, StubKey(StubFamily::CallOperationPreservingRegisters).withForm(form));

    StubKey barrierCall = callOperationPreservingRegisters({ .saves = OperationCall::Saves::CallerSavedRegisters, .checksException = false })
        .withImmediate(offsetInRuntimeTable(Entry::operationAOTBarrierSlowPreservingRegisters));
    AOT_TEST_CHECK(catalog.hasRecordingOf(barrierCall));
    checkPreservingCallSummary(catalog, barrierCall);
}

static void checkPreservingCallRegisters(const StubCatalog& catalog, OperationCall call)
{
    RegisterSet clobbered = registersClobberedByPreservingCall(catalog, call);
    unsigned numberOfVariants = 0;
    for (OperationCall::Saves saves : { OperationCall::Saves::CallerSavedRegisters, OperationCall::Saves::CallerSavedGPRs }) {
        call.saves = saves;
        std::optional<StubKey> key = tryCallOperationPreservingRegisters(call);
        if (!key)
            continue;
        ++numberOfVariants;
        AOT_TEST_CHECK(clobbered.subsumes(catalog.summaryOf(*key).clobbered));
    }
    AOT_TEST_CHECK(numberOfVariants && !clobbered.isEmpty());
}

static void checkExceptionSummaries(const StubCatalog& catalog)
{
    constexpr StubKey bodyWithOneExit = handleExceptionAtPrologue();
    static_assert(bodyOf(bodyWithOneExit) == bodyWithOneExit);
    const StubRecording& recording = catalog.recordingOf(bodyWithOneExit);
    AOT_TEST_CHECK(recording.operationsCalled.isEmpty() && !recording.hasUnknownEffects && !recording.leavesStubs);
    AOT_TEST_CHECK(recording.references.size() == 1);
    AOT_TEST_CHECK(recording.references[0].target == handleException(false) && recording.references[0].kind == StubReference::Kind::Jump);
    AOT_TEST_CHECK(catalog.effectsOf(bodyWithOneExit).mayThrow && !catalog.isLeaf(bodyWithOneExit));

    const StubRecording& handler = catalog.recordingOf(handleException(false));
    AOT_TEST_CHECK(!handler.hasUnknownEffects && handler.operationsCalled.get(indexOf(Entry::operationAOTLookupExceptionHandler)));

    for (StubKey key : { handleException(false), handleException(true), throwStackOverflow(false), throwStackOverflow(true) })
        AOT_TEST_CHECK(catalog.effectsOf(key).mayThrow);
}

static void checkOperationCallEffects(const StubCatalog& catalog)
{
    StubKey operationCall(StubFamily::CallOperation);
    AOT_TEST_CHECK(Typed::Effects::unknownCode().isSubsetOf(catalog.effectsOf(operationCall)));
    AOT_TEST_CHECK(!catalog.effectsOf(operationCall.withImmediate(offsetInRuntimeTable(Entry::operationAOTBarrierSlow))).mayThrow);

    StubKey allocation = tailCallOperation(OperationCall::FirstArgument::Instance).withImmediate(offsetInRuntimeTable(Entry::operationAOTAllocateSlow));
    AOT_TEST_CHECK(catalog.effectsOf(allocation) == Typed::declaredBoundOf(RuntimeOperation::AllocateSlow));
    AOT_TEST_CHECK(catalog.effectsOf(allocation) != catalog.effectsOf(allocation.withoutImmediate()));
}

static void checkBarrierEffects(const StubCatalog& catalog)
{
    Typed::Effects effects = catalog.effectsOf(writeBarrier());
    AOT_TEST_CHECK(effects.writes.contains(Typed::AbstractHeap::RuntimeState));
    AOT_TEST_CHECK(effects.writes.containsOnly(Typed::AbstractHeapSet { Typed::AbstractHeap::RuntimeState }));
    AOT_TEST_CHECK(!effects.mayExitSideways() && !effects.mayCollect);
}

static void checkLeaves(const StubCatalog& catalog)
{
    for (StubKey leaf : { weakMapGet(), weakMapHas(), weakSetHas() })
        AOT_TEST_CHECK(catalog.isLeaf(leaf));
    for (KeyComparison comparison : { KeyComparison::AnyValue, KeyComparison::String })
        AOT_TEST_CHECK(!catalog.isLeaf(mapGet(comparison)));
    AOT_TEST_CHECK(catalog.isLeaf(mapGet(KeyComparison::Bits)) == !catalog.options().useWideReferences);
    AOT_TEST_CHECK(!catalog.isLeaf(writeBarrier()));
}

static void testKnownStubSummaries()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    checkReadSummaries(catalog);
    checkExceptionSummaries(catalog);
    checkPreservingCallSummaries(catalog);
    checkPreservingCallRegisters(catalog, { .returns = OperationCall::Returns::Value, .firstArgument = OperationCall::FirstArgument::Instance, .checksException = true });
    checkPreservingCallRegisters(catalog, { .returns = OperationCall::Returns::Double, .firstArgument = OperationCall::FirstArgument::None, .checksException = false });

    AOT_TEST_CHECK(!catalog.effectsOf(toBoolean()).mayThrow);
    checkOperationCallEffects(catalog);
    checkBarrierEffects(catalog);
    checkLeaves(catalog);

    StubOptions optionsWithWideReferences;
    optionsWithWideReferences.useWideReferences = true;
    checkLeaves(StubCatalog(optionsWithWideReferences, StubPolicy { }));
}

static void testEqualStubCatalogs()
{
    StubCatalog first(StubOptions { }, StubPolicy { });
    {
        StubCatalog other(createValidatingOptions(), createBindingPolicy());
    }
    StubCatalog second(StubOptions { }, StubPolicy { });
    for (StubKey key : allStubKeys())
        AOT_TEST_CHECK(haveEqualAnswers(first, second, key));
}

static void checkBinding(const StubCatalog& catalog, StubKey key, uint32_t boundImmediate, uint32_t unboundImmediate)
{
    AOT_TEST_CHECK(catalog.bindsImmediate(key, boundImmediate));
    AOT_TEST_CHECK(!catalog.bindsImmediate(key, unboundImmediate));
}

static void checkNameBinding(const StubCatalog& catalog)
{
    StubKey read = getById();
    checkBinding(catalog, read, nameWithReadStub, nameWithStoreStub);
    AOT_TEST_CHECK(!catalog.bindsImmediate(read.withCallerFrame(CallerFrame::StubFrame), nameWithReadStub));
    if (isX86_64())
        AOT_TEST_CHECK(!catalog.bindsImmediate(read.withOperandRegister(alternateReceiverGPRs()[0]), nameWithReadStub));
    AOT_TEST_CHECK(!catalog.bindsImmediate(read, nameWithoutStub));
    AOT_TEST_CHECK(!catalog.bindsImmediate(read, nameBeyondPolicy));

    for (PutMode mode : { PutMode::Strict, PutMode::Sloppy })
        checkBinding(catalog, putById(mode), nameWithStoreStub, nameWithDefineStub);
    checkBinding(catalog, putById(PutMode::Define), nameWithDefineStub, nameWithStoreStub);

    checkBinding(catalog, callMethodById(1), nameWithReadStub, nameWithStoreStub);
}

static void checkBindingOfEveryKey(const StubCatalog& catalog)
{
    constexpr std::array immediates { 0u, nameWithReadStub, nameWithStoreStub, nameWithDefineStub, nameWithoutStub, nameBeyondPolicy };
    for (StubKey key : allStubKeys()) {
        ImmediateBinding binding = descriptorOf(key.family()).binding;
        StubBoundImmediates boundImmediates = signatureFor(key).boundImmediates;
        bool isPlainVariant = !key.operandRegister() && key.callerFrame() == CallerFrame::HasFrame;
        AOT_TEST_CHECK(boundImmediates == StubBoundImmediates::None || binding == ImmediateBinding::ByPolicy);
        for (uint32_t immediate : immediates) {
            bool binds = catalog.bindsImmediate(key, immediate);
            AOT_TEST_CHECK(!binds || exists(key.withImmediate(immediate)));
            AOT_TEST_CHECK(!binds || binding != ImmediateBinding::Never);
            AOT_TEST_CHECK(binds || binding != ImmediateBinding::OperationOffset);
            if (binding == ImmediateBinding::ByPolicy && !isPlainVariant)
                AOT_TEST_CHECK(!binds);
            if (boundImmediates == StubBoundImmediates::Every && isPlainVariant)
                AOT_TEST_CHECK(binds);
        }
    }
}

static void testStubImmediateBinding()
{
    StubCatalog catalog(StubOptions { }, createBindingPolicy());
    checkNameBinding(catalog);
    checkBindingOfEveryKey(catalog);

    AOT_TEST_CHECK(catalog.bindsImmediate(builtinCell(), 0));
    AOT_TEST_CHECK(catalog.bindsImmediate(regExpMatchLiteral(Typed::RegExpMatchForm::Test, false), literalBeyondPolicy));
    AOT_TEST_CHECK(!catalog.bindsImmediate(toBoolean(), 0));
    AOT_TEST_CHECK(catalog.bindsImmediate(StubKey(StubFamily::CallOperation), 0));
    AOT_TEST_CHECK(signatureFor(createConstant(CreatedConstant::String)).immediate && !catalog.bindsImmediate(createConstant(CreatedConstant::String), 0));

    StubCatalog catalogWithoutNames(StubOptions { }, StubPolicy { });
    for (StubKey key : { getById(), putById(PutMode::Strict), putById(PutMode::Define), callMethodById(1) }) {
        for (uint32_t name : { nameWithReadStub, nameWithStoreStub, nameWithDefineStub })
            AOT_TEST_CHECK(!catalogWithoutNames.bindsImmediate(key, name));
    }
}

static void checkBoundOperationCall(const StubCatalog& catalog, StubKey boundKey)
{
    StubRecording recording = catalog.dryRun(boundKey);
    AOT_TEST_CHECK(recording.body == boundKey);
    AOT_TEST_CHECK(catalog.summaryOf(boundKey).clobbered.subsumes(catalog.closedClobbersOf(recording)));
    AOT_TEST_CHECK(!catalog.isLeaf(boundKey));
}

static void testStubPreludeClobbers()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    Entry entryWithoutPrelude = operationFrontEnds()[0].raw;
    unsigned numberOfKeys = 0;
    for (StubKey key : allStubKeys()) {
        if (descriptorOf(key.family()).binding != ImmediateBinding::OperationOffset)
            continue;
        ++numberOfKeys;
        for (const OperationPrelude& prelude : operationPreludes()) {
            AOT_TEST_CHECK(prelude.operation != entryWithoutPrelude);
            StubKey boundKey = key.withImmediate(offsetInRuntimeTable(prelude.operation));
            checkBoundOperationCall(catalog, boundKey);
            AOT_TEST_CHECK(catalog.hasRecordingOf(boundKey));
        }
        StubKey keyWithoutPrelude = key.withImmediate(offsetInRuntimeTable(entryWithoutPrelude));
        checkBoundOperationCall(catalog, keyWithoutPrelude);
        AOT_TEST_CHECK(catalog.effectsOf(keyWithoutPrelude) == effectsOf(entryWithoutPrelude));
    }
    AOT_TEST_CHECK(numberOfKeys && !operationPreludes().empty());
}

static void checkBoundBody(const StubCatalog& catalog, StubKey key, uint32_t immediate)
{
    AOT_TEST_CHECK(catalog.bindsImmediate(key, immediate));
    StubKey boundKey = key.withImmediate(immediate);
    AOT_TEST_CHECK(exists(boundKey) && bodyOf(boundKey) == boundKey);

    StubRecording recording = catalog.dryRun(boundKey);
    AOT_TEST_CHECK(recording.body == boundKey);
    AOT_TEST_CHECK(catalog.summaryOf(boundKey).clobbered.subsumes(catalog.closedClobbersOf(recording)));
    AOT_TEST_CHECK(!recording.leavesStubs || !catalog.isLeaf(boundKey));
    checkEffectsCover(catalog, recording, catalog.effectsOf(boundKey));
}

static void checkSymbolsOfLiterals(const StubCatalog& catalog)
{
    StubKey key = regExpMatchLiteral(Typed::RegExpMatchForm::Test, false);
    AOT_TEST_CHECK(catalog.recordingOf(key).symbols.isEmpty());

    StubRecording bothWidths = catalog.dryRun(key.withImmediate(literalMatchingBothWidths));
    AOT_TEST_CHECK(bothWidths.symbols.size() == 2);
    AOT_TEST_CHECK(bothWidths.symbols[0] == (CodeSymbol { SymbolSpace::RegExp, literalMatchingBothWidths, 0 }));
    AOT_TEST_CHECK(bothWidths.symbols[1] == (CodeSymbol { SymbolSpace::RegExp, literalMatchingBothWidths, 1 }));

    StubRecording oneWidth = catalog.dryRun(key.withImmediate(literalMatchingAtCodePoints));
    AOT_TEST_CHECK(oneWidth.symbols.size() == 1);
    AOT_TEST_CHECK(oneWidth.symbols[0] == (CodeSymbol { SymbolSpace::RegExp, literalMatchingAtCodePoints, 0 }));

    for (Typed::RegExpMatchForm form : allMembersOf<Typed::RegExpMatchForm>) {
        StubKey withoutObject = regExpMatchLiteralWithoutObject(form);
        AOT_TEST_CHECK(signatureOfRegExpMatchLiteral(withoutObject).numberOfOperands == 1 && signatureOfRegExpMatchLiteral(regExpMatchLiteral(form, false)).numberOfOperands == 2);
        AOT_TEST_CHECK(catalog.recordingOf(withoutObject).symbols.isEmpty());
        AOT_TEST_CHECK(catalog.dryRun(withoutObject.withImmediate(literalMatchingBothWidths)).symbols.size() == 2);
        // A match that uses lastIndex or is recorded needs the object, so the operation does all of it.
        AOT_TEST_CHECK(catalog.dryRun(withoutObject.withImmediate(literalMatchingAtCodePoints)).symbols.isEmpty());
        AOT_TEST_CHECK(catalog.dryRun(withoutObject.withImmediate(literalBeyondPolicy)).symbols.isEmpty());
    }
}

static void checkArraysOfLiteralsWithoutObject(const StubCatalog& catalog)
{
    auto recordingOf = [&](Typed::RegExpMatchForm form, uint32_t record) {
        return catalog.dryRun(regExpMatchLiteralWithoutObject(form).withImmediate(record));
    };
    auto leavesArrayToStub = [](const StubRecording& recording) {
        return std::ranges::contains(recording.references, finishRegExpExec(FinishRegExpExecForm::WithoutObject), &StubReference::target);
    };
    auto leavesArrayToOperation = [](const StubRecording& recording) {
        return recording.operationsCalled.get(indexOf(Entry::operationAOTRegExpMatchLiteral));
    };
    for (Typed::RegExpMatchForm form : { Typed::RegExpMatchForm::Exec, Typed::RegExpMatchForm::Match }) {
        StubRecording plain = recordingOf(form, literalMatchingBothWidths);
        AOT_TEST_CHECK(leavesArrayToStub(plain) && !leavesArrayToOperation(plain));
        StubRecording withNames = recordingOf(form, literalWithNamedGroups);
        AOT_TEST_CHECK(!leavesArrayToStub(withNames) && leavesArrayToOperation(withNames));
    }
    for (Typed::RegExpMatchForm form : { Typed::RegExpMatchForm::Test, Typed::RegExpMatchForm::Search }) {
        StubRecording plain = recordingOf(form, literalMatchingBothWidths);
        AOT_TEST_CHECK(!leavesArrayToStub(plain) && !leavesArrayToOperation(plain));
    }
}

static void testBoundBodies()
{
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options, createBindingPolicy());
        checkBoundBody(catalog, getById(), nameWithReadStub);
        for (PutMode mode : { PutMode::Strict, PutMode::Sloppy })
            checkBoundBody(catalog, putById(mode), nameWithStoreStub);
        checkBoundBody(catalog, putById(PutMode::Define), nameWithDefineStub);
        for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count)
            checkBoundBody(catalog, callMethodById(count), nameWithReadStub);
        checkBoundBody(catalog, builtinCell(), builtinCellNumber);
        for (uint8_t form = 0; form < descriptorOf(StubFamily::RegExpMatchLiteral).numberOfForms; ++form) {
            for (uint32_t record : { literalMatchingBothWidths, literalMatchingAtCodePoints, literalWithNamedGroups, literalBeyondPolicy })
                checkBoundBody(catalog, StubKey(StubFamily::RegExpMatchLiteral).withForm(form), record);
        }
        checkSymbolsOfLiterals(catalog);
        checkArraysOfLiteralsWithoutObject(catalog);
    }
}

static void testRegExpLiteralConstants()
{
    StubPolicy policy = createBindingPolicy();

    RegExpLiteralConstants plain = policy.regExpLiteral(literalMatchingBothWidths, false);
    AOT_TEST_CHECK(plain.minimumSize == minimumSizeOfLiteral && !plain.usesLastIndex && !plain.isGlobal);
    AOT_TEST_CHECK(!plain.matchesAtCodePointBoundaries && !plain.recordsMatch && plain.numberOfGroups == numberOfGroupsOfLiteral && !plain.hasNamedGroups);

    RegExpLiteralConstants global = policy.regExpLiteral(literalMatchingBothWidths, true);
    AOT_TEST_CHECK(global.minimumSize == minimumSizeOfLiteral && global.usesLastIndex && global.isGlobal);
    AOT_TEST_CHECK(!global.matchesAtCodePointBoundaries && !global.recordsMatch);

    RegExpLiteralConstants sticky = policy.regExpLiteral(literalMatchingAtCodePoints, false);
    AOT_TEST_CHECK(sticky.usesLastIndex && !sticky.isGlobal && sticky.matchesAtCodePointBoundaries && sticky.recordsMatch);

    for (std::optional<uint32_t> record : { std::optional<uint32_t>(), std::optional<uint32_t>(literalBeyondPolicy) }) {
        for (bool isGlobal : { false, true }) {
            RegExpLiteralConstants unknown = policy.regExpLiteral(record, isGlobal);
            AOT_TEST_CHECK(!unknown.minimumSize && unknown.usesLastIndex && unknown.isGlobal == isGlobal);
            AOT_TEST_CHECK(unknown.matchesAtCodePointBoundaries && unknown.recordsMatch && unknown.hasNamedGroups);
        }
    }
}

static constexpr StubKey handWrittenBody = writeBarrier();
static constexpr StubKey handWrittenBodyWithResult = toBoolean();
static constexpr StubKey handWrittenBodyWithCallConvention = callMethodById(0);
static constexpr StubKey stubLosingRegisters = tailCallOperation(OperationCall::FirstArgument::Instance);
static constexpr GPRReg registerSavedAroundCall = gprOf(StubRegister::A1);

static_assert(bodyOf(handWrittenBody) == handWrittenBody && descriptorOf(handWrittenBody.family()).clobbers == StubClobbers::ClobbersOnlyScratch);
static_assert(bodyOf(handWrittenBodyWithResult) == handWrittenBodyWithResult && bodyOf(handWrittenBodyWithCallConvention) == handWrittenBodyWithCallConvention);
static_assert(bodyOf(stubLosingRegisters) == stubLosingRegisters && !neverReturns(stubLosingRegisters));

template<typename Emit>
static StubRecording record(const StubCatalog& catalog, StubKey body, const Emit& emit)
{
    AOT_TEST_CHECK(entriesOf(body).size() == 1);
    StubAssembler jit(catalog.options(), catalog);
    return jit.generateForTesting(body, emit);
}

static void checkClobbersOfCallees(const StubCatalog& catalog)
{
    constexpr Entry operation = Entry::operationAOTBarrierSlow;
    auto callOperation = [](StubAssembler& jit) {
        jit.callOperation(operation, jit.allocateScratch(StubRegister::T9));
    };

    StubRecording empty = record(catalog, handWrittenBody, [](StubAssembler&) { });
    AOT_TEST_CHECK(empty.clobbered().isEmpty() && catalog.closedClobbersOf(empty).isEmpty());

    StubRecording unsaved = record(catalog, handWrittenBody, callOperation);
    RegisterSet clobberedByUnsaved = catalog.closedClobbersOf(unsaved);
    AOT_TEST_CHECK(clobberedByUnsaved == unsaved.clobbered() && clobberedByUnsaved.subsumes(registersClobberedByOperations()));
    AOT_TEST_CHECK(!integerRegistersLostToCallees(clobberedByUnsaved, unsaved.clobberedInline).isEmpty());

    StubRecording saved = record(catalog, handWrittenBody, [&](StubAssembler& jit) {
        jit.preservingCallerSavedRegisters(false, [&](unsigned) { callOperation(jit); });
    });
    AOT_TEST_CHECK(saved.leavesStubs && catalog.closedClobbersOf(saved).isEmpty());

    StubRecording savingIntegers = record(catalog, handWrittenBody, [](StubAssembler& jit) {
        jit.callOperationAndReturn(operation, { .returns = OperationCall::Returns::Void, .saves = OperationCall::Saves::CallerSavedGPRs, .checksException = false });
    });
    RegisterSet clobberedBySavingIntegers = catalog.closedClobbersOf(savingIntegers);
    AOT_TEST_CHECK(integerRegistersLostToCallees(clobberedBySavingIntegers, savingIntegers.clobberedInline).isEmpty());
    AOT_TEST_CHECK(overlaps(clobberedBySavingIntegers, RegisterSet::allFPRs()));

    StubRecording leafCall = record(catalog, handWrittenBody, [](StubAssembler& jit) { jit.callLeaf(Entry::operationAOTStringImplsAreEqual); });
    RegisterSet clobberedByLeafCall = catalog.closedClobbersOf(leafCall);
    AOT_TEST_CHECK(!leafCall.leavesStubs && clobberedByLeafCall.subsumes(registersClobberedByPreserveMostLeaves()));
    AOT_TEST_CHECK(integerRegistersLostToCallees(clobberedByLeafCall, leafCall.clobberedInline).isEmpty());

    StubRecording enteredLikeFunction = record(catalog, handWrittenBodyWithCallConvention, [](StubAssembler&) { });
    AOT_TEST_CHECK(signatureFor(handWrittenBodyWithCallConvention).usesCallConvention && !enteredLikeFunction.leavesStubs);
    AOT_TEST_CHECK(catalog.closedClobbersOf(enteredLikeFunction).subsumes(registersClobberedByCompiledCalls()));
}

static void checkClobbersOfCalledStubs(const StubCatalog& catalog, const RegisterSet& clobberedByTarget)
{
    for (bool isCall : { false, true }) {
        StubRecording recording = record(catalog, handWrittenBody, [&](StubAssembler& jit) {
            if (isCall)
                jit.callStub(stubLosingRegisters);
            else
                jit.jumpToStub(stubLosingRegisters);
        });
        AOT_TEST_CHECK(recording.clobbered().isEmpty() && !recording.leavesStubs);
        AOT_TEST_CHECK(catalog.closedClobbersOf(recording) == clobberedByTarget);
    }

    StubRecording savedCall = record(catalog, handWrittenBody, [](StubAssembler& jit) {
        jit.preservingCallerSavedRegisters(false, [&](unsigned) { jit.callStub(stubLosingRegisters); });
    });
    AOT_TEST_CHECK(savedCall.references.size() == 1 && savedCall.references[0].savedAround == registersClobberedByOperations());
    AOT_TEST_CHECK(catalog.closedClobbersOf(savedCall) == RegisterSet(clobberedByTarget).exclude(registersClobberedByOperations()));
    AOT_TEST_CHECK(integerRegistersLostToCallees(catalog.closedClobbersOf(savedCall), savedCall.clobberedInline).isEmpty());

    StubRecording partlySavedCall = record(catalog, handWrittenBody, [](StubAssembler& jit) {
        jit.preserving({ registerSavedAroundCall }, [&] { jit.callStub(stubLosingRegisters); });
    });
    AOT_TEST_CHECK(catalog.closedClobbersOf(partlySavedCall) == RegisterSet(clobberedByTarget).remove(registerSavedAroundCall));
}

static void checkClobbersOfOtherReferences(const StubCatalog& catalog, const RegisterSet& clobberedByTarget)
{
    StubRecording address = record(catalog, handWrittenBody, [](StubAssembler& jit) {
        jit.preserving({ registerSavedAroundCall }, [&] { jit.loadStubAddress(stubLosingRegisters, jit.allocateScratch(StubRegister::T9)); });
    });
    AOT_TEST_CHECK(address.references.size() == 1 && containsRegister(address.references[0].savedAround, registerSavedAroundCall));
    AOT_TEST_CHECK(catalog.closedClobbersOf(address).subsumes(clobberedByTarget));

    StubRecording returnedAddress = record(catalog, handWrittenBodyWithResult, [](StubAssembler& jit) { jit.returnStubAddress(stubLosingRegisters); });
    AOT_TEST_CHECK(returnedAddress.references.size() == 1);
    AOT_TEST_CHECK(catalog.closedClobbersOf(returnedAddress) == returnedAddress.clobbered() && !returnedAddress.clobbered().subsumes(clobberedByTarget));

    StubRecording thrower = record(catalog, handWrittenBody, [](StubAssembler& jit) { jit.jumpToExceptionHandler(); });
    AOT_TEST_CHECK(thrower.references.size() == 1 && neverReturns(thrower.references[0].target));
    AOT_TEST_CHECK(!catalog.closedClobbersOf(catalog.recordingOf(bodyOf(thrower.references[0].target))).isEmpty());
    AOT_TEST_CHECK(catalog.closedClobbersOf(thrower).isEmpty());

    StubKey boundBody = getById().withImmediate(nameWithReadStub);
    AOT_TEST_CHECK(!catalog.hasRecordingOf(boundBody));
    StubRecording jumpToBoundBody = record(catalog, handWrittenBody, [&](StubAssembler& jit) { jit.jumpToStub(boundBody); });
    AOT_TEST_CHECK(catalog.closedClobbersOf(jumpToBoundBody) == catalog.closedClobbersOf(catalog.dryRun(boundBody)));
    AOT_TEST_CHECK(!catalog.closedClobbersOf(jumpToBoundBody).isEmpty());
}

static void testClosedClobbers()
{
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options, createBindingPolicy());
        RegisterSet clobberedByTarget = catalog.closedClobbersOf(catalog.recordingOf(stubLosingRegisters));
        AOT_TEST_CHECK(clobberedByTarget.subsumes(registersClobberedByOperations()) && containsRegister(clobberedByTarget, registerSavedAroundCall));
        checkClobbersOfCallees(catalog);
        checkClobbersOfCalledStubs(catalog, clobberedByTarget);
        checkClobbersOfOtherReferences(catalog, clobberedByTarget);
    }
}

namespace {

struct BuiltBlob {
    uint64_t statistic(StatisticID id) const { return statistics[static_cast<size_t>(id)]; }

    StubBlob blob;
    Vector<uint64_t> statistics;
};

} // anonymous namespace

static BuiltBlob build(const StubOptions& options, StubPolicy&& policy, std::span<const StubKey> calledKeys, const EntrySet& entriesUsedByFunctions)
{
    BuildContext context(makeDefaultCompileOptions());
    StubCatalog catalog(options, WTF::move(policy));
    auto blob = buildStubBlob(context, catalog, calledKeys, entriesUsedByFunctions);
    AOT_TEST_CHECK(blob.has_value() && !context.diagnostics().hasErrors());
    return { WTF::move(*blob), context.statistics().values() };
}

static BuiltBlob buildWithEveryKey(const StubOptions& options)
{
    EntrySet everyEntry;
    everyEntry.setAll();
    return build(options, { }, stubKeysWithLinkage(StubLinkage::CalledByCode).span(), everyEntry);
}

static BuiltBlob buildWithNoKey(const StubOptions& options)
{
    return build(options, { }, { }, EntrySet { });
}

static bool areEqual(const StubBlob& a, const StubBlob& b)
{
    return a.code.bytes == b.code.bytes
        && a.code.relocations == b.code.relocations
        && std::ranges::equal(a.placements, b.placements, [](const StubPlacement& x, const StubPlacement& y) {
            return x.key == y.key && x.offset == y.offset;
        })
        && std::ranges::equal(a.returnSites, b.returnSites, [](const StubReturnSite& x, const StubReturnSite& y) {
            return x.offset == y.offset && x.kind == y.kind;
        })
        && a.entriesUsed == b.entriesUsed;
}

static void checkPlacements(const StubBlob& blob)
{
    for (size_t i = 0; i < blob.placements.size(); ++i) {
        const StubPlacement& placement = blob.placements[i];
        AOT_TEST_CHECK(!i || blob.placements[i - 1].key < placement.key);
        AOT_TEST_CHECK(exists(placement.key));
        AOT_TEST_CHECK(placement.offset < blob.code.bytes.size());
        AOT_TEST_CHECK(blob.offsetOf(placement.key) == placement.offset);
    }
}

static void checkReturnSites(const StubBlob& blob)
{
    std::array<unsigned, numberOfStubFrameKinds> numberOfSites { };
    for (size_t i = 0; i < blob.returnSites.size(); ++i) {
        const StubReturnSite& site = blob.returnSites[i];
        AOT_TEST_CHECK(!i || blob.returnSites[i - 1].offset < site.offset);
        AOT_TEST_CHECK(site.offset && site.offset <= blob.code.bytes.size());
        AOT_TEST_CHECK(site.kind != StubFrameKind::Plain);
        ++numberOfSites[static_cast<size_t>(site.kind)];
    }
    for (size_t kind = 0; kind < numberOfStubFrameKinds; ++kind) {
        if (!hasSingleReturnSite(static_cast<StubFrameKind>(kind)))
            continue;
        // None where nothing asked for the stub that has it.
        if (numberOfSites[kind] > 1)
            dataLogLn("frames of kind ", kind, " have ", numberOfSites[kind], " return sites");
        AOT_TEST_CHECK(numberOfSites[kind] <= 1);
    }
}

static void checkFrontEnds(const StubBlob& blob)
{
    for (const OperationFrontEnd& frontEnd : operationFrontEnds()) {
        bool isUsed = blob.entriesUsed.get(indexOf(frontEnd.operation));
        AOT_TEST_CHECK(blob.offsetOf(frontEnd.stub).has_value() == isUsed);
        AOT_TEST_CHECK(!isUsed || blob.entriesUsed.get(indexOf(frontEnd.raw)));
    }
}

static void checkBlob(const BuiltBlob& built)
{
    const StubBlob& blob = built.blob;
    AOT_TEST_CHECK(!blob.code.bytes.isEmpty());
    checkPlacements(blob);
    checkReturnSites(blob);
    for (StubLinkage linkage : { StubLinkage::EnteredByRuntime, StubLinkage::Handler }) {
        for (StubKey key : stubKeysWithLinkage(linkage))
            AOT_TEST_CHECK(blob.offsetOf(key));
    }
    checkFrontEnds(blob);

    AOT_TEST_CHECK(built.statistic(StatisticID::StubBytes) == blob.code.bytes.size());
    uint64_t numberOfBodies = built.statistic(StatisticID::StubsGenerated);
    AOT_TEST_CHECK(numberOfBodies && numberOfBodies <= blob.placements.size());
}

static void testStubBlobOfEveryKey()
{
    Vector<StubKey> calledKeys = stubKeysWithLinkage(StubLinkage::CalledByCode);
    for (const StubOptions& options : stubOptionVariants()) {
        BuiltBlob built = buildWithEveryKey(options);
        checkBlob(built);
        for (StubKey key : calledKeys)
            AOT_TEST_CHECK(built.blob.offsetOf(key));
    }
}

static void testStubBlobOfNoKey()
{
    BuiltBlob built = buildWithNoKey(StubOptions { });
    checkBlob(built);
    bool omitsCalledKey = stubKeysWithLinkage(StubLinkage::CalledByCode).containsIf([&](StubKey key) {
        return !built.blob.offsetOf(key);
    });
    AOT_TEST_CHECK(omitsCalledKey);
    AOT_TEST_CHECK(built.blob.code.bytes.size() < buildWithEveryKey(StubOptions { }).blob.code.bytes.size());
}

static void testStubBlobFrontEnds()
{
    for (const OperationFrontEnd& frontEnd : operationFrontEnds()) {
        EntrySet entriesUsedByFunctions;
        entriesUsedByFunctions.set(indexOf(frontEnd.operation));
        BuiltBlob built = build(StubOptions { }, { }, { }, entriesUsedByFunctions);
        AOT_TEST_CHECK(built.blob.offsetOf(frontEnd.stub));
        checkBlob(built);
    }
}

static void testStubBlobBoundImmediates()
{
    Entry entry = operationFrontEnds()[0].raw;
    StubKey methodCall = callMethodById(1).withImmediate(nameWithReadStub);
    StubKey read = getById().withImmediate(nameWithReadStub);
    StubKey operationCall = StubKey(StubFamily::CallOperation).withImmediate(offsetInRuntimeTable(entry));
    StubKey cell = builtinCell().withImmediate(builtinCellNumber);
    std::array calledKeys { methodCall, read, operationCall, cell };
    std::ranges::sort(calledKeys);

    BuiltBlob built = build(StubOptions { }, createBindingPolicy(), calledKeys, EntrySet { });
    checkBlob(built);
    for (StubKey key : calledKeys) {
        AOT_TEST_CHECK(built.blob.offsetOf(key));
        AOT_TEST_CHECK(built.blob.offsetOf(key.withoutImmediate()));
        AOT_TEST_CHECK(built.blob.offsetOf(key) != built.blob.offsetOf(key.withoutImmediate()));
    }
    AOT_TEST_CHECK(!built.blob.offsetOf(putById(PutMode::Strict).withImmediate(nameWithStoreStub)));
    AOT_TEST_CHECK(built.blob.entriesUsed.get(indexOf(entry)));
}

static void testReproducibleStubBlobs()
{
    BuiltBlob first = buildWithEveryKey(StubOptions { });
    BuiltBlob other = buildWithNoKey(createValidatingOptions());
    BuiltBlob second = buildWithEveryKey(StubOptions { });
    AOT_TEST_CHECK(areEqual(first.blob, second.blob));
    AOT_TEST_CHECK(!areEqual(first.blob, other.blob));
}

static void testStubBlobLayoutSeed()
{
    StubOptions seededOptions;
    seededOptions.layoutSeed = 1;
    BuiltBlob unseeded = buildWithEveryKey(StubOptions { });
    BuiltBlob seeded = buildWithEveryKey(seededOptions);
    BuiltBlob seededAgain = buildWithEveryKey(seededOptions);
    for (const BuiltBlob* built : { &unseeded, &seeded, &seededAgain })
        checkBlob(*built);

    AOT_TEST_CHECK(areEqual(seeded.blob, seededAgain.blob));
    AOT_TEST_CHECK(std::ranges::equal(unseeded.blob.placements, seeded.blob.placements, { }, &StubPlacement::key, &StubPlacement::key));
    AOT_TEST_CHECK(!std::ranges::equal(unseeded.blob.placements, seeded.blob.placements, { }, &StubPlacement::offset, &StubPlacement::offset));
}

template<typename Emit>
static Vector<uint8_t> assemble(BuildContext& context, const Emit& emit)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    emit(jit);
    auto code = recorder.finalize(LinkBuffer::Profile::Thunk, &context);
    AOT_TEST_CHECK(code.has_value() && !code->bytes.isEmpty() && code->relocations.isEmpty());
    return WTF::move(code->bytes);
}

template<typename Emit>
static CodeBody assembleCalls(BuildContext& context, const StubCatalog& catalog, const Emit& emit)
{
    CCallHelpers jit;
    RelocationRecorder recorder(jit, context.assemblerPolicy());
    CodeUnitRecorder units;
    StubCalls calls(catalog, recorder, units);
    emit(jit, calls);
    auto code = recorder.finalize(LinkBuffer::Profile::Thunk, &context);
    AOT_TEST_CHECK(code.has_value());

    CodeBody body;
    body.bytes = WTF::move(code->bytes);
    body.relocations = WTF::move(code->relocations);
    calls.describeCalls(body.relocations.span(), body);
    return body;
}

static std::span<const uint8_t> slice(const Vector<uint8_t>& bytes, size_t begin, size_t end)
{
    AOT_TEST_CHECK(begin <= end && end <= bytes.size());
    return bytes.subspan(begin, end - begin);
}

static constexpr int32_t stackOffsetOfArgument = 16;
static constexpr int64_t constantArgument = 42;
static constexpr uint64_t hasImmediateInArgumentMoves = 1ull << 32;
static constexpr size_t numberOfHeaderWordsInArgumentMoves = 2;

static ArgumentMoves createArgumentMoves()
{
    ArgumentMoves moves;
    appendArgumentMove(moves, GPRInfo::argumentGPR1, ArgumentSource::Register, GPRInfo::argumentGPR2);
    appendArgumentMove(moves, GPRInfo::argumentGPR2, ArgumentSource::Register, GPRInfo::argumentGPR1);
    appendArgumentMove(moves, GPRInfo::argumentGPR3, ArgumentSource::Stack64, stackOffsetOfArgument);
    appendArgumentMove(moves, GPRInfo::argumentGPR4, ArgumentSource::Constant, constantArgument);
    return moves;
}

static Vector<uint8_t> assembleExpectedMoves(BuildContext& context, std::optional<GPRReg> immediateRegister)
{
    return assemble(context, [&](CCallHelpers& jit) {
        jit.swap(GPRInfo::argumentGPR1, GPRInfo::argumentGPR2);
        jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, stackOffsetOfArgument), GPRInfo::argumentGPR3);
        jit.move(CCallHelpers::TrustedImm64(constantArgument), GPRInfo::argumentGPR4);
        if (immediateRegister)
            jit.move(CCallHelpers::TrustedImm32(nameWithoutStub), *immediateRegister);
    });
}

static void checkArgumentMoves(const ArgumentMoves& list, StubKey key, uint64_t immediateWord, const ArgumentMoves& moves)
{
    AOT_TEST_CHECK(list.size() == numberOfHeaderWordsInArgumentMoves + moves.size());
    AOT_TEST_CHECK(list[0] == key.bits() && list[1] == immediateWord);
    AOT_TEST_CHECK(equalSpans(list.subspan(numberOfHeaderWordsInArgumentMoves), moves.span()));
}

static void checkCallsOfArgumentThunks(const CodeBody& body, std::span<const OptionalIndex> sites)
{
    AOT_TEST_CHECK(body.stubKeys.isEmpty() && body.callees.isEmpty() && body.firstReadSites.isEmpty());
    AOT_TEST_CHECK(body.bytes.size() == sites.size() * sizeOfNearCall);
    AOT_TEST_CHECK(body.relocations.size() == sites.size() && body.returnSites.size() == sites.size() && body.argumentMoves.size() == sites.size());
    for (uint32_t i = 0; i < sites.size(); ++i) {
        uint32_t offset = static_cast<uint32_t>(i * sizeOfNearCall);
        AOT_TEST_CHECK(body.relocations[i] == (Relocation { offset, RelocationKind::Call, SymbolSpace::ArgumentThunk, primaryEntry, i }));
        AOT_TEST_CHECK(body.returnSites[i].offset == offset + sizeOfNearCall && body.returnSites[i].site == sites[i]);
    }
}

static void checkArgumentThunk(const ArgumentThunks& thunks, size_t index, uint32_t indexOfKey, const Vector<uint8_t>& expectedMoves)
{
    const Relocation& jump = thunks.code.relocations[index];
    AOT_TEST_CHECK(jump == (Relocation { jump.offset, RelocationKind::Jump, SymbolSpace::Stub, primaryEntry, indexOfKey }));
    AOT_TEST_CHECK(equalSpans(slice(thunks.code.bytes, thunks.offsets[index], jump.offset), expectedMoves.span()));
}

static void testArgumentThunks()
{
    BuildContext context(makeDefaultCompileOptions());
    StubCatalog catalog(StubOptions { }, createBindingPolicy());
    StubKey key(StubFamily::GetByVal);
    StubKey keyWithImmediate = getById();
    ArgumentMoves moves = createArgumentMoves();
    constexpr std::array sites { OptionalIndex { 1 }, OptionalIndex { 2 }, OptionalIndex { } };

    CodeBody body = assembleCalls(context, catalog, [&](CCallHelpers& jit, StubCalls& calls) {
        calls.call(jit, key, std::nullopt, sites[0], moves);
        calls.call(jit, keyWithImmediate, nameWithoutStub, sites[1], moves);
        calls.call(jit, keyWithImmediate, nameWithReadStub, sites[2], moves);
    });
    checkCallsOfArgumentThunks(body, sites);
    const ArgumentMoves& list = body.argumentMoves[0];
    const ArgumentMoves& listWithImmediate = body.argumentMoves[1];
    const ArgumentMoves& listWithBoundImmediate = body.argumentMoves[2];
    checkArgumentMoves(list, key, 0, moves);
    checkArgumentMoves(listWithImmediate, keyWithImmediate, hasImmediateInArgumentMoves | nameWithoutStub, moves);
    checkArgumentMoves(listWithBoundImmediate, keyWithImmediate.withImmediate(nameWithReadStub), 0, moves);

    Vector<uint8_t> expectedMoves = assembleExpectedMoves(context, std::nullopt);
    Vector<uint8_t> expectedMovesWithImmediate = assembleExpectedMoves(context, catalog.summaryOf(keyWithImmediate).signature.immediate);
    AOT_TEST_CHECK(expectedMoves.size() < expectedMovesWithImmediate.size());

    auto thunksOfOneKey = generateArgumentThunks(context, catalog, { &list, &list });
    AOT_TEST_CHECK(thunksOfOneKey.has_value());
    AOT_TEST_CHECK(thunksOfOneKey->offsets.size() == 2 && thunksOfOneKey->code.relocations.size() == 2);
    AOT_TEST_CHECK(thunksOfOneKey->stubKeys == Vector<StubKey> { key });
    for (size_t i = 0; i < 2; ++i)
        checkArgumentThunk(*thunksOfOneKey, i, 0, expectedMoves);

    auto thunks = generateArgumentThunks(context, catalog, { &listWithImmediate, &list, &listWithBoundImmediate, &listWithImmediate });
    AOT_TEST_CHECK(thunks.has_value());
    AOT_TEST_CHECK(thunks->offsets.size() == 4 && thunks->code.relocations.size() == 4);
    AOT_TEST_CHECK((thunks->stubKeys == Vector<StubKey> { keyWithImmediate, key, keyWithImmediate.withImmediate(nameWithReadStub) }));
    checkArgumentThunk(*thunks, 0, 0, expectedMovesWithImmediate);
    checkArgumentThunk(*thunks, 1, 1, expectedMoves);
    checkArgumentThunk(*thunks, 2, 2, expectedMoves);
    checkArgumentThunk(*thunks, 3, 0, expectedMovesWithImmediate);

    auto noThunks = generateArgumentThunks(context, catalog, { });
    AOT_TEST_CHECK(noThunks.has_value());
    AOT_TEST_CHECK(noThunks->code.bytes.isEmpty() && noThunks->code.relocations.isEmpty() && noThunks->offsets.isEmpty() && noThunks->stubKeys.isEmpty());
    AOT_TEST_CHECK(!context.diagnostics().hasErrors());
}

std::span<const NativeTest> stubCatalogTests()
{
    static constexpr NativeTest tests[] = {
        { "stubs/summaries"_s, testStubSummaries },
        { "stubs/declared-columns-hold-in-summaries"_s, testDeclaredColumns },
        { "stubs/summaries-are-closed-over-references"_s, testClosureOverReferences },
        { "stubs/summaries-of-known-stubs"_s, testKnownStubSummaries },
        { "stubs/catalogs-are-equal"_s, testEqualStubCatalogs },
        { "stubs/immediate-binding"_s, testStubImmediateBinding },
        { "stubs/preludes-stay-within-summaries"_s, testStubPreludeClobbers },
        { "stubs/bound-bodies-stay-within-summaries"_s, testBoundBodies },
        { "stubs/regexp-literal-constants"_s, testRegExpLiteralConstants },
        { "stubs/closed-clobbers-of-hand-written-bodies"_s, testClosedClobbers },
        { "stubs/blob-of-every-key"_s, testStubBlobOfEveryKey },
        { "stubs/blob-of-no-key"_s, testStubBlobOfNoKey },
        { "stubs/blob-front-ends"_s, testStubBlobFrontEnds },
        { "stubs/blob-bound-immediates"_s, testStubBlobBoundImmediates },
        { "stubs/blobs-are-reproducible"_s, testReproducibleStubBlobs },
        { "stubs/blob-layout-seed"_s, testStubBlobLayoutSeed },
        { "stubs/argument-thunks"_s, testArgumentThunks },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

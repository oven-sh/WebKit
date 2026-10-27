/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTImageTestSupport.h"
#include "AOTInstance.h"
#include "AOTStubRunner.h"
#include "AOTStubTestSupport.h"
#include "AOTStubsArrays.h"
#include "AOTStubsBarriers.h"
#include "AOTStubsBoxedCalls.h"
#include "AOTStubsCallAdapters.h"
#include "AOTStubsCallHost.h"
#include "AOTStubsCallLists.h"
#include "AOTStubsCalls.h"
#include "AOTStubsCollections.h"
#include "AOTStubsCoroutines.h"
#include "AOTStubsElements.h"
#include "AOTStubsExceptions.h"
#include "AOTStubsHashTables.h"
#include "AOTStubsOperations.h"
#include "AOTStubsPolls.h"
#include "AOTStubsPrimitiveFastPaths.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTStubsRegExp.h"
#include "AOTStubsStrings.h"
#include "AOTStubsValues.h"
#include "AOTVector.h"
#include "Butterfly.h"
#include "IndexingType.h"
#include "JSCJSValueInlines.h"
#include "JSObject.h"
#include "PureNaN.h"
#include <algorithm>
#include <array>
#include <bit>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

using EntrySet = WTF::BitSet<numberOfEntries>;

static constexpr GPRReg R0 = gprOf(StubRegister::R0);
static constexpr GPRReg A2 = gprOf(StubRegister::A2);
static constexpr GPRReg A3 = gprOf(StubRegister::A3);
static constexpr GPRReg T9 = gprOf(StubRegister::T9);
static constexpr GPRReg T12 = gprOf(StubRegister::T12);
static constexpr GPRReg T13 = gprOf(StubRegister::T13);

static constexpr std::array stepChecks { GeneratorStepCheck::IsUnmodifiedGenerator, GeneratorStepCheck::IsGenerator, GeneratorStepCheck::IsUnmodified, GeneratorStepCheck::None };
static_assert(stepChecks.size() == descriptorOf(StubFamily::StepGenerator).numberOfForms);
static constexpr std::array stepConsumers { StepConsumer::AsyncFunction, StepConsumer::AsyncGenerator, StepConsumer::AsyncFunctionAwaitingCall, StepConsumer::AsyncGeneratorAwaitingCall };
static_assert(stepConsumers.size() == descriptorOf(StubFamily::StepAsyncGenerator).numberOfForms);
static constexpr std::array generatorKinds { NewGeneratorKind::Sync, NewGeneratorKind::Async };
static_assert(generatorKinds.size() == descriptorOf(StubFamily::NewGenerator).numberOfForms);

static StubBlob buildBlob(const StubCatalog& catalog, std::span<const StubKey> calledKeys, const EntrySet& entriesUsedByFunctions)
{
    BuildContext context(makeDefaultCompileOptions());
    auto blob = buildStubBlob(context, catalog, calledKeys, entriesUsedByFunctions);
    AOT_TEST_CHECK(blob.has_value());
    return WTF::move(*blob);
}

static StubBlob buildFullBlob(const StubCatalog& catalog)
{
    EntrySet everyEntry;
    everyEntry.setAll();
    return buildBlob(catalog, stubKeysWithLinkage(StubLinkage::CalledByCode).span(), everyEntry);
}

static StubKey bodyContaining(const StubBlob& blob, uint32_t offset)
{
    const StubPlacement* containing = nullptr;
    for (const StubPlacement& placement : blob.placements) {
        if (placement.offset <= offset && (!containing || placement.offset > containing->offset))
            containing = &placement;
    }
    AOT_TEST_CHECK(containing);
    return bodyOf(containing->key);
}

static uint32_t sizeOfBody(const StubBlob& blob, StubKey body)
{
    AOT_TEST_CHECK(entriesOf(body).size() == 1);
    std::optional<uint32_t> start = blob.offsetOf(body);
    AOT_TEST_CHECK(start);
    uint32_t end = static_cast<uint32_t>(blob.code.bytes.size());
    for (const StubPlacement& placement : blob.placements) {
        if (placement.offset > *start)
            end = std::min(end, placement.offset);
    }
    return end - *start;
}

static unsigned numberOfSites(const StubBlob& blob, StubFrameKind kind)
{
    return static_cast<unsigned>(std::ranges::count(blob.returnSites, kind, &StubReturnSite::kind));
}

static Vector<StubKey> bodiesContainingSites(const StubBlob& blob, StubFrameKind kind)
{
    Vector<StubKey> bodies;
    for (const StubReturnSite& site : blob.returnSites) {
        if (site.kind == kind)
            bodies.append(bodyContaining(blob, site.offset));
    }
    return bodies;
}

static const Vector<StubReference, 8>& referencesOf(const StubCatalog& catalog, StubKey from)
{
    return catalog.recordingOf(bodyOf(from)).references;
}

static unsigned numberOfReferences(const StubCatalog& catalog, StubKey from, StubKey target)
{
    return static_cast<unsigned>(std::ranges::count(referencesOf(catalog, from), target, &StubReference::target));
}

static bool refersToFamily(const StubCatalog& catalog, StubKey from, StubFamily family)
{
    return referencesOf(catalog, from).containsIf([&](const StubReference& reference) {
        return reference.target.family() == family;
    });
}

static bool refersToBody(const StubCatalog& catalog, StubKey from, StubKey body)
{
    return referencesOf(catalog, from).containsIf([&](const StubReference& reference) {
        return bodyOf(reference.target) == body;
    });
}

static bool refersOnlyBy(const StubCatalog& catalog, StubKey from, StubKey target, StubReference::Kind kind)
{
    return std::ranges::all_of(referencesOf(catalog, from), [&](const StubReference& reference) {
        return reference.target != target || reference.kind == kind;
    });
}

static bool refersOnceBy(const StubCatalog& catalog, StubKey from, StubKey target, StubReference::Kind kind)
{
    return numberOfReferences(catalog, from, target) == 1 && refersOnlyBy(catalog, from, target, kind);
}

static RegisterSet withoutMacroScratch(RegisterSet registers)
{
    return registers.exclude(RegisterSet::macroClobberedGPRs()).exclude(RegisterSet::macroClobberedFPRs());
}

static RegisterSet setOf(std::initializer_list<GPRReg> registers)
{
    RegisterSet set;
    for (GPRReg reg : registers) {
        if (reg != InvalidGPRReg)
            set.add(reg);
    }
    return set;
}

static bool clobbersAtMost(const StubSummary& summary, std::initializer_list<GPRReg> registers)
{
    return setOf(registers).subsumes(withoutMacroScratch(summary.clobbered));
}

static bool clobbersFPR(const StubSummary& summary)
{
    return withoutMacroScratch(summary.clobbered).numberOfSetFPRs();
}

static bool clobbersOperandOf(const StubSummary& summary, const StubSignature& signature)
{
    auto operands = std::span(signature.operands).first(signature.numberOfOperands);
    return std::ranges::any_of(operands, [&](Reg operand) {
        return containsRegister(summary.clobbered, operand);
    });
}

static bool containsArgumentGPR(const RegisterSet& registers)
{
    for (unsigned i = 0; i < numberOfArgumentGPRs; ++i) {
        if (containsRegister(registers, argumentGPR(i)))
            return true;
    }
    return false;
}

static bool mayThrowAndCollect(const StubCatalog& catalog, StubKey key)
{
    Typed::Effects effects = catalog.effectsOf(key);
    return effects.mayThrow && effects.mayCollect;
}

static bool neitherThrowsNorCollects(const StubCatalog& catalog, StubKey key)
{
    Typed::Effects effects = catalog.effectsOf(key);
    return !effects.mayThrow && !effects.mayCollect;
}

static EntrySet entrySetOf(std::initializer_list<Entry> entries)
{
    EntrySet set;
    for (Entry entry : entries)
        set.set(indexOf(entry));
    return set;
}

static const EntrySet& operationsCalledBy(const StubCatalog& catalog, StubKey key)
{
    return catalog.recordingOf(bodyOf(key)).operationsCalled;
}

static void checkEntryOffsets(const StubBlob& blob, const Vector<StubKey>& bodies)
{
    for (StubKey body : bodies) {
        if (!blob.offsetOf(body))
            continue;
        for (StubKey entry : entriesOf(body)) {
            std::optional<uint32_t> offset = blob.offsetOf(entry);
            AOT_TEST_CHECK(offset && *offset < blob.code.bytes.size());
        }
    }
}

static void checkDistinctBodyOffsets(const StubBlob& blob)
{
    Vector<StubPlacement> placements = blob.placements;
    std::ranges::sort(placements, { }, &StubPlacement::offset);
    for (size_t i = 1; i < placements.size(); ++i) {
        if (placements[i - 1].offset == placements[i].offset)
            AOT_TEST_CHECK(bodyOf(placements[i - 1].key) == bodyOf(placements[i].key));
    }
}

static void testEntryPlacements()
{
    Vector<StubKey> bodies = allStubBodies();
    for (const StubOptions& options : stubOptionVariants()) {
        StubCatalog catalog(options, StubPolicy { });
        StubBlob blob = buildFullBlob(catalog);
        checkEntryOffsets(blob, bodies);
        checkDistinctBodyOffsets(blob);
        for (StubKey key : { concatenate(2), constructValue(ConstructSyntax::NewExpression), constructValue(ConstructSyntax::SuperCall), stepGenerator(GeneratorStepCheck::None) })
            AOT_TEST_CHECK(blob.offsetOf(key));
    }
}

static void checkSingleReturnSites(const StubBlob& blob)
{
    constexpr std::pair<StubFrameKind, StubKey> singleSiteBodies[] = {
        { StubFrameKind::Adapter, enterClosure() },
        { StubFrameKind::ConstructAdapter, enterClosureForConstruct() },
        { StubFrameKind::SavedFrameAdapter, enterSavedFrame() },
        { StubFrameKind::AsyncGeneratorBody, runAsyncGeneratorStep() },
        { StubFrameKind::ArgumentList, callFromArgumentListFrame() },
    };
    for (auto [kind, body] : singleSiteBodies) {
        AOT_TEST_CHECK(hasSingleReturnSite(kind));
        AOT_TEST_CHECK(numberOfSites(blob, kind) == 1);
        AOT_TEST_CHECK(bodiesContainingSites(blob, kind)[0] == body);
    }

    std::optional<uint32_t> offsetOfListFrameReturn = blob.offsetOf(returnFromArgumentListFrame());
    AOT_TEST_CHECK(offsetOfListFrameReturn && *offsetOfListFrameReturn > *blob.offsetOf(callFromArgumentListFrame()));
    AOT_TEST_CHECK(blob.returnSites.containsIf([&](const StubReturnSite& site) {
        return site.kind == StubFrameKind::ArgumentList && site.offset == *offsetOfListFrameReturn;
    }));
}

static void checkGeneratorBodySites(const StubCatalog& catalog, const StubBlob& blob)
{
    Vector<StubKey> bodiesEnteringGenerators;
    for (GeneratorStepCheck check : stepChecks)
        bodiesEnteringGenerators.append(stepGenerator(check));
    std::ranges::sort(bodiesEnteringGenerators);

    for (StubKey body : allStubBodies()) {
        auto& returnLabels = catalog.recordingOf(body).returnLabels;
        if (bodiesEnteringGenerators.contains(body))
            AOT_TEST_CHECK(returnLabels.size() == 1 && returnLabels[0] == StubFrameKind::GeneratorBody);
        else
            AOT_TEST_CHECK(!returnLabels.contains(StubFrameKind::GeneratorBody));
        if (body == runAsyncGeneratorStep())
            AOT_TEST_CHECK(returnLabels.size() == 1 && returnLabels[0] == StubFrameKind::AsyncGeneratorBody);
        else
            AOT_TEST_CHECK(!returnLabels.contains(StubFrameKind::AsyncGeneratorBody));
    }

    Vector<StubKey> siteBodies = bodiesContainingSites(blob, StubFrameKind::GeneratorBody);
    std::ranges::sort(siteBodies);
    AOT_TEST_CHECK(siteBodies.size() == stepChecks.size() && siteBodies == bodiesEnteringGenerators);
}

static void testReturnSites()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    StubBlob blob = buildFullBlob(catalog);
    checkSingleReturnSites(blob);
    checkGeneratorBodySites(catalog, blob);
    AOT_TEST_CHECK(numberOfSites(blob, StubFrameKind::HostCall) >= 2);
}

static void checkEntries(const StubCatalog& catalog, StubKey body, std::initializer_list<StubKey> expectedEntries)
{
    Vector<StubKey, 4> entries = catalog.recordingOf(body).entries;
    std::ranges::sort(entries);
    Vector<StubKey> expected(expectedEntries);
    std::ranges::sort(expected);
    AOT_TEST_CHECK(entries == expected);
}

static void testBodyEntries()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    checkEntries(catalog, getByVal(false), { getByVal(false), getByVal(true) });
    checkEntries(catalog, getByValCached(), { getByValCached(), getByValCachedCells() });
    checkEntries(catalog, enterClosure(), { enterClosure(), enterSlimClosure(), enterModuleBody() });
    checkEntries(catalog, runAsyncGeneratorStep(), { runAsyncGeneratorStep(), stepAsyncIteratorInEngine() });
    for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count) {
        checkEntries(catalog, callValue(count), { callValue(count), callValueWithoutThis(count), callValueCell(count), callValueCellWithoutThis(count), callValueNotClosure(count) });
        if (exists(callHostFunctionWithoutBoxedCall(count)))
            checkEntries(catalog, callHostFunction(count), { callHostFunction(count), callHostFunctionWithoutBoxedCall(count) });
        else
            checkEntries(catalog, callHostFunction(count), { callHostFunction(count) });
    }
    checkEntries(catalog, callValueWithCount(), { callValueWithCount(), callCellWithCount() });
    checkEntries(catalog, arrayPush(), { arrayPush(), arrayPushOfKnownKind() });
    checkEntries(catalog, arrayPushDouble(), { arrayPushDouble() });

    StubBlob blob = buildFullBlob(catalog);
    std::optional<uint32_t> offsetOfFirstEntry = blob.offsetOf(getByValCached());
    std::optional<uint32_t> offsetOfCellsEntry = blob.offsetOf(getByValCachedCells());
    AOT_TEST_CHECK(offsetOfFirstEntry && offsetOfCellsEntry && *offsetOfFirstEntry < *offsetOfCellsEntry);
}

static bool isSlotHandler(StubKey key)
{
    return key.family() == StubFamily::ReadSlot || key.family() == StubFamily::StoreSlot;
}

static void checkHandlerReferences(const StubCatalog& catalog, StubKey from, SlotHandlerTable table)
{
    for (StubKey handler : slotHandlerKeys(table))
        AOT_TEST_CHECK(refersOnceBy(catalog, from, handler, StubReference::Kind::Jump));
    for (const StubReference& reference : referencesOf(catalog, from))
        AOT_TEST_CHECK(!isSlotHandler(reference.target) || handlerTableOf(reference.target) == table);
}

static void checkKeyedAccessReferences(const StubCatalog& catalog)
{
    AOT_TEST_CHECK(refersOnceBy(catalog, getByValCached(), getByVal(false), StubReference::Kind::Jump));
    AOT_TEST_CHECK(refersOnceBy(catalog, putByValCached(PutMode::Strict), putByVal(false, ECMAMode::strict()), StubReference::Kind::Jump));
    AOT_TEST_CHECK(refersOnceBy(catalog, putByValCached(PutMode::Sloppy), putByVal(false, ECMAMode::sloppy()), StubReference::Kind::Jump));
    AOT_TEST_CHECK(!refersToFamily(catalog, defineByValCached(), StubFamily::PutByVal));

    checkHandlerReferences(catalog, getByValCached(), SlotHandlerTable::ReadByVal);
    checkHandlerReferences(catalog, putByValCached(PutMode::Strict), SlotHandlerTable::StoreStrictByVal);
    checkHandlerReferences(catalog, putByValCached(PutMode::Sloppy), SlotHandlerTable::StoreSloppyByVal);
    checkHandlerReferences(catalog, defineByValCached(), SlotHandlerTable::DefineByVal);
}

static void checkResumptionReferences(const StubCatalog& catalog)
{
    AOT_TEST_CHECK(refersOnceBy(catalog, enterSavedFrame(), restoreSavedFrameToStack(), StubReference::Kind::Call));
    for (GeneratorStepCheck check : stepChecks)
        AOT_TEST_CHECK(refersOnceBy(catalog, stepGenerator(check), restoreSavedFrameToStack(), StubReference::Kind::Call));
    AOT_TEST_CHECK(refersOnceBy(catalog, runAsyncGeneratorStep(), restoreSavedFrameToStack(), StubReference::Kind::Call));
    for (StepConsumer consumer : stepConsumers) {
        AOT_TEST_CHECK(refersOnceBy(catalog, stepAsyncGenerator(consumer), runAsyncGeneratorStep(), StubReference::Kind::Jump));
        AOT_TEST_CHECK(refersOnceBy(catalog, stepAsyncGenerator(consumer), stepAsyncIteratorInEngine(), StubReference::Kind::Jump));
    }
}

static void checkExceptionReferences(const StubCatalog& catalog)
{
    for (StubKey from : { throwStackOverflow(false), handleExceptionAtPrologue(), pollBeforeEntering(PollBeforeEnteringForm::Plain), pollBeforeEntering(PollBeforeEnteringForm::FunctionObject) })
        AOT_TEST_CHECK(refersToBody(catalog, from, bodyOf(handleException(false))));
}

static void checkHostCallReferences(const StubCatalog& catalog)
{
    constexpr std::pair<BoxedCallForm, StubFamily> boxedCallTargets[] = {
        { BoxedCallForm::ArrayPush, StubFamily::ArrayPushOfKnownKind }, { BoxedCallForm::ArraySlice, StubFamily::ArraySlice },
        { BoxedCallForm::MapDelete, StubFamily::MapDelete }, { BoxedCallForm::MapGet, StubFamily::MapGet }, { BoxedCallForm::MapHas, StubFamily::MapHas }, { BoxedCallForm::MapSet, StubFamily::MapSet },
        { BoxedCallForm::SetAdd, StubFamily::SetAdd }, { BoxedCallForm::SetDelete, StubFamily::SetDelete }, { BoxedCallForm::SetHas, StubFamily::SetHas },
        { BoxedCallForm::StringIndexOf, StubFamily::StringIndexOf }, { BoxedCallForm::StringStartsWith, StubFamily::StringStartsWith },
        { BoxedCallForm::StringSlice, StubFamily::Substring }, { BoxedCallForm::StringToLowerCase, StubFamily::CallOperation },
        { BoxedCallForm::WeakMapGet, StubFamily::WeakMapGet }, { BoxedCallForm::WeakMapHas, StubFamily::WeakMapHas }, { BoxedCallForm::WeakMapSet, StubFamily::WeakMapSet },
        { BoxedCallForm::WeakSetHas, StubFamily::WeakSetHas },
    };
    for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count) {
        bool hasBoxedCalls = count >= BoxedCallEntry::minArgumentCount && count <= BoxedCallEntry::maxArgumentCount;
        AOT_TEST_CHECK(refersToFamily(catalog, callHostFunction(count), StubFamily::CallBoxedBuiltin) == hasBoxedCalls);
        if (!hasBoxedCalls)
            continue;
        for (auto [form, family] : boxedCallTargets) {
            StubKey handler = callBoxedBuiltin(form, count);
            AOT_TEST_CHECK(refersOnceBy(catalog, callHostFunction(count), handler, StubReference::Kind::Jump));
            AOT_TEST_CHECK(refersToFamily(catalog, handler, family) == acceptsArgumentCount(form, count));
            AOT_TEST_CHECK(refersOnceBy(catalog, handler, callHostFunctionWithoutBoxedCall(count), StubReference::Kind::Jump));
        }
    }
}

static void checkMethodCallReferences(const StubCatalog& catalog)
{
    StubOptions validatingOptions;
    validatingOptions.validates = true;
    StubOptions wideOptions;
    wideOptions.useWideReferences = true;
    StubCatalog validatingCatalog(validatingOptions, StubPolicy { });
    StubCatalog wideCatalog(wideOptions, StubPolicy { });
    for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count) {
        AOT_TEST_CHECK(refersOnceBy(catalog, callMethodById(count), callFromArgumentListFrame(), StubReference::Kind::Jump));
        AOT_TEST_CHECK(!numberOfReferences(validatingCatalog, callMethodById(count), callFromArgumentListFrame()));
        AOT_TEST_CHECK(!numberOfReferences(wideCatalog, callMethodById(count), callFromArgumentListFrame()));
    }
}

static void testReferences()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    checkKeyedAccessReferences(catalog);
    checkResumptionReferences(catalog);
    checkExceptionReferences(catalog);
    checkHostCallReferences(catalog);
    checkMethodCallReferences(catalog);
}

static void testReachability()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });

    StubBlob keyedReadBlob = buildBlob(catalog, std::array { getByValCached() }, EntrySet { });
    for (StubKey key : { getByValCached(), getByValCachedCells(), getByVal(false), getByVal(true) })
        AOT_TEST_CHECK(keyedReadBlob.offsetOf(key));
    AOT_TEST_CHECK(!keyedReadBlob.offsetOf(newPromise()));

    StubBlob promiseBlob = buildBlob(catalog, std::array { newPromise() }, EntrySet { });
    AOT_TEST_CHECK(promiseBlob.offsetOf(newPromise()));
    AOT_TEST_CHECK(promiseBlob.entriesUsed.get(indexOf(Entry::operationAOTNewPromise)));
    for (StubKey key : { stepGenerator(GeneratorStepCheck::None), getByValCached(), getByVal(false), newMap() })
        AOT_TEST_CHECK(!promiseBlob.offsetOf(key));

    for (StubLinkage linkage : { StubLinkage::EnteredByRuntime, StubLinkage::Handler }) {
        for (StubKey key : stubKeysWithLinkage(linkage))
            AOT_TEST_CHECK(promiseBlob.offsetOf(key));
    }
}

static void testMissingKeys()
{
    for (StubKey key : { newPromise(), toNumberAsDouble(), substring(), arraySlice(), arrayPush(), stringsAreEqual() })
        AOT_TEST_CHECK(exists(key) && !exists(key.withCallerFrame(CallerFrame::Frameless)));
    for (CallerFrame callerFrame : { CallerFrame::HasFrame, CallerFrame::Frameless }) {
        AOT_TEST_CHECK(exists(pollAtLoop(callerFrame)) == isX86_64());
        AOT_TEST_CHECK(exists(throwAtSite(ThrowSiteSource::FaultSite, callerFrame)));
    }

    AOT_TEST_CHECK(!exists(StubKey(StubFamily::StepGenerator).withForm(4)));
    AOT_TEST_CHECK(!exists(StubKey(StubFamily::AwaitBySavingFrame).withForm(4)));
    AOT_TEST_CHECK(!exists(StubKey(StubFamily::StepAsyncGenerator).withForm(4)));
    AOT_TEST_CHECK(!exists(StubKey(StubFamily::NewGenerator).withForm(2)));
    AOT_TEST_CHECK(!exists(callHostFunctionWithoutBoxedCall(BoxedCallEntry::maxArgumentCount).withCount(BoxedCallEntry::maxArgumentCount + 1)));
}

static void checkAwaitSummary(const StubCatalog& catalog, StubKey key, GPRReg awaited)
{
    const StubSignature& signature = catalog.summaryOf(key).signature;
    AOT_TEST_CHECK(mayThrowAndCollect(catalog, key));
    AOT_TEST_CHECK(signature.numberOfOperands == 2);
    AOT_TEST_CHECK(signature.operands[0] == Reg(awaited));
    AOT_TEST_CHECK(signature.operands[1] == Reg(T9));
    AOT_TEST_CHECK(signature.firstResult == Reg(GPRInfo::returnValueGPR));
}

static void checkCoroutineSummaries(const StubCatalog& catalog)
{
    checkAwaitSummary(catalog, awaitBySavingFrame(), A1);
    checkAwaitSummary(catalog, awaitCallBySavingFrame(), R0);
    checkAwaitSummary(catalog, awaitBySavingFrame(AwaitedOperand::ValueAfterTurn), A1);
    AOT_TEST_CHECK(mayThrowAndCollect(catalog, returnAsyncResult()));

    Typed::Effects effectsOfPromiseCreation = catalog.effectsOf(newPromise());
    AOT_TEST_CHECK(!effectsOfPromiseCreation.mayThrow && effectsOfPromiseCreation.mayCollect);
    AOT_TEST_CHECK(clobbersAtMost(catalog.summaryOf(newPromise()), { R0, T9, T11 }));

    for (GeneratorStepCheck check : stepChecks) {
        const StubSignature& signature = catalog.summaryOf(stepGenerator(check)).signature;
        AOT_TEST_CHECK(signature.result == StubResult::TwoGPRs);
        AOT_TEST_CHECK(signature.secondResult == Reg(T9));
        AOT_TEST_CHECK(catalog.effectsOf(stepGenerator(check)).mayThrow);
    }
}

static void checkNumberConversionSummary(const StubCatalog& catalog)
{
    const StubSummary& summary = catalog.summaryOf(toNumberAsDouble());
    AOT_TEST_CHECK(containsRegister(summary.clobbered, T11));
    AOT_TEST_CHECK(containsRegister(summary.clobbered, summary.signature.firstResult));
    AOT_TEST_CHECK(!containsRegister(summary.clobbered, A1));
}

static void checkSubstringSummary(const StubCatalog& catalog)
{
    const StubSummary& summary = catalog.summaryOf(substring());
    AOT_TEST_CHECK(summary.clobbered.subsumes(setOf({ R0, T9, T11, T12, T13, gprOf(isARM64() ? StubRegister::T14 : StubRegister::T10) })));
    for (GPRReg reg : { A1, A2, A3 })
        AOT_TEST_CHECK(!containsRegister(summary.clobbered, reg));
}

static void checkCallSummaries(const StubCatalog& catalog)
{
    const RegisterSet& clobberedByResolvedCall = catalog.summaryOf(callResolvedValue()).clobbered;
    for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count) {
        const StubSummary& summary = catalog.summaryOf(resolveCallValue(count));
        AOT_TEST_CHECK(withoutMacroScratch(summary.clobbered) == setOf({ T11, T13 }));
        AOT_TEST_CHECK(!containsArgumentGPR(summary.clobbered));
        AOT_TEST_CHECK(neitherThrowsNorCollects(catalog, resolveCallValue(count)));
        AOT_TEST_CHECK(!referencesOf(catalog, resolveCallValue(count)).isEmpty());
        for (const StubReference& reference : referencesOf(catalog, resolveCallValue(count))) {
            AOT_TEST_CHECK(reference.kind == StubReference::Kind::ReturnedAddress);
            AOT_TEST_CHECK(catalog.effectsOf(reference.target).mayThrow);
            AOT_TEST_CHECK(clobberedByResolvedCall.subsumes(catalog.summaryOf(reference.target).clobbered));
        }
    }
}

static void checkArrayCreationSummary(const StubCatalog& catalog, StubKey key, const StubSignature& largestLiteral)
{
    const StubSummary& summary = catalog.summaryOf(key);
    AOT_TEST_CHECK(!clobbersOperandOf(summary, largestLiteral));
    AOT_TEST_CHECK(!clobbersFPR(summary));
    AOT_TEST_CHECK(containsRegister(summary.clobbered, T9));
}

static void checkArraySummaries(const StubCatalog& catalog)
{
    constexpr std::array creationFamilies { StubFamily::ArrayLiteral, StubFamily::NewEmptyArray, StubFamily::NewEmptyArrayWithCapacity };
    constexpr unsigned maxCount = descriptorOf(StubFamily::ArrayLiteral).maxCount;
    const StubSignature& largestLiteral = catalog.summaryOf(arrayLiteral(maxCount)).signature;
    AOT_TEST_CHECK(largestLiteral.numberOfOperands == maxCount);
    for (StubKey key : allStubKeys()) {
        if (std::ranges::find(creationFamilies, key.family()) != creationFamilies.end())
            checkArrayCreationSummary(catalog, key, largestLiteral);
    }
    checkArrayCreationSummary(catalog, newEmptyArrayAtSite(ArrayElementKind::Contiguous), largestLiteral);

    for (StubKey key : { arrayPush(), arrayPushOfKnownKind() })
        AOT_TEST_CHECK(!clobbersFPR(catalog.summaryOf(key)));
    AOT_TEST_CHECK(!containsRegister(catalog.summaryOf(arrayPushDouble()).clobbered, FPRInfo::fpRegT0));
}

static void checkWeakMapGetSummary(const StubCatalog& catalog)
{
    constexpr HashTableRegisterPlan plan = hashTableRegisterPlan;
    const StubSummary& summary = catalog.summaryOf(weakMapGet());
    AOT_TEST_CHECK(neitherThrowsNorCollects(catalog, weakMapGet()));
    AOT_TEST_CHECK(withoutMacroScratch(summary.clobbered) == setOf({
        gprOf(plan.hash), gprOf(plan.table), gprOf(plan.cursor), gprOf(plan.keys), gprOf(plan.entry), gprOf(plan.candidate),
        plan.copyOfOwner ? gprOf(*plan.copyOfOwner) : InvalidGPRReg,
        summary.signature.firstResult.gpr(),
    }));
    AOT_TEST_CHECK(!containsRegister(summary.clobbered, argumentGPR(1)));
    AOT_TEST_CHECK(!clobbersFPR(summary));
}

static bool isLostInLeafCall(GPRReg reg)
{
    return reg == leafArgumentGPR(0) || reg == leafArgumentGPR(1) || containsRegister(registersClobberedByPreserveMostLeaves(), reg);
}

static void checkEqualitySummaries(const StubCatalog& catalog)
{
    const RegisterSet& clobberedByStrictEqual = catalog.summaryOf(equal(true)).clobbered;
    for (GPRReg reg : { A1, A2, A3, T13, gprOf(StubRegister::T14) })
        AOT_TEST_CHECK(!containsRegister(clobberedByStrictEqual, reg));

    const StubSummary& stringEquality = catalog.summaryOf(stringsAreEqual());
    AOT_TEST_CHECK(stringEquality.signature.operands[0] == Reg(T11) && stringEquality.signature.operands[1] == Reg(T12));
    for (GPRReg operand : { T11, T12 })
        AOT_TEST_CHECK(containsRegister(stringEquality.clobbered, operand) == isLostInLeafCall(operand));
}

static void checkBoundMethodCall()
{
    constexpr uint32_t name = 5;
    StubPolicy policy;
    policy.namesWithOwnStubByAccess[static_cast<size_t>(Typed::Access::Read)].set(name);
    StubCatalog catalog(StubOptions { }, WTF::move(policy));
    AOT_TEST_CHECK(catalog.bindsImmediate(callMethodById(2), name));
    AOT_TEST_CHECK(!catalog.bindsImmediate(callMethodById(2), name + 1));

    StubKey boundKey = callMethodById(2).withImmediate(name);
    AOT_TEST_CHECK(catalog.dryRun(boundKey).body == boundKey);
}

static void testSummaries()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    checkCoroutineSummaries(catalog);
    checkNumberConversionSummary(catalog);
    checkSubstringSummary(catalog);
    checkCallSummaries(catalog);
    checkArraySummaries(catalog);
    checkWeakMapGetSummary(catalog);
    checkEqualitySummaries(catalog);
    checkBoundMethodCall();
}

static void checkCoroutineOperations(const StubCatalog& catalog)
{
    Vector<StubKey> keys { awaitBySavingFrame(), awaitCallBySavingFrame(), returnAsyncResult(), newPromise() };
    for (GeneratorStepCheck check : stepChecks)
        keys.append(stepGenerator(check));

    EntrySet operationsCalled;
    for (StubKey key : keys)
        operationsCalled.merge(operationsCalledBy(catalog, key));
    AOT_TEST_CHECK(operationsCalled == entrySetOf({ Entry::operationAOTNewPromise, Entry::operationAOTSaveFrameAndAwait, Entry::operationAOTSaveFrameAndAwaitCall, Entry::operationAOTAwaitWithSavedFrame, Entry::operationAOTAwaitCallWithSavedFrame, Entry::operationAOTResolvePromiseOrReturnResult }));

    AOT_TEST_CHECK(operationsCalledBy(catalog, awaitBySavingFrame(AwaitedOperand::ValueAfterTurn)) == entrySetOf({ Entry::operationAOTSaveFrameAndAwaitAfterTurn, Entry::operationAOTAwaitWithSavedFrameAfterTurn }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, awaitBySavingFrame(AwaitedOperand::ValueInModuleBody)) == entrySetOf({ Entry::operationAOTSaveFrameAndAwaitInModuleBody, Entry::operationAOTAwaitWithSavedFrameInModuleBody }));
    for (StepConsumer consumer : stepConsumers)
        AOT_TEST_CHECK(operationsCalledBy(catalog, stepAsyncGenerator(consumer)).isEmpty());
    AOT_TEST_CHECK(operationsCalledBy(catalog, runAsyncGeneratorStep()) == entrySetOf({ Entry::operationAOTAsyncIteratorStep, Entry::operationAOTFinishAsyncIteratorStep }));
    for (NewGeneratorKind kind : generatorKinds)
        AOT_TEST_CHECK(operationsCalledBy(catalog, newGenerator(kind)) == entrySetOf({ Entry::operationAOTNewGenerator }));
}

static void checkCollectionOperations(const StubCatalog& catalog)
{
    AOT_TEST_CHECK(operationsCalledBy(catalog, mapSet(KeyComparison::String)) == entrySetOf({ Entry::operationAOTMapSet, Entry::operationAOTStringImplsAreEqual, Entry::operationAOTHashStringImpl }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, mapSet(KeyComparison::Bits)) == entrySetOf({ Entry::operationAOTMapSet }));
    AOT_TEST_CHECK(refersToBody(catalog, mapSet(KeyComparison::Bits), writeBarrier()));
    AOT_TEST_CHECK(refersToBody(catalog, mapSet(KeyComparison::Bits), newCompactTable(CompactTableOwner::Map)) && refersToBody(catalog, setAdd(KeyComparison::String), newCompactTable(CompactTableOwner::Set)));
    AOT_TEST_CHECK(operationsCalledBy(catalog, newCompactTable(CompactTableOwner::Map)) == entrySetOf({ Entry::operationAOTMapSet }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, newCompactTable(CompactTableOwner::Set)) == entrySetOf({ Entry::operationAOTSetAdd }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, mapDelete(KeyComparison::Bits)).isEmpty());
    AOT_TEST_CHECK(operationsCalledBy(catalog, setDelete(KeyComparison::String)) == entrySetOf({ Entry::operationAOTSetDelete, Entry::operationAOTStringImplsAreEqual, Entry::operationAOTHashStringImpl }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, writeBarrier()).get(indexOf(Entry::operationAOTBarrierSlow)));
    AOT_TEST_CHECK(operationsCalledBy(catalog, weakMapGet()).isEmpty());
}

static void checkAdapterOperations(const StubCatalog& catalog)
{
    AOT_TEST_CHECK(operationsCalledBy(catalog, enterClosureForConstruct()) == entrySetOf({ Entry::operationAOTCreateThis }));
    const EntrySet& calledForNewExpression = operationsCalledBy(catalog, constructValue(ConstructSyntax::NewExpression));
    const EntrySet& calledForSuperCall = operationsCalledBy(catalog, constructValue(ConstructSyntax::SuperCall));
    AOT_TEST_CHECK(calledForNewExpression.get(indexOf(Entry::operationAOTCreateThis)) && calledForNewExpression.get(indexOf(Entry::operationAOTConstruct)) && !calledForNewExpression.get(indexOf(Entry::operationAOTSuperConstruct)));
    AOT_TEST_CHECK(calledForSuperCall.get(indexOf(Entry::operationAOTCreateThis)) && calledForSuperCall.get(indexOf(Entry::operationAOTSuperConstruct)) && !calledForSuperCall.get(indexOf(Entry::operationAOTConstruct)));
    AOT_TEST_CHECK(operationsCalledBy(catalog, enterClosure()).isEmpty());
}

static void testOperationsCalled()
{
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    checkCoroutineOperations(catalog);
    checkCollectionOperations(catalog);
    checkAdapterOperations(catalog);
}

static void testWideReferences()
{
    StubOptions wideOptions;
    wideOptions.useWideReferences = true;
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    StubCatalog wideCatalog(wideOptions, StubPolicy { });

    constexpr StubKey store = mapSet(KeyComparison::Bits);
    StubBlob blob = buildBlob(catalog, std::array { store }, EntrySet { });
    StubBlob wideBlob = buildBlob(wideCatalog, std::array { store }, EntrySet { });
    AOT_TEST_CHECK(sizeOfBody(wideBlob, store) < sizeOfBody(blob, store));
    AOT_TEST_CHECK(operationsCalledBy(wideCatalog, store) == operationsCalledBy(catalog, store));
}

static bool haveSamePlacements(const StubBlob& first, const StubBlob& second)
{
    return std::ranges::equal(first.placements, second.placements, { }, &StubPlacement::key, &StubPlacement::key)
        && std::ranges::equal(first.placements, second.placements, { }, &StubPlacement::offset, &StubPlacement::offset);
}

static void testLayoutSeed()
{
    StubOptions seededOptions;
    seededOptions.layoutSeed = 1;
    StubCatalog catalog(StubOptions { }, StubPolicy { });
    StubCatalog seededCatalog(seededOptions, StubPolicy { });
    StubBlob blob = buildFullBlob(catalog);
    StubBlob seededBlob = buildFullBlob(seededCatalog);
    StubBlob repeatedBlob = buildFullBlob(seededCatalog);

    AOT_TEST_CHECK(seededBlob.code.bytes == repeatedBlob.code.bytes);
    AOT_TEST_CHECK(haveSamePlacements(seededBlob, repeatedBlob));
    AOT_TEST_CHECK(std::ranges::equal(seededBlob.returnSites, repeatedBlob.returnSites, { }, &StubReturnSite::offset, &StubReturnSite::offset));

    AOT_TEST_CHECK(std::ranges::equal(blob.placements, seededBlob.placements, { }, &StubPlacement::key, &StubPlacement::key));
    AOT_TEST_CHECK(!haveSamePlacements(blob, seededBlob));
    AOT_TEST_CHECK(blob.entriesUsed == seededBlob.entriesUsed);
    for (unsigned kind = 0; kind < numberOfStubFrameKinds; ++kind)
        AOT_TEST_CHECK(numberOfSites(blob, static_cast<StubFrameKind>(kind)) == numberOfSites(seededBlob, static_cast<StubFrameKind>(kind)));
    checkDistinctBodyOffsets(seededBlob);
}

static unsigned numberOfRegExpCalls(const StubBlob& blob, StubKey thunk, uint16_t entry)
{
    CodeSymbol symbol { SymbolSpace::RegExp, *thunk.immediate(), entry };
    return static_cast<unsigned>(std::ranges::count_if(blob.code.relocations, [&](const Relocation& relocation) {
        return relocation.kind == RelocationKind::Call && relocation.codeSymbol() == symbol && bodyContaining(blob, relocation.offset) == thunk;
    }));
}

static void testRegExpRelocations()
{
    constexpr uint32_t recordForBothWidths = 0;
    constexpr uint32_t recordFor8BitSubjects = 1;
    constexpr uint16_t entryFor8BitSubjects = 0;
    constexpr uint16_t entryFor16BitSubjects = 1;
    constexpr StubKey test = regExpMatchLiteral(Typed::RegExpMatchForm::Test, false);
    constexpr StubKey globalExec = regExpMatchLiteral(Typed::RegExpMatchForm::Exec, true);

    StubPolicy policy;
    policy.regExpLiterals = { RegExpLiteralConstants { .matchesAtCodePointBoundaries = false }, RegExpLiteralConstants { .matchesAtCodePointBoundaries = true } };
    StubCatalog catalog(StubOptions { }, WTF::move(policy));
    AOT_TEST_CHECK(catalog.bindsImmediate(test, recordForBothWidths) && catalog.bindsImmediate(globalExec, recordFor8BitSubjects));
    AOT_TEST_CHECK(buildBlob(catalog, std::array { test, globalExec }, EntrySet { }).code.relocations.isEmpty());

    constexpr std::array thunksForBothWidths { test.withImmediate(recordForBothWidths), globalExec.withImmediate(recordForBothWidths) };
    constexpr StubKey thunkFor8BitSubjects = test.withImmediate(recordFor8BitSubjects);
    StubBlob blob = buildBlob(catalog, std::array { thunksForBothWidths[0], thunksForBothWidths[1], thunkFor8BitSubjects }, EntrySet { });
    for (StubKey thunk : thunksForBothWidths) {
        AOT_TEST_CHECK(blob.offsetOf(thunk));
        AOT_TEST_CHECK(catalog.dryRun(thunk).symbols.size() == 2);
        AOT_TEST_CHECK(numberOfRegExpCalls(blob, thunk, entryFor8BitSubjects) == 1 && numberOfRegExpCalls(blob, thunk, entryFor16BitSubjects) == 1);
    }
    AOT_TEST_CHECK(blob.offsetOf(thunkFor8BitSubjects));
    AOT_TEST_CHECK(numberOfRegExpCalls(blob, thunkFor8BitSubjects, entryFor8BitSubjects) == 1 && !numberOfRegExpCalls(blob, thunkFor8BitSubjects, entryFor16BitSubjects));
    AOT_TEST_CHECK(blob.code.relocations.size() == 2 * thunksForBothWidths.size() + 1);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace {

struct alignas(16) FakeArray {
    WTF_MAKE_NONCOPYABLE(FakeArray);
public:
    static constexpr uint32_t capacity = 4;

    FakeArray(IndexingType shapeAndWritability, std::initializer_list<uint64_t> initialElements, uint8_t flags = IsPristine)
        : indexingTypeAndMisc(IsArray | shapeAndWritability)
        , flags(flags)
        , publicLength(static_cast<uint32_t>(initialElements.size()))
    {
        AOT_TEST_CHECK(initialElements.size() <= capacity);
        butterfly = elements.data();
        elements.fill(0);
        std::ranges::copy(initialElements, elements.begin());
    }

    uint64_t address() const { return std::bit_cast<uintptr_t>(this); }
    uint64_t shapeAndWritability() const { return indexingTypeAndMisc & IndexingShapeAndWritabilityMask; }

    uint32_t structureID { 0 };
    uint8_t indexingTypeAndMisc;
    uint8_t type { ArrayType };
    uint8_t flags;
    uint8_t cellState { 0 };
    uint64_t* butterfly { nullptr };
    uint32_t publicLength;
    uint32_t vectorLength { capacity };
    std::array<uint64_t, capacity> elements;
};
static_assert(OBJECT_OFFSETOF(FakeArray, indexingTypeAndMisc) == JSCell::indexingTypeAndMiscOffset() && OBJECT_OFFSETOF(FakeArray, type) == JSCell::typeInfoTypeOffset());
static_assert(OBJECT_OFFSETOF(FakeArray, flags) == JSCell::typeInfoFlagsOffset() && OBJECT_OFFSETOF(FakeArray, cellState) == JSCell::cellStateOffset());
static_assert(OBJECT_OFFSETOF(FakeArray, butterfly) == JSObject::butterflyOffset());
static_assert(static_cast<ptrdiff_t>(OBJECT_OFFSETOF(FakeArray, publicLength)) - static_cast<ptrdiff_t>(OBJECT_OFFSETOF(FakeArray, elements)) == Butterfly::offsetOfPublicLength());
static_assert(static_cast<ptrdiff_t>(OBJECT_OFFSETOF(FakeArray, vectorLength)) - static_cast<ptrdiff_t>(OBJECT_OFFSETOF(FakeArray, elements)) == Butterfly::offsetOfVectorLength());

} // anonymous namespace

static constexpr uint64_t bitsOf(double number) { return std::bit_cast<uint64_t>(number); }
static uint64_t boxed(JSValue value) { return static_cast<uint64_t>(JSValue::encode(value)); }

static constexpr uint64_t hole = bitsOf(PNaN);

static void checkFastPath(StubRun run, JSValue result)
{
    AOT_TEST_CHECK(!run.calledOperation && run.result == boxed(result));
}

template<typename Push>
static void checkArrayPush(const Push& push)
{
    FakeArray array(DoubleShape, { bitsOf(0.5) });
    checkFastPath(push(array, jsNumber(1.5)), jsNumber(2));
    checkFastPath(push(array, jsNumber(7)), jsNumber(3));
    AOT_TEST_CHECK(push(array, jsNaN()).calledOperation);
    AOT_TEST_CHECK(array.publicLength == 3 && array.elements[1] == bitsOf(1.5) && array.elements[2] == bitsOf(7));

    FakeArray arrayThatIsNotPristine(DoubleShape, { }, 0);
    checkFastPath(push(arrayThatIsNotPristine, jsNumber(1.5)), jsNumber(1));
    AOT_TEST_CHECK(arrayThatIsNotPristine.publicLength == 1 && arrayThatIsNotPristine.elements[0] == bitsOf(1.5));

    FakeArray copyOnWriteArray(DoubleShape | CopyOnWrite, { });
    AOT_TEST_CHECK(push(copyOnWriteArray, jsNumber(1.5)).calledOperation);
    AOT_TEST_CHECK(!copyOnWriteArray.publicLength && !copyOnWriteArray.elements[0]);
}

static void checkArrayPushDouble(StubRunner& runner)
{
    constexpr StubKey key = arrayPushDouble();
    FakeArray array(DoubleShape, { });
    checkFastPath(runner.run(key, { array.address(), bitsOf(2.5) }), jsNumber(1));
    checkFastPath(runner.run(key, { array.address(), bitsOf(4) }), jsNumber(2));
    AOT_TEST_CHECK(runner.run(key, { array.address(), hole }).calledOperation);
    AOT_TEST_CHECK(array.publicLength == 2 && array.elements[0] == bitsOf(2.5) && array.elements[1] == bitsOf(4));

    FakeArray integers(Int32Shape, { });
    AOT_TEST_CHECK(runner.run(key, { integers.address(), bitsOf(2.5) }).calledOperation);

    FakeArray values(ContiguousShape, { });
    checkFastPath(runner.run(key, { values.address(), bitsOf(2.5) }), jsNumber(1));
    checkFastPath(runner.run(key, { values.address(), bitsOf(4) }), jsNumber(2));
    AOT_TEST_CHECK(values.publicLength == 2 && values.elements[0] == boxed(jsDoubleNumber(2.5)) && values.elements[1] == boxed(jsNumber(4)));

    FakeArray copyOnWriteIntegers(Int32Shape | CopyOnWrite, { });
    AOT_TEST_CHECK(runner.run(key, { copyOnWriteIntegers.address(), bitsOf(2.5) }).calledOperation);
    AOT_TEST_CHECK(!copyOnWriteIntegers.publicLength);

    FakeArray arrayThatIsNotPristine(DoubleShape, { }, 0);
    checkFastPath(runner.run(key, { arrayThatIsNotPristine.address(), bitsOf(2.5) }), jsNumber(1));
    AOT_TEST_CHECK(arrayThatIsNotPristine.publicLength == 1 && arrayThatIsNotPristine.elements[0] == bitsOf(2.5));

    FakeArray copyOnWriteArray(DoubleShape | CopyOnWrite, { });
    AOT_TEST_CHECK(runner.run(key, { copyOnWriteArray.address(), bitsOf(2.5) }).calledOperation);
    AOT_TEST_CHECK(!copyOnWriteArray.publicLength && !copyOnWriteArray.elements[0]);
}

static void testPushStubRegisters()
{
    StubRunner runner;
    checkArrayPush([&](FakeArray& array, JSValue value) {
        return runner.run(arrayPush(), { array.address(), boxed(value) });
    });
    checkArrayPush([&](FakeArray& array, JSValue value) {
        return runner.run(arrayPushOfKnownKind(), { array.address(), boxed(value), array.shapeAndWritability() });
    });
    checkArrayPushDouble(runner);
}

// What a push of a number that compiled code holds as a double costs in the stubs, counted one by one. A number here may fall.
static void testInstructionsOfPushedDoubles()
{
    // A build with assertions puts more instructions into the stubs, so the budget is held against the other builds only.
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && !ASSERT_ENABLED
    StubRunner runner;
    runner.setCountsInstructions(true);

    struct Push {
        ASCIILiteral what;
        double number;
        uint32_t budget;
    };
    const Push pushes[] = {
        { "a fraction"_s, 2.5, 17 },
        { "a whole number"_s, 4, 17 },
        { "negative zero"_s, -0.0, 17 },
    };
    bool isWithinBudget = true;
    for (const Push& push : pushes) {
        FakeArray array(DoubleShape, { bitsOf(0.5) });
        StubRun run = runner.run(arrayPushDouble(), { array.address(), bitsOf(push.number) });
        checkFastPath(run, jsNumber(2));
        AOT_TEST_CHECK(array.publicLength == 2 && array.elements[1] == bitsOf(push.number) && run.instructionsInStubs);
        if (*run.instructionsInStubs != push.budget)
            dataLogLn("a push of ", push.what, " onto an array of doubles: ", *run.instructionsInStubs, " instructions, the budget is ", push.budget);
        isWithinBudget &= *run.instructionsInStubs <= push.budget;
    }
    AOT_TEST_CHECK(isWithinBudget);
#endif
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

std::span<const NativeTest> stubBlobTests()
{
    static constexpr NativeTest tests[] = {
        { "stub-blob/entry-placements"_s, testEntryPlacements },
        { "stub-blob/return-sites"_s, testReturnSites },
        { "stub-blob/body-entries"_s, testBodyEntries },
        { "stub-blob/references"_s, testReferences },
        { "stub-blob/reachability"_s, testReachability },
        { "stub-blob/missing-keys"_s, testMissingKeys },
        { "stub-blob/summaries"_s, testSummaries },
        { "stub-blob/operations-called"_s, testOperationsCalled },
        { "stub-blob/wide-references"_s, testWideReferences },
        { "stub-blob/layout-seed"_s, testLayoutSeed },
        { "stub-blob/regexp-relocations"_s, testRegExpRelocations },
        { "stub-blob/push-stubs-preserve-fprs"_s, ImageTests::whenExecutable<testPushStubRegisters> },
        { "stub-blob/instructions-of-pushed-doubles"_s, ImageTests::whenExecutable<testInstructionsOfPushedDoubles> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

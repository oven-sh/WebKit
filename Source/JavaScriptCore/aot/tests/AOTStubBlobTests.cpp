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
#include "AOTStubBlob.h"
#include "AOTStubCatalog.h"
#include "AOTStubRecording.h"
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
#include "AOTStubsConstants.h"
#include "AOTStubsCoroutines.h"
#include "AOTStubsElements.h"
#include "AOTStubsExceptions.h"
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
#include <algorithm>
#include <array>
#include <bit>
#include <utility>
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
static constexpr std::array stepConsumers { StepConsumer::AsyncFunction, StepConsumer::AsyncGenerator };
static_assert(stepConsumers.size() == descriptorOf(StubFamily::StepAsyncGenerator).numberOfForms);

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

static bool mayThrowAndCollect(const StubCatalog& catalog, StubKey key)
{
    Typed::Effects effects = catalog.effectsOf(key);
    return effects.mayThrow && effects.mayCollect;
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
        StubCatalog catalog(options);
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
        { StubFrameKind::EngineCallee, callEngineCallee() },
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
    StubCatalog catalog(StubOptions { });
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
    StubCatalog catalog(StubOptions { });
    checkEntries(catalog, getByVal(false), { getByVal(false), getByVal(true) });
    checkEntries(catalog, getByValCached(), { getByValCached(), getByValCachedCells(), getByValCachedAtPlace(), getByValCachedBehindPlaces() });
    checkEntries(catalog, getByValKeepingRegisters(), { getByValKeepingRegisters(), getByValAtConstantIndex() });
    for (ECMAMode mode : { ECMAMode::strict(), ECMAMode::sloppy() })
        checkEntries(catalog, putByVal(false, mode), { putByVal(false, mode), putByVal(true, mode), putByValAtConstantIndex(mode) });
    checkEntries(catalog, getByValCached(RegistersOfCaller::AreKept), { getByValCached(RegistersOfCaller::AreKept), getByValCachedCells(RegistersOfCaller::AreKept), getByValCachedAtPlace(RegistersOfCaller::AreKept), getByValCachedBehindPlaces(RegistersOfCaller::AreKept) });
    for (BaseOfKeyedRead base : { BaseOfKeyedRead::IsCell, BaseOfKeyedRead::IsAnyValue }) {
        RegisterSet keptByRead = registersClobberedByOperations();
        keptByRead.exclude(catalog.summaryOf(getByValOfKeyOfPair(base)).clobbered);
        AOT_TEST_CHECK(keptByRead.numberOfSetFPRs() >= 2);
    }
    AOT_TEST_CHECK(catalog.summaryOf(getByValCachedCells()).clobbered.subsumes(registersClobberedByOperations()));
    checkEntries(catalog, getByIdAtSite(), { getByIdAtSite(), getByIdOfObjectAtSite() });
    for (StubKey key : { getByIdCalledByCode(), getByIdCheckingColumn(), putByIdHashed(PutMode::Strict), putByIdHashed(PutMode::Define), putByIdAtSite(PutMode::Sloppy), readAtSite(ReceiverAtSite::IsAnyValue), readAtSite(ReceiverAtSite::IsCell), readTruthAtSite(ReceiverAtSite::IsAnyValue), readAtSiteAskingStructureFirst(ReceiverAtSite::IsCell), readTruthAtSiteAskingStructureFirst(ReceiverAtSite::IsAnyValue), readAtSiteUnlessAsMade(), callMethodAtSite(0), callMethodAtSite(2), callMethodAtSite(2, 1), callMethodAtSite(3, 3) }) {
        StubKey sibling = siblingUnderNameWithColumn(key).value();
        AOT_TEST_CHECK(exists(sibling) && sibling != key && !siblingUnderNameWithColumn(sibling));
        AOT_TEST_CHECK(catalog.summaryOf(key).clobbered.subsumes(catalog.summaryOf(sibling).clobbered));
    }
    checkEntries(catalog, enterClosure(), { enterClosure(), enterSlimClosure(), enterModuleBody() });
    checkEntries(catalog, runAsyncGeneratorStep(), { runAsyncGeneratorStep(), stepAsyncIteratorInEngine() });
    for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count) {
        checkEntries(catalog, callValue(count), { callValue(count), callValueWithoutThis(count), callValueCell(count), callValueCellWithoutThis(count), callValueFunctionOfEngine(count) });
        if (exists(callHostFunctionWithoutBoxedCall(count)))
            checkEntries(catalog, callHostFunction(count), { callHostFunction(count), callHostFunctionThatMayBeUnfetched(count), callHostFunctionWithoutBoxedCall(count) });
        else
            checkEntries(catalog, callHostFunction(count), { callHostFunction(count) });
    }
    checkEntries(catalog, callValueWithCount(), { callValueWithCount(), callCellWithCount() });
    checkEntries(catalog, arrayPush(), { arrayPush(), arrayPushOfKnownKind() });

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
    AOT_TEST_CHECK(refersOnceBy(catalog, defineByValCached(), storeElementOfCompiledObject(PutMode::Define), StubReference::Kind::Jump));
    AOT_TEST_CHECK(refersOnceBy(catalog, putByVal(false, ECMAMode::strict()), storeElementOfCompiledObject(PutMode::Strict), StubReference::Kind::Jump));
    AOT_TEST_CHECK(refersOnceBy(catalog, putByVal(false, ECMAMode::sloppy()), storeElementOfCompiledObject(PutMode::Sloppy), StubReference::Kind::Jump));

    checkHandlerReferences(catalog, getByValCached(), SlotHandlerTable::ReadByVal);
    checkHandlerReferences(catalog, putByValCached(PutMode::Strict), SlotHandlerTable::StoreStrictByVal);
    checkHandlerReferences(catalog, putByValCached(PutMode::Sloppy), SlotHandlerTable::StoreSloppyByVal);
    checkHandlerReferences(catalog, defineByValCached(), SlotHandlerTable::DefineByVal);
}

static void checkResumptionReferences(const StubCatalog& catalog)
{
    AOT_TEST_CHECK(refersOnceBy(catalog, enterSavedFrame(), restoreSavedFrameToStack(), StubReference::Kind::Call));
    for (GeneratorStepCheck check : stepChecks)
        AOT_TEST_CHECK(refersOnceBy(catalog, stepGenerator(check), enterGeneratorFrame(), StubReference::Kind::Call));
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
        { BoxedCallForm::SetAdd, StubFamily::SetAdd }, { BoxedCallForm::SetHas, StubFamily::MapHas },
        { BoxedCallForm::StringIndexOf, StubFamily::StringIndexOf },
        { BoxedCallForm::StringSlice, StubFamily::Substring },
        { BoxedCallForm::StringToLowerCase, StubFamily::CallOperation },
    };
    for (unsigned count = 0; count <= numberOfArgumentGPRs; ++count) {
        bool hasBoxedCalls = count >= BoxedCallEntry::minArgumentCount && count <= BoxedCallEntry::maxArgumentCount;
        AOT_TEST_CHECK(refersToFamily(catalog, callHostFunction(count), StubFamily::CallBoxedBuiltin) == hasBoxedCalls);
        if (!hasBoxedCalls)
            continue;
        for (auto [form, family] : boxedCallTargets) {
            StubKey handler = callBoxedBuiltin(form, count);
            AOT_TEST_CHECK(refersOnceBy(catalog, callHostFunction(count), handler, StubReference::Kind::Jump));
            bool callsOperationOfItsCount = count > 1 && (form == BoxedCallForm::ArrayPush || form == BoxedCallForm::StringIndexOf);
            AOT_TEST_CHECK(refersToFamily(catalog, handler, family) == (acceptsArgumentCount(form, count) && !callsOperationOfItsCount));
            AOT_TEST_CHECK(refersOnceBy(catalog, handler, hostCallLeftTo(form, count), StubReference::Kind::Jump));
        }
    }
}

static void testReferences()
{
    StubCatalog catalog(StubOptions { });
    checkKeyedAccessReferences(catalog);
    checkResumptionReferences(catalog);
    checkExceptionReferences(catalog);
    checkHostCallReferences(catalog);
}

static void testReachability()
{
    StubCatalog catalog(StubOptions { });

    StubBlob keyedReadBlob = buildBlob(catalog, std::array { getByValCached() }, EntrySet { });
    for (StubKey key : { getByValCached(), getByValCachedCells(), getByVal(false), getByVal(true) })
        AOT_TEST_CHECK(keyedReadBlob.offsetOf(key));
    AOT_TEST_CHECK(!keyedReadBlob.offsetOf(newMap()));

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

static void testStartOfModule()
{
    StubCatalog catalog(StubOptions { });
    AOT_TEST_CHECK(operationsCalledBy(catalog, startModule()) == entrySetOf({ Entry::operationAOTBeginModuleEvaluation, Entry::operationAOTStopLoadingBehindFile, Entry::operationAOTCatchException, Entry::operationAOTAbandonModuleEvaluation, Entry::operationAOTThrow }));
    AOT_TEST_CHECK(std::ranges::count(catalog.recordingOf(startModule()).returnLabels, StubFrameKind::ModuleStart) == 1);
    AOT_TEST_CHECK(refersToBody(catalog, startModule(), bodyOf(handleException(false))));
    AOT_TEST_CHECK(mayThrowAndCollect(catalog, startModule()));
    const StubSignature& signature = catalog.summaryOf(startModule()).signature;
    AOT_TEST_CHECK(signature.numberOfOperands == 1 && signature.operands[0] == Reg(gprOf(StubRegister::A0)) && signature.firstResult == Reg(GPRInfo::returnValueGPR));
    for (StubKey body : allStubBodies())
        AOT_TEST_CHECK(body == startModule() || !catalog.recordingOf(body).returnLabels.contains(StubFrameKind::ModuleStart));

    StubBlob blob = buildBlob(catalog, std::array { startModule() }, EntrySet { });
    AOT_TEST_CHECK(blob.offsetOf(startModule()));
    AOT_TEST_CHECK(numberOfSites(blob, StubFrameKind::ModuleStart) == 1 && bodiesContainingSites(blob, StubFrameKind::ModuleStart)[0] == startModule());
    for (Entry entry : { Entry::operationAOTBeginModuleEvaluation, Entry::operationAOTStopLoadingBehindFile, Entry::operationAOTAbandonModuleEvaluation })
        AOT_TEST_CHECK(blob.entriesUsed.get(indexOf(entry)));

    AOT_TEST_CHECK(refersOnceBy(catalog, startModuleThatRunTimeNames(), startModule(), StubReference::Kind::Jump));
    StubBlob blobBehindWords = buildBlob(catalog, std::array { startModuleThatRunTimeNames() }, EntrySet { });
    AOT_TEST_CHECK(blobBehindWords.offsetOf(startModuleThatRunTimeNames()) && blobBehindWords.offsetOf(startModule()) && numberOfSites(blobBehindWords, StubFrameKind::ModuleStart) == 1);

    StubBlob blobWithoutStarts = buildBlob(catalog, std::array { newPromise() }, EntrySet { });
    AOT_TEST_CHECK(!blobWithoutStarts.offsetOf(startModuleThatRunTimeNames()) && !blobWithoutStarts.offsetOf(startModule()) && !numberOfSites(blobWithoutStarts, StubFrameKind::ModuleStart));
    for (Entry entry : { Entry::operationAOTBeginModuleEvaluation, Entry::operationAOTStopLoadingBehindFile, Entry::operationAOTAbandonModuleEvaluation })
        AOT_TEST_CHECK(!blobWithoutStarts.entriesUsed.get(indexOf(entry)));
}

static void testMissingKeys()
{
    for (StubKey key : { newPromise(), toNumberAsDouble(), substring(), arraySlice(), arrayPush(), stringsAreEqual() })
        AOT_TEST_CHECK(exists(key) && !exists(key.withCallerFrame(CallerFrame::Frameless)));
    for (CallerFrame callerFrame : { CallerFrame::HasFrame, CallerFrame::Frameless }) {
        AOT_TEST_CHECK(exists(pollAtLoop(callerFrame)));
        AOT_TEST_CHECK(exists(throwAtSite(ThrowSiteSource::FaultSite, callerFrame)));
    }

    AOT_TEST_CHECK(!exists(StubKey(StubFamily::StepGenerator).withForm(4)));
    AOT_TEST_CHECK(!exists(StubKey(StubFamily::AwaitBySavingFrame).withForm(4)));
    AOT_TEST_CHECK(!exists(StubKey(StubFamily::StepAsyncGenerator).withForm(2)));
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
    AOT_TEST_CHECK(mayThrowAndCollect(catalog, returnAsyncResult()));
#if CPU(X86_64)
    for (RegistersSavedInAsyncFrame saved : { RegistersSavedInAsyncFrame::None, RegistersSavedInAsyncFrame::RBX, RegistersSavedInAsyncFrame::R15, RegistersSavedInAsyncFrame::Both }) {
        AOT_TEST_CHECK(mayThrowAndCollect(catalog, returnAsyncResultInTailPosition(saved)));
        AOT_TEST_CHECK(operationsCalledBy(catalog, returnAsyncResultInTailPosition(saved)) == operationsCalledBy(catalog, returnAsyncResult()));
    }
#endif

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

static void checkTextSearchSummary(const StubCatalog& catalog)
{
    const StubSummary& summary = catalog.summaryOf(findText8());
    for (GPRReg reg : { A2, A3, gprOf(StubRegister::A4) })
        AOT_TEST_CHECK(containsRegister(summary.clobbered, reg));
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
        if (std::ranges::find(creationFamilies, key.family()) != creationFamilies.end() && key.callerFrame() != CallerFrame::KeepsNoFPRs)
            checkArrayCreationSummary(catalog, key, largestLiteral);
    }
    checkArrayCreationSummary(catalog, newEmptyArrayAtSite(ArrayElementKind::Contiguous), largestLiteral);
    for (EmptyArray few : { EmptyArray::Int32Capacity1, EmptyArray::Int32Capacity3 })
        AOT_TEST_CHECK(refersToBody(catalog, newEmptyArrayAtSite(ArrayElementKind::Int32), bodyOf(newEmptyArray(few))));

    for (StubKey key : { arrayPush(), arrayPushOfKnownKind() })
        AOT_TEST_CHECK(!clobbersFPR(catalog.summaryOf(key)));
}

static bool isLostInLeafCall(GPRReg reg)
{
    return reg == leafArgumentGPR(0) || reg == leafArgumentGPR(1) || containsRegister(registersClobberedByPreserveMostLeaves(), reg);
}

static void checkEqualitySummaries(const StubCatalog& catalog)
{
    for (StubKey strictEqual : { equal(true), strictEqualInFlags() }) {
        const RegisterSet& clobberedByStrictEqual = catalog.summaryOf(strictEqual).clobbered;
        for (GPRReg reg : { A1, A2, A3, T13, gprOf(StubRegister::T14) })
            AOT_TEST_CHECK(!containsRegister(clobberedByStrictEqual, reg));
    }

    const StubSummary& stringEquality = catalog.summaryOf(stringsAreEqual());
    AOT_TEST_CHECK(stringEquality.signature.operands[0] == Reg(T11) && stringEquality.signature.operands[1] == Reg(T12));
    for (GPRReg operand : { T11, T12 })
        AOT_TEST_CHECK(containsRegister(stringEquality.clobbered, operand) == isLostInLeafCall(operand));
}

static void checkClobbersOfRegisterVariants(const StubCatalog& catalog)
{
    unsigned numberOfKeysWithVariants = 0;
    forEachKeyWithoutImmediate([&](StubKey key) {
        RegisterSet clobbered;
        unsigned numberOfVariants = 0;
        forEachRegisterVariant(key, [&](StubKey variant) {
            clobbered.merge(catalog.summaryOf(variant).clobbered);
            ++numberOfVariants;
        });
        numberOfKeysWithVariants += numberOfVariants > 1;
        AOT_TEST_CHECK(catalog.clobberedByRegisterVariantsOf(key) == clobbered);
        AOT_TEST_CHECK(catalog.clobberedByRegisterVariantsOf(key) == clobbered);
    });
    AOT_TEST_CHECK(numberOfKeysWithVariants);
#if CPU(X86_64)
    for (GPRReg kept : { X86Registers::ecx, X86Registers::edx, X86Registers::esi, X86Registers::edi })
        AOT_TEST_CHECK(!containsRegister(catalog.summaryOf(getByIdKeepingRegisters()).clobbered, kept));
    AOT_TEST_CHECK(catalog.summaryOf(getById()).clobbered.subsumes(catalog.summaryOf(getByIdKeepingRegisters()).clobbered));
#endif
}

static void checkClobbersBesideResults(const StubCatalog& catalog)
{
    RegisterSet scratchRegistersOfAssembler = RegisterSet::macroClobberedGPRs();
    scratchRegistersOfAssembler.merge(RegisterSet::macroClobberedFPRs());
    for (CreatedConstant constant : { CreatedConstant::String, CreatedConstant::Key }) {
        AOT_TEST_CHECK(scratchRegistersOfAssembler.subsumes(catalog.clobberedBesideResultByRegisterVariantsOf(createConstant(constant))));
        for (GPRReg reg : allocatableGPRs) {
            const StubSignature& signature = catalog.summaryOf(createConstant(constant).withOperandRegister(reg)).signature;
            AOT_TEST_CHECK(signature.firstResult == Reg(reg) && signature.immediate == reg && !signature.numberOfOperands);
        }
    }

    unsigned numberOfVariants = 0;
    forEachKeyWithoutImmediate([&](StubKey key) {
        if (key.operandRegister() || !namesRegisterOfResult(descriptorOf(key.family()).operandRegisters))
            return;
        const StubSummary& summary = catalog.summaryOf(key);
        const RegisterSet& lost = catalog.clobberedBesideResultByRegisterVariantsOf(key);
        AOT_TEST_CHECK(summary.signature.firstResult == Reg(GPRInfo::returnValueGPR) && !containsRegister(lost, GPRInfo::returnValueGPR));
        forEachRegisterVariant(key, [&](StubKey variant) {
            if (variant == key)
                return;
            ++numberOfVariants;
            GPRReg reg = *variant.operandRegister();
            const StubSignature& signature = catalog.summaryOf(variant).signature;
            AOT_TEST_CHECK(signature.firstResult == Reg(reg) && signature.numberOfOperands == summary.signature.numberOfOperands && !containsRegister(lost, reg));
            for (unsigned index = 0; index < signature.numberOfOperands; ++index)
                AOT_TEST_CHECK(signature.operands[index] == summary.signature.operands[index] && signature.operands[index] != Reg(reg));
        });
    });
    AOT_TEST_CHECK(!numberOfVariants == alternateResultGPRs().empty());
}

static void testSummaries()
{
    StubCatalog catalog(StubOptions { });
    checkClobbersOfRegisterVariants(catalog);
    checkClobbersBesideResults(catalog);
    checkCoroutineSummaries(catalog);
    checkNumberConversionSummary(catalog);
    checkSubstringSummary(catalog);
    checkTextSearchSummary(catalog);
    checkArraySummaries(catalog);
    checkEqualitySummaries(catalog);
}

static void checkCoroutineOperations(const StubCatalog& catalog)
{
    Vector<StubKey> keys { awaitBySavingFrame(), awaitCallBySavingFrame(), returnAsyncResult(), resolveReturnedPromise(), newPromise() };
    for (GeneratorStepCheck check : stepChecks)
        keys.append(stepGenerator(check));

    EntrySet operationsCalled;
    for (StubKey key : keys)
        operationsCalled.merge(operationsCalledBy(catalog, key));
    AOT_TEST_CHECK(operationsCalled == entrySetOf({ Entry::operationAOTNewPromise, Entry::operationAOTSettleAwaitedPromise, Entry::operationAOTSaveFrameAndAwait, Entry::operationAOTSaveFrameAndAwaitCall, Entry::operationAOTAwaitWithSavedFrame, Entry::operationAOTAwaitCallWithSavedFrame, Entry::operationAOTResolvePromiseOrReturnResult }));

    AOT_TEST_CHECK(operationsCalledBy(catalog, awaitBySavingFrame(AwaitedOperand::ValueInModuleBody)) == entrySetOf({ Entry::operationAOTSaveFrameAndAwaitInModuleBody, Entry::operationAOTAwaitWithSavedFrameInModuleBody }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, awaitBySavingFrame(AwaitedOperand::Resolution)) == entrySetOf({ Entry::operationAOTSettleAwaitedPromise, Entry::operationAOTSaveFrameAndAwaitResolution, Entry::operationAOTAwaitResolutionWithSavedFrame }));
    for (StepConsumer consumer : stepConsumers)
        AOT_TEST_CHECK(operationsCalledBy(catalog, stepAsyncGenerator(consumer)).isEmpty());
    AOT_TEST_CHECK(operationsCalledBy(catalog, runAsyncGeneratorStep()) == entrySetOf({ Entry::operationAOTAsyncIteratorStep, Entry::operationAOTFinishAsyncIteratorStep }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, newAsyncGenerator(GeneratorFunctionOrigin::Program)) == entrySetOf({ Entry::operationAOTNewAsyncGenerator }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, newAsyncGenerator(GeneratorFunctionOrigin::Library)) == entrySetOf({ Entry::operationAOTNewAsyncGenerator }));
}

static void checkCollectionOperations(const StubCatalog& catalog)
{
    AOT_TEST_CHECK(operationsCalledBy(catalog, mapSet(KeyComparison::String)) == entrySetOf({ Entry::operationAOTMapSet, Entry::operationAOTStringImplsAreEqual, Entry::operationAOTHashStringImpl }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, mapSet(KeyComparison::Bits)) == entrySetOf({ Entry::operationAOTMapSet }));
    AOT_TEST_CHECK(refersToBody(catalog, mapSet(KeyComparison::Bits), writeBarrier()));
    AOT_TEST_CHECK(refersToBody(catalog, mapSet(KeyComparison::Bits), newCompactTable(CompactTableOwner::Map)) && refersToBody(catalog, setAdd(KeyComparison::String), newCompactTable(CompactTableOwner::Set)));
    AOT_TEST_CHECK(operationsCalledBy(catalog, newCompactTable(CompactTableOwner::Map)) == entrySetOf({ Entry::operationAOTMapSet }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, newCompactTable(CompactTableOwner::Set)) == entrySetOf({ Entry::operationAOTSetAdd }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, mapDelete(KeyComparison::Bits)) == entrySetOf({ Entry::operationAOTMapDelete }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, setDelete(KeyComparison::String)) == entrySetOf({ Entry::operationAOTSetDelete, Entry::operationAOTStringImplsAreEqual, Entry::operationAOTHashStringImpl }));
    AOT_TEST_CHECK(operationsCalledBy(catalog, writeBarrier()).get(indexOf(Entry::operationAOTBarrierSlow)));
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
    StubCatalog catalog(StubOptions { });
    checkCoroutineOperations(catalog);
    checkCollectionOperations(catalog);
    checkAdapterOperations(catalog);
}

static void testTagsOfSmallTablesAreStoredWhole()
{
#if CPU(X86_64)
    constexpr std::array<uint8_t, 4> orWordOfTags { 0x49, 0x09, 0x42, CompactTable::offsetOfIndex() };
    StubCatalog catalog(StubOptions { });
    for (StubKey insertion : { mapSet(KeyComparison::Bits), mapSet(KeyComparison::String), setAdd(KeyComparison::Bits), setAdd(KeyComparison::String) }) {
        StubBlob blob = buildBlob(catalog, std::array { insertion }, EntrySet { });
        AOT_TEST_CHECK(!std::ranges::search(blob.code.bytes, orWordOfTags).empty());
    }
#endif
}

static void testWideReferences()
{
    StubOptions wideOptions;
    wideOptions.useWideReferences = true;
    StubCatalog catalog(StubOptions { });
    StubCatalog wideCatalog(wideOptions);

    constexpr StubKey store = mapSet(KeyComparison::Bits);
    StubBlob blob = buildBlob(catalog, std::array { store }, EntrySet { });
    StubBlob wideBlob = buildBlob(wideCatalog, std::array { store }, EntrySet { });
    AOT_TEST_CHECK(sizeOfBody(wideBlob, store) < sizeOfBody(blob, store));
    AOT_TEST_CHECK(operationsCalledBy(wideCatalog, store) == operationsCalledBy(catalog, store));
}

static void testInt32LiteralsStoreWholeWords()
{
#if CPU(X86_64)
    constexpr std::array<uint8_t, 8> storeOfUpperHalf { 0x41, 0xc7, 0x41, 0x0c, 0x00, 0x00, 0xfe, 0xff };
    StubCatalog catalog(StubOptions { });
    for (unsigned count = 1; count <= descriptorOf(StubFamily::ArrayLiteral).maxCount; ++count) {
        StubBlob blob = buildBlob(catalog, std::array { arrayLiteralWithInt32(count) }, EntrySet { });
        AOT_TEST_CHECK(std::ranges::search(blob.code.bytes, storeOfUpperHalf).empty());
    }
#endif
}

static void testImmediatesInKeys()
{
    constexpr StubKey cell = builtinCell().withImmediate(7);
    constexpr StubKey match = regExpMatchLiteralWithoutObject(Typed::RegExpMatchForm::Exec);
    static_assert(exists(cell) && exists(match) && !exists(match.withImmediate(1)));

    StubCatalog catalog(StubOptions { });
    AOT_TEST_CHECK(catalog.bindsImmediate(cell.withoutImmediate()));
    AOT_TEST_CHECK(!catalog.bindsImmediate(createConstant(CreatedConstant::Key)) && !catalog.bindsImmediate(match));

    StubRecording recording = catalog.dryRun(cell);
    AOT_TEST_CHECK(recording.references.size() == 1 && recording.references[0].target == cell.withoutImmediate() && recording.references[0].kind == StubReference::Kind::Jump);

    StubBlob blob = buildBlob(catalog, std::array { cell }, EntrySet { });
    std::optional<uint32_t> offset = blob.offsetOf(cell);
    std::optional<uint32_t> offsetOfStubTakingImmediate = blob.offsetOf(cell.withoutImmediate());
    AOT_TEST_CHECK(offset && offsetOfStubTakingImmediate && *offsetOfStubTakingImmediate < *offset);
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

static void testPushStubRegisters()
{
    StubRunner runner;
    checkArrayPush([&](FakeArray& array, JSValue value) {
        return runner.run(arrayPush(), { array.address(), boxed(value) });
    });
    checkArrayPush([&](FakeArray& array, JSValue value) {
        return runner.run(arrayPushOfKnownKind(), { array.address(), boxed(value), array.shapeAndWritability() });
    });
}

template<uint8_t shape, uint32_t readBudget, uint32_t writeBudget>
static void testInstructionsAtConstantIndex()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    StubRunner runner;
    const JSValue held = shape == DoubleShape ? jsDoubleNumber(0.1) : jsNumber(5);
    const JSValue stored = shape == DoubleShape ? jsDoubleNumber(0.3) : jsNumber(6);
    auto bitsInArray = [&](JSValue value) { return shape == DoubleShape ? bitsOf(value.asNumber()) : boxed(value); };
    FakeArray array(shape, { bitsInArray(held), bitsInArray(held) });
    auto isWithin = [&](ASCIILiteral what, StubRun run, uint32_t budget) {
        AOT_TEST_CHECK(!run.calledOperation && run.instructionsInStubs);
        if (*run.instructionsInStubs != budget)
            dataLogLn(what, " at a constant index: ", *run.instructionsInStubs, " instructions, the budget is ", budget);
        return *run.instructionsInStubs <= budget;
    };

    runner.setCountsInstructions(true);
    StubRun read = runner.run(getByValAtConstantIndex(), { array.address(), 1 });
    StubRun write = runner.run(putByValAtConstantIndex(ECMAMode::strict()), { array.address(), 1, boxed(stored) });
    AOT_TEST_CHECK(read.result == boxed(held) && array.elements[1] == bitsInArray(stored));
    AOT_TEST_CHECK(isWithin("a read"_s, read, readBudget) & isWithin("a store"_s, write, writeBudget));
#endif
}

enum class PushedOnto : uint8_t { Doubles, Integers, Values };

template<PushedOnto onto, bool kindIsKnown, uint32_t budget>
static void testInstructionsOfPush()
{
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
    StubRunner runner;
    runner.setCountsInstructions(true);
    const JSValue pushed = onto == PushedOnto::Doubles ? jsDoubleNumber(2.5) : jsNumber(7);
    FakeArray array(onto == PushedOnto::Doubles ? DoubleShape : onto == PushedOnto::Integers ? Int32Shape : ContiguousShape, { onto == PushedOnto::Doubles ? bitsOf(0.5) : boxed(jsNumber(1)) });
    StubRun run = kindIsKnown ? runner.run(arrayPushOfKnownKind(), { array.address(), boxed(pushed), array.shapeAndWritability() }) : runner.run(arrayPush(), { array.address(), boxed(pushed) });
    checkFastPath(run, jsNumber(2));
    AOT_TEST_CHECK(array.publicLength == 2 && array.elements[1] == (onto == PushedOnto::Doubles ? bitsOf(2.5) : boxed(pushed)) && run.instructionsInStubs);
    if (*run.instructionsInStubs != budget)
        dataLogLn("a push: ", *run.instructionsInStubs, " instructions, the budget is ", budget);
    AOT_TEST_CHECK(*run.instructionsInStubs <= budget);
#endif
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

std::span<const NativeTest> stubBlobTests()
{
    static constexpr NativeTest tests[] = {
        { "stub-blob/entry-placements"_s, testEntryPlacements },
        { "stub-blob/instructions-at-constant-indices/doubles"_s, ImageTests::whenExecutable<testInstructionsAtConstantIndex<DoubleShape, 20, 25>> },
        { "stub-blob/instructions-at-constant-indices/int32s"_s, ImageTests::whenExecutable<testInstructionsAtConstantIndex<Int32Shape, 17, 18>> },
        { "stub-blob/instructions-of-pushes/a-double-onto-doubles"_s, ImageTests::whenExecutable<testInstructionsOfPush<PushedOnto::Doubles, false, 26>> },
        { "stub-blob/instructions-of-pushes/a-double-onto-doubles-of-a-known-kind"_s, ImageTests::whenExecutable<testInstructionsOfPush<PushedOnto::Doubles, true, 24>> },
        { "stub-blob/instructions-of-pushes/an-integer-onto-integers"_s, ImageTests::whenExecutable<testInstructionsOfPush<PushedOnto::Integers, false, 23>> },
        { "stub-blob/instructions-of-pushes/an-integer-onto-integers-of-a-known-kind"_s, ImageTests::whenExecutable<testInstructionsOfPush<PushedOnto::Integers, true, 21>> },
        { "stub-blob/instructions-of-pushes/a-number-onto-values"_s, ImageTests::whenExecutable<testInstructionsOfPush<PushedOnto::Values, false, 19>> },
        { "stub-blob/instructions-of-pushes/a-number-onto-values-of-a-known-kind"_s, ImageTests::whenExecutable<testInstructionsOfPush<PushedOnto::Values, true, 17>> },
#if AOT_STUB_RUNNER_COUNTS_INSTRUCTIONS && CPU(X86_64) && !ASSERT_ENABLED
        { "stub-blob/instructions-that-were-enough/a-push-of-a-double-onto-doubles-takes-24-not-17"_s, ImageTests::whenExecutable<testInstructionsOfPush<PushedOnto::Doubles, true, 17>>, "lists: 24 instructions where the compiler before this one took 17"_s },
#endif
        { "stub-blob/return-sites"_s, testReturnSites },
        { "stub-blob/body-entries"_s, testBodyEntries },
        { "stub-blob/references"_s, testReferences },
        { "stub-blob/reachability"_s, testReachability },
        { "stub-blob/start-of-module"_s, testStartOfModule },
        { "stub-blob/missing-keys"_s, testMissingKeys },
        { "stub-blob/summaries"_s, testSummaries },
        { "stub-blob/operations-called"_s, testOperationsCalled },
        { "stub-blob/tags-of-small-tables-are-stored-whole"_s, testTagsOfSmallTablesAreStoredWhole },
        { "stub-blob/wide-references"_s, testWideReferences },
        { "stub-blob/int32-literals-store-whole-words"_s, testInt32LiteralsStoreWholeWords },
        { "stub-blob/immediates-in-keys"_s, testImmediatesInKeys },
        { "stub-blob/push-stubs-preserve-fprs"_s, ImageTests::whenExecutable<testPushStubRegisters> },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

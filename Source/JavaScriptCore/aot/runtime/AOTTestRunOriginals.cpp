/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSModuleLoader.h"
#include "ModuleRegistryEntry.h"
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT {

bool Modules::statementsHoldOriginalBehindEndOfMock(ModuleID mocked, ModuleID importer) const
{
    if (m_importersThatGotOriginal.isEmpty()) [[likely]]
        return false;
    auto got = m_importersThatGotOriginal.find(static_cast<uint64_t>(mocked.index()) << 32 | importer.index());
    if (got == m_importersThatGotOriginal.end() || (got->value.isGivenItNoMore && !got->value.staysUnderKeyOfOriginal))
        return false;
    for (const ModuleRequestData& request : m_table.imports(importer)) {
        if (fileOf(moduleLoadedBy(request)) == mocked)
            return true;
    }
    return false;
}

bool Modules::importsOriginalBesideMock(ModuleID fileOrModule, ModuleID importer, OriginalIsAskedBy askedBy) const
{
    ModuleID mocked = fileOf(fileOrModule);
    if (!importer)
        return false;
    if (!isMocked(mocked))
        return askedBy == OriginalIsAskedBy::Statement && statementsHoldOriginalBehindEndOfMock(mocked, importer);
    if (!host().givesOriginalBesideMock || !m_table.hasFilePath(mocked) || !m_table.hasFilePath(importer))
        return false;
    bool isFile = data(mocked).kind == ModuleKind::CommonJs && !data(mocked).isOfLibrary();
    if (isFile ? moduleWithExportsOf(mocked) == mocked || fileOf(importer) == mocked : !data(mocked).isESModuleOfProgram() && data(mocked).kind != ModuleKind::Data)
        return false;
    uint64_t pair = static_cast<uint64_t>(mocked.index()) << 32 | importer.index();
    if (auto gotMock = askedBy == OriginalIsAskedBy::Statement ? m_importersThatGotMock.find(pair) : m_importersThatGotMock.end(); gotMock != m_importersThatGotMock.end()) {
        if (momentLinked(importer) <= gotMock->value)
            return false;
        m_importersThatGotMock.remove(gotMock);
    }
    auto asked = m_importersAskedAboutOriginal.add(pair, AnswersOfHostOnOriginal { });
    if (asked.isNewEntry) {
        asked.iterator->value.isGiven = host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(mocked), m_table.pathText(importer), true);
        asked.iterator->value.staysLinked = host().staysLinkedToFileAheadOfMock(m_instance.globalObject, m_table.pathText(mocked), m_table.pathText(importer));
    }
    if (askedBy == OriginalIsAskedBy::CallOfImport && !asked.iterator->value.isGiven)
        return false;
    bool getsOriginal = asked.iterator->value.isGiven || asked.iterator->value.staysLinked;
    if (getsOriginal)
        m_importersThatGotOriginal.set(asked.iterator->key, GotOriginal { m_instance.vm->aotState().momentOfLoads, false, m_timesMocked.get(mocked), false, asked.iterator->value.isGiven });
    else if (auto got = m_importersThatGotOriginal.find(asked.iterator->key); got != m_importersThatGotOriginal.end())
        got->value.isGivenItNoMore = true;
    else if (askedBy == OriginalIsAskedBy::Statement && status(importer) > ModuleStatus::Linked)
        m_importersThatGotMock.add(pair, m_instance.vm->aotState().momentOfLoads);
    return getsOriginal;
}

bool Modules::keepsOriginalUnderLaterMock(ModuleID fileOrModule, ModuleID importer) const
{
    if (m_importersThatGotOriginal.isEmpty()) [[likely]]
        return false;
    ModuleID mocked = fileOf(fileOrModule);
    auto got = m_importersThatGotOriginal.find(static_cast<uint64_t>(mocked.index()) << 32 | importer.index());
    if (got == m_importersThatGotOriginal.end() || !got->value.isGivenItNoMore || !isMocked(mocked))
        return false;
    if (got->value.timesMocked != m_timesMocked.get(mocked) && !got->value.staysUnderKeyOfOriginal)
        return false;
    if (!m_modulesThatRanBesideMock.get(mocked.index()) && !m_mocksOverModulesThatHaveRun.contains(mocked))
        return false;
    auto anew = m_momentsLoadedAnew.find(importer);
    return status(importer) > ModuleStatus::Linked && (anew == m_momentsLoadedAnew.end() || anew->value.moment <= got->value.moment);
}

bool Modules::isLinkedToOriginalOf(ModuleID fileOrModule, ModuleID importer) const
{
    if (m_importersThatGotOriginal.isEmpty()) [[likely]]
        return false;
    ModuleID mocked = fileOf(fileOrModule);
    auto got = m_importersThatGotOriginal.find(static_cast<uint64_t>(mocked.index()) << 32 | importer.index());
    if (got == m_importersThatGotOriginal.end() || !got->value.isGivenByHost)
        return false;
    if (got->value.isGivenItNoMore && got->value.timesMocked != m_timesMocked.get(mocked) && !got->value.staysUnderKeyOfOriginal)
        return false;
    auto anew = m_momentsLoadedAnew.find(importer);
    return anew == m_momentsLoadedAnew.end() || anew->value.moment <= got->value.moment;
}

bool Modules::isOfPackage(ModuleID module) const
{
    String path = m_table.pathText(module);
    return path.contains("/node_modules/"_s) || path.contains("\\node_modules\\"_s);
}

bool Modules::wasLoadedUnderKeyOfOriginal(ModuleID fileOrModule) const
{
    ModuleID mocked = fileOf(fileOrModule);
    if (m_importersThatGotOriginal.isEmpty() || (!isMocked(mocked) && !isOfPackage(mocked)))
        return false;
    for (auto& got : m_importersThatGotOriginal) {
        if (got.key >> 32 == mocked.index() && got.value.isGivenByHost)
            return true;
    }
    return false;
}

void Modules::didLeaveOriginalUnderKeyOfOriginal(ModuleID fileOrModule)
{
    uint64_t mocked = fileOf(fileOrModule).index();
    for (auto& got : m_importersThatGotOriginal) {
        if (got.key >> 32 == mocked)
            got.value.staysUnderKeyOfOriginal = true;
    }
}

void Modules::noteWhereOriginalLiesBehindEndOfMock(ModuleID fileOrModule)
{
    ModuleID module = moduleWithExportsOf(fileOrModule);
    if (m_modulesThatRanBesideMock.get(module.index()) && !m_modulesThatMockLeavesLoaded.get(module.index())) [[unlikely]]
        didLeaveOriginalUnderKeyOfOriginal(fileOrModule);
}

bool Modules::hasImporterBelowThatGetsOriginal(ModuleID root, ModuleID fileOrFace) const
{
    ModuleID file = fileOf(fileOrFace);
    if (!isMocked(file) || isLeftAloneByMock(file) || moduleWithExportsOf(file) == file)
        return false;
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        ModuleID importer = toVisit.takeLast();
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (request.isHoisted || request.isRequire)
                continue;
            ModuleID imported = moduleLoadedBy(request);
            if (fileOf(imported) == file) {
                if (importsOriginalBesideMock(file, importer))
                    return true;
                continue;
            }
            if (isMocked(imported) && !runsBesideMockFor(imported, importer))
                continue;
            if (data(imported).isESModuleOfProgram() && status(imported) < ModuleStatus::Evaluating && !seen.quickSet(imported.index()))
                toVisit.append(imported);
        }
    }
    return false;
}

void Modules::runOriginalsOwedTo(ModuleID importer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!data(importer).isESModuleOfProgram())
        return;
    for (const ModuleRequestData& request : m_table.imports(importer)) {
        if (request.isHoisted || request.isRequire)
            continue;
        ModuleID file = fileOf(moduleLoadedBy(request));
        if (!mockInPlace(file) || isLeftAloneByMock(file) || moduleWithExportsOf(file) == file || !importsOriginalBesideMock(file, importer))
            continue;
        runFileBesideMock(file);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void Modules::runFileBesideMock(ModuleID file)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID face = moduleWithExportsOf(file);
    JSObject* mock = mockInPlace(file);
    ASSERT(mock && face != file && !isLeftAloneByMock(file));
    JSValue whatRequireAnswers = commonJSExports(file);
    JSValue original = answerRequireWithOriginal(file, face);
    RETURN_IF_EXCEPTION(scope, void());
    LoadedModule& loaded = ensureLoaded(file);
    if (mockInPlace(file) != mock || loaded.exportsOfOriginalBesideMock.get() != original)
        return;
    LoadedModule& exporting = ensureLoaded(face);
    auto setInPlace = [&](JSObject* standing) {
        loaded.mockInPlace.set(*this, standing);
        exporting.mockInPlace.set(*this, standing);
        m_numberOfMocksInPlace += standing ? 1 : -1;
        m_modulesWithMockInPlace.set(file.index(), !!standing);
    };
    setInPlace(nullptr);
    JSValue settledAnswer = loaded.requiredValue.get();
    exporting.capturedExports.set(*this, nullptr);
    exporting.exportsWhatMockNamesAlone = false;
    setNamespace(face, ModulePhase::Evaluation, nullptr);
    setCommonJSExports(file, original);
    loaded.requiredValue.set(*this, JSValue());
    captureExports(face);
    if (scope.exception()) [[unlikely]] {
        setInPlace(mock);
        setCommonJSExports(file, whatRequireAnswers);
        loaded.requiredValue.set(*this, settledAnswer);
        return;
    }
    VMState& state = m_instance.vm->aotState();
    auto laid = m_mocksOverModulesThatHaveRun.set(file, MockOverModuleThatHasRun { momentOfFirstMockInTestFile(file), state.momentOfTestFile });
    laid.iterator->value.momentThisWasLaid = laid.iterator->value.momentLaid;
    laid.iterator->value.didFileRunBesideMock = true;
    scope.release();
    setMockOverModuleThatHasRun(file, mock, whatRequireAnswers);
}

bool Modules::hasRunItself(ModuleID mocked) const
{
    return m_modulesThatRanBesideMock.get(mocked.index()) || m_modulesThatHadRunWhenMocked.get(mocked.index());
}

bool Modules::isMockedByFactoryThatTakesOriginal(ModuleID module) const
{
    return m_modulesWithMockThatAsksAgain.get(module.index()) && isMocked(module);
}

bool Modules::isGivenOriginalBesideMock(ModuleID file, ModuleID asker, LoadByName way)
{
    if (!asker || !m_table.hasFilePath(file) || !m_table.hasFilePath(fileOf(asker)))
        return false;
    if (way == LoadByName::Require)
        return host().isLeftToItselfByMock && host().isLeftToItselfByMock(m_instance.globalObject, m_table.pathText(file), m_table.pathText(fileOf(asker)));
    return host().givesOriginalBesideMock && host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(file), m_table.pathText(fileOf(asker)), true);
}

bool Modules::getsOriginalBesideMock(ModuleID target, ModuleID referrer)
{
    if (!m_numberOfMocksInPlace || !host().givesOriginalBesideMock) [[likely]]
        return false;
    if (!referrer || data(target).kind != ModuleKind::CommonJs || !mockInPlace(target) || !m_table.hasFilePath(referrer))
        return false;
    return host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(target), m_table.pathText(referrer), false);
}

JSValue Modules::answerRequireWithOriginal(ModuleID file, ModuleID referrer)
{
    ASSERT_UNUSED(referrer, referrer && mockInPlace(file));
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (JSValue original = ensureLoaded(file).exportsOfOriginalBesideMock.get())
        return original;
    if (status(file) == ModuleStatus::Evaluating)
        return commonJSExports(file);
    JSObject* mockBefore = mockInPlace(file);
    ensureLoaded(file).answerOfMockWhileOriginalRuns.set(*this, commonJSExports(file));
    JSValue settledAnswer = ensureLoaded(file).requiredValue.get();
    setStatus(file, ModuleStatus::Unlinked);
    setCommonJSExports(file, jsUndefined());
    ensureLoaded(file).requiredValue.set(*this, JSValue());
    m_filesWhoseOriginalRuns.set(file.index());
    evaluateCommonJS(file);
    m_filesWhoseOriginalRuns.clear(file.index());
    bool hasThrown = !!scope.exception();
    JSValue original = commonJSExports(file);
    JSValue answerOfMock = ensureLoaded(file).answerOfMockWhileOriginalRuns.get();
    ensureLoaded(file).answerOfMockWhileOriginalRuns.set(*this, JSValue());
    JSObject* mock = mockInPlace(file);
    if (hasThrown)
        forgetRunThatThrew(file);
    if (!mock)
        return hasThrown ? JSValue() : original;
    markEvaluated(file);
    setCommonJSExports(file, answerOfMock);
    if (mock == mockBefore)
        ensureLoaded(file).requiredValue.set(*this, settledAnswer);
    if (hasThrown)
        return { };
    ensureLoaded(file).exportsOfOriginalBesideMock.set(*this, original);
    takeNamesOfMockInPlace(moduleWithExportsOf(file), mock);
    RETURN_IF_EXCEPTION(scope, { });
    return original;
}

JSObject* Modules::namespaceOfModuleThatRanBesideMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    if (JSObject* own = loaded.namespaceBeforeMockInPlace.get())
        return own;
    JSObject* ofMock = namespaceIfCreated(module, ModulePhase::Evaluation);
    setNamespace(module, ModulePhase::Evaluation, nullptr);
    JSObject* own = nullptr;
    {
        SetForScope makesIt(m_moduleWhoseOwnNamespaceIsMade, std::optional(module));
        own = createNamespace(module, ModulePhase::Evaluation, true);
    }
    setNamespace(module, ModulePhase::Evaluation, ofMock);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (auto* object = dynamicDowncast<ModuleNamespace>(own)) {
        object->setKeepsToModule(true);
        loaded.namespaceBeforeMockInPlace.set(*this, object);
    }
    return own;
}

static JSC_DECLARE_HOST_FUNCTION(answerImportWithModuleItselfInTestRunOriginals);
static JSC_DECLARE_HOST_FUNCTION(importModuleItselfBehindFactoryInTestRunOriginals);

JSC_DEFINE_HOST_FUNCTION(answerImportWithModuleItselfInTestRunOriginals, (JSGlobalObject* realm, CallFrame* callFrame))
{
    return JSValue::encode(modulesOf(realm)->namespaceOfModuleItself(ModuleID(callFrame->argument(1).asUInt32())));
}

bool Modules::isAskedForByOwnFactory(ModuleID module)
{
    if (!isMocked(module) || data(module).isOfLibrary() || !data(module).hasExportTable() || data(module).isFaceOfFile()) [[likely]]
        return false;
    if (m_modulesWhoseFactoryRuns.get(module.index()) && !(m_isImportThatAsksWrittenByBodyBelowFileAskedFor && m_moduleThatImportThatAsksNames == module))
        return true;
    return !mockInPlace(module) && !m_modulesWithMockToRun.get(module.index()) && host().isCalledFromFactoryThatWaits && host().isCalledFromFactoryThatWaits(m_instance.globalObject, module);
}

void Modules::noteLoadBegunByFactories(ModuleID root)
{
    for (size_t index : m_mocks->modules) {
        if (ModuleID mocked(index); mocked != root && !hasRunItself(mocked) && isAskedForByOwnFactory(mocked))
            m_modulesThatOwnFactoryMayLoad.set(index);
    }
}

bool Modules::runsBesideMockFor(ModuleID mocked, std::optional<ModuleID> importer) const
{
    if (!isMocked(mocked) || hasRunItself(mocked) || data(mocked).kind == ModuleKind::CommonJs) [[likely]]
        return false;
    if (m_modulesThatOwnFactoryMayLoad.get(mocked.index()))
        return true;
    return importer ? importsOriginalBesideMock(mocked, *importer) : m_modulesOwedBesideMock.get(mocked.index());
}

void Modules::setAsideErrorOfModuleThatRanBesideMock(ModuleID module)
{
    if (status(module) != ModuleStatus::Failed || !isMocked(module))
        return;
    ModuleRecord* record = recordIfExists(module);
    ensureLoaded(module).errorOfModuleThatRanBesideMock.set(*this, record->evaluationError());
    record->setEvaluationError(*m_instance.vm, JSValue());
    setStatus(module, ModuleStatus::Evaluated);
}

void Modules::bringBackErrorOfModuleThatRanBesideMock(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    JSValue error = loaded ? loaded->errorOfModuleThatRanBesideMock.get() : JSValue();
    if (!error) [[likely]]
        return;
    loaded->errorOfModuleThatRanBesideMock.set(*this, JSValue());
    setFailed(module, error);
}

static JSC_DECLARE_HOST_FUNCTION(failImportOfModuleItselfInTestRunOriginals);

JSC_DEFINE_HOST_FUNCTION(failImportOfModuleItselfInTestRunOriginals, (JSGlobalObject* realm, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    modulesOf(realm)->setAsideErrorOfModuleThatRanBesideMock(ModuleID(callFrame->argument(1).asUInt32()));
    return throwVMError(realm, scope, callFrame->argument(0));
}

JSC_DEFINE_HOST_FUNCTION(importModuleItselfBehindFactoryInTestRunOriginals, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didGoOnBehindFactories();
    JSArray* heldCall = asArray(callFrame->argument(1));
    return JSValue::encode(modulesOf(realm)->importModuleItselfBesideMock(ModuleID(heldCall->getIndexQuickly(0).asUInt32()), heldCall->getIndexQuickly(1)));
}

void Modules::oweBodyBesideMock(ModuleID module)
{
    m_modulesOwedBesideMock.set(module.index());
    if (status(module) != ModuleStatus::Evaluated)
        return;
    setStatus(module, ModuleStatus::Unlinked);
    if (ModuleRecord* record = recordIfExists(module))
        record->clearTopLevelCapability();
}

void Modules::letLoadsInFlightKeepRecordOf(ModuleID module)
{
    const ModuleData& moduleData = data(module);
    if (!moduleData.isESModuleOfProgram() || moduleData.isFaceOfFile() || mockInPlace(module))
        return;
    ModuleRecord* record = m_records.get(module);
    if (!record || record->hasLiveExports() || record->keepsToModule())
        return;
    if (record->status() != CyclicModuleRecord::Status::New && record->status() != CyclicModuleRecord::Status::Unlinked)
        return;
    ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(module));
    if (!entry || (entry->record() && entry->record() != record))
        return;
    for (size_t below : reachedByStatements(module, AtMock::GoesOn)) {
        if (data(ModuleID(below)).hasTopLevelAwait())
            return;
    }
    if (storedStatus(module) == StoredModuleStatus::InRecord)
        setStoredStatus(module, StoredModuleStatus::Unlinked);
    loader()->removeEntryOfRecord(record);
    takeOutRecord(module);
    record->setKeepsToModule();
    m_modulesKeptByLoadsInFlight.set(module.index());
}

void Modules::evaluateModuleItself(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    scope.release();
    if (isMocked(module))
        requireModuleItselfBesideMock(module);
    else
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
}
JSValue Modules::requireModuleItselfBesideMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (ModuleRecord* record = recordSetAside(module); (record && record->status() == CyclicModuleRecord::Status::EvaluatingAsync) || (hasRunItself(module) && status(module) == ModuleStatus::EvaluatingAsync) || m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) { return pair.required == module && pair.mocked != module && pair.call == callOfFactoryThatHasNotSettled(pair.mocked); })) [[unlikely]] {
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, makeString(m_table.pathText(module), "?actual"_s)));
        return { };
    }
    if (!hasRunItself(module)) {
        oweBodyBesideMock(module);
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        if (scope.exception()) [[unlikely]] {
            m_modulesOwedBesideMock.clear(module.index());
            setAsideErrorOfModuleThatRanBesideMock(module);
            return { };
        }
    }
    if (JSValue error = ensureLoaded(module).errorOfModuleThatRanBesideMock.get()) [[unlikely]] {
        throwException(m_instance.globalObject, scope, error);
        return { };
    }
    JSObject* own = namespaceOfModuleItself(module);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue answer = whatRequireMakesOfNamespace(own);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* names = uncheckedDowncast<ModuleNamespace>(own); !names->hasExport(m_instance.vm->propertyNames->__esModule.impl()))
        names->setESModuleMarker(true);
    return answer;
}

uint64_t Modules::callOfFactoryThatHasNotSettled(ModuleID module, bool* refusesEveryRequire) const
{
    if (!host().callOfFactoryThatHasNotSettled)
        return 0;
    bool refuses = false;
    uint64_t call = host().callOfFactoryThatHasNotSettled(m_instance.globalObject, module, refuses);
    if (refusesEveryRequire)
        *refusesEveryRequire = refuses;
    return refuses || data(module).isESModuleOfProgram() || (data(module).kind == ModuleKind::CommonJs && !data(module).isOfLibrary()) ? call : 0;
}

bool Modules::waitsForFactoryThatLoadInFlightCalled(ModuleID module) const
{
    if (!isMocked(module) || !data(module).isESModuleOfProgram() || data(module).isFaceOfFile())
        return false;
    if (mockInPlace(module) || m_modulesWithMockToRun.get(module.index()) || !callOfFactoryThatHasNotSettled(module))
        return false;
    return !m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) { return pair.mocked == module; });
}

void Modules::askLoaderForNamesBelow(ModuleID required)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work { required };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (seen.quickSet(module.index()) || isMocked(module) || hasBegunToLoad(module))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (request.isHoisted || request.isRequire)
                continue;
            if (data(request.targetModule()).isUncompiledCode()) {
                resolvedKey(request.targetModule());
                RETURN_IF_EXCEPTION(scope, void());
            }
            work.append(moduleLoadedBy(request));
        }
    }
}

void Modules::noteThatRequireGaveUpOn(ModuleID required, bool hasGivenUp)
{
    BitVector reached = reachedByStatements(required, AtMock::Stops);
    if (isMocked(required)) {
        for (const ModuleRequestData& request : m_table.requests(required))
            reached.merge(reachedByStatements(moduleLoadedBy(request), AtMock::Stops));
    }
    for (size_t mocked : m_modulesWithMockThatWaited) {
        if (!reached.get(mocked))
            continue;
        bool isRefusedEveryTime = false;
        uint64_t call = callOfFactoryThatHasNotSettled(ModuleID(mocked), &isRefusedEveryTime);
        if (!hasGivenUp && !isRefusedEveryTime)
            continue;
        RequireThatGaveUp pair { hasGivenUp ? required : ModuleID(mocked), ModuleID(mocked), call, isRefusedEveryTime };
        if (pair.call && !m_requiresThatGaveUp.contains(pair))
            m_requiresThatGaveUp.append(pair);
    }
}

JSValue Modules::answerRequireWhileFactoryHasNotSettled(ModuleID target, ModuleID requirer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_requiresThatGaveUp.removeAllMatching([&](const RequireThatGaveUp& pair) {
        return pair.call != callOfFactoryThatHasNotSettled(pair.mocked) || mockInPlace(pair.mocked);
    });
    auto standsAsItRan = [&](ModuleID file) {
        return hasRun(file) && !isEvicted(file) && !m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) { return m_timesMocked.get(pair.mocked) > 1; });
    };
    if (m_requiresThatGaveUp.isEmpty() || (!isMocked(target) && standsAsItRan(target)))
        return { };
    auto givesUpAgain = [&] {
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.pathText(target)));
        return JSValue();
    };
    auto wasGivenUpOn = [&](ModuleID mocked) {
        return m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) { return pair.mocked == mocked; });
    };
    auto cannotBeRequired = [&](ModuleID module) {
        if (m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) { return pair.mocked == module && pair.isRefusedEveryTime; }))
            return true;
        return data(module).hasTopLevelAwait() || status(module) == ModuleStatus::EvaluatingAsync;
    };
    if (!isMocked(target) && !m_importOfFactorysOwnAsks && m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) { return pair.required == target && pair.mocked != target; })) {
        if (!m_instance.vm->m_synchronousModuleQueue) {
            refuseRequireAgain(target, target);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return givesUpAgain();
    }
    if (!isMocked(target) && !m_importOfFactorysOwnAsks) {
        for (const RequireThatGaveUp& pair : m_requiresThatGaveUp) {
            if (pair.required == pair.mocked || !reachedByStatements(pair.required, AtMock::Stops).get(target.index()) || !reachedByStatements(target, AtMock::Stops).get(pair.mocked.index()))
                continue;
            ModuleID above = pair.required;
            if (!m_instance.vm->m_synchronousModuleQueue) {
                refuseRequireAgain(target, above);
                RETURN_IF_EXCEPTION(scope, { });
            }
            return givesUpAgain();
        }
        BitVector belowTarget = reachedByStatements(target, AtMock::Stops);
        for (const WaitThatRequireLeft& left : m_waitsThatRequiresLeft) {
            if (left.wait->status() != JSPromise::Status::Pending)
                continue;
            ModuleID above = left.required;
            BitVector belowLoadThatWasLeft = reachedByStatements(above, AtMock::Stops);
            bool passesThroughLoadThatWasLeft = false;
            for (size_t index : belowTarget) {
                ModuleID file(index);
                if (file == above || file == target || isMocked(fileOf(file)) || standsAsItRan(file) || !belowLoadThatWasLeft.get(index))
                    continue;
                BitVector belowFile = reachedByStatements(file, AtMock::Stops);
                if (m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) { return belowFile.get(pair.mocked.index()); })) {
                    passesThroughLoadThatWasLeft = true;
                    break;
                }
            }
            if (!passesThroughLoadThatWasLeft)
                continue;
            if (!m_instance.vm->m_synchronousModuleQueue) {
                refuseRequireAgain(target, above);
                RETURN_IF_EXCEPTION(scope, { });
            }
            return givesUpAgain();
        }
    }
    auto isAskedByStranger = [&](ModuleID mocked) {
        return m_requirerOfRequireUnderWay && host().wroteFactoryThatHasNotSettled && !host().wroteFactoryThatHasNotSettled(m_instance.globalObject, fileOf(m_requirerOfRequireUnderWay), mocked);
    };
    auto isMetByWhatHasYetToRun = [&](ModuleID mocked) {
        BitVector seen(m_table.size());
        Vector<ModuleID, 8> work { target };
        while (!work.isEmpty()) {
            ModuleID file = work.takeLast();
            if (seen.quickSet(file.index()))
                continue;
            for (const ModuleRequestData& request : m_table.imports(file)) {
                if (request.isHoisted || request.isDeferred)
                    continue;
                ModuleID imported = moduleLoadedBy(request);
                if (fileOf(imported) == mocked)
                    return true;
                if (!isMocked(fileOf(imported)) && status(imported) < ModuleStatus::Evaluating)
                    work.append(imported);
            }
        }
        return false;
    };
    auto refuseStranger = [&](ModuleID mocked) {
        ModuleID above = target;
        for (const RequireThatGaveUp& pair : m_requiresThatGaveUp) {
            if (pair.mocked == mocked && pair.required != mocked) {
                above = pair.required;
                break;
            }
        }
        if (target != mocked)
            noteThatRequireGaveUpOn(target, true);
        if (!m_instance.vm->m_synchronousModuleQueue)
            refuseRequireAgain(target, above);
    };
    if (wasGivenUpOn(target)) {
        bool isAskedFromLoadThatWasLeft = requirer && m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) {
            return pair.mocked == target && pair.required != requirer && reachedByStatements(pair.required, AtMock::Stops).get(requirer.index());
        });
        if (cannotBeRequired(target) || isAskedFromLoadThatWasLeft)
            return givesUpAgain();
        if (isAskedByStranger(target)) {
            refuseStranger(target);
            RETURN_IF_EXCEPTION(scope, { });
            return givesUpAgain();
        }
        RELEASE_AND_RETURN(scope, requireModuleItselfBesideMock(target));
    }
    if (callOfFactoryThatHasNotSettled(target))
        return { };
    for (size_t below : reachedByStatements(target, AtMock::Stops)) {
        ModuleID mocked(below);
        if (!wasGivenUpOn(mocked))
            continue;
        if (cannotBeRequired(mocked))
            return givesUpAgain();
        if (isAskedByStranger(mocked) && isMetByWhatHasYetToRun(mocked)) {
            refuseStranger(mocked);
            RETURN_IF_EXCEPTION(scope, { });
            return givesUpAgain();
        }
        if (data(mocked).kind != ModuleKind::CommonJs) {
            requireModuleItselfBesideMock(mocked);
            RETURN_IF_EXCEPTION(scope, { });
        }
        noteFilesLinkedToOriginalBelowRequire(target, mocked);
    }
    return { };
}

void Modules::noteFilesLinkedToOriginalBelowRequire(ModuleID required, ModuleID mocked)
{
    Vector<Vector<uint32_t, 2>> importersOf(m_table.size());
    for (size_t importer : reachedByStatements(required, AtMock::Stops)) {
        if (isMocked(ModuleID(importer)))
            continue;
        for (const ModuleRequestData& request : m_table.requests(ModuleID(importer)))
            importersOf[moduleLoadedBy(request).index()].append(importer);
    }
    BitVector isNoted(m_table.size());
    Vector<uint32_t, 16> toFollow { mocked.index() };
    while (!toFollow.isEmpty()) {
        for (uint32_t importer : importersOf[toFollow.takeLast()]) {
            if (isNoted.quickSet(importer))
                continue;
            m_filesRequiredWithOriginalWhileFactoryRan.append({ ModuleID(importer), mocked });
            toFollow.append(importer);
        }
    }
}

bool Modules::isImportOfFactorysOwnBehindRequireThatGaveUp(ModuleID module, ModuleID referrer)
{
    if (!host().wroteFactoryThatHasNotSettled || isMocked(fileOf(module)))
        return false;
    m_requiresThatGaveUp.removeAllMatching([&](const RequireThatGaveUp& pair) {
        return pair.call != callOfFactoryThatHasNotSettled(pair.mocked) || mockInPlace(pair.mocked);
    });
    bool comesToMockThatWasGivenUpOn = false;
    for (size_t below : reachedByStatements(module, AtMock::Stops)) {
        ModuleID mocked = fileOf(ModuleID(below));
        if (!m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& pair) { return pair.mocked == mocked; }))
            continue;
        if (hasBegunToLoad(mocked) || !host().wroteFactoryThatHasNotSettled(m_instance.globalObject, fileOf(referrer), mocked))
            return false;
        comesToMockThatWasGivenUpOn = true;
    }
    return comesToMockThatWasGivenUpOn;
}

JSValue Modules::continueImportAsFactorysOwn(ModuleID module, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!isImportOfFactorysOwnBehindRequireThatGaveUp(module, referrer)) {
        SetForScope importOfFactorysOwnHasItsText(m_importOfFactorysOwnHasItsText, true);
        RELEASE_AND_RETURN(scope, import(module, referrer, jsUndefined()));
    }
    SetForScope importOfFactorysOwnAsks(m_importOfFactorysOwnAsks, true);
    bool anotherFactoryHasNotSettled = runMocksBelow(module);
    RETURN_IF_EXCEPTION(scope, { });
    if (anotherFactoryHasNotSettled) {
        m_importOfFactorysOwnAsks = false;
        SetForScope importOfFactorysOwnHasItsText(m_importOfFactorysOwnHasItsText, true);
        RELEASE_AND_RETURN(scope, import(module, referrer, jsUndefined()));
    }
    require(module, referrer, RequiredBy::Program);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* names = moduleNamespace(module, ModulePhase::Evaluation, false);
    RETURN_IF_EXCEPTION(scope, { });
    m_filesRequiredWithOriginalWhileFactoryRan.removeAllMatching([&](const auto& pair) {
        if (pair.first != module)
            return false;
        if (wouldBeEvicted(fileOf(module)))
            evict(module);
        return true;
    });
    return names;
}

JSValue Modules::importModuleItselfBesideMock(ModuleID module, JSValue answerOfImport)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isMocked(fileOf(module)))
        return answerOfImport;
    if (isCommonJSFileOrFace(module)) {
        if (mockInPlace(module) && !isLeftAloneByMock(fileOf(module))) {
            runFileBesideMock(fileOf(module));
            RETURN_IF_EXCEPTION(scope, { });
        }
        RELEASE_AND_RETURN(scope, namespaceOfModuleItself(moduleWithExportsOf(fileOf(module))));
    }
    bool hasBegun = hasRunItself(module);
    JSPromise* underWay = evaluationOfRecordSetAside(module);
    RETURN_IF_EXCEPTION(scope, { });
    if (hasBegun && !underWay) {
        underWay = evaluationUnderWayBelow(module);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!hasBegun || underWay || status(module) == ModuleStatus::EvaluatingAsync) {
        if (!hasBegun) {
            oweBodyBesideMock(module);
            bool factoryHasNotSettled = runMocksBelow(module);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue stateOfWait;
            JSPromise* factory = factoryHasNotSettled ? factoryThatWaitsBelow(module, FactoryAskedForBy::ImportCall, stateOfWait) : nullptr;
            RETURN_IF_EXCEPTION(scope, { });
            if (factory) [[unlikely]] {
                MarkedArgumentBuffer call;
                call.append(jsNumber(module.index()));
                call.append(answerOfImport);
                JSArray* heldCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
                RETURN_IF_EXCEPTION(scope, { });
                JSPromise* answered = JSPromise::create(vm, realm->promiseStructure());
                JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importModuleItselfBehindFactoryInTestRunOriginals, ImplementationVisibility::Private);
                factory->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), answered, heldCall);
                return answered;
            }
        }
        JSPromise* evaluation = underWay ? underWay : evaluate(module, std::nullopt, nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
        if (evaluation->status() != JSPromise::Status::Fulfilled) {
            JSPromise* answered = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* answer = JSFunction::create(vm, realm, 2, String(), answerImportWithModuleItselfInTestRunOriginals, ImplementationVisibility::Private);
            JSFunction* fail = JSFunction::create(vm, realm, 2, String(), failImportOfModuleItselfInTestRunOriginals, ImplementationVisibility::Private);
            evaluation->performPromiseThenWithContext(vm, realm, answer, fail, answered, jsNumber(module.index()));
            setAsideErrorOfModuleThatRanBesideMock(module);
            return answered;
        }
    }
    if (JSValue error = ensureLoaded(module).errorOfModuleThatRanBesideMock.get()) [[unlikely]] {
        throwException(realm, scope, error);
        return { };
    }
    RELEASE_AND_RETURN(scope, namespaceOfModuleItself(module));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

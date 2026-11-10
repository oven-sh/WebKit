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

bool Modules::keepsOriginalAfterRemoval(ModuleID mocked, ModuleID importer) const
{
    if (m_importersLinkedToOriginal.isEmpty()) [[likely]]
        return false;
    auto got = m_importersLinkedToOriginal.find(static_cast<uint64_t>(mocked.index()) << 32 | importer.index());
    if (got == m_importersLinkedToOriginal.end() || (got->value.isGivenItNoMore && !got->value.staysUnderActualKey))
        return false;
    for (const ModuleRequestData& request : m_table.imports(importer)) {
        if (fileOf(moduleLoadedBy(request)) == mocked)
            return true;
    }
    return false;
}

bool Modules::importsOriginalDespiteMock(ModuleID fileOrModule, ModuleID importer, OriginalRequester askedBy) const
{
    ModuleID mocked = fileOf(fileOrModule);
    if (!importer)
        return false;
    if (!isMocked(mocked))
        return askedBy == OriginalRequester::Statement && keepsOriginalAfterRemoval(mocked, importer);
    if (!host().shouldBypassMock || !m_table.hasFilePath(mocked) || !m_table.hasFilePath(importer))
        return false;
    bool isFile = data(mocked).kind == ModuleKind::CommonJs && !data(mocked).isOfLibrary();
    if (isFile ? moduleWithExportsOf(mocked) == mocked || fileOf(importer) == mocked : !data(mocked).isESModuleOfProgram() && data(mocked).kind != ModuleKind::Data)
        return false;
    uint64_t pair = static_cast<uint64_t>(mocked.index()) << 32 | importer.index();
    if (auto gotMock = askedBy == OriginalRequester::Statement ? m_importersLinkedToMock.find(pair) : m_importersLinkedToMock.end(); gotMock != m_importersLinkedToMock.end()) {
        if (momentLinked(importer) <= gotMock->value)
            return false;
        m_importersLinkedToMock.remove(gotMock);
    }
    auto asked = m_importersAskedAboutOriginal.add(pair, AnswersOfHostOnOriginal { });
    if (asked.isNewEntry) {
        asked.iterator->value.isGiven = host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(mocked), m_table.fullPath(importer), true);
        asked.iterator->value.staysLinked = host().wasLinkedBeforeMock(m_instance.globalObject, m_table.fullPath(mocked), m_table.fullPath(importer));
    }
    if (askedBy == OriginalRequester::ImportCall && !asked.iterator->value.isGiven)
        return false;
    bool getsOriginal = asked.iterator->value.isGiven || asked.iterator->value.staysLinked;
    if (getsOriginal)
        m_importersLinkedToOriginal.set(asked.iterator->key, GotOriginal { m_instance.vm->aotState().momentOfLoads, false, m_timesMocked.get(mocked), false, asked.iterator->value.isGiven });
    else if (auto got = m_importersLinkedToOriginal.find(asked.iterator->key); got != m_importersLinkedToOriginal.end())
        got->value.isGivenItNoMore = true;
    else if (askedBy == OriginalRequester::Statement && status(importer) > ModuleStatus::Linked)
        m_importersLinkedToMock.add(pair, m_instance.vm->aotState().momentOfLoads);
    return getsOriginal;
}

bool Modules::keepsOriginalAfterReplacement(ModuleID fileOrModule, ModuleID importer) const
{
    if (m_importersLinkedToOriginal.isEmpty()) [[likely]]
        return false;
    ModuleID mocked = fileOf(fileOrModule);
    auto got = m_importersLinkedToOriginal.find(static_cast<uint64_t>(mocked.index()) << 32 | importer.index());
    if (got == m_importersLinkedToOriginal.end() || !got->value.isGivenItNoMore || !isMocked(mocked))
        return false;
    if (got->value.timesMocked != m_timesMocked.get(mocked) && !got->value.staysUnderActualKey)
        return false;
    if (!m_originalsEvaluatedUnderMock.get(mocked.index()) && !m_mocksWithIntactOriginal.contains(mocked))
        return false;
    auto anew = m_momentsLoadedAnew.find(importer);
    return status(importer) > ModuleStatus::Linked && (anew == m_momentsLoadedAnew.end() || anew->value.moment <= got->value.moment);
}

bool Modules::isLinkedToOriginalOf(ModuleID fileOrModule, ModuleID importer) const
{
    if (m_importersLinkedToOriginal.isEmpty()) [[likely]]
        return false;
    ModuleID mocked = fileOf(fileOrModule);
    auto got = m_importersLinkedToOriginal.find(static_cast<uint64_t>(mocked.index()) << 32 | importer.index());
    if (got == m_importersLinkedToOriginal.end() || !got->value.isGivenByHost)
        return false;
    if (got->value.isGivenItNoMore && got->value.timesMocked != m_timesMocked.get(mocked) && !got->value.staysUnderActualKey)
        return false;
    auto anew = m_momentsLoadedAnew.find(importer);
    return anew == m_momentsLoadedAnew.end() || anew->value.moment <= got->value.moment;
}

bool Modules::isOfPackage(ModuleID module) const
{
    String path = m_table.fullPath(module);
    return path.contains("/node_modules/"_s) || path.contains("\\node_modules\\"_s);
}

bool Modules::wasLoadedUnderActualKey(ModuleID fileOrModule) const
{
    ModuleID mocked = fileOf(fileOrModule);
    if (m_importersLinkedToOriginal.isEmpty() || (!isMocked(mocked) && !isOfPackage(mocked)))
        return false;
    for (auto& got : m_importersLinkedToOriginal) {
        if (got.key >> 32 == mocked.index() && got.value.isGivenByHost)
            return true;
    }
    return false;
}

void Modules::keepUnderActualKey(ModuleID fileOrModule)
{
    uint64_t mocked = fileOf(fileOrModule).index();
    for (auto& got : m_importersLinkedToOriginal) {
        if (got.key >> 32 == mocked)
            got.value.staysUnderActualKey = true;
    }
}

void Modules::noteWhereOriginalLiesBehindEndOfMock(ModuleID fileOrModule)
{
    ModuleID module = moduleWithExportsOf(fileOrModule);
    if (m_originalsEvaluatedUnderMock.get(module.index()) && !m_originalsRetainedAfterMock.get(module.index())) [[unlikely]]
        keepUnderActualKey(fileOrModule);
}

bool Modules::hasBypassingImporterBelow(ModuleID root, ModuleID moduleOrFacade) const
{
    ModuleID file = fileOf(moduleOrFacade);
    if (!isMocked(file) || hasIntactOriginal(file) || moduleWithExportsOf(file) == file)
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
                if (importsOriginalDespiteMock(file, importer))
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
        if (!activeMock(file) || hasIntactOriginal(file) || moduleWithExportsOf(file) == file || !importsOriginalDespiteMock(file, importer))
            continue;
        runFileBesideMock(file);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void Modules::keepOriginalIntact(ModuleID file)
{
    auto kept = m_mocksWithIntactOriginal.set(file, MockWithIntactOriginal { momentOfFirstMockInTestFile(file), m_instance.vm->aotState().momentOfTestFile });
    kept.iterator->value.momentThisWasLaid = kept.iterator->value.momentLaid;
    kept.iterator->value.didFileRunBesideMock = true;
}

void Modules::evaluatePastPendingMock(ModuleID facade)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID file = fileOf(facade);
    bool wasEvaluatedByRequire = status(file) == ModuleStatus::Evaluated && m_modulesEvaluatedBeforeMock.get(file.index());
    if (file == facade || !isMocked(file) || activeMock(file) || hasIntactOriginal(file) || (status(file) >= ModuleStatus::Evaluating && !wasEvaluatedByRequire))
        return;
    didRemoveMock(file);
    bool wasMockPending = m_modulesWithPendingMock.get(file.index());
    m_modulesWithPendingMock.clear(file.index());
    if (!wasEvaluatedByRequire)
        evaluateCommonJS(file);
    if (scope.exception()) [[unlikely]] {
        if (!wasImportedDuringEvaluation(file))
            discardFailedEvaluation(file);
    } else
        captureExports(facade);
    m_modulesWithPendingMock.set(file.index(), wasMockPending);
    willMock(file);
    ensureRecord(file);
    if (scope.exception() || status(file) != ModuleStatus::Evaluated)
        return;
    m_originalsEvaluatedUnderMock.set(file.index());
    keepOriginalIntact(file);
}

void Modules::runFileBesideMock(ModuleID file)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID facade = moduleWithExportsOf(file);
    JSObject* mock = activeMock(file);
    ASSERT(mock && facade != file && !hasIntactOriginal(file));
    JSValue requireReturnValue = commonJSExports(file);
    JSValue original = answerRequireWithOriginal(file, facade);
    RETURN_IF_EXCEPTION(scope, void());
    LoadedModule& loaded = ensureLoaded(file);
    if (activeMock(file) != mock || loaded.exportsOfOriginalBesideMock.get() != original)
        return;
    LoadedModule& exporting = ensureLoaded(facade);
    setMockInPlace(file, nullptr);
    JSValue settledAnswer = loaded.requiredValue.get();
    exporting.capturedExports.set(*this, nullptr);
    bool hasEarlyCapture = exporting.earlyCapturedExports || exporting.earlyCapturedFields;
    if (hasEarlyCapture) {
        swapEarlyCapture(facade);
        exporting.earlyCapturedExports.set(*this, nullptr);
        exporting.earlyDeferredExportsSource.set(*this, nullptr);
        exporting.earlyCapturedFields.set(*this, nullptr);
        exporting.earlySourceOfCapturedFields.set(*this, nullptr);
    }
    exporting.exportsWhatMockNamesAlone = false;
    exporting.namespaceBeforeMockInPlace.set(*this, nullptr);
    setNamespace(facade, ModulePhase::Evaluation, nullptr);
    setCommonJSExports(file, original);
    loaded.requiredValue.set(*this, JSValue());
    captureExports(facade);
    if (hasEarlyCapture && !scope.exception())
        fillCapturedExports(facade);
    if (scope.exception()) [[unlikely]] {
        setMockInPlace(file, mock);
        setCommonJSExports(file, requireReturnValue);
        loaded.requiredValue.set(*this, settledAnswer);
        return;
    }
    keepOriginalIntact(file);
    scope.release();
    setMockWithIntactOriginal(file, mock, requireReturnValue);
}

bool Modules::wasOriginalEvaluated(ModuleID mocked) const
{
    return m_originalsEvaluatedUnderMock.get(mocked.index()) || m_originalsEvaluatedWhenMocked.get(mocked.index());
}

bool Modules::isMockedByFactoryThatTakesOriginal(ModuleID module) const
{
    return m_mocksRetriedAfterFailure.get(module.index()) && isMocked(module);
}

bool Modules::isMockBypassedFor(ModuleID file, ModuleID asker, LoadKind way)
{
    if (!asker || !m_table.hasFilePath(file) || !m_table.hasFilePath(fileOf(asker)))
        return false;
    if (way == LoadKind::Require)
        return host().isMockHiddenFromRequirer && host().isMockHiddenFromRequirer(m_instance.globalObject, m_table.fullPath(file), m_table.fullPath(fileOf(asker)));
    return host().shouldBypassMock && host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(file), m_table.fullPath(fileOf(asker)), true);
}

void Modules::setMockInPlace(ModuleID file, JSObject* mock)
{
    ensureLoaded(file).activeMock.set(*this, mock);
    ensureLoaded(moduleWithExportsOf(file)).activeMock.set(*this, mock);
    m_activeMockCount += mock ? 1 : -1;
    m_modulesWithActiveMock.set(file.index(), !!mock);
}

void Modules::swapEarlyCapture(ModuleID facade)
{
    LoadedModule& exporting = ensureLoaded(facade);
    JSObject* exports = exporting.capturedExports.get();
    JSObject* deferred = exporting.deferredExportsSource.get();
    JSCellButterfly* fields = exporting.capturedFields.get();
    JSObject* source = exporting.sourceOfCapturedFields.get();
    exporting.capturedExports.set(*this, exporting.earlyCapturedExports.get());
    exporting.deferredExportsSource.set(*this, exporting.earlyDeferredExportsSource.get());
    exporting.capturedFields.set(*this, exporting.earlyCapturedFields.get());
    exporting.sourceOfCapturedFields.set(*this, exporting.earlySourceOfCapturedFields.get());
    exporting.earlyCapturedExports.set(*this, exports);
    exporting.earlyDeferredExportsSource.set(*this, deferred);
    exporting.earlyCapturedFields.set(*this, fields);
    exporting.earlySourceOfCapturedFields.set(*this, source);
}

void Modules::captureEvaluatingRequirer(ModuleID requirer, ModuleID target)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID facade = moduleWithExportsOf(requirer);
    if (data(requirer).kind != ModuleKind::CommonJs || facade == requirer || status(requirer) != ModuleStatus::Evaluating || !data(facade).capturesExports())
        return;
    JSObject* mock = m_originalsBeingEvaluated.get(requirer.index()) ? activeMock(requirer) : nullptr;
    LoadedModule& exporting = ensureLoaded(facade);
    if (mock ? exporting.earlyCapturedExports || exporting.earlyCapturedFields : didCaptureExports(facade))
        return;
    if (!transitiveImportersOf(requirer).quickGet(target.index()))
        return;
    if (!mock)
        RELEASE_AND_RETURN(scope, captureExports(facade));
    bool namesMockAlone = std::exchange(exporting.exportsWhatMockNamesAlone, false);
    swapEarlyCapture(facade);
    setMockInPlace(requirer, nullptr);
    captureExports(facade);
    setMockInPlace(requirer, mock);
    swapEarlyCapture(facade);
    exporting.exportsWhatMockNamesAlone = namesMockAlone;
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, fillCapturedExports(facade));
}

bool Modules::requireBypassesMock(ModuleID mocked, ModuleID requirer)
{
    if (!host().isMockHiddenFromRequirer || !host().isMockHiddenFromRequirer(m_instance.globalObject, m_table.fullPath(mocked), m_table.fullPath(requirer)))
        return false;
    m_requirersBypassingMock.set(static_cast<uint64_t>(mocked.index()) << 32 | requirer.index(), m_timesMocked.get(mocked));
    return true;
}

bool Modules::getsOriginalBesideMock(ModuleID target, ModuleID referrer)
{
    if (!m_activeMockCount || !host().shouldBypassMock) [[likely]]
        return false;
    if (!referrer || data(target).kind != ModuleKind::CommonJs || !activeMock(target) || !m_table.hasFilePath(referrer))
        return false;
    return host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(target), m_table.fullPath(referrer), false);
}

JSValue Modules::answerRequireWithOriginal(ModuleID file, ModuleID referrer)
{
    ASSERT_UNUSED(referrer, referrer && activeMock(file));
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (JSValue original = ensureLoaded(file).exportsOfOriginalBesideMock.get())
        return original;
    if (status(file) == ModuleStatus::Evaluating)
        return commonJSExports(file);
    JSObject* mockBefore = activeMock(file);
    ensureLoaded(file).answerOfMockWhileOriginalRuns.set(*this, commonJSExports(file));
    JSValue settledAnswer = ensureLoaded(file).requiredValue.get();
    setStatus(file, ModuleStatus::Unlinked);
    setCommonJSExports(file, jsUndefined());
    ensureLoaded(file).requiredValue.set(*this, JSValue());
    m_originalsBeingEvaluated.set(file.index());
    evaluateCommonJS(file);
    m_originalsBeingEvaluated.clear(file.index());
    bool hasThrown = !!scope.exception();
    JSValue original = commonJSExports(file);
    JSValue answerOfMock = ensureLoaded(file).answerOfMockWhileOriginalRuns.get();
    ensureLoaded(file).answerOfMockWhileOriginalRuns.set(*this, JSValue());
    JSObject* mock = activeMock(file);
    if (hasThrown)
        discardFailedEvaluation(file);
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

static JSC_DECLARE_HOST_FUNCTION(didFulfillOriginalEvaluation);
static JSC_DECLARE_HOST_FUNCTION(importModuleItselfBehindFactoryInTestRunOriginals);

JSC_DEFINE_HOST_FUNCTION(didFulfillOriginalEvaluation, (JSGlobalObject* realm, CallFrame* callFrame))
{
    return JSValue::encode(modulesOf(realm)->resolveImportWithOriginal(ModuleID(callFrame->argument(1).asUInt32())));
}

bool Modules::isRequestedByOwnFactory(ModuleID module)
{
    if (!isMocked(module) || data(module).isOfLibrary() || !data(module).hasExportTable() || data(module).isFacade()) [[likely]]
        return false;
    if (m_modulesWithRunningFactory.get(module.index()) && !(m_isCurrentImportFromDependency && m_currentImportRequestedModule == module))
        return true;
    return !activeMock(module) && !m_modulesWithPendingMock.get(module.index()) && host().isRequestedByPendingFactory && host().isRequestedByPendingFactory(m_instance.globalObject, module);
}

void Modules::noteLoadBegunByFactories(ModuleID root)
{
    for (size_t index : m_mocks->modules) {
        ModuleID mocked(index);
        if (mocked == root || !isRequestedByOwnFactory(mocked))
            continue;
        if (!wasOriginalEvaluated(mocked))
            m_modulesThatOwnFactoryMayLoad.set(index);
        collectImportersBetween(root, mocked, m_factoryLoadedImporters);
    }
}

bool Modules::runsBesideMockFor(ModuleID mocked, std::optional<ModuleID> importer) const
{
    if (!isMocked(mocked) || wasOriginalEvaluated(mocked) || data(mocked).kind == ModuleKind::CommonJs) [[likely]]
        return false;
    if (m_modulesThatOwnFactoryMayLoad.get(mocked.index()))
        return true;
    return importer ? importsOriginalDespiteMock(mocked, *importer) : m_originalsDueUnderMock.get(mocked.index());
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

static JSC_DECLARE_HOST_FUNCTION(didRejectOriginalEvaluation);

JSC_DEFINE_HOST_FUNCTION(didRejectOriginalEvaluation, (JSGlobalObject* realm, CallFrame* callFrame))
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    modulesOf(realm)->setAsideErrorOfModuleThatRanBesideMock(ModuleID(callFrame->argument(1).asUInt32()));
    return throwVMError(realm, scope, callFrame->argument(0));
}

JSC_DEFINE_HOST_FUNCTION(importModuleItselfBehindFactoryInTestRunOriginals, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didResumeAfterFactories();
    JSArray* deferredCall = asArray(callFrame->argument(1));
    return JSValue::encode(modulesOf(realm)->importModuleItselfBesideMock(ModuleID(deferredCall->getIndexQuickly(0).asUInt32()), deferredCall->getIndexQuickly(1)));
}

void Modules::markOriginalDue(ModuleID module)
{
    m_originalsDueUnderMock.set(module.index());
    if (status(module) != ModuleStatus::Evaluated)
        return;
    setStatus(module, ModuleStatus::Unlinked);
    if (ModuleRecord* record = recordIfExists(module))
        record->clearTopLevelCapability();
}

bool Modules::isLeftByFailedLoad(ModuleID module) const
{
    bool wasReached = false;
    for (uint32_t index = 0; index < m_table.size() && !wasReached; ++index) {
        ModuleID importer(index);
        ModuleRecord* record = m_records.get(importer);
        wasReached = record && record->hasUnresolvableRequest() && status(importer) <= ModuleStatus::Linked && modulesInLoadGraph(importer).get(module.index());
    }
    if (!wasReached || m_rootAwaitingLoader)
        return false;
    for (const LoadInFlight& load : m_loadsInFlight) {
        if (m_loadsInFlight.isBeingLoaded(load) && modulesInLoadGraph(load.root).get(module.index()))
            return false;
    }
    return true;
}

void Modules::retainRecordForInFlightLoads(ModuleID module)
{
    const ModuleData& moduleData = data(module);
    if (!moduleData.isESModuleOfProgram() || moduleData.isFacade() || activeMock(module) || isLeftByFailedLoad(module))
        return;
    ModuleRecord* record = m_records.get(module);
    if (!record || record->hasLiveExports() || record->keepsToModule())
        return;
    if (record->status() != CyclicModuleRecord::Status::New && record->status() != CyclicModuleRecord::Status::Unlinked)
        return;
    ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(module));
    if (!entry || (entry->record() && entry->record() != record))
        return;
    for (size_t below : staticImportClosure(module, MockTraversal::Continue)) {
        if (data(ModuleID(below)).hasTopLevelAwait())
            return;
    }
    if (storedStatus(module) == StoredModuleStatus::InRecord)
        setStoredStatus(module, StoredModuleStatus::Unlinked);
    loader()->removeEntriesForRecord(record);
    takeOutRecord(module);
    record->setKeepsToModule();
    m_modulesRetainedByInFlightLoads.set(module.index());
}

void Modules::evaluateModuleItself(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    scope.release();
    if (isMocked(module))
        requireOriginalUnderMock(module);
    else
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
}
JSValue Modules::requireOriginalUnderMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    size_t detached = m_detachedLoads.findIf([&](const DetachedLoad& left) { return left.isForOriginal && left.required == module && left.wait->status() == JSPromise::Status::Pending; });
    if (ModuleRecord* record = displacedRecord(module); detached != notFound || (record && record->status() == CyclicModuleRecord::Status::EvaluatingAsync) || (wasOriginalEvaluated(module) && status(module) == ModuleStatus::EvaluatingAsync) || m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.required == module && pair.mocked != module && pair.call == pendingFactoryCallID(pair.mocked); })) [[unlikely]] {
        if (detached != notFound)
            m_detachedLoads[detached].wait->performPromiseThen(*m_instance.vm, m_instance.globalObject, jsUndefined(), jsUndefined(), JSPromise::create(*m_instance.vm, m_instance.globalObject->promiseStructure()));
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, makeString(m_table.fullPath(module), "?actual"_s)));
        return { };
    }
    if (!wasOriginalEvaluated(module)) {
        markOriginalDue(module);
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        if (scope.exception()) [[unlikely]] {
            m_originalsDueUnderMock.clear(module.index());
            setAsideErrorOfModuleThatRanBesideMock(module);
            return { };
        }
    }
    if (JSValue error = ensureLoaded(module).errorOfModuleThatRanBesideMock.get()) [[unlikely]] {
        throwException(m_instance.globalObject, scope, error);
        return { };
    }
    JSObject* own = originalNamespace(module);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue answer = whatRequireMakesOfNamespace(own);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* names = uncheckedDowncast<ModuleNamespace>(own); !names->hasExport(m_instance.vm->propertyNames->__esModule.impl()))
        names->setESModuleMarker(true);
    return answer;
}

uint64_t Modules::pendingFactoryCallID(ModuleID module, bool* shouldRejectEveryRequire) const
{
    if (!host().pendingFactoryCallID)
        return 0;
    bool refuses = false;
    uint64_t call = host().pendingFactoryCallID(m_instance.globalObject, module, refuses);
    if (shouldRejectEveryRequire)
        *shouldRejectEveryRequire = refuses;
    return refuses || data(module).isESModuleOfProgram() || (data(module).kind == ModuleKind::CommonJs && !data(module).isOfLibrary()) ? call : 0;
}

bool Modules::hasPendingFactoryFromLoad(ModuleID module) const
{
    if (!isMocked(module) || !data(module).isESModuleOfProgram() || data(module).isFacade())
        return false;
    if (activeMock(module) || m_modulesWithPendingMock.get(module.index()) || !pendingFactoryCallID(module))
        return false;
    return !m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.mocked == module; });
}

void Modules::resolveUncompiledImports(ModuleID required)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work { required };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (seen.quickSet(module.index()) || isMocked(module) || hasStartedLoading(module))
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

void Modules::recordRejectedRequire(ModuleID required, bool wasRejected)
{
    BitVector reached = staticImportClosure(required, MockTraversal::Stop);
    if (isMocked(required)) {
        for (const ModuleRequestData& request : m_table.requests(required))
            reached.merge(staticImportClosure(moduleLoadedBy(request), MockTraversal::Stop));
    }
    for (size_t mocked : m_modulesWithDeferredMock) {
        if (!reached.get(mocked))
            continue;
        bool isAlwaysRejected = false;
        uint64_t call = pendingFactoryCallID(ModuleID(mocked), &isAlwaysRejected);
        if (!wasRejected && !isAlwaysRejected)
            continue;
        RejectedRequire pair { wasRejected ? required : ModuleID(mocked), ModuleID(mocked), call, isAlwaysRejected };
        if (pair.call && !m_rejectedRequires.contains(pair))
            m_rejectedRequires.append(pair);
    }
    if (LoadedModule* loaded = wasRejected ? loadedIfExists(required) : nullptr; loaded && loaded->requiredValue) {
        loaded->requiredValueBeforeRefusal.set(*this, loaded->requiredValue.get());
        loaded->requiredValue.set(*this, JSValue());
    }
}

JSValue Modules::answerRequireWhileFactoryHasNotSettled(ModuleID target, ModuleID requirer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_rejectedRequires.removeAllMatching([&](const RejectedRequire& pair) {
        return pair.call != pendingFactoryCallID(pair.mocked) || activeMock(pair.mocked);
    });
    auto isEvaluationValid = [&](ModuleID file) {
        return hasRun(file) && !isEvicted(file) && !m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return m_timesMocked.get(pair.mocked) > 1; });
    };
    if (m_rejectedRequires.isEmpty() || (!isMocked(target) && isEvaluationValid(target)))
        return { };
    auto throwAsyncModuleError = [&] {
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.fullPath(target)));
        return JSValue();
    };
    auto isBlockingMock = [&](ModuleID mocked) {
        return m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.mocked == mocked; });
    };
    auto cannotBeRequired = [&](ModuleID module) {
        if (m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.mocked == module && pair.isAlwaysRejected; }))
            return true;
        return data(module).hasTopLevelAwait() || status(module) == ModuleStatus::EvaluatingAsync;
    };
    if (!isMocked(target) && !m_isResumingFactoryChainImport && m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.required == target && pair.mocked != target; })) {
        if (!m_instance.vm->m_synchronousModuleQueue) {
            willRejectRequireAgain(target, target);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return throwAsyncModuleError();
    }
    if (!isMocked(target) && !m_isResumingFactoryChainImport) {
        for (const RejectedRequire& pair : m_rejectedRequires) {
            if (pair.required == pair.mocked || !staticImportClosure(pair.required, MockTraversal::Stop).get(target.index()) || !staticImportClosure(target, MockTraversal::Stop).get(pair.mocked.index()))
                continue;
            ModuleID above = pair.required;
            if (!m_instance.vm->m_synchronousModuleQueue) {
                willRejectRequireAgain(target, above);
                RETURN_IF_EXCEPTION(scope, { });
            }
            return throwAsyncModuleError();
        }
        BitVector belowTarget = staticImportClosure(target, MockTraversal::Stop);
        for (const DetachedLoad& left : m_detachedLoads) {
            if (left.wait->status() != JSPromise::Status::Pending)
                continue;
            ModuleID above = left.required;
            BitVector detachedLoadClosure = staticImportClosure(above, MockTraversal::Stop);
            bool passesThroughDetachedLoad = false;
            for (size_t index : belowTarget) {
                ModuleID file(index);
                if (file == above || file == target || isMocked(fileOf(file)) || isEvaluationValid(file) || !detachedLoadClosure.get(index))
                    continue;
                BitVector belowFile = staticImportClosure(file, MockTraversal::Stop);
                if (m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return belowFile.get(pair.mocked.index()); })) {
                    passesThroughDetachedLoad = true;
                    break;
                }
            }
            if (!passesThroughDetachedLoad)
                continue;
            if (!m_instance.vm->m_synchronousModuleQueue) {
                willRejectRequireAgain(target, above);
                RETURN_IF_EXCEPTION(scope, { });
            }
            return throwAsyncModuleError();
        }
    }
    auto isRequirerOutsideFactoryChain = [&](ModuleID mocked) {
        return m_currentRequirer && host().isInPendingFactoryChain && !host().isInPendingFactoryChain(m_instance.globalObject, fileOf(m_currentRequirer), mocked);
    };
    auto hasUnevaluatedImporter = [&](ModuleID mocked) {
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
    auto rejectOutsideRequirer = [&](ModuleID mocked) {
        ModuleID above = target;
        for (const RejectedRequire& pair : m_rejectedRequires) {
            if (pair.mocked == mocked && pair.required != mocked) {
                above = pair.required;
                break;
            }
        }
        if (target != mocked)
            recordRejectedRequire(target, true);
        if (!m_instance.vm->m_synchronousModuleQueue)
            willRejectRequireAgain(target, above);
    };
    if (isBlockingMock(target)) {
        bool isRequestedFromDetachedLoad = requirer && m_rejectedRequires.containsIf([&](const RejectedRequire& pair) {
            return pair.mocked == target && pair.required != requirer && staticImportClosure(pair.required, MockTraversal::Stop).get(requirer.index());
        });
        if (cannotBeRequired(target) || isRequestedFromDetachedLoad)
            return throwAsyncModuleError();
        if (isRequirerOutsideFactoryChain(target)) {
            rejectOutsideRequirer(target);
            RETURN_IF_EXCEPTION(scope, { });
            return throwAsyncModuleError();
        }
        RELEASE_AND_RETURN(scope, requireOriginalUnderMock(target));
    }
    if (pendingFactoryCallID(target))
        return { };
    for (size_t below : staticImportClosure(target, MockTraversal::Stop)) {
        ModuleID mocked(below);
        if (!isBlockingMock(mocked))
            continue;
        if (cannotBeRequired(mocked))
            return throwAsyncModuleError();
        if (isRequirerOutsideFactoryChain(mocked) && hasUnevaluatedImporter(mocked)) {
            rejectOutsideRequirer(mocked);
            RETURN_IF_EXCEPTION(scope, { });
            return throwAsyncModuleError();
        }
        if (data(mocked).kind != ModuleKind::CommonJs) {
            requireOriginalUnderMock(mocked);
            RETURN_IF_EXCEPTION(scope, { });
        }
        collectImportersBetween(target, mocked, m_requiredImportersToEvict);
    }
    return { };
}

void Modules::collectImportersBetween(ModuleID root, ModuleID mocked, Vector<std::pair<ModuleID, ModuleID>>& importers)
{
    Vector<Vector<uint32_t, 2>> importersOf(m_table.size());
    for (size_t importer : staticImportClosure(root, MockTraversal::Stop)) {
        if (isMocked(ModuleID(importer)))
            continue;
        for (const ModuleRequestData& request : m_table.requests(ModuleID(importer)))
            importersOf[moduleLoadedBy(request).index()].append(importer);
    }
    BitVector isCollected(m_table.size());
    Vector<uint32_t, 16> toFollow { mocked.index() };
    while (!toFollow.isEmpty()) {
        for (uint32_t importer : importersOf[toFollow.takeLast()]) {
            if (isCollected.quickSet(importer))
                continue;
            importers.append({ ModuleID(importer), mocked });
            toFollow.append(importer);
        }
    }
}

bool Modules::isFactoryChainImport(ModuleID module, ModuleID referrer)
{
    if (!host().isInPendingFactoryChain || isMocked(fileOf(module)))
        return false;
    m_rejectedRequires.removeAllMatching([&](const RejectedRequire& pair) {
        return pair.call != pendingFactoryCallID(pair.mocked) || activeMock(pair.mocked);
    });
    bool reachesBlockingMock = false;
    for (size_t below : staticImportClosure(module, MockTraversal::Stop)) {
        ModuleID mocked = fileOf(ModuleID(below));
        if (!m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.mocked == mocked; }))
            continue;
        if (hasStartedLoading(mocked) || !host().isInPendingFactoryChain(m_instance.globalObject, fileOf(referrer), mocked))
            return false;
        reachesBlockingMock = true;
    }
    return reachesBlockingMock;
}

JSValue Modules::resumeFactoryChainImport(ModuleID module, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!isFactoryChainImport(module, referrer)) {
        SetForScope hasFactoryChainImportSource(m_hasFactoryChainImportSource, true);
        RELEASE_AND_RETURN(scope, import(module, referrer, jsUndefined()));
    }
    SetForScope isResumingFactoryChainImport(m_isResumingFactoryChainImport, true);
    bool hasOtherPendingFactory = runMocksInGraph(module);
    RETURN_IF_EXCEPTION(scope, { });
    if (hasOtherPendingFactory) {
        m_isResumingFactoryChainImport = false;
        SetForScope hasFactoryChainImportSource(m_hasFactoryChainImportSource, true);
        RELEASE_AND_RETURN(scope, import(module, referrer, jsUndefined()));
    }
    require(module, referrer, RequiredBy::Program);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* names = moduleNamespace(module, ModulePhase::Evaluation, false);
    RETURN_IF_EXCEPTION(scope, { });
    m_requiredImportersToEvict.removeAllMatching([&](const auto& pair) {
        if (pair.first != module)
            return false;
        if (wouldBeEvicted(fileOf(module)))
            evict(module);
        return true;
    });
    return names;
}

JSValue Modules::resolveImportWithOriginal(ModuleID module)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* underWay = pendingEvaluationInGraph(module);
    RETURN_IF_EXCEPTION(scope, { });
    if (!underWay)
        RELEASE_AND_RETURN(scope, originalNamespace(module));
    JSPromise* answered = JSPromise::create(vm, realm->promiseStructure());
    JSFunction* answer = JSFunction::create(vm, realm, 2, String(), didFulfillOriginalEvaluation, ImplementationVisibility::Private);
    JSFunction* fail = JSFunction::create(vm, realm, 2, String(), didRejectOriginalEvaluation, ImplementationVisibility::Private);
    underWay->performPromiseThenWithContext(vm, realm, answer, fail, answered, jsNumber(module.index()));
    return answered;
}

JSValue Modules::importModuleItselfBesideMock(ModuleID module, JSValue answerOfImport)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isMocked(fileOf(module)))
        return answerOfImport;
    if (isCommonJSModuleOrFacade(module)) {
        if (activeMock(module) && !hasIntactOriginal(fileOf(module))) {
            runFileBesideMock(fileOf(module));
            RETURN_IF_EXCEPTION(scope, { });
        }
        RELEASE_AND_RETURN(scope, originalNamespace(moduleWithExportsOf(fileOf(module))));
    }
    bool hasBegun = wasOriginalEvaluated(module);
    JSObject* original = hasBegun ? nullptr : actualKeyNamespace(fileOf(module));
    RETURN_IF_EXCEPTION(scope, { });
    if (original)
        return original;
    JSPromise* underWay = displacedRecordEvaluation(module);
    RETURN_IF_EXCEPTION(scope, { });
    if (hasBegun && !underWay) {
        evaluateDueDependencies(module);
        RETURN_IF_EXCEPTION(scope, { });
        underWay = pendingEvaluationInGraph(module);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!hasBegun || underWay || status(module) == ModuleStatus::EvaluatingAsync) {
        if (!hasBegun) {
            markOriginalDue(module);
            bool factoryHasNotSettled = runMocksInGraph(module);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue importWaitState;
            JSPromise* factory = factoryHasNotSettled ? pendingFactoryInGraph(module, PendingFactoryRequester::ImportCall, importWaitState) : nullptr;
            RETURN_IF_EXCEPTION(scope, { });
            if (factory) [[unlikely]] {
                MarkedArgumentBuffer call;
                call.append(jsNumber(module.index()));
                call.append(answerOfImport);
                JSArray* deferredCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
                RETURN_IF_EXCEPTION(scope, { });
                JSPromise* answered = JSPromise::create(vm, realm->promiseStructure());
                JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importModuleItselfBehindFactoryInTestRunOriginals, ImplementationVisibility::Private);
                factory->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), answered, deferredCall);
                return answered;
            }
        }
        JSPromise* evaluation = underWay ? underWay : evaluate(module, std::nullopt, nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
        if (evaluation->status() != JSPromise::Status::Fulfilled) {
            JSPromise* answered = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* answer = JSFunction::create(vm, realm, 2, String(), didFulfillOriginalEvaluation, ImplementationVisibility::Private);
            JSFunction* fail = JSFunction::create(vm, realm, 2, String(), didRejectOriginalEvaluation, ImplementationVisibility::Private);
            evaluation->performPromiseThenWithContext(vm, realm, answer, fail, answered, jsNumber(module.index()));
            setAsideErrorOfModuleThatRanBesideMock(module);
            return answered;
        }
    }
    if (JSValue error = ensureLoaded(module).errorOfModuleThatRanBesideMock.get()) [[unlikely]] {
        throwException(realm, scope, error);
        return { };
    }
    RELEASE_AND_RETURN(scope, originalNamespace(module));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

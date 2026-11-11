/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTEnvironment.h"
#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleRecord.h"
#include "AOTProgramStatics.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTState.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSModuleRecord.h"
#include "ModuleRegistryEntry.h"
#include "TopExceptionScope.h"
#include <wtf/Scope.h>
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
    if (!host().shouldBypassMock)
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
    if (!m_importersAskedAboutOriginal.contains(pair) && (!m_table.hasFilePath(mocked) || !m_table.hasFilePath(importer)))
        return false;
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

bool Modules::isExternalizedPackage(ModuleID module) const
{
    return isOfPackage(module) && (!data(module).isESModuleOfProgram() || !host().isESModuleInNode || host().isESModuleInNode(m_instance.globalObject, m_table.fullPath(module)));
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

bool Modules::hasUnevaluatedImporterInGraph(ModuleID root, ModuleID file) const
{
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        for (const ModuleRequestData& request : m_table.imports(toVisit.takeLast())) {
            if (request.isHoisted || request.isRequire)
                continue;
            ModuleID imported = moduleLoadedBy(request);
            if (fileOf(imported) == file)
                return true;
            if (data(imported).isESModuleOfProgram() && !isMocked(imported) && status(imported) <= ModuleStatus::Evaluating && !seen.quickSet(imported.index()))
                toVisit.append(imported);
        }
    }
    return false;
}

bool Modules::isDisplacedRunKept(ModuleID facade, ModuleID root) const
{
    if (m_displacedRunImporters.isEmpty()) [[likely]]
        return false;
    ModuleID file = fileOf(facade);
    return file != facade && root != facade && m_displacedRunImporters.contains(file.index()) && !hasUnevaluatedImporterInGraph(root, file);
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
    didChangeLinkState();
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
    JSValue original;
    if (!wasEvaluatedByRequire && m_filesWithCompiledOriginal.get(file.index()) && commonJSOriginalNamedBy(actualKey(file)) && host().requireOriginal) {
        if (status(file) == ModuleStatus::Unlinked)
            link(file);
        RETURN_IF_EXCEPTION(scope, void());
        original = host().requireOriginal(m_instance.globalObject, file, true);
        RETURN_IF_EXCEPTION(scope, void());
    }
    didRemoveMock(file);
    bool wasMockPending = m_modulesWithPendingMock.get(file.index());
    m_modulesWithPendingMock.clear(file.index());
    if (original) {
        setCommonJSExports(file, original);
        markEvaluated(file);
        didEvaluateAsOriginal(file);
    } else if (!wasEvaluatedByRequire)
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
    JSObject* own = namespaceInInstance(module, ModulePhase::Evaluation);
    if (own == ofMock)
        ofMock = nullptr;
    if (!own) {
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

bool Modules::isFileRequestedByOwnFactory(ModuleID file)
{
    const ModuleData& fileData = data(file);
    if (fileData.kind != ModuleKind::CommonJs || fileData.isOfLibrary() || !isMocked(file) || activeMock(file)) [[likely]]
        return false;
    return m_modulesWithRunningFactory.get(file.index()) || (!m_modulesWithPendingMock.get(file.index()) && host().isRequestedByPendingFactory && host().isRequestedByPendingFactory(m_instance.globalObject, file));
}

void Modules::restoreOwnFactoryOriginal(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    JSValue original = loaded && !loaded->isPatchedByMock ? loaded->exportsFromOwnFactory.get() : JSValue();
    if (!original || status(file) > ModuleStatus::Linked || hasIntactOriginal(file) || !isFileRequestedByOwnFactory(file)) [[likely]]
        return;
    setCommonJSExports(file, original);
    markEvaluated(file);
}

JSPromise* Modules::importLoadedOriginal(ModuleID file)
{
    if (status(file) >= ModuleStatus::Evaluating)
        return nullptr;
    ModuleRegistryEntry* entry = loader()->registryEntry(actualKey(file));
    if ((isOfPackage(file) || !commonJSOriginalNamedBy(actualKey(file))) && !m_originalsRequiredFromSource.get(file.index()) && !(entry && entry->record()))
        return nullptr;
    return loader()->requestImportModule(m_instance.globalObject, actualKey(file), moduleKey(file), nullptr, nullptr);
}

bool Modules::isRequestedByOwnFactory(ModuleID module)
{
    if (!isMocked(module) || data(module).isOfLibrary() || !data(module).hasExportTable() || data(module).isFacade()) [[likely]]
        return false;
    if (m_modulesWithRunningFactory.get(module.index()) && !(m_isCurrentImportFromDependency && m_currentImportRequestedModule == module))
        return true;
    return !activeMock(module) && !m_modulesWithPendingMock.get(module.index()) && host().isRequestedByPendingFactory && host().isRequestedByPendingFactory(m_instance.globalObject, module);
}

bool Modules::isRequestedDuringOwnPatch(ModuleID module)
{
    if (!m_modulesWithPendingMock.get(module.index()) || !hasInstances(module) || !isMocked(module) || activeMock(module) || status(module) > ModuleStatus::Linked) [[likely]]
        return false;
    return host().isRequestedByPendingFactory && host().isRequestedByPendingFactory(m_instance.globalObject, module);
}

void Modules::noteLoadBegunByFactories(ModuleID root)
{
    for (size_t index : m_mocks->modules) {
        ModuleID mocked(index);
        if (mocked == root)
            continue;
        if (isRequestedDuringOwnPatch(mocked) && !pendingFactoryCallID(mocked)) [[unlikely]] {
            Vector<std::pair<ModuleID, ModuleID>> importers;
            collectImportersBetween(root, mocked, importers);
            if (std::ranges::any_of(importers, [&](const auto& pair) { return status(pair.first) != ModuleStatus::Evaluated; }))
                m_patchesRequestedByOwnFile.add(root, ModuleIndexSet()).iterator->value.add(index);
            continue;
        }
        if (!isRequestedByOwnFactory(mocked))
            continue;
        if (!wasOriginalEvaluated(mocked))
            m_modulesThatOwnFactoryMayLoad.set(index);
        size_t first = m_factoryLoadedImporters.size();
        collectImportersBetween(root, mocked, m_factoryLoadedImporters);
        m_factoryLoadedImporters.removeAllMatching([&](const auto& pair) { return hasInstances(pair.first) && status(pair.first) == ModuleStatus::Evaluated && !isMocked(pair.first); }, first);
        for (size_t place = first; place < m_factoryLoadedImporters.size(); ++place)
            notePrivateCopy(m_factoryLoadedImporters[place].first, mocked);
    }
}

BitVector Modules::awaitedMocksBelow(ModuleID module, bool inCycleAlone)
{
    BitVector mocks = modulesInLoadGraph(module, LoadGraphMode::IgnoreMocks);
    mocks.filter(m_modulesWithPendingMock);
    mocks.clear(module.index());
    if (mocks.isEmpty() || !host().hasSynchronousFactory)
        return { };
    if (inCycleAlone) {
        listImportersAndRequirers();
        BitVector above(m_table.size());
        above.quickSet(module.index());
        Vector<ModuleID, 16> work { module };
        while (!work.isEmpty()) {
            for (uint32_t importer : m_importersByStatement[work.takeLast().index()]) {
                if (!above.quickSet(importer))
                    work.append(ModuleID(importer));
            }
        }
        mocks.filter(above);
    }
    BitVector awaited(m_table.size());
    for (size_t index : mocks) {
        ModuleID mocked(index);
        if (!host().hasSynchronousFactory(m_instance.globalObject, m_table.fullPath(fileOf(mocked)), data(mocked).isSynchronous()))
            awaited.quickSet(index);
    }
    return awaited;
}

bool Modules::mayImportAwaitItself(ModuleID module)
{
    if (m_modulesWithPendingMock.isEmpty())
        return false;
    auto countOriginals = [&](const BitVector& mocks) {
        unsigned count = 0;
        for (size_t mocked : mocks)
            count += needsOriginalBeforeFactory(ModuleID(mocked));
        return count;
    };
    return countOriginals(awaitedMocksBelow(module, true)) && countOriginals(awaitedMocksBelow(module, false)) > 1;
}

void Modules::tellOfFactoriesInCycle(ModuleID module)
{
    if (!m_modulesWithPendingMock.isEmpty() && host().didRunOriginal && !awaitedMocksBelow(module, true).isEmpty()) [[unlikely]]
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(module), OriginalRun::RunsAmongLoads);
}

bool Modules::tellIfMockedDuringFetch(ModuleID module)
{
    if (!m_modulesWithPendingMock.get(module.index()))
        return false;
    if (host().didRunOriginal)
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(module), OriginalRun::MockedDuringImport);
    return true;
}

bool Modules::hasPendingPatchInGraph(ModuleID root)
{
    BitVector reached = modulesInLoadGraph(root);
    reached.filter(m_modulesWithPendingMock);
    for (size_t index : reached) {
        if (ModuleID(index) != root && host().hasPendingPatch && host().hasPendingPatch(m_instance.globalObject, ModuleID(index)))
            return true;
    }
    return false;
}

bool Modules::tryDeferOwnPatch(ModuleID module)
{
    if (!mayImportAwaitItself(module) || !host().deferPatchToNextLoad || !host().deferPatchToNextLoad(m_instance.globalObject, module, true))
        return false;
    if (host().didRunOriginal)
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(module), OriginalRun::RunsAmongLoads);
    return true;
}

void Modules::reportPendingFactoriesInGraph(ModuleID root)
{
    if (m_modulesWithPendingMock.isEmpty() || !host().didRunOriginal || !host().hasSynchronousFactory)
        return;
    BitVector reached = modulesInLoadGraph(root);
    reached.filter(m_modulesWithPendingMock);
    for (size_t index : reached) {
        ModuleID mocked(index);
        if (mocked != root && !host().hasSynchronousFactory(m_instance.globalObject, m_table.fullPath(fileOf(mocked)), data(mocked).isSynchronous()))
            host().didRunOriginal(m_instance.globalObject, m_table.fullPath(root), OriginalRun::RunsAmongLoads);
    }
}

void Modules::reportPendingOwnPatches(ModuleID root)
{
    if (m_patchesRequestedByOwnFile.isEmpty()) [[likely]]
        return;
    for (uint32_t index : m_patchesRequestedByOwnFile.take(root)) {
        ModuleID mocked(index);
        if (isMockPending(mocked) && !activeMock(mocked) && host().didRunOriginal)
            host().didRunOriginal(m_instance.globalObject, m_table.fullPath(mocked), OriginalRun::RunsAmongLoads);
    }
}

void Modules::notePrivateCopy(ModuleID module, ModuleID mocked)
{
    auto isReachedFrom = [&](ModuleID root) {
        return modulesInLoadGraph(root).get(module.index());
    };
    bool isAwaited = (m_factoryCallImportTarget && isReachedFrom(m_factoryCallImportTarget)) || std::ranges::any_of(m_factoryBlockedImportRoots, isReachedFrom);
    if (isAwaited && m_mocksCalledAgain.get(mocked.index())) {
        auto isRequestedFrom = [&](ModuleID root) {
            FactoryCaller caller { root, mocked };
            return root == module || modulesRequestedBefore(caller).get(module.index());
        };
        if (!(m_factoryCallImportTarget && isRequestedFrom(m_factoryCallImportTarget)) && !std::ranges::any_of(m_factoryBlockedImportRoots, isRequestedFrom)) {
            m_modulesSharedWithFactories.set(module.index());
            return;
        }
    }
    if (isAwaited && m_modulesWithPrivateCopy.set(module.index()) && host().didRunOriginal)
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(module), OriginalRun::PrivateCopy);
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

bool Modules::wasAbandonedByFailedLoad(ModuleID module) const
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
    if (!moduleData.isESModuleOfProgram() || moduleData.isFacade() || activeMock(module) || wasAbandonedByFailedLoad(module))
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
String Modules::pathOfRefusedRequire(ModuleID module) const
{
    String path = m_table.fullPath(module);
    return m_originalRequiredByFactory == module ? makeString(path, "?actual"_s) : path;
}

void Modules::willReplaceRequiredDataFile(ModuleID file)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool isKept = tryKeepOriginalIntact(file, false, std::nullopt, true);
    RETURN_IF_EXCEPTION(scope, void());
    if (auto mock = m_mocksWithIntactOriginal.find(file); isKept && mock != m_mocksWithIntactOriginal.end()) {
        mock->value.replacesRequiredValue = true;
        if (auto requirers = m_requirersByFile.find(file.index()); requirers != m_requirersByFile.end()) {
            for (uint32_t requirer : requirers->value.ofLoad) {
                if (requirer >= m_table.size() || !runsTestFilesAgain())
                    requirers->value.ofMock.add(requirer);
            }
        }
    }
}

bool Modules::hasOnlyBeenRequired(ModuleID file)
{
    if (data(file).kind != ModuleKind::Data || hasBeenImported(file))
        return false;
    if (data(file).requireResult == RequireResult::NamespaceIfImported)
        return true;
    const ImportersOfModules& above = importersOfModules();
    for (uint32_t position = above.firstImporterOf[file.index()]; position < above.firstImporterOf[file.index() + 1]; ++position) {
        ModuleID importer(above.importers[position]);
        bool isImport = std::ranges::any_of(m_table.imports(importer), [&](const ModuleRequestData& request) {
            return request.targetModule() == file;
        });
        if (isImport && wasImporterLoaded(importer))
            return false;
    }
    return true;
}

void Modules::willImportEvictedDataOriginal(ModuleID file)
{
    if (!isEvictedDataFile(file))
        return;
    willImportOriginal(file);
    m_originalsEvaluatedByRequire.clear(file.index());
}

JSValue Modules::answerOwnFactoryRequire(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope originalRequiredByFactory(m_originalRequiredByFactory, module);
    RequireResult result = data(module).requireResult;
    if (result == RequireResult::NamespaceIfImported && (!wasOriginalEvaluated(module) || m_evictedDataOriginals.clear(module.index()) || (isEvictedDataFile(module) && !m_importedDataOriginals.get(module.index()))))
        m_originalsEvaluatedByRequire.set(module.index());
    JSValue answer = requireOriginalUnderMock(module);
    RETURN_IF_EXCEPTION(scope, { });
    if (hasInstances(module) && !m_requirersOfOwnOriginals.get(module.index()))
        m_originalsRequiredByFactories.set(module.index());
    if (result != RequireResult::DefaultExport && !(result == RequireResult::NamespaceIfImported && m_originalsEvaluatedByRequire.get(module.index())))
        return answer;
    JSObject* own = originalNamespace(module);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, readExportForRequire(own, m_instance.vm->propertyNames->defaultKeyword));
}

BitVector Modules::importClosureOfOriginal(ModuleID module) const
{
    BitVector reached(m_table.size());
    reached.quickSet(module.index());
    for (const ModuleRequestData& request : m_table.requests(module)) {
        if (ModuleID target = moduleLoadedBy(request); !isLinkedToReleasedInstance(module, target))
            reached.merge(staticImportClosure(target, MockTraversal::Stop));
    }
    return reached;
}

JSPromise* Modules::pendingFactoryInOriginalGraph(ModuleID original, JSValue& state)
{
    if (!host().nextPendingFactory)
        return nullptr;
    BitVector below = importClosureOfOriginal(original);
    below.quickClear(original.index());
    return host().nextPendingFactory(m_instance.globalObject, [&](ModuleID module) { return below.quickGet(module.index()); }, PendingFactoryRequester::DetachedLoad, state);
}

void Modules::detachRefusedOriginalLoad(ModuleID module)
{
    SuspendExceptionScope refusal(*m_instance.vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    if (!isStillBlockedOnFactories(module))
        return;
    startDetachedLoad(module, true);
    if (scope.exception()) [[unlikely]] {
        static_cast<void>(scope.tryClearException());
        return;
    }
    size_t detached = m_detachedLoads.findIf([&](const DetachedLoad& left) { return left.isForOriginal && left.required == module; });
    if (detached != notFound)
        m_detachedLoads[detached].isRefusedOriginal = true;
}

JSValue Modules::requireOriginalUnderMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (data(module).mayRunAsOriginal() && commonJSOriginalNamedBy(actualKey(module)) && host().requireOriginal) {
        JSValue original = host().requireOriginal(m_instance.globalObject, module, true);
        RETURN_IF_EXCEPTION(scope, { });
        if (original)
            return original;
    }
    auto findDetached = [&] {
        return m_detachedLoads.findIf([&](const DetachedLoad& left) { return left.isForOriginal && left.required == module && left.wait->status() == JSPromise::Status::Pending; });
    };
    size_t detached = findDetached();
    bool isCalledByImport = m_innermostFactoryCaller && m_innermostFactoryCaller->mocked == module;
    if (detached != notFound && isCalledByImport && m_detachedLoads[detached].hasWaitedForLoader) {
        evaluateDetachedOriginal(detached);
        RETURN_IF_EXCEPTION(scope, { });
        detached = findDetached();
    }
    if (ModuleRecord* record = displacedRecord(module); detached != notFound || (record && record->status() == CyclicModuleRecord::Status::EvaluatingAsync) || (wasOriginalEvaluated(module) && status(module) == ModuleStatus::EvaluatingAsync) || m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.required == module && pair.mocked != module && pair.call == pendingFactoryCallID(pair.mocked); })) [[unlikely]] {
        if (detached != notFound)
            m_detachedLoads[detached].wait->performPromiseThen(*m_instance.vm, m_instance.globalObject, jsUndefined(), jsUndefined(), JSPromise::create(*m_instance.vm, m_instance.globalObject->promiseStructure()));
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, makeString(m_table.fullPath(module), "?actual"_s)));
        return { };
    }
    if (!wasOriginalEvaluated(module)) {
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (request.isDeferred || request.isHoisted)
                continue;
            evaluateModulesOwedBelow(moduleLoadedBy(request));
            RETURN_IF_EXCEPTION(scope, { });
        }
        noteSecondInstanceOfOriginal(module);
        markOriginalDue(module);
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        m_failedInstancesBesideMocks.set(module.index(), !!scope.exception());
        if (scope.exception()) [[unlikely]] {
            detachRefusedOriginalLoad(module);
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
    if (activeMock(module) || m_modulesWithPendingMock.get(module.index()))
        return false;
    uint64_t call = pendingFactoryCallID(module);
    return call && !m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.mocked == module && pair.call == call; });
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
        if (call && wasRejected && required.index() != mocked) {
            uint64_t requireAndMock = static_cast<uint64_t>(required.index()) << 32 | mocked;
            auto earlier = m_callsOfRejectedRequires.add(requireAndMock, call);
            if (!earlier.isNewEntry && std::exchange(earlier.iterator->value, call) != call)
                m_requiresRejectedAgain.add(requireAndMock);
        }
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
    SetForScope currentImportRequestedModule(m_currentImportRequestedModule, module);
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
            noteSecondInstanceOfOriginal(module);
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

class CurrentInstanceScope {
public:
    CurrentInstanceScope(Modules& modules, ModuleID module, Environment* instance)
        : m_modules(modules)
        , m_module(module)
        , m_previous(modules.exchangeCurrentInstance(module, { instance, false, 0 }))
    {
    }

    ~CurrentInstanceScope() { m_modules.exchangeCurrentInstance(m_module, m_previous); }

private:
    Modules& m_modules;
    ModuleID m_module;
    Modules::CurrentInstance m_previous;
};

static ModuleNamespace* namespaceOfCompiledOriginal(JSModuleRecord* record)
{
    return record->isCompiledOriginal() ? uncheckedDowncast<ModuleNamespace>(record->liveExportsSource()) : nullptr;
}

std::optional<ModuleID> Modules::compiledOriginalNamedBy(const Identifier& key)
{
    std::optional<ModuleID> module = originalNamedBy(key);
    if (!module || key.string().contains("&actual"_s) || !hasInstances(*module) || !data(*module).isESModuleOfProgram() || data(*module).isFacade() || isEvicted(*module) || delegatesToHost(*module))
        return std::nullopt;
    return module;
}

std::optional<ModuleID> Modules::commonJSOriginalNamedBy(const Identifier& key)
{
    std::optional<ModuleID> module = originalNamedBy(key);
    if (!module || key.string().contains("&actual"_s))
        return std::nullopt;
    ModuleID file = fileOf(*module);
    if (!data(file).mayRunAsOriginal() || !data(file).bodyFunction() || isEvicted(file) || delegatesToHost(file))
        return std::nullopt;
    return file;
}

bool Modules::hasLoadedCommonJSFile()
{
    lookForMainCommonJSModule();
    return m_isMainCommonJSModuleDecided;
}

bool Modules::tryEvaluateCommonJSOriginal(const Identifier& key, const ArgList& arguments)
{
    std::optional<ModuleID> file = commonJSOriginalNamedBy(key);
    if (!file)
        return false;
    SetForScope commonJSOriginal(m_commonJSOriginal, file);
    SetForScope argumentsOfOriginal(m_argumentsOfOriginal, &arguments);
    SetForScope isInHostRequest(m_currentHostRequest, false);
    SetForScope isStatementHostRequest(m_isStatementHostRequest, false);
    if (!m_isMainCommonJSModuleDecided)
        decideMainCommonJSModule(std::nullopt);
    didStartThread(m_instance.vm);
    didChangeLinkState();
    uint8_t storedStatus = std::exchange(m_storedStatuses[file->index()], static_cast<uint8_t>(StoredModuleStatus::Linked));
    auto restoresStatus = makeScopeExit([&] {
        m_storedStatuses[file->index()] = storedStatus;
    });
    uint32_t outerMark = m_instance.modules.evaluatingMark;
    m_instance.modules.evaluatingMark = std::min<uint32_t>(outerMark + 1, UINT8_MAX);
    auto restoresMark = makeScopeExit([&] {
        m_instance.modules.evaluatingMark = outerMark;
    });
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto waiting = m_pendingMockCalls.find(*file);
    std::optional<OriginalRun> note;
    if (waiting != m_pendingMockCalls.end() && waiting->value.wasStoredDuringImport)
        note = OriginalRun::MockedDuringImport;
    else if (m_filesWithRejectedRequire.get(file->index()))
        note = OriginalRun::RequireRejected;
    else if (!isOriginalLoadedAlone(*file))
        note = OriginalRun::RunsAmongLoads;
    else if (host().runsTestFilesAgain && host().runsTestFilesAgain())
        note = OriginalRun::TestFilesRunAgain;
    m_filesWithCompiledOriginal.set(file->index());
    if (host().evictOriginal) {
        forgetCommonJSOriginal(*file);
        m_testFilesOfCommonJSOriginals.set(file->index(), m_hasBegunTestFile ? m_instance.vm->aotState().momentOfTestFile : 0);
    }
    enterBody(*file, *data(*file).bodyFunction());
    if (scope.exception()) {
        note = OriginalRun::Failed;
        forgetCommonJSOriginal(*file);
    }
    if (note)
        noteCompiledOriginal(*file, *note);
    return true;
}

JSValue Modules::takeArgumentOfOriginal(ModuleID module, unsigned index)
{
    if (!m_argumentsOfOriginal || m_commonJSOriginal != module) {
        if (isMocked(module) || isOfPackage(module))
            noteCompiledOriginal(module, OriginalRun::SecondInstance);
        return jsUndefined();
    }
    JSValue argument = m_argumentsOfOriginal->at(index);
    if (index + 1 == m_argumentsOfOriginal->size())
        m_argumentsOfOriginal = nullptr;
    return argument;
}

bool Modules::isOriginalLoadedAlone(ModuleID module)
{
    if ((!m_mocks && !isMockUnseen(module)) || isRequiringAnotherModule(module) || !m_importsAwaitingSource.isEmpty() || !m_importsAwaitingFirstStep.isEmpty() || !m_modulesOfQueuedImports.isEmpty())
        return false;
    BitVector imported = staticImportClosure(module, MockTraversal::Continue);
    BitVector none;
    for (size_t index : m_mocks ? m_mocks->modules : none) {
        ModuleID other { index };
        if (fileOf(other) == fileOf(module))
            continue;
        if (pendingFactoryCallID(other) || (imported.get(index) && isMockPending(other)))
            return false;
        if (!m_modulesWithPendingMock.get(index))
            continue;
        auto call = m_pendingMockCalls.find(other);
        if (activeMock(other) || (call != m_pendingMockCalls.end() && call->value.wasStoredByImportedFile))
            return false;
        ModuleRegistryEntry* entry = m_table.hasFilePath(other) ? loader()->registryEntry(actualKey(fileOf(other))) : nullptr;
        if (entry && entry->record() && entry->record()->isCompiledOriginal())
            return false;
    }
    for (const auto& entry : loader()->moduleMap().values()) {
        auto* record = dynamicDowncast<JSModuleRecord>(entry->record());
        if (record && record->status() != CyclicModuleRecord::Status::Evaluated && !record->moduleKey().isSymbol() && m_table.findByPath(record->moduleKey().string()))
            return false;
    }
    return true;
}

bool Modules::importsMock(ModuleID importer) const
{
    if (!m_mocks)
        return false;
    BitVector imported = staticImportClosure(importer, MockTraversal::Continue);
    for (size_t mocked : m_mocks->modules) {
        if (imported.get(mocked))
            return true;
    }
    return false;
}

bool Modules::isLoadedBelow(ModuleID module, ModuleID root) const
{
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(work.takeLast())) {
            if (request.targetModule() == module)
                return true;
            if (!seen.quickSet(request.target()))
                work.append(request.targetModule());
        }
    }
    return false;
}

bool Modules::hasAnotherStarExport(ModuleID module) const
{
    for (unsigned index = 0; index < m_table.size(); ++index) {
        bool exportsModule = false;
        unsigned starExports = 0;
        for (const ModuleRequestData& request : m_table.requests(ModuleID(index))) {
            starExports += request.isDynamicStarExport;
            exportsModule |= request.isDynamicStarExport && fileOf(request.targetModule()) == fileOf(module);
        }
        if (exportsModule && starExports > 1)
            return true;
    }
    return false;
}

void Modules::tryBindOriginal(JSModuleRecord* root)
{
    if (!m_table.anyModuleHasInstances() || root->hasExportsSource() || !originalNamedBy(root->moduleKey()))
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<std::pair<JSModuleRecord*, ModuleID>, 4> originals;
    bool canBeCompiled = true;
    auto append = [&](AbstractModuleRecord* requested) {
        auto* record = dynamicDowncast<JSModuleRecord>(requested);
        if (record && record->isCompiledOriginal())
            return;
        std::optional<ModuleID> module = record && !record->hasLiveExports() && record->status() == CyclicModuleRecord::Status::Unlinked ? compiledOriginalNamedBy(record->moduleKey()) : std::nullopt;
        if (!module)
            canBeCompiled = false;
        else if (!originals.contains(std::pair { record, *module }))
            originals.append({ record, *module });
    };
    append(root);
    bool exportsNamesOfMocks = false;
    bool loadsItsImporter = false;
    for (size_t index = 0; canBeCompiled && index < originals.size(); ++index) {
        auto [record, module] = originals[index];
        canBeCompiled &= !m_table.importsAbsentExport(module);
        for (const auto& request : record->requestedModules()) {
            AbstractModuleRecord* requested = JSModuleLoader::getImportedModule(record, request);
            if (auto* compiled = dynamicDowncast<ModuleRecord>(requested)) {
                ModuleID file = m_table.moduleBehindFacade(compiled->module()).value_or(compiled->module());
                canBeCompiled &= file != module;
                bool isStarExport = std::ranges::any_of(record->starExportEntries(), [&](const auto& star) { return star.first.get() == request.m_specifier.impl(); });
                exportsNamesOfMocks |= isStarExport && importsMock(file);
                loadsItsImporter |= data(file).kind == ModuleKind::CommonJs && isLoadedBelow(module, file);
            } else if (originalNamedBy(requested->moduleKey()))
                append(requested);
            else if (!requested->moduleKey().isSymbol() && requested->inherits<JSModuleRecord>() && m_table.findByPath(requested->moduleKey().string()))
                canBeCompiled = false;
        }
    }
    if (!canBeCompiled) {
        if (host().didRunOriginal && compiledOriginalNamedBy(root->moduleKey()))
            host().didRunOriginal(m_instance.globalObject, root->moduleKey().string(), OriginalRun::FromSource);
        return;
    }
    ModuleID module = originals[0].second;
    auto waiting = m_pendingMockCalls.find(module);
    std::optional<OriginalRun> note;
    if (waiting != m_pendingMockCalls.end() && waiting->value.wasStoredDuringImport)
        note = OriginalRun::MockedDuringImport;
    else if (exportsNamesOfMocks)
        note = OriginalRun::ExportsNamesOfMocks;
    else if (loadsItsImporter)
        note = OriginalRun::LoadsItsImporter;
    else if (m_filesWithRejectedRequire.get(fileOf(module).index()))
        note = OriginalRun::RequireRejected;
    else if (data(module).hasTopLevelAwait() && m_timesMocked.get(module) > 1)
        note = OriginalRun::AwaitsMockedAgain;
    else if (!isOriginalLoadedAlone(module))
        note = OriginalRun::RunsAmongLoads;
    else if (host().runsTestFilesAgain && host().runsTestFilesAgain())
        note = OriginalRun::TestFilesRunAgain;
    for (auto [record, original] : originals) {
        bindOriginal(record, original);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (note)
        noteCompiledOriginal(module, *note);
}

void Modules::bindOriginal(JSModuleRecord* record, ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Environment* instance = nullptr;
    {
        CurrentInstanceScope current(*this, module, nullptr);
        ProgramStatics::writeVariable(m_instance, *m_table.severalInstancesVariable(module), jsBoolean(true));
        callInstanceAllocator(module);
        RETURN_IF_EXCEPTION(scope, void());
        instance = currentInstance(module);
    }
    if (record->features() & ImportMetaFeature) {
        auto& originals = m_originalsByFile.ensure(fileOf(module).index(), [] { return Vector<OriginalInstance, 1>(); }).iterator->value;
        originals.removeAllMatching([](const OriginalInstance& original) { return !original.instance; });
        originals.append({ Weak<Environment>(instance), record->moduleKey() });
    }
    auto* object = ModuleNamespace::create(vm, namespaceStructure(true), module, ModulePhase::Evaluation, { }, instance);
    object->setKeepsToModule(true);
    m_filesWithCompiledOriginal.set(fileOf(module).index());
    record->setIsCompiledOriginal(vm, object);
}

AbstractModuleRecord* Modules::findRecordRequestedByOriginal(JSModuleRecord* record, ModuleID target) const
{
    ModuleID file = m_table.moduleBehindFacade(target).value_or(target);
    for (const auto& request : record->requestedModules()) {
        AbstractModuleRecord* requested = JSModuleLoader::getImportedModule(record, request);
        std::optional<ModuleID> module;
        if (auto* compiled = dynamicDowncast<ModuleRecord>(requested))
            module = compiled->module();
        else if (auto* original = dynamicDowncast<JSModuleRecord>(requested))
            module = originalNamedBy(original->moduleKey());
        if (module && m_table.moduleBehindFacade(*module).value_or(*module) == file)
            return requested;
    }
    return nullptr;
}

AbstractModuleRecord* Modules::findHostRecordRequestedByOriginal(JSModuleRecord* record, ModuleID target) const
{
    Identifier key = moduleKey(target);
    for (const auto& request : record->requestedModules()) {
        AbstractModuleRecord* requested = JSModuleLoader::getImportedModule(record, request);
        if (!requested->inherits<ModuleRecord>() && requested->moduleKey() == key)
            return requested;
    }
    return nullptr;
}

void Modules::linkOriginal(JSModuleRecord* record)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleNamespace* object = namespaceOfCompiledOriginal(record);
    ModuleID module = object->module();
    Environment* instance = object->boundModuleInstance();
    CurrentInstanceScope current(*this, module, instance);
    for (const ModuleLinkData& link : m_table.moduleLinks(module)) {
        if (!link.hasLink)
            continue;
        ensureInstance(link.targetModule());
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (const ModuleLinkData& link : m_table.moduleLinks(module)) {
        ModuleID target = link.targetModule();
        Environment* linked = currentInstance(target);
        if (link.isResolved) {
            if (link.hasLink)
                writeField(instance, link.link, linked);
            continue;
        }
        bool seesMock = isMockSeenByLoad(target, module);
        if (AbstractModuleRecord* requested = findRecordRequestedByOriginal(record, target)) {
            if (auto* original = dynamicDowncast<JSModuleRecord>(requested)) {
                linked = namespaceOfCompiledOriginal(original)->boundModuleInstance();
                seesMock = false;
            } else if (auto* compiled = uncheckedDowncast<ModuleRecord>(requested); compiled->belongsToReleasedInstance()) {
                linked = uncheckedDowncast<Environment>(compiled->moduleInstance());
                seesMock = false;
            } else {
                ModuleID file = m_table.moduleBehindFacade(target).value_or(target);
                seesMock = !compiled->keepsToModule() && displacedRecord(file) != compiled && ((activeMock(file) && !hasRemovedMock(file)) || isMockPending(file));
            }
        }
        if (link.hasLink)
            writeField(instance, link.link, linked);
        if (!link.hasMock)
            continue;
        ModuleNamespace* word = nullptr;
        if (findHostRecordRequestedByOriginal(record, target)) {
            word = ModuleNamespace::create(*m_instance.vm, namespaceStructure(true), moduleWithExportsOf(target), ModulePhase::Evaluation, { });
            word->setMockWord();
            word->setMockOfView(*m_instance.vm, createTableOfExports());
            writeField(instance, link.mock, word);
            continue;
        }
        word = wordForLoad(target, seesMock);
        writeField(instance, link.mock, word ? JSValue(word) : jsUndefined());
        if (word && word->linkedExports() && !word->namesOfMock()) {
            fixMockNames(word);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    resolveLinks({ &module, 1 });
    RELEASE_AND_RETURN(scope, bindNamesRequiredByOriginal(module));
}

void Modules::didFailOriginal(JSModuleRecord* record)
{
    if (host().didRunOriginal)
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(namespaceOfCompiledOriginal(record)->module()), OriginalRun::Failed);
}

void Modules::noteCompiledOriginal(ModuleID module, OriginalRun run)
{
    if (m_filesWithCompiledOriginal.get(fileOf(module).index()) && host().didRunOriginal)
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(fileOf(module)), run);
}

void Modules::noteOriginalsOfFailedLink(ModuleID importer)
{
    if (!m_mocks || !importer)
        return;
    BitVector imported = staticImportClosure(importer, MockTraversal::Continue);
    for (size_t mocked : m_mocks->modules) {
        if (imported.get(mocked))
            noteCompiledOriginal(ModuleID(mocked), OriginalRun::LoadFailsToLink);
    }
}

void Modules::noteOriginalsWithLoadedImporter(ModuleID imported, ModuleID referrer)
{
    for (size_t index : m_mocks->modules) {
        ModuleID mocked(index);
        if (m_filesWithCompiledOriginal.get(fileOf(mocked).index()) && isMockPending(mocked) && importsStatically(mocked, fileOf(referrer)) && importsStatically(imported, mocked))
            noteCompiledOriginal(mocked, OriginalRun::LoadsItsImporter);
    }
}

void Modules::willReplaceMock(ModuleID mocked)
{
    m_requiresRejectedAgain.removeIf([&](uint64_t pair) {
        if (static_cast<uint32_t>(pair) != mocked.index())
            return false;
        tellOfKeptRun(ModuleID(static_cast<uint32_t>(pair >> 32)));
        return true;
    });
}

void willReplaceMock(JSGlobalObject* realm, ModuleID mocked)
{
    if (Modules* modules = modulesOf(realm))
        modules->willReplaceMock(mocked);
}

void Modules::didRejectRequire(const String& path)
{
    std::optional<ModuleID> module = originalNamedBy(Identifier::fromString(*m_instance.vm, path));
    if (!module)
        module = m_table.findByPath(path);
    if (!module || !isMocked(fileOf(*module)))
        return;
    m_filesWithRejectedRequire.set(fileOf(*module).index());
    ModuleRegistryEntry* entry = loader()->registryEntry(actualKey(fileOf(*module)));
    auto* record = entry ? dynamicDowncast<JSModuleRecord>(entry->record()) : nullptr;
    if (record && record->status() == CyclicModuleRecord::Status::Evaluating)
        noteCompiledOriginal(*module, OriginalRun::LoadsItsImporter);
}

void Modules::willEndMockOfBuiltin(ModuleID builtin, MockEnd end)
{
    if (data(builtin).kind != ModuleKind::Runtime && data(builtin).kind != ModuleKind::RuntimeSource)
        return;
    for (size_t index : m_filesWithCompiledOriginal) {
        ModuleID file(index);
        if (end == MockEnd::Replaced) {
            if (activeMock(builtin) && m_originalsReadingHostMocks.contains(std::pair { file, builtin }))
                noteCompiledOriginal(file, OriginalRun::BuiltinMockChanged);
        } else if (staticImportClosure(file, MockTraversal::Continue).get(builtin.index()))
            m_originalsUnderEndedBuiltinMocks.set(index);
    }
}

void Modules::willImportOriginal(ModuleID file)
{
    if (data(file).kind == ModuleKind::Data) {
        m_importedDataOriginals.set(file.index());
        if (!wasOriginalEvaluated(file) || m_evictedDataOriginals.clear(file.index()))
            m_originalsEvaluatedByRequire.clear(file.index());
    }
    if (m_originalsUnderEndedBuiltinMocks.get(file.index()))
        noteCompiledOriginal(file, OriginalRun::BuiltinMockChanged);
}

void Modules::noteSecondInstanceOfOriginal(ModuleID module)
{
    ModuleRegistryEntry* entry = m_table.hasFilePath(module) ? loader()->registryEntry(actualKey(fileOf(module))) : nullptr;
    AbstractModuleRecord* record = entry ? entry->record() : nullptr;
    if (record && record->isCompiledOriginal())
        noteCompiledOriginal(module, OriginalRun::SecondInstance);
}

void Modules::bindNamesRequiredByOriginal(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    for (const ModuleRequestData& request : m_table.imports(module)) {
        ModuleID target = request.targetModule();
        if (!data(target).capturesExports() || isMockSeenByLoad(target, module))
            continue;
        if (data(target).capturesExportsBehindEvaluation())
            captureExportsBehindEvaluation(target);
        else
            captureExports(target);
        RETURN_IF_EXCEPTION(scope, void());
    }
    RELEASE_AND_RETURN(scope, fillDeferredExports(module));
}

void Modules::finishLinksOfOriginal(JSModuleRecord* record)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ModuleNamespace* object = namespaceOfCompiledOriginal(record);
    ModuleID module = object->module();
    CurrentInstanceScope current(*this, module, object->boundModuleInstance());
    bindNamesRequiredByOriginal(module);
    RETURN_IF_EXCEPTION(scope, void());
    for (const ModuleRequestData& request : m_table.imports(module)) {
        std::pair pair { fileOf(module), request.targetModule() };
        if (findHostRecordRequestedByOriginal(record, pair.second) && !m_originalsReadingHostMocks.contains(pair))
            m_originalsReadingHostMocks.append(pair);
    }
    for (const ModuleLinkData& link : m_table.moduleLinks(module)) {
        AbstractModuleRecord* requested = link.hasMock && !link.isResolved ? findHostRecordRequestedByOriginal(record, link.targetModule()) : nullptr;
        if (!requested)
            continue;
        JSObject* exports = requested->getModuleNamespace(m_instance.globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        auto* word = uncheckedDowncast<ModuleNamespace>(readField(object->boundModuleInstance(), link.mock));
        word->setMockOfView(vm, exports);
        fixMockNames(word);
        RETURN_IF_EXCEPTION(scope, void());
    }
    resolveLinks({ &module, 1 });
}

JSValue Modules::evaluateOriginal(JSModuleRecord* record)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ModuleNamespace* object = namespaceOfCompiledOriginal(record);
    ModuleID module = object->module();
    finishLinksOfOriginal(record);
    RETURN_IF_EXCEPTION(scope, { });
    std::optional<FunctionID> body = data(module).bodyFunction();
    if (!body)
        return jsUndefined();
    SetForScope isInHostRequest(m_currentHostRequest, false);
    SetForScope isStatementHostRequest(m_isStatementHostRequest, false);
    didStartThread(&vm);
    CurrentInstanceScope current(*this, module, object->boundModuleInstance());
    m_linkedInstances.set(module.index());
    didChangeLinkState();
    uint8_t storedStatus = std::exchange(m_storedStatuses[module.index()], static_cast<uint8_t>(StoredModuleStatus::Linked));
    auto restoresStatus = makeScopeExit([&] {
        m_storedStatuses[module.index()] = storedStatus;
    });
    uint32_t outerMark = m_instance.modules.evaluatingMark;
    m_instance.modules.evaluatingMark = std::min<uint32_t>(outerMark + 1, UINT8_MAX);
    auto restoresMark = makeScopeExit([&] {
        m_instance.modules.evaluatingMark = outerMark;
    });
    RELEASE_AND_RETURN(scope, enterBody(module, *body));
}

static bool isEvaluatedCompiledOriginal(JSModuleRecord* record)
{
    return record && record->isCompiledOriginal() && record->status() == CyclicModuleRecord::Status::Evaluated && !record->evaluationError();
}

void Modules::restoreCompiledOriginal(JSModuleRecord* record)
{
    ModuleNamespace* object = namespaceOfCompiledOriginal(record);
    ModuleID module = object->module();
    startLoadingModulesIndividually();
    restoreInstance(module, object->boundModuleInstance());
    setStatus(module, ModuleStatus::Evaluated);
    m_modulesLoadedBesideGroups.clear(module.index());
    m_modulesSettledForLinks[module.index() / 64] &= ~(1ull << (module.index() % 64));
    followImportsAnewAtNextQuestion();
    ++m_linkEpoch;
}

bool Modules::tryRestoreCompiledOriginal(JSModuleRecord* record)
{
    if (!isEvaluatedCompiledOriginal(record))
        return false;
    ModuleID module = namespaceOfCompiledOriginal(record)->module();
    if (isMocked(module) || activeMock(module) || isMockPending(module) || hasIntactOriginal(module) || isEvicted(module) || status(module) >= ModuleStatus::Evaluating)
        return false;
    restoreCompiledOriginal(record);
    return true;
}

bool Modules::tryRestoreCompiledOriginalUnderMock(ModuleID module)
{
    if (!m_filesWithCompiledOriginal.get(fileOf(module).index()))
        return false;
    Identifier key = actualKey(fileOf(module));
    ModuleRegistryEntry* entry = loader()->registryEntry(key);
    auto* record = entry ? dynamicDowncast<JSModuleRecord>(entry->record()) : nullptr;
    if (!isEvaluatedCompiledOriginal(record))
        return false;
    restoreCompiledOriginal(record);
    loader()->removeEntry(key);
    m_originalsEvaluatedUnderMock.set(module.index());
    return true;
}

void Modules::restoreOriginalForRequire(ModuleID module)
{
    if (isMocked(module) && !activeMock(module) && !hasIntactOriginal(module) && !hasStartedLoading(module))
        tryRestoreCompiledOriginalUnderMock(module);
}

void Modules::noteOriginalsRequestedBySource(JSModuleRecord* importer)
{
    if (!m_table.anyModuleHasInstances())
        return;
    for (const auto& request : importer->requestedModules()) {
        AbstractModuleRecord* requested = JSModuleLoader::getImportedModule(importer, request);
        if (!requested || !requested->isCompiledOriginal())
            continue;
        if (std::optional<ModuleID> original = originalNamedBy(requested->moduleKey()))
            noteCompiledOriginal(*original, OriginalRun::SecondInstance);
    }
}

void noteOriginalsRequestedBySource(JSGlobalObject* realm, JSModuleRecord* importer)
{
    if (Modules* modules = modulesOf(realm))
        modules->noteOriginalsRequestedBySource(importer);
}

void tryBindOriginal(JSGlobalObject* realm, JSModuleRecord* record)
{
    if (Modules* modules = modulesOf(realm))
        modules->tryBindOriginal(record);
}

void linkOriginal(JSGlobalObject* realm, JSModuleRecord* record)
{
    modulesOf(realm)->linkOriginal(record);
}

JSValue evaluateOriginal(JSGlobalObject* realm, JSModuleRecord* record)
{
    return modulesOf(realm)->evaluateOriginal(record);
}

void didFailOriginal(JSGlobalObject* realm, JSModuleRecord* record)
{
    modulesOf(realm)->didFailOriginal(record);
}

void evaluateOriginalAsync(JSGlobalObject* realm, JSModuleRecord* record)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = modulesOf(realm)->evaluateOriginal(record);
    RETURN_IF_EXCEPTION(scope, void());
    auto* promise = dynamicDowncast<JSPromise>(result);
    if (!promise) {
        promise = JSPromise::resolvedPromise(realm, result);
        RETURN_IF_EXCEPTION(scope, void());
    }
    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::AsyncModuleExecutionDone, nullptr, record);
}

void willEndMockOfBuiltin(JSGlobalObject* realm, ModuleID builtin, MockEnd end)
{
    modulesOf(realm)->willEndMockOfBuiltin(builtin, end);
}

void willImportOriginal(JSGlobalObject* realm, ModuleID file)
{
    modulesOf(realm)->willImportOriginal(file);
}

void didRejectRequire(JSGlobalObject* realm, const String& path)
{
    if (Modules* modules = modulesOf(realm))
        modules->didRejectRequire(path);
}

bool tryRestoreCompiledOriginal(JSGlobalObject* realm, AbstractModuleRecord* original)
{
    auto* record = dynamicDowncast<JSModuleRecord>(original);
    Modules* modules = modulesOf(realm);
    return record && modules && modules->tryRestoreCompiledOriginal(record);
}

bool isCompiledOriginal(JSGlobalObject* realm, const String& key)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return false;
    Identifier name = Identifier::fromString(realm->vm(), key);
    return modules->compiledOriginalNamedBy(name) || modules->commonJSOriginalNamedBy(name);
}

bool hasLoadedCommonJSFile(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasLoadedCommonJSFile();
}

bool tryEvaluateCommonJSOriginal(JSGlobalObject* realm, const String& key, const ArgList& arguments)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->tryEvaluateCommonJSOriginal(Identifier::fromString(realm->vm(), key), arguments);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTOriginalArgument, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t index))
{
    return JSValue::encode(modulesOf(*instance).takeArgumentOfOriginal(ModuleID(module), index));
}

void noteOriginalRequiredFromSource(JSGlobalObject* realm, ModuleID file)
{
    if (Modules* modules = modulesOf(realm))
        modules->noteOriginalRequiredFromSource(file);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

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
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT {

bool Modules::importsOriginalBesideMock(ModuleID fileOrModule, ModuleID importer) const
{
    ModuleID mocked = fileOf(fileOrModule);
    if (!isMocked(mocked) || !host().givesOriginalBesideMock || !importer || !m_table.hasFilePath(mocked) || !m_table.hasFilePath(importer))
        return false;
    bool isFile = data(mocked).kind == ModuleKind::CommonJs && !data(mocked).isOfLibrary();
    if (isFile ? moduleWithExportsOf(mocked) == mocked || fileOf(importer) == mocked : !data(mocked).isESModuleOfProgram())
        return false;
    auto asked = m_importersAskedAboutOriginal.add(static_cast<uint64_t>(mocked.index()) << 32 | importer.index(), false);
    if (asked.isNewEntry)
        asked.iterator->value = host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(mocked), m_table.pathText(importer), true);
    if (asked.iterator->value)
        m_importersThatGotOriginal.add(asked.iterator->key);
    return asked.iterator->value;
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
        SetForScope makesIt(m_makesNamespaceOfModuleItself, true);
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

JSC_DEFINE_HOST_FUNCTION(answerImportWithModuleItselfInTestRunOriginals, (JSGlobalObject* realm, CallFrame* callFrame))
{
    return JSValue::encode(modulesOf(realm)->namespaceOfModuleItself(ModuleID(callFrame->argument(1).asUInt32())));
}

bool Modules::isAskedForByOwnFactory(ModuleID module)
{
    if (!isMocked(module) || data(module).isOfLibrary() || !data(module).hasExportTable() || data(module).isFaceOfFile()) [[likely]]
        return false;
    if (m_modulesWhoseFactoryRuns.get(module.index()))
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

void Modules::oweBodyBesideMock(ModuleID module)
{
    m_modulesOwedBesideMock.set(module.index());
    if (status(module) != ModuleStatus::Evaluated)
        return;
    setStatus(module, ModuleStatus::Unlinked);
    if (ModuleRecord* record = recordIfExists(module))
        record->clearTopLevelCapability();
}

JSValue Modules::requireModuleItselfBesideMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!hasRunItself(module)) {
        oweBodyBesideMock(module);
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        if (scope.exception()) [[unlikely]] {
            setAsideErrorOfModuleThatRanBesideMock(module);
            return { };
        }
    }
    if (JSValue error = ensureLoaded(module).errorOfModuleThatRanBesideMock.get()) [[unlikely]] {
        throwException(m_instance.globalObject, scope, error);
        return { };
    }
    RELEASE_AND_RETURN(scope, namespaceOfModuleItself(module));
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
    if (!hasBegun || status(module) == ModuleStatus::EvaluatingAsync) {
        if (!hasBegun)
            oweBodyBesideMock(module);
        JSPromise* evaluation = evaluate(module, std::nullopt, nullptr, nullptr);
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

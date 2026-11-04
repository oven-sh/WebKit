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

// The host's own loader gives a package the module itself where a test mocks it as Vitest does.
bool Modules::importsOriginalBesideMock(ModuleID mocked, ModuleID importer) const
{
    if (!isMocked(mocked) || !host().givesOriginalBesideMock || !importer || !m_table.hasFilePath(mocked) || !m_table.hasFilePath(importer))
        return false;
    // TODO(bun): a CommonJS file has one variable for its exports, and what an import reads of it is taken by the mock.
    if (!data(mocked).isESModuleOfProgram() || data(mocked).isFaceOfFile())
        return false;
    auto asked = m_importersAskedAboutOriginal.add(static_cast<uint64_t>(mocked.index()) << 32 | importer.index(), false);
    if (asked.isNewEntry)
        asked.iterator->value = host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(mocked), m_table.pathText(importer), true);
    // It was never linked to the mock, and isn't once the mock has ended and the host has forgotten it.
    if (asked.iterator->value)
        m_importersThatGotOriginal.add(asked.iterator->key);
    return asked.iterator->value;
}

bool Modules::hasRunItself(ModuleID mocked) const
{
    return m_modulesThatRanBesideMock.get(mocked.index()) || m_modulesThatHadRunWhenMocked.get(mocked.index());
}

bool Modules::isMockedByFactoryThatTakesOriginal(ModuleID module) const
{
    return m_modulesWithMockThatAsksAgain.get(module.index()) && isMocked(module);
}

// Of a CommonJS file of the program whose body a mock has kept from running.
bool Modules::getsOriginalBesideMock(ModuleID target, ModuleID referrer)
{
    if (!m_numberOfMocksInPlace || !host().givesOriginalBesideMock) [[likely]]
        return false;
    if (!referrer || data(target).kind != ModuleKind::CommonJs || !mockInPlace(target) || !m_table.hasFilePath(referrer))
        return false;
    return host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(target), m_table.pathText(referrer), false);
}

// The answer of the mock stands where the exports of the file are read from, and the file counts as evaluated. The body runs once, with both set aside.
// Whoever calls it has seen the mock: getsOriginalBesideMock().
JSValue Modules::answerRequireWithOriginal(ModuleID file, ModuleID referrer)
{
    ASSERT_UNUSED(referrer, referrer && mockInPlace(file));
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (JSValue original = ensureLoaded(file).exportsOfOriginalBesideMock.get())
        return original;
    // A ring of files comes back while the body runs.
    if (status(file) == ModuleStatus::Evaluating)
        return commonJSExports(file);
    JSObject* mockBefore = mockInPlace(file);
    // The variable and the status are the body's while it runs. What the mock answers waits here: for whoever gets no original, and for a mock that is laid meanwhile.
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
    // As `bun run` does with a CommonJS file whose body throws: whoever asked hears of it, and the next one runs the body again. didThrowInBody() has left the file to this.
    if (hasThrown)
        forgetRunThatThrew(file);
    // The mock ended while the body ran: the file is what the run has made of it.
    if (!mock)
        return hasThrown ? JSValue() : original;
    markEvaluated(file);
    setCommonJSExports(file, answerOfMock);
    if (mock == mockBefore)
        ensureLoaded(file).requiredValue.set(*this, settledAnswer);
    if (hasThrown)
        return { };
    ensureLoaded(file).exportsOfOriginalBesideMock.set(*this, original);
    // The run may have taken the names of the file for whoever imports them.
    takeNamesOfMockInPlace(moduleWithExportsOf(file), mock);
    RETURN_IF_EXCEPTION(scope, { });
    return original;
}

// The body ran beside a mock that stands in place of the module. The object in the slot of the module is the mock's: whoever gets the module itself holds another, with the names and the values of the module.
JSObject* Modules::namespaceOfModuleThatRanBesideMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    if (JSObject* own = loaded.namespaceBeforeMockInPlace.get())
        return own;
    // Nobody had asked for it when the mock came.
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

// An import() of a package has taken the steps of any import(), and was answered with what the mock shows.
JSValue Modules::importModuleItselfBesideMock(ModuleID module, JSValue answerOfImport)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The mock has ended meanwhile.
    if (!mockInPlace(module))
        return answerOfImport;
    bool hasBegun = hasRunItself(module);
    if (!hasBegun || status(module) == ModuleStatus::EvaluatingAsync) {
        if (!hasBegun) {
            m_modulesOwedBesideMock.set(module.index());
            // An importer that sees the mock came by first.
            if (status(module) == ModuleStatus::Evaluated)
                setStatus(module, ModuleStatus::Unlinked);
        }
        JSPromise* evaluation = evaluate(module, std::nullopt, nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
        if (evaluation->status() != JSPromise::Status::Fulfilled) {
            JSPromise* answered = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* answer = JSFunction::create(vm, realm, 2, String(), answerImportWithModuleItselfInTestRunOriginals, ImplementationVisibility::Private);
            evaluation->performPromiseThenWithContext(vm, realm, answer, jsUndefined(), answered, jsNumber(module.index()));
            return answered;
        }
    }
    RELEASE_AND_RETURN(scope, namespaceOfModuleItself(module));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

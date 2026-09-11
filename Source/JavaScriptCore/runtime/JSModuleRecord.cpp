/*
 * Copyright (C) 2015-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JSModuleRecord.h"

#include "BuiltinNames.h"
#include "Interpreter.h"
#include "JSAsyncFunction.h"
#include "JSAsyncGeneratorFunction.h"
#include "JSCInlines.h"
#include "JSGeneratorFunction.h"
#include "JSMicrotask.h"
#include "JSModuleEnvironment.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSPromise.h"
#include "ModuleProgramExecutable.h"
#include "SourceProfiler.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "WeakGCMapInlines.h"
#include <wtf/text/MakeString.h>

namespace JSC {

const ClassInfo JSModuleRecord::s_info = { "ModuleRecord"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSModuleRecord) };

JSModuleRecord* JSModuleRecord::create(JSGlobalObject* globalObject, VM& vm, Structure* structure, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const SourceCode& sourceCode, CodeFeatures features)
{
    JSModuleRecord* instance = new (NotNull, allocateCell<JSModuleRecord>(vm)) JSModuleRecord(vm, structure, moduleLoader, moduleKey, sourceCode, features);
    instance->finishCreation(globalObject, vm);
    return instance;
}

#if USE(BUN_JSC_ADDITIONS)
JSModuleRecord* JSModuleRecord::createPrelinked(JSGlobalObject* globalObject, VM& vm, Structure* structure, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const SourceCode& sourceCode, Ref<PrelinkedModuleGraph>&& graph, uint32_t moduleIndex)
{
    const PrelinkedModuleGraph::Module& module = graph->module(moduleIndex);
    CodeFeatures features = (module.flags & PrelinkedModuleGraph::Module::HasImportMeta) ? ImportMetaFeature : NoFeatures;
    JSModuleRecord* instance = new (NotNull, allocateCell<JSModuleRecord>(vm)) JSModuleRecord(vm, structure, moduleLoader, moduleKey, sourceCode, features);
    instance->finishCreation(globalObject, vm);
    instance->initializePrelinked(vm, WTF::move(graph), moduleIndex);
    if (!Options::usePrelinkedModuleInfo()) [[unlikely]]
        instance->convertPrelinkedToEager();
    return instance;
}
#endif

JSModuleRecord::JSModuleRecord(VM& vm, Structure* structure, JSModuleLoader* moduleLoader, const Identifier& moduleKey, const SourceCode& sourceCode, CodeFeatures features)
    : Base(vm, structure, moduleLoader, moduleKey, SourceProviderSourceType::Module)
    , m_sourceCode(sourceCode)
    , m_features(features)
{
}

void JSModuleRecord::destroy(JSCell* cell)
{
    JSModuleRecord* thisObject = static_cast<JSModuleRecord*>(cell);
    thisObject->JSModuleRecord::~JSModuleRecord();
}

void JSModuleRecord::finishCreation(JSGlobalObject* globalObject, VM& vm)
{
    Base::finishCreation(globalObject, vm);
    ASSERT(inherits(info()));
}

#if USE(BUN_JSC_ADDITIONS)
size_t JSModuleRecord::estimatedSize(JSCell* cell, VM& vm)
{
    const auto& thisObject = uncheckedDowncast<JSModuleRecord>(cell);
    size_t size = Base::estimatedSize(cell, vm);
    const SourceCode& sourceCode = thisObject->sourceCode();
    StringView view = sourceCode.provider() ? sourceCode.provider()->source() : StringView();
    size += view.length() * (view.is8Bit() ? sizeof(Latin1Character) : sizeof(UChar));
    size += sourceCode.memoryCost();
    return size;
}
#endif

template<typename Visitor>
void JSModuleRecord::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSModuleRecord* thisObject = uncheckedDowncast<JSModuleRecord>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_moduleProgramExecutable);

#if USE(BUN_JSC_ADDITIONS)
    visitor.reportExtraMemoryVisited(thisObject->sourceCode().memoryCost());
#endif
}

DEFINE_VISIT_CHILDREN(JSModuleRecord);

bool JSModuleRecord::isTopLevelExecutionFinished() const
{
    JSValue state = internalField(Field::State).get();
    return !state.isNumber() || state.asInt32AsAnyInt() == std::to_underlying(State::Executing);
}

JSValue JSModuleRecord::evaluate(JSGlobalObject* globalObject, JSValue sentValue, JSValue resumeMode)
{
    if (!m_moduleProgramExecutable) {
        ASSERT_NOT_REACHED_WITH_MESSAGE("Can't evaluate a JSModuleRecord that has no executable");
        return jsUndefined();
    }

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (JSValue error = evaluationError()) {
        scope.throwException(globalObject, error);
        return { };
    }

    ModuleProgramExecutable* executable = m_moduleProgramExecutable.get();
    JSValue resultOrAwaitedValue = vm.interpreter.executeModuleProgram(this, executable, globalObject, moduleEnvironment(), sentValue, resumeMode);
    RETURN_IF_EXCEPTION(scope, { });

    if (isTopLevelExecutionFinished())
        m_moduleProgramExecutable.clear();

    RELEASE_AND_RETURN(scope, resultOrAwaitedValue);
}

void JSModuleRecord::execute(JSGlobalObject* globalObject, JSPromise* capability)
{
    // ExecuteModule([capability])
    // https://tc39.es/ecma262/#sec-source-text-module-record-execute-module

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // 1. Let moduleContext be a new ECMAScript code execution context.
    // 2. Set the Function of moduleContext to null.
    // 3. Set the Realm of moduleContext to module.[[Realm]].
    // 4. Set the ScriptOrModule of moduleContext to module.
    // 5. Assert: module has been linked and declarations in its module environment have been instantiated.
    ASSERT(static_cast<int>(status()) >= static_cast<int>(Status::Linked));
    // 6. Set the VariableEnvironment of moduleContext to module.[[Environment]].
    // 7. Set the LexicalEnvironment of moduleContext to module.[[Environment]].
    // 8. Suspend the running execution context.
    // 9. If module.[[HasTLA]] is false, then
    if (!hasTLA()) {
        // 9.a. Assert: capability is not present.
        ASSERT(capability == nullptr);
        // 9.b. Push moduleContext onto the execution context stack; moduleContext is now the running execution context.
        // 9.c. Let result be Completion(Evaluation of module.[[ECMAScriptCode]]).
        moduleLoader()->evaluate(globalObject, identifierToJSValue(vm, moduleKey()), this, nullptr, jsUndefined(), jsNumber(static_cast<int32_t>(ResumeMode::NormalMode)));
        // 9.d. Suspend moduleContext and remove it from the execution context stack.
        // 9.e. Resume the context that is now on the top of the execution context stack as the running execution context.
        // 9.f. If result is an abrupt completion, then
        // 9.f.i. Return ? result.
        RETURN_IF_EXCEPTION(scope, void());
    // 10. Else,
    } else {
        // 10.a. Assert: capability is a PromiseCapability Record.
        ASSERT(capability != nullptr);
        // 10.b. Perform AsyncBlockStart(capability, module.[[ECMAScriptCode]], moduleContext).
        asyncCapability(vm, capability);
        JSValue result = moduleLoader()->evaluate(globalObject, identifierToJSValue(vm, moduleKey()), this, nullptr, jsUndefined(), jsNumber(static_cast<int32_t>(ResumeMode::NormalMode)));
        asyncModuleResolveEvaluation(globalObject, vm, scope, this, result);
    }
    // 11. Return unused.
}

unsigned JSModuleRecord::importSlotCount() const
{
#if USE(BUN_JSC_ADDITIONS)
    if (importEntriesArePrelinked())
        return prelinkedGraph()->imports(prelinkedModule()).size();
#endif
    return importEntries().size();
}

unsigned JSModuleRecord::importSlotIndex(UniquedStringImpl* localName)
{
#if USE(BUN_JSC_ADDITIONS)
    if (importEntriesArePrelinked()) {
        const auto& module = prelinkedModule();
        const PrelinkedModuleGraph::Import* import = prelinkedGraph()->findImport(module, localName);
        RELEASE_ASSERT(import);
        return import - prelinkedGraph()->imports(module).data();
    }
#endif
    auto iterator = importEntries().find(localName);
    RELEASE_ASSERT(iterator != importEntries().end());
    return iterator->value.slotIndex;
}

template<typename Functor>
static void forEachSingleImport(JSModuleRecord& record, const Functor& functor)
{
    // functor(slotIndex, localName) for every non-namespace import entry.
#if USE(BUN_JSC_ADDITIONS)
    if (record.importEntriesArePrelinked()) {
        PrelinkedModuleGraph* graph = record.prelinkedGraph();
        auto imports = graph->imports(record.prelinkedModule());
        for (unsigned i = 0; i < imports.size(); ++i) {
            if (!imports[i].isNamespace())
                functor(i, graph->identifier(imports[i].localSid));
        }
        return;
    }
#endif
    for (const auto& entry : record.importEntries().values()) {
        if (entry.type != AbstractModuleRecord::ImportEntryType::Namespace)
            functor(entry.slotIndex, entry.localName);
    }
}

std::optional<Vector<unsigned>> JSModuleRecord::importSlotLayout(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Vector<unsigned> layout(importSlotCount(), [](size_t) { return UINT_MAX; });
    bool known = true;
    forEachSingleImport(*this, [&](unsigned slotIndex, const Identifier& localName) {
        if (!known || scope.exception())
            return;
        Resolution resolution = resolveImport(globalObject, localName);
        if (scope.exception() || resolution.type != Resolution::Type::Resolved)
            return;
        SymbolTable* symbolTable = nullptr;
        if (JSModuleEnvironment* environment = resolution.moduleRecord->moduleEnvironmentMayBeNull())
            symbolTable = environment->symbolTable();
        else if (auto* sourceTextModule = dynamicDowncast<JSModuleRecord>(resolution.moduleRecord)) {
            ModuleProgramExecutable* executable = sourceTextModule->getOrMakeExecutable(globalObject);
            if (scope.exception())
                return;
            symbolTable = executable->moduleEnvironmentSymbolTable();
        }
        if (!symbolTable) {
            known = false;
            return;
        }
        ConcurrentJSLocker locker(symbolTable->m_lock);
        auto iterator = symbolTable->find(locker, resolution.localName.impl());
        if (iterator != symbolTable->end(locker))
            layout[slotIndex] = iterator->value.scopeOffset().offset();
    });
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (!known)
        return std::nullopt;
    return layout;
}

void JSModuleRecord::fillImportSlots(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSModuleEnvironment* environment = moduleEnvironment();
    forEachSingleImport(*this, [&](unsigned slotIndex, const Identifier& localName) {
        if (scope.exception())
            return;
        Resolution resolution = resolveImport(globalObject, localName);
        if (scope.exception() || resolution.type != Resolution::Type::Resolved)
            return;
        environment->importSlot(slotIndex).set(vm, environment, resolution.moduleRecord->moduleEnvironment());
    });
}

ModuleProgramExecutable* JSModuleRecord::getOrMakeExecutable(JSGlobalObject* globalObject)
{
    ModuleProgramExecutable* executable = m_moduleProgramExecutable.get();
    if (executable)
        return executable;

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    executable = ModuleProgramExecutable::tryCreate(globalObject, sourceCode());
    RETURN_IF_EXCEPTION(scope, nullptr);
    m_moduleProgramExecutable.set(vm, this, executable);

    // Share the executable (CodeBlocks, JIT code, function declaration executables)
    // of an earlier record for the same URL and source when this record's imports
    // resolve to environments laid out the same way, so linked ModuleVar accesses hold.
    const String& url = sourceCode().provider()->sourceURL();
    if (url.isEmpty())
        return executable;
    std::optional<Vector<unsigned>> layout = importSlotLayout(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!layout)
        return executable;
    auto& executables = globalObject->moduleProgramExecutables();
    if (ModuleProgramExecutable* shared = executables.get(url); shared && shared->unlinkedCodeBlock() == executable->unlinkedCodeBlock() && shared->importSlotLayout() == *layout) {
        m_moduleProgramExecutable.set(vm, this, shared);
        return shared;
    }
    executable->setImportSlotLayout(WTF::move(*layout));
    executables.set(url, Weak<ModuleProgramExecutable>(executable));
    return executable;
}

} // namespace JSC

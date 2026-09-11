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
#include "JSLexicalEnvironment.h"
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

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(JSModuleRecord::UninstantiatedFunctionDeclarations);

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
    {
        Locker locker { thisObject->cellLock() };
        if (auto* uninstantiated = thisObject->m_uninstantiatedFunctionDeclarations.get()) {
            visitor.append(uninstantiated->executable);
            visitor.append(uninstantiated->unlinkedCodeBlock);
        }
    }

#if USE(BUN_JSC_ADDITIONS)
    visitor.reportExtraMemoryVisited(thisObject->sourceCode().memoryCost());
#endif
}

DEFINE_VISIT_CHILDREN(JSModuleRecord);

void JSModuleRecord::setFunctionDeclarationSlots(VM& vm, ModuleProgramExecutable* executable, UnlinkedModuleProgramCodeBlock* unlinkedCodeBlock, bool leftUninstantiated)
{
    RefPtr slots = unlinkedCodeBlock->heapAllocatedFunctionDeclSlots();
    std::unique_ptr<UninstantiatedFunctionDeclarations> uninstantiated;
    if (leftUninstantiated && slots && slots->size()) {
        RELEASE_ASSERT(slots->size() == unlinkedCodeBlock->numberOfHeapAllocatedFunctionDecls());
        uninstantiated = makeUnique<UninstantiatedFunctionDeclarations>();
        uninstantiated->executable.set(vm, this, executable);
        uninstantiated->unlinkedCodeBlock.set(vm, this, unlinkedCodeBlock);
        uninstantiated->remaining = slots->size();
    }
    Locker locker { cellLock() };
    m_functionDeclarationSlots = WTF::move(slots);
    m_uninstantiatedFunctionDeclarations = WTF::move(uninstantiated);
}

bool JSModuleRecord::isFunctionDeclarationSlot(ScopeOffset offset) const
{
    return Options::useLazyModuleFunctionDeclarations() && m_functionDeclarationSlots && m_functionDeclarationSlots->find(offset);
}

JSValue JSModuleRecord::readFunctionDeclarationSlot(VM& vm, JSModuleEnvironment* environment, ScopeOffset offset)
{
    ASSERT(environment->moduleRecord() == this);
    ASSERT(environment->isValidScopeOffset(offset));
    JSValue value = environment->variableAt(offset).get();
    if (value) [[likely]]
        return value;
    auto* uninstantiated = m_uninstantiatedFunctionDeclarations.get();
    if (!uninstantiated)
        return { };
    std::optional<unsigned> index = m_functionDeclarationSlots->find(offset);
    if (!index)
        return { };
    // Records that share the executable share the declarations' executables (and so their code): the first one to read a
    // declaration links it, from this record's reference to the module's unlinked code.
    ModuleProgramExecutable* executable = uninstantiated->executable.get();
    // The declaration's code is every record's, and what it is specialized on is the executable's symbol table.
    RELEASE_ASSERT(environment->symbolTable() == executable->moduleEnvironmentSymbolTable());
    FunctionExecutable* functionExecutable = executable->linkedFunctionDeclaration(*index);
    if (!functionExecutable)
        functionExecutable = executable->linkFunctionDeclaration(vm, *index, uninstantiated->unlinkedCodeBlock->functionDecl(*index));
    UnlinkedFunctionExecutable* unlinkedExecutable = functionExecutable->unlinkedExecutable();

    // InitializeEnvironment step 24.a.iii, for this one declaration.
    JSGlobalObject* globalObject = environment->globalObject();
    JSFunction* function = nullptr;
    SourceParseMode parseMode = functionExecutable->parseMode();
    if (isAsyncGeneratorWrapperParseMode(parseMode))
        function = JSAsyncGeneratorFunction::create(vm, globalObject, functionExecutable, environment);
    else if (isGeneratorWrapperParseMode(parseMode))
        function = JSGeneratorFunction::create(vm, globalObject, functionExecutable, environment);
    else if (isAsyncFunctionWrapperParseMode(parseMode))
        function = JSAsyncFunction::create(vm, globalObject, functionExecutable, environment);
    else
        function = JSFunction::create(vm, globalObject, functionExecutable, environment);

    InlineWatchpointSet* watchpointSet = nullptr;
    {
        SymbolTable* symbolTable = environment->symbolTable();
        ConcurrentJSLocker locker(symbolTable->m_lock);
        auto iter = symbolTable->find(locker, unlinkedExecutable->name().impl());
        if (iter != symbolTable->end(locker)) {
            ASSERT(iter->value.scopeOffset() == offset);
            watchpointSet = iter->value.watchpointSet();
        }
    }
    symbolTablePutTouchWatchpointSet(vm, environment, unlinkedExecutable->name(), function, &environment->variableAt(offset), watchpointSet);

    // An empty slot is one that was never stored to, so each declaration gets here at most once.
    ASSERT(uninstantiated->remaining);
    if (!--uninstantiated->remaining) {
        std::unique_ptr<UninstantiatedFunctionDeclarations> done;
        Locker locker { cellLock() };
        done = WTF::move(m_uninstantiatedFunctionDeclarations);
    }
    return function;
}

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

    // Every module this one imports from has its environment now. Filling the import
    // slots here rather than on first use (JSModuleEnvironment::fillImportSlot) keeps
    // optimized code shared with other records from meeting an empty slot per record.
    JSModuleEnvironment* environment = moduleEnvironment();
    for (unsigned i = 0, count = importSlotCount(); i < count; ++i) {
        if (environment->importSlot(i))
            continue;
        Resolution resolution = resolveImport(globalObject, importSlotNames()[i]);
        RETURN_IF_EXCEPTION(scope, { });
        if (resolution.type == Resolution::Type::Resolved)
            environment->importSlot(i).set(vm, environment, resolution.moduleRecord->moduleEnvironment());
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

const Vector<Identifier>& JSModuleRecord::importSlotNames()
{
    if (!m_importSlotNames) {
        Vector<Identifier> names;
#if USE(BUN_JSC_ADDITIONS)
        if (importEntriesArePrelinked()) {
            for (const auto& import : prelinkedGraph()->imports(prelinkedModule())) {
                if (!import.isNamespace())
                    names.append(prelinkedGraph()->identifier(import.localSid));
            }
        } else
#endif
        {
            for (const auto& entry : importEntries().values()) {
                if (entry.type != ImportEntryType::Namespace)
                    names.append(entry.localName);
            }
        }
        std::sort(names.begin(), names.end(), [](const Identifier& a, const Identifier& b) { return codePointCompare(a.string(), b.string()) < 0; });
        m_importSlotNames = WTF::move(names);
    }
    return *m_importSlotNames;
}

unsigned JSModuleRecord::importSlotIndex(UniquedStringImpl* localName)
{
    const Vector<Identifier>& names = importSlotNames();
    auto iterator = std::lower_bound(names.begin(), names.end(), localName, [](const Identifier& name, UniquedStringImpl* localName) { return codePointCompare(StringView(name.string()), StringView(*localName)) < 0; });
    RELEASE_ASSERT(iterator != names.end() && iterator->impl() == localName);
    return iterator - names.begin();
}

JSModuleEnvironment* JSModuleRecord::fillImportSlot(JSGlobalObject* globalObject, unsigned index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Resolution resolution = resolveImport(globalObject, importSlotNames()[index]);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_ASSERT(resolution.type == Resolution::Type::Resolved);
    JSModuleEnvironment* environment = resolution.moduleRecord->moduleEnvironment();
    moduleEnvironment()->importSlot(index).set(vm, moduleEnvironment(), environment);
    return environment;
}

std::optional<ModuleProgramExecutable::ImportedBindings> JSModuleRecord::importedBindings(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ModuleProgramExecutable::ImportedBindings bindings;
    for (const Identifier& localName : importSlotNames()) {
        Resolution resolution = resolveImport(globalObject, localName);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (resolution.type != Resolution::Type::Resolved)
            continue;
        if (auto* sourceTextModule = dynamicDowncast<JSModuleRecord>(resolution.moduleRecord)) {
            bindings.append({ localName.impl(), sourceTextModule->sourceCode().provider(), resolution.localName.impl(), 0 });
            continue;
        }
        JSModuleEnvironment* environment = resolution.moduleRecord->moduleEnvironmentMayBeNull();
        if (!environment)
            return std::nullopt;
        SymbolTable* symbolTable = environment->symbolTable();
        ConcurrentJSLocker locker(symbolTable->m_lock);
        auto iterator = symbolTable->find(locker, resolution.localName.impl());
        RELEASE_ASSERT(iterator != symbolTable->end(locker));
        bindings.append({ localName.impl(), nullptr, resolution.localName.impl(), iterator->value.scopeOffset().offset() });
    }
    return bindings;
}

ModuleProgramExecutable* JSModuleRecord::getOrMakeExecutable(JSGlobalObject* globalObject)
{
    ModuleProgramExecutable* executable = m_moduleProgramExecutable.get();
    if (executable)
        return executable;

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Linked module code embeds, for each imported binding, its ScopeOffset in the
    // exporting module's environment, which that module's source text determines, and
    // for variables of the loader's module scope, their offsets in its lexical
    // environments. So records in one global object for the same module key (URL) and
    // source text whose imports resolve to the same sources and names, and whose
    // loaders' module scopes have the same symbol tables, share the executable:
    // CodeBlocks, JIT code and the function declarations' executables. A record for
    // which these differ links its own, which later records are then compared against.
    std::optional<ModuleProgramExecutable::ImportedBindings> bindings = importedBindings(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    Vector<SymbolTable*> moduleScopeSymbolTables;
    for (JSScope* moduleScope = moduleLoader()->moduleScope(); moduleScope != globalObject->globalLexicalEnvironment(); moduleScope = moduleScope->next())
        moduleScopeSymbolTables.append(uncheckedDowncast<JSLexicalEnvironment>(moduleScope)->symbolTable());
    // Keyed by the module key's impl and the module scope's symbol table, so loaders with
    // different module scopes each keep their entry: a live entry whose key died and was
    // reused for another module fails the URL / source comparison and is replaced.
    auto& executables = globalObject->moduleProgramExecutables();
    JSGlobalObject::ModuleProgramExecutableKey key { moduleKey().impl(), moduleScopeSymbolTables.isEmpty() ? nullptr : moduleScopeSymbolTables.first() };
    if (bindings) {
        ModuleProgramExecutable* shared = executables.get(key);
        // (An executable whose code was deleted, ScriptExecutable::clearCode, is left to the
        // records that have it. The executable's code is in the mode of its first code, see
        // getUnlinkedCodeBlock, which has to be the one this record would ask for.)
        if (shared && shared->unlinkedCodeBlock() && shared->codeGenerationMode() == globalObject->defaultCodeGenerationMode() && shared->importedBindings() == bindings && shared->hasModuleScopeSymbolTables(moduleScopeSymbolTables)
            && shared->source().provider()->sourceURL() == sourceCode().provider()->sourceURL() && shared->source().provider()->hash() == sourceCode().provider()->hash() && shared->source().view() == sourceCode().view()) {
            m_moduleProgramExecutable.set(vm, this, shared);
            return shared;
        }
    }

    executable = ModuleProgramExecutable::tryCreate(globalObject, sourceCode(), WTF::move(bindings), moduleScopeSymbolTables);
    RETURN_IF_EXCEPTION(scope, nullptr);
    m_moduleProgramExecutable.set(vm, this, executable);
    if (executable->importedBindings())
        executables.set(key, Weak<ModuleProgramExecutable>(executable));
    return executable;
}

} // namespace JSC

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

#include "AsyncContextSwapScope.h"
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
        // Code that ModuleProgramExecutable::releaseUnlinkedCodeIfRecoverable() lets go of once the module has run is not
        // kept alive from here: its declarations are still in the payload the slots decode from.
        bool declarationsOutliveTheCodeBlock = slots->hasDecodeSource() && unlinkedCodeBlock->cachedPayloadIndex();
        if (!declarationsOutliveTheCodeBlock)
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
    // declaration links it. What it links from is this record's copy of the module's unlinked code if it has to keep one,
    // else the executable's while it has one (not released yet, or fetched again), else the bytecode cache payload.
    ModuleProgramExecutable* executable = uninstantiated->executable.get();
    // The declaration's code is every record's, and what it is specialized on is the executable's symbol table.
    RELEASE_ASSERT(environment->symbolTable() == executable->moduleEnvironmentSymbolTable());
    FunctionExecutable* functionExecutable = executable->linkedFunctionDeclaration(*index);
    if (!functionExecutable) {
        UnlinkedFunctionExecutable* unlinkedExecutable = nullptr;
        if (UnlinkedModuleProgramCodeBlock* unlinkedCodeBlock = uninstantiated->unlinkedCodeBlock.get())
            unlinkedExecutable = unlinkedCodeBlock->functionDecl(*index);
        else if (UnlinkedModuleProgramCodeBlock* unlinkedCodeBlock = executable->unlinkedCodeBlock())
            unlinkedExecutable = unlinkedCodeBlock->functionDecl(*index);
        else
            unlinkedExecutable = m_functionDeclarationSlots->decode(vm, *index);
        RELEASE_ASSERT(unlinkedExecutable);
        functionExecutable = executable->linkFunctionDeclaration(vm, *index, unlinkedExecutable);
    }
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

void JSModuleRecord::didFinishWithExecutable(VM& vm)
{
    ModuleProgramExecutable* executable = m_moduleProgramExecutable.get();
    if (!executable)
        return;
    m_moduleProgramExecutable.clear();
    executable->didFinishEvaluation(vm);
}

JSValue JSModuleRecord::evaluate(JSGlobalObject* globalObject, JSValue sentValue, JSValue resumeMode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Before the executable: a record with an error has given it up (CyclicModuleRecord::setEvaluationError).
    if (JSValue error = evaluationError()) {
        scope.throwException(globalObject, error);
        return { };
    }

    if (!m_moduleProgramExecutable) {
        ASSERT_NOT_REACHED_WITH_MESSAGE("Can't evaluate a JSModuleRecord that has no executable");
        return jsUndefined();
    }

    // Every module this one imports from has its environment now. Filling the import
    // slots here rather than on first use (JSModuleEnvironment::fillImportSlot) keeps
    // optimized code shared with other records from meeting an empty slot per record;
    // code only this record runs fills them on first use.
    ModuleProgramExecutable* executable = m_moduleProgramExecutable.get();

    if (executable->isShared()) {
        JSModuleEnvironment* environment = moduleEnvironment();
        forEachImportSlot([&](unsigned index, const Identifier& localName) {
            if (environment->importSlot(index))
                return IterationStatus::Continue;
            Resolution resolution = resolveImport(globalObject, localName);
            RETURN_IF_EXCEPTION(scope, IterationStatus::Done);
            if (resolution.type == Resolution::Type::Resolved)
                environment->importSlot(index).set(vm, environment, resolution.moduleRecord->moduleEnvironment());
            return IterationStatus::Continue;
        });
        if (scope.exception()) [[unlikely]] {
            didFinishWithExecutable(vm);
            return { };
        }
    }

    JSValue resultOrAwaitedValue = vm.interpreter.executeModuleProgram(this, executable, globalObject, moduleEnvironment(), sentValue, resumeMode);
    if (scope.exception()) [[unlikely]] {
        didFinishWithExecutable(vm);
        return { };
    }

    if (isTopLevelExecutionFinished())
        didFinishWithExecutable(vm);

    RELEASE_AND_RETURN(scope, resultOrAwaitedValue);
}

void JSModuleRecord::execute(JSGlobalObject* globalObject, JSPromise* capability)
{
    // ExecuteModule([capability])
    // https://tc39.es/ecma262/#sec-source-text-module-record-execute-module

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(BUN_JSC_ADDITIONS)
    // The first await of a module with top-level await captures the current async context
    // after the body returns (asyncModuleResolveEvaluation below), so the loader's context
    // spans that too; AsyncModuleExecutionResume then restores what each await captured.
    std::optional<AsyncContextSwapScope> loaderAsyncContext;
    if (JSValue asyncContext = moduleLoader()->asyncContext())
        loaderAsyncContext.emplace(vm, globalObject, asyncContext);
#endif

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
#if USE(BUN_JSC_ADDITIONS)
    ASSERT(!isPrelinked());
#endif
    if (!m_importSlotNames) {
        Vector<Identifier> names;
        for (const auto& entry : importEntries().values()) {
            if (entry.type == ImportEntryType::Namespace)
                continue;
            m_importSlotIndices.add(entry.localName.impl(), names.size());
            names.append(entry.localName);
        }
        m_importSlotNames = WTF::move(names);
    }
    return *m_importSlotNames;
}

unsigned JSModuleRecord::importSlotCount()
{
#if USE(BUN_JSC_ADDITIONS)
    if (isPrelinked())
        return prelinkedModule().importCount;
#endif
    return importSlotNames().size();
}

const Identifier& JSModuleRecord::importSlotLocalName(unsigned index)
{
#if USE(BUN_JSC_ADDITIONS)
    if (isPrelinked())
        return prelinkedGraph()->identifier(prelinkedGraph()->imports(prelinkedModule())[index].localSid);
#endif
    return importSlotNames()[index];
}

template<typename Functor>
void JSModuleRecord::forEachImportSlot(const Functor& functor)
{
#if USE(BUN_JSC_ADDITIONS)
    if (isPrelinked()) {
        auto imports = prelinkedGraph()->imports(prelinkedModule());
        for (unsigned i = 0; i < imports.size(); ++i) {
            if (!imports[i].isNamespace() && functor(i, importSlotLocalName(i)) == IterationStatus::Done)
                return;
        }
        return;
    }
#endif
    const Vector<Identifier>& names = importSlotNames();
    for (unsigned i = 0; i < names.size(); ++i) {
        if (functor(i, names[i]) == IterationStatus::Done)
            return;
    }
}

unsigned JSModuleRecord::importSlotIndex(UniquedStringImpl* localName)
{
#if USE(BUN_JSC_ADDITIONS)
    if (isPrelinked()) {
        auto imports = prelinkedGraph()->imports(prelinkedModule());
        const auto* import = prelinkedGraph()->findImport(prelinkedModule(), localName);
        RELEASE_ASSERT(import && !import->isNamespace());
        return import - imports.data();
    }
#endif
    importSlotNames();
    auto iterator = m_importSlotIndices.find(localName);
    RELEASE_ASSERT(iterator != m_importSlotIndices.end());
    return iterator->value;
}

auto JSModuleRecord::resolveImportWithSlot(JSGlobalObject* globalObject, const Identifier& localName, unsigned& importSlot) -> Resolution
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(BUN_JSC_ADDITIONS)
    // One lookup in the graph answers both; resolveImport() and importSlotIndex() would each make it.
    if (isPrelinked() && !Options::validatePrelinkedModuleInfo()) [[likely]] {
        const auto* import = prelinkedGraph()->findImport(prelinkedModule(), localName.impl());
        if (!import)
            return Resolution::notFound();
        std::optional<Resolution> resolution = tryResolveImportPrelinked(globalObject, *import);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (resolution) [[likely]] {
            importSlot = import - prelinkedGraph()->imports(prelinkedModule()).data();
            return *resolution;
        }
    }
#endif
    Resolution resolution = resolveImport(globalObject, localName);
    RETURN_IF_EXCEPTION(scope, Resolution::error());
    if (resolution.type == Resolution::Type::Resolved)
        importSlot = importSlotIndex(localName.impl());
    return resolution;
}

JSModuleEnvironment* JSModuleRecord::fillImportSlot(JSGlobalObject* globalObject, unsigned index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Resolution resolution = resolveImport(globalObject, importSlotLocalName(index));
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
    bool comparable = true;
    forEachImportSlot([&](unsigned index, const Identifier& localName) {
#if USE(BUN_JSC_ADDITIONS)
        // An import the graph resolved to a binding of another of its modules, and that this record resolves to that
        // binding of that module's record (what its code links against: tryResolveImportPrelinked keeps the answer), is
        // left out. It is the same variable of the same source for every record of this module of the graph that resolves
        // it so (createPrelinked() is given a graph module's own source), and only those are compared with each other
        // (ModuleProgramExecutable::wasLinkedFor). A record that resolves it to anything else lists it, and so differs.
        if (isPrelinked()) {
            const auto& import = prelinkedGraph()->imports(prelinkedModule())[index];
            if (import.resolution() == PrelinkedModuleGraph::ResolutionKind::Binding) {
                std::optional<Resolution> resolution = tryResolveImportPrelinked(globalObject, import);
                RETURN_IF_EXCEPTION(scope, IterationStatus::Done);
                if (resolution && resolution->type == Resolution::Type::Resolved) {
                    auto* exporter = dynamicDowncast<JSModuleRecord>(resolution->moduleRecord);
                    if (exporter && exporter->prelinkedGraph() == prelinkedGraph() && exporter->prelinkedIndex() == import.resolvedModule
                        && resolution->localName.impl() == prelinkedGraph()->identifier(import.resolvedLocalSid).impl())
                        return IterationStatus::Continue;
                }
            }
        }
#endif
        Resolution resolution = resolveImport(globalObject, localName);
        RETURN_IF_EXCEPTION(scope, IterationStatus::Done);
        if (resolution.type != Resolution::Type::Resolved)
            return IterationStatus::Continue;
        if (auto* sourceTextModule = dynamicDowncast<JSModuleRecord>(resolution.moduleRecord)) {
            bindings.append({ localName.impl(), sourceTextModule->sourceCode().provider(), resolution.localName.impl(), 0, index });
            return IterationStatus::Continue;
        }
        JSModuleEnvironment* environment = resolution.moduleRecord->moduleEnvironmentMayBeNull();
        if (!environment) {
            comparable = false;
            return IterationStatus::Done;
        }
        SymbolTable* symbolTable = environment->symbolTable();
        ConcurrentJSLocker locker(symbolTable->m_lock);
        auto iterator = symbolTable->find(locker, resolution.localName.impl());
        RELEASE_ASSERT(iterator != symbolTable->end(locker));
        bindings.append({ localName.impl(), nullptr, resolution.localName.impl(), iterator->value.scopeOffset().offset(), index });
        return IterationStatus::Continue;
    });
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (!comparable)
        return std::nullopt;
    return bindings;
}

bool JSModuleRecord::resolvesImportsLike(JSGlobalObject* globalObject, ModuleProgramExecutable* executable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(BUN_JSC_ADDITIONS)
    if (!executable->wasLinkedFor(prelinkedGraph(), isPrelinked() ? prelinkedIndex() : PrelinkedModuleGraph::noModule))
        return false;
#endif
    if (!executable->linkerImportedBindings()) {
        // Nothing resolved the linker's imports for this when it linked the executable: there was no record to compare it
        // with. What they resolve to does not change, so now is as good. A linker that is gone, or whose imports cannot
        // be compared, leaves nothing to compare with.
        JSModuleRecord* linker = executable->linker();
        if (!linker)
            return false;
        std::optional<ModuleProgramExecutable::ImportedBindings> bindings = linker->importedBindings(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        executable->setLinkerImportedBindings(WTF::move(bindings));
        if (!executable->linkerImportedBindings())
            return false;
    }
    std::optional<ModuleProgramExecutable::ImportedBindings> mine = importedBindings(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    return mine && *mine == *executable->linkerImportedBindings();
}

// Whether linked code cannot tell the two sources apart. Besides the text, running code observes its
// executable's source through error positions and stack traces (the URL and where the source starts) and
// through import(), which hands the SourceOrigin to the embedder (CallFrame::callerSourceOrigin). The
// SourceOrigin carries the embedder's ScriptFetcher, which is how an embedder knows which module asks.
// A CodeBlock also caches whether its source could be tainted.
static bool hasSameObservableSource(const SourceCode& a, const SourceCode& b)
{
    SourceProvider* providerA = a.provider();
    SourceProvider* providerB = b.provider();
    return providerA->sourceOrigin() == providerB->sourceOrigin()
        && providerA->sourceTaintedOrigin() == providerB->sourceTaintedOrigin()
        && providerA->sourceURL() == providerB->sourceURL()
        && a.firstLine() == b.firstLine() && a.startColumn() == b.startColumn()
        && providerA->hash() == providerB->hash() && a.view() == b.view();
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
    // source (hasSameObservableSource) whose imports resolve to the same sources and names, and whose
    // loaders' module scopes have the same symbol tables, share the executable:
    // CodeBlocks, JIT code and the function declarations' executables. A record for
    // which these differ links its own, which later records are then compared against.
    // Nothing about the imports is resolved for this until a second record for the key and module scope turns up
    // (resolvesImportsLike): a record that is the only one to link a module pays for the comparison nothing.
    Vector<SymbolTable*> moduleScopeSymbolTables;
    for (JSScope* moduleScope = moduleLoader()->moduleScope(); moduleScope != globalObject->globalLexicalEnvironment(); moduleScope = moduleScope->next())
        moduleScopeSymbolTables.append(uncheckedDowncast<JSLexicalEnvironment>(moduleScope)->symbolTable());
    // Keyed by the module key's impl and the module scope's symbol table, so loaders with
    // different module scopes each keep their entry: a live entry whose key died and was
    // reused for another module fails the URL / source comparison and is replaced.
    auto& executables = globalObject->moduleProgramExecutables();
    JSGlobalObject::ModuleProgramExecutableKey key { moduleKey().impl(), moduleScopeSymbolTables.isEmpty() ? nullptr : moduleScopeSymbolTables.first() };
    {
        ModuleProgramExecutable* shared = executables.get(key);
        // (An executable whose code was deleted, ScriptExecutable::clearCode, is left to the
        // records that have it. One that only let go of unlinked code it can decode again,
        // releaseUnlinkedCodeIfRecoverable, is adopted and decodes it again. Either way the
        // executable's code is in the mode of its first code, see getUnlinkedCodeBlock, which
        // has to be the one this record would ask for.)
        if (shared && (shared->unlinkedCodeBlock() || shared->hasReleasedUnlinkedCode()) && shared->codeGenerationMode() == globalObject->defaultCodeGenerationMode()
            && shared->hasModuleScopeSymbolTables(moduleScopeSymbolTables)
            && hasSameObservableSource(shared->source(), sourceCode())) {
            bool alike = resolvesImportsLike(globalObject, shared);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (alike) {
                if (!shared->unlinkedCodeBlock()) {
                    shared->getUnlinkedCodeBlock(globalObject);
                    RETURN_IF_EXCEPTION(scope, nullptr);
                }
                shared->willBeEvaluatedByAnotherRecord();
                shared->didShare();
                m_moduleProgramExecutable.set(vm, this, shared);
                return shared;
            }
        }
    }

    executable = ModuleProgramExecutable::tryCreate(globalObject, sourceCode(), this, moduleScopeSymbolTables);
    RETURN_IF_EXCEPTION(scope, nullptr);
    executable->willBeEvaluatedByAnotherRecord();
    m_moduleProgramExecutable.set(vm, this, executable);
    executables.set(key, Weak<ModuleProgramExecutable>(executable));
    return executable;
}

} // namespace JSC

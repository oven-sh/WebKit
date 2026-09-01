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
#include "IdentifierInlines.h"
#include "Interpreter.h"
#include <wtf/TZoneMallocInlines.h>
#include <ranges>
#include "StrongInlines.h"
#include "JSGenerator.h"
#include "JSNativeStdFunction.h"
#include "InternalFieldTuple.h"
#include "JSAsyncFunction.h"
#include "JSAsyncGeneratorFunction.h"
#include "JSCInlines.h"
#include "JSGeneratorFunction.h"
#include "JSMicrotask.h"
#include "JSModuleEnvironment.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "ModuleGraphInstance.h"
#include "JSMapInlines.h"
#include "JSLexicalEnvironmentInlines.h"
#include "SymbolTableInlines.h"
#include "SyntheticModuleRecord.h"
#include "JSPromise.h"
#include "JSPromiseCombinatorsGlobalContext.h"
#include "ModuleProgramExecutable.h"
#include "ModuleProgramCodeBlock.h"
#include "SourceProfiler.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/text/MakeString.h>

namespace JSC {

const ClassInfo JSModuleRecord::s_info = { "ModuleRecord"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSModuleRecord) };

JSModuleRecord* JSModuleRecord::create(JSGlobalObject* globalObject, VM& vm, Structure* structure, const Identifier& moduleKey, const SourceCode& sourceCode, CodeFeatures features)
{
    JSModuleRecord* instance = new (NotNull, allocateCell<JSModuleRecord>(vm)) JSModuleRecord(vm, structure, moduleKey, sourceCode, features);
    instance->finishCreation(globalObject, vm);
    return instance;
}

JSModuleRecord::JSModuleRecord(VM& vm, Structure* structure, const Identifier& moduleKey, const SourceCode& sourceCode, CodeFeatures features)
    : Base(vm, structure, moduleKey, SourceProviderSourceType::Module)
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
    visitor.append(thisObject->m_retainedExecutable);
    visitor.append(thisObject->m_retainedCodeBlock);
    {
        Locker locker { thisObject->cellLock() };
        for (auto& barrier : thisObject->m_functionDeclExecutables)
            visitor.append(barrier);
    }

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
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Module graph instances: by the time a module evaluates, its dependencies'
    // environments exist; fill the import slots the fast paths read.
    if (m_moduleEnvironment && m_moduleEnvironment->importSlotCount() && internalField(Field::State).get() == jsNumber(static_cast<int32_t>(State::Init))) {
        for (unsigned i = 0; i < importSlotCount(); ++i) {
            if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(importedRecordAt(i))) {
                synthetic->materializePrimaryIfPending(globalObject);
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
        m_moduleEnvironment->fillImportSlots(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }

    if (!m_moduleProgramExecutable) {
        ASSERT_NOT_REACHED_WITH_MESSAGE("Can't evaluate a JSModuleRecord that has no executable");
        return jsUndefined();
    }

    if (JSValue error = evaluationError()) {
        scope.throwException(globalObject, error);
        return { };
    }

    ModuleProgramExecutable* executable = m_moduleProgramExecutable.get();
    JSValue resultOrAwaitedValue = vm.interpreter.executeModuleProgram(this, executable, globalObject, moduleEnvironment(), sentValue, resumeMode);
    RETURN_IF_EXCEPTION(scope, { });

    if (m_retainedExecutable)
        pinRetainedCodeBlock(vm);
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
        globalObject->moduleLoader()->evaluate(globalObject, identifierToJSValue(vm, moduleKey()), this, nullptr, jsUndefined(), jsNumber(static_cast<int32_t>(ResumeMode::NormalMode)));
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
        JSValue result = globalObject->moduleLoader()->evaluate(globalObject, identifierToJSValue(vm, moduleKey()), this, nullptr, jsUndefined(), jsNumber(static_cast<int32_t>(ResumeMode::NormalMode)));
        asyncModuleResolveEvaluation(globalObject, vm, scope, this, result);
    }
    // 11. Return unused.
}

void JSModuleRecord::retainForGraphInstances(VM& vm, ModuleProgramExecutable* executable, Vector<WriteBarrier<FunctionExecutable>>&& functionDeclExecutables)
{
    m_retainedExecutable.set(vm, this, executable);
    pinRetainedCodeBlock(vm);
    {
        // The concurrent marker iterates m_functionDeclExecutables under the cell lock.
        Locker locker { cellLock() };
        m_functionDeclExecutables = WTF::move(functionDeclExecutables);
    }
    for (auto& barrier : m_functionDeclExecutables) {
        if (barrier)
            vm.writeBarrier(this, barrier.get());
    }
}

// InitializeEnvironment steps 5-24 against a fresh environment that belongs to
// `instance`, reusing everything the primary instantiation linked. Recursively
// instantiates every source text dependency into the instance first.
JSModuleEnvironment* JSModuleRecord::createInstanceEnvironment(JSGlobalObject* globalObject, ModuleGraphInstance* instance, Vector<JSModuleRecord*>& created)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (instance->isCleared()) {
        throwTypeError(globalObject, scope, "Module graph instance was disposed"_s);
        return nullptr;
    }

    if (JSModuleEnvironment* existing = instance->environment(this))
        return existing;

    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return nullptr;
    }
    // Evaluate() step 2 for the template: it must have completed Link().
    if (status() == Status::New || status() == Status::Unlinked || status() == Status::Linking) {
        throwTypeError(globalObject, scope, makeString("Module '"_s, moduleKey().string(), "' is not linked and cannot be instantiated into a module graph instance"_s));
        return nullptr;
    }
    ModuleProgramExecutable* executable = m_retainedExecutable.get();
    if (!executable) {
        throwTypeError(globalObject, scope, makeString("Module '"_s, moduleKey().string(), "' cannot be instantiated again: it was linked before module graph instances were enabled"_s));
        return nullptr;
    }

    SymbolTable* symbolTable = executable->moduleEnvironmentSymbolTable();
    // The record's CodeBlocks are shared by every instance and were linked
    // against the primary environment's scope chain, so an instance's parent
    // scope must have the same shape: the global's module environment parent
    // scope, or an overlay from createModuleScopeOverlay when the primary graph
    // is under one too.
    JSScope* parentScope = instance->parentScope() ? instance->parentScope() : globalObject->moduleEnvironmentParentScope();
    if (instance->parentScope() && !(globalObject->isModuleScopeOverlay(parentScope) && globalObject->primaryModuleScopeOverlay())) {
        throwTypeError(globalObject, scope, "A module graph instance's parent scope must be a module scope overlay of a global object whose modules are under one"_s);
        return nullptr;
    }
    JSModuleEnvironment* env = JSModuleEnvironment::create(vm, globalObject, parentScope, symbolTable, jsTDZValue(), this);
    RETURN_IF_EXCEPTION(scope, nullptr);
    env->setGraphInstance(vm, instance);
    // Register before recursing so import cycles terminate (status Linked), and
    // in `created` so a failure further on can roll every record of this
    // instantiation back out of the instance (Link() step 4.a).
    instance->add(vm, this, env);
    created.append(this);

    for (const auto& request : requestedModules()) {
        AbstractModuleRecord* imported = hostResolveImportedModule(globalObject, request.m_specifier, request.type());
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (auto* importedSource = dynamicDowncast<JSModuleRecord>(imported)) {
            importedSource->createInstanceEnvironment(globalObject, instance, created);
            RETURN_IF_EXCEPTION(scope, nullptr);
        } else if (is<CyclicModuleRecord>(imported)) {
            // Every Cyclic Module Record an instance reaches must have its own
            // state in the instance (the evaluation algorithm never falls back to
            // the primary graph's); only Source Text Module Records can today.
            throwTypeError(globalObject, scope, makeString("Module '"_s, imported->moduleKey().string(), "' cannot be instantiated into a module graph instance (only JavaScript and synthetic modules can)"_s));
            return nullptr;
        } else {
            // Synthetic records with per-instance state get an environment in
            // the instance; others are shared with the primary graph.
            imported->graphInstanceEnvironment(globalObject, instance, true);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }

    // 7.c. Namespace imports bind the exporter's namespace in this instance;
    // single imports that resolve to a namespace (export * as ns from) get that
    // namespace materialised in its module's instance environment.
    for (const auto& [key, in] : importEntries()) {
        AbstractModuleRecord* importedModule = hostResolveImportedModule(globalObject, in.moduleRequest, in.moduleRequestType);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (in.type == ImportEntryType::Namespace) {
            JSModuleNamespaceObject* ns = importedModule->getModuleNamespace(globalObject, instance, in.phase);
            RETURN_IF_EXCEPTION(scope, nullptr);
            bool putResult = false;
            symbolTablePutTouchWatchpointSet(env, globalObject, in.localName, ns, false, true, putResult);
            RETURN_IF_EXCEPTION(scope, nullptr);
            continue;
        }
        Resolution resolution = importedModule->resolveExport(globalObject, in.importName);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (resolution.type == Resolution::Type::Resolved && resolution.localName == vm.propertyNames->starNamespacePrivateName) {
            resolution.moduleRecord->getModuleNamespace(globalObject, instance);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }

    // 21. var declarations start as undefined (lexical ones stay in TDZ).
    UnlinkedModuleProgramCodeBlock* unlinkedCodeBlock = executable->unlinkedCodeBlock();
    for (const auto& variable : unlinkedCodeBlock->variableDeclarations()) {
        SymbolTableEntry::Fast entry = symbolTable->get(variable.key.get());
        if (!entry.varOffset().isStack()) {
            bool putResult = false;
            symbolTablePutTouchWatchpointSet(env, globalObject, Identifier::fromUid(vm, variable.key.get()), jsUndefined(), false, true, putResult);
            RETURN_IF_EXCEPTION(scope, nullptr);
        }
    }

    // 24. Function declarations: new function objects over the executables the
    // primary instantiation linked, closed over this environment.
    for (size_t i = 0, count = unlinkedCodeBlock->numberOfFunctionDecls(); i < count; ++i) {
        FunctionExecutable* functionExecutable = i < m_functionDeclExecutables.size() ? m_functionDeclExecutables[i].get() : nullptr;
        if (!functionExecutable)
            continue;
        SourceParseMode parseMode = functionExecutable->parseMode();
        JSFunction* function = nullptr;
        if (isAsyncGeneratorWrapperParseMode(parseMode))
            function = JSAsyncGeneratorFunction::create(vm, globalObject, functionExecutable, env);
        else if (isGeneratorWrapperParseMode(parseMode))
            function = JSGeneratorFunction::create(vm, globalObject, functionExecutable, env);
        else if (isAsyncFunctionWrapperParseMode(parseMode))
            function = JSAsyncFunction::create(vm, globalObject, functionExecutable, env);
        else
            function = JSFunction::create(vm, globalObject, functionExecutable, env);
        RETURN_IF_EXCEPTION(scope, nullptr);
        bool putResult = false;
        symbolTablePutTouchWatchpointSet(env, globalObject, unlinkedCodeBlock->functionDecl(i)->name(), function, false, true, putResult);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    // import.meta: a fresh object per instance, carrying the instance for the host.
    if (m_features & ImportMetaFeature) {
        JSObject* meta = globalObject->moduleLoader()->createImportMetaProperties(globalObject, identifierToJSValue(vm, moduleKey()), this, nullptr);
        RETURN_IF_EXCEPTION(scope, nullptr);
        meta->putDirect(vm, vm.propertyNames->builtinNames().moduleGraphInstancePrivateName(), instance, static_cast<unsigned>(PropertyAttribute::DontEnum));
        bool putResult = false;
        symbolTablePutTouchWatchpointSet(env, globalObject, vm.propertyNames->builtinNames().metaPrivateName(), meta, false, true, putResult);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    return env;
}

// Link() step 4.a for an instance: an instantiation that failed part-way leaves
// no source-text environment behind, so a retry starts clean instead of
// evaluating half-initialised environments. Synthetic per-instance environments
// created on the way are complete on creation (their bindings are set when the
// environment is made) and stay in the instance, as an import() of that module
// alone would have left them.
static void rollBackInstantiation(ModuleGraphInstance* instance, const Vector<JSModuleRecord*>& created)
{
    for (JSModuleRecord* record : created)
        instance->remove(record);
}

static void fillGraphInstanceImportSlots(JSGlobalObject* globalObject, const Vector<JSModuleRecord*>& created, ModuleGraphInstance* instance)
{
    for (JSModuleRecord* record : created) {
        if (JSModuleEnvironment* environment = instance->environment(record))
            environment->fillImportSlots(globalObject);
    }
}

JSModuleEnvironment* JSModuleRecord::instantiateIntoGraphInstance(JSGlobalObject* globalObject, ModuleGraphInstance* instance, ModulePhase phase)
{
    ModuleGraphInstance::BusyScope busy(globalObject, instance);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Vector<JSModuleRecord*> created;
    JSModuleEnvironment* env = createInstanceEnvironment(globalObject, instance, created);
    if (scope.exception()) [[unlikely]] {
        rollBackInstantiation(instance, created);
        return nullptr;
    }
    fillGraphInstanceImportSlots(globalObject, created, instance);
    if (phase == ModulePhase::Defer) {
        // import defer: only the asynchronous transitive dependencies evaluate
        // now; that must complete synchronously here.
        OrderedHashSet<AbstractModuleRecord*> asyncDependencies;
        UncheckedKeyHashSet<AbstractModuleRecord*> seen;
        gatherAsynchronousTransitiveDependencies(asyncDependencies, seen, instance);
        for (AbstractModuleRecord* dependency : asyncDependencies) {
            auto* cyclic = dynamicDowncast<CyclicModuleRecord>(dependency);
            if (!cyclic)
                continue;
#if USE(BUN_JSC_ADDITIONS)
            JSPromise* promise = cyclic->evaluate(globalObject, nullptr, instance);
#else
            JSPromise* promise = cyclic->evaluate(globalObject, instance);
#endif
            RETURN_IF_EXCEPTION(scope, nullptr);
            switch (promise->status()) {
            case JSPromise::Status::Fulfilled:
                continue;
            case JSPromise::Status::Rejected:
                promise->markAsHandled();
                scope.throwException(globalObject, promise->result());
                return nullptr;
            case JSPromise::Status::Pending:
                throwTypeError(globalObject, scope, makeString("Module '"_s, cyclic->moduleKey().string(), "' uses top-level await and cannot be evaluated synchronously"_s));
                return nullptr;
            }
        }
        return env;
    }
#if USE(BUN_JSC_ADDITIONS)
    JSPromise* promise = CyclicModuleRecord::evaluate(globalObject, nullptr, instance);
#else
    JSPromise* promise = CyclicModuleRecord::evaluate(globalObject, instance);
#endif
    RETURN_IF_EXCEPTION(scope, nullptr);
    switch (promise->status()) {
    case JSPromise::Status::Fulfilled:
        return env;
    case JSPromise::Status::Rejected:
        promise->markAsHandled();
        scope.throwException(globalObject, promise->result());
        return nullptr;
    case JSPromise::Status::Pending:
        // Top-level await somewhere in the sub-graph: a synchronous caller
        // cannot wait for it (the asynchronous form can).
        throwTypeError(globalObject, scope, makeString("Module '"_s, moduleKey().string(), "' or one of its dependencies uses top-level await and cannot be evaluated synchronously"_s));
        return nullptr;
    }
    return env;
}

JSPromise* JSModuleRecord::instantiateIntoGraphInstanceAsync(JSGlobalObject* globalObject, ModuleGraphInstance* instance, ModulePhase phase, JSPromise* dynamicImportPromise)
{
    ModuleGraphInstance::BusyScope busy(globalObject, instance);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<JSModuleRecord*> created;
    createInstanceEnvironment(globalObject, instance, created);
    if (scope.exception()) [[unlikely]] {
        rollBackInstantiation(instance, created);
        JSPromise* rejected = JSPromise::create(vm, globalObject->promiseStructure());
        rejected->rejectWithCaughtException(vm, scope);
        return rejected;
    }
    fillGraphInstanceImportSlots(globalObject, created, instance);
    if (phase == ModulePhase::Defer) {
        // import defer: evaluate the asynchronous transitive dependencies now
        // and wait for all of them; the rest runs on first namespace access.
        OrderedHashSet<AbstractModuleRecord*> asyncDependencies;
        UncheckedKeyHashSet<AbstractModuleRecord*> seen;
        gatherAsynchronousTransitiveDependencies(asyncDependencies, seen, instance);
        MarkedArgumentBuffer promises;
        for (AbstractModuleRecord* dependency : asyncDependencies) {
            auto* cyclic = dynamicDowncast<CyclicModuleRecord>(dependency);
            if (!cyclic)
                continue;
#if USE(BUN_JSC_ADDITIONS)
            JSPromise* promise = cyclic->evaluate(globalObject, dynamicImportPromise, instance);
#else
            JSPromise* promise = cyclic->evaluate(globalObject, instance);
#endif
            if (scope.exception()) [[unlikely]] {
                JSPromise* rejected = JSPromise::create(vm, globalObject->promiseStructure());
                rejected->rejectWithCaughtException(vm, scope);
                return rejected;
            }
            promises.append(promise);
        }
        if (promises.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            JSPromise* rejected = JSPromise::create(vm, globalObject->promiseStructure());
            rejected->rejectWithCaughtException(vm, scope);
            return rejected;
        }
        // SafePerformPromiseAll: an AND-join through internal microtasks (first
        // rejection rejects). Nothing here is script-observable or Strong<>-rooted.
        JSPromise* result = JSPromise::create(vm, globalObject->promiseStructure());
        if (promises.isEmpty()) {
            result->resolve(globalObject, vm, jsUndefined());
            RELEASE_AND_RETURN(scope, result);
        }
        auto* joinContext = JSPromiseCombinatorsGlobalContext::create(vm, result, jsUndefined(), promises.size());
        for (unsigned i = 0; i < promises.size(); ++i)
            uncheckedDowncast<JSPromise>(promises.at(i))->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleGraphInstanceDependencySettled, result, joinContext);
        return result;
    }
#if USE(BUN_JSC_ADDITIONS)
    JSPromise* promise = CyclicModuleRecord::evaluate(globalObject, dynamicImportPromise, instance);
#else
    UNUSED_PARAM(dynamicImportPromise);
    JSPromise* promise = CyclicModuleRecord::evaluate(globalObject, instance);
#endif
    if (scope.exception()) [[unlikely]] {
        JSPromise* rejected = JSPromise::create(vm, globalObject->promiseStructure());
        rejected->rejectWithCaughtException(vm, scope);
        return rejected;
    }
    return promise;
}

// ExecuteModule for a module graph instance: the record's body against its
// environment in the instance, with the instance's own execution state for a
// body with top-level await.
void JSModuleRecord::executeInstance(JSGlobalObject* globalObject, ModuleRecordInstance* recordInstance, JSPromise* capability)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasTLA()) {
        ASSERT(!capability);
        vm.interpreter.executeModuleProgram(this, recordInstance, m_retainedExecutable.get(), globalObject, recordInstance->environment(), jsUndefined(), jsNumber(static_cast<int32_t>(ResumeMode::NormalMode)));
        pinRetainedCodeBlock(vm);
        RETURN_IF_EXCEPTION(scope, void());
        return;
    }
    ASSERT(capability);
    recordInstance->setAsyncCapability(vm, capability);
    JSValue result = evaluateInstance(globalObject, recordInstance, jsUndefined(), jsNumber(static_cast<int32_t>(ResumeMode::NormalMode)));
    asyncModuleResolveEvaluation(globalObject, vm, scope, recordInstance, result);
}

// One step of a top-level-await module body in an instance (first run or a
// resumption after an await).
JSValue JSModuleRecord::evaluateInstance(JSGlobalObject* globalObject, ModuleRecordInstance* recordInstance, JSValue sentValue, JSValue resumeMode)
{
    VM& vm = globalObject->vm();
    JSValue result = vm.interpreter.executeModuleProgram(this, recordInstance, m_retainedExecutable.get(), globalObject, recordInstance->environment(), sentValue, resumeMode);
    pinRetainedCodeBlock(vm);
    return result;
}

void JSModuleRecord::pinRetainedCodeBlock(VM& vm)
{
    if (m_retainedCodeBlock || !m_retainedExecutable)
        return;
    if (ModuleProgramCodeBlock* codeBlock = m_retainedExecutable->codeBlock())
        m_retainedCodeBlock.set(vm, this, codeBlock);
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

    RELEASE_AND_RETURN(scope, executable);
}

} // namespace JSC

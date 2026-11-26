/*
 * Copyright (C) 2015-2021, 2026 Apple Inc. All rights reserved.
 * Copyright (C) 2016 Yusuke Suzuki <utatane.tea@gmail.com>.
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
#include "JSModuleLoader.h"

#if ENABLE(AOT)
#include "AOTModuleRecord.h"
#endif
#include "ProgramExecutable.h"

#include "AsyncContextSwapScope.h"
#include "BuiltinNames.h"
#include "Completion.h"
#include "DeferTermination.h"
#include "FrameTracers.h"
#include "GlobalObjectMethodTable.h"
#include "ImportOrderWalk.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleNamespaceObject.h"
#include "JSModuleRecord.h"
#include "JSPromise.h"
#include "JSSourceCode.h"
#include "JSWebAssembly.h"
#include "Microtask.h"
#include "ModuleAnalyzer.h"
#include "ModuleLoadingContext.h"
#include "ModuleRegistryEntry.h"
#include "Nodes.h"
#include "ObjectConstructor.h"
#include "Parser.h"
#include "ParserError.h"
#include "Symbol.h"
#include "SyntheticModuleRecord.h"
#include "TopExceptionScope.h"
#include "VMTrapsInlines.h"
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/text/MakeString.h>

namespace JSC {

namespace JSModuleLoaderInternal {
static constexpr unsigned maximumResolutionFailures = 128;
}

static Identifier jsValueToSpecifier(JSGlobalObject* globalObject, JSValue value)
{
    if (value.isSymbol())
        return Identifier::fromUid(uncheckedDowncast<Symbol>(value)->privateName());
    ASSERT(value.isString());
    return asString(value)->toIdentifier(globalObject);
}

bool JSModuleLoader::isFetchError(JSGlobalObject* globalObject, ErrorInstance* error)
{
    VM& vm = globalObject->vm();
    return error->hasOwnProperty(globalObject, vm.propertyNames->builtinNames().moduleFetchFailureKindPrivateName());
}

// Web Platform Tests requires that import() promises reject with difference error object instances for fetch errors.
// It also requires that errors be cached. To satisfy both requirements, it's necessary to duplicate errors.
ErrorInstance* JSModuleLoader::duplicateTypeError(JSGlobalObject* globalObject, ErrorInstance* error)
{
    ASSERT(error);

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* copy = uncheckedDowncast<ErrorInstance>(createTypeErrorCopy(globalObject, error));
    RETURN_IF_EXCEPTION(scope, nullptr);

    auto copyField = [&](const Identifier& name) -> bool {
        if (JSValue value = error->getDirect(vm, name)) {
            copy->putDirect(vm, name, value);
            RETURN_IF_EXCEPTION(scope, false);
        }

        return true;
    };

    const BuiltinNames& names = vm.propertyNames->builtinNames();

    if (!copyField(names.moduleFetchFailureKindPrivateName()))
        return nullptr;

    if (!copyField(names.moduleFailureModuleRecordPrivateName()))
        return nullptr;

    if (!copyField(names.moduleFailureModuleKeyPrivateName()))
        return nullptr;

    if (!copyField(names.moduleFailureModuleTypePrivateName()))
        return nullptr;

    if (!copyField(names.moduleFailureKindPrivateName()))
        return nullptr;

    return copy;
}

ErrorInstance* JSModuleLoader::duplicateError(JSGlobalObject* globalObject, ErrorInstance* error)
{
    ASSERT(error);

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (error->errorType() == ErrorType::TypeError)
        RELEASE_AND_RETURN(scope, JSModuleLoader::duplicateTypeError(globalObject, error));

    String message = error->sanitizedMessageString(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);

    ErrorInstance* out = ErrorInstance::create(globalObject, WTF::move(message), error->errorType(), { }, { }, { });
    RETURN_IF_EXCEPTION(scope, nullptr);

    auto copyField = [&](const Identifier& name) -> bool {
        if (JSValue value = error->getDirect(vm, name)) {
            out->putDirect(vm, name, value);
            RETURN_IF_EXCEPTION(scope, false);
        }
        return true;
    };

    const BuiltinNames& names = vm.propertyNames->builtinNames();

    if (!copyField(names.moduleFetchFailureKindPrivateName()))
        return nullptr;

    if (!copyField(names.moduleFailureModuleRecordPrivateName()))
        return nullptr;

    if (!copyField(names.moduleFailureModuleKeyPrivateName()))
        return nullptr;

    if (!copyField(names.moduleFailureModuleTypePrivateName()))
        return nullptr;

    if (!copyField(names.moduleFailureKindPrivateName()))
        return nullptr;

    return out;
}

ErrorInstance* JSModuleLoader::maybeDuplicateFetchError(JSGlobalObject* globalObject, ErrorInstance* error)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (error->hasOwnProperty(globalObject, vm.propertyNames->builtinNames().moduleFetchFailureKindPrivateName())) {
        error = JSModuleLoader::duplicateError(globalObject, error);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    RELEASE_AND_RETURN(scope, error);
}

JSModuleLoader::ModuleFailure::ModuleFailure(AbstractModuleRecord* source, ScriptFetchParameters::Type type, Kind kind)
    : m_source(source)
    , m_key(source->moduleKey())
    , m_type(type)
    , m_kind(kind)
{
}

JSModuleLoader::ModuleFailure::ModuleFailure(Identifier key, ScriptFetchParameters::Type type, Kind kind)
    : m_source(nullptr)
    , m_key(WTF::move(key))
    , m_type(type)
    , m_kind(kind)
{
}

bool JSModuleLoader::ModuleFailure::isEvaluationError(const Identifier& expectedSpecifier, ScriptFetchParameters::Type expectedType) const
{
    return m_kind == ModuleFailure::Kind::Evaluation || (!m_key.isNull() && m_key != expectedSpecifier) || (m_type != ScriptFetchParameters::None && m_type != expectedType);
}

JSModuleLoader::ModuleFailure::operator bool() const
{
    return m_source != nullptr || !m_key.isNull() || m_type != ScriptFetchParameters::Type::None || m_kind != ModuleFailure::Kind::Unknown;
}

JSModuleLoader::ModuleFailure JSModuleLoader::getErrorInfo(JSGlobalObject* globalObject, ErrorInstance* error)
{
    VM& vm = globalObject->vm();
    JSModuleLoader::ModuleFailure failure;
    if (JSValue sourceValue = error->getDirect(vm, vm.propertyNames->builtinNames().moduleFailureModuleRecordPrivateName()))
        failure.m_source = dynamicDowncast<AbstractModuleRecord>(sourceValue);
    if (JSValue keyValue = error->getDirect(vm, vm.propertyNames->builtinNames().moduleFailureModuleKeyPrivateName()))
        failure.m_key = jsValueToSpecifier(globalObject, keyValue);
    if (JSValue typeValue = error->getDirect(vm, vm.propertyNames->builtinNames().moduleFailureModuleTypePrivateName()))
        failure.m_type = static_cast<ScriptFetchParameters::Type>(typeValue.asInt32());
    if (JSValue kindValue = error->getDirect(vm, vm.propertyNames->builtinNames().moduleFailureKindPrivateName()))
        failure.m_kind = static_cast<JSModuleLoader::ModuleFailure::Kind>(kindValue.asInt32());
    return failure;
}

void JSModuleLoader::attachErrorInfo(JSGlobalObject* globalObject, ErrorInstance* error, AbstractModuleRecord* source, const Identifier& key, ScriptFetchParameters::Type type, JSModuleLoader::ModuleFailure::Kind kind)
{
    VM& vm = globalObject->vm();
    if (source)
        error->putDirect(vm, vm.propertyNames->builtinNames().moduleFailureModuleRecordPrivateName(), source);
    if (!key.isNull())
        error->putDirect(vm, vm.propertyNames->builtinNames().moduleFailureModuleKeyPrivateName(), identifierToJSValue(vm, key));
    if (type != ScriptFetchParameters::Type::None)
        error->putDirect(vm, vm.propertyNames->builtinNames().moduleFailureModuleTypePrivateName(), jsNumber(std::to_underlying(type)));
    if (kind != JSModuleLoader::ModuleFailure::Kind::Unknown)
        error->putDirect(vm, vm.propertyNames->builtinNames().moduleFailureKindPrivateName(), jsNumber(std::to_underlying(kind)));
}

bool JSModuleLoader::attachErrorInfo(JSGlobalObject* globalObject, Exception* exception, AbstractModuleRecord* source, const Identifier& key, ScriptFetchParameters::Type type, ModuleFailure::Kind kind)
{
    if (auto* error = dynamicDowncast<ErrorInstance>(exception->value())) {
        attachErrorInfo(globalObject, error, source, key, type, kind);
        return true;
    }
    return false;
}

bool JSModuleLoader::attachErrorInfo(JSGlobalObject* globalObject, ThrowScope& scope, AbstractModuleRecord* source, const Identifier& key, ScriptFetchParameters::Type type, ModuleFailure::Kind kind)
{
    if (Exception* exception = scope.exception())
        return attachErrorInfo(globalObject, exception, source, key, type, kind);
    return false;
}

const ClassInfo JSModuleLoader::s_info = { "ModuleLoader"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(JSModuleLoader) };

JSModuleLoader::JSModuleLoader(VM& vm, Structure* structure, JSScope* moduleScope)
    : JSCell(vm, structure)
    , m_moduleScope(moduleScope, WriteBarrierEarlyInit)
{
}

JSModuleLoader* JSModuleLoader::create(JSGlobalObject* globalObject, VM& vm, JSScope* moduleScope)
{
    return create(globalObject, vm, vm.moduleLoaderStructure.get(), moduleScope);
}

void JSModuleLoader::destroy(JSCell* cell)
{
    SUPPRESS_MEMORY_UNSAFE_CAST auto* thisObject = static_cast<JSModuleLoader*>(cell);
    thisObject->JSModuleLoader::~JSModuleLoader();
}

void JSModuleLoader::finishCreation(JSGlobalObject* globalObject, VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
    // JSModuleRecord::getOrMakeExecutable and CodeCache rely on this shape.
    for (JSScope* scope = moduleScope(); scope != globalObject->globalLexicalEnvironment(); scope = scope->next())
        RELEASE_ASSERT(scope && scope->type() == LexicalEnvironmentType);
}

template<typename Visitor>
void JSModuleLoader::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSModuleLoader* thisObject = uncheckedDowncast<JSModuleLoader>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_moduleScope);
#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_asyncContext);
    visitor.append(thisObject->m_endOfImportOrderWalk);
    visitor.append(thisObject->m_turnToBeEvaluated);
#endif
    Locker locker { thisObject->cellLock() };
    auto moduleMapValues = thisObject->m_moduleMap.values();
    visitor.append(moduleMapValues.begin(), moduleMapValues.end());
    for (auto& failure : thisObject->m_resolutionFailures.values())
        visitor.append(failure.error);
    for (auto& [key, loadedModule] : thisObject->m_loadedModules)
        visitor.append(loadedModule.m_module);
#if USE(BUN_JSC_ADDITIONS)
    visitor.append(thisObject->m_prelinkedRecords.begin(), thisObject->m_prelinkedRecords.end());
    for (auto& load : thisObject->m_loadsInProgress) {
        visitor.append(load.importInFlight);
        visitor.append(load.asyncContext);
    }
#endif
}

DEFINE_VISIT_CHILDREN(JSModuleLoader);

// ------------------------------ Functions --------------------------------

static String printableModuleKey(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (key.isString() || key.isSymbol()) {
        auto propertyName = key.toPropertyKey(globalObject);
        scope.assertNoExceptionExceptTermination(); // This is OK since this function is just for debugging purpose.
        return propertyName.impl();
    }
    return vm.propertyNames->emptyIdentifier.impl();
}

JSArray* JSModuleLoader::dependencyKeysIfEvaluated(JSGlobalObject* globalObject, const String& key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Identifier ident = Identifier::fromString(vm, key);

    auto iter = m_moduleMap.find({ ident.impl(), ScriptFetchParameters::Type::JavaScript });

    if (iter == m_moduleMap.end())
        iter = m_moduleMap.find({ ident.impl(), ScriptFetchParameters::Type::WebAssembly });

    if (iter == m_moduleMap.end())
        RELEASE_AND_RETURN(scope, nullptr);

    ModuleRegistryEntry* entry = iter->value.get();
    AbstractModuleRecord* record = entry->record();

    if (record == nullptr)
        RELEASE_AND_RETURN(scope, nullptr);

    if (auto status = entry->status(); status != ModuleRegistryEntry::Status::Fetched && status != ModuleRegistryEntry::Status::EvaluationFailed)
        RELEASE_AND_RETURN(scope, nullptr);

    if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record); cyclic && cyclic->status() != CyclicModuleRecord::Status::Evaluated)
        RELEASE_AND_RETURN(scope, nullptr);

    const Vector<AbstractModuleRecord::ModuleRequest>& requests = record->requestedModules();

    JSArray* array = constructEmptyArray(globalObject, nullptr, requests.size());
    RETURN_IF_EXCEPTION(scope, nullptr);

    for (unsigned index = 0; const AbstractModuleRecord::ModuleRequest& request : requests) {
        Identifier resolved = resolve(globalObject, request.m_specifier, ident, nullptr, /* useImportMap */ true);
        RETURN_IF_EXCEPTION(scope, nullptr);
        array->putDirectIndex(globalObject, index++, identifierToJSValue(vm, resolved));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    RELEASE_AND_RETURN(scope, array);
}

void JSModuleLoader::provideFetch(JSGlobalObject* globalObject, const Identifier& key, ScriptFetchParameters::Type type, SourceCode&& sourceCode)
{
    ModuleRegistryEntry* entry = ensureRegistered(globalObject, key, type);
    if (entry->status() == ModuleRegistryEntry::Status::New)
        entry->provideFetch(globalObject, WTF::move(sourceCode)); // can throw
}

void JSModuleLoader::provideFetch(JSGlobalObject* globalObject, const Identifier& key, ScriptFetchParameters::Type type, JSValue moduleData)
{
    ModuleRegistryEntry* entry = ensureRegistered(globalObject, key, type);
    if (entry->status() == ModuleRegistryEntry::Status::New)
        entry->provideFetch(globalObject, moduleData); // can throw
}

#if USE(BUN_JSC_ADDITIONS)
static bool cannotWaitForFetch(VM& vm, ModuleRegistryEntry* entry)
{
    if (entry->status() != ModuleRegistryEntry::Status::Fetching)
        return false;
    if (uint32_t queue = vm.numberOfSynchronousModuleQueue())
        return queue != entry->queueThatMakesRecord();
    return vm.m_areFetchesOfHostOnHold && entry->fetchWaitsFor() == JSModuleLoader::FetchWaitsFor::Host;
}

// Settles the fetch promise of an entry whose asynchronous fetch is still in flight by running the embedder's fetch
// again. fetchPromise was already pipeFrom()'d by the asynchronous path, which set
// isFirstResolvingFunctionCalledFlag, so this uses the unguarded fulfill/reject. The ModuleRegistryFetchSettled
// reaction on fetchPromise drives the rest of the chain.
bool JSModuleLoader::fetchSynchronously(JSGlobalObject* globalObject, JSPromise* fetchPromise, const Identifier& key, const String& referrer, RefPtr<ScriptFetchParameters>&& parameters, RefPtr<ScriptFetcher>&& scriptFetcher, bool mayBePostponed)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(vm.m_synchronousModuleQueue || vm.m_areFetchesOfHostOnHold);
    ASSERT(fetchPromise->status() == JSPromise::Status::Pending);

    JSPromise* promise = nullptr;
#if ENABLE(AOT)
    // This repeats a fetch that the host already allowed, so a compiled module must resolve the same way it did the first time.
    if (vm.hasCompiledCode()) {
        BitVector savedHostRequests = AOT::ModuleRecord::willRefetchFromHost(globalObject);
        promise = fetch(globalObject, identifierToJSValue(vm, key), referrer, WTF::move(parameters), WTF::move(scriptFetcher), mayBePostponed);
        AOT::ModuleRecord::didRefetchFromHost(globalObject, savedHostRequests);
    } else
#endif
        promise = fetch(globalObject, identifierToJSValue(vm, key), referrer, WTF::move(parameters), WTF::move(scriptFetcher), mayBePostponed);
    RETURN_IF_EXCEPTION(scope, true);
    if (!promise)
        return false;
    if (promise->status() == JSPromise::Status::Fulfilled)
        fetchPromise->fulfillPromise(vm, promise->result());
    else if (promise->status() == JSPromise::Status::Rejected)
        fetchPromise->rejectPromise(vm, promise->result());
    return true;
}
#endif

JSPromise* JSModuleLoader::loadModule(JSGlobalObject* globalObject, const Identifier& specifier, RefPtr<ScriptFetchParameters> parameters, RefPtr<ScriptFetcher> scriptFetcher, OptionSet<ModuleLoadFlag> flags, int64_t referrerAsyncOrder, const String& referrer, AbstractModuleRecord* providedRecord)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSPromise* promise = nullptr;

#if USE(BUN_JSC_ADDITIONS)
    parameters = parametersOfEntry(specifier, WTF::move(parameters));
#endif
    ScriptFetchParameters::Type type = parameters ? parameters->type() : ScriptFetchParameters::Type::JavaScript;
#if USE(BUN_JSC_ADDITIONS)
    // Preserve the caller's ScriptFetchParameters (host-defined data) into the loading context
    // before `parameters` is moved into fetch() below.
    RefPtr<ScriptFetchParameters> contextParameters = parameters ? parameters : ScriptFetchParameters::create(type);
#endif

    ModuleRegistryEntry* entry = getRegisteredMayBeNull(specifier, type);
    if (entry) {
        if (entry->fetchError())
            removeFailedFetchEntry(std::exchange(entry, nullptr));
        else {
            JSValue error = entry->error(globalObject);
            RETURN_IF_EXCEPTION(scope, nullptr);
#if USE(BUN_JSC_ADDITIONS)
            bool isFirstAttempt = !flags.contains(ModuleLoadFlag::IsSecondAttempt);
            bool triesToRecover = error && isFirstAttempt && mayRecoverLoad(entry);
#if ENABLE(AOT)
            // A registry hit never reaches the host, so the AOT runtime checks access here.
            if (vm.hasCompiledCode() && isFirstAttempt && (error ? triesToRecover : entry->status() != ModuleRegistryEntry::Status::New) && !providedRecord && this == globalObject->moduleLoader() && (!entry->record() || dynamicDowncast<AOT::ModuleRecord>(entry->record()))) {
                AOT::ModuleRecord::checkAccessOnRegistryHit(globalObject, specifier, referrer);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }
#endif
            if (triesToRecover) {
                auto* context = ModuleLoadingContext::create(vm, this, { specifier, WTF::move(contextParameters) }, WTF::move(scriptFetcher), flags, referrerAsyncOrder);
                context->setEntry(vm, entry);
                context->setQueueOfLoad(vm.numberOfSynchronousModuleQueue());
                if (context->dynamic())
                    context->setAsyncContext(vm, asyncContextOfImport(globalObject));
                JSPromise* resultPromise = JSPromise::create(vm, globalObject->promiseStructure());
                resultPromise->markAsHandled();
                JSPromise* failedLoad = JSPromise::create(vm, globalObject->promiseStructure());
                failedLoad->rejectAsHandled(vm, error);
                failedLoad->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadTopRejected, resultPromise, context);
                return resultPromise;
            }
#endif
            if (error)
                return JSPromise::rejectedPromise(globalObject, error);

            if (entry->status() != ModuleRegistryEntry::Status::New) {
                promise = entry->ensureFetchPromise(globalObject);
#if USE(BUN_JSC_ADDITIONS)
                // require(esm) of a module whose fetch an import already started.
                if (cannotWaitForFetch(vm, entry) && promise->status() == JSPromise::Status::Pending) {
                    fetchSynchronously(globalObject, promise, specifier, referrer, parameters.copyRef(), scriptFetcher.copyRef());
                    RETURN_IF_EXCEPTION(scope, nullptr);
                }
#endif
            }
        }
    }

    if (!promise && providedRecord) {
        promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->fulfill(vm, providedRecord);
    }
    if (!promise) {
        promise = fetch(globalObject, identifierToJSValue(vm, specifier), referrer, WTF::move(parameters), scriptFetcher);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

#if USE(BUN_JSC_ADDITIONS)
    AbstractModuleRecord::ModuleRequest request { specifier, WTF::move(contextParameters) };
#else
    AbstractModuleRecord::ModuleRequest request { specifier, ScriptFetchParameters::create(type) };
#endif
    auto* context = ModuleLoadingContext::create(vm, this, request, WTF::move(scriptFetcher), flags, referrerAsyncOrder);
#if USE(BUN_JSC_ADDITIONS)
    context->setEntry(vm, entry);
    context->setQueueOfLoad(vm.numberOfSynchronousModuleQueue());
    if (context->dynamic())
        context->setAsyncContext(vm, asyncContextOfImport(globalObject));
#endif

    JSPromise* intermediatePromise = JSPromise::create(vm, globalObject->promiseStructure());
    intermediatePromise->markAsHandled();
    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadTopSettled, intermediatePromise, context);

    JSPromise* resultPromise = JSPromise::create(vm, globalObject->promiseStructure());
    resultPromise->markAsHandled();
    intermediatePromise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadTopRejected, resultPromise, context);

    return resultPromise;
}

JSPromise* JSModuleLoader::linkAndEvaluateModule(JSGlobalObject* globalObject, const Identifier& moduleKey, RefPtr<ScriptFetchParameters> parameters, RefPtr<ScriptFetcher> scriptFetcher)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ScriptFetchParameters::Type type = parameters ? parameters->type() : ScriptFetchParameters::Type::JavaScript;
    ModuleRegistryEntry* entry = ensureRegistered(globalObject, moduleKey, type);
    RETURN_IF_EXCEPTION(scope, nullptr);

    AbstractModuleRecord* record = entry->record();
    ASSERT(record);

    JSValue error = entry->error(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (error) {
        scope.throwException(globalObject, error);
        return nullptr;
    }

    record->link(globalObject, WTF::move(scriptFetcher));
    if (Exception* exception = scope.exception()) {
        attachErrorInfo(globalObject, scope, record, entry->key(), entry->moduleType(), ModuleFailure::Kind::Instantiation);
        entry->setInstantiationError(globalObject, exception->value());
        if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record))
            cyclic->setEvaluationError(vm, exception->value());
        return nullptr;
    }

    JSPromise* promise = record->evaluate(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);

    error = entry->error(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (error) {
        // At this point, the promise exists but won't be returned. However, it will be rejected later.
        // To prevent an unhandled promise rejection error, we have to explicitly mark the promise as handled.
        promise->markAsHandled();
        scope.throwException(globalObject, error);
        return nullptr;
    }

    return promise;
}

// The referrer of a module fetch is the referring script's base URL, i.e. its module key. An inline
// module's key is a Symbol: it has no URL of its own, so we return the empty string and let the host
// substitute its base URL. A null key means there is no referring script at all.
static String moduleReferrer(const Identifier& referrerKey)
{
    if (referrerKey.isSymbol())
        return emptyString();
    return referrerKey.string();
}

JSPromise* JSModuleLoader::requestImportModule(JSGlobalObject* globalObject, const Identifier& moduleName, const Identifier& referrer, RefPtr<ScriptFetchParameters> parameters, RefPtr<ScriptFetcher> scriptFetcher, bool deferred, int64_t referrerAsyncOrder)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Identifier resolved = resolve(globalObject, moduleName, referrer, scriptFetcher, /* useImportMap */ true);
    RETURN_IF_EXCEPTION(scope, nullptr);

    RELEASE_AND_RETURN(scope, requestImportModuleForKey(globalObject, resolved, referrer, WTF::move(parameters), WTF::move(scriptFetcher), deferred, referrerAsyncOrder));
}

JSPromise* JSModuleLoader::requestImportModuleForKey(JSGlobalObject* globalObject, const Identifier& resolvedKey, const Identifier& referrer, RefPtr<ScriptFetchParameters> parameters, RefPtr<ScriptFetcher> scriptFetcher, bool deferred, int64_t referrerAsyncOrder, AbstractModuleRecord* providedRecord, JSPromise* resultPromise)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    OptionSet<ModuleLoadFlag> flags { ModuleLoadFlag::Evaluate, ModuleLoadFlag::Dynamic };
    if (deferred)
        flags.add(ModuleLoadFlag::Deferred);
    // Per "fetch an import() module script graph", the referring script's base URL is the fetch's referrer.
    JSPromise* promise = loadModule(globalObject, resolvedKey, WTF::move(parameters), WTF::move(scriptFetcher), flags, referrerAsyncOrder, moduleReferrer(referrer), providedRecord);
    RETURN_IF_EXCEPTION(scope, nullptr);

    if (!resultPromise)
        resultPromise = JSPromise::create(vm, globalObject->promiseStructure());
    resultPromise->markAsHandled();
    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ImportModuleNamespace, resultPromise, jsUndefined());

    return resultPromise;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#module-type-allowed
static bool moduleTypeIsAllowed(JSGlobalObject* globalObject, ScriptFetchParameters::Type type)
{
    switch (type) {
    case ScriptFetchParameters::JavaScript:
    case ScriptFetchParameters::WebAssembly:
    case ScriptFetchParameters::JSON:
    case ScriptFetchParameters::Text:
    case ScriptFetchParameters::None:
        return true;

    default:
        if (globalObject->globalObjectMethodTable()->moduleTypeIsAllowed)
            return globalObject->globalObjectMethodTable()->moduleTypeIsAllowed(type);

#if USE(BUN_JSC_ADDITIONS)
        // A host-defined type is one the host said it handles (ScriptFetchParameters::parseType), so without a
        // moduleTypeIsAllowed hook the host's module loader decides, as it did before the hook existed.
        if (ScriptFetchParameters::isHostDefined(type))
            return true;
#endif
        return false;
    };
}

// The error is the promise import() returns for a module type the environment does not allow.
std::expected<RefPtr<ScriptFetchParameters>, JSPromise*> JSModuleLoader::fetchParametersFromImportOptions(JSGlobalObject* globalObject, JSValue options)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto attributes = retrieveImportAttributesFromDynamicImportOptions(globalObject, options, { vm.propertyNames->type.impl() });
    RETURN_IF_EXCEPTION(scope, makeUnexpected<JSPromise*>(nullptr));

    auto type = retrieveTypeImportAttribute(globalObject, attributes);
    RETURN_IF_EXCEPTION(scope, makeUnexpected<JSPromise*>(nullptr));

    if (type && !moduleTypeIsAllowed(globalObject, *type)) {
        auto* promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->reject(vm, createTypeError(globalObject, "Module type not supported by environment"_s));
        RELEASE_AND_RETURN(scope, makeUnexpected(promise));
    }

    RefPtr<ScriptFetchParameters> fetchParams;
#if USE(BUN_JSC_ADDITIONS)
    if (!attributes.isEmpty()) {
        // The attributes map is per-request state: never set it on a shared per-type instance.
        fetchParams = ScriptFetchParameters::createUnique(type.value_or(ScriptFetchParameters::Type::None));
        fetchParams->setAttributes(WTF::move(attributes));
    }
#else
    if (type)
        fetchParams = ScriptFetchParameters::create(type.value());
#endif
    return fetchParams;
}

JSPromise* JSModuleLoader::importModule(JSGlobalObject* globalObject, JSString* moduleName, JSValue parameters, const SourceOrigin& referrer, bool deferred)
{
    dataLogLnIf(Options::dumpModuleLoadingState(), "Loader [import] ", printableModuleKey(globalObject, moduleName));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto fetchParams = fetchParametersFromImportOptions(globalObject, parameters);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!fetchParams)
        return fetchParams.error();

    if (globalObject->globalObjectMethodTable()->moduleLoaderImportModule)
        RELEASE_AND_RETURN(scope, globalObject->globalObjectMethodTable()->moduleLoaderImportModule(globalObject, this, moduleName, WTF::move(*fetchParams), referrer, deferred));

    auto* promise = JSPromise::create(vm, globalObject->promiseStructure());
    auto moduleNameString = moduleName->value(globalObject);
    RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));

    scope.release();
    promise->reject(vm, createError(globalObject, makeString("Could not import the module '"_s, moduleNameString.data, "'."_s)));
    return promise;
}

Identifier JSModuleLoader::resolve(JSGlobalObject* globalObject, JSValue name, JSValue referrer, RefPtr<ScriptFetcher> scriptFetcher, bool useImportMap)
{
    dataLogLnIf(Options::dumpModuleLoadingState(), "Loader [resolve] ", printableModuleKey(globalObject, name));

    if (globalObject->globalObjectMethodTable()->moduleLoaderResolve)
        return globalObject->globalObjectMethodTable()->moduleLoaderResolve(globalObject, this, name, referrer, WTF::move(scriptFetcher), useImportMap);
    return name.toPropertyKey(globalObject);
}

Identifier JSModuleLoader::resolve(JSGlobalObject* globalObject, const Identifier& name, const Identifier& referrer, RefPtr<ScriptFetcher> scriptFetcher, bool useImportMap)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue nameValue = name.isSymbol() || name.isNull() ? jsUndefined() : jsString(vm, name.string());
    RETURN_IF_EXCEPTION(scope, { });

    JSValue referrerValue = referrer.isSymbol() || referrer.isNull() ? jsUndefined() : jsString(vm, referrer.string());
    RETURN_IF_EXCEPTION(scope, { });

    RELEASE_AND_RETURN(scope, resolve(globalObject, nameValue, referrerValue, WTF::move(scriptFetcher), useImportMap));
}

JSPromise* JSModuleLoader::fetch(JSGlobalObject* globalObject, JSValue key, const String& referrer, RefPtr<ScriptFetchParameters> parameters, RefPtr<ScriptFetcher> scriptFetcher, [[maybe_unused]] bool mayBePostponed)
{
    dataLogLnIf(Options::dumpModuleLoadingState(), "Loader [fetch] ", printableModuleKey(globalObject, key));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
#if USE(BUN_JSC_ADDITIONS)
    SetForScope mayPostponeFetch(m_mayPostponeFetch, mayBePostponed);
#endif

    // https://html.spec.whatwg.org/multipage/webappapis.html#fetching-scripts:module-type-allowed
    // Assert: the result of running the module type allowed steps given moduleType and settingsObject is true.
    // Otherwise, we would not have reached this point because a failure would have been raised when inspecting
    // moduleRequest.[[Attributes]] in HostLoadImportedModule or fetch a single imported module script.
    if (parameters)
        ASSERT(moduleTypeIsAllowed(globalObject, parameters->type()));

    if (globalObject->globalObjectMethodTable()->moduleLoaderFetch)
        RELEASE_AND_RETURN(scope, globalObject->globalObjectMethodTable()->moduleLoaderFetch(globalObject, this, key, referrer, WTF::move(parameters), WTF::move(scriptFetcher)));

    auto* promise = JSPromise::create(vm, globalObject->promiseStructure());
    String moduleKey = key.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));

    scope.release();
    promise->reject(vm, createError(globalObject, makeString("Could not open the module '"_s, moduleKey, "'."_s)));
    return promise;
}

JSObject* JSModuleLoader::createImportMetaProperties(JSGlobalObject* globalObject, JSValue key, JSModuleRecord* moduleRecord, RefPtr<ScriptFetcher> scriptFetcher)
{
    if (globalObject->globalObjectMethodTable()->moduleLoaderCreateImportMetaProperties)
        return globalObject->globalObjectMethodTable()->moduleLoaderCreateImportMetaProperties(globalObject, this, key, moduleRecord, WTF::move(scriptFetcher));
    return constructEmptyObject(globalObject->vm(), globalObject->nullPrototypeObjectStructure());
}

JSValue JSModuleLoader::evaluate(JSGlobalObject* globalObject, JSValue key, JSValue moduleRecordValue, RefPtr<ScriptFetcher> scriptFetcher, JSValue sentValue, JSValue resumeMode)
{
    dataLogLnIf(Options::dumpModuleLoadingState(), "Loader [evaluate] ", printableModuleKey(globalObject, key));

    if (globalObject->globalObjectMethodTable()->moduleLoaderEvaluate)
        return globalObject->globalObjectMethodTable()->moduleLoaderEvaluate(globalObject, this, key, moduleRecordValue, WTF::move(scriptFetcher), sentValue, resumeMode);

    return evaluateNonVirtual(globalObject, key, moduleRecordValue, nullptr, sentValue, resumeMode);
}

JSValue JSModuleLoader::evaluateNonVirtual(JSGlobalObject* globalObject, JSValue, JSValue moduleRecordValue, RefPtr<ScriptFetcher>, JSValue sentValue, JSValue resumeMode)
{
    if (auto* moduleRecord = dynamicDowncast<AbstractModuleRecord>(moduleRecordValue))
        return moduleRecord->evaluate(globalObject, sentValue, resumeMode);
    return jsUndefined();
}

JSModuleNamespaceObject* JSModuleLoader::getModuleNamespaceObject(JSGlobalObject* globalObject, JSValue moduleRecordValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* moduleRecord = dynamicDowncast<AbstractModuleRecord>(moduleRecordValue);
    if (!moduleRecord) {
        throwTypeError(globalObject, scope);
        return nullptr;
    }

    RELEASE_AND_RETURN(scope, moduleRecord->getModuleNamespace(globalObject));
}

AbstractModuleRecord* JSModuleLoader::getImportedModule(AbstractModuleRecord* referrer, const AbstractModuleRecord::ModuleRequest& request)
{
    // GetImportedModule(referrer, request)
    // https://tc39.es/ecma262/#sec-GetImportedModule

#if USE(BUN_JSC_ADDITIONS)
    if (AbstractModuleRecord* loaded = referrer->prelinkedRequestedModule(request))
        return loaded;
#endif
    // 1. Let records be a List consisting of each LoadedModuleRequest Record r of referrer.[[LoadedModules]] such that ModuleRequestsEqual(r, request) is true.
    auto iter = referrer->loadedModules().find(ModuleMapKey { request.m_specifier.impl(), request.type() });
    // 2. Assert: records has exactly one element, since LoadRequestedModules has completed successfully on referrer prior to invoking this abstract operation.
    ASSERT(iter != referrer->loadedModules().end());
    // 3. Let record be the sole element of records.
    // 4. Return record.[[Module]].
    return iter->value.m_module.get();
}

JSPromise* JSModuleLoader::hostLoadImportedModule(JSGlobalObject* globalObject, const ModuleReferrer& referrer, const ModuleRequest& moduleRequest, JSCell* payload, RefPtr<ScriptFetcher> scriptFetcher, bool useImportMap, [[maybe_unused]] OptionSet<RequestFlag> flags)
{
    // HostLoadImportedModule(referrer, moduleRequest, loadState, payload)
    // https://html.spec.whatwg.org/multipage/webappapis.html#hostloadimportedmodule

    ASSERT(isModuleLoaderHostDefinedPayload(payload));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Identifier referrerKey;
    CyclicModuleRecord* record = referrer.getModule();

    if (record)
        referrerKey = record->moduleKey();

#if ENABLE(AOT)
    // Check the record's type, not its key: uncompiled code can be loaded under any key.
    bool hasUncompiledReferrer = vm.hasCompiledCode() && record && this == globalObject->moduleLoader() && !dynamicDowncast<AOT::ModuleRecord>(record);
    uint8_t savedHostRequestFlags = hasUncompiledReferrer ? AOT::ModuleRecord::willLoadForUncompiledReferrer(globalObject) : 0;
    auto restoreHostRequestFlags = makeScopeExit([&] {
        if (hasUncompiledReferrer)
            AOT::ModuleRecord::didLoadForUncompiledReferrer(globalObject, savedHostRequestFlags);
    });
#endif

    ModuleRegistryEntry* mapEntry = nullptr;
    const Identifier& specifier = moduleRequest.m_specifier;
    auto type = moduleRequest.type();

    // Step 14 calls for calling "fetch a single imported module script"
    // https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-imported-module-script
    // 3. If the result of running the module type allowed steps given moduleType and settingsObject is false,
    //    then run onComplete given null, and return.
    if (!moduleTypeIsAllowed(globalObject, type)) {
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());

        auto error = createTypeError(globalObject, "Module type not supported by environment"_s);
        promise->reject(vm, error);

        scope.release();
        auto exception = Exception::create(vm, error);
        finishLoadingImportedModule(globalObject, referrer, moduleRequest, payload, exception, scriptFetcher);
        return promise;
    }

    ModuleMapKey moduleMapKey { specifier.impl(), type };

#if USE(BUN_JSC_ADDITIONS)
    // See loadModuleSync().
    bool isReloadingRoot = m_rootBeingReloaded && m_rootBeingReloaded == specifier.impl() && referrer.isRealm() && payload->inherits<ModuleGraphLoadingState>();
    if (isReloadingRoot)
        m_rootBeingReloaded = nullptr;
#endif

    // HostLoadImportedModule is required to be idempotent for the same
    // (referrer, moduleRequest) pair. referrer.[[LoadedModules]] is that cache;
    // FinishLoadingImportedModule populates it, and innerModuleLoading consults it,
    // but top-level loadModule (dynamic import) reaches here without checking.
    // Consult it now so we can skip the host resolve() hook for repeat imports.
    {
        auto& loadedModules = record ? record->loadedModules() : m_loadedModules;
#if USE(BUN_JSC_ADDITIONS)
        if (auto iter = loadedModules.find(moduleMapKey); iter != loadedModules.end() && !isReloadingRoot) {
#else
        if (auto iter = loadedModules.find(moduleMapKey); iter != loadedModules.end()) {
#endif
            AbstractModuleRecord* loaded = iter->value.m_module.get();
            ModuleRegistryEntry* loadedEntry = getRegisteredMayBeNull(loaded->moduleKey(), type);
            ASSERT(loadedEntry);
#if USE(BUN_JSC_ADDITIONS)
            ASSERT(loadedEntry->record());
#else
            ASSERT(loadedEntry->record() == loaded);
#endif
#if USE(BUN_JSC_ADDITIONS)
            JSPromise* loadedPromise = loadedEntry->loadedPromise(globalObject);
#else
            JSPromise* loadedPromise = loadedEntry->loadPromise();
#endif
            ASSERT(loadedPromise);
            finishLoadingImportedModule(globalObject, referrer, moduleRequest, payload, loaded, scriptFetcher);
            RETURN_IF_EXCEPTION(scope, nullptr);
            return loadedPromise;
        }
    }

    if (specifier.isSymbol())
        mapEntry = getRegisteredMayBeNull(specifier, type);

    ResolutionMapKey resolutionKey { referrerKey.impl(), specifier.impl() };

    if (auto iter = m_resolutionFailures.find(resolutionKey); iter != m_resolutionFailures.end()) {
        JSValue errorValue = iter->value.error.get();
        ASSERT(errorValue);
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->reject(vm, errorValue);
        // 9.1. If loadState is not undefined and loadState.[[ErrorToRethrow]] is null, set loadState.[[ErrorToRethrow]] to resolutionError.
        // (Unused.)
        // 9.2. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, ThrowCompletion(resolutionError)).
        finishLoadingImportedModule(globalObject, referrer, moduleRequest, payload, Exception::create(vm, errorValue), scriptFetcher);
        // 9.3. Return.
        RELEASE_AND_RETURN(scope, promise);
    }

    Identifier resolved;
#if USE(BUN_JSC_ADDITIONS)
    using ResolvedRequest = AbstractModuleRecord::ResolvedRequest;
    auto* resolvedRequests = record && payload->inherits<ModuleGraphLoadingState>() ? &record->resolvedRequests() : nullptr;
    uint32_t numberOfRequest = 0;
    bool mayBePostponed = resolvedRequests && vm.synchronousModuleQueue() && !flags.contains(RequestFlag::CannotBePostponed);
    auto didPostpone = [&] {
#if ENABLE(AOT)
        if (AOT::compiledModule(vm, record))
            return record->removeResolvedRequest(moduleRequest);
#endif
        record->setStateOfResolvedRequest(moduleRequest, ResolvedRequest::State::Postponed);
    };
#else
    constexpr bool mayBePostponed = false;
#endif

    if (!mapEntry) {
#if USE(BUN_JSC_ADDITIONS)
        // Each module gets its own ModuleGraphLoadingState (see moduleLoadStep), and every state that reaches a record that's still
        // New would ask the host to resolve the same requests again. Cache the resolved key on the referrer until the load finishes.
        // After that it's in [[LoadedModules]], or the load failed and we ask the host again.
        ResolvedRequest known = resolvedRequests ? resolvedRequests->get(moduleMapKey) : ResolvedRequest { };
        if (known.state == ResolvedRequest::State::Postponed) {
            if (mayBePostponed)
                return nullptr;
            known.state = ResolvedRequest::State::Loading;
            record->setStateOfResolvedRequest(moduleRequest, known.state);
        }
        if (known.key && known.state == ResolvedRequest::State::Loading) {
            resolved = Identifier::fromUid(vm, known.key.get());
            numberOfRequest = known.number;
        } else {
        if (known.state == ResolvedRequest::State::Obsolete)
            resolvedRequests->remove(moduleMapKey);
#endif
        // 8. Let url be the result of resolving a module specifier given referencingScript and moduleRequest.[[Specifier]], catching any exceptions. If they throw an exception, let resolutionError be the thrown exception.
        resolved = resolve(globalObject, specifier, referrerKey, scriptFetcher, useImportMap);
#if ENABLE(AOT)
        if (hasUncompiledReferrer && !scope.exception())
            AOT::ModuleRecord::checkAccessFromUncompiledReferrer(globalObject, resolved, referrerKey);
#endif
        // 9. If the previous step threw an exception, then:
        if (Exception* resolutionError = scope.exception()) {
            // A TerminationException is not a resolution failure: it can be neither cleared
            // (rejectWithCaughtException leaves it pending) nor cached, so let it propagate
            // instead of continuing into FinishLoadingImportedModule with it still set.
            if (vm.isTerminationException(resolutionError)) [[unlikely]]
                RELEASE_AND_RETURN(scope, nullptr);
            attachErrorInfo(globalObject, resolutionError, nullptr, specifier, moduleRequest.type(), ModuleFailure::Kind::Instantiation);
            // Cache the resolution error so subsequent calls for the same specifier return the same error object.
            JSValue errorValue = resolutionError->value();
            addResolutionFailure(vm, resolutionKey, errorValue);

            JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
            promise->rejectWithCaughtException(vm, scope);
            // 9.1. If loadState is not undefined and loadState.[[ErrorToRethrow]] is null, set loadState.[[ErrorToRethrow]] to resolutionError.
            // (Unused.)
            // 9.2. Perform FinishLoadingImportedModule(referrer, moduleRequest, payload, ThrowCompletion(resolutionError)).
            finishLoadingImportedModule(globalObject, referrer, moduleRequest, payload, Exception::create(vm, errorValue), scriptFetcher);
            // 9.3. Return.
            RELEASE_AND_RETURN(scope, promise);
        }
#if USE(BUN_JSC_ADDITIONS)
        if (resolvedRequests) {
            numberOfRequest = ++vm.importOrderState().numberOfResolvedRequests;
            resolvedRequests->set(moduleMapKey, ResolvedRequest { resolved.impl(), ResolvedRequest::State::Loading, false, numberOfRequest });
        }
        }
#endif

        moduleMapKey = keyInModuleMap(resolved.impl(), type);

        if (auto iter = m_moduleMap.find(moduleMapKey); iter != m_moduleMap.end())
            mapEntry = iter->value.get();
    } else
        resolved = specifier;

#if ENABLE(AOT)
    if (auto* compiled = mayBePostponed ? AOT::compiledModule(vm, record) : nullptr)
        mayBePostponed = !compiled->isKeyOfCompiledModule(resolved);
#endif
#if USE(BUN_JSC_ADDITIONS)
    if (mapEntry)
        mapEntry->setIsAbandoned(false);
    if (flags.contains(RequestFlag::ForgetsObsoleteFailure) && mapEntry && !mapEntry->record() && mapEntry->status() >= ModuleRegistryEntry::Status::FetchFailed && mapEntry->isFailureObsolete(vm)) {
        Locker locker { cellLock() };
        m_moduleMap.remove(moduleMapKey);
        didRemoveModuleMapEntry(moduleMapKey.second);
        mapEntry = nullptr;
    }
#endif

    if (mapEntry) {
#if USE(BUN_JSC_ADDITIONS)
        ASSERT(mapEntry->status() != ModuleRegistryEntry::Status::New || (!mapEntry->record() && !mapEntry->loadPromise()));
#else
        ASSERT(mapEntry->status() != ModuleRegistryEntry::Status::New);
#endif

        // Only fetching errors should be checked here, not instantiation or evaluation errors.
        // This allows fetch errors to propagate first because they're required to have priority.
        // To avoid a race condition, instantiation errors need to be checked later, not here.
        if (JSValue fetchError = mapEntry->fetchError()) {
            if (auto* errorInstance = dynamicDowncast<ErrorInstance>(fetchError)) {
                fetchError = JSModuleLoader::duplicateError(globalObject, errorInstance);
                RETURN_IF_EXCEPTION(scope, nullptr);
            }

            finishLoadingImportedModule(globalObject, referrer, moduleRequest, payload, Exception::create(vm, fetchError), scriptFetcher);
            RETURN_IF_EXCEPTION(scope, nullptr);
            return JSPromise::rejectedPromise(globalObject, fetchError);
        }

#if USE(BUN_JSC_ADDITIONS)
        if (cannotWaitForFetch(vm, mapEntry)) {
            // require(esm) hit a dependency that the surrounding async graph
            // already started loading. Force the fetch+makeModule chain through
            // synchronously so this graph can complete without yielding; the
            // outer async path will see the entry as Fetched when it eventually
            // drains and short-circuit.
            JSPromise* fetchPromise = mapEntry->ensureFetchPromise(globalObject);
            JSPromise* modulePromise = mapEntry->ensureModulePromise(globalObject);
            if (modulePromise->status() == JSPromise::Status::Pending) {
                if (fetchPromise->status() == JSPromise::Status::Pending) {
#if ENABLE(AOT)
                    // A compiled module already has the record for each of its requests, so there is nothing to fetch.
                    auto* compiled = record && !moduleRequest.m_attributes ? AOT::compiledModule(vm, record) : nullptr;
                    AbstractModuleRecord* requested = compiled ? compiled->findRequestedRecord(resolved) : nullptr;
                    if (Exception* exception = scope.exception()) [[unlikely]] {
                        TRY_CLEAR_EXCEPTION(scope, nullptr);
                        fetchPromise->rejectPromise(vm, exception->value());
                    } else if (requested)
                        fetchPromise->fulfillPromise(vm, requested);
                    else
#endif
                    if (!fetchSynchronously(globalObject, fetchPromise, resolved, moduleReferrer(referrerKey), parametersOfEntry(resolved, RefPtr { moduleRequest.m_attributes }), scriptFetcher.copyRef(), mayBePostponed)) {
                        scope.assertNoException();
                        didPostpone();
                        return nullptr;
                    }
                    RETURN_IF_EXCEPTION(scope, nullptr);
                } else if (fetchPromise->status() == JSPromise::Status::Fulfilled) {
                    // fetchPromise already settled but its
                    // ModuleRegistryFetchSettled reaction is sitting on the
                    // *normal* microtask queue (it fired before we entered sync
                    // mode). Replay that step inline. The queued
                    // normal-microtask copy will see modulePromise already
                    // settled and bail in moduleRegistryFetchSettled's handler.
                    JSPromise* makePromise = makeModule(globalObject, resolved, fetchPromise->result());
                    RETURN_IF_EXCEPTION(scope, nullptr);
                    // Making a record can run script, and a load that it starts may have settled the entry.
                    if (modulePromise->status() == JSPromise::Status::Pending) {
                        if (makePromise->status() == JSPromise::Status::Fulfilled) {
                            mapEntry->fetchComplete(globalObject, uncheckedDowncast<AbstractModuleRecord>(makePromise->result()));
                            modulePromise->fulfillPromise(vm, makePromise->result());
                        } else if (makePromise->status() == JSPromise::Status::Rejected)
                            modulePromise->rejectPromise(vm, makePromise->result());
                    }
                }
                // The reactions above were diverted to the sync queue but
                // haven't *run* yet — they'll run when the caller's
                // drainSynchronousModuleQueue loop reaches them. The
                // loadPromise()/record() reads below therefore still see the
                // pre-advance state, so fall through to the modulePromise path
                // instead of returning the (still-Pending) cached loadPromise.
            }
        }
#endif

#if USE(BUN_JSC_ADDITIONS)
        JSPromise* promise = isReloadingRoot ? nullptr : mapEntry->loadedPromise(globalObject);
#else
        JSPromise* promise = mapEntry->loadPromise();
#endif
        if (promise) {
            if (mapEntry->record()) {
#if ENABLE(AOT)
                // A module that imports the entry point while the entry point's graph is loading is notified one loader step before the caller that loaded the program, so it gets its own load promise.
                if (auto* compiled = promise->status() == JSPromise::Status::Pending ? dynamicDowncast<AOT::ModuleRecord>(mapEntry->record()) : nullptr; compiled && compiled->takeMainPromiseSplit()) {
                    promise = JSPromise::create(vm, globalObject->promiseStructure());
                    promise->markAsHandled();
                    mapEntry->setLoadPromise(vm, promise);
                }
#endif
                finishLoadingImportedModule(globalObject, referrer, moduleRequest, payload, mapEntry->record(), scriptFetcher);
                RETURN_IF_EXCEPTION(scope, nullptr);
            } else {
                auto* context = ModuleLoadingContext::create(vm, this, ModuleLoadingContext::Step::Cached, referrer, moduleRequest, payload, mapEntry, scriptFetcher);
#if USE(BUN_JSC_ADDITIONS)
                context->setNumberOfRequest(numberOfRequest);
#endif
                JSPromise* resultPromise = JSPromise::create(vm, globalObject->promiseStructure());
                resultPromise->markAsHandled();
                promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadStep, resultPromise, context);
                promise = resultPromise;
            }

            return promise;
        }
    } else {
        mapEntry = ModuleRegistryEntry::create(vm, this, resolved, moduleMapKey.second, scriptFetcher);
        Locker locker { cellLock() };
        m_moduleMap.add(moduleMapKey, WriteBarrier<ModuleRegistryEntry>(vm, this, mapEntry));
        didAddModuleMapEntry(moduleMapKey.second);
    }

    if (mapEntry->status() == ModuleRegistryEntry::Status::New) {
        JSPromise* promise = nullptr;
#if ENABLE(AOT)
        // A compiled module already knows the record of each compiled module it requests. This promise replaces the one the host would return.
        if (auto* compiled = record && !moduleRequest.m_attributes ? AOT::compiledModule(vm, record) : nullptr) {
            AbstractModuleRecord* requested = compiled->nextRequestedRecord(globalObject, resolved);
            if (scope.exception()) [[unlikely]] {
                promise = JSPromise::create(vm, globalObject->promiseStructure());
                promise->markAsHandled();
                promise->rejectWithCaughtException(vm, scope);
            } else if (requested) {
                promise = JSPromise::create(vm, globalObject->promiseStructure());
                if (!compiled->tryDeferFetchFulfillment(promise, requested))
                    promise->fulfill(vm, requested);
            }
        }
#endif
#if USE(BUN_JSC_ADDITIONS)
        if (flags.contains(RequestFlag::CannotBePostponed))
            mapEntry->setWasFetchPostponed();
        RefPtr parameters = parametersOfEntry(resolved, RefPtr { moduleRequest.m_attributes });
#else
        RefPtr parameters = moduleRequest.m_attributes;
#endif
        // Per "fetch the descendants of a module script", the referrer is the referring module's base URL.
        if (!promise)
            promise = fetch(globalObject, identifierToJSValue(vm, resolved), moduleReferrer(referrerKey), WTF::move(parameters), scriptFetcher, mayBePostponed);
        RETURN_IF_EXCEPTION(scope, nullptr);
#if USE(BUN_JSC_ADDITIONS)
        if (!promise) {
            didPostpone();
            Locker locker { cellLock() };
            m_moduleMap.remove(moduleMapKey);
            didRemoveModuleMapEntry(moduleMapKey.second);
            return nullptr;
        }
#endif

        mapEntry->setStatus(ModuleRegistryEntry::Status::Fetching);
#if USE(BUN_JSC_ADDITIONS)
        // Otherwise the next importer finds the fetch pending and makes it again.
        if (uint32_t queue = vm.numberOfSynchronousModuleQueue(); queue && promise->status() == JSPromise::Status::Fulfilled && promise->result().inherits<JSSourceCode>()) {
            mapEntry->setQueueThatMakesRecord(queue);
            mapEntry->ensureFetchPromise(globalObject)->fulfill(vm, promise->result());
        } else
#endif
            mapEntry->ensureFetchPromise(globalObject)->pipeFrom(vm, promise);
    }
    JSPromise* modulePromise = mapEntry->ensureModulePromise(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);

    auto* context = ModuleLoadingContext::create(vm, this, ModuleLoadingContext::Step::Main, referrer, moduleRequest, payload, mapEntry, scriptFetcher);
#if USE(BUN_JSC_ADDITIONS)
    context->setNumberOfRequest(numberOfRequest);
#endif
    JSPromise* loadPromise = JSPromise::create(vm, globalObject->promiseStructure());
    loadPromise->markAsHandled();

    modulePromise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadStep, loadPromise, context);

    mapEntry->setLoadPromise(vm, loadPromise);

#if ENABLE(AOT)
    if (vm.hasCompiledCode())
        AOT::ModuleRecord::didRequestModule(globalObject);
#endif

    return loadPromise;
}

JSPromise* JSModuleLoader::loadModule(JSGlobalObject* globalObject, const ModuleReferrer& referrer, const ModuleRequest& moduleRequest, JSCell* payload, RefPtr<ScriptFetcher> scriptFetcher, OptionSet<ModuleLoadFlag> flags, [[maybe_unused]] uint32_t queueOfLoad)
{
    ASSERT(isModuleLoaderHostDefinedPayload(payload));
    ASSERT(!flags.contains(ModuleLoadFlag::Dynamic));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSPromise* promise = hostLoadImportedModule(globalObject, referrer, moduleRequest, payload, scriptFetcher, flags.contains(ModuleLoadFlag::UseImportMap));
    RETURN_IF_EXCEPTION(scope, nullptr);

    auto* context = ModuleLoadingContext::create(vm, this, moduleRequest, WTF::move(scriptFetcher), flags);
#if USE(BUN_JSC_ADDITIONS)
    context->setEntry(vm, getRegisteredMayBeNull(moduleRequest.m_specifier, moduleRequest.type()));
    context->setQueueOfLoad(queueOfLoad);
    if (ModuleRegistryEntry* entry = flags.contains(ModuleLoadFlag::IsSynchronous) ? nullptr : context->entry()) {
        auto* dynamicPayload = dynamicDowncast<ModuleLoaderPayload>(payload);
        if (flags.contains(ModuleLoadFlag::Evaluate) || dynamicPayload)
            entry->setIsRootOfLoadThatLinks();
        didStartLoad(globalObject, entry, dynamicPayload ? dynamicPayload->asyncContext() : JSValue());
    }
#endif
    JSPromise* resultPromise = JSPromise::create(vm, globalObject->promiseStructure());
    resultPromise->markAsHandled();

    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadLinkEvaluateSettled, resultPromise, context);
    resultPromise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadStoreError, nullptr, context);

    return resultPromise;
}

static void didLoadRequestedModules([[maybe_unused]] JSGlobalObject* globalObject, CyclicModuleRecord* module)
{
    if (module->status() != CyclicModuleRecord::Status::New)
        return;
#if USE(BUN_JSC_ADDITIONS)
    // It was postponed, or a walk has requested it again.
    if (module->hasRequestBeingLoaded()) [[unlikely]]
        return;
#endif
    module->setStatus(CyclicModuleRecord::Status::Unlinked);
#if USE(BUN_JSC_ADDITIONS)
#if ENABLE(AOT)
    if (AOT::compiledModule(globalObject->vm(), module))
        return;
#endif
    if (ModuleRegistryEntry* entry = module->moduleLoader()->entryOf(module); entry && entry->status() >= ModuleRegistryEntry::Status::FetchFailed && !entry->isFailureFinal() && !module->hasStartedLinking()) [[unlikely]]
        entry->forgetFailureToLoad(globalObject);
#endif
}

void JSModuleLoader::innerModuleLoading(JSGlobalObject* globalObject, ModuleGraphLoadingState *state, AbstractModuleRecord* startModule)
{
    // InnerModuleLoading(state, module)
    // https://tc39.es/ecma262/#sec-InnerModuleLoading
    //
    // The spec phrases this recursively, with HostLoadImportedModule re-entering
    // via FinishLoadingImportedModule -> ContinueModuleLoading. That re-entry
    // happens once per import edge, but only one-per-module actually enters the
    // step-2 loop body — the rest hit the visited check, decrement the counter
    // and return. With dense `export * from` graphs (E >> V) the per-edge
    // function entry / throw-scope / dynamicDowncast dominates. We instead drain
    // a worklist on the state: ContinueModuleLoading enqueues, and the outermost
    // call drains. Same observable [[Visited]] / [[PendingModulesCount]].

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    state->enqueueInnerLoad(startModule);
    if (state->drainingInnerLoad())
        RELEASE_AND_RETURN(scope, void());
    state->setDrainingInnerLoad(true);

    AbstractModuleRecord* module;
    while ((module = state->takeInnerLoad())) {
        // 1. Assert: state.[[IsLoading]] is true.
        ASSERT(state->isLoading());
        // 2. If module is a Cyclic Module Record, module.[[Status]] is NEW, and state.[[Visited]] does not contain module, then
        // (containsVisited first — it's the cheap pointer-hash check that fails
        //  fast for the common already-seen case before we pay for the dyncast.)
        if (!state->containsVisited(module)) {
            auto* cyclic = dynamicDowncast<CyclicModuleRecord>(module);
#if ENABLE(AOT)
            if (state->hasRootState() && cyclic && cyclic->status() == CyclicModuleRecord::Status::New && !state->markVisitedInRootState(vm, cyclic)) {
                if (auto* compiled = dynamicDowncast<AOT::ModuleRecord>(cyclic)) {
                    compiled->countLoaderStep();
                    cyclic = nullptr;
                }
            }
#endif
            if (cyclic && cyclic->status() == CyclicModuleRecord::Status::New) {
                // 2.a. Append module to state.[[Visited]].
                state->appendVisited(vm, cyclic);
#if ENABLE(AOT)
                if (auto* compiled = AOT::compiledModule(vm, cyclic)) {
                    compiled->willLoadRequests(*state);
                    RETURN_IF_EXCEPTION(scope, void());
                }
#endif
                // 2.b. Let requestedModulesCount be the number of elements in module.[[RequestedModules]].
                size_t requestedModulesCount = module->requestedModules().size();
                // 2.c. Set state.[[PendingModulesCount]] to state.[[PendingModulesCount]] + requestedModulesCount.
                state->setPendingModulesCount(state->pendingModulesCount() + requestedModulesCount);
                // 2.d. For each ModuleRequest Record request of module.[[RequestedModules]], do
                for (const AbstractModuleRecord::ModuleRequest& request : module->requestedModules()) {
                    // 2.d.i. If AllImportAttributesSupported(request.[[Attributes]]) is false, then
                    // 2.d.i.1. Let error be ThrowCompletion(a newly created SyntaxError object).
                    // 2.d.i.2. Perform ContinueModuleLoading(state, error).
                    // (Not possible.)
                    // 2.d.ii. Else if module.[[LoadedModules]] contains a LoadedModuleRequest Record record such that ModuleRequestsEqual(record, request) is true, then
                    AbstractModuleRecord* loaded = nullptr;
#if USE(BUN_JSC_ADDITIONS)
                    loaded = module->prelinkedRequestedModule(request);
#endif
                    if (!loaded) {
                        if (auto iter = module->loadedModules().find(ModuleMapKey { request.m_specifier.impl(), request.type() }); iter != module->loadedModules().end())
                            loaded = iter->value.m_module.get();
                    }
                    if (loaded) {
                        // 2.d.ii.1. Perform InnerModuleLoading(state, record.[[Module]]).
                        if (state->containsVisited(loaded))
                            state->setPendingModulesCount(state->pendingModulesCount() - 1);
                        else
                            state->enqueueInnerLoad(loaded);
                        if (!state->isLoading())
                            break;
                        continue;
                    }
                    // 2.d.iii. Else,
                    // 2.d.iii.1. Perform HostLoadImportedModule(module, request, state.[[HostDefined]], state).
#if !USE(BUN_JSC_ADDITIONS)
                    unsigned loadedModulesCountBefore = module->loadedModules().size();
#endif
                    bool useImportMap = true;
#if ENABLE(AOT)
                    auto* compiled = AOT::compiledModule(vm, cyclic);
                    useImportMap = !compiled;
                    uint8_t flagsBefore = compiled ? compiled->willLoadImportedModule(request.m_specifier) : 0;
#endif
                    JSPromise* promise = hostLoadImportedModule(globalObject, cyclic, request, state, state->scriptFetcher(), useImportMap);
#if ENABLE(AOT)
                    if (compiled)
                        compiled->didLoadImportedModule(flagsBefore);
#endif
                    if (scope.exception()) [[unlikely]] {
#if USE(BUN_JSC_ADDITIONS)
                        cyclic->didFailForRequest(request);
#endif
                        state->setDrainingInnerLoad(false);
                        return;
                    }
#if USE(BUN_JSC_ADDITIONS)
                    if (!promise) {
                        if (auto* queue = vm.synchronousModuleQueue(); queue->keepsPostponedRequests)
                            queue->postponedRequests.append({ state, cyclic, static_cast<unsigned>(&request - module->requestedModules().span().data()) });
                        else
                            state->setPendingModulesCount(state->pendingModulesCount() - 1);
                        continue;
                    }
#endif
                    // 2.d.iii.2. NOTE: HostLoadImportedModule will call FinishLoadingImportedModule, which re-enters the graph loading process through ContinueModuleLoading.
                    //
                    // If module.[[LoadedModules]] grew across the HostLoadImportedModule call, the requested
                    // module was loaded synchronously, which means it was already loaded before. In that case
                    // there is no need to attach a ModuleGraphLoadingError reaction, so we skip it.
#if USE(BUN_JSC_ADDITIONS)
                    // The host can run script, which may load or drop other requests of this module.
                    bool needsErrorReaction = !module->loadedModules().contains(ModuleMapKey { request.m_specifier.impl(), request.type() });
#else
                    bool needsErrorReaction = module->loadedModules().size() == loadedModulesCountBefore;
                    ASSERT(module->loadedModules().size() <= loadedModulesCountBefore + 1);
                    ASSERT(needsErrorReaction != module->loadedModules().contains(ModuleMapKey { request.m_specifier.impl(), request.type() }));
#endif
                    if (needsErrorReaction)
                        promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleGraphLoadingError, nullptr, state);
                    // 2.d.iv. If state.[[IsLoading]] is false, return UNUSED.
                    if (!state->isLoading())
                        break;
                }
                if (!state->isLoading()) {
                    state->setDrainingInnerLoad(false);
                    RELEASE_AND_RETURN(scope, void());
                }
            }
        }
        // 3. Assert: state.[[PendingModulesCount]] ≥ 1.
        ASSERT(state->pendingModulesCount() >= 1);
        // 4. Set state.[[PendingModulesCount]] to state.[[PendingModulesCount]] - 1.
        state->setPendingModulesCount(state->pendingModulesCount() - 1);
        // 5. If state.[[PendingModulesCount]] = 0, then
        if (!state->pendingModulesCount()) {
            // 5.a. Set state.[[IsLoading]] to false.
            state->setIsLoading(false);
            // 5.b. For each Cyclic Module Record loaded of state.[[Visited]], do
#if ENABLE(AOT)
            // Modules that share a root state stay New until the whole load ends, so other loads keep visiting them.
            if (!state->hasRootState())
#endif
            state->iterateVisited([&](CyclicModuleRecord* loaded) {
                // 5.b.i. If loaded.[[Status]] is NEW, set loaded.[[Status]] to UNLINKED.
                didLoadRequestedModules(globalObject, loaded);
            });
            // 5.c. Perform ! Call(state.[[PromiseCapability]].[[Resolve]], undefined, « undefined »).
            state->promise()->fulfill(vm, module);
            break;
        }
    }
    // 6. Return UNUSED.
    state->setDrainingInnerLoad(false);
    scope.release();
}

void JSModuleLoader::finishLoadingImportedModule(JSGlobalObject* globalObject, const ModuleReferrer& referrer, const ModuleRequest& moduleRequest, JSCell* payload, ModuleCompletion result, RefPtr<ScriptFetcher> scriptFetcher)
{
    // FinishLoadingImportedModule(referrer, moduleRequest, payload, result)
    // https://tc39.es/ecma262/#sec-FinishLoadingImportedModule

    ASSERT(isModuleLoaderHostDefinedPayload(payload));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

#if USE(BUN_JSC_ADDITIONS)
    bool wasReached = false;
    if (CyclicModuleRecord* module = referrer.getModule()) {
        if (std::holds_alternative<AbstractModuleRecord*>(result))
            wasReached = module->resolvedRequests().take(ModuleMapKey { moduleRequest.m_specifier.impl(), moduleRequest.type() }).wasLeftForLater;
        else
            module->setStateOfResolvedRequest(moduleRequest, AbstractModuleRecord::ResolvedRequest::State::Failed);
    }
#endif

    // 1. If result is a normal completion, then
#if USE(BUN_JSC_ADDITIONS)
    // hostLoadImportedModule() relies on the realm's [[LoadedModules]] holding only what the registry holds. removeEntry()
    // and clearAll() take a module out of both, and a load of it that was in flight then must not put it back into one.
    auto isRegistered = [&](AbstractModuleRecord* record) {
        ModuleRegistryEntry* entry = getRegisteredMayBeNull(record->moduleKey(), moduleRequest.type());
        return entry && entry->record() == record;
    };
    if (auto* resultRecord = std::get_if<AbstractModuleRecord*>(&result); resultRecord && (!referrer.isRealm() || isRegistered(*resultRecord))) {
#else
    if (auto* resultRecord = std::get_if<AbstractModuleRecord*>(&result)) {
#endif
        JSCell* owner = nullptr;

        auto& loadedModules = [&] -> ModuleMap<AbstractModuleRecord::LoadedModuleRequest> & {
            if (CyclicModuleRecord* module = referrer.getModule()) {
                owner = module;
                return module->loadedModules();
            }
            ASSERT(referrer.isRealm());
            owner = this;
            return m_loadedModules;
        }();

        ASSERT(owner);

        // 1.a. If referrer.[[LoadedModules]] contains a LoadedModuleRequest Record record such that ModuleRequestsEqual(record, moduleRequest) is true, then
        if (auto iter = loadedModules.find(ModuleMapKey { moduleRequest.m_specifier.impl(), moduleRequest.type() }); iter != loadedModules.end()) {
            // 1.a.i. Assert: record.[[Module]] and result.[[Value]] are the same Module Record.
#if USE(BUN_JSC_ADDITIONS)
            // A key has a second record once ModuleRegistryEntry::supersede() has given it to another module.
            ASSERT(iter->value.m_module.get() == *resultRecord || iter->value.m_module->moduleKey() == (*resultRecord)->moduleKey());
#else
            ASSERT(iter->value.m_module.get() == *resultRecord);
#endif
        // 1.b. Else,
        } else {
            // 1.b.i. Append the LoadedModuleRequest Record { [[Specifier]]: moduleRequest.[[Specifier]], [[Attributes]]: moduleRequest.[[Attributes]], [[Module]]: result.[[Value]] } to referrer.[[LoadedModules]].
            ModuleMapKey key { moduleRequest.m_specifier.impl(), moduleRequest.type() };
            Locker locker { owner->cellLock() };
            AbstractModuleRecord::LoadedModuleRequest value { vm, moduleRequest, *resultRecord, owner };
#if USE(BUN_JSC_ADDITIONS)
            value.m_wasReached = wasReached;
#endif
            loadedModules.add(WTF::move(key), WTF::move(value));
#if USE(BUN_JSC_ADDITIONS)
            m_hasAliasedLoadedModules |= owner == this && (moduleRequest.m_specifier.impl() != (*resultRecord)->moduleKey().impl() || moduleRequest.type() != ScriptFetchParameters::Type::JavaScript);
#endif
        }
    }

    // 2. If payload is a GraphLoadingState Record, then
    if (auto* state = dynamicDowncast<ModuleGraphLoadingState>(payload)) {
        // 2.a. Perform ContinueModuleLoading(payload, result).
        continueModuleLoading(globalObject, state, result);
        RETURN_IF_EXCEPTION(scope, void());
    // 3. Else,
    } else {
        // 3.a. Perform ContinueDynamicImport(payload, result).
        auto* dynamicPayload = uncheckedDowncast<ModuleLoaderPayload>(payload);
        continueDynamicImport(globalObject, dynamicPayload, result, WTF::move(scriptFetcher));
        RETURN_IF_EXCEPTION(scope, void());
    }

    // 4. Return UNUSED.
    scope.release();
}

void JSModuleLoader::continueModuleLoading(JSGlobalObject* globalObject, ModuleGraphLoadingState *state, ModuleCompletion moduleCompletion)
{
    // ContinueModuleLoading(state, moduleCompletion)
    // https://tc39.es/ecma262/#sec-ContinueModuleLoading

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // 1. If state.[[IsLoading]] is false, return UNUSED.
    if (!state->isLoading())
        RELEASE_AND_RETURN(scope, void());
    // 2. If moduleCompletion is a normal completion, then
    if (auto* module = std::get_if<AbstractModuleRecord*>(&moduleCompletion)) {
        // 2.a. Perform InnerModuleLoading(state, moduleCompletion.[[Value]]).
        // Per-edge fast path: most calls land here with a module that's already
        // visited (E >> V); skip the dyncast+enqueue and just decrement.
        if (state->containsVisited(*module)) {
            ASSERT(state->pendingModulesCount() > 1 || !state->drainingInnerLoad());
            state->setPendingModulesCount(state->pendingModulesCount() - 1);
            if (!state->pendingModulesCount()) {
                state->setIsLoading(false);
#if ENABLE(AOT)
                // Same as step 5.b of innerModuleLoading().
                if (!state->hasRootState())
#endif
                state->iterateVisited([&](CyclicModuleRecord* loaded) {
                    didLoadRequestedModules(globalObject, loaded);
                });
                state->promise()->fulfill(vm, *module);
            }
        } else if (state->drainingInnerLoad())
            state->enqueueInnerLoad(*module);
        else {
            innerModuleLoading(globalObject, state, *module);
            RETURN_IF_EXCEPTION(scope, void());
        }
    // 3. Else,
    } else {
        // 3.a. Set state.[[IsLoading]] to false.
        state->setIsLoading(false);
#if ENABLE(AOT)
        if (vm.hasCompiledCode()) {
            AOT::ModuleRecord::didFailToLoad(globalObject, *state);
            scope.assertNoExceptionExceptTermination();
        }
#endif
        // 3.b. Perform ! Call(state.[[PromiseCapability]].[[Reject]], undefined, « moduleCompletion.[[Value]] »).
        state->promise()->reject(vm, std::get<Exception*>(moduleCompletion)->value());
    }
    // 4. Return UNUSED.
    scope.release();
}

void JSModuleLoader::continueDynamicImport(JSGlobalObject* globalObject, ModuleLoaderPayload* payload, ModuleCompletion completion, RefPtr<ScriptFetcher> scriptFetcher)
{
    // ContinueDynamicImport(promiseCapability, moduleCompletion)
    // https://tc39.es/ecma262/#sec-ContinueDynamicImport

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSPromise* promise = payload->promise();

    // 1. If moduleCompletion is an abrupt completion, then
    if (Exception** exception = std::get_if<Exception*>(&completion)) {
        // 1.a. Perform ! Call(promiseCapability.[[Reject]], undefined, « moduleCompletion.[[Value]] »).
        promise->reject(vm, (*exception)->value());
        // 1.b. Return UNUSED.
        scope.assertNoException();
        return;
    }
    // 2. Let module be moduleCompletion.[[Value]].
    auto* module = std::get<AbstractModuleRecord*>(completion);
    // 3. Let loadPromise be module.LoadRequestedModules().
    JSPromise* loadPromise = loadRequestedModules(globalObject, module, WTF::move(scriptFetcher));
    RETURN_IF_EXCEPTION(scope, void());
    // 4-8. Link and evaluate using microtask dispatch instead of closures.
    loadPromise->performPromiseThenWithInternalMicrotask(vm, payload->deferred() ? InternalMicrotask::DynamicImportDeferLoadSettled : InternalMicrotask::DynamicImportLoadSettled, module, payload);
    // 9. Return UNUSED.
    scope.release();
}

JSPromise* JSModuleLoader::loadRequestedModules(JSGlobalObject* globalObject, AbstractModuleRecord* module, RefPtr<ScriptFetcher> scriptFetcher, ModuleGraphLoadingState* parentState)
{
    // LoadRequestedModules([hostDefined])
    // https://tc39.es/ecma262/#sec-LoadRequestedModules

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // 1. If hostDefined is not present, let hostDefined be empty.
    // 2. Let pc be ! NewPromiseCapability(%Promise%).
    JSPromise* pc = JSPromise::create(vm, globalObject->promiseStructure()); // This will eventually be resolved with an AbstractModuleRecord*.
    pc->markAsHandled();
    // 3. Let state be the GraphLoadingState Record { [[IsLoading]]: true, [[PendingModulesCount]]: 1, [[Visited]]: « », [[PromiseCapability]]: pc, [[HostDefined]]: hostDefined }.
    auto state = ModuleGraphLoadingState::create(vm, pc, WTF::move(scriptFetcher));
    RETURN_IF_EXCEPTION(scope, nullptr);
#if ENABLE(AOT)
    // A module that the loader loads step by step behaves like an uncompiled record: each state visits every record that is still New.
    if (auto* compiled = parentState ? dynamicDowncast<AOT::ModuleRecord>(module) : nullptr; compiled && !vm.synchronousModuleQueue() && !compiled->isLoadedByLoader())
        state->setRootState(vm, parentState);
#else
    UNUSED_PARAM(parentState);
#endif
    // 4. Perform InnerModuleLoading(state, module).
    innerModuleLoading(globalObject, state, module);
#if ENABLE(AOT)
    if (scope.exception() && vm.hasCompiledCode()) [[unlikely]]
        AOT::ModuleRecord::didFailToLoad(globalObject, *state);
#endif
    RETURN_IF_EXCEPTION(scope, nullptr);
    // 5. Return pc.[[Promise]].
    return pc;
}

ModuleRegistryEntry* JSModuleLoader::ensureRegistered(JSGlobalObject* globalObject, const Identifier& key, ScriptFetchParameters::Type type)
{
    VM& vm = globalObject->vm();

    ModuleMapKey moduleMapKey = keyInModuleMap(key.impl(), type);

    if (auto iter = m_moduleMap.find(moduleMapKey); iter != m_moduleMap.end())
        return iter->value.get();

    ModuleRegistryEntry* entry = ModuleRegistryEntry::create(vm, this, key, moduleMapKey.second, nullptr);

    Locker locker { cellLock() };
    m_moduleMap.add(moduleMapKey, WriteBarrier<ModuleRegistryEntry>(vm, this, entry));
    didAddModuleMapEntry(moduleMapKey.second);

    return entry;
}

ModuleRegistryEntry* JSModuleLoader::getRegisteredMayBeNull(const Identifier& key, ScriptFetchParameters::Type type)
{
    if (auto iter = m_moduleMap.find(keyInModuleMap(key.impl(), type)); iter != m_moduleMap.end())
        return iter->value.get();
    return nullptr;
}

#if USE(BUN_JSC_ADDITIONS)
ScriptFetchParameters::Type JSModuleLoader::askHostForTypeInModuleMap(UniquedStringImpl* key, ScriptFetchParameters::Type type) const
{
    JSGlobalObject* globalObject = moduleScope()->realm();
    auto* moduleTypeInRegistry = globalObject->globalObjectMethodTable()->moduleTypeInRegistry;
    return moduleTypeInRegistry && !key->isSymbol() ? moduleTypeInRegistry(globalObject, StringView(key), type) : type;
}

RefPtr<ScriptFetchParameters> JSModuleLoader::parametersOfEntry(const Identifier& key, RefPtr<ScriptFetchParameters>&& parametersOfRequest) const
{
    if (!parametersOfRequest)
        return nullptr;
    ScriptFetchParameters::Type type = keyInModuleMap(key.impl(), parametersOfRequest->type()).second;
    if (type == parametersOfRequest->type())
        return WTF::move(parametersOfRequest);
    if (type == ScriptFetchParameters::Type::JavaScript)
        return nullptr;
    Ref parameters = ScriptFetchParameters::createUnique(type);
    parameters->setAttributes(ScriptFetchParameters::AttributesMap { parametersOfRequest->attributes() });
    return parameters;
}

Vector<ModuleRegistryEntry*, 2> JSModuleLoader::entriesWithKey(const Identifier& key) const
{
    Vector<ModuleRegistryEntry*, 2> entries;
    auto lookUp = [&](ScriptFetchParameters::Type type) {
        if (auto entry = m_moduleMap.get({ key.impl(), type }))
            entries.append(entry.get());
    };
    lookUp(ScriptFetchParameters::Type::JavaScript);
    if (m_nonJavaScriptEntryCount) [[unlikely]]
        std::ranges::for_each(m_nonJavaScriptTypes, lookUp);
    return entries;
}

bool JSModuleLoader::removeEntry(const Identifier& key)
{
    return removeEntry(key, vm().importOrderState().isRunningDeferredModule ? std::optional(AbstractModuleRecord::SupersededFor::LaterImports) : std::nullopt);
}

void JSModuleLoader::removeEntryIfUnused(const Identifier& key)
{
    VM& vm = this->vm();
    auto isUnused = [&](ModuleRegistryEntry* entry) {
        if (isAheadOfRunningModule(entry))
            return true;
        if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(entry->record()))
            return synthetic->deferredModule() && !synthetic->isRunningDeferredModule();
#if ENABLE(AOT)
        if (AOT::compiledModule(vm, entry->record()))
            return false;
#endif
        // It failed to load and has never been linked.
        return vm.importOrderState().isRunningDeferredModule && mayRecoverLoad(entry) && !entry->record()->hasStartedLinking() && !ImportOrderWalk::isBeingReached(vm, entry->record());
    };
    if (auto entries = entriesWithKey(key); !entries.isEmpty() && std::ranges::all_of(entries, isUnused))
        removeEntry(key, AbstractModuleRecord::SupersededFor::AllImports);
}

bool JSModuleLoader::removeEntry(const Identifier& key, std::optional<AbstractModuleRecord::SupersededFor> supersededFor)
{
    // Bun's registry is conceptually flat (one entry per specifier), so
    // delete every (specifier, type) variant.
    auto* impl = key.impl();
    if (supersededFor) {
        for (ModuleRegistryEntry* entry : entriesWithKey(key))
            entry->supersede(vm(), *supersededFor);
    }
    Locker locker { cellLock() }; // visitChildren iterates these
    forgetPrelinkedRecordsWithKey(impl);
    // Keyed by the specifier that was asked for, which resolve() may have turned into another key.
    m_loadedModules.removeIf([&](auto& entry) { return entry.key.first == impl || entry.value.m_module->moduleKey().impl() == impl; });
    m_resolutionFailures.removeIf([&](auto& entry) { return entry.key.first == impl || entry.key.second == impl; });
    return m_moduleMap.removeIf([&](auto& entry) {
        if (entry.key.first != impl)
            return false;
        didRemoveModuleMapEntry(entry.key.second);
        return true;
    });
}

void JSModuleLoader::clearAll()
{
    if (vm().importOrderState().isRunningDeferredModule) {
        for (auto& entry : m_moduleMap.values())
            entry->supersede(vm(), AbstractModuleRecord::SupersededFor::LaterImports);
    }
    Locker locker { cellLock() };
    forgetPrelinkedRecordsWithKey(nullptr);
    m_loadedModules.clear();
    m_moduleMap.clear();
    m_nonJavaScriptEntryCount = 0;
    m_resolutionFailures.clear();
}

void JSModuleLoader::removeFromLoadedModules(AbstractModuleRecord* record)
{
    if (m_hasAliasedLoadedModules) [[unlikely]] {
        Locker locker { cellLock() };
        m_loadedModules.removeIf([&](auto& request) { return request.value.m_module.get() == record; });
        return;
    }
    if (auto loaded = m_loadedModules.find({ record->moduleKey().impl(), ScriptFetchParameters::Type::JavaScript }); loaded != m_loadedModules.end() && loaded->value.m_module.get() == record) {
        Locker locker { cellLock() };
        m_loadedModules.remove(loaded);
    }
}

ModuleRegistryEntry* JSModuleLoader::entryOf(AbstractModuleRecord* record)
{
    for (ModuleRegistryEntry* entry : entriesWithKey(record->moduleKey())) {
        if (entry->record() == record)
            return entry;
    }
    return nullptr;
}

JSPromise* JSModuleLoader::runDeferredModules(JSGlobalObject* globalObject, AbstractModuleRecord* root, RefPtr<ScriptFetcher> scriptFetcher, bool hasWaited, JSValue errorOfImport, std::optional<EvaluationOfLoad> evaluation)
{
    return ImportOrderWalk::run(globalObject, root, WTF::move(scriptFetcher), hasWaited, errorOfImport, evaluation);
}

void JSModuleLoader::setFetchWaitsFor(JSGlobalObject* globalObject, const Identifier& key, FetchWaitsFor what)
{
    for (ModuleRegistryEntry* entry : entriesWithKey(key)) {
        if (entry->record())
            continue;
        if (what != FetchWaitsFor::Host && entry->fetchWaitsFor() == FetchWaitsFor::Host)
            m_fetchesThatWaitForProgram.append({ key, entry->moduleType() });
        entry->setFetchWaitsFor(what);
        didSettleFetch(globalObject);
    }
}

ModuleRegistryEntry* JSModuleLoader::registryEntryForRequire(const Identifier& key)
{
    if (auto entry = m_moduleMap.get({ key.impl(), ScriptFetchParameters::Type::JavaScript }))
        return entry.get();
    if (!m_nonJavaScriptEntryCount) [[likely]]
        return nullptr;
    for (ModuleRegistryEntry* entry : entriesWithKey(key)) {
        if (entry->record() && !entry->record()->isOtherFormOfFile())
            return entry;
    }
    return nullptr;
}

AbstractModuleRecord* JSModuleLoader::recordOfOtherFormOfFile(const Identifier& key)
{
    if (!m_nonJavaScriptEntryCount) [[likely]]
        return nullptr;
    for (ModuleRegistryEntry* entry : entriesWithKey(key)) {
        if (entry->record() && entry->record()->isOtherFormOfFile())
            return entry->record();
    }
    return nullptr;
}

void JSModuleLoader::didMakeModule(JSGlobalObject* globalObject, ModuleRegistryEntry* entry, AbstractModuleRecord* record)
{
    auto* synthetic = entry->isEntryPoint() ? dynamicDowncast<SyntheticModuleRecord>(record) : nullptr;
    bool runsNow = synthetic && synthetic->deferredModule();
#if ENABLE(AOT)
    if (auto* compiled = AOT::compiledModule(globalObject->vm(), record))
        runsNow = compiled->runsWhenFetched();
#endif
    // From here on require() has to find it, or it makes a value of its own.
    if (record->isValue())
        entry->fetchComplete(globalObject, record);
    if (!runsNow) [[likely]]
        return;
    entry->fetchComplete(globalObject, record);
    constexpr bool mayHaveImportsInFlight = false;
    ImportOrderWalk::run(globalObject, record, nullptr, mayHaveImportsInFlight);
}

static bool isRootOfLoadInProgress(ModuleRegistryEntry* entry)
{
    if (!entry || !entry->isRootOfLoad() || entry->isFailureFinal())
        return false;
    if (!entry->record())
        return entry->status() < ModuleRegistryEntry::Status::FetchFailed;
    auto* module = dynamicDowncast<CyclicModuleRecord>(entry->record());
    return module && module->status() <= CyclicModuleRecord::Status::Unlinked;
}

JSValue JSModuleLoader::asyncContextOfImport(JSGlobalObject* globalObject) const
{
    if (m_asyncContext)
        return { };
    JSValue asyncContext = AsyncContextSwapScope::current(globalObject->vm(), globalObject);
    auto* ofImport = asyncContext.isUndefined() ? nullptr : globalObject->globalObjectMethodTable()->asyncContextOfImport;
    return ofImport ? ofImport(globalObject, asyncContext) : asyncContext;
}

void JSModuleLoader::didStartLoad(JSGlobalObject* globalObject, ModuleRegistryEntry* root, JSValue asyncContext)
{
    if (root->isRootOfLoad())
        return;
    root->setIsRootOfLoad();
    if (!isRootOfLoadInProgress(root))
        return;
    if (m_loadsInProgress.size() == m_loadsInProgress.capacity())
        forgetLoadsThatHaveEnded();
    size_t loadOfEarlierEntry = m_loadsInProgress.findIf([&](const LoadInProgress& load) { return load.root.key == root->key() && load.root.type == root->moduleType(); });
    {
        Locker locker { cellLock() };
        LoadInProgress load { { root->key(), root->moduleType() }, { }, { }, { globalObject->vm(), this, asyncContext } };
        if (loadOfEarlierEntry == notFound) [[likely]]
            m_loadsInProgress.append(WTF::move(load));
        else
            m_loadsInProgress[loadOfEarlierEntry] = WTF::move(load);
    }
    didSettleFetch(globalObject);
}

void JSModuleLoader::releaseAsyncContextOfLoad(ModuleRegistryEntry* root)
{
    for (LoadInProgress& load : m_loadsInProgress) {
        if (load.root.key == root->key() && load.root.type == root->moduleType())
            load.asyncContext.clear();
    }
}

void JSModuleLoader::forgetLoadsThatHaveEnded()
{
    if (m_isRunningAheadOfFetches)
        return;
    Locker locker { cellLock() };
    m_loadsInProgress.removeAllMatching([&](const LoadInProgress& load) {
        return !isRootOfLoadInProgress(getRegisteredMayBeNull(load.root.key, load.root.type));
    });
    m_loadsInProgress.reserveCapacity(2 * m_loadsInProgress.size());
}

// Not in a job: script that only needs the jobs it has queued gets to settle first.
void JSModuleLoader::runDeferredModulesAheadOfFetchesSoon(JSGlobalObject* globalObject)
{
    if (m_runAheadOfFetches.get())
        return;
    VM& vm = globalObject->vm();
    m_runAheadOfFetches = vm.deferredWorkTimer->addPendingWork(DeferredWorkTimer::WorkType::ImminentlyScheduled, vm, globalObject, { this });
    vm.deferredWorkTimer->scheduleWorkSoonIfActive(m_runAheadOfFetches, [this, globalObject](DeferredWorkTimer::Ticket&) {
        m_runAheadOfFetches = nullptr;
        runDeferredModulesAheadOfFetches(globalObject);
    });
}

void JSModuleLoader::runDeferredModulesAheadOfFetches(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    while (!m_fetchesThatWaitForProgram.isEmpty()) {
        auto& [key, type] = m_fetchesThatWaitForProgram.last();
        if (ModuleRegistryEntry* entry = getRegisteredMayBeNull(key, type); entry && !entry->record() && entry->status() < ModuleRegistryEntry::Status::FetchFailed && entry->fetchWaitsFor() != FetchWaitsFor::Host)
            break;
        m_fetchesThatWaitForProgram.removeLast();
    }
    if (m_fetchesThatWaitForProgram.isEmpty()) {
        for (LoadInProgress& load : m_loadsInProgress) {
            load.importInFlight.clear();
            load.passed = { };
        }
        return;
    }

    // A deferred module can start loads, and turn the event loop.
    SetForScope isRunningAheadOfFetches(m_isRunningAheadOfFetches, true);
    for (size_t index = 0; index < m_loadsInProgress.size(); ++index) {
        LoadInProgress& load = m_loadsInProgress[index];
        if (load.importInFlight && load.importInFlight->status() == JSPromise::Status::Pending)
            continue;
        load.importInFlight.clear();
        ModuleRegistryEntry* root = getRegisteredMayBeNull(load.root.key, load.root.type);
        auto* module = isRootOfLoadInProgress(root) && root->status() < ModuleRegistryEntry::Status::FetchFailed ? uncheckedDowncast<CyclicModuleRecord>(root->record()) : nullptr;
        if (!module || module->status() != CyclicModuleRecord::Status::New)
            continue;
        PassedImports passed = std::exchange(load.passed, { });
        AsyncContextOfImportScope asyncContextScope(vm, globalObject, load.asyncContext.get());
        JSPromise* importInFlight = ImportOrderWalk::runAheadOfFetches(globalObject, module, passed);
        TRY_CLEAR_EXCEPTION(scope, void());
        if (getRegisteredMayBeNull(root->key(), root->moduleType()) != root) [[unlikely]]
            continue;
        m_loadsInProgress[index].importInFlight.setMayBeNull(vm, this, importInFlight);
        m_loadsInProgress[index].passed = WTF::move(passed);
    }
}

auto JSModuleLoader::runDeferredModulesAheadOfLoad(JSGlobalObject* globalObject, const Identifier& root) -> RunAheadOfLoad
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    ModuleRegistryEntry* entry = registryEntry(root);
    auto* module = entry && !entry->isFailureFinal() ? dynamicDowncast<CyclicModuleRecord>(entry->record()) : nullptr;
    if (!module)
        return { };
    RunAheadOfLoad result = ImportOrderWalk::runAheadOfLoad(globalObject, module);
    TRY_CLEAR_EXCEPTION(scope, { });
    return result;
}

void JSModuleLoader::didRunDeferredModule(JSGlobalObject* globalObject, SyntheticModuleRecord* record)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    record->didStopRunningDeferredModule();
    if (scope.exception()) {
        // The next import runs it again, as the host would.
        record->moduleLoader()->didEndImportOrderWalk(vm);
        return;
    }
    record->AbstractModuleRecord::link(globalObject);
    TRY_CLEAR_EXCEPTION(scope, void());
}

void JSModuleLoader::forgetFailuresToLink()
{
    for (auto& entry : m_moduleMap.values())
        entry->forgetFailureToLink();
}

bool JSModuleLoader::mayRecoverLoad(ModuleRegistryEntry* entry)
{
    auto* module = dynamicDowncast<CyclicModuleRecord>(entry->record());
    if (!module || entry->isFailureFinal() || entry->didFailToRecoverLoad())
        return false;
#if ENABLE(AOT)
    if (auto* compiled = AOT::compiledModule(module->vm(), module); compiled && !module->didLoadAfterFailure())
        return compiled->mayRecoverLoad();
#endif
    if (!module->hasStartedLinking())
        return true;
    // Another load has recovered and linked it while the failure was on its way here.
    return module->didLoadAfterFailure() && module->status() >= CyclicModuleRecord::Status::Linked && !module->evaluationError();
}

bool JSModuleLoader::tryToRecoverLoad(JSGlobalObject* globalObject, ModuleLoadingContext* context, JSPromise* resultPromise, JSValue& error)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    const ModuleRequest& request = context->moduleRequest();
    bool hasWaited = context->step() == ModuleLoadingContext::Step::WaitsForImport;
    ModuleRegistryEntry* entry = getRegisteredMayBeNull(request.m_specifier, request.type());
    bool mayRecover = entry && entry == context->entry() && mayRecoverLoad(entry);
    auto hasGivenWayToCopy = [&] {
#if ENABLE(AOT)
        auto* compiled = context->entry() ? AOT::compiledModule(vm, context->entry()->record()) : nullptr;
        return compiled && compiled->hasGivenWayToCopy() && context->step() != ModuleLoadingContext::Step::LoadsAgain;
#else
        return false;
#endif
    };
    if (!hasWaited && !hasGivenWayToCopy() && (!mayRecover || context->flags().contains(ModuleLoadFlag::IsSecondAttempt) || context->step() == ModuleLoadingContext::Step::LoadsAgain)) {
        // Another load has settled what this one fails with.
        JSValue errorOfEntry = entry && entry == context->entry() && entry->isFailureFinal() ? entry->error(globalObject) : JSValue();
        RETURN_IF_EXCEPTION(scope, true);
        if (errorOfEntry)
            error = errorOfEntry;
        return false;
    }

    AsyncContextOfImportScope asyncContextScope(vm, globalObject, context->asyncContext());
    if (mayRecover) {
        constexpr bool mayHaveImportsInFlight = true;
        std::optional<EvaluationOfLoad> evaluation;
        if (context->evaluate() && !context->deferred())
            evaluation = { context->referrerAsyncOrder(), context->dynamic() ? resultPromise : nullptr };
        JSValue errorOfLoad = hasWaited ? entry->error(globalObject) : error;
        RETURN_IF_EXCEPTION(scope, true);
        entry->setIsRecoveringLoad(true);
        JSPromise* importInFlight = ImportOrderWalk::run(globalObject, entry->record(), context->scriptFetcher(), mayHaveImportsInFlight, errorOfLoad, evaluation);
        entry->setIsRecoveringLoad(importInFlight);
        if (Exception* exception = scope.exception()) {
            TRY_CLEAR_EXCEPTION(scope, true);
            if (!ImportOrderWalk::isOnPathOfWalk(vm, entry->record()))
                entry->setDidFailToRecoverLoad();
            JSValue errorOfEntry = entry->isFailureFinal() ? entry->error(globalObject) : JSValue();
            RETURN_IF_EXCEPTION(scope, true);
            error = errorOfEntry ? errorOfEntry : exception->value();
            return false;
        }
        if (importInFlight) {
            context->setStep(ModuleLoadingContext::Step::WaitsForImport);
            importInFlight->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadTopRejected, resultPromise, context);
            return true;
        }
    } else if (hasWaited)
        didEndImportOrderWalk(vm);

    OptionSet<ModuleLoadFlag> flags = context->flags();
    flags.set(ModuleLoadFlag::IsSecondAttempt, !hasGivenWayToCopy());
    JSPromise* promise = loadModule(globalObject, request.m_specifier, request.m_attributes, context->scriptFetcher(), flags, context->referrerAsyncOrder());
    if (Exception* exception = scope.exception()) [[unlikely]] {
        error = exception->value();
        TRY_CLEAR_EXCEPTION(scope, true);
        return false;
    }
    context->setStep(ModuleLoadingContext::Step::LoadsAgain);
    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleLoadTopRejected, resultPromise, context);
    return true;
}

bool JSModuleLoader::isAheadOfRunningModule(ModuleRegistryEntry* entry)
{
    VM& vm = entry->vm();
    if (!vm.importOrderState().isRunningDeferredModule || entry->isFailureFinal())
        return false;
    AbstractModuleRecord* record = entry->record();
    if (!record)
        return true;
#if ENABLE(AOT)
    if (AOT::compiledModule(vm, record))
        return false;
#endif
    return !record->wasReached() && !record->hasStartedLinking();
}

bool JSModuleLoader::isAheadOfRunningModule(AbstractModuleRecord* importer, const AbstractModuleRecord::LoadedModuleRequest& loaded)
{
    VM& vm = importer->vm();
    if (!vm.importOrderState().isRunningDeferredModule)
        return false;
#if ENABLE(AOT)
    if (AOT::compiledModule(vm, importer))
        return false;
#endif
    return !loaded.m_wasReached && !importer->hasStartedLinking();
}

bool JSModuleLoader::hasPostponedImportOf(const Identifier& key)
{
    return ImportOrderWalk::hasPostponedImportOf(this, key);
}

bool JSModuleLoader::someImporterAboutToBeLinked(AbstractModuleRecord* imported, NOESCAPE const Function<bool(CyclicModuleRecord*)>& counts)
{
    return ImportOrderWalk::modulesAboutToBeLinked(imported->vm()).containsIf([&](CyclicModuleRecord* importer) {
        return std::ranges::any_of(importer->loadedModules().values(), [&](auto& loaded) { return loaded.m_module.get() == imported; }) && counts(importer);
    });
}

bool JSModuleLoader::hasRunAheadOfLink(SyntheticModuleRecord* record)
{
    if (!record->vm().importOrderState().isRunningDeferredModule || !record->isOfDeferredModule() || !record->isReadyToBeLinked() || record->hasStartedLinking())
        return false;
    return !someImporterAboutToBeLinked(record, [](CyclicModuleRecord*) { return true; });
}

static bool isWalkToStartAt(ModuleRegistryEntry* entry)
{
    if (!entry->isRootOfLoadThatLinks())
        return false;
    if (entry->status() >= ModuleRegistryEntry::Status::FetchFailed)
        return entry->isRecoveringLoad() && JSModuleLoader::mayRecoverLoad(entry);
    if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(entry->record()))
        return synthetic->deferredModule();
#if ENABLE(AOT)
    if (AOT::compiledModule(entry->vm(), entry->record()))
        return false;
#endif
    return !entry->record()->hasStartedLinking();
}

// A walk does not wait for the steps that fulfill the load promise of an import that has loaded.
static bool hasWalkOvertakenLoad(ModuleRegistryEntry* entry)
{
    if (!entry->vm().importOrderState().isRunningDeferredModule || !entry->record() || entry->isRootOfLoad())
        return false;
    auto* module = dynamicDowncast<CyclicModuleRecord>(entry->record());
    return !module || module->status() != CyclicModuleRecord::Status::New;
}

JSPromise* JSModuleLoader::promiseForEndOfLoad(JSGlobalObject* globalObject, ModuleRegistryEntry* entry)
{
    AbstractModuleRecord* record = entry->record();
    if (record && ImportOrderWalk::isBeingReached(globalObject->vm(), record))
        return promiseForEndOfImportOrderWalk(globalObject);
    JSPromise* promise = entry->loadPromise();
    if (!promise && entry->status() == ModuleRegistryEntry::Status::Fetching)
        promise = entry->ensureModulePromise(globalObject);
    if (promise && promise->status() == JSPromise::Status::Pending && !hasWalkOvertakenLoad(entry))
        return promise;
    if (!record)
        return nullptr;
    auto* module = dynamicDowncast<CyclicModuleRecord>(record);
    if (JSPromise* load = module ? ImportOrderWalk::pendingLoadBelow(module) : nullptr)
        return load;
    return isWalkToStartAt(entry) ? promiseForEndOfImportOrderWalk(globalObject) : nullptr;
}

JSPromise* JSModuleLoader::promiseForTurnToBeEvaluated(JSGlobalObject* globalObject, AbstractModuleRecord* record)
{
    VM& vm = globalObject->vm();
    auto* module = dynamicDowncast<CyclicModuleRecord>(record);
    if (!module || !EvaluationAheadOfLink::waitsForEvaluationOfImport(globalObject, EvaluationAheadOfLink::moduleOf(vm, module))) [[likely]]
        return nullptr;
    if (!m_turnToBeEvaluated)
        m_turnToBeEvaluated.set(vm, this, JSPromise::create(vm, globalObject->promiseStructure()));
    return m_turnToBeEvaluated.get();
}

JSPromise* JSModuleLoader::promiseForEndOfImportOrderWalk(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    if (!m_endOfImportOrderWalk)
        m_endOfImportOrderWalk.set(vm, this, JSPromise::create(vm, globalObject->promiseStructure()));
    return m_endOfImportOrderWalk.get();
}

static void fulfillAndClear(VM& vm, WriteBarrier<JSPromise>& slot)
{
    JSPromise* promise = slot.get();
    if (!promise) [[likely]]
        return;
    slot.clear();
    promise->fulfill(vm, jsUndefined());
}

void JSModuleLoader::didEndImportOrderWalk(VM& vm)
{
    fulfillAndClear(vm, m_endOfImportOrderWalk);
    didStopImportOrderWalk(vm);
}

void JSModuleLoader::didStopImportOrderWalk(VM& vm)
{
    fulfillAndClear(vm, m_turnToBeEvaluated);
}

void JSModuleLoader::didChangeResolution(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& state = vm.importOrderState();
    if (!state.isRunningDeferredModule) {
        state.clock += !state.isHostResolving;
        return;
    }
#if ENABLE(AOT)
    if (vm.hasCompiledCode())
        AOT::keepImportsThatWalksHold(globalObject);
#endif
    state.lastChangeOfResolution = ImportOrderWalk::noteChange(vm);
}

JSModuleLoader::BatchOfChanges::BatchOfChanges(JSGlobalObject* globalObject)
    : m_vm(globalObject->vm())
    , m_isInOuterBatch(m_vm.importOrderState().isInBatchOfChanges)
{
#if ENABLE(AOT)
    if (m_vm.hasCompiledCode())
        AOT::keepImportsThatWalksHold(globalObject);
#endif
    ImportOrderWalk::keepImportsOfWhatHasBegun(m_vm);
    m_vm.importOrderState().isInBatchOfChanges = true;
}

JSModuleLoader::BatchOfChanges::~BatchOfChanges()
{
    m_vm.importOrderState().isInBatchOfChanges = m_isInOuterBatch;
}

void JSModuleLoader::BatchOfChanges::willEvictImporterAheadOfRunningModule(AbstractModuleRecord* record)
{
    for (CyclicModuleRecord* module : ImportOrderWalk::modulesAboutToBeLinked(m_vm)) {
        for (auto& loaded : module->loadedModules().values())
            loaded.m_wasReached |= loaded.m_module.get() == record;
    }
}

void JSModuleLoader::didChangeFetch(VM& vm, NOESCAPE const Function<bool(const String& key)>& isAffected)
{
    if (!vm.importOrderState().isRunningDeferredModule)
        return;
    ImportOrderWalk::forEachLoaderOfWalkInProgress(vm, [&](JSModuleLoader* loader) {
        Vector<Identifier> keys;
        for (auto& entry : loader->m_moduleMap.values()) {
            if (isAheadOfRunningModule(entry.get()))
                keys.append(entry->key());
        }
        for (const Identifier& key : keys) {
            if (isAffected(key.string()))
                loader->removeEntry(key);
        }
    });
}

void JSModuleLoader::setPrelinkedModuleGraph(Ref<PrelinkedModuleGraph>&& graph)
{
    ASSERT(Options::usePrelinkedModuleInfo());
    ASSERT(!m_prelinkedGraph || m_prelinkedGraph == graph.ptr());
    if (m_prelinkedGraph)
        return;
    Locker locker { cellLock() };
    m_prelinkedGraph = WTF::move(graph);
    m_prelinkedRecords.grow(m_prelinkedGraph->moduleCount());
}

void JSModuleLoader::setPrelinkedRecord(VM& vm, uint32_t moduleIndex, AbstractModuleRecord* record)
{
    RELEASE_ASSERT(m_prelinkedGraph && moduleIndex < m_prelinkedRecords.size(), moduleIndex, m_prelinkedRecords.size());
    ASSERT(record->isPrelinked() && record->prelinkedGraph() == m_prelinkedGraph && record->prelinkedIndex() == moduleIndex);
    ASSERT(!m_prelinkedRecords[moduleIndex] || m_prelinkedRecords[moduleIndex].get() == record);
    if (moduleIndex < m_prelinkedRecordRemoved.size() && m_prelinkedRecordRemoved.quickGet(moduleIndex)) [[unlikely]]
        return; // prelinkedRecordForResolution stays null for it; the record itself still works by its own edges
    m_prelinkedRecords[moduleIndex].set(vm, this, record);
}

// A graph edge importer -> target is normally held only by this table (AbstractModuleRecord::prelinkedRequestedModule).
// Before a slot is cleared, put the edges that touch it into the records' own [[LoadedModules]], as they would be for
// ordinary records: the leaving record keeps its targets, and its importers keep it (their memoized import resolutions
// and later GetImportedModule calls name it).
void JSModuleLoader::pinPrelinkedEdges(uint32_t moduleIndex)
{
    AbstractModuleRecord* leaving = m_prelinkedRecords[moduleIndex].get();
    if (!leaving)
        return;
    pinPrelinkedEdgesOf(leaving, PrelinkedModuleGraph::noModule);
    for (auto& slot : m_prelinkedRecords) {
        if (slot && slot.get() != leaving)
            pinPrelinkedEdgesOf(slot.get(), moduleIndex);
    }
}

// importer's edges to onlyTarget (noModule: to every module) whose target is still in the table.
void JSModuleLoader::pinPrelinkedEdgesOf(AbstractModuleRecord* importer, uint32_t onlyTarget)
{
    auto requests = m_prelinkedGraph->requests(importer->prelinkedModule());
    for (unsigned i = 0; i < requests.size(); ++i) {
        uint32_t target = requests[i].moduleIndex;
        if (target == PrelinkedModuleGraph::noModule || (onlyTarget != PrelinkedModuleGraph::noModule && target != onlyTarget))
            continue;
        if (AbstractModuleRecord* record = prelinkedRecordForResolution(target))
            importer->setImportedModule(importer->globalObject(), importer->requestedModules()[i], record);
    }
}

void JSModuleLoader::forgetPrelinkedRecord(uint32_t moduleIndex)
{
    RELEASE_ASSERT(moduleIndex < m_prelinkedRecords.size(), moduleIndex, m_prelinkedRecords.size());
    pinPrelinkedEdges(moduleIndex);
    m_prelinkedRecords[moduleIndex].clear();
    m_prelinkedRecordRemoved.ensureSize(m_prelinkedRecords.size());
    m_prelinkedRecordRemoved.quickSet(moduleIndex);
}

void JSModuleLoader::forgetPrelinkedRecordsWithKey(UniquedStringImpl* keyOrNullForAll)
{
    if (!keyOrNullForAll) {
        // Every slot leaves: pin each edge once, from its importer, while all the targets are still in the table.
        for (auto& slot : m_prelinkedRecords) {
            if (slot)
                pinPrelinkedEdgesOf(slot.get(), PrelinkedModuleGraph::noModule);
        }
    }
    for (unsigned i = 0; i < m_prelinkedRecords.size(); ++i) {
        auto& slot = m_prelinkedRecords[i];
        if (slot && (!keyOrNullForAll || slot->moduleKey().impl() == keyOrNullForAll)) {
            if (keyOrNullForAll)
                pinPrelinkedEdges(i);
            slot.clear();
            m_prelinkedRecordRemoved.ensureSize(m_prelinkedRecords.size());
            m_prelinkedRecordRemoved.quickSet(i);
        }
    }
}

int64_t JSModuleLoader::asyncEvaluationOrderForKey(const Identifier& key)
{
    if (key.isNull() || key.isEmpty())
        return -1;
    auto* entry = getRegisteredMayBeNull(key, ScriptFetchParameters::Type::JavaScript);
    if (!entry)
        return -1;
    auto* cyclic = dynamicDowncast<CyclicModuleRecord>(entry->record());
    if (!cyclic)
        return -1;
    auto order = cyclic->asyncEvaluationOrder();
    return order.hasOrder() ? order.order() : -1;
}
#endif

void JSModuleLoader::removeFailedFetchEntry(ModuleRegistryEntry* entry)
{
    // https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-module-script step 13.1.2.
    ASSERT(entry->status() == ModuleRegistryEntry::Status::FetchFailed);
    ModuleMapKey moduleMapKey { entry->key().impl(), entry->moduleType() };
    auto iter = m_moduleMap.find(moduleMapKey);
    if (iter == m_moduleMap.end() || iter->value.get() != entry)
        return;
    Locker locker { cellLock() };
    m_moduleMap.remove(iter);
    didRemoveModuleMapEntry(moduleMapKey.second);
}

#if ENABLE(AOT)
void JSModuleLoader::removeEntriesForRecord(AbstractModuleRecord* record, std::optional<AbstractModuleRecord::SupersededFor> supersededFor)
{
    ModuleMapKey moduleMapKey { record->moduleKey().impl(), ScriptFetchParameters::Type::JavaScript };
    ModuleRegistryEntry* entry = m_moduleMap.get(moduleMapKey).get();
    bool isRegistered = entry && entry->record() == record;
    if (supersededFor && isRegistered)
        entry->supersede(vm(), *supersededFor);
    else if (supersededFor)
        record->supersede(ImportOrderWalk::noteChange(vm()), *supersededFor);
    if (isRegistered) {
        Locker locker { cellLock() };
        m_moduleMap.remove(moduleMapKey);
    }
    removeFromLoadedModules(record);
}
#endif

void JSModuleLoader::addResolutionFailure(VM& vm, const ResolutionMapKey& key, JSValue error)
{
    Locker locker { cellLock() };
    if (m_resolutionFailures.size() >= JSModuleLoaderInternal::maximumResolutionFailures)
        m_resolutionFailures.remove(m_resolutionFailures.begin());
#if USE(BUN_JSC_ADDITIONS)
    m_resolutionFailures.add(key, ResolutionFailure { WriteBarrier<Unknown>(vm, this, error), key.first, key.second, vm.importOrderState().clock });
#else
    m_resolutionFailures.add(key, ResolutionFailure { WriteBarrier<Unknown>(vm, this, error), key.first, key.second });
#endif
}

ProgramExecutable* JSModuleLoader::ModuleReferrer::getScript() const
{
    auto option = std::get_if<ProgramExecutable*>(this);
    return option ? *option : nullptr;
}

CyclicModuleRecord* JSModuleLoader::ModuleReferrer::getModule() const
{
    auto option = std::get_if<CyclicModuleRecord*>(this);
    return option ? *option : nullptr;
}

JSGlobalObject* JSModuleLoader::ModuleReferrer::getRealm() const
{
    auto option = std::get_if<JSGlobalObject*>(this);
    return option ? *option : nullptr;
}

bool JSModuleLoader::ModuleReferrer::isScript() const
{
    return std::holds_alternative<ProgramExecutable*>(*this);
}

bool JSModuleLoader::ModuleReferrer::isModule() const
{
    return std::holds_alternative<CyclicModuleRecord*>(*this);
}

bool JSModuleLoader::ModuleReferrer::isRealm() const
{
    return std::holds_alternative<JSGlobalObject*>(*this);
}

JSValue JSModuleLoader::ModuleReferrer::toJSValue() const
{
    if (auto executable = std::get_if<ProgramExecutable*>(this))
        return *executable;
    if (auto module = std::get_if<CyclicModuleRecord*>(this))
        return *module;
    ASSERT(isRealm());
    return std::get<JSGlobalObject*>(*this);
}

#if USE(BUN_JSC_ADDITIONS)
// The load ends without it.
static void leaveToImportOrderWalk(JSGlobalObject* globalObject, const VM::PostponedModuleRequest& request)
{
    auto* importer = uncheckedDowncast<CyclicModuleRecord>(request.importer);
    importer->moduleLoader()->continueModuleLoading(globalObject, uncheckedDowncast<ModuleGraphLoadingState>(request.loadingState), importer);
}

// A queue gets the steps of every load that waits for something it settles. A load that began outside of it goes on
// there if it imports a module that is in the middle of the require() that drains the queue.
static bool leaveStepToQueueOfItsLoad(JSGlobalObject* globalObject, VM::SynchronousModuleQueue& queue, const VM::SynchronousModuleTask& step)
{
    uint32_t queueOfLoad;
    AbstractModuleRecord* root;
    switch (step.task) {
    case InternalMicrotask::ModuleLoadLinkEvaluateSettled:
    case InternalMicrotask::ModuleLoadTopRejected: {
        auto* context = uncheckedDowncast<ModuleLoadingContext>(step.arg2);
        queueOfLoad = context->queueOfLoad();
        root = context->entry() ? context->entry()->record() : nullptr;
        break;
    }
    case InternalMicrotask::DynamicImportLoadSettled:
    case InternalMicrotask::DynamicImportDeferLoadSettled:
        queueOfLoad = uncheckedDowncast<ModuleLoaderPayload>(step.arg2)->queueOfLoad();
        root = uncheckedDowncast<AbstractModuleRecord>(step.arg0);
        break;
    default:
        return false;
    }
    if (queueOfLoad == queue.number) [[likely]]
        return false;
    if (!root || !ImportOrderWalk::wouldComeToRunningModule(globalObject, root))
        return false;
    for (auto* outer = queue.prev; outer; outer = outer->prev) {
        if (outer->number == queueOfLoad) {
            outer->tasks.append(step);
            return true;
        }
    }
    globalObject->queueMicrotask(globalObject->vm(), step.task, step.payload, step.arg0, step.arg1, step.arg2, step.arg3);
    return true;
}

void JSModuleLoader::drainSynchronousModuleQueue(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* queue = vm.m_synchronousModuleQueue;
    ASSERT(queue);
    auto& tasks = queue->tasks;
    // FIFO drain. Tasks may append while we iterate, so index by position and
    // never shrink mid-loop. Clear afterwards so the next caller starts fresh.
    size_t i = 0;
    while (i < tasks.size()) {
        auto t = tasks[i++];
        if (leaveStepToQueueOfItsLoad(globalObject, *queue, t)) [[unlikely]]
            continue;
        runInternalMicrotask(globalObject, vm, t.task, t.payload, t.arg0, t.arg1, t.arg2, t.arg3);
        if (scope.exception()) [[unlikely]] {
            // The remaining entries are reactions that performPromiseThen…/
            // triggerPromiseReactions diverted off the global microtask queue.
            // Dropping them would leave their loadPromise/modulePromise pending
            // forever, so hand them back to the global queue (where they would
            // have gone without the synchronous diversion) before propagating
            // the exception. They run on the next normal microtask drain.
            if (!queue->postponedRequests.isEmpty()) {
                SuspendExceptionScope suspendException(vm);
                DeferTerminationForAWhile deferTermination(vm);
                for (auto& request : std::exchange(queue->postponedRequests, { })) {
                    leaveToImportOrderWalk(globalObject, request);
                    (void)scope.tryClearException();
                }
            }
            while (i < tasks.size()) {
                auto rest = tasks[i++];
                globalObject->queueMicrotask(vm, rest.task, rest.payload, rest.arg0, rest.arg1, rest.arg2, rest.arg3);
            }
            tasks.shrink(0);
            return;
        }
    }
    tasks.shrink(0);
}

void JSModuleLoader::makePostponedRequest(JSGlobalObject* globalObject, const VM::PostponedModuleRequest& postponed)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* state = uncheckedDowncast<ModuleGraphLoadingState>(postponed.loadingState);
    auto* importer = uncheckedDowncast<CyclicModuleRecord>(postponed.importer);
    ModuleRequest request = importer->requestedModules()[postponed.index];
    ModuleMapKey key { request.m_specifier.impl(), request.type() };
    JSPromise* promise = nullptr;
    if (AbstractModuleRecord* loaded = importer->loadedModules().get(key).m_module.get())
        continueModuleLoading(globalObject, state, loaded);
    else {
        bool useImportMap = true;
#if ENABLE(AOT)
        auto* compiled = AOT::compiledModule(vm, importer);
        useImportMap = !compiled;
        uint8_t flagsBefore = compiled ? compiled->willLoadImportedModule(request.m_specifier) : 0;
#endif
        promise = hostLoadImportedModule(globalObject, importer, request, state, state->scriptFetcher(), useImportMap, RequestFlag::CannotBePostponed);
#if ENABLE(AOT)
        if (compiled)
            compiled->didLoadImportedModule(flagsBefore);
#endif
        if (scope.exception()) [[unlikely]]
            importer->didFailForRequest(request);
    }
    if (Exception* exception = scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, continueModuleLoading(globalObject, state, exception));
    }
    if (promise && !importer->loadedModules().contains(key))
        promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleGraphLoadingError, nullptr, state);
}

void JSModuleLoader::finishSynchronousLoad(JSGlobalObject* globalObject, const Identifier& rootKey, ScriptFetchParameters::Type type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto& postponedRequests = vm.m_synchronousModuleQueue->postponedRequests;
    SetForScope keepsPostponedRequests(vm.m_synchronousModuleQueue->keepsPostponedRequests, true);
    bool didRunAheadOfFetches = false;
    while (true) {
        drainSynchronousModuleQueue(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (postponedRequests.isEmpty() && (m_fetchesThatWaitForProgram.isEmpty() || didRunAheadOfFetches)) [[likely]]
            return;

        ModuleRegistryEntry* entry = getRegisteredMayBeNull(rootKey, type);
        auto* root = entry ? dynamicDowncast<CyclicModuleRecord>(entry->record()) : nullptr;
        if (postponedRequests.isEmpty()) {
            if (!root || root->status() != CyclicModuleRecord::Status::New || entry->status() >= ModuleRegistryEntry::Status::FetchFailed)
                return;
            didRunAheadOfFetches = true;
            PassedImports passed;
            ImportOrderWalk::runAheadOfFetches(globalObject, root, passed);
            TRY_CLEAR_EXCEPTION(scope, void());
            continue;
        }
        JSPromise* importInFlight = root && !entry->isFailureFinal() ? ImportOrderWalk::runAheadOfLoad(globalObject, root).importInFlight : nullptr;
        TRY_CLEAR_EXCEPTION(scope, void());
        if (importInFlight) [[unlikely]] {
            drainSynchronousModuleQueue(globalObject);
            RETURN_IF_EXCEPTION(scope, void());
            if (importInFlight->status() != JSPromise::Status::Pending)
                continue;
        }
        auto modulesOfLoad = root && !importInFlight ? ImportOrderWalk::modulesYetToBeLinkedBelow(root) : ImportOrderWalk::VisitedRecords { };

        size_t count = postponedRequests.size();
        for (size_t index = 0; index < count; ++index) {
            VM::PostponedModuleRequest request = postponedRequests[index];
            auto* importer = uncheckedDowncast<CyclicModuleRecord>(request.importer);
            if (!uncheckedDowncast<ModuleGraphLoadingState>(request.loadingState)->isLoading())
                continue;
            bool isMadeByLoad = modulesOfLoad.contains(importer);
#if ENABLE(AOT)
            if (AOT::compiledModule(vm, importer))
                isMadeByLoad = root && entry->isFailureFinal() && request.index < importer->requestedModules().size();
#endif
            if (isMadeByLoad)
                importer->moduleLoader()->makePostponedRequest(globalObject, request);
            else
                leaveToImportOrderWalk(globalObject, request);
            TRY_CLEAR_EXCEPTION(scope, void());
        }
        postponedRequests.removeAt(0, count);
    }
}

JSPromise* JSModuleLoader::loadModuleSync(JSGlobalObject* globalObject, const Identifier& moduleName, RefPtr<ScriptFetchParameters>&& parameters, RefPtr<ScriptFetcher>&& scriptFetcher)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    VM::SynchronousModuleQueueScope queue(vm);

    // The root is linked once the load promise in its entry settles. If another load created that promise and has already made the
    // record, the ModuleLoadStep that settles it is on the normal microtask queue or in the queue of an outer synchronous load.
    // This load drains neither.
    auto type = parameters ? parameters->type() : ScriptFetchParameters::Type::JavaScript;
    ModuleRegistryEntry* existingEntry = getRegisteredMayBeNull(moduleName, type);
    bool wasAbandoned = existingEntry && existingEntry->isAbandoned();
    auto isBlockedOnAnotherLoad = [&] {
        ModuleRegistryEntry* entry = getRegisteredMayBeNull(moduleName, type);
        if (!entry || entry->status() != ModuleRegistryEntry::Status::Fetched || !entry->loadPromise() || entry->loadPromise()->status() != JSPromise::Status::Pending)
            return false;
        // An outer synchronous load that started the root's load also finishes it, unless it has gone on to run a deferred module.
        if (!vm.hasCompiledCode())
            return !entry->wasLoadStartedSynchronously() || wasAbandoned || vm.importOrderState().isRunningDeferredModule;
#if ENABLE(AOT)
        // It is not blocked if an import() started the root's load and the step that loads the root's own requests is queued in an outer synchronous load.
        for (auto* outer = vm.m_synchronousModuleQueue->prev; outer; outer = outer->prev) {
            for (const VM::SynchronousModuleTask& step : outer->tasks) {
                if (step.task != InternalMicrotask::ModuleLoadStep || uncheckedDowncast<JSPromise>(step.arg0)->status() != JSPromise::Status::Pending)
                    continue;
                auto* context = uncheckedDowncast<ModuleLoadingContext>(step.arg2);
                if (context->entry() == entry && !context->payload()->inherits<ModuleGraphLoadingState>())
                    return false;
            }
        }
#endif
        return true;
    };
    bool wasLoadAlreadyPending = existingEntry && existingEntry->loadPromise() && existingEntry->loadPromise()->status() == JSPromise::Status::Pending;
    bool isLoadedByAnotherLoad = existingEntry && existingEntry->loadPromise() && !wasAbandoned;
    JSPromise* result = nullptr;
    auto noteIfAbandoned = makeScopeExit([&] {
        ModuleRegistryEntry* entry = result && result->status() == JSPromise::Status::Pending && !isLoadedByAnotherLoad ? getRegisteredMayBeNull(moduleName, type) : nullptr;
        if (entry && !(entry->record() && entry->record()->hasStartedLinking()))
            entry->setIsAbandoned(true);
    });
    if (!(wasAbandoned || vm.importOrderState().isRunningDeferredModule) || !isBlockedOnAnotherLoad()) {
        result = loadModule(globalObject, moduleName, parameters.copyRef(), scriptFetcher.copyRef(), { ModuleLoadFlag::Evaluate, ModuleLoadFlag::IsSynchronous });
        RETURN_IF_EXCEPTION(scope, result);
        finishSynchronousLoad(globalObject, moduleName, type);
        RETURN_IF_EXCEPTION(scope, result);
        if (result->status() != JSPromise::Status::Pending || !wasLoadAlreadyPending || !isBlockedOnAnotherLoad())
            return result;
    }
    // Load the root again. This time the load queues its own steps, starting from the module promise.
    // Otherwise require() throws where it runs between the loader steps of an import() of the same module, or between a mock factory and the loader steps of the import() that called the factory. See Bun's test/aot/parallel-test-runs/a-require-of-a-module-whose-load-an-import-has-begun.
    SetForScope rootBeingReloaded(m_rootBeingReloaded, moduleName.impl());
    result = loadModule(globalObject, moduleName, WTF::move(parameters), WTF::move(scriptFetcher), { ModuleLoadFlag::Evaluate, ModuleLoadFlag::IsSynchronous });
    RETURN_IF_EXCEPTION(scope, result);

    scope.release();
    finishSynchronousLoad(globalObject, moduleName, type);
    return result;
}

extern "C" __attribute__((weak)) EncodedJSValue Bun__analyzeTranspiledModule(JSGlobalObject* globalObject, JSModuleLoader*, const Identifier& moduleKey, const SourceCode& sourceCode, JSPromise* promise)
{
    (void)moduleKey;
    (void)sourceCode;

    VM& vm = globalObject->vm();
    promise->reject(vm, createError(globalObject, makeString("Bun__analyzeTranspiledModule is not implemented"_s)));
    return JSValue::encode(promise);
}
#endif

JSPromise* JSModuleLoader::makeModule(JSGlobalObject* globalObject, const Identifier& moduleKey, JSValue moduleData)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (auto* record = dynamicDowncast<AbstractModuleRecord>(moduleData)) {
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->markAsHandled();
#if ENABLE(AOT)
        if (auto* compiled = dynamicDowncast<AOT::ModuleRecord>(record)) {
            record = compiled->recordToLoad(globalObject);
            RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));
            uncheckedDowncast<AOT::ModuleRecord>(record)->didLoad();
            RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));
        }
#endif
        scope.release();
        promise->fulfill(vm, record);
        return promise;
    }

    JSSourceCode* jsSourceCode = dynamicDowncast<JSSourceCode>(moduleData);
    if (!jsSourceCode) {
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->markAsHandled();
        auto* moduleRecord = SyntheticModuleRecord::tryCreateDefaultExportSyntheticModule(globalObject, this, moduleKey, moduleData, SourceProviderSourceType::CSS);
        RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));
        promise->fulfill(vm, moduleRecord);
        RELEASE_AND_RETURN(scope, promise);
    }

#if ENABLE(AOT)
    if (vm.hasCompiledCode() && this == globalObject->moduleLoader())
        AOT::willMakeRecordFromSource(globalObject, moduleKey);
#endif

#if USE(BUN_JSC_ADDITIONS)
    EnsureStillAliveScope ownerOfSourceCode(jsSourceCode);
#endif
    const SourceCode& sourceCode = jsSourceCode->sourceCode();

    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    promise->markAsHandled();

    SourceProviderSourceType sourceType = sourceCode.provider()->sourceType();
#if USE(BUN_JSC_ADDITIONS)
    if (vm.runsOnlyStaticCode()) [[unlikely]] {
        // Earlier than ScriptExecutable::prepareForExecutionImpl: what a module imports is loaded and evaluated before the module is. Every type but these four is parsed as JavaScript below.
        bool isParsedAsJavaScript = sourceType != SourceProviderSourceType::WebAssembly && sourceType != SourceProviderSourceType::JSON && sourceType != SourceProviderSourceType::Text && sourceType != SourceProviderSourceType::Synthetic;
        if (isParsedAsJavaScript && sourceCode.provider()->provenance() != SourceProvenance::Static) {
            throwNonStaticCodeError(globalObject, scope);
            return promise->rejectWithCaughtException(vm, scope);
        }
    }

    bool hasLiveExports = sourceCode.provider()->moduleHasLiveExports();
    RELEASE_ASSERT(!hasLiveExports || sourceType == SourceProviderSourceType::Module || sourceType == SourceProviderSourceType::Synthetic);
#endif
    switch (sourceType) {
#if ENABLE(WEBASSEMBLY)
    case SourceProviderSourceType::WebAssembly:
        RELEASE_AND_RETURN(scope, uncheckedDowncast<JSPromise>(JSWebAssembly::instantiate(globalObject, promise, this, sourceCode.provider(), moduleKey, jsSourceCode)));
#endif

    // https://tc39.es/proposal-json-modules/#sec-parse-json-module
    case SourceProviderSourceType::JSON: {
        auto* moduleRecord = SyntheticModuleRecord::parseJSONModule(globalObject, this, moduleKey, SourceCode { sourceCode });
        attachErrorInfo(globalObject, scope, moduleRecord, moduleKey, ScriptFetchParameters::Type::JSON, ModuleFailure::Kind::Evaluation);
        RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));
        scope.release();
        promise->fulfill(vm, moduleRecord);
        return promise;
    }
    case SourceProviderSourceType::Text: {
        auto* moduleRecord = SyntheticModuleRecord::createTextModule(globalObject, this, moduleKey, SourceCode { sourceCode });
        attachErrorInfo(globalObject, scope, moduleRecord, moduleKey, ScriptFetchParameters::Type::Text, ModuleFailure::Kind::Evaluation);
        RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));
        scope.release();
        promise->fulfill(vm, moduleRecord);
        return promise;
    }

#if USE(BUN_JSC_ADDITIONS)
    case SourceProviderSourceType::Synthetic: {
        SyntheticSourceProvider* syntheticSourceProvider = reinterpret_cast<SyntheticSourceProvider*>(sourceCode.provider());
        if (syntheticSourceProvider->deferredModule()) {
            RELEASE_ASSERT(!hasLiveExports);
            auto* moduleRecord = SyntheticModuleRecord::createWithDeferredGenerator(globalObject, this, moduleKey, Ref { *syntheticSourceProvider });
            scope.release();
            promise->fulfill(vm, moduleRecord);
            return promise;
        }

        MarkedArgumentBuffer args;
        Vector<Identifier, 4> exportNames;
        JSObject* lazyExportsSource = syntheticSourceProvider->generate(globalObject, moduleKey, exportNames, args);
        RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));

        auto lazyExports = hasLiveExports ? SyntheticModuleRecord::LazyExports::Live : SyntheticModuleRecord::LazyExports::ReadOnce;
        auto* moduleRecord = SyntheticModuleRecord::tryCreateWithExportNamesAndValues(globalObject, this, moduleKey, exportNames, args, lazyExportsSource, lazyExports);
        RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));
        if (syntheticSourceProvider->copiesMutableExports())
            moduleRecord->setGeneratorOfMutableExports(*syntheticSourceProvider);
        moduleRecord->setIsValue(syntheticSourceProvider->moduleIsValue());
        moduleRecord->setIsOtherFormOfFile(syntheticSourceProvider->moduleIsOtherFormOfFile());

        scope.release();
        promise->fulfill(vm, moduleRecord);
        return promise;
    }
    case SourceProviderSourceType::BunTranspiledModule: {
        RELEASE_AND_RETURN(scope, uncheckedDowncast<JSPromise>(JSValue::decode(Bun__analyzeTranspiledModule(globalObject, this, moduleKey, sourceCode, promise))));
    }
#endif
    default:
        break;
    }

    ParserError error;
    std::unique_ptr<ModuleProgramNode> moduleProgramNode = parseRootNode<ModuleProgramNode>(
        vm, sourceCode, ImplementationVisibility::Public, JSParserBuiltinMode::NotBuiltin,
        StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, SourceParseMode::ModuleAnalyzeMode, error);
    if (error.isValid()) {
        auto* errorObject = error.toErrorObject(globalObject, sourceCode);
        RETURN_IF_EXCEPTION(scope, promise->rejectWithCaughtException(vm, scope));
        auto* errorInstance = uncheckedDowncast<ErrorInstance>(errorObject);
        attachErrorInfo(globalObject, errorInstance, nullptr, moduleKey, ScriptFetchParameters::JavaScript, ModuleFailure::Kind::Evaluation);
        promise->reject(vm, errorInstance);
        RELEASE_AND_RETURN(scope, promise);
    }
    ASSERT(moduleProgramNode);

    ModuleAnalyzer moduleAnalyzer(globalObject, this, moduleKey, sourceCode, moduleProgramNode->features());
    RETURN_IF_EXCEPTION(scope, nullptr);

    auto result = moduleAnalyzer.analyze(*moduleProgramNode);
    if (!result) {
        auto [errorType, message] = WTF::move(result.error());
        auto* errorInstance = uncheckedDowncast<ErrorInstance>(createError(globalObject, errorType, message));
        attachErrorInfo(globalObject, errorInstance, nullptr, moduleKey, ScriptFetchParameters::JavaScript, ModuleFailure::Kind::Evaluation);
        promise->reject(vm, errorInstance);
        RELEASE_AND_RETURN(scope, promise);
    }

#if USE(BUN_JSC_ADDITIONS)
    size_t memoryCost = result.value()->sourceCode().memoryCost();
    vm.heap.reportExtraMemoryAllocated(result.value(), memoryCost);
    if (hasLiveExports) [[unlikely]]
        result.value()->setHasLiveExports();
#endif
    promise->fulfill(vm, result.value());
    RELEASE_AND_RETURN(scope, promise);
}

} // namespace JSC

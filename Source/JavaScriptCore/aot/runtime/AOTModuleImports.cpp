/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleLinkWalk.h"
#include "AOTModuleMicrotasks.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "MicrotaskQueueInlines.h"
#include "ScriptFetchParameters.h"

namespace JSC { namespace AOT {

static JSValue jobArgument(ModuleID module)
{
    return jsNumber(module.index());
}

static JSValue jobArgument(std::optional<int64_t> asyncEvaluationOrder)
{
    return asyncEvaluationOrder ? jsNumber(static_cast<double>(*asyncEvaluationOrder)) : jsUndefined();
}

static ModuleID moduleFromJobArgument(JSValue argument)
{
    return ModuleID(static_cast<uint32_t>(argument.asInt32()));
}

static std::optional<int64_t> asyncEvaluationOrderFromJobArgument(JSValue argument)
{
    if (argument.isUndefined())
        return std::nullopt;
    return static_cast<int64_t>(argument.asNumber());
}

static void queueImportJob(JSGlobalObject* realm, JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    constexpr uint8_t unusedPayload = 0;
    realm->queueMicrotask(realm->vm(), InternalMicrotask::CompiledModuleImport, unusedPayload, importPromise, jobArgument(module), jobArgument(referrerAsyncOrder));
}

// A synchronous module load runs the jobs it diverts in its own realm, so the realm of a job is the one its import promise was created in.
static Modules& modulesOfImport(JSPromise* importPromise)
{
    return *modulesOf(importPromise->realm());
}

std::optional<int64_t> Modules::asyncEvaluationOrder(ModuleID module) const
{
    ModuleRecord* record = recordIfExists(module);
    if (!record)
        return std::nullopt;
    AbstractModuleRecord::AsyncEvaluationOrder order = record->asyncEvaluationOrder();
    if (!order.hasOrder())
        return std::nullopt;
    return order.order();
}

JSPromise* Modules::import(ModuleID module, std::optional<ModuleID> referrer)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    if (!data(module).isUncompiledCode())
        RELEASE_AND_RETURN(scope, startImport(module, referrer ? asyncEvaluationOrder(*referrer) : std::nullopt));

    const Identifier& key = resolvedKey(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    RELEASE_AND_RETURN(scope, importUncompiled(key, *data(module).referrerModule(), nullptr));
}

JSPromise* Modules::importFromHost(ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    return startImport(module, referrerAsyncOrder);
}

JSPromise* Modules::startImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (data(module).reachesUncompiledCode() && status(module) == ModuleStatus::Unlinked)
        RELEASE_AND_RETURN(scope, importThroughLoader(module, referrerAsyncOrder));

    JSPromise* importPromise = JSPromise::create(vm, realm->promiseStructure());
    queueImportJob(realm, importPromise, module, referrerAsyncOrder);
    return importPromise;
}

void Modules::continueImport(JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (status(module) == ModuleStatus::Evaluating) {
        // The body of the module is on the stack below this job, so a job queued now would run, and queue itself again, before the body can go on.
        vm.whenIdle([realm = m_instance.globalObject, importPromise = Strong<JSPromise>(vm, importPromise), module, referrerAsyncOrder] {
            queueImportJob(realm, importPromise.get(), module, referrerAsyncOrder);
        });
        return;
    }

    std::unique_ptr<ModuleLinkWalk> linkWalk = std::exchange(m_linkWalkOfQueuedImport, nullptr);
    if (!linkWalk || linkWalk->root() != module)
        linkWalk = makeUnique<ModuleLinkWalk>(*this, module);
    bool didRunModule = loadRuntimeModules(*linkWalk);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    if (didRunModule) {
        m_linkWalkOfQueuedImport = WTF::move(linkWalk);
        queueImportJob(m_instance.globalObject, importPromise, module, referrerAsyncOrder);
        return;
    }

    JSPromise* evaluation = evaluate(module, referrerAsyncOrder, importPromise, linkWalk.get());
    RETURN_IF_EXCEPTION(scope, void());
    switch (evaluation->status()) {
    case JSPromise::Status::Pending:
        evaluation->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledModuleImportSettled, importPromise, jobArgument(module));
        return;
    case JSPromise::Status::Fulfilled:
        RELEASE_AND_RETURN(scope, resolveWithNamespace(importPromise, module));
    case JSPromise::Status::Rejected:
        evaluation->markAsHandled();
        importPromise->reject(vm, evaluation->result());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::didSettleImport(JSPromise* importPromise, ModuleID module, JSValue resolution, JSPromise::Status status)
{
    switch (status) {
    case JSPromise::Status::Fulfilled:
        resolveWithNamespace(importPromise, module);
        return;
    case JSPromise::Status::Rejected:
        importPromise->reject(*m_instance.vm, resolution);
        return;
    case JSPromise::Status::Pending:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::resolveWithNamespace(JSPromise* importPromise, ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    scope.release();
    importPromise->resolve(m_instance.globalObject, vm, namespaceObject);
}

JSPromise* Modules::importDynamic(JSValue specifier, JSValue options, ModuleID referrer)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    JSString* text = specifier.toString(realm);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    willLoadByName(realm, text, LoadByName::Import);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    Identifier key = loader()->resolve(realm, text, pathString(referrer), nullptr, true);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!*fetchParameters) {
        if (std::optional<ModuleID> found = m_table.findByPath(key.string())) {
            willAnswerWithModule(realm, *found, LoadByName::Import);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            RELEASE_AND_RETURN(scope, import(*found, referrer));
        }
    }
    RELEASE_AND_RETURN(scope, importUncompiled(key, referrer, WTF::move(*fetchParameters)));
}

JSPromise* Modules::importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&& fetchParameters)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSPromise* promise = loader()->requestImportModuleForKey(realm, key, moduleKey(referrer), WTF::move(fetchParameters), nullptr, false, asyncEvaluationOrder(referrer).value_or(-1));
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    return promise;
}

void runCompiledModuleImport(JSGlobalObject*, JSValue importPromise, JSValue module, JSValue referrerAsyncOrder)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    modulesOfImport(promise).continueImport(promise, moduleFromJobArgument(module), asyncEvaluationOrderFromJobArgument(referrerAsyncOrder));
}

void runCompiledModuleImportSettled(JSGlobalObject*, JSValue importPromise, JSValue resolution, JSValue module, JSPromise::Status status)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    modulesOfImport(promise).didSettleImport(promise, moduleFromJobArgument(module), resolution, status);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

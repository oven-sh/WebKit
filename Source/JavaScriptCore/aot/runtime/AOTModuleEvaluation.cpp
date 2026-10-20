/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTState.h"
#include "Error.h"
#include "Exception.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include <algorithm>
#include <memory>
#include <ranges>
#include <wtf/BitVector.h>
#include <wtf/Variant.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using EvaluationTarget = Variant<ModuleID, AbstractModuleRecord*>;

struct EvaluationFrame {
    ModuleID module;
    ModuleRecord* record { nullptr };
    unsigned dfsIndex { 0 };
    unsigned stackPosition { 0 };
    unsigned next { 0 };
    unsigned end { 0 };
    std::unique_ptr<Vector<EvaluationTarget>> evaluationList;
};

static bool isComponentEvaluated(const Modules& modules, ModuleID module)
{
    if (ModuleRecord* record = modules.recordIfExists(module))
        return record->isSCCEvaluated();
    return modules.status(module) == ModuleStatus::Evaluated;
}

static bool isOnStack(CyclicModuleRecord* record, const ModuleEvaluationStack& stack)
{
    if (auto* compiled = dynamicDowncast<ModuleRecord>(record))
        return compiled->isOnStack(stack);
    return stack.contains(record);
}

static void checkSafeToRecurse(JSGlobalObject* realm, ThrowScope& scope)
{
    if (!realm->vm().isSafeToRecurse()) [[unlikely]]
        throwRangeError(realm, scope, "Maximum call stack size exceeded"_s);
}

// Its status is the byte, its requests are those of the table, and nothing but its body is left to run when they are done.
static ALWAYS_INLINE bool needsOnlyFrame(const ModuleData& data)
{
    return data.kind == ModuleKind::SourceText && data.isSimple() && !data.hasDeferredRequest();
}

static void computeEvaluationList(Modules& modules, EvaluationFrame& frame)
{
    auto scope = DECLARE_THROW_SCOPE(*modules.instance().vm);
    const ModuleTable& table = modules.table();
    Vector<EvaluationTarget> evaluationList;
    BitVector listedModules(table.size());
    VisitedModuleRecords listedRecords;
    auto appendOnce = [&](ModuleID module) {
        if (listedModules.add(module.index()))
            evaluationList.append(module);
    };
    for (const ModuleRequestData& request : table.requests(frame.module)) {
        if (!request.isDeferred) {
            appendOnce(request.targetModule());
            continue;
        }
        OrderedHashSet<AbstractModuleRecord*> found;
        VisitedModuleRecords seen;
        modules.gatherAsynchronousTransitiveDependencies(request.targetModule(), found, seen);
        RETURN_IF_EXCEPTION(scope, void());
        for (AbstractModuleRecord* record : found) {
            if (auto* compiled = dynamicDowncast<ModuleRecord>(record))
                appendOnce(compiled->module());
            else if (listedRecords.add(record).isNewEntry)
                evaluationList.append(record);
        }
    }
    frame.next = 0;
    frame.end = evaluationList.size();
    frame.evaluationList = makeUnique<Vector<EvaluationTarget>>(WTF::move(evaluationList));
}

void Modules::gatherAsynchronousTransitiveDependencies(ModuleID start, OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    BitVector visited(m_table.size());
    Vector<ModuleID, 8> work { start };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (!visited.add(module.index()))
            continue;
        const ModuleData& moduleData = data(module);
        if (moduleData.isUncompiledCode()) {
            AbstractModuleRecord* record = uncompiledRecord(module);
            RETURN_IF_EXCEPTION(scope, void());
            checkSafeToRecurse(realm, scope);
            RETURN_IF_EXCEPTION(scope, void());
            record->gatherAsynchronousTransitiveDependencies(result, seen);
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        if (!moduleData.hasExportTable() || moduleData.isSynchronous())
            continue;
        if (status(module) == ModuleStatus::Evaluating || isComponentEvaluated(*this, module))
            continue;
        if (moduleData.hasTopLevelAwait()) {
            ModuleRecord* record = ensureRecord(module);
            record->retain();
            seen.add(record);
            result.add(record);
            continue;
        }
        for (const ModuleRequestData& request : m_table.requests(module) | std::views::reverse)
            work.append(request.targetModule());
    }
}

bool Modules::readyForSyncExecution(ModuleID start, VisitedModuleRecords& seen)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    BitVector visited(m_table.size());
    Vector<ModuleID, 8> work { start };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (!visited.add(module.index()))
            continue;
        const ModuleData& moduleData = data(module);
        if (moduleData.isUncompiledCode()) {
            AbstractModuleRecord* record = uncompiledRecord(module);
            RETURN_IF_EXCEPTION(scope, false);
            checkSafeToRecurse(realm, scope);
            RETURN_IF_EXCEPTION(scope, false);
            bool isReady = record->readyForSyncExecution(seen);
            RETURN_IF_EXCEPTION(scope, false);
            if (!isReady)
                return false;
            continue;
        }
        if (!moduleData.hasExportTable() || isComponentEvaluated(*this, module))
            continue;
        switch (status(module)) {
        case ModuleStatus::Evaluating:
        case ModuleStatus::EvaluatingAsync:
            return false;
        case ModuleStatus::Unlinked:
        case ModuleStatus::Linking:
        case ModuleStatus::Linked:
        case ModuleStatus::Evaluated:
        case ModuleStatus::Failed:
            break;
        }
        if (moduleData.hasTopLevelAwait())
            return false;
        for (const ModuleRequestData& request : m_table.requests(module) | std::views::reverse)
            work.append(request.targetModule());
    }
    return true;
}

bool Modules::enterModule(ModuleID module, EvaluationContext& context, EvaluationFrames& frames)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    if (!data(module).hasExportTable())
        return false;

    if (status(module) == ModuleStatus::Unlinked) {
        link(module);
        RETURN_IF_EXCEPTION(scope, false);
    }
    switch (status(module)) {
    case ModuleStatus::Linking:
    case ModuleStatus::Evaluating:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Evaluated:
        return false;
    case ModuleStatus::Failed:
        scope.release();
        throwEvaluationError(module);
        return false;
    case ModuleStatus::Unlinked:
        RELEASE_ASSERT_NOT_REACHED();
    case ModuleStatus::Linked:
        break;
    }

    const ModuleData& moduleData = data(module);
    setStatus(module, ModuleStatus::Evaluating);
    EvaluationFrame frame { module };
    frame.dfsIndex = context.index++;
    frame.stackPosition = static_cast<unsigned>(context.stack.size());
    frame.next = moduleData.firstRequest;
    frame.end = m_table.endOfRequests(module);
    ModuleRecord* record = recordIfExists(module);
    if (!record && !moduleData.isSimple())
        record = ensureRecord(module);
    if (record)
        pushRecord(frame, record, context);
    frames.append(WTF::move(frame));
    if (moduleData.hasDeferredRequest()) [[unlikely]] {
        scope.release();
        computeEvaluationList(*this, frames.last());
    }
    return true;
}

void Modules::pushRecord(EvaluationFrame& frame, ModuleRecord* record, EvaluationContext& context)
{
    RELEASE_ASSERT(frame.stackPosition <= context.stack.size());
    frame.record = record;
    record->setStatus(CyclicModuleRecord::Status::Evaluating);
    record->setDFSIndex(frame.dfsIndex);
    record->setDFSAncestorIndex(frame.dfsIndex);
    record->setPendingAsyncDependencies(0);
    record->setEvaluationStack(&context.stack);
    AbstractModuleRecord* entry = record;
    context.stack.insert(frame.stackPosition, entry);
}

ModuleRecord* Modules::ensureFrameRecord(EvaluationFrame& frame, EvaluationContext& context)
{
    if (!frame.record)
        pushRecord(frame, ensureRecord(frame.module), context);
    return frame.record;
}

// An entry that was on the stack when a frame entered stays until that frame has left, so a recorded position moves only by what is inserted below it.
void Modules::materializeFrames(EvaluationFrames& frames, EvaluationContext& context)
{
    unsigned inserted = 0;
    for (EvaluationFrame& frame : frames) {
        frame.stackPosition += inserted;
        if (frame.record)
            continue;
        pushRecord(frame, ensureRecord(frame.module), context);
        ++inserted;
    }
}

void Modules::evaluateUncompiledRecord(AbstractModuleRecord* required, EvaluationContext& context, EvaluationFrames& frames)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    auto* cyclic = dynamicDowncast<CyclicModuleRecord>(required);
    if (cyclic && cyclic->status() == CyclicModuleRecord::Status::Unlinked) {
        required->link(realm, nullptr);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (cyclic && cyclic->status() == CyclicModuleRecord::Status::Linked)
        materializeFrames(frames, context);

    checkSafeToRecurse(realm, scope);
    RETURN_IF_EXCEPTION(scope, void());
    unsigned index = required->innerModuleEvaluation(realm, context.stack, context.index, context.referrerAsyncOrder.value_or(-1), context.importPromise);
    RETURN_IF_EXCEPTION(scope, void());
    context.index = index;
    if (cyclic)
        RELEASE_AND_RETURN(scope, didEvaluateRequiredModule(frames.last(), cyclic, context));
}

void Modules::didEvaluateRequiredModule(EvaluationFrame& importer, CyclicModuleRecord* required, EvaluationContext& context)
{
    if (!required)
        return;

    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (required->status() == CyclicModuleRecord::Status::Evaluating) {
        if (!isOnStack(required, context.stack))
            return;
        ModuleRecord* record = ensureFrameRecord(importer, context);
        record->setDFSAncestorIndex(std::min(record->dfsAncestorIndex(), required->dfsAncestorIndex()));
    } else {
        if (CyclicModuleRecord* root = required->cycleRoot())
            required = root;
        if (JSValue error = required->evaluationError()) {
            throwException(m_instance.globalObject, scope, error);
            return;
        }
    }

    AbstractModuleRecord::AsyncEvaluationOrder order = required->asyncEvaluationOrder();
    if (!order.hasOrder() || order.order() == context.referrerAsyncOrder)
        return;
    if (context.importPromise && AbstractModuleRecord::importPromiseGatesAsyncDependency(context.importPromise, required))
        return;

    ModuleRecord* record = ensureFrameRecord(importer, context);
    record->setPendingAsyncDependencies(*record->pendingAsyncDependencies() + 1);
    required->appendAsyncParentModule(vm, record);
}

void Modules::leaveModule(EvaluationFrame& frame, EvaluationContext& context)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    ModuleRecord* record = frame.record;
    if (record && (*record->pendingAsyncDependencies() > 0 || data(frame.module).hasTopLevelAwait())) {
        record->setAsyncEvaluationOrder(vm.incrementModuleAsyncEvaluationCount());
        if (!*record->pendingAsyncDependencies())
            record->executeAsync(m_instance.globalObject);
    } else
        callBody(frame.module, jsUndefined(), ArgList());
    RETURN_IF_EXCEPTION(scope, void());

    if (!record) {
        setStatus(frame.module, ModuleStatus::Evaluated);
        return;
    }
    if (record->dfsAncestorIndex() != record->dfsIndex())
        return;

    Vector<ModuleRecord*, 8> recordsToRetire;
    bool canRetireComponent = true;
    for (;;) {
        AbstractModuleRecord* member = context.stack.takeLast();
        auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(member);
        bool isEvaluated = cyclic->asyncEvaluationOrder().isUnset();
        cyclic->setStatus(isEvaluated ? CyclicModuleRecord::Status::Evaluated : CyclicModuleRecord::Status::EvaluatingAsync);
        member->setCycleRoot(vm, record);
        auto* compiled = dynamicDowncast<ModuleRecord>(member);
        if (!compiled || !isEvaluated)
            canRetireComponent = false;
        else if (!compiled->isRetained())
            recordsToRetire.append(compiled);
        if (member == record)
            break;
    }
    if (!canRetireComponent)
        return;
    for (ModuleRecord* member : recordsToRetire)
        retireRecord(member);
}

void Modules::innerModuleEvaluation(ModuleID root, EvaluationContext& context)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!data(root).hasExportTable()) {
        ModuleRecord* record = recordIfExists(root);
        if (!record)
            return;
        if (record->evaluationError())
            RELEASE_AND_RETURN(scope, throwEvaluationError(root));
        if (record->status() != CyclicModuleRecord::Status::Linked)
            return;
        // The body ran at link. If require.cache dropped the module since then, only require() runs it again: the record is finished, and the status moves back to the status byte.
        {
            Locker locker { m_state.caches->lock() };
            m_records.remove(root);
        }
        setStoredStatus(root, StoredModuleStatus::Linked);
        record->setStatus(CyclicModuleRecord::Status::Evaluated);
        record->setCycleRoot(vm, record);
        return;
    }

    std::span<const ModuleRequestData> requests = m_table.allRequests();
    EvaluationFrames frames;
    enterModule(root, context, frames);
    while (!scope.exception() && !frames.isEmpty()) {
        if (EvaluationFrame& frame = frames.last(); frame.next != frame.end) {
            EvaluationTarget target = ModuleID();
            if (frame.evaluationList) [[unlikely]]
                target = frame.evaluationList->at(frame.next++);
            else {
                ModuleID required = requests[frame.next++].targetModule();
                StoredModuleStatus stored = storedStatus(required);
                if (stored == StoredModuleStatus::Evaluated)
                    continue;
                const ModuleData& requiredData = data(required);
                if (needsOnlyFrame(requiredData) && (stored == StoredModuleStatus::Linked || (stored == StoredModuleStatus::Unlinked && !requiredData.needsLink()))) {
                    setStoredStatus(required, StoredModuleStatus::Evaluating);
                    frames.append(EvaluationFrame { required, nullptr, context.index++, static_cast<unsigned>(context.stack.size()), requiredData.firstRequest, m_table.endOfRequests(required), nullptr });
                    continue;
                }
                target = required;
            }
            const ModuleID* module = std::get_if<ModuleID>(&target);
            if (module && !data(*module).isUncompiledCode()) {
                if (!enterModule(*module, context, frames) && !scope.exception())
                    didEvaluateRequiredModule(frames.last(), recordIfExists(*module), context);
                continue;
            }
            AbstractModuleRecord* record = module ? uncompiledRecord(*module) : std::get<AbstractModuleRecord*>(target);
            if (!scope.exception())
                evaluateUncompiledRecord(record, context, frames);
            continue;
        }
        leaveModule(frames.last(), context);
        if (scope.exception())
            break;
        ModuleID evaluated = frames.takeLast().module;
        if (!frames.isEmpty())
            didEvaluateRequiredModule(frames.last(), recordIfExists(evaluated), context);
    }

    if (Exception* exception = scope.exception())
        failFrames(exception->value(), frames);
}

void Modules::failFrames(JSValue error, const EvaluationFrames& frames)
{
    for (const EvaluationFrame& frame : frames) {
        if (!frame.record)
            setFailed(frame.module, error);
    }
}

void Modules::failStack(JSValue error, ModuleEvaluationStack& stack)
{
    VM& vm = *m_instance.vm;
    for (AbstractModuleRecord* member : stack) {
        auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(member);
        cyclic->setStatus(CyclicModuleRecord::Status::Evaluated);
        cyclic->setEvaluationError(vm, error);
        if (auto* compiled = dynamicDowncast<ModuleRecord>(member))
            compiled->retain();
    }
    stack.clear();
}

void Modules::evaluateWithOwnStack(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    ModuleEvaluationStack stack;
    EvaluationContext context { stack };
    innerModuleEvaluation(root, context);
    if (Exception* exception = scope.exception()) {
        failStack(exception->value(), stack);
        return;
    }
    ASSERT(stack.isEmpty());
}

JSPromise* Modules::evaluate(ModuleID root, std::optional<int64_t> referrerAsyncOrder, JSPromise* importPromise, const ModuleLinkWalk* linkWalk)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& moduleData = data(root);
    ASSERT(status(root) != ModuleStatus::Evaluating);

    if (status(root) == ModuleStatus::Failed) {
        throwEvaluationError(root);
        return rejectedPromiseWithPendingException(realm, scope);
    }
    if (linkWalk)
        link(*linkWalk);
    else
        link(root);
    if (scope.exception())
        return rejectedPromiseWithPendingException(realm, scope);
    if (!moduleData.hasExportTable())
        RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(realm, jsUndefined()));

    if (moduleData.isSimple() && moduleData.isSynchronous() && !recordIfExists(root)) {
        if (status(root) != ModuleStatus::Evaluated) {
            evaluateWithOwnStack(root);
            if (scope.exception())
                return rejectedPromiseWithPendingException(realm, scope);
        }
        RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(realm, jsUndefined()));
    }
    RELEASE_AND_RETURN(scope, ensureRecord(root)->evaluate(realm, referrerAsyncOrder.value_or(-1), importPromise));
}

void Modules::evaluateSynchronously(ModuleID module, SynchronousEvaluationReason reason)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& moduleData = data(module);

    auto throwAsyncModuleError = [&] {
        throwTypeError(realm, scope, makeString("require() async module \""_s, m_table.pathText(module), "\" is unsupported. use \"await import()\" instead."_s));
    };

    if (!moduleData.hasExportTable()) {
        scope.release();
        execute(module);
        return;
    }

    switch (reason) {
    case SynchronousEvaluationReason::Require:
        break;
    case SynchronousEvaluationReason::DeferredNamespace: {
        VisitedModuleRecords seen;
        bool isReady = readyForSyncExecution(module, seen);
        RETURN_IF_EXCEPTION(scope, void());
        if (!isReady) {
            throwTypeError(realm, scope, "Unable to synchronously evaluate deferred module"_s);
            return;
        }
        break;
    }
    }

    bool isUnlinked = false;
    switch (status(module)) {
    case ModuleStatus::Evaluated:
    case ModuleStatus::Linking:
        return;
    case ModuleStatus::Failed:
        RELEASE_AND_RETURN(scope, throwEvaluationError(module));
    case ModuleStatus::Evaluating:
        if (moduleData.hasTopLevelAwait())
            throwAsyncModuleError();
        return;
    case ModuleStatus::EvaluatingAsync:
        throwAsyncModuleError();
        return;
    case ModuleStatus::Unlinked:
        isUnlinked = true;
        break;
    case ModuleStatus::Linked:
        break;
    }

    JSPromise* promise = nullptr;
    if (isUnlinked && moduleData.reachesUncompiledCode())
        promise = evaluateThroughLoaderSynchronously(module);
    else {
        link(module);
        RETURN_IF_EXCEPTION(scope, void());
        if (moduleData.isSynchronous())
            RELEASE_AND_RETURN(scope, evaluateWithOwnStack(module));
        promise = evaluate(module, std::nullopt, nullptr);
    }
    RETURN_IF_EXCEPTION(scope, void());

    switch (promise->status()) {
    case JSPromise::Status::Fulfilled:
        return;
    case JSPromise::Status::Rejected:
        promise->markAsHandled();
        throwException(realm, scope, promise->result());
        return;
    case JSPromise::Status::Pending:
        promise->markAsHandled();
        if (!moduleData.hasTopLevelAwait() && (status(module) == ModuleStatus::Evaluating || status(module) == ModuleStatus::Evaluated))
            return;
        throwAsyncModuleError();
        return;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

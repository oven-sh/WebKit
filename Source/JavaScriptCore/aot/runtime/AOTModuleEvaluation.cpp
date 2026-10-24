/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTProgramStatics.h"
#include "AOTState.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTVector.h"
#include "Error.h"
#include "Exception.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>
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

struct EvaluationInProgress {
    const Modules::EvaluationFrames& frames;
    const EvaluationInProgress* outer;
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
        throwException(realm, scope, createModuleGraphTooDeepError(realm));
}

// Its status is the byte, its requests are those of the table, and nothing but its body is left to run when they are done.
static ALWAYS_INLINE bool needsOnlyFrame(const ModuleData& data)
{
    return data.kind == ModuleKind::SourceText && data.isSimple() && !data.hasDeferredRequest();
}

// The lists of evaluation requests leave out a module that runs nothing itself: what it requests stands in its place, so the evaluation of an importer stores no status for it.
static bool isPassedOverByEvaluation(const ModuleData& data)
{
    if (!data.hasExportTable())
        return false;
    return !data.needsEvaluation() || (data.isSimple() && data.isSynchronous() && !data.bodyFunction() && !data.hasDeferredRequest());
}

// An evaluation that goes by all requests comes from the importer to the required module through modules that are passed over, on the first such way in the order of the requests.
static bool isPassedOverBetween(const ModuleTable& table, ModuleID importer, ModuleID required, ModuleID candidate)
{
    struct Step {
        ModuleID module;
        uint32_t nextRequest;
        uint32_t endOfRequests;
    };

    auto stepInto = [&](ModuleID module) {
        return Step { module, table.data(module).firstRequest, table.endOfRequests(module) };
    };
    std::span<const ModuleRequestData> requests = table.allRequests();
    BitVector visited(table.size());
    Vector<Step, 16> way { stepInto(importer) };
    while (!way.isEmpty()) {
        Step& step = way.last();
        if (step.nextRequest == step.endOfRequests) {
            way.removeLast();
            continue;
        }
        const ModuleRequestData& request = requests[step.nextRequest++];
        if (request.isRequire)
            continue;
        ModuleID target = request.targetModule();
        if (target == required) {
            return way.containsIf([&](const Step& passed) {
                return passed.module == candidate;
            });
        }
        const ModuleData& targetData = table.data(target);
        if (!targetData.needsEvaluation() || !isPassedOverByEvaluation(targetData) || visited.quickSet(target.index()))
            continue;
        way.append(stepInto(target));
    }
    return false;
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
    for (const ModuleRequestData& request : table.imports(frame.module)) {
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
        for (const ModuleRequestData& request : m_table.imports(module) | std::views::reverse)
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
        for (const ModuleRequestData& request : m_table.imports(module) | std::views::reverse)
            work.append(request.targetModule());
    }
    return true;
}

bool Modules::enterModule(ModuleID module, EvaluationContext& context, EvaluationFrames& frames)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& moduleData = data(module);

    if (moduleData.kind == ModuleKind::CommonJs) {
        scope.release();
        evaluateCommonJS(module);
        return false;
    }
    // A module of the library ran when its importer was linked.
    if (!moduleData.hasExportTable())
        return false;

    ModuleStatus moduleStatus = status(module);
    if (moduleStatus == ModuleStatus::Unlinked) {
        // What a linked module requests was linked with it. No status says so for a module without work in a link.
        if (frames.isEmpty()) {
            link(module);
            RETURN_IF_EXCEPTION(scope, false);
            moduleStatus = status(module);
        } else
            moduleStatus = ModuleStatus::Linked;
    }
    switch (moduleStatus) {
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

    ModuleRecord* record = recordIfExists(module);
    if (!record && moduleData.evaluatesItself()) {
        scope.release();
        evaluateByCallingBody(module);
        return false;
    }
    if (!record && !moduleData.needsEvaluation()) {
        setStoredStatus(module, StoredModuleStatus::Evaluated);
        return false;
    }

    setStatus(module, ModuleStatus::Evaluating);
    EvaluationFrame frame { module };
    frame.dfsIndex = context.index++;
    frame.stackPosition = static_cast<unsigned>(context.stack.size());
    if (!moduleData.evaluatesItself()) {
        frame.next = moduleData.firstEvaluationRequest;
        frame.end = m_table.endOfEvaluationRequests(module);
    }
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
    } else if (data(frame.module).evaluatesItself())
        evaluateByCallingBody(frame.module);
    else
        callBodyOfModuleEnteredByLoader(frame.module);
    RETURN_IF_EXCEPTION(scope, void());
    if (data(frame.module).capturesExportsBehindEvaluation()) [[unlikely]] {
        captureExports(frame.module);
        RETURN_IF_EXCEPTION(scope, void());
    }

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

    if (data(root).kind == ModuleKind::CommonJs)
        RELEASE_AND_RETURN(scope, evaluateCommonJS(root));

    if (!data(root).hasExportTable()) {
        ModuleRecord* record = recordIfExists(root);
        if (!record)
            return;
        if (record->evaluationError())
            RELEASE_AND_RETURN(scope, throwEvaluationError(root));
        if (record->status() != CyclicModuleRecord::Status::Linked)
            return;
        // The body ran at link: the record is finished, and the status moves back to the status byte.
        {
            Locker locker { m_state.caches->lock() };
            m_records.remove(root);
        }
        setStoredStatus(root, StoredModuleStatus::Linked);
        record->setStatus(CyclicModuleRecord::Status::Evaluated);
        record->setCycleRoot(vm, record);
        return;
    }

    ModuleNumbers requests = m_table.allEvaluationRequests();
    EvaluationFrames frames;
    EvaluationInProgress evaluation { frames, m_innermostEvaluation };
    SetForScope innermostEvaluation(m_innermostEvaluation, &evaluation);
    enterModule(root, context, frames);
    while (!scope.exception() && !frames.isEmpty()) {
        if (EvaluationFrame& frame = frames.last(); frame.next != frame.end) {
            EvaluationTarget target = ModuleID();
            if (frame.evaluationList) [[unlikely]]
                target = frame.evaluationList->at(frame.next++);
            else {
                ModuleID required(requests[frame.next++]);
                StoredModuleStatus stored = storedStatus(required);
                if (stored == StoredModuleStatus::Evaluated)
                    continue;
                const ModuleData& requiredData = data(required);
                // It was linked with its importer, whether or not a status was stored for it.
                if (requiredData.evaluatesItself() && (stored == StoredModuleStatus::Linked || stored == StoredModuleStatus::Unlinked)) {
                    evaluateByCallingBody(required);
                    continue;
                }
                if (needsOnlyFrame(requiredData) && (stored == StoredModuleStatus::Linked || stored == StoredModuleStatus::Unlinked)) {
                    setStoredStatus(required, StoredModuleStatus::Evaluating);
                    frames.append(EvaluationFrame { required, nullptr, context.index++, static_cast<unsigned>(context.stack.size()), requiredData.firstEvaluationRequest, m_table.endOfEvaluationRequests(required), nullptr });
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

// The body stores the status of the module in the table. The status of a module with a record is in the record, and whoever gave it the record goes by that: for the time of the call the table has it.
void Modules::evaluateByCallingBody(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ASSERT(data(module).evaluatesItself());

    bool isInRecord = storedStatus(module) == StoredModuleStatus::InRecord;
    if (isInRecord)
        setStoredStatus(module, StoredModuleStatus::Linked);
    uint32_t outerMark = m_instance.modules.evaluatingMark;
    uint8_t mark = static_cast<uint8_t>(std::min<uint32_t>(outerMark + 1, UINT8_MAX));
    m_instance.modules.evaluatingMark = mark;
    callBody(module);
    m_instance.modules.evaluatingMark = outerMark;
    if (isInRecord)
        setStoredStatus(module, StoredModuleStatus::InRecord);
    if (Exception* exception = scope.exception()) [[unlikely]]
        failModulesLeftEvaluating(mark, exception->value());
}

JSValue Modules::callBodyOfModuleEnteredByLoader(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    uint32_t outerMark = m_instance.modules.evaluatingMark;
    uint8_t mark = static_cast<uint8_t>(std::min<uint32_t>(outerMark + 1, UINT8_MAX));
    m_instance.modules.evaluatingMark = mark;
    JSValue result = callBody(module);
    m_instance.modules.evaluatingMark = outerMark;
    if (Exception* exception = scope.exception()) [[unlikely]]
        failModulesLeftEvaluating(mark, exception->value());
    return result;
}

// No body runs again. Where the module keeps the error, whoever asks for the module later gets it; where it does not, they get what the module had exported when it threw.
void Modules::didThrowInBody(ModuleID module, JSValue error)
{
    if (data(module).keepsError()) {
        setFailed(module, error);
        return;
    }
    setStatus(module, ModuleStatus::Evaluated);
}

// Compiled code comes here from the handler around the evaluation of a module, with what it has just caught and is about to throw again.
void Modules::abandonEvaluation(ModuleID module, JSValue error)
{
    if (storedStatus(module) < StoredModuleStatus::Evaluating)
        return;
    didThrowInBody(module, error);
    failEvaluatedModulesOfUnfinishedCycles(std::span<const ModuleID>(&module, 1), error);
}

// What was being evaluated when a body threw has failed with it, and whoever asks for one of these modules later gets the same error.
void Modules::failModulesLeftEvaluating(uint8_t mark, JSValue error)
{
    Vector<ModuleID, 16> failedModules;
    for (unsigned index = 0; index < m_storedStatuses.size(); ++index) {
        if (m_storedStatuses[index] == mark)
            failedModules.append(ModuleID(index));
    }
    for (ModuleID module : failedModules)
        didThrowInBody(module, error);
    failEvaluatedModulesOfUnfinishedCycles(failedModules.span(), error);
}

// A module of a cycle of imports is evaluated for good when the first module of the cycle that was entered is. A require() makes no such cycle: what it has evaluated stays evaluated.
void Modules::failEvaluatedModulesOfUnfinishedCycles(std::span<const ModuleID> failedModules, JSValue error)
{
    bool hasCycle = std::ranges::any_of(failedModules, [&](ModuleID module) {
        return data(module).hasExportTable() && !data(module).isSimple();
    });
    if (!hasCycle)
        return;

    struct ImportTargets {
        std::span<const ModuleRequestData> requests;
        uint32_t importer;

        size_t size() const { return requests.size(); }
        // A require() leads nowhere.
        uint32_t operator[](size_t index) const { return requests[index].isRequire ? importer : requests[index].target(); }
    };
    Typed::StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(m_table.size(), [&](unsigned index) {
        return ImportTargets { m_table.requests(ModuleID(index)), index };
    });
    BitVector unfinishedComponents(components.numberOfComponents);
    for (ModuleID module : failedModules) {
        if (data(module).hasExportTable())
            unfinishedComponents.quickSet(components.componentOf[module.index()]);
    }
    for (unsigned index = 0; index < m_table.size(); ++index) {
        ModuleID module(index);
        if (unfinishedComponents.quickGet(components.componentOf[index]) && data(module).evaluatesItself() && storedStatus(module) == StoredModuleStatus::Evaluated)
            setFailed(module, error);
    }
}

// Compiled code comes here for a module whose status is in a record: one that failed, one that JavaScriptCore's loader knows, and one whose exports are captured behind its evaluation.
bool Modules::beginEvaluationOfModuleInRecord(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    switch (status(module)) {
    case ModuleStatus::Failed:
        scope.release();
        throwEvaluationError(module);
        return false;
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linked:
        scope.release();
        evaluateWithOwnStack(module);
        return false;
    case ModuleStatus::Linking:
    case ModuleStatus::Evaluating:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Evaluated:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    if (moduleData.kind == ModuleKind::CommonJs) {
        evaluateCommonJS(root);
        if (scope.exception())
            return rejectedPromiseWithPendingException(realm, scope);
    }
    if (!moduleData.hasExportTable())
        RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(realm, jsUndefined()));

    if (!moduleData.needsEvaluation() && !recordIfExists(root)) {
        setStoredStatus(root, StoredModuleStatus::Evaluated);
        RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(realm, jsUndefined()));
    }

    if (((moduleData.isSimple() && moduleData.isSynchronous()) || moduleData.evaluatesItself()) && !recordIfExists(root)) {
        if (status(root) != ModuleStatus::Evaluated) {
            evaluateWithOwnStack(root);
            if (scope.exception())
                return rejectedPromiseWithPendingException(realm, scope);
        }
        RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(realm, jsUndefined()));
    }
    RELEASE_AND_RETURN(scope, ensureRecord(root)->evaluate(realm, referrerAsyncOrder.value_or(-1), importPromise));
}

// To evaluate such a module is to evaluate what stands in its place in the lists of its importers.
void Modules::recordEvaluationIfNothingIsLeftToRun(ModuleID module)
{
    ASSERT(status(module) == ModuleStatus::Linked);
    if (!isPassedOverByEvaluation(data(module)))
        return;
    for (uint32_t required : m_table.evaluationRequests(module)) {
        if (status(ModuleID(required)) != ModuleStatus::Evaluated)
            return;
    }
    setStatus(module, ModuleStatus::Evaluated);
}

// While a body runs, the modules between it and the root of its evaluation are being evaluated, and no status says so for one that is passed over.
bool Modules::isPassedOverByEvaluationInProgress(ModuleID module) const
{
    if (!m_innermostEvaluation || !isPassedOverByEvaluation(data(module)))
        return false;
    // What such a module leads to is in its own list.
    ModuleNumbers requests = m_table.evaluationRequests(module);
    for (const EvaluationInProgress* evaluation = m_innermostEvaluation; evaluation; evaluation = evaluation->outer) {
        const EvaluationFrames& frames = evaluation->frames;
        for (size_t index = 1; index < frames.size(); ++index) {
            const EvaluationFrame& importer = frames[index - 1];
            ModuleID required = frames[index].module;
            if (importer.evaluationList || !requests.contains(required.index()))
                continue;
            if (isPassedOverBetween(m_table, importer.module, required, module))
                return true;
        }
    }
    return false;
}

// A CommonJS file that is imported runs in front of its importers, which were linked with it. An importer that the file requires binds what the file has exported by then, as under `bun run`.
void Modules::captureExportsOfFilesBeingEvaluated(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    Vector<ModuleID, 4> filesBeingEvaluated;
    for (const EvaluationInProgress* evaluation = m_innermostEvaluation; evaluation; evaluation = evaluation->outer) {
        for (const EvaluationFrame& frame : evaluation->frames) {
            if (data(frame.module).capturesExportsBehindEvaluation() && !didCaptureExports(frame.module))
                filesBeingEvaluated.append(frame.module);
        }
    }
    if (filesBeingEvaluated.isEmpty()) [[likely]]
        return;

    struct Importer {
        std::span<const ModuleRequestData> requests;
        size_t next { 0 };
    };
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<Importer, 16> path;
    path.append(Importer { m_table.requests(root) });
    while (!path.isEmpty()) {
        Importer& importer = path.last();
        if (importer.next == importer.requests.size()) {
            path.removeLast();
            continue;
        }
        const ModuleRequestData& request = importer.requests[importer.next++];
        ModuleID target = request.targetModule();
        if (request.isRequire || request.isDeferred || seen.quickSet(target.index()))
            continue;
        if (filesBeingEvaluated.contains(target)) {
            captureExports(target);
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        // Only what has not begun runs now.
        if (status(target) == ModuleStatus::Unlinked || status(target) == ModuleStatus::Linked)
            path.append(Importer { m_table.requests(target) });
    }
}

void Modules::evaluateSynchronously(ModuleID module, SynchronousEvaluationReason reason)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& moduleData = data(module);

    auto throwAsyncModuleError = [&] {
        throwException(realm, scope, host().createAsyncModuleRequiredError(realm, m_table.pathText(module)));
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
            throwException(realm, scope, createDeferredModuleNotReadyError(realm));
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
        if (moduleData.hasTopLevelAwait()) {
            throwAsyncModuleError();
            return;
        }
        RELEASE_AND_RETURN(scope, captureExportsOfFilesBeingEvaluated(module));
    case ModuleStatus::EvaluatingAsync:
        throwAsyncModuleError();
        return;
    case ModuleStatus::Unlinked:
        isUnlinked = true;
        break;
    case ModuleStatus::Linked:
        captureExportsOfFilesBeingEvaluated(module);
        RETURN_IF_EXCEPTION(scope, void());
        break;
    }
    if (isPassedOverByEvaluationInProgress(module))
        return;

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

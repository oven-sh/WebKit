/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTCompiledCall.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTObjectModel.h"
#include "AOTProgramStatics.h"
#include "AOTState.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTStubEntrypoints.h"
#include "ExceptionHelpers.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "ObjectConstructor.h"
#include "VMInlines.h"
#include <wtf/Scope.h>
#include <wtf/StackPointer.h>
#include <wtf/Variant.h>

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

using EvaluationFrames = Vector<EvaluationFrame, 16>;

struct EvaluationInProgress {
    const EvaluationFrames& frames;
    const EvaluationInProgress* outer;
};

enum class ImportWalkStep : uint8_t { Skip, Enter, Stop };

static constexpr size_t stackNeededToStartRuntimeModule = 128 * KB;
static_assert(BuiltinCells::stackKeptFromProgram >= 2 * stackNeededToStartRuntimeModule, "the first use of a built-in cell loads a module, which loads others");

static bool isComponentEvaluated(const Modules& modules, ModuleID module)
{
    if (ModuleRecord* record = modules.recordIfExists(module))
        return record->isSCCEvaluated();
    return modules.status(module) == ModuleStatus::Evaluated;
}

static void checkSafeToRecurse(JSGlobalObject* realm, ThrowScope& scope)
{
    if (!realm->vm().isSafeToRecurse()) [[unlikely]]
        throwException(realm, scope, createModuleGraphTooDeepError(realm));
}

// The lists of evaluation requests leave out a module that runs nothing itself: what it requests stands in its place, so the evaluation of an importer stores no status for it.
static bool isPassedOverByEvaluation(const ModuleData& data)
{
    return data.hasExportTable() && (!data.needsEvaluation() || (data.isSimple() && data.isSynchronous() && !data.bodyFunction() && !data.hasDeferredRequest()));
}

// An evaluation that goes by all requests comes from the importer to the required module through modules that are passed over, on the first such way in the order of the requests.
static bool isPassedOverBetween(const ModuleTable& table, ModuleID importer, ModuleID required, ModuleID candidate)
{
    struct Step {
        ModuleID module;
        uint32_t nextRequest;
    };
    BitVector visited(table.size());
    Vector<Step, 16> way { Step { importer, table.data(importer).firstRequest } };
    while (!way.isEmpty()) {
        Step& step = way.last();
        if (step.nextRequest == table.endOfRequests(step.module)) {
            way.removeLast();
            continue;
        }
        const ModuleRequestData& request = table.allRequests()[step.nextRequest++];
        if (request.isRequire)
            continue;
        ModuleID target = request.targetModule();
        if (target == required) {
            return way.containsIf([&](const Step& passed) {
                return passed.module == candidate;
            });
        }
        const ModuleData& targetData = table.data(target);
        if (targetData.needsEvaluation() && isPassedOverByEvaluation(targetData) && !visited.quickSet(target.index()))
            way.append(Step { target, targetData.firstRequest });
    }
    return false;
}

template<typename UncompiledFunctor, typename CompiledFunctor>
static bool walkImports(Modules& modules, ModuleID start, const UncompiledFunctor& visitUncompiled, const CompiledFunctor& visitCompiled)
{
    JSGlobalObject* realm = modules.instance().globalObject;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());

    BitVector visited(modules.table().size());
    Vector<ModuleID, 8> work { start };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (!visited.add(module.index()))
            continue;
        const ModuleData& moduleData = modules.data(module);
        ImportWalkStep step = ImportWalkStep::Skip;
        if (moduleData.isUncompiledCode()) {
            AbstractModuleRecord* record = modules.uncompiledRecord(module);
            RETURN_IF_EXCEPTION(scope, false);
            checkSafeToRecurse(realm, scope);
            RETURN_IF_EXCEPTION(scope, false);
            step = visitUncompiled(record);
            RETURN_IF_EXCEPTION(scope, false);
        } else if (moduleData.hasExportTable())
            step = visitCompiled(module, moduleData);
        if (step == ImportWalkStep::Stop)
            return false;
        if (step == ImportWalkStep::Skip)
            continue;
        for (const ModuleRequestData& request : modules.table().imports(module) | std::views::reverse)
            work.append(request.targetModule());
    }
    return true;
}

void Modules::gatherAsynchronousTransitiveDependencies(ModuleID start, OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen)
{
    walkImports(*this, start, [&](AbstractModuleRecord* record) {
        record->gatherAsynchronousTransitiveDependencies(result, seen);
        return ImportWalkStep::Skip;
    }, [&](ModuleID module, const ModuleData& moduleData) {
        if (moduleData.isSynchronous() || status(module) == ModuleStatus::Evaluating || isComponentEvaluated(*this, module))
            return ImportWalkStep::Skip;
        if (!moduleData.hasTopLevelAwait())
            return ImportWalkStep::Enter;
        ModuleRecord* record = ensureRecord(module);
        record->retain();
        seen.add(record);
        result.add(record);
        return ImportWalkStep::Skip;
    });
}

bool Modules::readyForSyncExecution(ModuleID start, VisitedModuleRecords& seen)
{
    return walkImports(*this, start, [&](AbstractModuleRecord* record) {
        return record->readyForSyncExecution(seen) ? ImportWalkStep::Skip : ImportWalkStep::Stop;
    }, [&](ModuleID module, const ModuleData& moduleData) {
        if (isComponentEvaluated(*this, module))
            return ImportWalkStep::Skip;
        ModuleStatus moduleStatus = status(module);
        bool isReady = moduleStatus != ModuleStatus::Evaluating && moduleStatus != ModuleStatus::EvaluatingAsync && !moduleData.hasTopLevelAwait();
        return isReady ? ImportWalkStep::Enter : ImportWalkStep::Stop;
    });
}

static void computeEvaluationList(Modules& modules, EvaluationFrame& frame)
{
    auto scope = DECLARE_THROW_SCOPE(*modules.instance().vm);
    auto evaluationList = makeUnique<Vector<EvaluationTarget>>();
    BitVector listedModules(modules.table().size());
    VisitedModuleRecords listedRecords;
    auto appendOnce = [&](ModuleID module) {
        if (listedModules.add(module.index()))
            evaluationList->append(module);
    };
    for (const ModuleRequestData& request : modules.table().imports(frame.module)) {
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
                evaluationList->append(record);
        }
    }
    frame.next = 0;
    frame.end = evaluationList->size();
    frame.evaluationList = WTF::move(evaluationList);
}

void Modules::innerModuleEvaluation(ModuleID root, EvaluationContext& context)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (data(root).kind == ModuleKind::CommonJs)
        RELEASE_AND_RETURN(scope, evaluateCommonJSForLoader(root));

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

    EvaluationFrames frames;
    EvaluationInProgress evaluation { frames, m_innermostEvaluation };
    SetForScope innermostEvaluation(m_innermostEvaluation, &evaluation);

    auto pushRecord = [&](EvaluationFrame& frame, ModuleRecord* record) {
        RELEASE_ASSERT(frame.stackPosition <= context.stack.size());
        frame.record = record;
        // This evaluation began the module, and goes by the record from here on.
        record->setHoldsStatus();
        setStoredStatus(frame.module, StoredModuleStatus::InRecord);
        record->setStatus(CyclicModuleRecord::Status::Evaluating);
        record->setDFSAncestorIndex(frame.dfsIndex);
        record->setPendingAsyncDependencies(0);
        record->setEvaluationStack(&context.stack);
        context.stack.insert(frame.stackPosition, static_cast<AbstractModuleRecord*>(record));
    };

    auto recordOfImporter = [&] {
        EvaluationFrame& importer = frames.last();
        if (!importer.record)
            pushRecord(importer, ensureRecord(importer.module));
        return importer.record;
    };

    auto enter = [&](ModuleID module) {
        const ModuleData& moduleData = data(module);
        if (moduleData.kind == ModuleKind::CommonJs) {
            evaluateCommonJS(module);
            return false;
        }
        // A module of the library ran when its importer was linked.
        if (!moduleData.hasExportTable())
            return false;

        // What a linked module requests was linked with it. No status says so for a module without work in a link.
        ModuleStatus moduleStatus = status(module);
        if (moduleStatus == ModuleStatus::Unlinked && !frames.isEmpty())
            moduleStatus = ModuleStatus::Linked;
        else if (moduleStatus == ModuleStatus::Unlinked) {
            link(module);
            RETURN_IF_EXCEPTION(scope, false);
            moduleStatus = status(module);
            RELEASE_ASSERT(moduleStatus != ModuleStatus::Unlinked);
        }
        if (moduleStatus == ModuleStatus::Failed)
            throwEvaluationError(module);
        if (moduleStatus != ModuleStatus::Linked)
            return false;

        ModuleRecord* record = recordIfExists(module);
        if (!record && moduleData.evaluatesItself()) {
            evaluateByCallingBody(module);
            return false;
        }
        if (!record && !moduleData.needsEvaluation()) {
            setStoredStatus(module, StoredModuleStatus::Evaluated);
            return false;
        }

        setStatus(module, ModuleStatus::Evaluating);
        EvaluationFrame frame { module, nullptr, context.index++, static_cast<unsigned>(context.stack.size()) };
        if (!moduleData.evaluatesItself()) {
            frame.next = moduleData.firstEvaluationRequest;
            frame.end = m_table.endOfEvaluationRequests(module);
        }
        if (!record && !moduleData.isSimple())
            record = ensureRecord(module);
        if (record)
            pushRecord(frame, record);
        frames.append(WTF::move(frame));
        if (moduleData.hasDeferredRequest()) [[unlikely]]
            computeEvaluationList(*this, frames.last());
        return true;
    };

    auto didEvaluateRequiredModule = [&](CyclicModuleRecord* required) {
        if (!required)
            return;
        if (required->status() == CyclicModuleRecord::Status::Evaluating) {
            auto* compiled = dynamicDowncast<ModuleRecord>(required);
            if (compiled ? !compiled->isOnStack(context.stack) : !context.stack.contains(required))
                return;
            ModuleRecord* record = recordOfImporter();
            record->setDFSAncestorIndex(std::min(record->dfsAncestorIndex(), required->dfsAncestorIndex()));
        } else {
            if (CyclicModuleRecord* cycleRoot = required->cycleRoot())
                required = cycleRoot;
            if (JSValue error = required->evaluationError()) {
                throwException(realm, scope, error);
                return;
            }
        }

        AbstractModuleRecord::AsyncEvaluationOrder order = required->asyncEvaluationOrder();
        if (!order.hasOrder() || order.order() == context.referrerAsyncOrder)
            return;
        if (context.importPromise && AbstractModuleRecord::importPromiseGatesAsyncDependency(context.importPromise, required))
            return;
        ModuleRecord* record = recordOfImporter();
        record->setPendingAsyncDependencies(*record->pendingAsyncDependencies() + 1);
        required->appendAsyncParentModule(vm, record);
    };

    auto evaluateUncompiledRecord = [&](AbstractModuleRecord* required) {
        auto* cyclic = dynamicDowncast<CyclicModuleRecord>(required);
        if (cyclic && cyclic->status() == CyclicModuleRecord::Status::Unlinked) {
            required->link(realm, nullptr);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (cyclic && cyclic->status() == CyclicModuleRecord::Status::Linked) {
            // An entry that was on the stack when a frame entered stays until that frame has left, so a recorded position moves only by what is inserted below it.
            unsigned inserted = 0;
            for (EvaluationFrame& frame : frames) {
                frame.stackPosition += inserted;
                if (frame.record)
                    continue;
                pushRecord(frame, ensureRecord(frame.module));
                ++inserted;
            }
        }
        checkSafeToRecurse(realm, scope);
        RETURN_IF_EXCEPTION(scope, void());
        unsigned index = required->innerModuleEvaluation(realm, context.stack, context.index, context.referrerAsyncOrder.value_or(-1), context.importPromise);
        RETURN_IF_EXCEPTION(scope, void());
        context.index = index;
        didEvaluateRequiredModule(cyclic);
    };

    auto leave = [&](EvaluationFrame& frame) {
        const ModuleData& moduleData = data(frame.module);
        ModuleRecord* record = frame.record;
        if (record && (*record->pendingAsyncDependencies() > 0 || moduleData.hasTopLevelAwait())) {
            record->setAsyncEvaluationOrder(vm.incrementModuleAsyncEvaluationCount());
            if (!*record->pendingAsyncDependencies())
                record->executeAsync(realm);
        } else
            callBodyUnderOwnMark(frame.module, moduleData.evaluatesItself());
        RETURN_IF_EXCEPTION(scope, void());
        if (moduleData.capturesExportsBehindEvaluation()) [[unlikely]] {
            captureExports(frame.module);
            RETURN_IF_EXCEPTION(scope, void());
        }

        if (!record) {
            setStatus(frame.module, ModuleStatus::Evaluated);
            return;
        }
        if (record->dfsAncestorIndex() != frame.dfsIndex)
            return;

        Vector<ModuleRecord*, 8> recordsToRetire;
        bool canRetireComponent = true;
        for (AbstractModuleRecord* member = nullptr; member != record;) {
            member = context.stack.takeLast();
            auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(member);
            bool isEvaluated = cyclic->asyncEvaluationOrder().isUnset();
            cyclic->setStatus(isEvaluated ? CyclicModuleRecord::Status::Evaluated : CyclicModuleRecord::Status::EvaluatingAsync);
            member->setCycleRoot(vm, record);
            auto* compiled = dynamicDowncast<ModuleRecord>(member);
            if (!compiled || !isEvaluated)
                canRetireComponent = false;
            else if (!compiled->isRetained())
                recordsToRetire.append(compiled);
        }
        if (!canRetireComponent)
            return;
        for (ModuleRecord* member : recordsToRetire)
            retireRecord(member);
    };

    ModuleNumbers requests = m_table.allEvaluationRequests();
    enter(root);
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
                bool isLinked = stored == StoredModuleStatus::Linked || stored == StoredModuleStatus::Unlinked;
                if (isLinked && requiredData.evaluatesItself()) {
                    evaluateByCallingBody(required);
                    continue;
                }
                if (isLinked && requiredData.kind == ModuleKind::SourceText && requiredData.isSimple() && !requiredData.hasDeferredRequest()) {
                    setStoredStatus(required, StoredModuleStatus::Evaluating);
                    frames.append(EvaluationFrame { required, nullptr, context.index++, static_cast<unsigned>(context.stack.size()), requiredData.firstEvaluationRequest, m_table.endOfEvaluationRequests(required), nullptr });
                    continue;
                }
                target = required;
            }
            const ModuleID* module = std::get_if<ModuleID>(&target);
            if (module && !data(*module).isUncompiledCode()) {
                if (!enter(*module) && !scope.exception())
                    didEvaluateRequiredModule(recordIfExists(*module));
                continue;
            }
            AbstractModuleRecord* record = module ? uncompiledRecord(*module) : std::get<AbstractModuleRecord*>(target);
            if (!scope.exception())
                evaluateUncompiledRecord(record);
            continue;
        }
        leave(frames.last());
        if (scope.exception())
            break;
        ModuleID evaluated = frames.takeLast().module;
        if (!frames.isEmpty())
            didEvaluateRequiredModule(recordIfExists(evaluated));
    }

    Exception* exception = scope.exception();
    if (!exception)
        return;
    for (const EvaluationFrame& frame : frames) {
        // A require() under the evaluation may have run the body and kept its error. Whoever owns the stack finishes what is on it.
        if (frame.record)
            frame.record->setStatus(CyclicModuleRecord::Status::Evaluating);
        else
            setFailed(frame.module, exception->value());
    }
}

JSValue Modules::callBody(ModuleID module)
{
    const ModuleData& moduleData = data(module);
    std::optional<FunctionID> body = moduleData.bodyFunction();
    if (!body)
        return jsUndefined();

    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    ClosureTable& closures = m_state.objects->closures;
    SetForScope isAskingHost(m_isAskingHost, false);
    // Nothing in a file of the program names the function that is its body, so the body needs no closure of its own: the entry takes the number of the function.
    if ((moduleData.kind == ModuleKind::SourceText && !moduleData.hasTopLevelAwait()) || moduleData.kind == ModuleKind::CommonJs) {
        EncodedJSValue function = JSValue::encode(jsNumber(static_cast<int32_t>(body->index())));
        CallData entry = CallData::forCompiledCode(moduleBodyEntrypoint().taggedPtr(), realm);
        return callCompiledCode(vm, closures.calleeOfModuleBodies(vm, *body), entry, jsUndefined(), nullptr, ArgList(&function, 1));
    }
    return callCompiledCode(realm, closures.createClosureForOneCall(vm, *body), jsUndefined(), ArgList());
}

Vector<ModuleID, 8> Modules::modulesThatEvaluationWillBegin(ModuleID root)
{
    Vector<ModuleID, 8> found;
    found.append(root);
    for (size_t next = 0; next < found.size(); ++next) {
        for (const ModuleRequestData& request : m_table.requests(found[next])) {
            ModuleID target = request.targetModule();
            if (request.isRequire || request.isDeferred || storedStatus(target) >= StoredModuleStatus::InRecord || found.contains(target))
                continue;
            found.append(target);
        }
    }
    return found;
}

// A body that stores the status of its module stores it in the table. The status of a module with a record is in the record, and whoever gave it the record goes by that: for the time of the call the table has it.
JSValue Modules::callBodyUnderOwnMark(ModuleID module, bool bodyStoresStatus, bool runsImportedFilesAlone)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ASSERT(!bodyStoresStatus || data(module).evaluatesItself());
    SetForScope rootWhoseFilesRun(m_rootWhoseFilesRun, runsImportedFilesAlone ? std::optional(module) : std::nullopt);

    bool isInRecord = bodyStoresStatus && storedStatus(module) == StoredModuleStatus::InRecord;
    if (isInRecord)
        setStoredStatus(module, StoredModuleStatus::Linked);
    uint32_t outerMark = m_instance.modules.evaluatingMark;
    uint8_t mark = static_cast<uint8_t>(std::min<uint32_t>(outerMark + 1, UINT8_MAX));
    bool isUnderAnotherEvaluation = outerMark != static_cast<uint32_t>(StoredModuleStatus::Evaluating);
    Vector<ModuleID, 8> begunHere;
    if (isUnderAnotherEvaluation && bodyStoresStatus && !data(module).isSimple()) [[unlikely]]
        begunHere = modulesThatEvaluationWillBegin(module);
    m_instance.modules.evaluatingMark = mark;
    JSValue result = callBody(module);
    m_instance.modules.evaluatingMark = outerMark;
    if (isInRecord)
        setStoredStatus(module, StoredModuleStatus::InRecord);
    Exception* exception = scope.exception();
    if (!exception) [[likely]] {
        for (ModuleID begun : begunHere)
            m_modulesEvaluatedUnderAnotherEvaluation.set(begun.index());
        return result;
    }

    // What was being evaluated when a body threw has failed with it.
    Vector<ModuleID, 16> failedModules;
    for (unsigned index = 0; index < m_storedStatuses.size(); ++index) {
        if (m_storedStatuses[index] == mark)
            failedModules.append(ModuleID(index));
    }
    for (ModuleID failed : failedModules)
        didThrowInBody(failed, exception->value());
    // Whoever gave the module its record stores the error there.
    if (isInRecord)
        failedModules.append(module);
    failEvaluatedModulesOfUnfinishedCycles(failedModules.span(), exception->value());
    return result;
}

// No body runs again. Where the module does not keep the error, whoever asks for it later gets what it had exported when it threw.
void Modules::didThrowInBody(ModuleID module, JSValue error)
{
    if (data(module).keepsError())
        setFailed(module, error);
    else
        setStatus(module, ModuleStatus::Evaluated);
}

void Modules::abandonEvaluation(ModuleID module, JSValue error)
{
    if (storedStatus(module) < StoredModuleStatus::Evaluating)
        return;
    didThrowInBody(module, error);
    failEvaluatedModulesOfUnfinishedCycles(std::span<const ModuleID>(&module, 1), error);
}

// A module of a cycle of imports is evaluated for good when the first module of the cycle that was entered is. A require() makes no such cycle, and what it evaluates is evaluated for good when it returns.
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
        if (unfinishedComponents.quickGet(components.componentOf[index]) && storedStatus(module) == StoredModuleStatus::Evaluated && !m_modulesEvaluatedUnderAnotherEvaluation.get(index))
            setFailed(module, error);
    }
}

void Modules::evaluateModuleInRecord(ModuleID module)
{
    ModuleStatus moduleStatus = status(module);
    if (moduleStatus == ModuleStatus::Failed)
        throwEvaluationError(module);
    else if (moduleStatus == ModuleStatus::Unlinked || moduleStatus == ModuleStatus::Linked)
        evaluateWithOwnStack(module);
}

bool Modules::beginModuleInRecord(ModuleID module)
{
    if (isRunningFilesOf(module))
        return false;
    ModuleStatus moduleStatus = status(module);
    // The loader gave the module a record when it went through the graph. The start that asks is called by the start of an importer: the module is evaluated with that importer, as without a record,
    // and fails with it where they lie on a cycle. The record takes the status when it is asked for again. Nothing but its start runs the code of a module without a body.
    bool hasNotBegun = moduleStatus == ModuleStatus::Linked || moduleStatus == ModuleStatus::Unlinked;
    if (hasNotBegun && (!data(module).bodyFunction() || (moduleStatus == ModuleStatus::Linked && !data(module).capturesExportsBehindEvaluation()))) {
        ModuleRecord* record = recordIfExists(module);
        record->setStatus(CyclicModuleRecord::Status::Evaluating);
        record->clearHoldsStatus();
        m_storedStatuses[module.index()] = static_cast<uint8_t>(m_instance.modules.evaluatingMark);
        return true;
    }
    evaluateModuleInRecord(module);
    return false;
}

// `bun run` runs an imported CommonJS file while it loads the graph of the importer. No ES module of that graph has begun by then, so a require() in the file that leads back to one evaluates it there and then.
bool Modules::evaluateImportedCommonJSFiles(ModuleID root, JobsOfImportedFiles jobs, ErrorOfImportedFile error)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (data(root).isLoadedByLoader())
        return false;
    if (data(root).kind != ModuleKind::SourceText || (status(root) != ModuleStatus::Unlinked && status(root) != ModuleStatus::Linked))
        return false;
    if (error == ErrorOfImportedFile::IsLeftToLoader && data(root).reachesUncompiledCode()) {
        bool loadEndsEarly = comesToCodeThatDoesNotLoad(root);
        RETURN_IF_EXCEPTION(scope, false);
        // The loader goes through the graph, and each file runs when the loader comes to it.
        if (loadEndsEarly) {
            m_rootWhoseFilesRanAhead = root;
            return false;
        }
    }

    bool didRunFile = false;
    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    // The start of the root calls the files one after the other, and leaves where one of them has queued a job or where the root would begin.
    if (jobs == JobsOfImportedFiles::WaitForLoader && data(root).evaluatesItself() && data(root).isEntryPoint() && queue.isEmpty() && !m_rootWhoseFilesRun) {
        ensureRecord(root)->retain();
        if (storedStatus(root) == StoredModuleStatus::InRecord) {
            callBodyUnderOwnMark(root, false, true);
            didRunFile = true;
            Exception* exception = scope.exception();
            if (!exception && m_fileWhoseJobsWaitForLoader) {
                m_rootWhoseFilesRanAhead = root;
                return true;
            }
            if (exception) [[unlikely]] {
                if (vm.isTerminationException(exception))
                    return true;
                TRY_CLEAR_EXCEPTION(scope, true);
                std::optional<ModuleID> failedFile;
                for (const ModuleRequestData& request : m_table.imports(root)) {
                    if (!request.isHoisted)
                        break;
                    if (status(request.targetModule()) == ModuleStatus::Failed) {
                        failedFile = request.targetModule();
                        break;
                    }
                }
                if (!failedFile) {
                    throwException(m_instance.globalObject, scope, exception);
                    return true;
                }
                // As in the loop below.
                if (!wasImportedWhileItRan(m_table.fileBehindFace(*failedFile).value_or(*failedFile))) {
                    m_rootWhoseFilesRanAhead = root;
                    return true;
                }
                recordIfExists(*failedFile)->setEvaluationError(vm, JSValue());
                setStatus(*failedFile, ModuleStatus::Evaluated);
            }
        }
    }
    // The files stand in front of everything else that the root requests.
    std::span<const ModuleRequestData> requests = m_table.requests(root);
    for (size_t index = 0; index < requests.size(); ++index) {
        // Most of the files below a root of a large program ran with an earlier graph.
        while (requests[index].isHoisted && m_storedStatuses[requests[index].target()] == static_cast<uint8_t>(StoredModuleStatus::Evaluated)) {
            if (++index == requests.size())
                break;
        }
        if (index == requests.size())
            break;
        const ModuleRequestData& request = requests[index];
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
            if (request.isHoisted)
                continue;
            break;
        }
        ModuleStatus statusAtStart = status(imported);
        if (statusAtStart != ModuleStatus::Unlinked && statusAtStart != ModuleStatus::Linked && statusAtStart != ModuleStatus::Failed)
            continue;
        didRunFile |= statusAtStart != ModuleStatus::Failed;
        size_t numberOfJobs = queue.size();
        // As the start of the root would call it.
        if (storedStatus(imported) < StoredModuleStatus::InRecord && data(imported).evaluatesItself())
            evaluateByCallingBody(imported);
        else
            evaluateModuleInRecord(imported);
        Exception* exception = scope.exception();
        if (!exception) [[likely]] {
            if (jobs == JobsOfImportedFiles::StayQueued || queue.size() == numberOfJobs) [[likely]]
                continue;
            holdJobsBehind(numberOfJobs, imported);
            m_rootWhoseFilesRanAhead = root;
            return didRunFile;
        }
        // A graph that the file required has imported the file back and bound what it had exported by then. The loader has the file from then on, and nobody waits for its end.
        if (statusAtStart == ModuleStatus::Failed || vm.isTerminationException(exception) || !wasImportedWhileItRan(m_table.fileBehindFace(imported).value_or(imported))) {
            // `bun run` loads the rest of the graph, and with it runs the other files, before it reports the error. The loader goes through the graph and finds the error where it comes to the file.
            if (error == ErrorOfImportedFile::IsLeftToLoader && !vm.isTerminationException(exception)) {
                TRY_CLEAR_EXCEPTION(scope, didRunFile);
                m_rootWhoseFilesRanAhead = root;
            }
            return didRunFile;
        }
        TRY_CLEAR_EXCEPTION(scope, didRunFile);
        if (status(imported) != ModuleStatus::Failed)
            continue;
        recordIfExists(imported)->setEvaluationError(vm, JSValue());
        setStatus(imported, ModuleStatus::Evaluated);
    }
    if (didRunFile)
        m_rootsLoadedWithoutLoader.append(root);
    return didRunFile;
}

// Under `bun run` these jobs run before the loader comes to a file that a deeper module imports. From here on the loader goes through the graph.
void Modules::holdJobsBehind(size_t numberOfJobs, ModuleID file)
{
    VM& vm = *m_instance.vm;
    if (!m_jobsThatWaitForLoader)
        m_jobsThatWaitForLoader = MicrotaskQueue::create(vm);
    vm.defaultMicrotaskQueue().moveTasksBehind(numberOfJobs, *m_jobsThatWaitForLoader);
    m_fileWhoseJobsWaitForLoader = m_table.fileBehindFace(file).value_or(file);
}

bool Modules::stopLoadingBehindFile(ModuleID file)
{
    if (!m_rootWhoseFilesRun)
        return false;
    holdJobsBehind(0, file);
    return true;
}

void Modules::queueJobsThatWaitForLoader()
{
    if (!m_fileWhoseJobsWaitForLoader)
        return;
    m_fileWhoseJobsWaitForLoader = std::nullopt;
    m_jobsThatWaitForLoader->moveTasksBehind(0, m_instance.vm->defaultMicrotaskQueue());
}

// The loader comes to nothing more of the graph.
void Modules::didFailToLoadGraph(ModuleID root)
{
    if (m_rootWhoseFilesRanAhead != root)
        return;
    m_rootWhoseFilesRanAhead = std::nullopt;
    queueJobsThatWaitForLoader();
}

void Modules::didLoadGraph(ModuleID root)
{
    if (m_rootWhoseFilesRanAhead == root) [[unlikely]]
        m_rootWhoseFilesRanAhead = std::nullopt;
    for (LoadInFlight& load : m_loadsInFlight)
        load.isLoaded |= load.root == root;
}

unsigned Modules::numberOfGraphsBeingLoaded()
{
    m_loadsInFlight.removeAllMatching([&](const LoadInFlight& load) {
        if (load.promise->status() == JSPromise::Status::Pending)
            return false;
        if (m_rootWhoseFilesRanAhead == load.root)
            m_rootWhoseFilesRanAhead = std::nullopt;
        return true;
    });
    bool hostLoadsGraph = m_rootWhoseFilesRanAhead && !m_loadsInFlight.containsIf([&](const LoadInFlight& load) {
        return load.root == m_rootWhoseFilesRanAhead;
    });
    return hostLoadsGraph + std::ranges::count_if(m_loadsInFlight, [](const LoadInFlight& load) {
        return !load.isLoaded;
    });
}

// A synchronous load takes over the steps of an import() where it comes to a module that the loader has in its hands for that import(): the root, and below a root whose graph the loader goes through
// whatever is not loaded yet. The promise of that import() is settled before require() returns. A graph that shares nothing of the kind is loaded in steps that nobody sees.
bool Modules::comesToLoadInFlight(ModuleID required)
{
    Vector<ModuleID, 8> rootsThatWait;
    bool loaderGoesThroughGraph = false;
    for (LoadInFlight& load : m_loadsInFlight) {
        if (load.isLoaded)
            continue;
        if (load.isWalked) {
            loaderGoesThroughGraph = true;
            if (!std::exchange(load.hasMarksBelowRoot, true))
                markModulesBelowWalkedRoot(load.root);
        } else if (status(load.root) != ModuleStatus::Evaluating)
            rootsThatWait.append(load.root);
    }
    if (!loaderGoesThroughGraph && rootsThatWait.isEmpty())
        return false;

    BitVector seen(m_table.size());
    seen.quickSet(required.index());
    Vector<ModuleID, 16> modules { required };
    while (!modules.isEmpty()) {
        ModuleID module = modules.takeLast();
        if (rootsThatWait.contains(module))
            return true;
        bool isNotLoaded = this->isNotLoaded(module);
        if (loaderGoesThroughGraph && m_modulesBelowWalkedRoots.get(module.index()) && (isNotLoaded || hasNotBegunAsFile(module)))
            return true;
        // The loader asks a module that it has loaded for no requests.
        if (!isNotLoaded)
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID target = request.targetModule();
            if (!request.isHoisted && !seen.quickSet(target.index()))
                modules.append(target);
        }
    }
    return false;
}

bool Modules::hasNotBegunAsFile(ModuleID module) const
{
    return isCommonJSFileOrFace(module) && (status(module) == ModuleStatus::Unlinked || status(module) == ModuleStatus::Linked);
}

// Once for a load, when the first require() asks. A mark outlives the load: whoever reads it asks whether the module is loaded by now.
void Modules::markModulesBelowWalkedRoot(ModuleID root)
{
    m_modulesBelowWalkedRoots.ensureSize(m_table.size());
    m_modulesBelowWalkedRoots.quickSet(root.index());
    Vector<ModuleID, 16> modules { root };
    while (!modules.isEmpty()) {
        ModuleID module = modules.takeLast();
        if (!isNotLoaded(module))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID target = request.targetModule();
            if (request.isHoisted || m_modulesBelowWalkedRoots.quickGet(target.index()))
                continue;
            if (isNotLoaded(target)) {
                m_modulesBelowWalkedRoots.quickSet(target.index());
                modules.append(target);
            } else if (hasNotBegunAsFile(target))
                m_modulesBelowWalkedRoots.quickSet(target.index());
        }
    }
}

void Modules::didStartLoad(JSPromise* load, ModuleID root)
{
    if (load->status() != JSPromise::Status::Pending)
        return;
    // Whoever asks about the loads first drops those that are settled. A program that only imports never asks.
    m_loadsInFlight.removeAllMatching([&](const LoadInFlight& earlier) {
        return earlier.promise->status() != JSPromise::Status::Pending && m_rootWhoseFilesRanAhead != earlier.root;
    });
    m_loadsInFlight.append(LoadInFlight { Strong<JSPromise>(*m_instance.vm, load), root });
}

// `bun run` loads the graph of a required ES module in steps, all of them before require() returns. A program can tell only where the steps of another graph come due among them.
bool Modules::needsLoaderForRequire(ModuleID module)
{
    if (!isLoadedWithRequests(module))
        return false;
    if (data(module).isLoadedByLoader())
        return true;
    if (!numberOfGraphsBeingLoaded())
        return false;
    return m_rootWhoseFilesRanAhead || comesToLoadInFlight(module);
}

// The loader has not linked the module, so it may not have walked what the module imports.
bool Modules::isNotLoaded(ModuleID module)
{
    if (!isLoadedWithRequests(module))
        return false;
    if (ModuleRecord* record = recordIfExists(module))
        return record->status() == CyclicModuleRecord::Status::New || record->status() == CyclicModuleRecord::Status::Unlinked;
    if (storedStatus(module) != StoredModuleStatus::Unlinked || (m_entryPointIsLinked && data(module).isLinkedWithEntryPoint()))
        return false;
    return data(module).needsLink() || !wasLoadedWithGraphBeingEvaluated(module);
}

// A graph is loaded before a module of it begins, and nothing is stored when a module that needs no link is loaded. What has not begun below a module that is being evaluated was loaded with it.
bool Modules::wasLoadedWithGraphBeingEvaluated(ModuleID module)
{
    if (!m_innermostEvaluation)
        return false;
    m_modulesLoadedWithGraphs.ensureSize(m_table.size());
    Vector<ModuleID, 32> importers;
    for (const EvaluationInProgress* evaluation = m_innermostEvaluation; evaluation; evaluation = evaluation->outer) {
        for (const EvaluationFrame& frame : evaluation->frames) {
            if (!m_modulesLoadedWithGraphs.quickSet(frame.module.index()))
                importers.append(frame.module);
        }
    }
    while (!importers.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(importers.takeLast())) {
            ModuleID target = request.targetModule();
            if (request.isRequire || request.isHoisted || storedStatus(target) == StoredModuleStatus::Evaluated || !isLoadedWithRequests(target))
                continue;
            if (!m_modulesLoadedWithGraphs.quickSet(target.index()))
                importers.append(target);
        }
    }
    return m_modulesLoadedWithGraphs.quickGet(module.index());
}

bool Modules::planLevelsOfLoad(ModuleID root)
{
    struct Found {
        ModuleID module;
        uint32_t importer;
        uint32_t place;
    };
    struct Level {
        Vector<Found, 8> modules;
        Vector<Found, 8> files;
    };
    if (data(root).reachesUncompiledCode())
        return false;

    auto isInHandsOfLoader = [&](ModuleID module) {
        if (m_levelsOfLoads.contains(module))
            return true;
        ModuleRecord* record = recordIfExists(module);
        if (!record || record->status() != CyclicModuleRecord::Status::New)
            return false;
        return record->isRequestedByWalkedModule() || m_loadsInFlight.containsIf([&](const LoadInFlight& load) {
            return load.root == module;
        });
    };
    // A module with nothing to link and nothing to run stays as the loader left it when it had loaded it. The loader takes no step for it again.
    auto isLeftAsLoaded = [&](ModuleID module) {
        ModuleRecord* record = isCommonJSFileOrFace(module) ? nullptr : recordIfExists(module);
        return record && record->status() == CyclicModuleRecord::Status::Unlinked;
    };
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<Level, 8> levels(1);
    levels[0].modules.append(Found { root, 0, 0 });
    while (true) {
        Level next;
        uint32_t place = 0;
        const Level& level = levels.last();
        for (uint32_t importer = 0; importer < level.modules.size(); ++importer) {
            for (const ModuleRequestData& request : m_table.imports(level.modules[importer].module)) {
                ModuleID target = request.targetModule();
                if (request.isHoisted || seen.quickSet(target.index()) || wasLoadedWithEarlierGraph(target) || isLeftAsLoaded(target))
                    continue;
                // Another graph that is being loaded has come to the module, or will.
                if (isInHandsOfLoader(target))
                    return false;
                if (isCommonJSFileOrFace(target) || data(target).kind == ModuleKind::Data)
                    next.files.append(Found { target, importer, place++ });
                else if (isFileOfProgram(target))
                    next.modules.append(Found { target, importer, place++ });
            }
        }
        if (next.modules.isEmpty() && next.files.isEmpty())
            break;
        levels.append(WTF::move(next));
    }

    std::optional<ModuleID> below;
    std::optional<ModuleID> firstFileBehind;
    uint32_t position = 0;
    for (size_t depth = levels.size(); depth--;) {
        const Level& level = levels[depth];
        bool isFile = !below && level.modules.isEmpty();
        const Found& onWay = isFile ? level.files[0] : level.modules[position];
        auto planned = makeUnique<LevelOfLoad>();
        planned->next = below;
        planned->firstFileBehindNext = std::exchange(firstFileBehind, std::nullopt);
        std::unique_ptr<LevelOfLoad> behind;
        for (const Found& file : level.files) {
            if (file.module == onWay.module)
                continue;
            if (isFile || file.place < onWay.place)
                planned->files.append(file.module);
            else if (behind)
                behind->files.append(file.module);
            else {
                behind = makeUnique<LevelOfLoad>();
                firstFileBehind = file.module;
            }
        }
        if (behind)
            m_levelsOfLoads.set(*firstFileBehind, WTF::move(behind));
        m_levelsOfLoads.set(onWay.module, WTF::move(planned));
        below = onWay.module;
        position = onWay.importer;
    }
    return true;
}

void Modules::evaluateFilesOfLevel(ModuleID moduleOnWay, bool runsWhenLoaded)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (runsWhenLoaded)
        evaluateLoadedCommonJSFile(moduleOnWay);
    auto found = m_levelsOfLoads.find(moduleOnWay);
    if (found == m_levelsOfLoads.end()) [[likely]] {
        scope.release();
        return;
    }

    // `bun run` loads the rest of the graph, and with it runs the other files, before it reports an error.
    JSValue firstError = found->value->firstError.get();
    auto noteError = [&] {
        Exception* exception = scope.exception();
        if (!exception) [[likely]]
            return true;
        if (vm.isTerminationException(exception))
            return false;
        if (!firstError)
            firstError = exception->value();
        return scope.tryClearException();
    };
    if (!noteError())
        return;
    Vector<ModuleID, 4> files = std::exchange(found->value->files, { });
    std::optional<ModuleID> next = found->value->next;
    for (ModuleID file : files) {
        evaluateLoadedCommonJSFile(file);
        if (!noteError())
            return;
        m_filesLoadedWithoutLoader.set(file.index());
    }
    // The loader asks a file for no requests, nor the module that stands for one: it has run by now.
    if (!isLoadedWithRequests(moduleOnWay) || isCommonJSFileOrFace(moduleOnWay)) {
        m_filesLoadedWithoutLoader.set(moduleOnWay.index());
        m_levelsOfLoads.remove(moduleOnWay);
    }
    if (!firstError)
        return;
    if (auto below = next ? m_levelsOfLoads.find(*next) : m_levelsOfLoads.end(); below != m_levelsOfLoads.end()) {
        below->value->firstError.set(vm, firstError);
        return;
    }
    throwException(m_instance.globalObject, scope, firstError);
}

// While the loader goes through a graph whose files ran ahead of it, it comes to each of them.
bool Modules::wasLoadedWithEarlierGraph(ModuleID module)
{
    if (!isCommonJSFileOrFace(module))
        return isLoadedWithRequests(module) && !isNotLoaded(module);
    if (m_rootWhoseFilesRanAhead || status(module) != ModuleStatus::Evaluated)
        return false;
    for (ModuleID root : std::exchange(m_rootsLoadedWithoutLoader, { })) {
        for (const ModuleRequestData& request : m_table.imports(root)) {
            if (!request.isHoisted)
                break;
            m_filesLoadedWithoutLoader.set(request.target());
        }
    }
    return m_filesLoadedWithoutLoader.get(module.index());
}

ModuleRecord* Modules::recordOfCommonJSFile(ModuleID module)
{
    ModuleID file = m_table.fileBehindFace(module).value_or(module);
    // Where nothing but its face runs a file, the face alone has the status.
    if (status(file) == ModuleStatus::Evaluating || status(module) == ModuleStatus::Evaluating)
        ensureLoaded(file).wasLoadedWhileItRan = true;
    ModuleRecord* record = ensureRecord(module);
    record->retain();
    record->setRunsWhenLoaded();
    return record;
}

// `bun run` runs an imported CommonJS file when the loader makes the record of the file.
void Modules::evaluateLoadedCommonJSFile(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isCommonJSFileOrFace(module))
        return;
    if (m_fileWhoseJobsWaitForLoader && m_fileWhoseJobsWaitForLoader == m_table.fileBehindFace(module).value_or(module)) [[unlikely]]
        queueJobsThatWaitForLoader();
    ModuleStatus statusAtStart = status(module);
    // A graph that the file requires comes to the file: the loader makes its record a second time.
    if (statusAtStart == ModuleStatus::Evaluating)
        ensureLoaded(m_table.fileBehindFace(module).value_or(module)).wasLoadedWhileItRan = true;
    if (statusAtStart != ModuleStatus::Unlinked && statusAtStart != ModuleStatus::Linked && statusAtStart != ModuleStatus::Failed)
        return;
    // As the start of an importer would call it.
    if (storedStatus(module) < StoredModuleStatus::InRecord && data(module).evaluatesItself())
        evaluateByCallingBody(module);
    else
        evaluateModuleInRecord(module);
    Exception* exception = scope.exception();
    if (!exception) [[likely]]
        return;
    if (statusAtStart == ModuleStatus::Failed || vm.isTerminationException(exception) || !wasImportedWhileItRan(m_table.fileBehindFace(module).value_or(module)))
        return;
    TRY_CLEAR_EXCEPTION(scope, void());
    if (status(module) != ModuleStatus::Failed)
        return;
    recordIfExists(module)->setEvaluationError(vm, JSValue());
    setStatus(module, ModuleStatus::Evaluated);
}

// Of a file whose body has just thrown. The file ran in front of every ES module that imports it, so one that has begun was loaded by the file itself.
bool Modules::wasImportedWhileItRan(ModuleID file) const
{
    if (LoadedModule* loaded = loadedIfExists(file); loaded && loaded->wasLoadedWhileItRan)
        return true;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (data(importer).kind != ModuleKind::SourceText || status(importer) <= ModuleStatus::Linked)
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (request.targetModule() == file || m_table.fileBehindFace(request.targetModule()) == file)
                return true;
        }
    }
    return false;
}

// For an entry point, as for an imported file: see evaluateImportedCommonJSFiles().
void Modules::evaluateCommonJSForLoader(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    evaluateCommonJS(file);
    Exception* exception = scope.exception();
    if (!exception) [[likely]] {
        // `bun run` imports an entry point, so its loader takes the names of the file when the file has run. An import() finds them as they were then.
        if (std::optional<ModuleID> face = m_table.faceOfFile(file))
            RELEASE_AND_RETURN(scope, evaluateLoadedCommonJSFile(*face));
        return;
    }
    if (vm.isTerminationException(exception) || !wasImportedWhileItRan(file))
        return;
    TRY_CLEAR_EXCEPTION(scope, void());
    if (status(file) != ModuleStatus::Failed)
        return;
    recordIfExists(file)->setEvaluationError(vm, JSValue());
    setStatus(file, ModuleStatus::Evaluated);
}

void Modules::evaluateWithOwnStack(ModuleID root)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    ModuleEvaluationStack stack;
    EvaluationContext context { stack };
    innerModuleEvaluation(root, context);
    Exception* exception = scope.exception();
    ASSERT(exception || stack.isEmpty());
    if (!exception)
        return;
    for (AbstractModuleRecord* member : stack) {
        auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(member);
        cyclic->setStatus(CyclicModuleRecord::Status::Evaluated);
        cyclic->setEvaluationError(vm, exception->value());
        if (auto* compiled = dynamicDowncast<ModuleRecord>(member))
            compiled->retain();
    }
}

JSPromise* Modules::evaluate(ModuleID root, std::optional<int64_t> referrerAsyncOrder, JSPromise* importPromise, const ModuleLinkWalk* linkWalk)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& moduleData = data(root);
    ASSERT(status(root) != ModuleStatus::Evaluating);

    evaluateImportedCommonJSFiles(root);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (status(root) == ModuleStatus::Failed)
        throwEvaluationError(root);
    else if (linkWalk)
        link(*linkWalk);
    else
        link(root);
    if (!scope.exception() && moduleData.kind == ModuleKind::CommonJs)
        evaluateCommonJSForLoader(root);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));

    if (moduleData.hasExportTable() && (recordIfExists(root) || (moduleData.needsEvaluation() && !(moduleData.isSimple() && moduleData.isSynchronous()) && !moduleData.evaluatesItself()))) {
        ModuleRecord* record = ensureRecord(root);
        // The tables are linked, whether or not the loader has come to the module.
        if (record->status() == CyclicModuleRecord::Status::New || record->status() == CyclicModuleRecord::Status::Unlinked)
            record->setStatus(CyclicModuleRecord::Status::Linked);
        RELEASE_AND_RETURN(scope, record->evaluate(realm, referrerAsyncOrder.value_or(-1), importPromise));
    }

    if (moduleData.hasExportTable() && !moduleData.needsEvaluation())
        setStoredStatus(root, StoredModuleStatus::Evaluated);
    else if (moduleData.hasExportTable() && status(root) != ModuleStatus::Evaluated) {
        evaluateWithOwnStack(root);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(realm, jsUndefined()));
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
    ModuleNumbers requests = m_table.evaluationRequests(module);
    for (const EvaluationInProgress* evaluation = m_innermostEvaluation; evaluation; evaluation = evaluation->outer) {
        const EvaluationFrames& frames = evaluation->frames;
        for (size_t index = 1; index < frames.size(); ++index) {
            const EvaluationFrame& importer = frames[index - 1];
            ModuleID required = frames[index].module;
            if (!importer.evaluationList && requests.contains(required.index()) && isPassedOverBetween(m_table, importer.module, required, module))
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

    if (reason == SynchronousEvaluationReason::DeferredNamespace) {
        VisitedModuleRecords seen;
        bool isReady = readyForSyncExecution(module, seen);
        RETURN_IF_EXCEPTION(scope, void());
        if (!isReady) {
            throwException(realm, scope, createDeferredModuleNotReadyError(realm));
            return;
        }
    }

    ModuleStatus statusAtStart = status(module);
    switch (statusAtStart) {
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
        break;
    case ModuleStatus::Linked:
        captureExportsOfFilesBeingEvaluated(module);
        RETURN_IF_EXCEPTION(scope, void());
        break;
    }
    if (isPassedOverByEvaluationInProgress(module))
        return;

    bool needsLoader = needsLoaderForRequire(module);
    auto didLeaveLoad = makeScopeExit([&] {
        if (m_rootWhoseFilesRanAhead == module)
            m_rootWhoseFilesRanAhead = std::nullopt;
    });
    if (!needsLoader) {
        evaluateImportedCommonJSFiles(module, JobsOfImportedFiles::StayQueued, ErrorOfImportedFile::IsLeftToLoader);
        RETURN_IF_EXCEPTION(scope, void());
        needsLoader = m_rootWhoseFilesRanAhead == module;
        // One of the files may have required the module.
        if (!needsLoader && status(module) != statusAtStart)
            RELEASE_AND_RETURN(scope, evaluateSynchronously(module, reason));
    }

    JSPromise* promise = nullptr;
    if (needsLoader) {
        SetForScope isAskingHost(m_isAskingHost, true);
        promise = loader()->loadModuleSync(realm, moduleKey(module), nullptr, nullptr);
    } else if (statusAtStart == ModuleStatus::Unlinked && moduleData.reachesUncompiledCode()) {
        ModuleRecord* record = registerWithLoader(module);
        RETURN_IF_EXCEPTION(scope, void());
        promise = loader()->loadModuleSync(realm, record->moduleKey(), nullptr, nullptr);
    } else {
        link(module);
        RETURN_IF_EXCEPTION(scope, void());
        // As in JSModuleLoader::loadModuleSync(): the jobs of the loader run before require() returns. One of them resumes a module whose awaited promise is settled.
        VM::SynchronousModuleQueueScope queue(*m_instance.vm);
        if (moduleData.isSynchronous()) {
            evaluateWithOwnStack(module);
            Exception* exception = scope.exception();
            if (exception) [[unlikely]] {
                if (m_instance.vm->isTerminationException(exception))
                    return;
                TRY_CLEAR_EXCEPTION(scope, void());
            }
            JSModuleLoader::drainSynchronousModuleQueue(realm);
            RETURN_IF_EXCEPTION(scope, void());
            if (exception) [[unlikely]]
                throwException(realm, scope, exception);
            return;
        }
        promise = evaluate(module, std::nullopt, nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        JSModuleLoader::drainSynchronousModuleQueue(realm);
    }
    RETURN_IF_EXCEPTION(scope, void());

    if (promise->status() == JSPromise::Status::Fulfilled)
        return;
    promise->markAsHandled();
    if (promise->status() == JSPromise::Status::Rejected)
        throwException(realm, scope, promise->result());
    else if (moduleData.hasTopLevelAwait() || (status(module) != ModuleStatus::Evaluating && status(module) != ModuleStatus::Evaluated))
        throwAsyncModuleError();
}

JSValue Modules::execute(ModuleID module)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);

    if (moduleData.kind == ModuleKind::CommonJs) {
        evaluateCommonJS(module);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, exportsSeenByLoader(module));
    }

    switch (status(module)) {
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linked:
        break;
    case ModuleStatus::Evaluating:
        // Bun's registry would create the module again and again until the stack overflows.
        throwStackOverflowError(realm, scope);
        return { };
    case ModuleStatus::Evaluated:
        return value(module);
    case ModuleStatus::Failed:
        scope.release();
        throwEvaluationError(module);
        return { };
    case ModuleStatus::Linking:
    case ModuleStatus::EvaluatingAsync:
        RELEASE_ASSERT_NOT_REACHED();
    }

    JSValue value;
    if (moduleData.kind == ModuleKind::RuntimeSource) {
        // The stack is asked for once, for the module and all that its body requires.
        size_t availableStack = std::bit_cast<uintptr_t>(currentStackPointer()) - std::bit_cast<uintptr_t>(vm.softStackLimit());
        if (!m_isStartingRuntimeModule && (availableStack < stackNeededToStartRuntimeModule || !vm.isSafeToRecurseSoft())) [[unlikely]] {
            throwStackOverflowError(realm, scope);
            return { };
        }
        SetForScope isStartingRuntimeModule(m_isStartingRuntimeModule, true);
        setStatus(module, ModuleStatus::Evaluating);
        value = callBody(module);
        // Bun's registry keeps nothing of a module whose body threw: the next request runs the body again.
        if (scope.exception()) [[unlikely]] {
            setStatus(module, ModuleStatus::Linked);
            return { };
        }
    } else if (moduleData.kind == ModuleKind::Runtime) {
        auto* requireBuiltinModule = realm->globalObjectMethodTable()->requireBuiltinModule;
        if (!requireBuiltinModule) [[unlikely]] {
            throwException(realm, scope, createBuiltinModulesUnavailableError(realm));
            return { };
        }
        setStatus(module, ModuleStatus::Evaluating);
        value = requireBuiltinModule(realm, *moduleData.registryID());
    } else {
        RELEASE_ASSERT(moduleData.isHostValue());
        JSObject* requirer = moduleObject(*moduleData.referrerModule());
        RETURN_IF_EXCEPTION(scope, { });
        setStatus(module, ModuleStatus::Evaluating);
        RequestToHostScope requestToHost(*this);
        value = host().requireSpecifier(realm, requirer, pathString(module));
    }
    if (scope.exception()) [[unlikely]] {
        setStatus(module, ModuleStatus::Unlinked);
        return { };
    }
    setValue(module, value);
    setStatus(module, ModuleStatus::Evaluated);
    return value;
}

JSValue Modules::require(ModuleID target)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& targetData = data(target);

    if (targetData.isUncompiledCode())
        RELEASE_AND_RETURN(scope, requireSpecifier(pathString(target), *targetData.referrerModule(), true));
    if (!targetData.hasExportTable()) {
        JSValue executed = execute(target);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_ASSERT(executed);
        return executed;
    }

    LoadedModule& loaded = ensureLoaded(target);
    // In plain Bun the cache holds a module object with empty exports until the loader's require() returns. The jobs of the loader that it runs behind the evaluation can run a file that requires the module.
    if (loaded.isBeingRequired) {
        if (!loaded.exportsWhileRequired)
            loaded.exportsWhileRequired.set(*this, constructEmptyObject(m_instance.globalObject));
        return loaded.exportsWhileRequired.get();
    }
    if (status(target) != ModuleStatus::Evaluated) {
        // An evaluation that an import has begun shows the namespace object, with its bindings as they are.
        loaded.isBeingRequired = status(target) != ModuleStatus::Evaluating;
        evaluateSynchronously(target, SynchronousEvaluationReason::Require);
        loaded.isBeingRequired = false;
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (JSValue answered = loaded.requiredValue)
        return answered;

    JSValue result;
    if (targetData.requireResult == RequireResult::DefaultExport) {
        std::optional<unsigned> index = m_table.findExport(target, vm.propertyNames->defaultKeyword.impl());
        RELEASE_ASSERT(index);
        result = readExport(target, *index);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        RELEASE_ASSERT(targetData.requireResult == RequireResult::Namespace);
        JSObject* namespaceObject = moduleNamespace(target, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        if (m_moduleExportsName.isNull())
            m_moduleExportsName = Identifier::fromString(vm, "module.exports"_s);
        JSValue esModule = readExportForRequire(namespaceObject, vm.propertyNames->__esModule);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue moduleExports;
        if (esModule) {
            moduleExports = readExportForRequire(namespaceObject, m_moduleExportsName);
            RETURN_IF_EXCEPTION(scope, { });
        }
        markNamespaceAsRequired(target);
        result = !moduleExports || moduleExports.isUndefinedOrNull() ? JSValue(namespaceObject) : moduleExports;
    }
    loaded.requiredValue.set(*this, result);
    return result;
}

// A module that is required again while its body runs has exported what it has exported by then.
void Modules::evaluateCommonJS(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& moduleData = data(module);
    ASSERT(moduleData.kind == ModuleKind::CommonJs);

    ModuleStatus moduleStatus = status(module);
    if (moduleStatus == ModuleStatus::Evaluating || moduleStatus == ModuleStatus::Evaluated)
        return;
    if (moduleStatus == ModuleStatus::Failed)
        RELEASE_AND_RETURN(scope, throwEvaluationError(module));
    RELEASE_ASSERT(moduleStatus == ModuleStatus::Unlinked || moduleStatus == ModuleStatus::Linked);
    if (moduleStatus == ModuleStatus::Unlinked) {
        link(module);
        RETURN_IF_EXCEPTION(scope, void());
    }

    ModuleRecord* record = recordIfExists(module);
    if (!record && moduleData.evaluatesItself())
        RELEASE_AND_RETURN(scope, evaluateByCallingBody(module));

    // The body of a module that nothing of the image calls stores no status, and whoever gave a module a record goes by the status in the record.
    setStatus(module, ModuleStatus::Evaluating);
    callBodyUnderOwnMark(module, moduleData.evaluatesItself());
    if (Exception* exception = scope.exception()) [[unlikely]] {
        didThrowInBody(module, exception->value());
        return;
    }
    setStatus(module, ModuleStatus::Evaluated);
    if (!record)
        return;

    if (moduleData.capturesExports()) {
        captureExports(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (!record->isRetained())
        retireRecord(record);
}

JSValue Modules::commonJSExports(ModuleID module) const
{
    ASSERT(data(module).kind == ModuleKind::CommonJs);
    JSValue exports = data(module).hasValue() ? ProgramStatics::readVariable(m_instance, data(module).value) : JSValue();
    return exports ? exports : jsUndefined();
}

// Whoever holds the module object may have defined another property in the place of `exports`.
JSValue Modules::exportsSeenByLoader(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->requirer) [[likely]]
        return commonJSExports(module);
    return loaded->requirer->get(m_instance.globalObject, m_instance.vm->propertyNames->exports);
}

// `bun run` gives the id "." to the first CommonJS file that its loader of ES modules fetches: the entry point, or the first such file in the graph of an entry point that is an ES module.
std::optional<ModuleID> Modules::mainCommonJSModule() const
{
    if (!m_firstModuleRequestedByHost)
        return std::nullopt;
    ModuleID entryPoint = *m_firstModuleRequestedByHost;
    if (data(entryPoint).kind == ModuleKind::CommonJs)
        return entryPoint;
    auto imports = m_table.imports(entryPoint);
    if (imports.begin() == imports.end())
        return std::nullopt;
    ModuleID first = (*imports.begin()).targetModule();
    return data(first).kind == ModuleKind::CommonJs ? std::optional(first) : m_table.fileBehindFace(first);
}

JSObject* Modules::moduleObject(ModuleID module)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.requirer)
        return loaded.requirer;

    ModuleObjects objects;
    if (data(module).kind == ModuleKind::CommonJs) {
        OptionSet<ModuleObjectTrait> traits;
        if (data(module).hasValue())
            traits.add(ModuleObjectTrait::HasExportsVariable);
        if (mainCommonJSModule() == module)
            traits.add(ModuleObjectTrait::IsMain);
        host().createModuleObject(realm, pathString(module), module, traits, objects);
    } else {
        JSObject* meta = importMeta(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        host().createRequirer(realm, meta, objects);
    }
    RETURN_IF_EXCEPTION(scope, nullptr);
    loaded.requirer.set(*this, objects.module);
    loaded.requireFunction.set(*this, objects.require);
    return objects.module;
}

JSObject* Modules::mainModuleObject()
{
    if (!m_firstModuleRequestedByHost || data(*m_firstModuleRequestedByHost).kind != ModuleKind::CommonJs || !mayHandOutModuleObject(*m_firstModuleRequestedByHost))
        return nullptr;
    return moduleObject(*m_firstModuleRequestedByHost);
}

void Modules::recordRequire(ModuleID target, ModuleID referrer)
{
    if (data(target).kind != ModuleKind::CommonJs) {
        markNamespaceAsRequired(target);
        return;
    }
    // A require() is recorded in front of the evaluation and behind it. The module has its parent from the first record on. It is a child from the second on, which a call that throws does not reach.
    auto recorded = m_recordedRequires.add((static_cast<uint64_t>(referrer.index()) + 1) << 32 | target.index(), false);
    if (recorded.isNewEntry) {
        LoadedModule& required = ensureLoaded(target);
        if (!required.firstRequirer)
            required.firstRequirer = referrer;
        return;
    }
    bool& isChild = recorded.iterator->value;
    if (isChild || status(target) == ModuleStatus::Failed)
        return;
    isChild = true;
    ensureLoaded(referrer).requiredModules.append(target);
}

JSObject* Modules::parentModuleObject(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->firstRequirer || mainCommonJSModule() == module || !mayHandOutModuleObject(*loaded->firstRequirer))
        return nullptr;
    return moduleObject(*loaded->firstRequirer);
}

bool Modules::forEachNewChild(ModuleID module, const ScopedLambda<void(JSObject*)>& functor)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded)
        return true;
    for (ModuleID child : loaded->requiredModules) {
        if (!mayHandOutModuleObject(child))
            return false;
    }
    // The functor may run code of the program, which may require more.
    while (loaded->numberOfChildrenShown < loaded->requiredModules.size()) {
        JSObject* childObject = moduleObject(loaded->requiredModules[loaded->numberOfChildrenShown++]);
        RETURN_IF_EXCEPTION(scope, true);
        functor(childObject);
        RETURN_IF_EXCEPTION(scope, true);
    }
    return true;
}

JSValue Modules::requireSpecifier(JSValue specifier, ModuleID referrer, bool isNamedByBuild)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    if (isNamedByBuild)
        admitCodeNamedBy(specifier);
    else
        willLoadByName(realm, specifier);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* requirer = moduleObject(referrer);
    RETURN_IF_EXCEPTION(scope, { });
    SetForScope isAskingHost(m_isAskingHost, m_isAskingHost || isNamedByBuild);
    RELEASE_AND_RETURN(scope, host().requireSpecifier(realm, requirer, specifier));
}

JSValue Modules::resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm form)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSObject* holder = form == ResolvedSpecifierForm::URL ? importMeta(referrer) : moduleObject(referrer);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, host().resolveSpecifier(m_instance.globalObject, holder, specifier, form));
}

// From the first require() on, the namespace object of a module without an export `__esModule` reads `true` under that name, for everybody who holds it.
void Modules::markNamespaceAsRequired(ModuleID module)
{
    if (!data(module).hasExportTable() || m_modulesWithMarkOfRequire.get(module.index()))
        return;
    m_modulesWithMarkOfRequire.set(module.index());
    auto* namespaceObject = uncheckedDowncast<ModuleNamespace>(namespaceIfCreated(module, ModulePhase::Evaluation));
    if (namespaceObject && !namespaceObject->hasExport(m_instance.vm->propertyNames->__esModule.impl()))
        namespaceObject->setESModuleMarker(true);
}

// An empty result says that the read threw, as it does for a binding in its dead zone.
JSValue Modules::readExportForRequire(JSObject* namespaceObject, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto* compiledNamespace = uncheckedDowncast<ModuleNamespace>(namespaceObject);
    std::optional<unsigned> index = compiledNamespace->find(name.impl());
    if (!index && !compiledNamespace->hasExport(name.impl()))
        return jsUndefined();

    JSValue exported = index ? readExport(compiledNamespace->module(), *index) : namespaceObject->get(m_instance.globalObject, name);
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, { });
        return { };
    }
    return exported;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

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
#include "AOTRuntimeValidation.h"
#include "AOTProgramStatics.h"
#include "AOTRuntimeHooks.h"
#include "AOTState.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTStubEntrypoints.h"
#include "ExceptionHelpers.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
#include "ModuleGraphLoadingState.h"
#include "ModuleLoadingContext.h"
#include "ModuleRegistryEntry.h"
#include "ObjectConstructor.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"
#include <algorithm>
#include <limits>
#include <ranges>
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
        // The loader of `bun run` has no status for a CommonJS file. An importer gets what a running file has exported so far.
        if (moduleStatus == ModuleStatus::Evaluating && isCommonJSFileOrFace(module))
            return ImportWalkStep::Skip;
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
            if (auto* compiled = dynamicDowncast<ModuleRecord>(record); compiled && compiled->isOf(modules.instance()))
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

    runMocksBelow(root);
    RETURN_IF_EXCEPTION(scope, void());
    noteModulesLoadedWith(root);
    MocksBelowRoot mocks = mocksBelow(root);

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
        if (mocks.hides(module)) [[unlikely]]
            return false;
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
        if (isMocked(module)) [[unlikely]] {
            setStatus(module, ModuleStatus::Evaluated);
            return false;
        }

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
            if (compiled && !compiled->isOf(m_instance))
                compiled = nullptr;
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
        bool hadCapturedExports = didCaptureExports(frame.module);
        if (record && (*record->pendingAsyncDependencies() > 0 || moduleData.hasTopLevelAwait())) {
            record->setAsyncEvaluationOrder(vm.incrementModuleAsyncEvaluationCount());
            if (!*record->pendingAsyncDependencies())
                record->executeAsync(realm);
        } else
            callBodyUnderOwnMark(frame.module, moduleData.evaluatesItself());
        RETURN_IF_EXCEPTION(scope, void());
        if (moduleData.capturesExportsBehindEvaluation()) [[unlikely]] {
            if (!hadCapturedExports && didCaptureExports(frame.module))
                copyExportsOfFinishedFile(frame.module);
            else
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
            if (!compiled || !compiled->isOf(m_instance) || !isEvaluated)
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
                if (mocks.hides(required)) [[unlikely]]
                    continue;
                const ModuleData& requiredData = data(required);
                // It was linked with its importer, whether or not a status was stored for it.
                bool isLinked = stored == StoredModuleStatus::Linked || stored == StoredModuleStatus::Unlinked;
                if (isLinked && isMocked(required)) [[unlikely]] {
                    setStoredStatus(required, StoredModuleStatus::Evaluated);
                    continue;
                }
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
    if (!body || isMocked(module))
        return jsUndefined();

    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    ClosureTable& closures = m_state.objects->closures;
    if (validatesRuntime(vm) && !std::exchange(m_hasAskedEveryClass, true)) [[unlikely]]
        m_state.objects->classes.askEveryClassForItsPrototype(vm);
    SetForScope isAskingHost(m_isAskingHost, false);
    if (!moduleData.isOfLibrary())
        didStartThread(&vm);
    // Nothing in a file of the program names the function that is its body, so the body needs no closure of its own: the entry takes the closure number of the function.
    if ((moduleData.kind == ModuleKind::SourceText && !moduleData.hasTopLevelAwait()) || moduleData.kind == ModuleKind::CommonJs) {
        EncodedJSValue function = JSValue::encode(jsNumber(static_cast<int32_t>(closureNumberOf(*body))));
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
    if (ModuleID file = m_table.fileBehindFace(module).value_or(module); data(file).mayBeImportedWhileItRuns()) [[unlikely]]
        ensureLoaded(file).synchronousLoadCountAtStart = m_synchronousLoadCount;
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
    // `bun run` takes a CommonJS file whose body throws out of require.cache, and the next require() runs it again. A test run loads it again from disk where its build
    // has compiled the code that asks for the file for a file that may load twice.
    if (data(module).kind == ModuleKind::CommonJs && mayBeEvicted(module) && status(module) == ModuleStatus::Evaluating && !loaderHasFileThatThrew(module)) [[unlikely]] {
        evict(module);
        return;
    }
    if (data(module).keepsError())
        setFailed(module, error);
    else
        setStatus(module, ModuleStatus::Evaluated);
    // Whoever runs the file hears of the error. Whoever asks later gets what the loader has.
    if (isCommonJSFileOrFace(module) && wasImportedWhileItRan(m_table.fileBehindFace(module).value_or(module)))
        dropErrorOfFile(module);
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
    if (m_filesToLoadAgain.get(module.index())) [[unlikely]] {
        loadEvictedFile(module);
        return false;
    }
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

void Modules::forEachFileEvaluatedByLoader(const Invocable<void(ModuleID)> auto& functor) const
{
    forEachFileOrFaceEvaluatedByLoader([&](ModuleID fileOrFace) {
        functor(fileOf(fileOrFace));
    });
}

void Modules::forEachFileOrFaceEvaluatedByLoader(const Invocable<void(ModuleID)> auto& functor) const
{
    auto isBeingEvaluated = [&](ModuleID fileOrFace) {
        return status(fileOrFace) == ModuleStatus::Evaluating || status(fileOf(fileOrFace)) == ModuleStatus::Evaluating;
    };
    for (const FilesBeingRun* load = m_innermostFilesBeingRun; load; load = load->outer) {
        if (load->fileOrFace) {
            if (isBeingEvaluated(*load->fileOrFace))
                functor(*load->fileOrFace);
            continue;
        }
        // The start calls the files in order, so any later file that is being evaluated was required by the first one.
        for (const ModuleRequestData& request : m_table.requests(load->root)) {
            if (request.isRequire)
                continue;
            ModuleID imported = request.targetModule();
            if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
                if (request.isHoisted)
                    continue;
                break;
            }
            if (isBeingEvaluated(imported)) {
                functor(imported);
                break;
            }
        }
    }
}

// `bun run` evaluates a CommonJS file inside JSModuleLoader::makeModule(), while its registry entry is still being fetched: an asynchronous load waits for the entry, a synchronous one makes the record again.
bool Modules::asyncLoadMustWaitFor(ModuleID fileOrFace) const
{
    return !m_instance.vm->synchronousModuleQueue() && isEvaluatedByLoader(fileOf(fileOrFace));
}

bool Modules::isEvaluatedByLoader(ModuleID file) const
{
    bool found = false;
    forEachFileEvaluatedByLoader([&](ModuleID evaluated) {
        found |= evaluated == file;
    });
    return found;
}

bool Modules::importsFileBeingEvaluated(ModuleID root) const
{
    if (!m_innermostFilesBeingRun) [[likely]]
        return false;
    for (const ModuleRequestData& request : m_table.requests(root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
            if (request.isHoisted)
                continue;
            return false;
        }
        if (asyncLoadMustWaitFor(imported))
            return true;
    }
    return false;
}

bool Modules::hasKey(ModuleID file, const Identifier& key)
{
    std::optional<ModuleID> face = m_table.faceOfFile(file);
    return moduleKey(file) == key || (face && moduleKey(*face) == key);
}

std::optional<ModuleID> Modules::fileToWaitFor(const Identifier& key)
{
    if (!m_innermostFilesBeingRun || m_instance.vm->synchronousModuleQueue()) [[likely]]
        return std::nullopt;
    std::optional<ModuleID> found;
    forEachFileEvaluatedByLoader([&](ModuleID file) {
        if (file != m_fileBeingHandedToLoader && hasKey(file, key))
            found = file;
    });
    return found;
}

JSPromise* Modules::deferRequest(ModuleID file, ModuleLoadingContext* context, bool useImportMap)
{
    VM& vm = *m_instance.vm;
    JSPromise* promise = JSPromise::create(vm, m_instance.globalObject->promiseStructure());
    promise->markAsHandled();
    m_deferredRequests.append(DeferredRequest { file, Strong<JSCell>(vm, context), Strong<JSPromise>(vm, promise), useImportMap });
    return promise;
}

bool Modules::hasDeferredLoads(ModuleID fileOrFace) const
{
    if (!hasDeferredLoads()) [[likely]]
        return false;
    ModuleID file = fileOf(fileOrFace);
    return m_deferredRequests.containsIf([&](const DeferredRequest& request) { return request.file == file; })
        || m_deferredImports.containsIf([&](const DeferredImport& import) { return import.file == file; });
}

void Modules::resumeDeferredRequests(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<DeferredRequest, 1> requests;
    m_deferredRequests.removeAllMatching([&](DeferredRequest& request) {
        if (request.file != file)
            return false;
        requests.append(WTF::move(request));
        return true;
    });
    SetForScope isAskingHost(m_isAskingHost, true);
    for (DeferredRequest& request : requests) {
        auto* context = uncheckedDowncast<ModuleLoadingContext>(request.context.get());
        JSPromise* promise = request.promise.get();
        JSPromise* loaded = loader()->hostLoadImportedModule(m_instance.globalObject, context->referrer(), context->moduleRequest(), context->payload(), context->scriptFetcher(), request.useImportMap, promise);
        if (scope.exception()) [[unlikely]] {
            promise->rejectWithCaughtException(vm, scope);
            RETURN_IF_EXCEPTION(scope, void());
        } else if (loaded != promise)
            promise->pipeFrom(vm, loaded);
    }
}

std::optional<ModuleID> Modules::fileWithDeferredLoads(const Identifier& key)
{
    for (const DeferredRequest& request : m_deferredRequests) {
        if (hasKey(request.file, key))
            return request.file;
    }
    for (const DeferredImport& import : m_deferredImports) {
        if (hasKey(import.file, key))
            return import.file;
    }
    return std::nullopt;
}

void Modules::didFailToMakeModule(const Identifier& key)
{
    if (std::optional<ModuleID> file = fileWithDeferredLoads(key))
        resumeDeferredRequests(*file);
}

void Modules::didRequestModule(const Identifier& key, JSCell* payload)
{
    std::optional<ModuleID> file = fileWithDeferredLoads(key);
    if (!file)
        return;
    // From the request to ModuleLoadingContext::Step::Cached.
    constexpr uint8_t jobsUntilLoaded = 6;
    resumeDeferredImports(*file, jobsUntilLoaded);
    if (!payload->inherits<ModuleGraphLoadingState>())
        return resumeDeferredRequests(*file);
    for (DeferredRequest& request : m_deferredRequests) {
        if (request.file == *file && !request.importerLoad)
            request.importerLoad.set(*m_instance.vm, payload);
    }
}

void Modules::didSettleLoad(const ModuleGraphLoadingState& load)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<ModuleID, 1> files;
    for (const DeferredRequest& request : m_deferredRequests) {
        if (request.importerLoad.get() == &load && !files.contains(request.file))
            files.append(request.file);
    }
    for (ModuleID file : files) {
        resumeDeferredRequests(file);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void Modules::resumeDeferredLoadsWithoutImporter(ModuleID fileOrFace)
{
    if (!hasDeferredLoads(fileOrFace)) [[likely]]
        return;
    ModuleID file = fileOf(fileOrFace);
    if (m_deferredRequests.containsIf([&](const DeferredRequest& request) { return request.file == file && request.importerLoad; }))
        return;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Exception* exception = scope.exception();
    if (exception) {
        if (vm.isTerminationException(exception))
            return;
        TRY_CLEAR_EXCEPTION(scope, void());
    }
    auto rethrow = makeScopeExit([&] {
        if (!scope.exception() && exception)
            throwException(m_instance.globalObject, scope, exception);
    });
    // From the end of makeModule() to ModuleLoadingContext::Step::Cached.
    constexpr uint8_t jobsUntilLoaded = 4;
    resumeDeferredImports(file, jobsUntilLoaded);
    if (!m_deferredRequests.containsIf([&](const DeferredRequest& request) { return request.file == file; }))
        return;
    ModuleRecord* record = recordOfCommonJSFile(m_table.faceOfFile(file).value_or(file));
    RETURN_IF_EXCEPTION(scope, void());
    loader()->provideFetch(m_instance.globalObject, record->moduleKey(), ScriptFetchParameters::Type::JavaScript, record);
    RETURN_IF_EXCEPTION(scope, void());
    resumeDeferredRequests(file);
}

void Modules::resumeDeferredLoadsOfFinishedFiles()
{
    if (!hasDeferredLoads()) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<ModuleID, 2> files;
    auto appendIfFinished = [&](ModuleID file) {
        if (!isEvaluatedByLoader(file) && !files.contains(file))
            files.append(file);
    };
    for (const DeferredRequest& request : m_deferredRequests)
        appendIfFinished(request.file);
    for (const DeferredImport& import : m_deferredImports)
        appendIfFinished(import.file);
    for (ModuleID file : files) {
        resumeDeferredLoadsWithoutImporter(file);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

std::pair<Modules*, ModuleID> Modules::placeInRealm(ModuleID module)
{
    constexpr uint32_t isNotAsked = 0;
    constexpr uint32_t isNotBelow = 1;
    std::pair<Modules*, ModuleID> here { this, module };
    if (!data(module).isUncompiledCode())
        return here;
    Vector<uint32_t>& files = m_instance.vm->aotState().parts[m_instance.program->position()].filesInImagesBelow;
    if (files.isEmpty())
        files.fill(isNotAsked, m_table.size());
    uint32_t& file = files[module.index()];
    if (file == isNotAsked) {
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        const Identifier& key = resolvedKey(module);
        if (scope.exception()) [[unlikely]] {
            TRY_CLEAR_EXCEPTION(scope, here);
            return here;
        }
        file = isNotBelow;
        for (const Image* image = &m_instance.program->first(); image != m_instance.program && !fetchParametersOf(module); image = image->above()) {
            if (std::optional<ModuleID> found = ModuleTable(image->programTables()).findByPath(key.string())) {
                file = moduleIDForHost(*image, *found).index();
                break;
            }
        }
    }
    if (file == isNotBelow)
        return here;
    for (Instance* instance = m_instance.globalObject->aotInstance(); instance && instance != &m_instance; instance = instance->above) {
        Modules* below = instance->state->modules.get();
        if (std::optional<ModuleID> found = moduleOfImage(*instance->program, ModuleID(file)); found && below)
            return { below, *found };
    }
    return here;
}

bool Modules::isFileLeftToRun(ModuleID fileOrFace) const
{
    ModuleID file = fileOf(fileOrFace);
    std::optional<ModuleID> face = m_table.faceOfFile(file);
    return hasNotBegunAsFile(file) && (!face || hasNotBegunAsFile(*face)) && !isMocked(file) && !(face && isMocked(*face));
}

// In the order in which the loader of `bun run` comes to them: level by level, and in a level by the text of each module. The graph goes through every image of the realm.
Modules::ModulesOfRealm Modules::filesOfRealmLeftToRunBelow(ModuleID root, ModulesOfRealm& entries, ModulesOfRealm& importers)
{
    ModulesOfRealm files;
    Vector<BitVector, 4> seen(m_instance.vm->aotState().parts.size());
    auto isNew = [&](Modules& modules, ModuleID module) {
        BitVector& seenInImage = seen[modules.m_instance.program->position()];
        seenInImage.ensureSize(modules.m_table.size());
        return !seenInImage.quickSet(module.index());
    };
    auto mayLeadToFileOfItsImage = [](Modules& modules, ModuleID firstOfImage) {
        return !modules.data(firstOfImage).mayBeginLoad() || modules.hasImportedFileLeftToRun(firstOfImage);
    };
    importers.append({ this, root });
    Vector<bool, 32> importerMayLeadToFileOfItsImage { mayLeadToFileOfItsImage(*this, root) };
    isNew(*this, root);
    for (uint32_t place = 0; place < importers.size(); ++place) {
        ModuleOfRealm importer = importers[place];
        bool importerMayLead = importerMayLeadToFileOfItsImage[place];
        uint32_t level = importer.levelBelowRoot + 1;
        if (importer.modules->isMocked(importer.module))
            continue;
        for (const ModuleRequestData& request : importer.modules->m_table.requests(importer.module)) {
            if (request.isHoisted || request.isRequire)
                continue;
            if (!importerMayLead && !importer.modules->data(request.targetModule()).reachesUncompiledCode())
                continue;
            auto [modules, target] = importer.modules->placeInRealm(request.targetModule());
            if (modules->isCommonJSFileOrFace(target)) {
                if (!request.isDeferred && isNew(*modules, modules->fileOf(target)) && modules->isFileLeftToRun(target))
                    files.append({ modules, target, importer.entry, level, place });
                continue;
            }
            if (!modules->isLoadedWithRequests(target) || modules->status(target) != ModuleStatus::Unlinked || !isNew(*modules, target))
                continue;
            bool mayLead = modules == importer.modules ? importerMayLead : mayLeadToFileOfItsImage(*modules, target);
            if (!mayLead && !modules->data(target).reachesUncompiledCode())
                continue;
            uint32_t entry = importer.entry;
            if (!entry && modules != this) {
                entries.append({ modules, target, 0, level, place });
                entry = entries.size();
            }
            importers.append({ modules, target, entry, level, place });
            importerMayLeadToFileOfItsImage.append(mayLead);
        }
    }
    return files;
}

// An ES module of another image runs the files below it when the loader asks it for its requests, as long as that keeps every file in its turn.
static Modules::ModulesOfRealm requestsThatRunFilesInTurn(Modules::ModulesOfRealm&& files, const Modules::ModulesOfRealm& entries)
{
    Modules::ModulesOfRealm requests;
    BitVector isAskedFor(entries.size() + 1);
    uint32_t lastEntry = 0;
    for (const Modules::ModuleOfRealm& file : files) {
        if (file.entry == lastEntry) {
            if (!file.entry) {
                if (!requests.isEmpty() && requests.last().levelBelowRoot != file.levelBelowRoot)
                    return WTF::move(files);
                requests.append(file);
            }
            continue;
        }
        if (!file.entry || isAskedFor.quickSet(file.entry))
            return WTF::move(files);
        requests.append(entries[file.entry - 1]);
        lastEntry = file.entry;
    }
    return requests;
}

bool Modules::layWayThroughRealm(ModuleID root, FilesLeftToLoader& left, LevelOfRealm& levelOfRoot)
{
    if (left.requests.isEmpty() || m_instance.vm->synchronousModuleQueue())
        return false;
    uint32_t highestLevel = left.requests.first().levelBelowRoot;
    uint32_t lowestLevel = left.requests.last().levelBelowRoot;
    if (highestLevel == lowestLevel)
        return false;
    for (const ModuleOfRealm& request : left.requests) {
        if (!request.modules->isCommonJSFileOrFace(request.module))
            return false;
    }

    ModulesOfRealm modulesOnWay(FillWith { }, lowestLevel - highestLevel, ModuleOfRealm { nullptr, root });
    for (const ModuleOfRealm& importer : left.importers) {
        if (importer.levelBelowRoot < highestLevel)
            continue;
        if (importer.levelBelowRoot >= lowestLevel)
            break;
        ModuleOfRealm& onWay = modulesOnWay[importer.levelBelowRoot - highestLevel];
        if (onWay.modules && onWay.modules->m_instance.program->position() >= importer.modules->m_instance.program->position())
            continue;
        if (importer.modules->isNotLoaded(importer.module) && !importer.modules->m_levelsOfRealm.contains(importer.module) && !importer.modules->m_levelsOfLoads.contains(importer.module))
            onWay = importer;
    }
    for (const ModuleOfRealm& file : left.requests) {
        if (file.levelBelowRoot == highestLevel)
            continue;
        const ModuleOfRealm& asks = modulesOnWay[file.levelBelowRoot - highestLevel - 1];
        if (!asks.modules || asks.modules->m_instance.program->position() < file.modules->m_instance.program->position())
            return false;
    }
    for (size_t index = 0; index < modulesOnWay.size(); ++index) {
        if (!modulesOnWay[index].modules)
            return false;
        if (index && modulesOnWay[index - 1].modules->m_instance.program->position() < modulesOnWay[index].modules->m_instance.program->position())
            return false;
    }

    ModulesOfRealm requestsOfRoot;
    Vector<std::unique_ptr<LevelOfRealm>, 8> levels(modulesOnWay.size(), [](size_t) { return makeUnique<LevelOfRealm>(); });
    for (const ModuleOfRealm& file : left.requests) {
        if (file.levelBelowRoot == highestLevel)
            requestsOfRoot.append(file);
        else
            levels[file.levelBelowRoot - highestLevel - 1]->requests.append(file);
    }
    requestsOfRoot.append(modulesOnWay.first());
    for (size_t index = 0; index + 1 < modulesOnWay.size(); ++index)
        levels[index]->requests.append(modulesOnWay[index + 1]);
    levels.last()->importersOfUncompiledCode.append({ this, root });
    for (const ModuleOfRealm& onWay : modulesOnWay) {
        if (onWay.modules != this)
            levels.last()->importersOfUncompiledCode.append(onWay);
    }
    levelOfRoot.made = adoptRef(*new RequestsMadeOnWay);
    levelOfRoot.modulesOfRoot = this;
    for (size_t index = 0; index < modulesOnWay.size(); ++index) {
        levels[index]->modulesOfRoot = this;
        levels[index]->made = levelOfRoot.made;
        modulesOnWay[index].modules->m_levelsOfRealm.set(modulesOnWay[index].module, WTF::move(levels[index]));
    }
    m_waysThroughRealm.append({ root, WTF::move(modulesOnWay) });
    left.requests = WTF::move(requestsOfRoot);
    return true;
}

bool Modules::isOnWayBelowRootOfItsImage(ModuleID module) const
{
    if (m_levelsOfRealm.isEmpty()) [[likely]]
        return false;
    auto found = m_levelsOfRealm.find(module);
    return found != m_levelsOfRealm.end() && found->value->modulesOfRoot == this;
}

void Modules::takeUpWayThroughRealm(ModuleID root)
{
    m_waysThroughRealm.removeAllMatching([&](const WayThroughRealm& way) {
        if (way.root != root)
            return false;
        for (const ModuleOfRealm& onWay : way.modulesOnWay)
            onWay.modules->m_levelsOfRealm.remove(onWay.module);
        return true;
    });
}

// `bun run` runs an imported CommonJS file while it loads the graph of the importer. No ES module of that graph has begun by then, so a require() in the file that leads back to one evaluates it there and then.
bool Modules::evaluateImportedCommonJSFiles(ModuleID root, JobsOfImportedFiles jobs, ErrorOfImportedFile error, ModuleGraphLoadingState* load, FilesLeftToLoader* leftToLoader)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (data(root).isLoadedByLoader())
        return false;
    if (data(root).kind != ModuleKind::SourceText || (status(root) != ModuleStatus::Unlinked && status(root) != ModuleStatus::Linked))
        return false;
    std::optional<ModuleID> firstFileBehindFileOfOtherImage;
    if (error == ErrorOfImportedFile::IsLeftToLoader && data(root).reachesUncompiledCode()) {
        bool loadEndsEarly = comesToCodeThatDoesNotLoad(root);
        RETURN_IF_EXCEPTION(scope, false);
        // The loader goes through the graph, and each file runs when the loader comes to it.
        if (loadEndsEarly) {
            m_rootWhoseFilesRanAhead = root;
            return false;
        }
        if (vm.aotState().hasSeveralImages) [[unlikely]] {
            ModulesOfRealm entries;
            ModulesOfRealm importers;
            ModulesOfRealm files = filesOfRealmLeftToRunBelow(root, entries, importers);
            RETURN_IF_EXCEPTION(scope, false);
            size_t firstOfOtherImage = files.findIf([&](const ModuleOfRealm& file) { return file.modules != this; });
            if (firstOfOtherImage != notFound) {
                files.removeAt(0, firstOfOtherImage);
                if (size_t own = files.findIf([&](const ModuleOfRealm& file) { return file.modules == this; }); own != notFound)
                    firstFileBehindFileOfOtherImage = files[own].module;
                if (leftToLoader) {
                    leftToLoader->requests = requestsThatRunFilesInTurn(WTF::move(files), entries);
                    leftToLoader->importers = WTF::move(importers);
                }
            }
        }
    }

    // A file that only a mocked module imports never runs.
    MocksBelowRoot mocks = mocksBelow(root);

    // No link has come to the files. Whoever evaluates a record takes the names behind the body.
    bool didCaptureLibraryModules = false;
    for (const ModuleRequestData& request : m_table.requests(root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
            if (request.isHoisted)
                continue;
            break;
        }
        if (mocks.hides(imported)) [[unlikely]]
            continue;
        if (!std::exchange(didCaptureLibraryModules, true)) {
            captureExportsOfLibraryModules(root);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (data(imported).capturesExportsBehindEvaluation()) {
            captureExportsBehindEvaluation(imported);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }

    bool didRunFile = false;
    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    FilesBeingRun filesBeingRun { root, std::nullopt, m_innermostFilesBeingRun, jobs == JobsOfImportedFiles::WaitForLoader, load };
    filesBeingRun.isAheadOfStepsOfLoader = jobs == JobsOfImportedFiles::StepsOfLoaderWaitForLoader;
    SetForScope innermostFilesBeingRun(m_innermostFilesBeingRun, &filesBeingRun);
    SetForScope modulesWhoseFilesRunAheadOfLoader(vm.aotState().modulesWhoseFilesRunAheadOfLoader, filesBeingRun.isAheadOfJobsOfLoader ? this : vm.aotState().modulesWhoseFilesRunAheadOfLoader);
    // The start of the root calls the files one after the other, and leaves where one of them has queued a job or where the root would begin.
    if (jobs == JobsOfImportedFiles::WaitForLoader && data(root).evaluatesItself() && data(root).isEntryPoint() && queue.isEmpty() && !m_rootWhoseFilesRun && !firstFileBehindFileOfOtherImage) {
        ensureRecord(root)->retain();
        if (storedStatus(root) == StoredModuleStatus::InRecord) {
            bool hadQueuedTicks = hasQueuedTicks();
            callBodyUnderOwnMark(root, false, true);
            didRunFile = true;
            if (filesBeingRun.fileHandedToLoader) [[unlikely]] {
                ModuleID file = *filesBeingRun.fileHandedToLoader;
                if (Exception* exception = scope.exception()) {
                    if (vm.isTerminationException(exception))
                        return true;
                    TRY_CLEAR_EXCEPTION(scope, true);
                }
                JSValue errorOfFile;
                if (status(file) == ModuleStatus::Failed) {
                    if (wasImportedWhileItRan(fileOf(file)))
                        dropErrorOfFile(file);
                    else
                        errorOfFile = recordIfExists(file)->evaluationError();
                }
                finishStepInWhichFileRan(file, errorOfFile);
                return true;
            }
            if (Exception* exception = scope.exception()) [[unlikely]] {
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
                if (!queue.isEmpty()) {
                    handGraphToLoader(filesBeingRun, *failedFile);
                    RETURN_IF_EXCEPTION(scope, true);
                    if (filesBeingRun.fileHandedToLoader) {
                        JSValue errorOfFile;
                        if (wasImportedWhileItRan(fileOf(*failedFile)))
                            dropErrorOfFile(*failedFile);
                        else
                            errorOfFile = exception->value();
                        finishStepInWhichFileRan(*failedFile, errorOfFile);
                        return true;
                    }
                    holdJobsBehind(0, *failedFile);
                }
                // As in the loop below.
                if (!wasImportedWhileItRan(m_table.fileBehindFace(*failedFile).value_or(*failedFile))) {
                    m_rootWhoseFilesRanAhead = root;
                    return true;
                }
                dropErrorOfFile(*failedFile);
            }
            if (m_fileWhoseJobsWaitForLoader || hasDeferredLoads()) {
                m_rootWhoseFilesRanAhead = root;
                return true;
            }
            // A tick runs between the steps that the loader of `bun run` has left, and so do the first jobs that it queues.
            if (!hadQueuedTicks && hasQueuedTicks()) [[unlikely]] {
                std::optional<ModuleID> lastFile;
                for (const ModuleRequestData& request : m_table.imports(root)) {
                    if (!request.isHoisted)
                        break;
                    if (isCommonJSFileOrFace(request.targetModule()) && status(request.targetModule()) == ModuleStatus::Evaluated)
                        lastFile = request.targetModule();
                }
                if (lastFile) {
                    handGraphToLoader(filesBeingRun, *lastFile);
                    RETURN_IF_EXCEPTION(scope, true);
                    if (filesBeingRun.fileHandedToLoader) {
                        finishStepInWhichFileRan(*lastFile, JSValue());
                        return true;
                    }
                }
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
        if (imported == firstFileBehindFileOfOtherImage) [[unlikely]]
            break;
        if (mocks.hides(imported)) [[unlikely]]
            continue;
        ModuleStatus statusAtStart = status(imported);
        if (statusAtStart != ModuleStatus::Unlinked && statusAtStart != ModuleStatus::Linked && statusAtStart != ModuleStatus::Failed)
            continue;
        didRunFile |= statusAtStart != ModuleStatus::Failed;
        size_t numberOfJobs = numberOfJobsThatWouldWait(jobs);
        bool hadQueuedTicks = hasQueuedTicks();
        filesBeingRun.fileOrFace = imported;
        filesBeingRun.numberOfJobsInFrontOfFile = numberOfJobs;
        // As the start of the root would call it.
        if (storedStatus(imported) < StoredModuleStatus::InRecord && data(imported).evaluatesItself())
            evaluateByCallingBody(imported);
        else
            evaluateModuleInRecord(imported);
        Exception* exception = scope.exception();
        bool didQueueJobsAheadOfLoader = filesBeingRun.isAheadOfJobsOfLoader && (numberOfJobsThatWouldWait(jobs) != numberOfJobs || (!hadQueuedTicks && hasQueuedTicks()));
        if (didQueueJobsAheadOfLoader && !(exception && vm.isTerminationException(exception))) [[unlikely]] {
            TRY_CLEAR_EXCEPTION(scope, didRunFile);
            handGraphToLoader(filesBeingRun, imported);
            RETURN_IF_EXCEPTION(scope, didRunFile);
            if (exception && !filesBeingRun.fileHandedToLoader)
                throwException(m_instance.globalObject, scope, exception);
        }
        if (filesBeingRun.fileHandedToLoader) [[unlikely]] {
            JSValue errorOfFile;
            if (exception) {
                if (vm.isTerminationException(exception))
                    return didRunFile;
                TRY_CLEAR_EXCEPTION(scope, didRunFile);
                if (wasImportedWhileItRan(fileOf(imported)))
                    dropErrorOfFile(imported);
                else
                    errorOfFile = exception->value();
            }
            finishStepInWhichFileRan(imported, errorOfFile);
            return didRunFile;
        }
        bool hasDeferredLoads = this->hasDeferredLoads(imported);
        if (hasDeferredLoads) [[unlikely]] {
            // The file drained the queue.
            numberOfJobs = 0;
            if (error == ErrorOfImportedFile::IsThrown) {
                resumeDeferredLoadsWithoutImporter(imported);
                exception = scope.exception();
                hasDeferredLoads = false;
            }
        }
        if (exception) [[unlikely]] {
            // A graph that the file required has imported the file back and bound what it had exported by then. The loader has the file from then on, and nobody waits for its end.
            if (statusAtStart == ModuleStatus::Failed || vm.isTerminationException(exception) || !wasImportedWhileItRan(m_table.fileBehindFace(imported).value_or(imported))) {
                // `bun run` loads the rest of the graph, and with it runs the other files, before it reports the error. The loader goes through the graph and finds the error where it comes to the file.
                if (error == ErrorOfImportedFile::IsLeftToLoader && !vm.isTerminationException(exception)) {
                    TRY_CLEAR_EXCEPTION(scope, didRunFile);
                    if (numberOfJobsThatWouldWait(jobs) != numberOfJobs)
                        holdJobsBehind(numberOfJobs, imported, jobs);
                    m_rootWhoseFilesRanAhead = root;
                }
                return didRunFile;
            }
            TRY_CLEAR_EXCEPTION(scope, didRunFile);
            dropErrorOfFile(imported);
        }
        bool didQueueJobs = numberOfJobsThatWouldWait(jobs) != numberOfJobs;
        if (!didQueueJobs && !hasDeferredLoads) [[likely]]
            continue;
        if (didQueueJobs)
            holdJobsBehind(numberOfJobs, imported, jobs);
        m_rootWhoseFilesRanAhead = root;
        return didRunFile;
    }
    if (didRunFile)
        m_rootsLoadedWithoutLoader.append(root);
    return didRunFile;
}

// Under `bun run` these jobs run before the loader comes to a file that a deeper module imports. From here on the loader goes through the graph.
void Modules::holdJobsBehind(size_t numberOfJobs, ModuleID file, JobsOfImportedFiles jobs)
{
    VM& vm = *m_instance.vm;
    m_fileWhoseJobsWaitForLoader = m_table.fileBehindFace(file).value_or(file);
    if (jobs == JobsOfImportedFiles::StepsOfLoaderWaitForLoader) {
        // The loader opens a queue of its own for the graph, and nothing runs out of this one until then.
        ASSERT(!numberOfJobs);
        m_stepsThatWaitForLoader = vm.m_synchronousModuleQueue;
        return;
    }
    if (!m_jobsThatWaitForLoader)
        m_jobsThatWaitForLoader = MicrotaskQueue::create(vm);
    vm.defaultMicrotaskQueue().moveTasksBehind(numberOfJobs, *m_jobsThatWaitForLoader);
}

size_t Modules::numberOfJobsThatWouldWait(JobsOfImportedFiles jobs) const
{
    VM& vm = *m_instance.vm;
    switch (jobs) {
    case JobsOfImportedFiles::StayQueued:
        return 0;
    case JobsOfImportedFiles::WaitForLoader:
        return vm.defaultMicrotaskQueue().size();
    case JobsOfImportedFiles::StepsOfLoaderWaitForLoader:
        return vm.m_synchronousModuleQueue->tasks.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A file that runs ahead of the loader has the waiting jobs run, or asks the loader for a graph. Under `bun run` the file runs in a step of the loader, which has the graph of the root in its hands by then.
void Modules::handGraphsToLoader()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<FilesBeingRun*, 2> ahead;
    for (FilesBeingRun* files = m_innermostFilesBeingRun; files; files = files->outer) {
        if (files->isAheadOfJobsOfLoader)
            ahead.append(files);
    }
    for (FilesBeingRun* files : ahead | std::views::reverse) {
        handGraphToLoader(*files);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// The loader goes through the graph as it would have behind the end of the file, and takes its steps as far as the one in which the file runs. They run no code of the program: what stands in front of the file has run.
// From here on the registry and the queue are those of `bun run`, and the loader runs what is left of the files.
// A file that has finished and queued jobs is handed over too. If its jobs waited while the loader took those steps as jobs of its own, a job queued by a process.nextTick() callback would overtake them.
void Modules::handGraphToLoader(FilesBeingRun& files, std::optional<ModuleID> finishedFile)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    files.isAheadOfJobsOfLoader = false;
    std::optional<ModuleID> running = finishedFile;
    if (!running) {
        SetForScope onlyTheseFiles(m_innermostFilesBeingRun, &files);
        SetForScope noneOutside(files.outer, nullptr);
        forEachFileOrFaceEvaluatedByLoader([&](ModuleID fileOrFace) {
            running = fileOrFace;
        });
    }
    CountedImport* counted = !files.load && m_innermostCountedImportInOwnJob && m_innermostCountedImportInOwnJob->module == files.root ? m_innermostCountedImportInOwnJob : nullptr;
    if (!running || (!files.load && !counted))
        return;

    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    Ref<MicrotaskQueue> jobsOfFile = MicrotaskQueue::create(vm);
    queue.moveTasksBehind(files.numberOfJobsInFrontOfFile, jobsOfFile.get());
    m_rootWhoseFilesRanAhead = files.root;
    files.fileHandedToLoader = running;
    {
        // A termination requested meanwhile is thrown at the caller's exception check.
        DeferTerminationForAWhile deferTermination(vm);
        SetForScope fileBeingHandedToLoader(m_fileBeingHandedToLoader, fileOf(*running));
        // The steps were due before any require() of the file began.
        SetForScope noSynchronousLoad(vm.m_synchronousModuleQueueSetAside, vm.m_synchronousModuleQueue);
        if (files.load) {
            ModuleRecord* record = recordIfExists(files.root);
            files.load->forgetVisited(record);
            files.load->setDrainingInnerLoad(false);
            loader()->innerModuleLoading(m_instance.globalObject, files.load, record);
            record->didGoOnWithLoadWhileRequestsWereListed(*files.load);
        } else {
            handCountedImportsToLoader();
            if (!scope.exception())
                runJobsThatAreDue(vm, *counted->jobsOfLoader);
        }
        scope.assertNoException();
        runStepsOfLoaderInFrontOf(fileOf(*running));
        scope.assertNoException();
    }
    jobsOfFile->moveTasksBehind(0, queue);
}

// As handGraphToLoader(), for a graph that require() loads: the steps stand in the queue of that load, and evaluateSynchronously() takes the rest of them when the file has returned.
void Modules::handSynchronousLoadToLoader(FilesBeingRun& files)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    files.isAheadOfStepsOfLoader = false;
    if (!files.fileOrFace || !vm.synchronousModuleQueue() || !vm.synchronousModuleQueue()->tasks.isEmpty())
        return;
    ModuleID running = *files.fileOrFace;
    m_rootWhoseFilesRanAhead = files.root;
    files.fileHandedToLoader = running;
    // A termination requested meanwhile is thrown at the caller's exception check.
    DeferTerminationForAWhile deferTermination(vm);
    SetForScope fileBeingHandedToLoader(m_fileBeingHandedToLoader, fileOf(running));
    SetForScope isAskingHost(m_isAskingHost, true);
    takeUpWaysOfLoads();
    JSPromise* promise = loader()->loadModule(m_instance.globalObject, moduleKey(files.root), nullptr, nullptr, { ModuleLoadFlag::Evaluate });
    scope.assertNoException();
    m_synchronousLoadsHandedToLoader.append({ files.root, Strong<JSPromise>(vm, promise) });
    runSynchronousStepsInFrontOf(fileOf(running));
    scope.assertNoException();
}

void Modules::runSynchronousStepsInFrontOf(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& steps = vm.synchronousModuleQueue()->tasks;
    size_t taken = 0;
    while (taken < steps.size()) {
        VM::SynchronousModuleTask step = steps[taken++];
        if (step.task == InternalMicrotask::ModuleRegistryFetchSettled && hasKey(file, uncheckedDowncast<ModuleRegistryEntry>(step.arg2)->key()))
            break;
        runInternalMicrotask(m_instance.globalObject, vm, step.task, step.payload, step.arg0, step.arg1, step.arg2, step.arg3);
        scope.assertNoException();
    }
    steps.removeAt(0, taken);
}

void Modules::runStepsOfLoaderInFrontOf(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A termination requested meanwhile is thrown at the exception check of whoever handed the graph over.
    DeferTerminationForAWhile deferTermination(vm);
    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    Ref<MicrotaskQueue> turn = MicrotaskQueue::create(vm);
    while (!queue.isEmpty()) {
        queue.moveTasksBehind(0, turn.get());
        size_t stepsTaken = 0;
        bool hasComeToFile = !turn->allOf([&](const QueuedTask& job) {
            // As MicrotaskQueue::drain(), a retired realm's job is dropped. Its fetch step may carry the key of this realm's file.
            if (job.globalObject()->microtaskRunnability() == QueuedTaskResult::Discard) {
                ++stepsTaken;
                return true;
            }
            auto arguments = job.arguments();
            if (job.job() == InternalMicrotask::ModuleRegistryFetchSettled && hasKey(file, uncheckedDowncast<ModuleRegistryEntry>(arguments[2])->key()))
                return false;
            ++stepsTaken;
            runInternalMicrotask(job.globalObject(), vm, job.job(), job.payload(), arguments[0], arguments[1], arguments[2], arguments[3]);
            scope.assertNoException();
            return true;
        });
        if (hasComeToFile) {
            // What stood behind that step stands in front of what the steps of this turn have queued.
            Ref<MicrotaskQueue> queuedInThisTurn = MicrotaskQueue::create(vm);
            queue.moveTasksBehind(0, queuedInThisTurn.get());
            turn->moveTasksBehind(stepsTaken + 1, queue);
            queuedInThisTurn->moveTasksBehind(0, queue);
        }
        turn->clear();
        if (hasComeToFile)
            return;
    }
}

// As behind JSModuleLoader::makeModule() in the step in which the loader has fetched a module.
void Modules::finishStepInWhichFileRan(ModuleID fileOrFace, JSValue error)
{
    VM& vm = *m_instance.vm;
    ModuleID file = fileOf(fileOrFace);
    for (ModuleID requested : { m_table.faceOfFile(file).value_or(file), file }) {
        ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(requested));
        if (!entry || entry->status() != ModuleRegistryEntry::Status::Fetching)
            continue;
        JSPromise* fetched = entry->ensureFetchPromise(m_instance.globalObject);
        JSPromise* modulePromise = entry->ensureModulePromise(m_instance.globalObject);
        if (fetched->status() != JSPromise::Status::Fulfilled || modulePromise->status() != JSPromise::Status::Pending)
            continue;
        JSPromise* made = JSPromise::create(vm, m_instance.globalObject->promiseStructure());
        made->markAsHandled();
        if (error)
            made->reject(vm, error);
        else
            made->fulfill(vm, fetched->result());
        made->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleRegistryModuleSettled, modulePromise, entry);
        return;
    }
}

// Only a request to terminate is left behind, and the jobs find it.
void RuntimeHooks::runFilesThatWaitInLoader(VM& vm)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    vm.aotState().modulesWhoseFilesRunAheadOfLoader->handGraphsToLoader();
    static_cast<void>(scope.exception());
}

bool Modules::stopLoadingBehindFile(ModuleID file)
{
    if (!m_rootWhoseFilesRun)
        return false;
    if (m_innermostFilesBeingRun && m_innermostFilesBeingRun->isAheadOfJobsOfLoader)
        handGraphToLoader(*m_innermostFilesBeingRun, file);
    // The jobs that wait are those of the loader, and what the file queued stands among them.
    if (m_innermostFilesBeingRun && m_innermostFilesBeingRun->fileHandedToLoader)
        return true;
    holdJobsBehind(0, file);
    return true;
}

void Modules::queueJobsThatWaitForLoader()
{
    if (!m_fileWhoseJobsWaitForLoader)
        return;
    m_fileWhoseJobsWaitForLoader = std::nullopt;
    if (VM::SynchronousModuleQueue* steps = std::exchange(m_stepsThatWaitForLoader, nullptr)) {
        if (steps != m_instance.vm->m_synchronousModuleQueue)
            m_instance.vm->m_synchronousModuleQueue->tasks.appendVector(std::exchange(steps->tasks, { }));
        return;
    }
    m_jobsThatWaitForLoader->moveTasksBehind(0, m_instance.vm->defaultMicrotaskQueue());
}

// The loader comes to nothing more of the graph.
void Modules::didFailToLoadGraph(ModuleID root)
{
    if (!m_waysThroughRealm.isEmpty()) [[unlikely]]
        takeUpWayThroughRealm(root);
    if (m_rootWhoseFilesRanAhead != root)
        return;
    m_rootWhoseFilesRanAhead = std::nullopt;
    queueJobsThatWaitForLoader();
    resumeDeferredLoadsOfFinishedFiles();
}

void Modules::didLoadGraph(ModuleID root)
{
    if (!m_waysThroughRealm.isEmpty()) [[unlikely]]
        takeUpWayThroughRealm(root);
    if (m_rootWhoseFilesRanAhead == root) [[unlikely]] {
        m_rootWhoseFilesRanAhead = std::nullopt;
        resumeDeferredLoadsOfFinishedFiles();
    }
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
            if (RefPtr plan = std::exchange(load.planWithoutMarks, nullptr))
                markModulesOfPlan(*plan);
            else if (!std::exchange(load.hasMarksBelowRoot, true))
                markModulesBelowWalkedRoot(load.root);
        } else if (status(load.root) != ModuleStatus::Evaluating && (load.root == required || data(load.root).isImportedByModule()))
            rootsThatWait.append(load.root);
    }
    if (!loaderGoesThroughGraph && rootsThatWait.isEmpty())
        return false;

    const uint32_t isSeen = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    placesSeen[required.index()] = isSeen;
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
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (request.isHoisted || request.isRequire)
                continue;
            ModuleID target = request.targetModule();
            if (std::exchange(placesSeen[target.index()], isSeen) == isSeen)
                continue;
            // Most of what a module of a large program imports is an ES module that has begun: it is loaded, and only as a root is it in the hands of the loader.
            if (isLoadedModuleWithoutRecord(target) && !rootsThatWait.contains(target))
                continue;
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
            if (request.isHoisted || m_modulesBelowWalkedRoots.quickGet(target.index()) || isLoadedModuleWithoutRecord(target))
                continue;
            if (isNotLoaded(target)) {
                m_modulesBelowWalkedRoots.quickSet(target.index());
                modules.append(target);
            } else if (hasNotBegunAsFile(target))
                m_modulesBelowWalkedRoots.quickSet(target.index());
        }
    }
}

void Modules::markModulesOfPlan(const PlanOfLoad& plan)
{
    m_modulesBelowWalkedRoots.ensureSize(m_table.size());
    for (const PlanOfLoad::Found& found : plan.found)
        m_modulesBelowWalkedRoots.quickSet(found.module.index());
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
    // The loader itself runs the file that asks, in a step of a load: evaluateLoadedCommonJSFile() lists such a file as its own root. It has the graph of that load in its hands,
    // and the steps of this require() may settle what that graph waits for: then the graph runs before the require() returns.
    for (const FilesBeingRun* files = m_innermostFilesBeingRun; files; files = files->outer) {
        if (files->fileOrFace == files->root)
            return true;
    }
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
    return storedStatus(module) == StoredModuleStatus::Unlinked && isNotLoadedWithoutStatus(module, data(module));
}

// What lies below a root whose evaluation has begun was loaded with it.
void Modules::noteModulesLoadedWith(ModuleID root)
{
    if (!data(root).isESModuleOfProgram() || data(root).isFaceOfFile() || wasLoadedWithGraph(root))
        return;
    m_buildHasSeenWhoNamesModules &= data(root).mayBeginLoad();
    Vector<uint32_t, 32> found { m_buildHasSeenWhoNamesModules ? m_table.headOfLoadGroup(root).index() : root.index() };
    while (!found.isEmpty()) {
        uint32_t module = found.takeLast();
        if (m_buildHasSeenWhoNamesModules) [[likely]] {
            if (std::exchange(m_loadedGroups[module / 64], m_loadedGroups[module / 64] | 1ull << (module % 64)) >> (module % 64) & 1)
                continue;
            for (uint32_t head : m_table.groupsLoadedWith(ModuleID(module)))
                found.append(head);
            continue;
        }
        if (!data(ModuleID(module)).isESModuleOfProgram() || wasLoadedWithGraph(ModuleID(module)))
            continue;
        m_modulesLoadedBesideGroups.set(module);
        for (const ModuleRequestData& request : m_table.requests(ModuleID(module))) {
            if (!request.isRequire && !request.isHoisted)
                found.append(request.target());
        }
    }
}

// A module with nothing to link and nothing to run stays as the loader left it when it had loaded it. The loader takes no step for it again.
ALWAYS_INLINE bool Modules::isLeftAsLoaded(ModuleID module) const
{
    ModuleRecord* record = isCommonJSFileOrFace(module) ? nullptr : recordIfExists(module);
    return record && record->status() == CyclicModuleRecord::Status::Unlinked && !data(module).capturesExports();
}

// Most of what a large graph requests is an ES module without a record.
ALWAYS_INLINE bool Modules::isModuleWithoutRecord(const ModuleData& moduleData, StoredModuleStatus stored) const
{
    return moduleData.kind == ModuleKind::SourceText && stored != StoredModuleStatus::InRecord && !moduleData.isFaceOfFile();
}

// Such a module of the program is loaded if it has a status, and no load has come to it if it has none.
ALWAYS_INLINE bool Modules::isFoundWithoutRecord(ModuleID module, const ModuleData& moduleData, StoredModuleStatus stored)
{
    return stored == StoredModuleStatus::Unlinked && !moduleData.isOfLibrary() && isNotLoadedWithoutStatus(module, moduleData);
}

// The loader takes no step for anything else that is neither of the program nor data, but for a built-in module that it does not have.
ALWAYS_INLINE bool Modules::isNeitherOfProgramNorData(const ModuleData& moduleData, StoredModuleStatus stored) const
{
    return stored != StoredModuleStatus::InRecord && !moduleData.hasExportTable() && moduleData.kind != ModuleKind::CommonJs;
}

// Of what a plan has found.
bool Modules::isStillToBeLoaded(ModuleID module)
{
    const ModuleData& moduleData = data(module);
    StoredModuleStatus stored = storedStatus(module);
    if (isModuleWithoutRecord(moduleData, stored))
        return isFoundWithoutRecord(module, moduleData, stored);
    if (isNeitherOfProgramNorData(moduleData, stored))
        return isBuiltinModuleNewToLoader(module);
    return !wasLoadedWithEarlierGraph(module) && !isLeftAsLoaded(module);
}

// The loader of `bun run` takes four jobs from a request for a module to the job in which it asks that module for its requests. That job opens a count of its own: of the requests of the module and of
// every module that it comes to which was asked for in an earlier turn and below which something is left to hear of. Who asks first for a module hears of it one job behind the end of its count, who asks
// in the same turn one job later, and who asks later is answered at once. So all modules of a level are asked for their requests in one turn, a level takes five jobs, and a way down from the root takes
// one job more for each module on it that was asked for by a count that was not the first to ask. The most such requests on a way to each module are found level by level. What the counts of a level go
// through is gone through once for the count that comes to it first and once for all the others, however many they are.
// The search hands over, for each place, the places that it asks for, in the order of its requests.
uint32_t Modules::countStepsOfWholeGraph(const PlanOfLoad& plan, std::span<uint32_t> firstRequestOfPlace, std::span<uint32_t> allRequestedPlaces)
{
    // A level stands here as its number and one more: zero says none.
    struct Place {
        // The lowest level that a way from it leads to. A count of that level or of a lower one finds nothing below it to wait for.
        uint32_t lowestLevelBelow;
        // The most on a way from the root.
        uint32_t laterRequests;
        uint32_t levelGoneThrough;
        uint32_t firstCount;
        // Of the counts that came to it and were not the first.
        uint32_t levelComeToAgain;
        uint32_t mostLaterRequestsComeWith;
        uint32_t levelGoneThroughAgain;
        // While the lists are made: the place that asked for it last, and one more. Then: whether a count has asked for it.
        uint32_t lastAskerOrIsAskedFor;
    };
    constexpr uint32_t isAskedFor = std::numeric_limits<uint32_t>::max();
    uint32_t numberOfPlaces = plan.found.size();
    uint32_t numberOfLevels = plan.levels.size();
    Vector<Place> storageOfPlaces(numberOfPlaces);
    std::span<Place> places = storageOfPlaces.mutableSpan();
    zeroSpan(places);
    auto firstPlaceOfLevel = [&](uint32_t level) {
        return level < numberOfLevels ? plan.levels[level].first : numberOfPlaces;
    };

    // Behind the last importer of each place, then in front of the first.
    Vector<uint32_t> storageOfImportersOfPlaces;
    storageOfImportersOfPlaces.fill(0, numberOfPlaces + 1);
    std::span<uint32_t> importersOfPlace = storageOfImportersOfPlaces.mutableSpan();
    // Each once: they are gone through more than once.
    uint32_t numberOfRequests = 0;
    for (uint32_t importer = 0; importer < numberOfPlaces; ++importer) {
        uint32_t first = std::exchange(firstRequestOfPlace[importer], numberOfRequests);
        for (uint32_t target : allRequestedPlaces.subspan(first, firstRequestOfPlace[importer + 1] - first)) {
            if (std::exchange(places[target].lastAskerOrIsAskedFor, importer + 1) == importer + 1)
                continue;
            allRequestedPlaces[numberOfRequests++] = target;
            ++importersOfPlace[target];
        }
    }
    firstRequestOfPlace[numberOfPlaces] = numberOfRequests;
    std::span<const uint32_t> requestedPlaces = allRequestedPlaces.first(numberOfRequests);
    auto requestsOf = [&](uint32_t place) {
        return requestedPlaces.subspan(firstRequestOfPlace[place], firstRequestOfPlace[place + 1] - firstRequestOfPlace[place]);
    };
    // No walk comes to more at a time than there are requests, and the place that it begins with.
    Vector<uint32_t> storageOfPlacesToGoThrough(requestedPlaces.size() + 1);
    std::span<uint32_t> toGoThrough = storageOfPlacesToGoThrough.mutableSpan();
    uint32_t numberToGoThrough = 0;
    {
        for (uint32_t place = 0; place < numberOfPlaces; ++place)
            importersOfPlace[place + 1] += importersOfPlace[place];
        Vector<uint32_t> storageOfImporters(requestedPlaces.size());
        std::span<uint32_t> importers = storageOfImporters.mutableSpan();
        for (uint32_t importer = 0; importer < numberOfPlaces; ++importer) {
            for (uint32_t target : requestsOf(importer))
                importers[--importersOfPlace[target]] = importer;
        }
        for (uint32_t lowest = numberOfPlaces, level = numberOfLevels; lowest--;) {
            if (lowest < plan.levels[level - 1].first)
                --level;
            if (places[lowest].lowestLevelBelow)
                continue;
            places[lowest].lowestLevelBelow = level;
            toGoThrough[numberToGoThrough++] = lowest;
            while (numberToGoThrough) {
                uint32_t target = toGoThrough[--numberToGoThrough];
                for (uint32_t importer : importers.subspan(importersOfPlace[target], importersOfPlace[target + 1] - importersOfPlace[target])) {
                    if (places[importer].lowestLevelBelow)
                        continue;
                    places[importer].lowestLevelBelow = level;
                    toGoThrough[numberToGoThrough++] = importer;
                }
            }
        }
    }

    // The places in the order in which they are asked for. A level holds what it holds in the plan.
    Vector<uint32_t> storageOfCounts(numberOfPlaces);
    std::span<uint32_t> counts = storageOfCounts.mutableSpan();
    counts[0] = 0;
    uint32_t numberAskedFor = 1;
    // The places that counts came to again, in a list for each number of later requests: a place of a level has fewer than the level says. Each with the one behind it in its list, and one more.
    struct ComeToAgain {
        uint32_t place;
        uint32_t next;
    };
    Vector<ComeToAgain> comeToAgain;
    Vector<uint32_t> storageOfFirstComeToAgain;
    storageOfFirstComeToAgain.fill(0, numberOfLevels);
    std::span<uint32_t> firstComeToAgainWith = storageOfFirstComeToAgain.mutableSpan();
    uint32_t steps = 0;
    for (uint32_t level = 1; level <= numberOfLevels; ++level) {
        uint32_t firstPlaceOfNextLevel = firstPlaceOfLevel(level);
        for (uint32_t count : counts.subspan(firstPlaceOfLevel(level - 1), firstPlaceOfNextLevel - firstPlaceOfLevel(level - 1))) {
            uint32_t laterRequests = places[count].laterRequests;
            steps = std::max(steps, (level - 1) * PlanOfLoad::jobsOfLevel + laterRequests);
            if (places[count].lowestLevelBelow == level)
                continue;
            // The loader goes on with what it came to last.
            toGoThrough[numberToGoThrough++] = count;
            while (numberToGoThrough) {
                uint32_t importer = toGoThrough[--numberToGoThrough];
                Place& goneThrough = places[importer];
                if (goneThrough.levelGoneThrough == level) {
                    if (goneThrough.firstCount != count && (goneThrough.levelComeToAgain != level || goneThrough.mostLaterRequestsComeWith < laterRequests)) {
                        goneThrough.levelComeToAgain = level;
                        goneThrough.mostLaterRequestsComeWith = laterRequests;
                        comeToAgain.append(ComeToAgain { importer, firstComeToAgainWith[laterRequests] });
                        firstComeToAgainWith[laterRequests] = comeToAgain.size();
                    }
                    continue;
                }
                goneThrough.levelGoneThrough = level;
                goneThrough.firstCount = count;
                for (uint32_t target : requestsOf(importer)) {
                    Place& askedFor = places[target];
                    if (target < firstPlaceOfNextLevel) {
                        // What the count itself has gone through has nothing more to say to it.
                        if (askedFor.lowestLevelBelow > level && (askedFor.levelGoneThrough != level || askedFor.firstCount != count))
                            toGoThrough[numberToGoThrough++] = target;
                        continue;
                    }
                    if (askedFor.lastAskerOrIsAskedFor == isAskedFor) {
                        askedFor.laterRequests = std::max(askedFor.laterRequests, laterRequests + 1);
                        continue;
                    }
                    askedFor.lastAskerOrIsAskedFor = isAskedFor;
                    askedFor.laterRequests = laterRequests;
                    counts[numberAskedFor++] = target;
                }
            }
        }
        // Those with the most first: where one of them has been no other adds anything.
        for (uint32_t with = comeToAgain.isEmpty() ? 0 : level; with--;) {
            for (uint32_t again = std::exchange(firstComeToAgainWith[with], 0); again; again = comeToAgain[again - 1].next) {
                toGoThrough[numberToGoThrough++] = comeToAgain[again - 1].place;
                while (numberToGoThrough) {
                    uint32_t importer = toGoThrough[--numberToGoThrough];
                    if (std::exchange(places[importer].levelGoneThroughAgain, level) == level)
                        continue;
                    for (uint32_t target : requestsOf(importer)) {
                        Place& askedFor = places[target];
                        if (target >= firstPlaceOfNextLevel)
                            askedFor.laterRequests = std::max(askedFor.laterRequests, with + 1);
                        else if (askedFor.lowestLevelBelow > level && askedFor.levelGoneThroughAgain != level)
                            toGoThrough[numberToGoThrough++] = target;
                    }
                }
            }
        }
        comeToAgain.shrink(0);
    }
    return steps;
}

// Every search begins to count where the one before it ended, so what that one has written says nothing to this one.
uint32_t Modules::takeMarkOfSearch()
{
    if (m_placesSeenByPlans.size() != m_table.size() || m_firstPlaceOfNextPlan > std::numeric_limits<uint32_t>::max() - 3 - m_table.size()) {
        m_placesSeenByPlans.fill(0, m_table.size());
        m_firstPlaceOfNextPlan = 1;
    }
    return m_firstPlaceOfNextPlan++;
}

bool Modules::planLevelsOfLoad(ModuleID root, RefPtr<PlanOfLoad>* planWithoutWay)
{
    using Found = PlanOfLoad::Found;
    using Level = PlanOfLoad::Level;
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
    // The loader takes two graphs that share a module that is left to load, so the jobs of neither are counted.
    const bool othersAreCounted = planWithoutWay && !m_orderOfCountedJobs.isEmpty();
    // Most requests of a large graph lead to a module that the search has come to. The number of a module says where it stands among what was found, or that it does not.
    const uint32_t isNotFound = takeMarkOfSearch();
    const uint32_t firstPlaceFound = isNotFound + 1;
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    bool isFoundAlready = !planWithoutWay && m_planOfRootKnownToBeWatched && m_rootKnownToBeWatched == root;
    Ref<PlanOfLoad> plan = isFoundAlready ? Ref(*m_planOfRootKnownToBeWatched) : adoptRef(*new PlanOfLoad);
    auto didEndSearch = makeScopeExit([&] {
        m_firstPlaceOfNextPlan = firstPlaceFound + plan->found.size();
    });
    auto& found = plan->found;
    auto& levels = plan->levels;
    bool isSharedWithCountedImport = false;
    // For each place the places that it asks for, in the order of its requests.
    Vector<uint32_t, 64> firstRequestOfPlace;
    Vector<uint32_t, 256> requestedPlaces;
    placesSeen[root.index()] = firstPlaceFound;
    if (isFoundAlready) {
        for (uint32_t place = 1; place < found.size(); ++place) {
            if (isInHandsOfLoader(found[place].module))
                return false;
            placesSeen[found[place].module.index()] = firstPlaceFound + place;
        }
    } else {
        found.append(Found { root, 0, 0, false, false, false });
        levels.append(Level { 0, 0, 0, false });
    }
    while (!isFoundAlready) {
        Level next { static_cast<uint32_t>(found.size()), 0, 0, false };
        uint32_t firstPlaceOfNext = firstPlaceFound + next.first;
        for (uint32_t importer = levels.last().first; importer < next.first; ++importer) {
            firstRequestOfPlace.append(requestedPlaces.size());
            // What only a mocked module imports is not loaded.
            if (found[importer].isFile || isMocked(found[importer].module))
                continue;
            uint32_t laterRequests = found[importer].laterRequests;
            for (const ModuleRequestData& request : m_table.requests(found[importer].module)) {
                if (request.isHoisted || request.isRequire)
                    continue;
                ModuleID target = request.targetModule();
                uint32_t& placeSeen = placesSeen[target.index()];
                if (placeSeen >= isNotFound) {
                    if (placeSeen == isNotFound)
                        continue;
                    requestedPlaces.append(placeSeen - firstPlaceFound);
                    if (placeSeen < firstPlaceOfNext) {
                        plan->asksAcrossLevels = true;
                        continue;
                    }
                    Found& askedFor = found[placeSeen - firstPlaceFound];
                    if (askedFor.importer == importer || askedFor.laterRequests > laterRequests)
                        continue;
                    askedFor.laterRequests = laterRequests + 1;
                    if (laterRequests + 1 > next.laterRequests) {
                        next.laterRequests = laterRequests + 1;
                        next.placeOfMostLaterRequests = placeSeen - firstPlaceFound;
                    }
                    continue;
                }
                const ModuleData& targetData = data(target);
                StoredModuleStatus stored = storedStatus(target);
                bool isFound;
                bool isFile = false;
                if (isModuleWithoutRecord(targetData, stored))
                    isFound = isFoundWithoutRecord(target, targetData, stored);
                else if (isNeitherOfProgramNorData(targetData, stored))
                    isFound = isFile = isBuiltinModuleNewToLoader(target);
                else if (wasLoadedWithEarlierGraph(target) || isLeftAsLoaded(target))
                    isFound = false;
                else {
                    // Another graph that is being loaded has come to the module, or will.
                    if (isInHandsOfLoader(target))
                        return false;
                    isFile = isCommonJSFileOrFace(target) || targetData.kind == ModuleKind::Data;
                    isFound = isFile || isFileOfProgram(target);
                }
                if (!isFound) {
                    placeSeen = isNotFound;
                    continue;
                }
                placeSeen = firstPlaceFound + found.size();
                requestedPlaces.append(found.size());
                isSharedWithCountedImport |= othersAreCounted && m_modulesOfCountedImports.get(target.index());
                found.append(Found { target, importer, laterRequests, isFile, false, false });
                next.hasFiles |= isFile;
                if (laterRequests > next.laterRequests) {
                    next.laterRequests = laterRequests;
                    next.placeOfMostLaterRequests = found.size() - 1;
                }
            }
            if (isSharedWithCountedImport)
                return false;
        }
        if (next.first == found.size())
            break;
        levels.append(next);
    }
    // Another graph that is being loaded will come to a module without a record that was found. Few modules wait on the ways of other graphs.
    for (ModuleID onWay : m_levelsOfLoads.keys()) {
        if (placesSeen[onWay.index()] >= isNotFound && isModuleWithoutRecord(data(onWay), storedStatus(onWay)) && isFoundWithoutRecord(onWay, data(onWay), storedStatus(onWay)))
            return false;
    }

    if (plan->asksAcrossLevels && !isFoundAlready) {
        firstRequestOfPlace.append(requestedPlaces.size());
        plan->stepsOfWholeGraph = countStepsOfWholeGraph(plan.get(), firstRequestOfPlace.mutableSpan(), requestedPlaces.mutableSpan());
    }
    if (planWithoutWay) {
        *planWithoutWay = plan.ptr();
        return true;
    }
    layWayOfLoad(plan.get(), nullptr);
    // What is found is what lies below the root and is not loaded. It is marked when a require() asks, where one can come between.
    bool marksWait = false;
    for (LoadInFlight& load : m_loadsInFlight) {
        if (load.root != root)
            continue;
        load.hasMarksBelowRoot = true;
        load.planWithoutMarks = plan.ptr();
        marksWait = true;
    }
    if (!marksWait)
        markModulesOfPlan(plan.get());
    return true;
}

// From the lowest level up to the one below a level that the loader has come to, or up to the root.
void Modules::layWayOfLoad(PlanOfLoad& plan, LevelOfLoad* above)
{
    using Found = PlanOfLoad::Found;
    auto& found = plan.found;
    auto& levels = plan.levels;
    size_t highestLevel = above ? above->depth + 1 : 0;

    // The way leads to a module of the lowest level where something is left, or to a file where that level has no module.
    const Found* onWay = nullptr;
    while (levels.size() > highestLevel) {
        for (Found& each : plan.level(levels.size() - 1)) {
            if ((onWay && each.isFile) || !isLeftToLoad(plan, each))
                continue;
            onWay = &each;
            if (!each.isFile)
                break;
        }
        if (onWay)
            break;
        found.shrink(levels.last().first);
        levels.removeLast();
    }

    std::optional<ModuleID> below;
    Vector<LevelOfLoad::FileOfNextLevel, 4> filesInFront;
    Vector<LevelOfLoad::FileOfNextLevel, 4> filesBehind;
    for (size_t depth = levels.size(); depth-- > highestLevel; onWay = &found[onWay->importer]) {
        auto planned = makeUnique<LevelOfLoad>();
        planned->next = below;
        planned->filesInFrontOfNext = std::exchange(filesInFront, { });
        planned->filesBehindNext = std::exchange(filesBehind, { });
        planned->plan = &plan;
        planned->depth = depth;
        planned->place = onWay - found.span().data();
        if (levels[depth].hasFiles) {
            // Where a way was laid again, the module where the loader stands need not import what the way goes on with.
            uint32_t placeOfModuleThatAsks = above && depth == highestLevel ? above->place : onWay->importer;
            for (Found& file : plan.level(depth)) {
                if (!file.isFile || &file == onWay || !isLeftToLoad(plan, file))
                    continue;
                if (isCommonJSFileOrFace(file.module))
                    (&file < onWay ? filesInFront : filesBehind).append({ file.module, file.importer == placeOfModuleThatAsks });
                else
                    planned->filesWithoutCode.append(file.module);
            }
        }
        m_levelsOfLoads.set(onWay->module, WTF::move(planned));
        below = onWay->module;
    }
    if (!above)
        return;
    above->next = below;
    above->filesInFrontOfNext = WTF::move(filesInFront);
    above->filesBehindNext = WTF::move(filesBehind);
}

// The loader asks for a level. The root is reported as loaded when the last of the requests so far has been heard of.
void Modules::countStepsOfLevel(PlanOfLoad& plan, size_t depth)
{
    uint32_t laterRequests = plan.levels[depth].laterRequests;
    if (laterRequests && !isLeftToLoad(plan, plan.found[plan.levels[depth].placeOfMostLaterRequests])) {
        laterRequests = 0;
        for (PlanOfLoad::Found& each : plan.level(depth)) {
            if (each.laterRequests > laterRequests && isLeftToLoad(plan, each))
                laterRequests = each.laterRequests;
        }
    }
    uint32_t stepsToLevel = depth * PlanOfLoad::jobsOfLevel;
    plan.steps = plan.isWhole && plan.stepsOfWholeGraph ? plan.stepsOfWholeGraph : std::max(plan.steps, stepsToLevel + laterRequests);
    if (ModuleRecord* root = recordIfExists(plan.found[0].module))
        root->setStepsBehindLoad(plan.steps - stepsToLevel);
}

// What is loaded is loaded with all that it imports, and not every module below it says so itself.
bool Modules::isLeftToLoad(PlanOfLoad& plan, PlanOfLoad::Found& found)
{
    using Found = PlanOfLoad::Found;
    if (plan.isWhole)
        return true;
    Found* root = &plan.found[0];
    Found* known = &found;
    while (known != root && !known->isKnownToBeLeft && !known->wasLoadedSince && isStillToBeLoaded(known->module))
        known = &plan.found[known->importer];
    bool isLeft = known == root || known->isKnownToBeLeft;
    if (!isLeft)
        known->wasLoadedSince = true;
    for (Found* each = &found; each != known; each = &plan.found[each->importer]) {
        each->isKnownToBeLeft = isLeft;
        each->wasLoadedSince = !isLeft;
        if (isLeft)
            plan.knownToBeLeft.append(each - root);
    }
    return isLeft;
}

// A require() goes through the loader, and what it loads may lie on the way of a graph that is being loaded. The ways end where the loader stands, and go on from there when it does.
void Modules::takeUpWaysOfLoads()
{
    m_levelsOfLoads.removeIf([&](auto& entry) {
        return !entry.value->isAskedFor;
    });
}

// A require() has come between. What is left of the graph is as far from the root as it was.
void Modules::layWayOfLoadAgain(LevelOfLoad& level)
{
    auto take = [&](std::optional<ModuleID> onWay) {
        return onWay ? m_levelsOfLoads.take(*onWay) : nullptr;
    };
    level.plan->isWhole = false;
    for (uint32_t place : std::exchange(level.plan->knownToBeLeft, { }))
        level.plan->found[place].isKnownToBeLeft = false;
    for (std::unique_ptr<LevelOfLoad> below = take(level.next); below; below = take(below->next)) { }
    layWayOfLoad(*level.plan, &level);
}

void Modules::generateNativeModuleForLoader(ModuleID module)
{
    if (!isNativeModule(module) || didCaptureExports(module))
        return;
    ensureLoaded(module).isBeingLoadedByLoader = true;
    captureExports(module);
}

void Modules::evaluateFilesOfLevel(ModuleID moduleOnWay, bool runsWhenLoaded)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (runsWhenLoaded)
        evaluateLoadedCommonJSFile(moduleOnWay);
    else
        generateNativeModuleForLoader(moduleOnWay);
    scope.release();
    auto found = m_levelsOfLoads.find(moduleOnWay);
    if (found == m_levelsOfLoads.end()) [[likely]]
        return;
    for (ModuleID file : std::exchange(found->value->filesWithoutCode, { }))
        m_filesLoadedWithoutLoader.set(file.index());
    // The loader asks a file for no requests, nor the module that stands for one: it has run by now.
    if (!isLoadedWithRequests(moduleOnWay) || isCommonJSFileOrFace(moduleOnWay)) {
        m_filesLoadedWithoutLoader.set(moduleOnWay.index());
        m_levelsOfLoads.remove(moduleOnWay);
    }
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

// A graph that the file requires comes to the file.
void Modules::noteLoadOfFile(ModuleID module, LoadOfFile load)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID file = m_table.fileBehindFace(module).value_or(module);
    // Where nothing but its face runs a file, the face alone has the status.
    if ((status(file) != ModuleStatus::Evaluating && status(module) != ModuleStatus::Evaluating) || asyncLoadMustWaitFor(file) || file == m_fileBeingHandedToLoader)
        return;
    ensureLoaded(file).wasLoadedWhileItRan = true;
    ensureLoaded(file).wasLoadedSynchronouslyWhileItRan |= !!m_instance.vm->synchronousModuleQueue();
    if (!data(file).mayBeImportedWhileItRuns() || load == LoadOfFile::IsRequested)
        return;
    // The loader takes the names of the file as they are now. Where no importer asks for one of them, only a later require() can show them.
    std::optional<ModuleID> face = m_table.faceOfFile(file);
    if (!face) {
        if (!didCaptureExports(file))
            RELEASE_AND_RETURN(scope, captureExports(file));
        return;
    }
    // An importer binds the names when it is linked. A load that fails before that keeps them too, if a require() inside the running file started it:
    // the steps of any other load reach the file after its body has returned.
    std::optional<uint32_t> countAtStart = ensureLoaded(file).synchronousLoadCountAtStart;
    // A face whose importers bind no name of the file copies nothing.
    if (load != LoadOfFile::IsRootOfImport && countAtStart && m_activeSynchronousLoad > *countAtStart && data(*face).capturesExports() && !didCaptureExports(*face))
        RELEASE_AND_RETURN(scope, captureExports(*face));
}

ModuleRecord* Modules::recordOfCommonJSFile(ModuleID module, LoadOfFile load)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (load != LoadOfFile::IsAskedFor) {
        noteLoadOfFile(module, load);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
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
        RELEASE_AND_RETURN(scope, noteLoadOfFile(module));
    // A require() started the file, and its face has not begun.
    noteLoadOfFile(module);
    RETURN_IF_EXCEPTION(scope, void());
    // Another load comes to the file before the loader has heard that the file threw. The file does not run again, and the record is made of what it has left.
    if (statusAtStart == ModuleStatus::Failed && loader()->isMakingRecordInline(moduleKey(module)))
        return;
    if (statusAtStart != ModuleStatus::Unlinked && statusAtStart != ModuleStatus::Linked && statusAtStart != ModuleStatus::Failed)
        return;
    {
        FilesBeingRun filesBeingRun { module, module, m_innermostFilesBeingRun };
        SetForScope innermostFilesBeingRun(m_innermostFilesBeingRun, &filesBeingRun);
        // As the start of an importer would call it.
        if (storedStatus(module) < StoredModuleStatus::InRecord && data(module).evaluatesItself())
            evaluateByCallingBody(module);
        else
            evaluateModuleInRecord(module);
    }
    Exception* exception = scope.exception();
    if (hasDeferredLoads(module)) [[unlikely]] {
        resumeDeferredLoadsWithoutImporter(module);
        exception = scope.exception();
    }
    if (!exception) [[likely]] {
        // The loader takes the names of a file when the file has run. A file that no module of the image imports has them itself.
        if (data(module).kind == ModuleKind::CommonJs && !m_table.faceOfFile(module) && !didCaptureExports(module))
            RELEASE_AND_RETURN(scope, captureExports(module));
        return;
    }
    if (statusAtStart == ModuleStatus::Failed || vm.isTerminationException(exception) || !wasImportedWhileItRan(m_table.fileBehindFace(module).value_or(module)))
        return;
    TRY_CLEAR_EXCEPTION(scope, void());
    dropErrorOfFile(module);
}

void Modules::didLoseErrorOfModule(const Identifier& key)
{
    std::optional<ModuleID> module = findByKey(key);
    if (!module || !isCommonJSFileOrFace(*module))
        return;
    ModuleID file = fileOf(*module);
    ensureLoaded(file).wasReachedBySecondLoad = true;
    if (std::optional<ModuleID> face = m_table.faceOfFile(file))
        dropErrorOfFile(*face);
    dropErrorOfFile(file);
}

// The loader has the file, and nobody waits for its end. `bun run` forgets that the file was required and keeps what the loader has: every later require() of the file answers with the namespace object of that import.
void Modules::dropErrorOfFile(ModuleID fileOrFace)
{
    ModuleID file = m_table.fileBehindFace(fileOrFace).value_or(fileOrFace);
    if (data(file).mayBeImportedWhileItRuns())
        ensureLoaded(file).isAnsweredWithNamespace = true;
    captureNoExports(file);
    if (status(fileOrFace) != ModuleStatus::Failed)
        return;
    recordIfExists(fileOrFace)->setEvaluationError(*m_instance.vm, JSValue());
    setStatus(fileOrFace, ModuleStatus::Evaluated);
    ensureLoaded(fileOrFace).didDropError = true;
}

// A file that threw has no names left. An importer that was linked while the file ran keeps what it bound, and so does the record of that link, unless another load has made an empty one since.
void Modules::captureNoExports(ModuleID file)
{
    bool hasEmptyRecord = ensureLoaded(file).wasReachedBySecondLoad;
    // The file itself has names for whoever asks for its namespace object.
    for (ModuleID module : { m_table.faceOfFile(file).value_or(file), file }) {
        if ((module != file && !data(module).capturesExports()) || (!hasEmptyRecord && didCaptureExports(module)))
            continue;
        LoadedModule& loaded = ensureLoaded(module);
        loaded.capturedFields.set(*this, nullptr);
        loaded.deferredExportsSource.set(*this, nullptr);
        loaded.capturedExports.set(*this, createTableOfExports());
        for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(module)) {
            if (syntheticExport.hasVariable && syntheticExport.absentIsUndefined)
                ProgramStatics::writeVariable(m_instance, syntheticExport.variable, jsUndefined());
        }
    }
}

// Of a file whose body has just thrown. The file ran in front of every ES module that imports it, so one that has begun was loaded by the file itself.
bool Modules::wasImportedWhileItRan(ModuleID file) const
{
    if (LoadedModule* loaded = loadedIfExists(file); loaded && (loaded->wasLoadedWhileItRan || loaded->wasReachedBySecondLoad))
        return true;
    return std::ranges::any_of(m_table.importsOfHeldFile(file), [&](const ImportOfHeldFileData& import) {
        return status(ModuleID(import.importer)) > ModuleStatus::Linked;
    });
}

// The throw takes the file out of require.cache and leaves the loader of ES modules as it is. A require() that finds nothing in the cache looks there before it reads the file,
// and answers with what the loader has: the error of the file, or its namespace object.
bool Modules::loaderHasFileThatThrew(ModuleID file)
{
    if (isEvaluatedByLoader(file) || wasLoadedByImport(file))
        return true;
    // A require() runs the file. An import() that the file calls comes to the file in a later job, when the file has thrown, unless a require() of an ES module is under way: the loader takes its steps inside the call then.
    LoadedModule* loaded = loadedIfExists(file);
    return loaded && (loaded->wasLoadedSynchronouslyWhileItRan || loaded->wasReachedBySecondLoad);
}

// For an entry point, as for an imported file: see evaluateImportedCommonJSFiles().
void Modules::evaluateCommonJSForLoader(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::optional<ModuleID> face = m_table.faceOfFile(file);
    bool hadCapturedExports = face && didCaptureExports(*face);
    evaluateCommonJS(file);
    Exception* exception = scope.exception();
    if (!exception) [[likely]] {
        // `bun run` imports an entry point, so its loader takes the names of the file when the file has run. An import() finds them as they were then.
        if (face) {
            // The loader has them from then on, and takes no step for them again.
            m_filesLoadedWithoutLoader.set(face->index());
            bool wasImportedBack = !hadCapturedExports && didCaptureExports(*face);
            evaluateLoadedCommonJSFile(*face);
            RETURN_IF_EXCEPTION(scope, void());
            if (wasImportedBack) [[unlikely]]
                RELEASE_AND_RETURN(scope, copyExportsOfFinishedFile(*face));
        }
        return;
    }
    if (vm.isTerminationException(exception) || !wasImportedWhileItRan(file))
        return;
    TRY_CLEAR_EXCEPTION(scope, void());
    dropErrorOfFile(file);
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
    // An import() of the root that was called while the root ran has given it a capability, which CyclicModuleRecord::evaluate() would settle here.
    ModuleRecord* record = recordIfExists(root);
    JSPromise* capability = record ? record->topLevelCapability() : nullptr;
    if (capability && capability->status() != JSPromise::Status::Pending)
        capability = nullptr;
    if (!exception) {
        if (capability) [[unlikely]]
            capability->fulfill(vm, jsUndefined());
        return;
    }
    for (AbstractModuleRecord* member : stack) {
        auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(member);
        cyclic->setStatus(CyclicModuleRecord::Status::Evaluated);
        cyclic->setEvaluationError(vm, exception->value());
        if (auto* compiled = dynamicDowncast<ModuleRecord>(member))
            compiled->retain();
    }
    if (capability && !vm.isTerminationException(exception)) [[unlikely]]
        capability->reject(vm, exception->value());
}

JSPromise* Modules::evaluate(ModuleID root, std::optional<int64_t> referrerAsyncOrder, JSPromise* importPromise, const ModuleLinkWalk* linkWalk, bool isLinked)
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
    else if (!isLinked)
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

    if (moduleData.hasExportTable() && !moduleData.needsEvaluation()) {
        noteModulesLoadedWith(root);
        setStoredStatus(root, StoredModuleStatus::Evaluated);
    } else if (moduleData.hasExportTable() && status(root) != ModuleStatus::Evaluated) {
        evaluateWithOwnStack(root);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(realm, jsUndefined()));
}

JSPromise* Modules::evaluateMainModule(ModuleID entryPoint, JSPromise* evaluation)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!host().isImportOfMainModule || mainModuleWaitsForItsImport() || !host().isImportOfMainModule(realm, moduleIDForHost(*m_instance.program, entryPoint)))
        return evaluation;

    switch (evaluation->status()) {
    case JSPromise::Status::Pending: {
        JSPromise* capability = JSPromise::create(vm, realm->promiseStructure());
        // The main module hears of a failure in the job of the failure, and the host of the failure of the main module.
        evaluation->markAsHandled();
        m_importOfMainModule = entryPoint;
        m_evaluationOfImportOfMainModule.set(vm, evaluation);
        m_capabilityOfMainModule.set(vm, capability);
        return capability;
    }
    case JSPromise::Status::Fulfilled:
        host().runMainModule(realm, moduleIDForHost(*m_instance.program, entryPoint));
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        return evaluation;
    case JSPromise::Status::Rejected:
        return evaluation;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::runMainModuleThatWaited()
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_evaluationOfImportOfMainModule->status() != JSPromise::Status::Fulfilled)
        return;
    JSPromise* capability = m_capabilityOfMainModule.get();
    m_evaluationOfImportOfMainModule.clear();
    m_capabilityOfMainModule.clear();
    host().runMainModule(m_instance.globalObject, moduleIDForHost(*m_instance.program, m_importOfMainModule));
    if (scope.exception()) [[unlikely]] {
        capability->rejectWithCaughtException(vm, scope);
        return;
    }
    capability->fulfill(vm, jsUndefined());
}

void Modules::failMainModuleThatWaited()
{
    JSPromise* evaluation = m_evaluationOfImportOfMainModule.get();
    if (evaluation->status() != JSPromise::Status::Rejected)
        return;
    JSPromise* capability = m_capabilityOfMainModule.get();
    m_evaluationOfImportOfMainModule.clear();
    m_capabilityOfMainModule.clear();
    capability->reject(*m_instance.vm, evaluation->result());
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
    runMocksBelow(module);
    RETURN_IF_EXCEPTION(scope, void());
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
        // A module of a cycle ends as the root of the cycle does, which may fail behind an await when the module itself has run.
        if (reason == SynchronousEvaluationReason::DeferredNamespace) {
            ModuleRecord* record = recordIfExists(module);
            CyclicModuleRecord* root = record ? record->cycleRoot() : nullptr;
            if (root && root->evaluationError()) {
                if (JSPromise* capability = root->topLevelCapability())
                    capability->markAsHandled();
                throwException(realm, scope, root->evaluationError());
            }
        }
        return;
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

    if (comesToCountedImport(module)) {
        handCountedImportsToLoader();
        RETURN_IF_EXCEPTION(scope, void());
    }
    bool needsLoader = needsLoaderForRequire(module);
    // As in JSModuleLoader::loadModuleSync(): the steps of the loader run before require() returns, those that a file of the graph asks for among them. One of them resumes a module whose awaited promise is settled.
    // A read of a deferred namespace evaluates the module and takes no step of the loader: an import() that the module calls is loaded in later jobs.
    bool runsStepsOfLoader = reason == SynchronousEvaluationReason::Require;
    std::optional<VM::SynchronousModuleQueueScope> queue;
    if (runsStepsOfLoader)
        queue.emplace(*m_instance.vm);
    SetForScope activeSynchronousLoad(m_activeSynchronousLoad, ++m_synchronousLoadCount);
    auto leaveLoad = [&] {
        if (!queue)
            return;
        if (m_rootWhoseFilesRanAhead == module)
            m_rootWhoseFilesRanAhead = std::nullopt;
        // The loader has not come to the file. As where JSModuleLoader::drainSynchronousModuleQueue() leaves early.
        VM::SynchronousModuleQueue* steps = m_instance.vm->m_synchronousModuleQueue;
        if (m_stepsThatWaitForLoader == steps) {
            m_stepsThatWaitForLoader = nullptr;
            m_fileWhoseJobsWaitForLoader = std::nullopt;
        }
        for (const VM::SynchronousModuleTask& step : steps->tasks)
            realm->queueMicrotask(*m_instance.vm, step.task, step.payload, step.arg0, step.arg1, step.arg2, step.arg3);
        queue.reset();
    };
    auto didLeaveLoad = makeScopeExit(leaveLoad);
    if (!needsLoader) {
        evaluateImportedCommonJSFiles(module, m_instance.vm->synchronousModuleQueue() ? JobsOfImportedFiles::StepsOfLoaderWaitForLoader : JobsOfImportedFiles::StayQueued, ErrorOfImportedFile::IsLeftToLoader);
        RETURN_IF_EXCEPTION(scope, void());
        needsLoader = m_rootWhoseFilesRanAhead == module;
        // One of the files may have required the module.
        if (!needsLoader && status(module) != statusAtStart)
            RELEASE_AND_RETURN(scope, evaluateSynchronously(module, reason));
    }

    JSPromise* promise = nullptr;
    if (size_t handed = m_synchronousLoadsHandedToLoader.findIf([&](const LoadHandedToLoader& load) { return load.root == module; }); handed != notFound) [[unlikely]] {
        promise = m_synchronousLoadsHandedToLoader[handed].promise.get();
        m_synchronousLoadsHandedToLoader.removeAt(handed);
        SetForScope isAskingHost(m_isAskingHost, true);
        JSModuleLoader::drainSynchronousModuleQueue(realm);
    } else if (needsLoader) {
        takeUpWaysOfLoads();
        SetForScope isAskingHost(m_isAskingHost, true);
        promise = loader()->loadModuleSync(realm, moduleKey(module), nullptr, nullptr);
    } else if (statusAtStart == ModuleStatus::Unlinked && moduleData.reachesUncompiledCode()) {
        ModuleRecord* record = registerWithLoader(module);
        RETURN_IF_EXCEPTION(scope, void());
        promise = loader()->loadModuleSync(realm, record->moduleKey(), nullptr, nullptr);
    } else {
        link(module);
        RETURN_IF_EXCEPTION(scope, void());
        if (moduleData.isSynchronous()) {
            evaluateWithOwnStack(module);
            Exception* exception = scope.exception();
            if (exception) [[unlikely]] {
                if (m_instance.vm->isTerminationException(exception))
                    return;
                TRY_CLEAR_EXCEPTION(scope, void());
            }
            if (runsStepsOfLoader)
                JSModuleLoader::drainSynchronousModuleQueue(realm);
            RETURN_IF_EXCEPTION(scope, void());
            if (exception) [[unlikely]]
                throwException(realm, scope, exception);
            return;
        }
        promise = evaluate(module, std::nullopt, nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        if (runsStepsOfLoader)
            JSModuleLoader::drainSynchronousModuleQueue(realm);
    }
    leaveLoad();
    RETURN_IF_EXCEPTION(scope, void());
    retireRecordsOfSettledLoads();

    if (promise->status() == JSPromise::Status::Fulfilled)
        return;
    promise->markAsHandled();
    if (promise->status() == JSPromise::Status::Rejected)
        throwException(realm, scope, promise->result());
    else if (moduleData.hasTopLevelAwait() || (status(module) != ModuleStatus::Evaluating && status(module) != ModuleStatus::Evaluated))
        throwAsyncModuleError();
}

JSValue Modules::requireNativeModule(ModuleID module)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (JSValue cached = value(module))
        return cached;
    JSValue exports;
    if (host().requireNativeModule) {
        RequestToHostScope requestToHost(*this);
        exports = host().requireNativeModule(realm, pathString(module));
    } else if (auto* requireBuiltinModule = realm->globalObjectMethodTable()->requireBuiltinModule)
        exports = requireBuiltinModule(realm, *data(module).registryID());
    else
        throwException(realm, scope, createBuiltinModulesUnavailableError(realm));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_ASSERT(exports);
    setValue(module, exports);
    if (exports.isObject())
        m_state.builtinCells->didLoadNativeModule(RuntimeModuleID(*data(module).registryID()), asObject(exports));
    if (status(module) == ModuleStatus::Unlinked || status(module) == ModuleStatus::Linked)
        setStatus(module, ModuleStatus::Evaluated);
    return exports;
}

void Modules::forgetRunOfLibraryModule(ModuleID module)
{
    ObjectModel& objects = *m_state.objects;
    for (const StateOfLibraryModuleData& state : std::ranges::equal_range(m_instance.tables().stateOfLibraryModules(), module.index(), { }, &StateOfLibraryModuleData::module)) {
        switch (state.kind()) {
        case StateOfLibraryModuleData::Kind::Classes:
            for (uint32_t index = state.first; index < state.first + state.count; ++index) {
                if (const LayoutData* classLayout = objects.classes.classWithIndex(index))
                    objects.classes.forgetEvaluationOf(*classLayout);
            }
            break;
        case StateOfLibraryModuleData::Kind::Closures:
            objects.closures.forgetClosures(state.first, state.count);
            break;
        case StateOfLibraryModuleData::Kind::Variables:
            m_state.statics->putBack(state.first, state.count);
            break;
        }
    }
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
    if (isNativeModule(module))
        RELEASE_AND_RETURN(scope, requireNativeModule(module));

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
        if (m_libraryModulesLeftByThrow.get(module.index())) [[unlikely]] {
            m_libraryModulesLeftByThrow.clear(module.index());
            forgetRunOfLibraryModule(module);
        }
        setStatus(module, ModuleStatus::Evaluating);
        value = callBody(module);
        // Bun's registry keeps nothing of a module whose body threw: the next request runs the body again.
        if (scope.exception()) [[unlikely]] {
            m_libraryModulesLeftByThrow.set(module.index());
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
        m_buildHasSeenWhoNamesModules = false;
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

JSValue Modules::require(ModuleID target, ModuleID referrer, RequiredBy requiredBy)
{
    // Only a program that names a way from one module object to another can tell.
    if (!referrer || !m_showsWaysBetweenModuleObjects) [[likely]]
        return answerRequire(target, referrer, requiredBy);
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    recordRequire(target, referrer);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue required = answerRequire(target, referrer, requiredBy);
    RETURN_IF_EXCEPTION(scope, { });
    recordRequire(target, referrer);
    RETURN_IF_EXCEPTION(scope, { });
    return required;
}

// The loader of `bun run` holds a file once an import() of it has settled, or once a module that imports it has been loaded. A data file is evaluated at load whoever asks, so its status does not tell.
bool Modules::isHeldForImport(ModuleID target) const
{
    if (namespaceIfCreated(target, ModulePhase::Evaluation))
        return true;
    // A module of another image imports the file: the loader has asked for its record.
    if (m_instance.program->sharesRealms() && recordIfExists(target))
        return true;
    if (m_dataFilesImportedByUncompiledCode.get(target.index()))
        return true;
    return std::ranges::any_of(m_table.importsOfHeldFile(target), [&](const ImportOfHeldFileData& import) {
        ModuleID importer(import.importer);
        if (status(importer) > ModuleStatus::Linked || (m_entryPointIsLinked && data(importer).isLinkedWithEntryPoint()))
            return true;
        // The loader has loaded the importer for a module of another image.
        if (ModuleRecord* record = recordIfExists(importer); record && record->status() != CyclicModuleRecord::Status::New)
            return true;
        // A module that runs ahead of the importer, and a CommonJS file that runs while the graph of the importer is loaded.
        return data(importer).isESModuleOfProgram() && (wasLoadedWithGraph(importer) || isBelowRootWhoseFilesRun(importer));
    });
}

bool Modules::isBelowRootWhoseFilesRun(ModuleID wanted) const
{
    for (const FilesBeingRun* files = m_innermostFilesBeingRun; files; files = files->outer) {
        BitVector seen(m_table.size());
        Vector<uint32_t, 32> found { files->root.index() };
        while (!found.isEmpty()) {
            ModuleID module(found.takeLast());
            if (module == wanted)
                return true;
            if (!data(module).isESModuleOfProgram() || seen.set(module.index()))
                continue;
            for (const ModuleRequestData& request : m_table.requests(module)) {
                if (!request.isRequire && !request.isHoisted)
                    found.append(request.target());
            }
        }
    }
    return false;
}

JSValue Modules::loadEvictedFile(ModuleID file)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    // The file may take itself out of require.cache again while it loads.
    bool storesExports = m_filesToLoadAgain.get(file.index());
    bool storesStatus = storesExports && !recordIfExists(file);
    m_filesToLoadAgain.clear(file.index());
    if (storesStatus)
        setStoredStatus(file, StoredModuleStatus::Evaluating);
    // `require("./a.custom")` reads a file of a kind that nothing names as code, and the image has that form under the path, a question mark and the name of the form.
    // The host knows the file by its path, and takes a query for one more instance.
    JSString* path = pathString(file);
    if (String text = m_table.pathText(file); text.contains('?')) [[unlikely]]
        path = jsString(*m_instance.vm, text.left(text.reverseFind('?')));
    JSValue value = requireSpecifier(path, file, true);
    if (scope.exception()) [[unlikely]] {
        // A load that throws leaves nothing in the cache, and the next require() tries again.
        if (storesExports) {
            m_filesToLoadAgain.set(file.index());
            setStoredStatus(file, StoredModuleStatus::InRecord);
        }
        return { };
    }
    if (storesStatus)
        setStoredStatus(file, StoredModuleStatus::Evaluated);
    // Whoever imports the file from now on gets what this load exports.
    if (storesExports) {
        captureExportsAgain(file);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return value;
}

JSValue Modules::requireThroughCache(ModuleID target, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(target);
    if (JSValue kept = loaded.valueInRequireCache)
        return kept;
    bool isMocked = m_libraryModulesWithMock.get(target.index()) && host().replacesNativeModule && host().replacesNativeModule(m_instance.globalObject, m_table.pathText(target));
    JSValue answered = isMocked ? answerRequireWithMock(target, referrer) : answerRequire(target);
    RETURN_IF_EXCEPTION(scope, { });
    loaded.valueInRequireCache.set(*this, answered);
    return answered;
}

// The first require() behind a mock, as fetchCommonJSModule() of the host answers it.
JSValue Modules::answerRequireWithMock(ModuleID target, ModuleID referrer)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The loader, which holds what stands in place of the module, asks nobody.
    if (referrer && m_libraryModulesThatMockReplaces.get(target.index()))
        RELEASE_AND_RETURN(scope, requireSpecifier(pathString(target), referrer, true));
    // The factory of a mock may not have settled.
    if (JSObject* exports = ensureLoaded(target).exportsOfMock) {
        JSValue esModule = exports->getIfPropertyExists(realm, vm.propertyNames->__esModule);
        RETURN_IF_EXCEPTION(scope, { });
        if (esModule && esModule.toBoolean(realm)) {
            JSValue defaultExport = exports->getIfPropertyExists(realm, vm.propertyNames->defaultKeyword);
            RETURN_IF_EXCEPTION(scope, { });
            if (defaultExport && !defaultExport.isUndefined())
                return defaultExport;
        }
    }
    // An import had loaded the module when it was mocked. There the loader holds it as an ES module, so require() gets the namespace object, with the mock in it.
    RELEASE_AND_RETURN(scope, moduleNamespace(target, ModulePhase::Evaluation));
}

JSValue Modules::answerRequire(ModuleID target, ModuleID referrer, RequiredBy requiredBy)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    runMocksBelow(target);
    RETURN_IF_EXCEPTION(scope, { });
    if (isLeftToHost(target)) [[unlikely]]
        RELEASE_AND_RETURN(scope, loadEvictedFile(fileOf(target)));
    const ModuleData& targetData = data(target);
    // The library gets its own modules from the registry, where no mock stands.
    bool isRequiredByProgram = requiredBy == RequiredBy::Program || (referrer && !data(referrer).isOfLibrary());
    if (targetData.isOfLibrary() && isRequiredByProgram && isKeptInRequireCache(target)) [[unlikely]]
        RELEASE_AND_RETURN(scope, requireThroughCache(target, referrer));
    if (m_state.statics->fillsVariablesWithFiles() && (status(target) == ModuleStatus::Linking || status(target) == ModuleStatus::Evaluating))
        m_state.statics->fillVariablesOfAllFiles();

    if (targetData.isUncompiledCode()) {
        m_runsCopyFromDisk |= m_table.pathText(target).contains('?');
        RELEASE_AND_RETURN(scope, requireSpecifier(pathString(target), *targetData.referrerModule(), true));
    }
    if (!targetData.hasExportTable()) {
        if (LoadedModule* loaded = targetData.mayBeImportedWhileItRuns() ? loadedIfExists(target) : nullptr; loaded && loaded->isAnsweredWithNamespace) [[unlikely]] {
            JSObject* namespaceObject = moduleNamespace(target, ModulePhase::Evaluation, false);
            RETURN_IF_EXCEPTION(scope, { });
            // As for an ES module below.
            if (auto* names = uncheckedDowncast<ModuleNamespace>(namespaceObject); !names->hasExport(vm.propertyNames->__esModule.impl()))
                names->setESModuleMarker(true);
            return namespaceObject;
        }
        JSValue executed = execute(target);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_ASSERT(executed);
        return executed;
    }

    willRequireESModule(target);
    LoadedModule& loaded = ensureLoaded(target);
    // In plain Bun the cache holds a module object with empty exports until the loader's require() returns. The jobs of the loader that it runs behind the evaluation can run a file that requires the module.
    if (loaded.isBeingRequired) {
        if (!loaded.exportsWhileRequired)
            loaded.exportsWhileRequired.set(*this, constructEmptyObject(m_instance.globalObject));
        return loaded.exportsWhileRequired.get();
    }
    // The calls that the build has bound and the loader each keep their own answer: LoadedModule::requiredValue is that of the calls, and the namespace object is made once.
    bool isAnsweredAsByLoader = requiredBy == RequiredBy::Program && targetData.answersLoaderWithNamespace();
    bool answersDefaultExport = !isAnsweredAsByLoader && (targetData.requireResult == RequireResult::DefaultExport || (targetData.requireResult == RequireResult::NamespaceIfImported && !loaded.requiredValue && !isHeldForImport(target)));
    if (loaded.isKeptByRequire && loaded.requiredValue && !isAnsweredAsByLoader && status(target) == ModuleStatus::Failed) [[unlikely]]
        return loaded.requiredValue.get();
    bool wasBeingRequired = false;
    if (status(target) != ModuleStatus::Evaluated) {
        // An evaluation that an import has begun shows the namespace object, with its bindings as they are.
        wasBeingRequired = loaded.isBeingRequired = status(target) != ModuleStatus::Evaluating;
        bool wasKeptByRequire = std::exchange(loaded.isKeptByRequire, true);
        evaluateSynchronously(target, SynchronousEvaluationReason::Require);
        loaded.isBeingRequired = false;
        if (scope.exception()) [[unlikely]] {
            loaded.isKeptByRequire = wasKeptByRequire;
            if (!wasKeptByRequire)
                loaded.objectMadeByRequire.set(*this, nullptr);
            return { };
        }
    }
    auto answer = [&](JSValue answered) {
        if (wasBeingRequired && loaded.isKeptByRequire && loaded.objectMadeByRequire) [[unlikely]]
            loaded.objectMadeByRequire->putDirect(vm, vm.propertyNames->exports, answered);
        return answered;
    };
    if (JSValue answered = loaded.requiredValue; answered && !isAnsweredAsByLoader)
        return answer(answered);

    JSValue result;
    if (answersDefaultExport) {
        std::optional<unsigned> index = m_table.findExport(target, vm.propertyNames->defaultKeyword.impl());
        RELEASE_ASSERT(index);
        result = readExport(target, *index);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        RELEASE_ASSERT(targetData.requireResult != RequireResult::ModuleExports);
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
    if (!isAnsweredAsByLoader)
        loaded.requiredValue.set(*this, result);
    return answer(result);
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

// `bun run` gives the id "." to the CommonJS file that its loader of ES modules comes to while no file has a module object: the entry point, the first such file in the graph of an entry point that is an ES module,
// or the first in the graph of an import(), unless a require() of a file came before.
std::optional<ModuleID> Modules::mainCommonJSModule()
{
    lookForMainCommonJSModule();
    return m_mainCommonJSModule;
}

void Modules::decideMainCommonJSModule(std::optional<ModuleID> file)
{
    m_mainCommonJSModule = file;
    m_isMainCommonJSModuleDecided = true;
    m_modulesFirstRequiredByCallsInProgress.clear();
    m_firstFilesOfLoads.clear();
    m_modulesSearchedForFirstFile = { };
}

// The loader asks for a graph level by level, and in a level for what each module requests in the order of the text.
std::optional<ModuleID> Modules::firstFileThatLoaderFindsBelow(ModuleID root)
{
    if (isCommonJSFileOrFace(root))
        return fileOf(root);
    // There is no file below a module that an earlier search has come to.
    m_modulesSearchedForFirstFile.ensureSize(m_table.size());
    if (!isLoadedWithRequests(root) || m_modulesSearchedForFirstFile.quickSet(root.index()))
        return std::nullopt;
    Vector<ModuleID, 16> importers { root };
    for (size_t index = 0; index < importers.size(); ++index) {
        for (const ModuleRequestData& request : m_table.requests(importers[index])) {
            if (request.isHoisted || request.isRequire || request.isDeferred)
                continue;
            ModuleID target = request.targetModule();
            // A statement of a test file that the loader links names a file of this image by its path.
            if (data(target).isUncompiledCode()) {
                auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
                Identifier key = resolvedKey(target);
                // Whoever loads the module hears that the name stands for nothing.
                if (scope.exception()) [[unlikely]]
                    scope.clearException();
                else
                    target = m_table.findByPath(key.string()).value_or(target);
            }
            if (isCommonJSFileOrFace(target)) {
                for (ModuleID importer : importers)
                    m_modulesSearchedForFirstFile.quickClear(importer.index());
                return fileOf(target);
            }
            if (isLoadedWithRequests(target) && !m_modulesSearchedForFirstFile.quickSet(target.index()))
                importers.append(target);
        }
    }
    return std::nullopt;
}

// The graph below the entry point of a realm goes through all of its images. The loader has resolved what it has come to.
std::optional<std::pair<Modules*, ModuleID>> Modules::firstFileOfRealmThatLoaderFindsBelow(ModuleID root)
{
    using Place = std::pair<Modules*, ModuleID>;
    auto placeOf = [](Modules& modules, ModuleID target) -> Place {
        if (!modules.data(target).isUncompiledCode())
            return { &modules, target };
        const Vector<Identifier>& keysOfFilesBelow = modules.m_instance.vm->aotState().parts[modules.m_instance.program->position()].keysOfFilesInImagesBelow;
        String path;
        if (target.index() < keysOfFilesBelow.size() && !keysOfFilesBelow[target.index()].isNull())
            path = keysOfFilesBelow[target.index()].string();
        else if (LoadedModule* loaded = modules.loadedIfExists(target); loaded && !loaded->resolvedKey.isNull())
            path = loaded->resolvedKey.string();
        else
            return { &modules, target };
        for (Instance* instance = modules.m_instance.globalObject->aotInstance(); instance && instance != &modules.m_instance; instance = instance->above) {
            Modules* below = instance->state->modules.get();
            if (!below)
                continue;
            if (std::optional<ModuleID> found = below->m_table.findByPath(path))
                return { below, *found };
        }
        return { &modules, target };
    };
    if (isCommonJSFileOrFace(root))
        return Place { this, fileOf(root) };
    if (!isLoadedWithRequests(root))
        return std::nullopt;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> seen;
    auto isNew = [&](const Place& place) {
        return seen.add(static_cast<uint64_t>(place.first->m_instance.program->position()) << 32 | place.second.index()).isNewEntry;
    };
    Vector<Place, 16> importers { Place { this, root } };
    isNew(importers[0]);
    for (size_t index = 0; index < importers.size(); ++index) {
        auto [modules, importer] = importers[index];
        for (const ModuleRequestData& request : modules->m_table.requests(importer)) {
            if (request.isHoisted || request.isRequire || request.isDeferred)
                continue;
            Place target = placeOf(*modules, request.targetModule());
            if (target.first->isCommonJSFileOrFace(target.second))
                return Place { target.first, target.first->fileOf(target.second) };
            if (target.first->isLoadedWithRequests(target.second) && isNew(target))
                importers.append(target);
        }
    }
    return std::nullopt;
}

// One file of a realm is the main one, or none: `owner` is null.
void Modules::decideMainCommonJSModuleOfRealm(Modules* owner, ModuleID file)
{
    for (Instance* instance = m_instance.globalObject->aotInstance(); instance; instance = instance->above) {
        if (Modules* modules = instance->state->modules.get())
            modules->decideMainCommonJSModule(modules == owner ? std::optional(file) : std::nullopt);
    }
}

// Asked in front of every require() that makes a module object, and by whoever wants to know.
void Modules::lookForMainCommonJSModule()
{
    if (m_isMainCommonJSModuleDecided)
        return;
    // The search begins at the entry point of the realm, whichever image holds it.
    if (m_otherImageHoldsEntryPointOfRealm) [[unlikely]] {
        if (Modules* modules = modulesWithEntryPointOfRealm(); modules && modules != this) {
            modules->lookForMainCommonJSModule();
            if (m_isMainCommonJSModuleDecided)
                return;
            if (modules->m_isMainCommonJSModuleDecided)
                return decideMainCommonJSModule(std::nullopt);
        }
    }
    // Nothing of the program runs before the loader has come to the first file below the entry point.
    if (m_firstModuleRequestedByHost && !m_otherImageHoldsEntryPointOfRealm && !std::exchange(m_didSearchBelowEntryPoint, true)) {
        if (m_instance.vm->aotState().hasSeveralImages) [[unlikely]] {
            if (auto file = firstFileOfRealmThatLoaderFindsBelow(*m_firstModuleRequestedByHost))
                return decideMainCommonJSModuleOfRealm(file->first, file->second);
        }
        if (std::optional<ModuleID> file = firstFileThatLoaderFindsBelow(*m_firstModuleRequestedByHost))
            return decideMainCommonJSModule(file);
    }
    for (ModuleID file : m_firstFilesOfLoads) {
        if (!wasRequestedByLoader(file))
            continue;
        // A require() that throws leaves no module object behind.
        bool someFileHasModuleObject = false;
        for (Instance* instance = m_instance.globalObject->aotInstance(); instance; instance = instance->above) {
            Modules* modules = instance->state->modules.get();
            someFileHasModuleObject |= modules && std::ranges::any_of(modules->m_modulesFirstRequiredByCallsInProgress, [&](ModuleID required) {
                return modules->status(required) != ModuleStatus::Failed;
            });
        }
        return decideMainCommonJSModuleOfRealm(someFileHasModuleObject ? nullptr : this, file);
    }
}

// The loader comes to what lies below the root some steps later.
void Modules::noteLoadOfGraph(ModuleID root)
{
    if (!m_showsWaysBetweenModuleObjects)
        return;
    lookForMainCommonJSModule();
    if (m_isMainCommonJSModuleDecided)
        return;
    if (std::optional<ModuleID> file = firstFileThatLoaderFindsBelow(root)) {
        m_firstFilesOfLoads.append(*file);
        lookForMainCommonJSModule();
    }
}

void Modules::didImportFileFromUncompiledCode(ModuleID fileOrFace)
{
    noteRequestByLoader(fileOrFace);
    noteLoadOfGraph(fileOrFace);
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
        host().createModuleObject(realm, pathString(module), moduleIDForHost(*m_instance.program, module), traits, objects);
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

// What require() keeps of a file that is no CommonJS file. `import.meta.require` of an ES module is bound to another object.
JSObject* Modules::moduleObjectMadeByRequire(ModuleID module)
{
    if (data(module).kind == ModuleKind::CommonJs)
        return moduleObject(module);
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSObject* made = ensureLoaded(module).objectMadeByRequire)
        return made;

    JSValue answered = answerRequire(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    OptionSet<ModuleObjectTrait> traits { ModuleObjectTrait::IsNoCommonJSFile };
    if (data(module).kind == ModuleKind::SourceText)
        traits.add(ModuleObjectTrait::IsESModule);
    ModuleObjects objects;
    host().createModuleObject(m_instance.globalObject, pathString(module), moduleIDForHost(*m_instance.program, module), traits, objects);
    RETURN_IF_EXCEPTION(scope, nullptr);
    objects.module->putDirect(vm, vm.propertyNames->exports, answered);
    ensureLoaded(module).objectMadeByRequire.set(*this, objects.module);
    return objects.module;
}

JSObject* Modules::mainModuleObject(ModuleID main)
{
    ModuleID file = fileOf(main);
    // Flow lists the same files: modulesThatMayBeMain().
    if (!data(file).isEntryPoint() && !m_storesPathOfMainModule)
        return nullptr;
    if (data(file).kind == ModuleKind::CommonJs)
        return mayHandOutModuleObject(file) && wasRequestedByLoader(file) ? moduleObject(file) : nullptr;
    bool hasObjectMadeByRequire = m_storesPathOfMainModule && !data(file).isOfLibrary() && m_modulesWithMarkOfRequire.get(file.index());
    return hasObjectMadeByRequire ? moduleObjectMadeByRequire(file) : nullptr;
}

// `bun run` keeps the module object of every file that a require() has named, whatever the file holds, and of every CommonJS file that its loader of ES modules has come to. Whoever asks first makes the object.
// A require() that makes it leaves the requiring file in it as the parent. A require() that returns lists it among the children of the requiring file, but for the one that has made the object of an ES module.
void Modules::recordRequire(ModuleID target, ModuleID referrer)
{
    const ModuleData& targetData = data(target);
    bool isCommonJSFile = targetData.kind == ModuleKind::CommonJs;
    if (!isCommonJSFile)
        markNamespaceAsRequired(target);
    if (!m_showsWaysBetweenModuleObjects || !(isFileOfProgram(target) || targetData.kind == ModuleKind::Data))
        return;
    if (isCommonJSFile)
        noteRequireOfFile(target);
    // A file without a module object to hand out is shown neither as a child nor as a parent. Which file is the main one hangs on the calls that come before that is decided.
    if (m_isMainCommonJSModuleDecided && (!mayHandOutModuleObject(target) || !mayHandOutModuleObject(referrer)))
        return;
    // A require() is recorded in front of the evaluation and behind it. The module has its parent from the first record on. It is a child from the second on, which a call that throws does not reach.
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    uint64_t pair = (static_cast<uint64_t>(referrer.index()) + 1) << 32 | target.index();
    auto recorded = m_recordedRequires.add(pair, RecordedRequire::Begun);
    if (recorded.isNewEntry) {
        lookForMainCommonJSModule();
        LoadedModule& required = ensureLoaded(target);
        if (required.firstRequirer || required.firstRequirerOfOtherImage || (isCommonJSFile && wasRequestedByLoader(target)))
            return;
        required.firstRequirer = referrer;
        if (targetData.kind == ModuleKind::SourceText)
            recorded.iterator->value = RecordedRequire::BegunAndMadeObjectOfESModule;
        if (!m_isMainCommonJSModuleDecided)
            m_modulesFirstRequiredByCallsInProgress.append(target);
        return;
    }
    // Code that holds the module object may have read its list of children or stored something in the place of the list. That is asked about the file at every call that returns.
    switch (recorded.iterator->value) {
    case RecordedRequire::BegunAndMadeObjectOfESModule:
        if (status(target) == ModuleStatus::Failed)
            return;
        recorded.iterator->value = RecordedRequire::ReturnedAndNotListed;
        if (!m_isMainCommonJSModuleDecided)
            decideMainCommonJSModule(std::nullopt);
        return;
    case RecordedRequire::ReturnedAndNotListed:
        recorded.iterator->value = RecordedRequire::Begun;
        return;
    case RecordedRequire::Begun: {
        if (status(target) == ModuleStatus::Failed)
            return;
        recorded.iterator->value = RecordedRequire::Returned;
        if (!m_isMainCommonJSModuleDecided)
            decideMainCommonJSModule(std::nullopt);
        LoadedModule& requirer = ensureLoaded(referrer);
        requirer.requiredModules.append(target);
        if (requirer.requirer && host().showChildren)
            RELEASE_AND_RETURN(scope, host().showChildren(m_instance.globalObject, requirer.requirer, nullptr));
        return;
    }
    case RecordedRequire::Returned: {
        // The module has run, so no code runs between this record and the one behind the call.
        LoadedModule* requirer = loadedIfExists(referrer);
        LoadedModule* required = loadedIfExists(target);
        JSObject* objectOfRequired = !required ? nullptr : isCommonJSFile ? required->requirer.get() : required->objectMadeByRequire.get();
        if (requirer && requirer->requirer && objectOfRequired && host().showChildren) {
            host().showChildren(m_instance.globalObject, requirer->requirer, objectOfRequired);
            RETURN_IF_EXCEPTION(scope, void());
        }
        m_recordedRequires.set(pair, RecordedRequire::BegunAgain);
        return;
    }
    case RecordedRequire::BegunAgain:
        recorded.iterator->value = RecordedRequire::Returned;
        return;
    }
}

void Modules::noteRequestByLoader(ModuleID fileOrFace)
{
    if (!m_showsWaysBetweenModuleObjects)
        return;
    ModuleID file = m_table.fileBehindFace(fileOrFace).value_or(fileOrFace);
    if (data(file).kind == ModuleKind::CommonJs)
        m_filesRequestedByLoader.set(file.index());
}

bool Modules::wasRequestedByLoader(ModuleID file)
{
    if (m_filesRequestedByLoader.get(file.index()))
        return true;
    bool wasRequested = status(file) > ModuleStatus::Linked;
    if (!wasRequested) {
        std::optional<ModuleID> face = m_table.faceOfFile(file);
        wasRequested = face && status(*face) > ModuleStatus::Linked;
        if (face || data(file).isImportedByModule()) {
            for (FilesBeingRun* load = m_innermostFilesBeingRun; load && !wasRequested; load = load->outer)
                wasRequested = hasReachedLevelOf(*load, file);
        }
    }
    if (wasRequested)
        m_filesRequestedByLoader.set(file.index());
    return wasRequested;
}

void Modules::noteRequireOfFile(ModuleID file)
{
    if (status(file) <= ModuleStatus::Linked && !wasRequestedByLoader(file))
        m_filesLoadedByRequire.set(file.index());
}

bool Modules::wasLoadedSynchronously(ModuleID file)
{
    if (!m_filesWithKnownLoad.set(file.index()))
        m_filesLoadedSynchronously.set(file.index(), wasFirstLoadedSynchronously(file));
    return m_filesLoadedSynchronously.get(file.index());
}

bool Modules::wasFirstLoadedSynchronously(ModuleID file)
{
    VMState::PartsOfImage& parts = m_instance.vm->aotState().parts[m_instance.program->position()];
    if (!parts.filesWithKnownFirstLoad.set(file.index()))
        parts.filesFirstLoadedSynchronously.set(file.index(), m_filesLoadedByRequire.get(file.index()) || m_instance.vm->synchronousModuleQueue() || hostLoadsSynchronously(file));
    return parts.filesFirstLoadedSynchronously.get(file.index());
}

bool Modules::hostLoadsSynchronously(ModuleID file)
{
    return host().loadsSynchronously && host().loadsSynchronously(m_instance.globalObject, moduleIDForHost(*m_instance.program, file));
}

// `bun test --isolate` drops the text that it keeps for the realms to come, and keeps that of the next load.
void Modules::forgetFirstLoad(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (m_instance.vm->aotState().parts[m_instance.program->position()].filesWithKnownFirstLoad.clear(file.index()))
        m_filesWhoseNextLoadIsFirst.set(file.index());
}

void Modules::willLoadCopyFromDisk(ModuleID module, LoadByName way)
{
    ModuleID file = fileOf(module);
    if (!m_filesWhoseNextLoadIsFirst.clear(file.index()))
        return;
    VMState::PartsOfImage& parts = m_instance.vm->aotState().parts[m_instance.program->position()];
    parts.filesWithKnownFirstLoad.set(file.index());
    parts.filesFirstLoadedSynchronously.set(file.index(), way == LoadByName::Require || m_instance.vm->synchronousModuleQueue() || hostLoadsSynchronously(file));
}

void Modules::findDistanceOfFiles(FilesBeingRun& load)
{
    load.distanceOfFiles.fill(UINT32_MAX, m_table.size());
    BitVector seen(m_table.size());
    seen.quickSet(load.root.index());
    Vector<ModuleID> importers { load.root };
    size_t endOfLevel = 1;
    uint32_t distance = 0;
    for (size_t index = 0; index < importers.size(); ++index) {
        if (index == endOfLevel) {
            ++distance;
            endOfLevel = importers.size();
        }
        for (const ModuleRequestData& request : m_table.requests(importers[index])) {
            if (request.isHoisted || request.isRequire || request.isDeferred)
                continue;
            ModuleID target = request.targetModule();
            if (isCommonJSFileOrFace(target)) {
                uint32_t& nearest = load.distanceOfFiles[fileOf(target).index()];
                nearest = std::min(nearest, distance);
            } else if (isLoadedWithRequests(target) && !seen.quickSet(target.index()))
                importers.append(target);
        }
    }
}

// The loader asks for everything that stands as far from the root before a file among it runs.
bool Modules::hasReachedLevelOf(FilesBeingRun& load, ModuleID file)
{
    auto fileBehind = [&](ModuleID fileOrFace) {
        return m_table.fileBehindFace(fileOrFace).value_or(fileOrFace);
    };
    std::optional<ModuleID> running = load.fileOrFace;
    for (const ModuleRequestData& request : m_table.requests(load.root)) {
        if (running || !request.isHoisted)
            break;
        ModuleID hoisted = request.targetModule();
        if (status(hoisted) == ModuleStatus::Evaluating || status(fileBehind(hoisted)) == ModuleStatus::Evaluating)
            running = hoisted;
    }
    if (!running)
        return false;
    running = fileBehind(*running);

    if (load.distanceOfFiles.isEmpty())
        findDistanceOfFiles(load);
    uint32_t distance = load.distanceOfFiles[file.index()];
    return distance != UINT32_MAX && distance <= load.distanceOfFiles[running->index()];
}

// The numbers of modules are those of one image. Of a file of another one the host has the module object.
void Modules::recordRequireFromOtherImage(ModuleID target, JSObject* moduleObjectOfReferrer)
{
    bool isCommonJSFile = data(target).kind == ModuleKind::CommonJs;
    if (!m_showsWaysBetweenModuleObjects || !(isFileOfProgram(target) || data(target).kind == ModuleKind::Data))
        return;
    if (isCommonJSFile)
        noteRequireOfFile(target);
    if (!mayHandOutModuleObject(target))
        return;
    lookForMainCommonJSModule();
    LoadedModule& required = ensureLoaded(target);
    if (required.firstRequirer || required.firstRequirerOfOtherImage || (isCommonJSFile && wasRequestedByLoader(target)))
        return;
    required.firstRequirerOfOtherImage.set(*this, moduleObjectOfReferrer);
    if (!m_isMainCommonJSModuleDecided)
        m_modulesFirstRequiredByCallsInProgress.append(target);
}

void Modules::recordRequireOfOtherImage(ModuleID referrer, ModuleID required)
{
    if (!m_showsWaysBetweenModuleObjects || !mayHandOutModuleObject(referrer))
        return;
    auto [modules, module] = ownerOfRequiredModule(required);
    const ModuleData& requiredData = modules->data(module);
    // As in recordRequire(). The require() that makes the module object of an ES module does not list it: that one is left out here for good.
    if (!(requiredData.kind == ModuleKind::CommonJs || requiredData.kind == ModuleKind::Data) || requiredData.isOfLibrary() || !modules->mayHandOutModuleObject(module))
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& requirer = ensureLoaded(referrer);
    if (!requirer.requiredModules.contains(required)) {
        requirer.requiredModules.append(required);
        if (requirer.requirer && host().showChildren)
            RELEASE_AND_RETURN(scope, host().showChildren(m_instance.globalObject, requirer.requirer, nullptr));
        return;
    }
    // Code that holds the module object may have taken the file out of the list. A require() that returns puts it back.
    if (!requirer.requirer || !host().showChildren)
        return;
    JSObject* objectOfRequired;
    {
        RunningImageScope imageOfRequired(m_instance.vm->aotState(), modules->m_instance.program);
        objectOfRequired = modules->moduleObjectMadeByRequire(module);
    }
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, host().showChildren(m_instance.globalObject, requirer.requirer, objectOfRequired));
}

JSObject* Modules::parentModuleObject(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (loaded && loaded->firstRequirerOfOtherImage && mainCommonJSModule() != module)
        return loaded->firstRequirerOfOtherImage;
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
        auto [modules, module] = ownerOfRequiredModule(child);
        if (!modules->mayHandOutModuleObject(module))
            return false;
    }
    // The functor may run code of the program, which may require more.
    while (loaded->numberOfChildrenShown < loaded->requiredModules.size()) {
        auto [modules, module] = ownerOfRequiredModule(loaded->requiredModules[loaded->numberOfChildrenShown++]);
        JSObject* childObject;
        {
            RunningImageScope imageOfChild(m_instance.vm->aotState(), modules->m_instance.program);
            childObject = modules->moduleObjectMadeByRequire(module);
        }
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

// In plain Bun a namespace object is extensible if require() creates it, and not if an import does: import(), or the link of a module with `import * as`.
// Compiled code creates the object on first use, so the first require() decides here.
void Modules::willRequireESModule(ModuleID module)
{
    if (m_modulesWithMarkOfRequire.get(module.index()) || namespaceIfCreated(module, ModulePhase::Evaluation))
        return;
    if (data(module).isImportedAsNamespace() && !isNotLoaded(module))
        return;
    m_modulesWithExtensibleNamespace.set(module.index());
}

// From the first require() on, the namespace object of a module without an export `__esModule` reads `true` under that name, for everybody who holds it.
void Modules::markNamespaceAsRequired(ModuleID module)
{
    if (!data(module).hasExportTable() || m_modulesWithMarkOfRequire.get(module.index()))
        return;
    willRequireESModule(module);
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

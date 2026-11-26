/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleGraph.h"
#include "AOTInliningAcrossFiles.h"
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
#include "AOTProgramStrings.h"
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeReporter.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTStubEntrypoints.h"
#include "AOTVMState.h"
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "GlobalObjectMethodTable.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
#include "JSModuleRecord.h"
#include "MicrotaskQueueInlines.h"
#include "ModuleGraphLoadingState.h"
#include "ModuleLoadingContext.h"
#include "ModuleRegistryEntry.h"
#include "ObjectConstructor.h"
#include "SyntheticModuleRecord.h"
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
    std::optional<ModuleID> rejectedRequest { };
};

using EvaluationFrames = Vector<EvaluationFrame, 16>;

struct EvaluationInProgress {
    const EvaluationFrames& frames;
    const EvaluationInProgress* outer;
};

enum class ImportWalkStep : uint8_t { Skip, Enter, Stop };

static constexpr size_t stackNeededToStartRuntimeModule = 128 * KB;
static_assert(BuiltinCells::stackKeptFromProgram >= 2 * stackNeededToStartRuntimeModule, "the first use of a built-in cell loads a module, which loads others");

static bool isComponentEvaluated(const Modules& modules, ModuleID module, Modules::ModulesThatWaitForCycles& waiting)
{
    if (ModuleRecord* record = modules.recordIfExists(module))
        return record->isSCCEvaluated();
    return modules.status(module) == ModuleStatus::Evaluated && !modules.waitsForItsCycle(module, waiting);
}

static void checkSafeToRecurse(JSGlobalObject* realm, ThrowScope& scope)
{
    if (!realm->vm().isSafeToRecurse()) [[unlikely]]
        throwException(realm, scope, createModuleGraphTooDeepError(realm));
}

static bool isPassedOverByEvaluation(const ModuleData& data)
{
    return data.hasExportTable() && (!data.needsEvaluation() || (data.isSimple() && data.isSynchronous() && !data.bodyFunction() && !data.hasDeferredRequest()));
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
    ModulesThatWaitForCycles waiting;
    walkImports(*this, start, [&](AbstractModuleRecord* record) {
        record->gatherAsynchronousTransitiveDependencies(result, seen);
        return ImportWalkStep::Skip;
    }, [&](ModuleID module, const ModuleData& moduleData) {
        if (moduleData.isSynchronous() || status(module) == ModuleStatus::Evaluating || isComponentEvaluated(*this, module, waiting))
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
    ModulesThatWaitForCycles waiting;
    return walkImports(*this, start, [&](AbstractModuleRecord* record) {
        return record->readyForSyncExecution(seen) ? ImportWalkStep::Skip : ImportWalkStep::Stop;
    }, [&](ModuleID module, const ModuleData& moduleData) {
        if (isComponentEvaluated(*this, module, waiting))
            return ImportWalkStep::Skip;
        ModuleStatus moduleStatus = status(module);
        if (moduleStatus == ModuleStatus::Evaluating && isCommonJSModuleOrFacade(module))
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
        if (request.isHoisted && modules.data(request.targetModule()).kind != ModuleKind::Data)
            continue;
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
        RELEASE_AND_RETURN(scope, evaluateCommonJS(root));

    if (!data(root).hasExportTable()) {
        ModuleRecord* record = recordIfExists(root);
        if (!record)
            return;
        if (record->evaluationError())
            RELEASE_AND_RETURN(scope, throwEvaluationError(root));
        if (record->status() != CyclicModuleRecord::Status::Linked)
            return;
        takeOutRecord(root);
        setStoredStatus(root, StoredModuleStatus::Linked);
        record->setStatus(CyclicModuleRecord::Status::Evaluated);
        record->setCycleRoot(vm, record);
        return;
    }

    bool isSweptRoot = context.stack.isEmpty() && m_lastSweptRoot == root && !m_rootWhoseLoadIsSwept;
    auto clearSweptRootOnExit = makeScopeExit([&] {
        if (!isSweptRoot)
            return;
        m_lastSweptRoot = std::nullopt;
        m_modulesPendingAfterSweep.clearAll();
    });
    if (context.stack.isEmpty())
        didBeginWalkOnEmptyStack(root);
    else if (passImporterEnteredAboveMock(root, root, context.stack)) [[unlikely]]
        return;
    runMocksInGraph(root);
    RETURN_IF_EXCEPTION(scope, void());
    noteModulesLoadedWith(root);
    noteThatGraphBegins(root);
    MocksInGraph mocks = mocksInGraph(root);
    Vector<ModuleID, 16> newlyLinked = std::exchange(m_instancesLinkedByLoader, { });
    uint32_t numberOfLinkFailures = m_numberOfLinkFailures;
    auto unlinkIfLinkFailed = makeScopeExit([&] {
        if (scope.exception() && numberOfLinkFailures != m_numberOfLinkFailures) [[unlikely]]
            unlinkInstances(root, newlyLinked.span());
    });
    if (status(root) != ModuleStatus::Unlinked && (mocks.loadsRootOriginal() || !isReplacedByMock(root))) {
        newlyLinked.appendVector(linkInstances(root));
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (!m_waitingImportersToBeLinked.isEmpty()) [[unlikely]]
        didLinkImportersThatWaitBelow(root);

    if (status(root) == ModuleStatus::Evaluating && !m_modulesAskedForWhileTheyRan.contains(root)) [[unlikely]]
        m_modulesAskedForWhileTheyRan.append(root);
    auto settleImports = makeScopeExit([&] {
        settleImportsOfModulesThatHaveRun(root);
        resetPendingModulesIfNeeded();
    });
    EvaluationFrames frames;
    EvaluationInProgress evaluation { frames, m_innermostEvaluation };
    SetForScope innermostEvaluation(m_innermostEvaluation, &evaluation);

    auto pushRecord = [&](EvaluationFrame& frame, ModuleRecord* record) {
        RELEASE_ASSERT(frame.stackPosition <= context.stack.size());
        frame.record = record;
        record->setOwnsStatus();
        setStoredStatus(frame.module, StoredModuleStatus::InRecord);
        record->setStatus(CyclicModuleRecord::Status::Evaluating);
        record->setDFSAncestorIndex(frame.dfsIndex);
        record->setPendingAsyncDependencies(0);
        record->setAsyncEvaluationOrder({ });
        record->setEvaluationStack(&context.stack);
        context.stack.insert(frame.stackPosition, static_cast<AbstractModuleRecord*>(record));
    };

    BitVector isBelowModuleThatRunsBesideMock;
    auto isHidden = [&](ModuleID module) {
        if (!mocks.hides(module)) [[likely]]
            return false;
        if (frames.isEmpty())
            return true;
        unsigned importer = frames.last().module.index();
        if (!m_originalsEvaluatedUnderMock.get(importer) && !isBelowModuleThatRunsBesideMock.get(importer))
            return true;
        isBelowModuleThatRunsBesideMock.set(module.index());
        return false;
    };

    BitVector modulesPassed;
    BitVector seenBehindModulesThatHaveRun;
    auto pass = [&](ModuleID module) {
        if (m_mockOperations) [[unlikely]]
            passModuleThatHasRun(module, modulesPassed);
        if (!m_importersEnteredAboveMocks.isEmpty() && !scope.exception()) [[unlikely]]
            evaluateImportersBehindModuleThatHasRun(module, root, context.stack, seenBehindModulesThatHaveRun);
    };

    auto recordOfImporter = [&] {
        EvaluationFrame& importer = frames.last();
        if (!importer.record)
            pushRecord(importer, ensureRecord(importer.module));
        return importer.record;
    };

    auto enter = [&](ModuleID module) {
        if (isHidden(module)) [[unlikely]]
            return false;
        const ModuleData& moduleData = data(module);
        if (moduleData.kind == ModuleKind::CommonJs) {
            evaluateCommonJS(module);
            return false;
        }
        if (!moduleData.hasExportTable())
            return false;

        bool runsBesideMock = runsBesideMockFor(module, frames.isEmpty() ? std::nullopt : std::optional(frames.last().module));
        if (runsBesideMock && status(module) == ModuleStatus::Evaluated) [[unlikely]]
            setStatus(module, ModuleStatus::Unlinked);

        ModuleStatus moduleStatus = status(module);
        if (moduleStatus == ModuleStatus::Unlinked && !frames.isEmpty())
            moduleStatus = ModuleStatus::Linked;
        else if (moduleStatus == ModuleStatus::Unlinked) {
            link(module);
            RETURN_IF_EXCEPTION(scope, false);
            moduleStatus = status(module);
            RELEASE_ASSERT(moduleStatus != ModuleStatus::Unlinked);
            if (mocks.loadsRootOriginal() || !isReplacedByMock(module)) {
                newlyLinked.appendVector(linkInstances(module));
                RETURN_IF_EXCEPTION(scope, false);
            }
            if (!m_waitingImportersToBeLinked.isEmpty()) [[unlikely]] {
                didLinkImportersThatWaitBelow(module);
                moduleStatus = status(module);
            }
        }
        if (moduleStatus == ModuleStatus::Failed)
            throwEvaluationError(module);
        if ((moduleStatus == ModuleStatus::Evaluated || moduleStatus == ModuleStatus::EvaluatingAsync) && context.index)
            pass(module);
        if (moduleStatus != ModuleStatus::Linked)
            return false;
        if (!frames.isEmpty() && status(module) == ModuleStatus::Linked && passImporterEnteredAboveMock(module, root, context.stack)) [[unlikely]]
            return false;
        if (isMocked(module) && !runsBesideMock && !isPatchedByMock(module)) [[unlikely]] {
            setStatus(module, ModuleStatus::Evaluated);
            return false;
        }
        if (runsBesideMock) [[unlikely]] {
            noteSecondInstanceOfOriginal(module);
            didEvaluateOriginalUnderMock(module);
            m_originalsDueUnderMock.clear(module.index());
            m_importsMadeByOwnFactory.removeAll(module);
            m_modulesThatOwnFactoryMayLoad.clear(module.index());
        }

        m_modulesWithResetFailure.clear(module.index());
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
        if (frame.rejectedRequest) [[unlikely]]
            return throwEvaluationError(*frame.rejectedRequest);
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
        bool isWhollyCompiled = true;
        bool hasRunToItsEnd = true;
        for (AbstractModuleRecord* member = nullptr; member != record;) {
            member = context.stack.takeLast();
            auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(member);
            bool isEvaluated = cyclic->asyncEvaluationOrder().isUnset();
            cyclic->setStatus(isEvaluated ? CyclicModuleRecord::Status::Evaluated : CyclicModuleRecord::Status::EvaluatingAsync);
            member->setCycleRoot(vm, record);
            hasRunToItsEnd &= isEvaluated;
            auto* compiled = dynamicDowncast<ModuleRecord>(member);
            if (compiled)
                compiled->didChangeStatus();
            if (!compiled)
                isWhollyCompiled = false;
            else if (!compiled->isRetained())
                recordsToRetire.append(compiled);
        }
        if (!isWhollyCompiled)
            return;
        for (ModuleRecord* member : recordsToRetire) {
            if (hasRunToItsEnd)
                retireRecord(member);
            else
                m_modulesToRetireAfterAsyncEvaluation.append(member->module());
        }
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
                if (JSValue error = releasedInstanceError(frame.module, required)) [[unlikely]] {
                    throwException(realm, scope, error);
                    continue;
                }
                if (isLinkedToReleasedInstance(frame.module, required)) [[unlikely]]
                    continue;
                bool runsBesideMock = runsBesideMockFor(required, frame.module);
                StoredModuleStatus stored = storedStatus(required);
                if (stored == StoredModuleStatus::Evaluated && !runsBesideMock) {
                    pass(required);
                    continue;
                }
                if (isHidden(required)) [[unlikely]]
                    continue;
                const ModuleData& requiredData = data(required);
                bool isLinked = !runsBesideMock && (stored == StoredModuleStatus::Linked || stored == StoredModuleStatus::Unlinked);
                if (isLinked && isMocked(required) && !isPatchedByMock(required)) [[unlikely]] {
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
            if (module && isSweptRoot && m_modulesPendingAfterSweep.get(module->index()) && status(*module) == ModuleStatus::Failed) [[unlikely]] {
                if (!frames.last().rejectedRequest)
                    frames.last().rejectedRequest = *module;
                continue;
            }
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
        if (frame.record)
            frame.record->setStatus(CyclicModuleRecord::Status::Evaluating);
        else
            setFailed(frame.module, exception->value());
    }
    if (!evaluation.outer)
        noteFailedEvaluation(root, frames.isEmpty() ? root : frames.last().module);
}

JSValue Modules::callBody(ModuleID module)
{
    const ModuleData& moduleData = data(module);
    if (m_modulesToReleaseAfterEvaluation.clear(module.index())) [[unlikely]]
        m_modulesToReleaseAtNextTestFile.set(module.index());
    std::optional<FunctionID> body = moduleData.bodyFunction();
    if (!body || (isMocked(module) && !m_originalsBeingEvaluated.get(module.index()) && !m_originalsEvaluatedUnderMock.get(module.index()) && !isPatchedByMock(module)))
        return jsUndefined();

    VM& vm = *m_instance.vm;
    SetForScope isInHostRequest(m_currentHostRequest, false);
    if (!moduleData.isOfLibrary())
        didStartThread(&vm);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_activeMockCount) [[unlikely]] {
        runOriginalsOwedTo(module);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (hasInstances(module)) [[unlikely]] {
        linkInstances(module);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, enterBody(module, *body));
}

JSValue Modules::enterBody(ModuleID module, FunctionID body)
{
#if ASSERT_ENABLED
    if (Environment* instance = currentInstance(module)) {
        for (const ModuleLinkData& link : m_table.moduleLinks(module))
            ASSERT(!link.hasMock || readField(instance, link.mock));
    }
#endif
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    ClosureTable& closures = m_state.objects->closures;
    const ModuleData& moduleData = data(module);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if ((moduleData.kind == ModuleKind::SourceText && !moduleData.hasTopLevelAwait()) || moduleData.kind == ModuleKind::CommonJs) {
        EncodedJSValue function = JSValue::encode(jsNumber(static_cast<int32_t>(closureNumberOf(body))));
        m_instance.modules.moduleThatRunTimeStartsPlusOne = module.index() + 1;
        auto forgetsStart = makeScopeExit([&] {
            m_instance.modules.moduleThatRunTimeStartsPlusOne = 0;
        });
        CallData entry = CallData::forCompiledCode(moduleBodyEntrypoint().taggedPtr(), realm);
        RELEASE_AND_RETURN(scope, callCompiledCode(vm, closures.calleeOfModuleBodies(vm, body), entry, jsUndefined(), nullptr, ArgList(&function, 1)));
    }
    RELEASE_AND_RETURN(scope, callCompiledCode(realm, closures.createClosureForOneCall(vm, body), jsUndefined(), ArgList()));
}

Modules::EvaluationMarkScope::EvaluationMarkScope(Modules& modules)
    : m_modules(modules)
    , m_outerMark(modules.m_evaluationMark)
{
    modules.m_evaluationMark = mark();
    modules.m_instance.modules.evaluatingMark = storedMark();
    if (m_outerMark < UINT8_MAX) [[likely]]
        return;
    std::span<uint8_t> statuses = modules.m_storedStatuses;
    for (size_t index = WTF::find(std::span<const uint8_t>(statuses), storedMark()); index != notFound; index = WTF::find(std::span<const uint8_t>(statuses), storedMark(), index + 1)) {
        m_outerModules.append(ModuleID(static_cast<uint32_t>(index)));
        modules.m_marksOfOuterModules.set(m_outerModules.last(), m_outerMark);
        statuses[index] = static_cast<uint8_t>(StoredModuleStatus::Evaluating);
    }
}

void Modules::EvaluationMarkScope::leave()
{
    if (std::exchange(m_hasLeft, true))
        return;
    stopMarking();
    m_modules.m_evaluationMark = m_outerMark;
    for (ModuleID module : m_outerModules) {
        m_modules.m_marksOfOuterModules.remove(module);
        if (uint8_t& stored = m_modules.m_storedStatuses[module.index()]; stored == static_cast<uint8_t>(StoredModuleStatus::Evaluating))
            stored = storedMark(m_outerMark);
    }
}

uint32_t Modules::evaluationMarkOf(ModuleID module) const
{
    uint8_t stored = m_storedStatuses[module.index()];
    if (stored == UINT8_MAX) [[unlikely]]
        return m_evaluationMark;
    if (stored == static_cast<uint8_t>(StoredModuleStatus::Evaluating) && !m_marksOfOuterModules.isEmpty()) [[unlikely]]
        return m_marksOfOuterModules.getOptional(module).value_or(stored);
    return stored;
}

auto Modules::noteModulesThatMayBeginUnder(uint32_t mark, ModuleID root) -> Vector<ModuleThatMayBegin, 8>
{
    if (m_marksOfEvaluationsUnderOthers.isEmpty())
        m_marksOfEvaluationsUnderOthers.fill(0, m_table.size());
    Vector<ModuleThatMayBegin, 8> found;
    auto note = [&](ModuleID module) {
        uint32_t& noted = m_marksOfEvaluationsUnderOthers[module.index()];
        if (noted == mark || !hasNotBegun(module))
            return;
        found.append({ module, noted });
        noted = mark;
    };
    note(root);
    for (size_t next = 0; next < found.size(); ++next) {
        for (const ModuleRequestData& request : m_table.requests(found[next].module)) {
            if (!request.isRequire && !request.isDeferred)
                note(request.targetModule());
        }
    }
    return found;
}

JSValue Modules::callBodyUnderOwnMark(ModuleID module, bool bodyStoresStatus)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ASSERT(!bodyStoresStatus || data(module).evaluatesItself());
    if (std::optional<ModuleID> file = m_table.moduleBehindFacade(module)) {
        detachFailedModuleObject(*file);
        RETURN_IF_EXCEPTION(scope, { });
    }

    bool isInRecord = bodyStoresStatus && storedStatus(module) == StoredModuleStatus::InRecord;
    if (isInRecord)
        setStoredStatus(module, StoredModuleStatus::Linked);
    EvaluationMarkScope markScope(*this);
    Vector<ModuleThatMayBegin, 8> mayBeginHere;
    if (markScope.isUnderAnotherEvaluation() && bodyStoresStatus) [[unlikely]]
        mayBeginHere = noteModulesThatMayBeginUnder(markScope.mark(), module);
    JSValue result = callBody(module);
    markScope.stopMarking();
    for (auto [left, markBefore] : mayBeginHere) {
        if (hasNotBegun(left))
            m_marksOfEvaluationsUnderOthers[left.index()] = markBefore;
    }
    Exception* exception = scope.exception();
    isInRecord = isInRecord && m_records.contains(module);
    if (!exception) [[likely]] {
        if (isInRecord)
            setStoredStatus(module, StoredModuleStatus::InRecord);
        return result;
    }

    Vector<ModuleID, 16> beingEvaluated;
    std::span<const uint8_t> statuses = m_storedStatuses;
    for (size_t index = WTF::find(statuses, markScope.storedMark()); index != notFound; index = WTF::find(statuses, markScope.storedMark(), index + 1))
        beingEvaluated.append(ModuleID(static_cast<uint32_t>(index)));
    ModulesThatWaitForCycles haveRun;
    for (ModuleID failed : beingEvaluated) {
        if (isOnCycleOfImports(failed))
            lookAtCycleOf(failed, haveRun);
    }
    markScope.leave();
    if (isInRecord)
        setStoredStatus(module, StoredModuleStatus::InRecord);
    for (ModuleID failed : beingEvaluated) {
        if (!isInRecord || failed != module)
            didThrowInBody(failed, exception->value());
    }
    for (size_t index : haveRun.modules) {
        if (ModuleID failed(static_cast<uint32_t>(index)); markOfEvaluationThatBegan(failed) == markScope.mark())
            setFailed(failed, exception->value());
    }
    return result;
}

void Modules::didMakeTemplateObject(ModuleID file, uint32_t offsetOfCell)
{
    if (data(file).kind == ModuleKind::CommonJs)
        m_templateObjectsOfFiles.append({ file, offsetOfCell });
}

void Modules::forgetTemplateObjects(ModuleID file)
{
    m_templateObjectsOfFiles.removeAllMatching([&](const TemplateObjectOfFile& made) {
        if (made.file != file)
            return false;
        *reinterpret_cast<uint64_t*>(m_instance.statics() + made.offsetOfCell) = 0;
        return true;
    });
}

auto Modules::recordedRequireIndex() -> RecordedRequireIndex&
{
    if (!m_recordedRequireIndex) {
        m_recordedRequireIndex = makeUniqueWithoutFastMallocCheck<RecordedRequireIndex>();
        for (uint64_t key : m_recordedRequires.keys())
            m_recordedRequireIndex->addRequire(key);
        for (const FirstRequire& first : m_firstRequires)
            m_recordedRequireIndex->addFirstRequire(first.requirer, first.required);
    }
    return *m_recordedRequireIndex;
}

Vector<ModuleID, 2> Modules::filesFirstRequiredBy(ModuleID requirer)
{
    RecordedRequireIndex& index = recordedRequireIndex();
    auto files = index.firstRequired.find(requirer.index());
    return files == index.firstRequired.end() ? Vector<ModuleID, 2>() : files->value;
}

void Modules::saveFailedModuleObject(ModuleID file)
{
    SuspendExceptionScope errorStillThrown(*m_instance.vm);
    DeferTerminationForAWhile noTerminationArrives(*m_instance.vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    if (m_fileThatRanAsMain == file && !std::exchange(m_didMainFail, true) && m_showsWaysBetweenModuleObjects && canExposeModuleObject(file)) {
        JSObject* main = this->moduleObject(file);
        if (scope.exception()) [[unlikely]]
            return;
        ensureLoaded(file).moduleObjectThatFailedAsMain.set(*this, main);
    }
    JSObject* moduleObject = moduleObjectIfCreated(file);
    for (ModuleID required : filesFirstRequiredBy(file)) {
        if (firstRequirerOf(required) != file || ensureLoaded(required).firstUncompiledRequirer)
            continue;
        if (!moduleObject) {
            moduleObject = moduleObjectOrPlaceholder(file);
            if (scope.exception()) [[unlikely]]
                return;
        }
        ensureLoaded(required).firstUncompiledRequirer.set(*this, moduleObject);
    }
    if (moduleObject && !hidesModuleObject(file) && host().didDiscardModuleEvaluation) {
        host().didDiscardModuleEvaluation(m_instance.globalObject, moduleObject, false);
        ensureLoaded(file).failedModuleObject.set(*this, moduleObject);
    }
    static_cast<void>(scope.exception());
}

void Modules::detachFailedModuleObject(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    if (!loaded || !loaded->failedModuleObject) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSObject* moduleObject = loaded->failedModuleObject;
    loaded->failedModuleObject.set(*this, nullptr);
    for (ModuleID required : filesFirstRequiredBy(file)) {
        if (firstRequirerOf(required) == file && !ensureLoaded(required).firstUncompiledRequirer)
            ensureLoaded(required).firstUncompiledRequirer.set(*this, moduleObject);
    }
    host().didDiscardModuleEvaluation(m_instance.globalObject, moduleObject, true);
    RETURN_IF_EXCEPTION(scope, void());
    forgetWhatFileRequired(file);
}

void Modules::forgetWhatFileRequired(ModuleID file)
{
    if (WaysToModule* ways = waysToIfRecorded(file)) {
        ways->requiredModules.clear();
        ways->numberOfChildrenShown = 0;
    }
    for (uint64_t key : recordedRequireIndex().byReferrer.take(file.index() + 1))
        m_recordedRequires.remove(key);
}

JSValue Modules::errorCachedByLoader(ModuleID file) const
{
    std::optional<ModuleID> facade = data(file).kind == ModuleKind::CommonJs ? m_table.facadeOf(file) : std::nullopt;
    return facade && status(*facade) == ModuleStatus::Failed ? recordIfExists(*facade)->evaluationError() : JSValue();
}

void Modules::discardFailedEvaluation(ModuleID file)
{
    saveFailedModuleObject(file);
    discardModuleObject(file);
    forgetTemplateObjects(file);
    if (LoadedModule* loaded = loadedIfExists(file)) {
        loaded->requiredValue.set(*this, JSValue());
        loaded->firstUncompiledRequirer.set(*this, nullptr);
        loaded->hasStaleFirstRequire = true;
    }
    forgetWhatFileRequired(file);
    if (WaysToModule* ways = waysToIfRecorded(file))
        ways->firstRequirer = std::nullopt;
    for (uint64_t key : recordedRequireIndex().byTarget.take(file.index()))
        m_recordedRequires.remove(key);
    m_filesStartedAsEntryPoint.clear(file.index());
    setStatus(file, ModuleStatus::Linked);
}

void Modules::leaveFailureToImports(ModuleID file)
{
    if (status(file) != ModuleStatus::Failed || isEvicted(file) || isMocked(file) || activeMock(file))
        return;
    if (mayBeEvicted(file))
        return evictForRequireAlone(file);
    if (!data(file).runsAgainAfterThrow())
        return;
    ModuleRecord* record = recordIfExists(file);
    JSValue error = record->evaluationError();
    ensureLoaded(file).errorLeftToImports.set(*this, error);
    m_filesWithFailureLeftToImports.set(file.index());
    if (ModuleID imported = moduleWithExportsOf(file); status(imported) != ModuleStatus::Failed)
        setFailed(imported, error);
    record->setEvaluationError(*m_instance.vm, JSValue());
    discardFailedEvaluation(file);
}

bool Modules::forgetFailureLeftToImports(ModuleID file)
{
    if (!m_filesWithFailureLeftToImports.clear(file.index())) [[likely]]
        return false;
    ensureLoaded(file).errorLeftToImports.set(*this, JSValue());
    loader()->removeEntry(moduleKey(moduleWithExportsOf(file)));
    return true;
}

bool Modules::isAwaitedByPendingImport(ModuleID file)
{
    for (ModuleID moduleOrFacade : { file, m_table.facadeOf(file).value_or(file) }) {
        if (m_modulesOfQueuedImports.get(moduleOrFacade.index()) || m_batchedFetches.containsCallOf(moduleOrFacade, fileOf(moduleOrFacade)))
            return true;
        bool isPending = false;
        m_loadsInFlight.forEachOfRoot(moduleOrFacade, [&](LoadInFlight& load) {
            isPending |= !load.isKnownToBeSettled && load.promise->status() == JSPromise::Status::Pending;
        });
        if (isPending)
            return true;
    }
    return false;
}

bool Modules::isAwaitedByImport(ModuleID file)
{
    for (ModuleID moduleOrFacade : { file, m_table.facadeOf(file).value_or(file) }) {
        if (m_modulesOfQueuedImports.get(moduleOrFacade.index()) || m_loadsInFlight.hasRoot(moduleOrFacade) || m_batchedFetches.containsCallOf(moduleOrFacade, fileOf(moduleOrFacade)))
            return true;
    }
    return false;
}

void Modules::didThrowInBody(ModuleID module, JSValue error)
{
    if (m_originalsBeingEvaluated.get(module.index())) [[unlikely]]
        return;
    if (data(module).kind == ModuleKind::CommonJs && m_instance.vm->hasPendingTerminationException()) [[unlikely]] {
        setStatus(module, ModuleStatus::Evaluated);
        return;
    }
    if (data(module).kind == ModuleKind::CommonJs && (mayBeEvicted(module) || data(module).runsAgainAfterThrow()) && status(module) == ModuleStatus::Evaluating && !wasStartedByWalk(module)) [[unlikely]] {
        didThrowAfterCaptureOfExports(module);
        if (mayBeEvicted(module))
            evict(module);
        else
            discardFailedEvaluation(module);
        return;
    }
    JSValue cachedByLoader = errorCachedByLoader(module);
    if (data(module).cachesEvaluationError())
        setFailed(module, cachedByLoader ? cachedByLoader : error);
    else
        setStatus(module, ModuleStatus::Evaluated);
}

void Modules::abandonEvaluation(ModuleID module, JSValue error)
{
    if (storedStatus(module) >= StoredModuleStatus::Evaluating && !isOnCycleOfImports(module))
        didThrowInBody(module, error);
}

bool Modules::waitsForItsCycle(ModuleID module, ModulesThatWaitForCycles& waiting) const
{
    if (!mayWaitForItsCycle(module))
        return false;
    lookAtCycleOf(module, waiting);
    return waiting.modules.get(module.index());
}

const CyclesOfImports& Modules::cyclesOfImports() const
{
    std::unique_ptr<CyclesOfImports>& cycles = m_instance.vm->aotState().cyclesOfImports;
    if (!cycles)
        cycles = makeUnique<CyclesOfImports>(m_table);
    return *cycles;
}

void Modules::lookAtCycleOf(ModuleID module, ModulesThatWaitForCycles& waiting) const
{
    constexpr uint8_t evaluated = static_cast<uint8_t>(StoredModuleStatus::Evaluated);
    constexpr uint8_t evaluating = static_cast<uint8_t>(StoredModuleStatus::Evaluating);
    const CyclesOfImports& cycles = cyclesOfImports();
    const CyclesOfImports::Member* asked = cycles.find(module);
    if (!asked || waiting.cyclesLookedAt.set(asked->cycle))
        return;
    std::span<const uint32_t> members = cycles.membersOf(asked->cycle);
    countEvent(*m_instance.vm, RuntimeCounterID::ModulesScannedForPendingCycles, members.size());
    if (std::ranges::none_of(members, [&](uint32_t member) { return evaluationMarkOf(ModuleID(member)) > evaluating; }))
        return;

    Vector<uint32_t> evaluationOf(members.size(), [&](size_t place) -> uint32_t {
        ModuleID member(members[place]);
        uint32_t stored = evaluationMarkOf(member);
        if (stored < evaluated || stored == evaluating || !isOnCycleOfImports(member))
            return 0;
        return stored == evaluated ? markOfEvaluationThatBegan(member) : stored;
    });

    Vector<uint32_t, 16> importers;
    for (uint32_t place = 0; place < members.size(); ++place) {
        if (evaluationOf[place])
            importers.append(place);
    }
    while (!importers.isEmpty()) {
        uint32_t importer = importers.takeLast();
        for (const ModuleRequestData& request : m_table.requests(ModuleID(members[importer]))) {
            if (request.isRequire || request.isDeferred)
                continue;
            ModuleID imported = request.targetModule();
            const CyclesOfImports::Member* target = cycles.find(imported);
            if (!target || target->cycle != asked->cycle || evaluationOf[target->place])
                continue;
            if (m_storedStatuses[imported.index()] >= evaluated || data(imported).bodyFunction() || data(imported).evaluatesItself() || !isOnCycleOfImports(imported))
                continue;
            evaluationOf[target->place] = evaluationOf[importer];
            importers.append(target->place);
        }
    }

    struct ImportTargets {
        std::span<const ModuleRequestData> requests;
        uint32_t importer;
        uint32_t cycle;
        const CyclesOfImports& cycles;
        const Vector<uint32_t>& evaluationOf;

        size_t size() const { return requests.size(); }
        uint32_t operator[](size_t index) const
        {
            const ModuleRequestData& request = requests[index];
            if (request.isRequire || request.isDeferred || !evaluationOf[importer])
                return importer;
            const CyclesOfImports::Member* target = cycles.find(request.targetModule());
            bool isFollowed = target && target->cycle == cycle && evaluationOf[target->place] == evaluationOf[importer];
            return isFollowed ? target->place : importer;
        }
    };
    Typed::StronglyConnectedComponents components = Typed::computeStronglyConnectedComponents(members.size(), [&](unsigned place) {
        return ImportTargets { m_table.requests(ModuleID(members[place])), place, asked->cycle, cycles, evaluationOf };
    });
    BitVector unfinishedComponents(components.numberOfComponents);
    for (unsigned place = 0; place < members.size(); ++place) {
        if (evaluationOf[place] && m_storedStatuses[members[place]] > evaluated)
            unfinishedComponents.quickSet(components.componentOf[place]);
    }
    for (unsigned place = 0; place < members.size(); ++place) {
        if (evaluationOf[place] && m_storedStatuses[members[place]] == evaluated && unfinishedComponents.quickGet(components.componentOf[place]))
            waiting.modules.set(members[place]);
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
    if (!m_displacedRunImporters.isEmpty()) [[unlikely]] {
        if (m_innermostEvaluation && !m_innermostEvaluation->frames.isEmpty() && isDisplacedRunKept(module, m_innermostEvaluation->frames.last().module))
            return false;
    }
    if (m_requirersThatRunAgain.clear(module.index())) [[unlikely]]
        tellOfKeptRun(module);
    if (!m_mockedFilesWithReleasedRequirers.isEmpty()) [[unlikely]] {
        if (auto released = m_mockedFilesWithReleasedRequirers.find(module); released != m_mockedFilesWithReleasedRequirers.end() && released->value && !isOnCycleOfLoads(module))
            released->value = false;
    }
    bool hasPendingFactory = tellHostOfRequire(module);
    if (storedStatus(module) != StoredModuleStatus::InRecord) {
        ModuleID file = fileOf(module);
        if (!isChangedOnDisk(file)) [[likely]] {
            m_storedStatuses[module.index()] = static_cast<uint8_t>(m_instance.modules.evaluatingMark);
            return true;
        }
        m_filesModifiedSinceBuild.set(file.index());
        JSGlobalObject* realm = m_instance.globalObject;
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        JSPromise* loaded = nullptr;
        {
            SetForScope isInHostRequest(m_currentHostRequest, true);
            loaded = loader()->loadModuleSync(realm, keyWithoutForm(file), nullptr, nullptr);
        }
        RETURN_IF_EXCEPTION(scope, false);
        if (loaded->status() != JSPromise::Status::Rejected) {
            throwTypeError(realm, scope, makeString("Cannot load '"_s, m_table.fullPath(file), "': it was written after this test run was compiled"_s));
            return false;
        }
        loaded->markAsHandled();
        throwException(realm, scope, loaded->result());
        return false;
    }
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    removeUnusedLingeringMocks(module);
    RETURN_IF_EXCEPTION(scope, false);
    if (m_importersRetainedAboveMock.get(module.index())) [[unlikely]]
        tellOfFilesThatStayedLoadedBelow(module);
    if (storedStatus(module) == StoredModuleStatus::Evaluated) [[unlikely]]
        return false;
    if (storedStatus(module) != StoredModuleStatus::InRecord) [[unlikely]]
        RELEASE_AND_RETURN(scope, beginModuleInRecord(module));
    if (data(module).mayRunAsOriginal()) [[unlikely]] {
        rejectRequireInSweep(module, ModuleID());
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (data(module).mayRunAsOriginal() && status(module) <= ModuleStatus::Linked && host().requireOriginal) [[unlikely]] {
        bool isForOwnFactory = isFileRequestedByOwnFactory(module);
        if ((isForOwnFactory || m_modulesWithPendingMock.get(module.index())) && commonJSOriginalNamedBy(actualKey(module))) {
            JSValue original = host().requireOriginal(m_instance.globalObject, module, isForOwnFactory);
            RETURN_IF_EXCEPTION(scope, false);
            if (original) {
                setCommonJSExports(module, original);
                return false;
            }
        }
    }
    if (m_modulesWithPendingMock.get(module.index()) && !(isMockLingering(module) && wasStartedByWalk(module))) [[unlikely]] {
        SetForScope requireRunsMocks(m_instance.vm->aotState().requireRunsMocks, true);
        SetForScope currentRequireTarget(m_currentRequireTarget, module);
        SetForScope requiringModule(m_requiringModule, ModuleID());
        auto waiting = m_pendingMockCalls.find(module);
        bool liesInRequireMap = waiting != m_pendingMockCalls.end() && waiting->value.keepsActiveMock && wasRequiredUnderActiveMock(module);
        bool factoryHasNotSettled = runPendingMock(module);
        RETURN_IF_EXCEPTION(scope, false);
        if (factoryHasNotSettled) [[unlikely]] {
            throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, pathOfRefusedRequire(module)));
            return false;
        }
        if (activeMock(module)) {
            ensureLoaded(module).wasRequiredUnderActiveMock = true;
            noteRequireOfMockLaidInPreload(module);
        }
        if (!liesInRequireMap && m_modulesWithPendingMock.get(module.index()) && getsOriginalBesideMock(module, module)) {
            JSValue original = answerRequireWithOriginal(module, module);
            RETURN_IF_EXCEPTION(scope, false);
            setCommonJSExports(module, original);
        }
    }
    if (hasPendingFactory) [[unlikely]] {
        bool shouldRejectEveryRequire = false;
        if (pendingFactoryCallID(module, &shouldRejectEveryRequire) && shouldRejectEveryRequire) {
            throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, pathOfRefusedRequire(module)));
            return false;
        }
    }
    if (activeMock(module)) [[unlikely]] {
        noteAskUnderMockInPlace(module);
        if (!m_innermostWalkOfImports)
            ensureLoaded(module).wasRequiredUnderActiveMock = true;
        if (!isBegunForStatement(module))
            noteRequireOfMockLaidInPreload(module);
    }
    if (m_filesToLoadAgain.get(module.index())) [[unlikely]] {
        JSGlobalObject* realm = m_instance.globalObject;
        JSPromise* loaded = nullptr;
        {
            SetForScope isInHostRequest(m_currentHostRequest, true);
            loaded = loader()->loadModuleSync(realm, keyWithoutForm(module), nullptr, nullptr);
        }
        RETURN_IF_EXCEPTION(scope, false);
        if (loaded->status() == JSPromise::Status::Rejected) {
            loaded->markAsHandled();
            throwException(realm, scope, loaded->result());
            return false;
        }
        if (!m_filesToLoadAgain.clear(module.index()))
            return false;
        if (!recordIfExists(module))
            setStoredStatus(module, StoredModuleStatus::Evaluated);
        scope.release();
        recaptureExports(module);
        return false;
    }
    ModuleStatus moduleStatus = status(module);
    if (moduleStatus == ModuleStatus::Evaluated || moduleStatus == ModuleStatus::EvaluatingAsync) {
        BitVector isPassed;
        scope.release();
        passModuleThatHasRun(module, isPassed);
        return false;
    }
    bool hasNotBegun = moduleStatus == ModuleStatus::Linked || moduleStatus == ModuleStatus::Unlinked;
    if (hasNotBegun && (!data(module).bodyFunction() || (moduleStatus == ModuleStatus::Linked && !data(module).capturesExportsBehindEvaluation()))) {
        ModuleRecord* record = recordIfExists(module);
        record->setStatus(CyclicModuleRecord::Status::Evaluating);
        record->clearOwnsStatus();
        m_storedStatuses[module.index()] = static_cast<uint8_t>(m_instance.modules.evaluatingMark);
        return true;
    }
    scope.release();
    evaluateModuleInRecord(module);
    return false;
}

bool Modules::isBegunForStatement(ModuleID moduleOrFacade) const
{
    ModuleID file = fileOf(moduleOrFacade);
    if (moduleOrFacade != file)
        return true;
    std::optional<ModuleID> facade = m_table.facadeOf(file);
    return facade && status(*facade) == ModuleStatus::Evaluating;
}

void Modules::rerunBodyOfFacade(ModuleID facade)
{
    if (status(facade) != ModuleStatus::Evaluated || isMocked(fileOf(facade)))
        return;
    bool evaluatesItself = data(facade).evaluatesItself();
    if (evaluatesItself && storedStatus(facade) == StoredModuleStatus::Evaluated)
        setStoredStatus(facade, StoredModuleStatus::Linked);
    callBodyUnderOwnMark(facade, evaluatesItself);
}

bool Modules::wasStartedByWalk(ModuleID file) const
{
    for (const WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        if (walk->fileStarted == file)
            return true;
    }
    return false;
}

bool Modules::isOnPathOfWalk(ModuleID module, const ImportOrderWalk* otherThan) const
{
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        if (!walk->walkOfLoader || walk->walkOfLoader == otherThan)
            continue;
        catchUpWithList(*walk);
        if (walk->path.containsIf([&](const WalkOfImports::Importer& importer) { return importer.module == module; }))
            return true;
    }
    return false;
}

bool Modules::wasReachedByWalk(ModuleID heldFile) const
{
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        catchUpWithList(*walk);
        if (walk->reached.get(heldFile.index()))
            return true;
    }
    return m_heldFilesReachedByWalks.get(heldFile.index());
}

bool Modules::hasValuesThatWalkHolds(ModuleID file) const
{
    if (!m_innermostWalkOfImports) [[likely]]
        return false;
    if (activeMock(file))
        return !hasRemovedMock(file) && (wasReachedByWalk(file) || isAboutToBeLinked(moduleWithExportsOf(file)));
    return data(file).kind == ModuleKind::Data && !isReplacedByMock(file) && wasReachedByWalk(file);
}

void Modules::takeValuesThatWalkHolds(ModuleID file)
{
    if (!hasValuesThatWalkHolds(file)) [[likely]]
        return;
    SuspendExceptionScope errorStillThrown(*m_instance.vm);
    DeferTerminationForAWhile noTerminationArrives(*m_instance.vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    if (data(file).kind == ModuleKind::CommonJs)
        runFileInItsTurn(moduleWithExportsOf(file));
    else
        evaluateDataFileThatLoadHolds(file, false);
    (void)scope.tryClearException();
}

bool Modules::isAboutToBeLinked(ModuleID module) const
{
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        if (walk->evaluation.isEvaluatingModule() && modulesBelowRoot(*walk).quickGet(module.index()))
            return true;
    }
    return false;
}

std::span<const ModuleRequestData> Modules::requestsThatWalksHold(ModuleID importer) const
{
    std::span<const ModuleRequestData> requests = m_table.requests(importer);
    if (isAboutToBeLinked(importer))
        return requests;
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        if (size_t place = walk->path.findIf([&](const WalkOfImports::Importer& each) { return each.module == importer; }); place != notFound)
            return requests.first(requests.size() - walk->path[place].requestsLeft.size());
    }
    return requests;
}

bool Modules::hasBegunForWalks(ModuleID imported, const BitVector& reached)
{
    ModuleID file = fileOf(imported);
    if (isMockPending(file) || isEvictedForImports(file) || data(imported).isUncompiledCode())
        return false;
    if (activeMock(file) && !hasRemovedMock(file))
        return true;
    if (isCommonJSModuleOrFacade(imported) && !activeMock(file))
        return m_filesThatTookTheirTurn.get(file.index()) || wasStartedByWalk(file) || (imported != file && status(imported) == ModuleStatus::Evaluated);
    if (!goesThroughImportsOfKind(imported) && namespaceIfCreated(imported, ModulePhase::Evaluation) && hasBeenImportedSinceReset(imported))
        return true;
    return reached.quickGet(imported.index()) || std::ranges::any_of(importersOfEveryImport().of(imported), [&](uint32_t importer) {
        return wasImporterLinked(ModuleID(importer));
    }) || (goesThroughImportsOfKind(imported) && wasImporterLinked(imported));
}

// See ImportOrderWalk::isObsolete() and noteChange().
void Modules::keepImportsThatWalksHold()
{
    VM& vm = *m_instance.vm;
    if (!m_innermostWalkOfImports || !vm.importOrderState().isRunningDeferredModule || vm.importOrderState().isInBatchOfChanges)
        return;
    catchUpWithListsOfImports();
    forgetEndedLoadsThatKeptImports();
    BitVector reached(m_table.size());
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer)
        reached.merge(walk->reached);
    uint32_t moment = ++vm.aotState().momentOfLoads;
    SuspendExceptionScope errorStillThrown(vm);
    DeferTerminationForAWhile noTerminationArrives(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        size_t load = m_loadsThatKeepImports.findIf([&](const LoadThatKeepsImports& each) { return each.root == walk->root; });
        if (load == notFound) {
            load = m_loadsThatKeepImports.size();
            m_loadsThatKeepImports.append({ walk->root });
        }
        for (size_t index : walk->evaluation.isEvaluatingModule() ? modulesBelowRoot(*walk) : walk->reached) {
            ModuleID importer(index);
            if (!goesThroughImports(importer) || wasImporterLinked(importer) || isMocked(importer) || isEvicted(importer))
                continue;
            for (const ModuleRequestData& request : requestsThatWalksHold(importer)) {
                ModuleID imported = request.targetModule();
                if (request.isHoisted || request.isRequire || keepsImport(importer, imported) || !hasBegunForWalks(imported, reached))
                    continue;
                linkAheadOfOtherImports(importer, imported);
                if (scope.exception()) [[unlikely]] {
                    (void)scope.tryClearException();
                    return;
                }
                bool seesMockWithoutLink = !hasInstances(importer) && isMockSeenByLoad(imported, importer);
                setWhatKeptImportStandsFor(importer, fileOf(imported), seesMockWithoutLink ? mockNamespaceForLoad(imported) : nullptr);
                m_importsKeptAheadOfLink.add(index, MomentsFilesWereKept { }).iterator->value.add(fileOf(imported).index(), moment);
            }
            if (reached.quickGet(index))
                m_importsKeptAheadOfLink.add(index, MomentsFilesWereKept { });
            if (m_importsKeptAheadOfLink.contains(index))
                m_loadsThatKeepImports[load].unlinkedImporters.set(index);
        }
    }
}

// See JSModuleLoader::didChangeResolution() and didChangeFetch().
void Modules::didChangePluginsInMiddleOfLoad(ChangeOfPlugins change)
{
    keepImportsThatWalksHold();
    BitVector reached(m_table.size());
    BitVector staysCompiled(m_table.size());
    BitVector loaded = m_filesWhoseGraphHasBegun;
    loaded.merge(m_filesAskedAbout);
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        reached.merge(walk->reached);
        bool mayGiveWay = walk->walkOfLoader ? walk->walkOfLoader->letsGiveWayToCopy(walk->recordOfRoot) : walk->stopsAtChangeOfAnswers;
        if (mayGiveWay && !isMocked(walk->root)) {
            loaded.merge(modulesBelowRoot(*walk));
            continue;
        }
        staysCompiled.merge(modulesBelowRoot(*walk));
        noteThatGraphBegins(walk->root);
    }
    for (auto& [module, record] : m_records) {
        if (record->wasReached() && goesThroughImports(module) && !reached.quickGet(module.index()))
            staysCompiled.merge(modulesYetToBeginBelow(module));
    }
    loaded.exclude(staysCompiled);
    for (size_t index : loaded) {
        ModuleID file(index);
        if (!goesThroughImports(file) || wasImporterLinked(file) || isMocked(file) || !mayBeEvicted(file) || m_instance.vm->aotState().filesStartedByHost.get(index))
            continue;
        if (hasFailedToLoadForGood(file)) {
            staysCompiled.quickSet(index);
            continue;
        }
        m_filesToAskAboutAgain.set(index);
        if (reached.quickGet(index))
            m_filesReadBeforeChangeOfPlugins.set(index);
        noteNamesAnsweredBeforeChangeOfPlugins(file);
        if (change == ChangeOfPlugins::Fetch)
            continue;
        m_filesWhoseGraphHasBegun.clear(index);
        m_filesAskedAbout.clear(index);
    }
    m_filesThatLoadsHold = WTF::move(reached);
    m_filesThatLoadsHold.merge(staysCompiled);
}

void Modules::keepAnswersOfHostThatWalksHold()
{
    VM& vm = *m_instance.vm;
    catchUpWithListsOfImports();
    BitVector reached(m_table.size());
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer)
        reached.merge(walk->reached);
    auto hasBegun = [&](AbstractModuleRecord* record) {
        if (ModuleRecord* compiled = compiledModule(vm, record))
            return hasBegunForWalks(compiled->module(), reached);
        return record->wasReached() || record->hasStartedLinking();
    };
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        LoadedModule* loaded = walk->walkOfLoader && walk->moduleInTurn ? loadedIfExists(*walk->moduleInTurn) : nullptr;
        if (loaded && loaded->uncompiledRecord && !hasBegun(loaded->uncompiledRecord))
            m_modulesWithRecordOfReleasedImporter.set(walk->moduleInTurn->index());
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    for (const UncompiledModuleData& uncompiled : m_table.modulesOfUncompiledCode()) {
        ModuleID external(uncompiled.module);
        LoadedModule* loaded = loadedIfExists(external);
        if (!loaded || loaded->uncompiledRecord || loaded->resolvedKey.isNull() || !isAboutToBeLinked(*data(external).referrerModule()))
            continue;
        ModuleRegistryEntry* entry = registryEntry(loaded->resolvedKey, fetchParametersOf(external).get());
        std::optional<ModuleID> compiled = entry && entry->record() ? std::nullopt : findModuleThatAnswersImportsOf(loaded->resolvedKey);
        if (compiled ? hasBegunForWalks(moduleWithExportsOf(fileOf(*compiled)), reached) : entry && entry->record() && hasBegun(entry->record()))
            uncompiledRecord(external);
        scope.assertNoExceptionExceptTermination();
    }
}

void Modules::noteNamesAnsweredBeforeChangeOfPlugins(ModuleID importer)
{
    auto kept = m_importsKeptAheadOfLink.find(importer.index());
    if (kept == m_importsKeptAheadOfLink.end())
        return;
    MomentsFilesWereKept files = kept->value;
    Vector<String> names;
    for (const ModuleRequestData& request : requestsThatWalksHold(importer)) {
        ModuleID imported = request.targetModule();
        LoadedModule* loaded = !request.isHoisted && !request.isRequire && data(imported).isUncompiledCode() ? loadedIfExists(imported) : nullptr;
        if (loaded && loaded->uncompiledRecord && !m_modulesWithRecordOfReleasedImporter.get(imported.index()))
            names.append(m_table.fullPath(imported));
    }
    String path = m_table.fullPath(importer);
    for (const NameOfStatementData& written : m_table.namesOfStatements(importer)) {
        String name = ProgramStrings::text(m_table.programTables(), written.nameString());
        String resolved = host().resolveWithoutPlugins(m_instance.globalObject, name, path);
        std::optional<ModuleID> linked = resolved.isNull() ? std::nullopt : m_table.findByPath(resolved);
        if (!linked || !files.contains(fileOf(*linked).index()))
            continue;
        names.appendIfNotContains(name);
        names.appendIfNotContains(resolved);
    }
    m_namesAnsweredBeforeChangeOfPlugins.set(importer.index(), WTF::move(names));
}

void Modules::askAgainAboutGraphOf(ModuleID root)
{
    if (m_filesThatGaveWayToCopy.get(root.index()) && hasGivenWayToCopy(root) && !wasImporterLinked(root)) [[unlikely]]
        return giveWayToCopy(root);
    if (!m_filesToAskAboutAgain.get(root.index())) [[likely]]
        return;
    SetForScope isRunningDeferredModule(m_instance.vm->importOrderState().isRunningDeferredModule, false);
    SetForScope asksHostInImportOrder(m_asksHostInImportOrder, true);
    bool givesWay = goesThroughImports(root) && !wasImporterLinked(root) && !isMocked(root) && delegatesToHost(root);
    if (!givesWay)
        noteThatGraphBegins(root);
    BitVector below = modulesYetToBeginBelow(root);
    below.filter(m_filesToAskAboutAgain);
    Vector<ModuleID, 8> leaving;
    for (size_t index : below) {
        ModuleID file(index);
        if (givesWay && goesThroughImports(file) && !wasImporterLinked(file) && !isMocked(file) && delegatesWithoutPlugins(file))
            leaving.append(file);
        else if (!mayBeAskedAbout(file)) {
            m_filesToAskAboutAgain.clear(index);
            m_filesReadBeforeChangeOfPlugins.clear(index);
            m_namesAnsweredBeforeChangeOfPlugins.remove(index);
        }
    }
    for (ModuleID file : leaving)
        giveWayToCopy(file);
}

void Modules::giveWayToCopy(ModuleID file)
{
    m_filesToAskAboutAgain.clear(file.index());
    m_filesThatGaveWayToCopy.set(file.index());
    forgetKeptImports(file);
    if (size_t load = m_loadsThatKeepImports.findIf([&](const LoadThatKeepsImports& each) { return each.root == file; }); load != notFound) {
        BitVector importers = WTF::move(m_loadsThatKeepImports[load].unlinkedImporters);
        m_loadsThatKeepImports.removeAt(load);
        for (size_t index : importers) {
            if (isEvicted(ModuleID(index)))
                continue;
            m_loadsThatKeepImports.append({ ModuleID(index) });
            m_loadsThatKeepImports.last().unlinkedImporters.set(index);
        }
    }
    didFailToLoadGraph(file);
    ModuleRecord* record = m_records.get(file);
    if (!record)
        return;
    record->setHasGivenWayToCopy();
    loader()->removeEntriesForRecord(record, AbstractModuleRecord::SupersededFor::AllImports);
    takeOutRecordThatHasNotBegun(file);
    // The steps of its load that are still to come ask for nothing.
    record->setStatus(CyclicModuleRecord::Status::Unlinked);
}

bool Modules::wasFileReadBeforeChangeOfPlugins(const String& path) const
{
    std::optional<ModuleID> file = m_filesReadBeforeChangeOfPlugins.isEmpty() ? std::nullopt : m_table.findByPath(path);
    return file && m_filesReadBeforeChangeOfPlugins.get(file->index());
}

bool Modules::takeNameAnsweredBeforeChangeOfPlugins(const String& importer, const String& name)
{
    std::optional<ModuleID> file = m_namesAnsweredBeforeChangeOfPlugins.isEmpty() ? std::nullopt : m_table.findByPath(importer);
    if (!file || !isEvicted(*file))
        return false;
    auto answered = m_namesAnsweredBeforeChangeOfPlugins.find(file->index());
    return answered != m_namesAnsweredBeforeChangeOfPlugins.end() && answered->value.removeFirst(name);
}

void Modules::willMakeRecordFromSource(const Identifier& key)
{
    std::optional<ModuleID> file = m_filesReadBeforeChangeOfPlugins.isEmpty() || key.isSymbol() ? std::nullopt : m_table.findByPath(key.string());
    if (file)
        m_filesReadBeforeChangeOfPlugins.clear(file->index());
}

void Modules::leaveImportsToCopy(ModuleID file)
{
    if (isUnlinkedImporterThatKeepsImports(file) && host().resolveWithoutPlugins) [[unlikely]]
        m_importsLeftToCopies.set(file.index(), ImportsLeftToCopy { m_importsKeptAheadOfLink.get(file.index()), m_modulesPendingReset.contains(file), m_modulesPendingReset.contains(file) || m_modulesToEvictOnceLoaded.contains(file) });
}

JSObject* Modules::whatKeptImportStandsFor(ModuleID importer, ModuleID file) const
{
    const LoadedModule* loaded = loadedIfExists(importer);
    JSObject* table = loaded ? loaded->whatKeptImportsStandFor.get() : nullptr;
    JSValue kept = table ? table->getDirectIndex(m_instance.globalObject, file.index()) : JSValue();
    return kept ? kept.getObject() : nullptr;
}

void Modules::setWhatKeptImportStandsFor(ModuleID importer, ModuleID file, JSObject* kept)
{
    if (kept == whatKeptImportStandsFor(importer, file)) [[likely]]
        return;
    LoadedModule& loaded = ensureLoaded(importer);
    if (!loaded.whatKeptImportsStandFor)
        loaded.whatKeptImportsStandFor.set(*this, createTableOfExports());
    storeValue(m_instance.globalObject, loaded.whatKeptImportsStandFor.get(), file.index(), kept ? JSValue(kept) : jsUndefined());
}

void Modules::leaveToCopiesOfImporters(ModuleID file, AbstractModuleRecord* record)
{
    for (ModuleID importer : unlinkedImportersThatKeep(file)) {
        if (!whatKeptImportStandsFor(importer, file))
            setWhatKeptImportStandsFor(importer, file, record);
    }
}

ModuleRecord* Modules::createRecordOfCurrentInstance(ModuleID module)
{
    VM& vm = *m_instance.vm;
    ModuleRecord* record = ModuleRecord::create(m_instance, vm.aotState().moduleRecordStructure.get(), loader(), moduleKey(module), module);
    record->bindModuleInstance(vm, currentInstance(module));
    record->setKeepsToModule();
    record->setStatus(CyclicModuleRecord::Status::Evaluated);
    record->setCycleRoot(vm, record);
    return record;
}

void Modules::willReleaseInstance(ModuleID module)
{
    if (status(module) == ModuleStatus::Evaluated && !isMocked(module) && !unlinkedImportersThatKeep(module).isEmpty()) [[unlikely]]
        leaveToCopiesOfImporters(module, createRecordOfCurrentInstance(module));
}

void Modules::keepExportsForImportersYetToBeLinked(ModuleID file)
{
    if (status(file) != ModuleStatus::Evaluated)
        return;
    VM& vm = *m_instance.vm;
    ImportOrderWalk::ImportsOfModules imports;
    if (ModuleRecord* record = isMocked(file) ? nullptr : m_records.get(file)) {
        Vector<AbstractModuleRecord*, 8> importsOfCompiledModules;
        for (const UncompiledModuleData& uncompiled : m_table.modulesOfUncompiledCode()) {
            if (LoadedModule* loaded = loadedIfExists(ModuleID(uncompiled.module)); loaded && loaded->uncompiledRecord)
                importsOfCompiledModules.append(loaded->uncompiledRecord.get());
        }
        imports = ImportOrderWalk::importsThatWalksHaveReached(record, importsOfCompiledModules.span());
    }
    if (imports.isEmpty() && unlinkedImportersThatKeep(file).isEmpty()) [[likely]]
        return;
    SuspendExceptionScope errorStillThrown(vm);
    DeferTerminationForAWhile noTerminationArrives(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    AbstractModuleRecord* kept = createRecordOfWhatFileHasExported(file);
    (void)scope.tryClearException();
    if (!kept)
        return;
    leaveToCopiesOfImporters(file, kept);
    for (auto& [importer, request] : imports) {
        if (auto loaded = importer->loadedModules().find(request); loaded != importer->loadedModules().end())
            loaded->value.m_module.set(vm, importer, kept);
    }
}

std::optional<ModuleID> Modules::moduleImportedFor(ModuleID importer, const AbstractModuleRecord::ModuleRequest& requestOfCopy)
{
    using Type = ScriptFetchParameters::Type;
    const String& name = requestOfCopy.m_specifier.string();
    String path = host().resolveWithoutPlugins(m_instance.globalObject, name, m_table.fullPath(importer));
    Type typeOfForm = path.isNull() ? Type::JavaScript : loader()->keyInModuleMap(Identifier::fromString(*m_instance.vm, path).impl(), requestOfCopy.type()).second;
    String pathOfForm = typeOfForm == Type::JavaScript ? String() : makeString(path, '?', typeOfForm == Type::JSON ? "json"_s : ScriptFetchParameters::nameOfHostDefinedType(typeOfForm));
    for (const ModuleRequestData& request : m_table.requests(importer)) {
        ModuleID imported = request.targetModule();
        if (request.isHoisted || request.isRequire)
            continue;
        if (data(imported).isUncompiledCode()) {
            RefPtr parameters = fetchParametersOf(imported);
            if (m_table.fullPath(imported) == name && (parameters ? parameters->type() : Type::JavaScript) == requestOfCopy.type())
                return imported;
        } else if (pathOfForm.isNull() ? !data(imported).isOtherFormOfFile() && keyInRequireCache(fileOf(imported)) == path : m_table.fullPath(imported) == pathOfForm)
            return imported;
    }
    return std::nullopt;
}

AbstractModuleRecord* Modules::recordThatRegistryHasLost(ModuleID importer, ModuleID imported, ImportsLeftToCopy& left)
{
    if (data(imported).isUncompiledCode()) {
        LoadedModule* loaded = m_modulesWithRecordOfReleasedImporter.get(imported.index()) ? nullptr : loadedIfExists(imported);
        AbstractModuleRecord* record = loaded ? loaded->uncompiledRecord.get() : nullptr;
        return record && !record->isSupersededForEarlierImports() && !loader()->entryOf(record) ? record : nullptr;
    }
    ModuleID file = fileOf(imported);
    if (!left.files.contains(file.index()))
        return nullptr;
    JSObject* kept = whatKeptImportStandsFor(importer, file);
    Environment* instance = hasInstances(importer) ? currentInstance(importer) : nullptr;
    const ModuleLinkData* link = instance ? findModuleLink(importer, imported) : nullptr;
    if (ModuleNamespace* word = link ? mockWord(*link, instance) : dynamicDowncast<ModuleNamespace>(kept); word && !word->isLoadWord() && word->linkedExports()) {
        auto waiting = m_pendingMockCalls.find(file);
        bool isReplaced = isOfEndedMock(*word, file) || (waiting != m_pendingMockCalls.end() && waiting->value.keepsActiveMock);
        left.isCopyEvictedOnceLoaded |= isReplaced;
        return isReplaced || (data(file).isOfLibrary() ? left.wasImporterReset : isEvictedForImports(file)) ? createRecordOfMock(file, word) : nullptr;
    }
    if (kept) {
        left.isCopyEvictedOnceLoaded |= kept->inherits<ModuleRecord>();
        return dynamicDowncast<AbstractModuleRecord>(kept);
    }
    bool wasMockedSince = isReplacedByMock(file) && !isPatchedByMock(file);
    if (!wasMockedSince && (!isEvictedForImports(file) || isMocked(file)))
        return nullptr;
    if (isCommonJSModuleOrFacade(imported) && isEvictedForImports(file))
        return wasStartedByWalk(file) ? nullptr : createRecordOfWhatFileHasExported(file);
    if (goesThroughImports(file))
        return nullptr;
    if (!wasMockedSince)
        return status(file) == ModuleStatus::Evaluated && currentInstance(file) ? createRecordOfCurrentInstance(file) : exposeRecord(file, RecordHolder::Anyone);
    ModuleRecord* original = ensureDisplacedRecord(file);
    original->setKeepsToModule();
    return original;
}

AbstractModuleRecord* Modules::createRecordOfMock(ModuleID file, ModuleNamespace* word)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!word->namesOfMock()) {
        fixMockNames(word);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    PropertyNameArrayBuilder namesOfMock(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(word->namesOfMock() ? word->namesOfMock() : word->linkedExports(), realm, namesOfMock, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, nullptr);
    WTF::Vector<Identifier, 4> names;
    MarkedArgumentBuffer values;
    for (const Identifier& name : namesOfMock) {
        names.append(name);
        values.append(JSValue());
    }
    if (values.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(realm, scope);
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, SyntheticModuleRecord::tryCreateWithExportNamesAndValues(realm, loader(), moduleKey(file), names, values, word->linkedExports(), SyntheticModuleRecord::LazyExports::Live));
}

// A copy that takes the place of a file in the middle of its load goes on from where the file was.
void Modules::didMakeRecordFromSource(JSModuleRecord* copy)
{
    if (m_importsLeftToCopies.isEmpty()) [[likely]]
        return;
    const Identifier& key = copy->moduleKey();
    std::optional<ModuleID> file = key.isSymbol() ? std::nullopt : m_table.findByPath(key.string());
    std::optional<ImportsLeftToCopy> left = file ? m_importsLeftToCopies.takeOptional(file->index()) : std::nullopt;
    if (!left)
        return;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    for (const AbstractModuleRecord::ModuleRequest& request : copy->requestedModules()) {
        std::optional<ModuleID> imported = moduleImportedFor(*file, request);
        AbstractModuleRecord* record = imported ? recordThatRegistryHasLost(*file, *imported, *left) : nullptr;
        if (scope.exception()) [[unlikely]]
            TRY_CLEAR_EXCEPTION(scope, void());
        if (!record)
            continue;
        AbstractModuleRecord::LoadedModuleRequest loaded { vm, request, record, copy };
        loaded.m_wasReached = true;
        Locker locker { copy->cellLock() };
        copy->loadedModules().add(ModuleMapKey { request.m_specifier.impl(), request.type() }, WTF::move(loaded));
    }
    ensureLoaded(*file).whatKeptImportsStandFor.set(*this, nullptr);
    if (left->isCopyEvictedOnceLoaded && host().evictOnceLoaded)
        host().evictOnceLoaded(m_instance.globalObject, key.string());
}

bool Modules::keepsImport(ModuleID importer, ModuleID imported) const
{
    if (m_importsKeptAheadOfLink.isEmpty()) [[likely]]
        return false;
    auto kept = m_importsKeptAheadOfLink.find(importer.index());
    return kept != m_importsKeptAheadOfLink.end() && kept->value.contains(fileOf(imported).index());
}

void Modules::forgetKeptImports(ModuleID importer)
{
    MomentsFilesWereKept files = m_importsKeptAheadOfLink.take(importer.index());
    for (uint32_t moment : files.values())
        didGiveUpMomentLinked(moment);
}

bool Modules::wasLinkedAheadOfPendingMock(ModuleID importer, ModuleID imported) const
{
    ModuleID file = fileOf(imported);
    if (!m_modulesWithPendingMock.get(file.index())) [[likely]]
        return false;
    bool hasRunAhead = goesThroughImportsOfKind(importer) && status(importer) > ModuleStatus::Linked && m_loadsThatKeepImports.containsIf([&](const LoadThatKeepsImports& load) {
        return load.mocksLaidMeanwhile.get(file.index()) && status(load.root) <= ModuleStatus::Linked;
    });
    if (!hasRunAhead && !isLinkedAheadAndYetToRun(importer) && !keepsImport(importer, imported))
        return false;
    return isEvictedForImports(file) || hasPendingMockAfterReset(file) || !(hasInstances(importer) ? isLinkedToMock(importer, imported) : !!activeMock(file));
}

bool Modules::staysUntilLoadEnds(ModuleID module) const
{
    return m_loadsThatKeepImports.containsIf([&](const LoadThatKeepsImports& load) {
        return load.unlinkedImporters.get(module.index()) && status(load.root) <= ModuleStatus::Evaluating;
    });
}

bool Modules::forgetEndedLoadsThatKeptImports()
{
    return m_loadsThatKeepImports.removeAllMatching([&](const LoadThatKeepsImports& load) {
        return status(load.root) > ModuleStatus::Evaluating;
    });
}

void Modules::didFailLoadThatKeptImports(ModuleID root)
{
    size_t load = m_loadsThatKeepImports.findIf([&](const LoadThatKeepsImports& each) { return each.root == root; });
    if (load == notFound) [[likely]]
        return;
    BitVector importers = WTF::move(m_loadsThatKeepImports[load].unlinkedImporters);
    m_loadsThatKeepImports.removeAt(load);
    for (size_t index : importers) {
        ModuleID importer(index);
        if (!isUnlinkedImporterThatKeepsImports(importer) || staysUntilLoadEnds(importer))
            continue;
        forgetKeptImports(importer);
        m_modulesResetWithImporters.clear(index);
        m_isPassOverFailedModulesDue |= m_modulesToEvictOnceLoaded.removeAll(importer) + m_modulesPendingReset.removeAll(importer) > 0;
    }
    if (m_isPassOverFailedModulesDue)
        startLoadingModulesIndividually();
    m_filesAboveNoEvictedFile.clearAll();
    didChangeLinkState();
}

BitVector Modules::modulesLinkedAheadOfImportersThatKeepThem() const
{
    BitVector modules;
    for (auto& [importer, files] : m_importsKeptAheadOfLink) {
        if (!staysUntilLoadEnds(ModuleID(importer)))
            continue;
        for (uint32_t file : files.keys()) {
            if (goesThroughImports(ModuleID(file)) && wasImporterLinked(ModuleID(file)) && !m_importsKeptAheadOfLink.contains(file))
                modules.set(file);
        }
    }
    return modules;
}

bool Modules::isUnlinkedImporterThatKeepsImports(ModuleID module) const
{
    return m_importsKeptAheadOfLink.contains(module.index()) && hasNotBegun(module) && !wasImporterLinked(module);
}

bool Modules::holdsKeptImportsAlone(const ModuleRecord& importer) const
{
    if (!m_instance.vm->importOrderState().isRunningDeferredModule || importer.hasStartedLinking() || wasImporterLinked(importer.module()))
        return false;
    return !importer.wasReached() || isUnlinkedImporterThatKeepsImports(importer.module());
}

Vector<ModuleID, 2> Modules::unlinkedImportersThatKeep(ModuleID module) const
{
    Vector<ModuleID, 2> importers;
    for (uint32_t index : m_importsKeptAheadOfLink.keys()) {
        if (ModuleID importer(index); keepsImport(importer, module) && isUnlinkedImporterThatKeepsImports(importer))
            importers.append(importer);
    }
    return importers;
}

const BitVector& Modules::modulesBelowRoot(WalkOfImports& walk) const
{
    if (!walk.modulesBelowRoot)
        walk.modulesBelowRoot = modulesYetToBeginBelow(walk.root);
    return *walk.modulesBelowRoot;
}

BitVector Modules::modulesYetToBeginBelow(ModuleID root) const
{
    BitVector below(m_table.size());
    below.quickSet(root.index());
    Vector<ModuleID, 32> importers { root };
    while (!importers.isEmpty()) {
        ModuleID importer = importers.takeLast();
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID imported = moduleLoadedBy(request);
            if (request.isHoisted || isLinkedToReleasedInstance(importer, imported))
                continue;
            if (!below.quickSet(imported.index()) && data(imported).isESModuleOfProgram() && hasNotBegun(imported) && !isMocked(imported))
                importers.append(imported);
        }
    }
    return below;
}

bool Modules::isOrImportsRunningFile(ModuleID root) const
{
    BitVector visited;
    visited.set(root.index());
    Vector<ModuleID, 32> modules { root };
    while (!modules.isEmpty()) {
        ModuleID module = modules.takeLast();
        if (isMocked(module))
            continue;
        if (isCommonJSModuleOrFacade(module)) {
            if (status(fileOf(module)) == ModuleStatus::Evaluating)
                return true;
            continue;
        }
        if (!data(module).isESModuleOfProgram())
            continue;
        if (status(module) == ModuleStatus::Evaluating || waitsForEvaluationOfImport(module))
            return true;
        if (!hasNotBegun(module))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (ModuleID imported = moduleLoadedBy(request); !request.isHoisted && !visited.set(imported.index()))
                modules.append(imported);
        }
    }
    return false;
}

bool Modules::wasLoadedForWalk(ModuleID module) const
{
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        if (modulesBelowRoot(*walk).quickGet(module.index()))
            return true;
    }
    return m_modulesLoadedForWalksThatFailed.get(module.index());
}

bool Modules::listTellsWhichFilesRun(ModuleID root) const
{
    return !m_loadsModulesIndividually && m_evictedFiles.isEmpty() && !m_someFileThrewAfterGivingExports && m_filesWithFailureLeftToImports.isEmpty() && !data(root).reachesUncompiledCode() && !loadFailure(root);
}

bool Modules::someESModuleIsBeingEvaluated() const
{
    bool found = false;
    forEachUnsettledModule([&](ModuleID module, ModuleStatus current) {
        found |= current == ModuleStatus::Evaluating && goesThroughImportsOfKind(module);
    });
    return found;
}

void Modules::enterImporter(WalkOfImports& walk, ModuleID module, bool isDeferred, FormAheadOfLink form) const
{
    using MayBeOnCycle = EvaluationAheadOfLink::MayBeOnCycle;
    EvaluationAheadOfLink& evaluation = walk.evaluation;
    if (form == FormAheadOfLink::Module && waitsForEvaluationOfImport(module)) [[unlikely]]
        m_waitingImportersToBeLinked.appendIfNotContains(module);
    bool wasBelowDeferredImport = evaluation.isBelowDeferredImport();
    evaluation.setIsBelowDeferredImport(wasBelowDeferredImport || isDeferred);
    unsigned index = evaluation.enter(moduleAheadOfLink(module, form), data(module).isSimple() && !data(module).reachesUncompiledCode() ? MayBeOnCycle::No : MayBeOnCycle::Yes);
    walk.path.append({ module, m_table.requests(module), index, evaluation.numberOfModulesAhead(), wasBelowDeferredImport, form });
}

void Modules::leaveImporter(WalkOfImports& walk) const
{
    WalkOfImports::Importer left = walk.path.takeLast();
    walk.evaluation.leave(true);
    if (left.form == FormAheadOfLink::ImporterOfOriginal) [[unlikely]]
        passImporterOfOriginal(walk, left.module, left.numberOfModulesAhead);
    else if (!walk.path.isEmpty())
        walk.evaluation.didPassImport(moduleAheadOfLink(left.module, left.form), left.numberOfModulesAhead);
    walk.evaluation.setIsBelowDeferredImport(left.wasBelowDeferredImport);
}

void Modules::passImporterOfOriginal(WalkOfImports& walk, ModuleID mockedFile, size_t numberOfModulesAheadOfIt) const
{
    // Only a compiled importer can be left for the factory to settle.
    if (walk.evaluation.cameThroughRecordBelowRoot())
        walk.evaluation.holdUpUntilWalkEnds();
    else if (!walk.path.isEmpty())
        walk.evaluation.didPassImport(moduleAheadOfLink(mockedFile, FormAheadOfLink::ImporterOfOriginal), numberOfModulesAheadOfIt);
}

void Modules::leaveImportersThatHaveBegun(WalkOfImports& walk)
{
    auto hasBegun = [&](ModuleID importer) {
        return walk.originalsReachedBesideMocks.get(importer.index()) ? wasOriginalEvaluated(importer) : !hasNotBegun(importer);
    };
    while (!walk.path.isEmpty() && hasBegun(walk.path.last().module))
        leaveImporter(walk);
}

std::optional<ModuleID> Modules::nextImport(WalkOfImports& walk) const
{
    EvaluationAheadOfLink& evaluation = walk.evaluation;
    while (!walk.path.isEmpty()) {
        ModuleID importer = walk.path.last().module;
        std::span<const ModuleRequestData>& requestsLeft = walk.path.last().requestsLeft;
        if (requestsLeft.empty()) {
            if (failureBehindImportsOf(importer)) [[unlikely]]
                return importer;
            leaveImporter(walk);
            continue;
        }
        const ModuleRequestData& request = requestsLeft.front();
        requestsLeft = requestsLeft.subspan(1);
        ModuleID imported = request.targetModule();
        if (request.isHoisted || request.isRequire)
            continue;
        if (isLinkedToReleasedInstance(importer, imported) || (keepsImport(importer, imported) && isEvicted(fileOf(imported))))
            continue;
        bool hasImports = goesThroughImportsOfKind(imported);
        if ((hasImports ? wasOriginalEvaluated(imported) : !isCommonJSModuleOrFacade(imported)) && wasLinkedAheadOfPendingMock(importer, imported)) [[unlikely]]
            continue;
        bool runsBesideMock = !walk.goesByList && (hasImports ? runsBesideMockFor(imported, importer) : isFileImportedPastMock(imported, importer));
        auto passWithoutEntering = [&](EvaluationAheadOfLink::Module module) {
            evaluation.didComeBackTo(module);
            if (!request.isDeferred)
                evaluation.didPassImport(module, evaluation.numberOfModulesAhead());
        };
        FormAheadOfLink form = runsBesideMock ? FormAheadOfLink::OriginalBesideMock : FormAheadOfLink::Module;
        bool wasReached = walk.reached.quickSet(imported.index());
        if (runsBesideMock) [[unlikely]] {
            if (!wasReached)
                walk.mocksYetToBeReachedBesideOriginals.set(imported.index());
            wasReached = walk.originalsReachedBesideMocks.set(imported.index());
        } else if (wasReached && walk.mocksYetToBeReachedBesideOriginals.clear(imported.index())) [[unlikely]]
            wasReached = false;
        if (wasReached) {
            if (hasPendingMocks() && !runsBesideMock && isMockedByImporterOfOriginal(fileOf(imported))) [[unlikely]]
                passWithoutEntering(moduleAheadOfLink(fileOf(imported), FormAheadOfLink::ImporterOfOriginal));
            else if (hasImports)
                passWithoutEntering(moduleAheadOfLink(imported, form));
            continue;
        }
        walk.isTurnOfDeferredImport = request.isDeferred;
        if (!hasImports)
            return walk.lastReached = imported;
        if (!walk.goesByList && isMocked(imported) && !runsBesideMock) {
            didLeaveFactoryForLater(walk, imported);
            if (hasImportOfMockToReach(walk, imported)) [[unlikely]]
                return imported;
            continue;
        }
        if (data(imported).bodyThrowsAtLoad()) [[unlikely]]
            return imported;
        if (!walk.goesByList && !hasNotBegun(imported) && !runsBesideMock) {
            if (hasRunAndLeadsToWhatIsDue(imported)) [[unlikely]]
                return imported;
            passWithoutEntering(moduleAheadOfLink(imported));
            continue;
        }
        enterImporter(walk, imported, request.isDeferred, form);
        if (m_modulesThatFailedToImport.get(imported.index())) [[unlikely]]
            return imported;
    }
    return std::nullopt;
}

void Modules::didLeaveFactoryForLater(const WalkOfImports& walk, ModuleID mocked) const
{
    if (!m_modulesWithPendingMock.get(mocked.index()))
        return;
    if (walk.pendingFactoryModules == PendingFactoryModules::Skip && !isMockedByImporterOfOriginal(mocked))
        walk.evaluation.didLeaveImportForLater();
    if (!walk.walkOfLoader || walk.isAheadOfFetch)
        return;
    String key = m_table.fullPath(mocked);
    ModuleRegistryEntry* entry = m_modulesBehindFetches.contains(key) ? loader()->registryEntry(Identifier::fromString(*m_instance.vm, key)) : nullptr;
    if (entry && !entry->record())
        walk.walkOfLoader->didLeaveFactoryToFetch();
    else
        walk.walkOfLoader->didLeaveFactoryForLater();
}

void Modules::catchUpWithList(WalkOfImports& walk) const
{
    if (!walk.goesByList || !walk.moduleInTurn || walk.lastReached == walk.moduleInTurn)
        return;
    SetForScope<EvaluationAheadOfLink*> noneWasUnderWayWhenWalkBegan(m_instance.vm->importOrderState().innermostEvaluationAheadOfLink, nullptr);
    while (walk.lastReached != walk.moduleInTurn && nextImport(walk)) { }
}

void Modules::catchUpWithListsOfImports() const
{
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer)
        catchUpWithList(*walk);
}

void Modules::stopGoingByList(WalkOfImports& walk)
{
    catchUpWithList(walk);
    walk.goesByList = false;
    walk.listedModulesAhead = { };
}

bool Modules::runImportedFiles(ModuleID root, std::optional<JSModuleLoader::EvaluationOfLoad> evaluationOfLoad, const VisitedModuleRecords* visited, bool* didStopAtChangeOfAnswers)
{
    if (WalkOfImports* outer = m_innermostWalkOfImports; outer && visited && outer->visited == visited) {
        WalkOfImports walk { root, outer, outer->visited, outer->reached, outer->evaluation, outer->pendingFactoryModules };
        return runImportedFiles(walk, true);
    }
    EvaluationAheadOfLink evaluation(m_instance.globalObject, nullptr, evaluationOfLoad);
    BitVector reached;
    VisitedModuleRecords visitedByWalk;
    if (visited && data(root).reachesUncompiledCode())
        visitedByWalk = *visited;
    WalkOfImports walk { root, m_innermostWalkOfImports, &visitedByWalk, reached, evaluation, PendingFactoryModules::Evaluate };
    walk.stopsAtChangeOfAnswers = didStopAtChangeOfAnswers;
    bool didRunFile = runImportedFiles(walk, false);
    if (didStopAtChangeOfAnswers)
        *didStopAtChangeOfAnswers = walk.outcome == ImportOrderWalk::Outcome::NeedsImports;
    if (m_innermostWalkOfImports || m_instance.vm->importOrderState().innermostWalk)
        loader()->didStopImportOrderWalk(*m_instance.vm);
    else
        loader()->didEndImportOrderWalk(*m_instance.vm);
    return didRunFile;
}

ImportOrderWalk::Outcome Modules::reachImports(ModuleRecord& record, ImportOrderWalk& walkOfLoader)
{
    using Mode = ImportOrderWalk::Mode;
    using Outcome = ImportOrderWalk::Outcome;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID root = record.module();
    VisitedModuleRecords& visited = walkOfLoader.visited();
    if (walkOfLoader.mode() == Mode::KeepsAnswers) {
        runImportedFilesAndCaptureExports(root, visited);
        RETURN_IF_EXCEPTION(scope, Outcome::Threw);
        return Outcome::Passed;
    }
    if (!visited.add(&record).isNewEntry) {
        walkOfLoader.evaluation().didComeBackTo(record.moduleAheadOfLink());
        return Outcome::Passed;
    }
    record.setWasReached();
    if (walkOfLoader.mode() == Mode::WaitsForImports) {
        didLoadGraph(root);
        RETURN_IF_EXCEPTION(scope, Outcome::Threw);
    }
    WalkOfImports* outer = m_innermostWalkOfImports;
    bool continuesOuterWalk = outer && outer->visited == &visited;
    if (!continuesOuterWalk || hasPendingMocks())
        walkOfLoader.willLinkBehindWalk(&record);
    BitVector reached;
    WalkOfImports walk { root, outer, &visited, continuesOuterWalk ? outer->reached : reached, walkOfLoader.evaluation(), walkOfLoader.leavesFactoriesForLater() ? PendingFactoryModules::Skip : PendingFactoryModules::Evaluate, &walkOfLoader, &record };
    runImportedFiles(walk, continuesOuterWalk);
    if (scope.exception()) [[unlikely]] {
        if (walkOfLoader.didRequestImports() || walkOfLoader.didFailToImport())
            keepFailureToImport(walk);
        EXCEPTION_ASSERT(scope.exception());
        if (!continuesOuterWalk && hasPendingMocks() && walkOfLoader.mode() == Mode::WaitsForImports && walkOfLoader.leavesFactoriesForLater()) {
            scope.release();
            callFactoriesOfLoadThatFailed(root);
        }
        return Outcome::Threw;
    }
    if (hasPendingMocks() && walkOfLoader.leavesFactoriesForLater() && data(root).reachesUncompiledCode()) [[unlikely]]
        walk.outcome = std::max(walk.outcome, Outcome::PassedInPart);
    if (walk.outcome == Outcome::Passed && !continuesOuterWalk && walkOfLoader.mode() == Mode::WaitsForImports && (walkOfLoader.didRequestImports() || mayWalkRecoverLoad())) [[unlikely]]
        didRecoverLoad(walk.reached);
    return walk.outcome;
}

ImportOrderWalk::Outcome Modules::reachImportsAheadOfFetch(ModuleRecord& record, ImportOrderWalk& walkOfLoader)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    WalkOfImports* outer = m_innermostWalkOfImports;
    bool continuesOuterWalk = outer && outer->visited == &walkOfLoader.visited();
    BitVector reached;
    WalkOfImports walk { record.module(), outer, &walkOfLoader.visited(), continuesOuterWalk ? outer->reached : reached, walkOfLoader.evaluation(), PendingFactoryModules::Skip, &walkOfLoader, &record };
    walk.isAheadOfFetch = true;
    runImportedFiles(walk, continuesOuterWalk);
    if (!scope.exception()) [[likely]]
        return walk.outcome;
    scope.release();
    if (walkOfLoader.didFailToImport())
        keepFailureToImport(walk);
    return ImportOrderWalk::Outcome::Threw;
}

void Modules::setModuleBehindFetch(const String& key, std::optional<ModuleID> module)
{
    if (module)
        m_modulesBehindFetches.set(key, *module);
    else
        m_modulesBehindFetches.remove(key);
}

ModuleRecord* Modules::recordBehindFetchOf(UniquedStringImpl* key)
{
    auto behind = m_modulesBehindFetches.find(String(key));
    return behind == m_modulesBehindFetches.end() ? nullptr : ensureRecordWithoutStatus(behind->value);
}

bool Modules::runImportedFiles(WalkOfImports& walk, bool continuesOuterWalk)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID root = walk.root;
    const ModuleData& rootData = data(root);
    if (!rootData.isESModuleOfProgram() || rootData.isFacade()) {
        if (rootData.kind == ModuleKind::Data)
            m_heldFilesReachedByWalks.set(root.index());
        SetForScope innermostWalk(m_innermostWalkOfImports, &walk);
        RELEASE_AND_RETURN(scope, takeTurn(walk, root));
    }
    if (hasRunAndLeadsToWhatIsDue(root) && walk.evaluation.isBelowImporter()) [[unlikely]] {
        walk.reached.ensureSize(m_table.size());
        SetForScope innermostWalk(m_innermostWalkOfImports, &walk);
        RELEASE_AND_RETURN(scope, goThroughImportsThatLeadToWhatIsDue(walk, root));
    }
    if (!hasNotBegun(root) || (!rootData.leadsToImportedFile() && !rootData.reachesUncompiledCode() && !continuesOuterWalk && !m_table.hasLoadFailures() && !hasPendingMocks()))
        return false;
    if (isMocked(root) && !mocksInGraph(root).loadsRootOriginal()) [[unlikely]] {
        didLeaveFactoryForLater(walk, root);
        if (!hasImportOfMockToReach(walk, root))
            return false;
        if (moduleImportedByMock(root) != root)
            RELEASE_AND_RETURN(scope, takeTurnOfImportOfMock(walk, root));
    }
    walk.reached.ensureSize(m_table.size());
    if (walk.reached.quickSet(root.index())) {
        walk.evaluation.didComeBackTo(moduleAheadOfLink(root));
        return false;
    }

    SetForScope innermostWalk(m_innermostWalkOfImports, &walk);
    if (!continuesOuterWalk) {
        loadRuntimeModulesWithSource(root);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (m_rootsLoadedWithoutLoader.isEmpty() || m_rootsLoadedWithoutLoader.last() != root)
        m_rootsLoadedWithoutLoader.append(root);
    enterImporter(walk, root, false, !isMocked(root) ? FormAheadOfLink::Module : mocksInGraph(root).loadsRootOriginal() ? FormAheadOfLink::OriginalBesideMock : FormAheadOfLink::ImporterOfOriginal);
    bool didRunFile = false;
    if (m_modulesThatFailedToImport.get(root.index())) [[unlikely]]
        takeTurn(walk, root);
    if (!scope.exception()) [[likely]]
        didRunFile = goThroughImports(walk);
    if (scope.exception()) [[unlikely]]
        catchUpWithList(walk);
    for (const WalkOfImports::Importer& importer : walk.path | std::views::reverse) {
        walk.evaluation.leave(false);
        walk.evaluation.setIsBelowDeferredImport(importer.wasBelowDeferredImport);
    }
    if (scope.exception()) [[unlikely]] {
        forgetLinkWalk(root);
        m_heldFilesReachedByWalks.merge(walk.reached);
        m_modulesLoadedForWalksThatFailed.merge(modulesBelowRoot(walk));
        didFailLoadThatKeptImports(root);
        bool isFinal = walk.walkOfLoader ? !walk.walkOfLoader->didFailToTakeOverImport() : !leavesFailureToLoad(walk);
        scope.release();
        if (isFinal && (holdsCodeThatMayBeEvicted() || hasPendingMocks()))
            keepFailureToImport(walk);
    }
    return didRunFile;
}

bool Modules::goThroughImports(WalkOfImports& walk)
{
    using Outcome = ImportOrderWalk::Outcome;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool didRunFile = false;
    std::span<const ModuleRequestData> requests = m_table.requests(walk.root);
    std::span<const ModuleRequestData> list = requests.first(std::ranges::find_if_not(requests, [](const ModuleRequestData& request) { return request.isHoisted; }) - requests.begin());
    walk.goesByList = listTellsWhichFilesRun(walk.root) && !walk.evaluation.leavesModulesForLater() && !EvaluationAheadOfLink::isUnderWay(*m_instance.vm) && std::ranges::any_of(list, [&](const ModuleRequestData& listed) {
        return isCommonJSModuleOrFacade(listed.targetModule());
    });
    std::span<const ModuleAheadOfFileData> modulesAhead = walk.goesByList && walk.evaluation.evaluatesWhatItPasses() ? m_table.modulesAheadOfFiles(walk.root) : std::span<const ModuleAheadOfFileData> { };
    if (!modulesAhead.empty() && someESModuleIsBeingEvaluated())
        walk.goesByList = false;
    const VM::ImportOrderState& importOrder = m_instance.vm->importOrderState();
    auto takeTurnOf = [&](ModuleID imported) {
        uint64_t lastChange = importOrder.lastChange;
        bool didRunInTurn = takeTurn(walk, imported);
        if (walk.stopsAtChangeOfAnswers && lastChange != importOrder.lastChange) [[unlikely]]
            walk.outcome = Outcome::NeedsImports;
        return didRunInTurn;
    };
    for (size_t place = data(walk.root).hasCheckOfNames(), numberAhead = 0; walk.goesByList; ++place) {
        if (place == list.size()) {
            leaveImporter(walk);
            return didRunFile;
        }
        ModuleID imported = list[place].targetModule();
        if (storedStatus(imported) == StoredModuleStatus::Evaluated && isCommonJSModuleOrFacade(imported) && (data(imported).isFacade() || m_filesThatTookTheirTurn.get(imported.index())) && !hasMocksToReinstall())
            continue;
        while (numberAhead < modulesAhead.size() && modulesAhead[numberAhead].place <= place)
            ++numberAhead;
        walk.listedModulesAhead = modulesAhead.first(numberAhead);
        didRunFile |= takeTurnOf(imported);
        RETURN_IF_EXCEPTION(scope, true);
        if (walk.listedModulesAhead.empty())
            modulesAhead = modulesAhead.subspan(std::exchange(numberAhead, 0));
        if (walk.outcome == Outcome::NeedsImports) [[unlikely]]
            return didRunFile;
        if (walk.goesByList && !walk.lastReached && hasNotBegun(walk.root) && listTellsWhichFilesRun(walk.root)) [[likely]]
            continue;
        stopGoingByList(walk);
        leaveImportersThatHaveBegun(walk);
    }
    while (std::optional<ModuleID> imported = nextImport(walk)) {
        bool didRunInTurn = takeTurnOf(*imported);
        RETURN_IF_EXCEPTION(scope, true);
        didRunFile |= didRunInTurn;
        if (walk.outcome == Outcome::NeedsImports || walk.evaluation.didStopAtDeferredModule()) [[unlikely]]
            break;
        if (didRunInTurn)
            leaveImportersThatHaveBegun(walk);
    }
    return didRunFile;
}

// `bun test` loads such an importer anew, so what is due below it has its turn in the walk.
bool Modules::goThroughImportsThatLeadToWhatIsDue(WalkOfImports& walk, ModuleID importerThatRan)
{
    using Outcome = ImportOrderWalk::Outcome;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    checkSafeToRecurse(m_instance.globalObject, scope);
    RETURN_IF_EXCEPTION(scope, false);
    uint64_t lastChange = m_instance.vm->importOrderState().lastChange;
    bool didRunFile = false;
    for (const ModuleRequestData& request : m_table.imports(importerThatRan)) {
        ModuleID imported = moduleLoadedBy(request);
        ModuleID file = fileOf(imported);
        if (request.isHoisted || request.isDeferred || walk.reached.quickGet(imported.index()))
            continue;
        bool isOwed = m_modulesOwedToImportersThatRan.get(imported.index()) && hasNotBegun(imported) && !isMocked(file) && !activeMock(file);
        bool isDue = isOwed || (isCommonJSModuleOrFacade(imported) ? status(imported) == ModuleStatus::Evaluated && m_modulesNeedingMockReinstall.get(file.index()) : leadsToWhatIsDue(imported));
        if (!isDue || keepsWhatItLinked(importerThatRan, imported) || seesEndedMockLaidInPreload(file, importerThatRan))
            continue;
        releaseRemovedMock(imported);
        RETURN_IF_EXCEPTION(scope, true);
        if (isOwed && goesThroughImportsOfKind(imported)) {
            WalkOfImports below { imported, m_innermostWalkOfImports, walk.visited, walk.reached, walk.evaluation, walk.pendingFactoryModules, walk.walkOfLoader, walk.recordOfRoot };
            below.stopsAtChangeOfAnswers = walk.stopsAtChangeOfAnswers;
            didRunFile |= runImportedFiles(below, true);
            walk.outcome = std::max(walk.outcome, below.outcome);
        } else {
            walk.reached.quickSet(imported.index());
            bool hasNoStatusOfItsRun = goesThroughImportsOfKind(imported) && status(imported) != ModuleStatus::Evaluated;
            didRunFile |= hasNoStatusOfItsRun ? goThroughImportsThatLeadToWhatIsDue(walk, imported) : takeTurn(walk, imported);
        }
        RETURN_IF_EXCEPTION(scope, true);
        if (walk.stopsAtChangeOfAnswers && lastChange != m_instance.vm->importOrderState().lastChange)
            walk.outcome = Outcome::NeedsImports;
        if (walk.outcome == Outcome::NeedsImports || walk.evaluation.didStopAtDeferredModule())
            break;
    }
    return didRunFile;
}

EvaluationAheadOfLink::Module Modules::moduleAheadOfLink(ModuleRecord& record) const
{
    ModuleID module = record.module();
    if (!record.keepsToModule() && isMockedByImporterOfOriginal(fileOf(module))) [[unlikely]]
        return moduleAheadOfLink(fileOf(module), FormAheadOfLink::ImporterOfOriginal);
    bool standsBesideModule = record.keepsToModule() || record.belongsToReleasedInstance() || m_records.get(module) != &record;
    return standsBesideModule && !isCommonJSModuleOrFacade(module) ? JSValue(&record) : moduleAheadOfLink(module);
}

bool Modules::isToBeEvaluatedAheadOfLink(EvaluationAheadOfLink::Module moduleAhead, bool isLeftForLater)
{
    ModuleID module = compiledModuleOf(moduleAhead);
    switch (formOf(moduleAhead)) {
    case FormAheadOfLink::Module:
        break;
    case FormAheadOfLink::OriginalBesideMock:
        return !wasOriginalEvaluated(module);
    case FormAheadOfLink::ImporterOfOriginal:
        return needsOriginalBeforeFactory(module) && !wasOriginalRunInSweep(module);
    }
    if (!goesThroughImportsOfKind(module) || isReplacedByMock(module) || isHeldAboveMock(module))
        return false;
    switch (status(module)) {
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linked:
        if (!isLeftForLater && (data(module).needsEvaluation() || data(module).needsLink()))
            return true;
        // In a test run mocks and require.cache show whether a module without code to run has been evaluated.
        return (isLeftForLater || m_table.hasSourceStamps()) && !hasRun(module);
    case ModuleStatus::Failed:
        return true;
    case ModuleStatus::Evaluated: {
        if (hasRunAndLeadsToWhatIsDue(module)) [[unlikely]]
            return true;
        ModuleRecord* record = recordIfExists(module);
        return record && record->cycleRoot() && record->cycleRoot()->evaluationError();
    }
    case ModuleStatus::Linking:
    case ModuleStatus::Evaluating:
    case ModuleStatus::EvaluatingAsync:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSPromise* Modules::evaluateAheadOfImporters(EvaluationAheadOfLink::Module moduleAhead, const JSModuleLoader::EvaluationOfLoad& load)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID module = compiledModuleOf(moduleAhead);
    if (formOf(moduleAhead) == FormAheadOfLink::OriginalBesideMock) [[unlikely]] {
        linkAheadOfImporters(moduleAhead);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    } else if (formOf(moduleAhead) == FormAheadOfLink::Module && hasRunAndLeadsToWhatIsDue(module)) [[unlikely]] {
        BitVector isPassed;
        passModuleThatHasRun(module, isPassed);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    } else if (comesToImporterOfOriginal(module)) [[unlikely]] {
        ModuleID root = rootOfWalkThatCameTo(module);
        sweepAheadOfFactories(root, module);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (formOf(moduleAhead) == FormAheadOfLink::Module && hasNotBegun(module))
            m_modulesHeldAheadOfLink.append({ root, module });
        return nullptr;
    }
    RELEASE_AND_RETURN(scope, evaluateLinkedModule(module, load.referrerAsyncOrder < 0 ? std::nullopt : std::optional(load.referrerAsyncOrder), load.dynamicImportPromise));
}

bool Modules::comesToImporterOfOriginal(ModuleID first) const
{
    if (!hasPendingMocks()) [[likely]]
        return false;
    BitVector seen(m_table.size());
    seen.quickSet(first.index());
    Vector<ModuleID, 32> modules { first };
    while (!modules.isEmpty()) {
        ModuleID module = modules.takeLast();
        if (isMockedByImporterOfOriginal(fileOf(module)))
            return true;
        if (!goesThroughImports(module) || isMocked(module))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID imported = moduleLoadedBy(request);
            if (!request.isHoisted && !importsOriginalDespiteMock(imported, module) && !seen.quickSet(imported.index()))
                modules.append(imported);
        }
    }
    return false;
}

ModuleID Modules::rootOfWalkThatCameTo(ModuleID module) const
{
    ModuleID root = module;
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk && &walk->evaluation == m_instance.vm->importOrderState().innermostEvaluationAheadOfLink; walk = walk->outer) {
        if (modulesBelowRoot(*walk).quickGet(moduleWithExportsOf(module).index()))
            root = walk->root;
    }
    return root;
}

bool Modules::walksGoThroughOriginalOf(ModuleID mocked) const
{
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        if (walk->path.containsIf([&](const WalkOfImports::Importer& importer) { return importer.module == mocked; }))
            return true;
    }
    return false;
}

// Returns whether the file is to take its turn now.
bool Modules::evaluateModulesAheadOfFile(WalkOfImports& walk)
{
    using KeepsAnswers = EvaluationAheadOfLink::KeepsAnswers;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    EvaluationAheadOfLink& evaluation = walk.evaluation;
    if (evaluation.stopsAtDeferredModule()) [[unlikely]] {
        walk.outcome = std::max(walk.outcome, ImportOrderWalk::Outcome::PassedInPart);
        return false;
    }
    EvaluationAheadOfLink::Result result = evaluation.evaluateModulesAhead(walk.stopsAtChangeOfAnswers ? KeepsAnswers::No : KeepsAnswers::Yes);
    RETURN_IF_EXCEPTION(scope, false);
    if (result == EvaluationAheadOfLink::Result::ChangedAnswers) [[unlikely]] {
        walk.outcome = ImportOrderWalk::Outcome::NeedsImports;
        return false;
    }
    std::span<const ModuleAheadOfFileData> listed = std::exchange(walk.listedModulesAhead, { });
    if (listed.empty())
        return true;
    auto goOnWithoutList = [&] {
        stopGoingByList(walk);
        RELEASE_AND_RETURN(scope, evaluateModulesAheadOfFile(walk));
    };
    if (walk.lastReached || !listTellsWhichFilesRun(walk.root)) [[unlikely]]
        return goOnWithoutList();
    for (const ModuleAheadOfFileData& each : listed) {
        evaluation.link(moduleAheadOfLink(each.moduleAhead()));
        RETURN_IF_EXCEPTION(scope, false);
    }
    uint64_t lastChange = m_instance.vm->importOrderState().lastChange;
    for (const ModuleAheadOfFileData& each : listed) {
        EvaluationAheadOfLink::Module module = moduleAheadOfLink(each.moduleAhead());
        if (!evaluation.isToBeEvaluated(module))
            continue;
        JSPromise* promise = evaluation.evaluate(module);
        RETURN_IF_EXCEPTION(scope, false);
        bool didThrow = promise && promise->status() == JSPromise::Status::Rejected;
        if (!didThrow && walk.stopsAtChangeOfAnswers && lastChange != m_instance.vm->importOrderState().lastChange) [[unlikely]] {
            walk.outcome = ImportOrderWalk::Outcome::NeedsImports;
            return false;
        }
        if (didThrow || walk.lastReached || !listTellsWhichFilesRun(walk.root)) [[unlikely]]
            return goOnWithoutList();
    }
    return true;
}

std::optional<ModuleFailureID> Modules::failureBehindImportsOf(ModuleID module) const
{
    if (!m_table.hasLoadFailures()) [[likely]]
        return std::nullopt;
    std::optional<ModuleFailureID> failure = loadFailure(module);
    if (!failure || m_table.failure(*failure).isAbsentExport() || m_table.failure(*failure).importerModule() != module)
        return std::nullopt;
    return failure;
}

JSValue Modules::errorOfImportThatFailedForGood(ModuleID module)
{
    if (!m_modulesThatFailedToImport.get(module.index())) [[likely]]
        return { };
    ModuleRecord* record = recordIfExists(module);
    if (ModuleRegistryEntry* entry = record ? loader()->entryOf(record) : nullptr; entry && entry->isFailureFinal())
        return entry->error(m_instance.globalObject);
    m_modulesThatFailedToImport.clear(module.index());
    return { };
}

void Modules::failToImport(WalkOfImports& walk, JSValue error)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!walk.walkOfLoader) {
        throwException(m_instance.globalObject, scope, error);
        return;
    }
    if (hasPendingMocks() && walk.walkOfLoader->willCallFactoriesAheadOfFailure()) [[unlikely]] {
        bool asksAgain = callFactoriesAheadOfFailure(walk, error);
        RETURN_IF_EXCEPTION(scope, void());
        if (asksAgain)
            return;
    }
    scope.release();
    walk.outcome = walk.walkOfLoader->failToImport(walk.recordOfRoot, error);
}

ModuleRegistryEntry* Modules::entryThatKeepsFailureOf(ModuleID module)
{
    ModuleRecord* record = ensureRecord(module);
    record->retain();
    ModuleRegistryEntry* entry = loader()->ensureRegistered(m_instance.globalObject, record->moduleKey(), ScriptFetchParameters::Type::JavaScript);
    if (!entry->record() && (entry->status() == ModuleRegistryEntry::Status::New || (entry->status() == ModuleRegistryEntry::Status::Fetching && !isEvicted(module))))
        entry->provideModule(*m_instance.vm, record);
    return entry->record() == record ? entry : nullptr;
}

void Modules::keepFailureToLink(ModuleID root)
{
    if (!m_linkFailures.contains(root) || isMocked(root))
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Exception* exception = scope.exception();
    TRY_CLEAR_EXCEPTION(scope, void());
    if (ModuleRegistryEntry* entry = entryThatKeepsFailureOf(root)) {
        setStatus(root, ModuleStatus::Unlinked);
        entry->record()->setHasStartedLinking();
        entry->setInstantiationError(m_instance.globalObject, exception->value());
    }
    throwException(m_instance.globalObject, scope, exception);
}

bool Modules::leavesFailureToLoad(const WalkOfImports& walk) const
{
    return walk.evaluation.runsAheadOfLoad() && std::ranges::all_of(walk.path, [&](const WalkOfImports::Importer& importer) {
        return hasNotBegun(importer.module);
    });
}

void Modules::keepFailureToImport(const WalkOfImports& walk)
{
    using IsFinal = ModuleRegistryEntry::IsFinal;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Exception* exception = scope.exception();
    TRY_CLEAR_EXCEPTION(scope, void());
    for (const WalkOfImports::Importer& importer : walk.path) {
        if (!walk.evaluation.isFailureOfImportOf(importer.index) || !hasNotBegun(importer.module) || isMocked(importer.module))
            continue;
        ModuleRegistryEntry* entry = entryThatKeepsFailureOf(importer.module);
        if (!entry)
            continue;
        ensureRecord(importer.module)->setStatus(CyclicModuleRecord::Status::New);
        if (!entry->isFailureFinal())
            entry->failToLoad(m_instance.globalObject, exception->value(), isOnPathOfWalk(importer.module, walk.walkOfLoader) ? IsFinal::No : IsFinal::Yes, walk.evaluation.didModuleThrow());
        m_modulesThatFailedToImport.set(importer.module.index());
    }
    throwException(m_instance.globalObject, scope, exception);
}

bool Modules::mayWalkRecoverLoad()
{
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        if (walk->walkOfLoader && walk->walkOfLoader->didRequestImports())
            return true;
    }
    if (m_modulesWhoseRecordFailedToLoad.isEmpty()) [[likely]]
        return false;
    for (size_t index : BitVector(m_modulesWhoseRecordFailedToLoad)) {
        ModuleRecord* record = recordIfExists(ModuleID(index));
        ModuleRegistryEntry* entry = record ? loader()->entryOf(record) : nullptr;
        if (entry && JSModuleLoader::mayRecoverLoad(entry))
            return true;
        m_modulesWhoseRecordFailedToLoad.clear(index);
    }
    return false;
}

void Modules::didRecoverLoad(const BitVector& modules)
{
    for (size_t index : modules) {
        ModuleID module(index);
        ModuleRecord* record = goesThroughImports(module) && !isMocked(module) ? recordIfExists(module) : nullptr;
        if (!record || !record->mayRecoverLoad())
            continue;
        ModuleRegistryEntry* entry = loader()->entryOf(record);
        if (entry && entry->isFailureFinal())
            continue;
        record->setHasUnresolvableRequest(false);
        m_failedLoadModules.clear(index);
        m_modulesWhoseRecordFailedToLoad.clear(index);
        record->setStatus(CyclicModuleRecord::Status::Unlinked);
        if (entry)
            entry->forgetFailureToLoad(m_instance.globalObject);
    }
}

AbstractModuleRecord* Modules::findUncompiledRecord(ModuleID external, ImportOrderWalk& walkOfLoader, ImportOrderWalk::Turn turn, JSValue& failureToResolve)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ModuleID importer = *data(external).referrerModule();
    LoadedModule& loaded = ensureLoaded(external);
    if (AbstractModuleRecord* record = loaded.uncompiledRecord) {
        if (record->isSupersededForEarlierImports() && !loader()->entryOf(record) && !wasImporterLinked(importer)) [[unlikely]] {
            m_modulesWithRecordOfReleasedImporter.set(external.index());
            if (ModuleRecord* compiled = compiledModule(vm, record); compiled && compiled->hasGivenWayToCopy())
                loaded.resolvedKey = compiled->moduleKey();
        }
        if (!m_modulesWithRecordOfReleasedImporter.get(external.index()))
            return record;
    }
    if (loaded.resolvedKey.isNull()) [[unlikely]] {
        Identifier importerKey = moduleKey(importer);
        Identifier name = Identifier::fromString(vm, m_table.fullPath(external));
        failureToResolve = walkOfLoader.failureToResolve(loader(), importerKey, name, turn);
        if (failureToResolve)
            return nullptr;
        resolvedKey(external);
        if (Exception* exception = scope.exception()) {
            TRY_CLEAR_EXCEPTION(scope, nullptr);
            keepFailureToResolve(importerKey, name, exception);
            failureToResolve = exception->value();
            return nullptr;
        }
    }
    if (ModuleRegistryEntry* entry = registryEntry(loaded.resolvedKey, fetchParametersOf(external).get()); entry && entry->record())
        return entry->record();
    if (std::optional<ModuleID> compiled = findModuleThatAnswersImportsOf(loaded.resolvedKey))
        RELEASE_AND_RETURN(scope, recordForResolution(moduleWithExportsOf(fileOf(*compiled))));
    return nullptr;
}

bool Modules::requestImportsBelow(ModuleRecord& root, ImportOrderWalk& walkOfLoader)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto isGoneThrough = [&](ModuleID module) {
        if (!goesThroughImports(module))
            return false;
        if (!isMocked(module) && !m_modulesWithPendingMock.get(module.index())) [[likely]]
            return true;
        return needsOriginalBeforeFactory(module) && moduleImportedByMock(module) == module;
    };
    if (!isGoneThrough(root.module()))
        return true;
    BitVector reached(m_table.size());
    reached.quickSet(root.module().index());
    Vector<WalkOfImports::Importer, 16> path { { root.module(), m_table.requests(root.module()) } };
    while (!path.isEmpty()) {
        std::span<const ModuleRequestData>& requestsLeft = path.last().requestsLeft;
        if (requestsLeft.empty()) {
            if (failureBehindImportsOf(path.last().module))
                return false;
            path.removeLast();
            continue;
        }
        const ModuleRequestData& request = requestsLeft.front();
        requestsLeft = requestsLeft.subspan(1);
        ModuleID imported = request.targetModule();
        if (request.isHoisted || request.isRequire || reached.quickSet(imported.index()))
            continue;
        if (!data(imported).isUncompiledCode()) {
            if (!isGoneThrough(imported))
                continue;
            if (data(imported).bodyThrowsAtLoad())
                return false;
            JSValue error = errorOfImportThatFailedForGood(imported);
            RETURN_IF_EXCEPTION(scope, false);
            if (error)
                return false;
            path.append({ imported, m_table.requests(imported) });
            continue;
        }
        JSValue failureToResolve;
        AbstractModuleRecord* record = findUncompiledRecord(imported, walkOfLoader, ImportOrderWalk::Turn::IsToCome, failureToResolve);
        RETURN_IF_EXCEPTION(scope, false);
        if (failureToResolve)
            return false;
        if (ModuleRecord* compiled = compiledModule(*m_instance.vm, record); compiled && walkOfLoader.letsGiveWayToCopy(compiled) && compiled->tryGiveWayToCopyAfterChangeOfPlugins()) [[unlikely]] {
            record = findUncompiledRecord(imported, walkOfLoader, ImportOrderWalk::Turn::IsToCome, failureToResolve);
            RETURN_IF_EXCEPTION(scope, false);
        }
        bool shouldContinue = record ? walkOfLoader.requestImportsBelowImport(record) : walkOfLoader.requestImportThatHasNotLoaded(&root, { ensureLoaded(imported).resolvedKey, fetchParametersOf(imported) });
        RETURN_IF_EXCEPTION(scope, false);
        if (!shouldContinue)
            return false;
    }
    return true;
}

bool Modules::tryGiveWayToCopyFromDisk(ModuleRecord& record)
{
    ModuleID module = record.module();
    if (m_evictedFiles.isEmpty() || !goesThroughImports(module) || wasImporterLinked(module) || isMocked(module) || m_instance.vm->aotState().filesStartedByHost.get(module.index()) || !delegatesWithoutPlugins(module)) [[likely]]
        return false;
    giveWayToCopy(module);
    return true;
}

bool Modules::takeTurnOfUncompiledCode(WalkOfImports& walk, ModuleID external)
{
    using Outcome = ImportOrderWalk::Outcome;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (ImportOrderWalk* walkOfLoader = walk.walkOfLoader) {
        JSValue failureToResolve;
        bool isLoaded = findUncompiledRecord(external, *walkOfLoader, ImportOrderWalk::Turn::HasCome, failureToResolve);
        RETURN_IF_EXCEPTION(scope, false);
        if (failureToResolve) [[unlikely]] {
            scope.release();
            failToImport(walk, failureToResolve);
            return false;
        }
        if (!isLoaded) [[unlikely]] {
            scope.release();
            Outcome outcome = walkOfLoader->reachImportThatHasNotLoaded(walk.recordOfRoot, { ensureLoaded(external).resolvedKey, fetchParametersOf(external) });
            if (outcome == Outcome::PassedInPart)
                walk.evaluation.didLeaveImportForLater();
            walk.outcome = std::max(walk.outcome, outcome);
            return false;
        }
    }
    AbstractModuleRecord* record = linkUncompiledRecord(external);
    RETURN_IF_EXCEPTION(scope, false);
    checkSafeToRecurse(m_instance.globalObject, scope);
    RETURN_IF_EXCEPTION(scope, false);
    uint64_t clock = m_instance.vm->importOrderState().clock;
    EvaluationAheadOfLink& evaluation = walk.evaluation;
    size_t numberOfModulesAhead = evaluation.numberOfModulesAhead();
    bool isBelowDeferredImport = evaluation.isBelowDeferredImport();
    evaluation.setIsBelowDeferredImport(isBelowDeferredImport || walk.isTurnOfDeferredImport);
    auto didLeaveImport = makeScopeExit([&] {
        evaluation.setIsBelowDeferredImport(isBelowDeferredImport);
    });
    Outcome outcome = Outcome::Passed;
    if (walk.walkOfLoader)
        outcome = walk.walkOfLoader->reachImport(record);
    else
        ImportOrderWalk::runWithoutWaiting(m_instance.globalObject, record, *walk.visited, AbstractModuleRecord::IsBehindWalk::No, &evaluation);
    RETURN_IF_EXCEPTION(scope, true);
    walk.outcome = std::max(walk.outcome, outcome);
    if (outcome == Outcome::PassedInPart)
        evaluation.didLeaveImportForLater();
    if (auto* cyclic = outcome <= Outcome::PassedInPart ? dynamicDowncast<CyclicModuleRecord>(record) : nullptr)
        evaluation.didPassImport(EvaluationAheadOfLink::moduleOf(*m_instance.vm, cyclic), numberOfModulesAhead);
    return clock != m_instance.vm->importOrderState().clock;
}

bool Modules::takeTurn(WalkOfImports& walk, ModuleID imported)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& importedData = data(imported);
    walk.moduleInTurn = imported;
    if (isCommonJSModuleOrFacade(imported)) {
        ModuleID file = fileOf(imported);
        if (data(file).isOfLibrary())
            return false;
        RELEASE_AND_RETURN(scope, takeTurnOfFile(walk, moduleWithExportsOf(file)));
    }
    bool isLibraryModule = importedData.exportRule == SyntheticExportRule::RuntimeModule;
    if ((isLibraryModule || importedData.kind == ModuleKind::Data) && m_modulesWithPendingMock.get(imported.index())) [[unlikely]] {
        didLeaveFactoryForLater(walk, imported);
        RELEASE_AND_RETURN(scope, hasImportOfMockToReach(walk, imported) && takeTurnOfImportOfMock(walk, imported));
    }
    if (isLibraryModule) {
        scope.release();
        captureExportsOfLibraryModule(imported);
        return false;
    }
    if (importedData.isUncompiledCode())
        RELEASE_AND_RETURN(scope, takeTurnOfUncompiledCode(walk, imported));
    if (!importedData.isESModuleOfProgram()) [[likely]]
        return false;
    if (hasRunAndLeadsToWhatIsDue(imported)) [[unlikely]] {
        size_t numberOfModulesAhead = walk.evaluation.numberOfModulesAhead();
        bool didRunFile = goThroughImportsThatLeadToWhatIsDue(walk, imported);
        RETURN_IF_EXCEPTION(scope, true);
        if (walk.outcome != ImportOrderWalk::Outcome::NeedsImports && !walk.isTurnOfDeferredImport)
            walk.evaluation.didPassImport(moduleAheadOfLink(imported), numberOfModulesAhead);
        return didRunFile;
    }
    if (hasImportOfMockToReach(walk, imported) && walk.path.last().module != imported) [[unlikely]]
        RELEASE_AND_RETURN(scope, takeTurnOfImportOfMock(walk, imported));
    JSValue error = errorOfImportThatFailedForGood(imported);
    RETURN_IF_EXCEPTION(scope, false);
    if (error && loader()->entryOf(recordIfExists(imported))->wasErrorThrownByModule()) {
        bool takesTurn = evaluateModulesAheadOfFile(walk);
        RETURN_IF_EXCEPTION(scope, true);
        if (!takesTurn)
            return true;
        walk.evaluation.setDidModuleThrow();
    }
    if (!error && (importedData.bodyThrowsAtLoad() || walk.path.last().requestsLeft.empty())) {
        if (std::optional<ModuleFailureID> failure = failureBehindImportsOf(imported))
            throwFailure(*failure, false);
        else if (importedData.bodyThrowsAtLoad())
            evaluateModuleInRecord(imported);
        if (Exception* exception = scope.exception()) {
            TRY_CLEAR_EXCEPTION(scope, false);
            error = exception->value();
        }
    }
    scope.release();
    if (error)
        failToImport(walk, error);
    return false;
}

std::optional<ModuleID> Modules::moduleImportedByMock(ModuleID mocked) const
{
    String path = host().fileImportedByMock ? host().fileImportedByMock(m_instance.globalObject, mocked) : String();
    if (path.isNull())
        return std::nullopt;
    return path == m_table.fullPath(mocked) ? mocked : m_table.findByPath(path);
}

bool Modules::takeTurnOfImportOfMock(WalkOfImports& walk, ModuleID mocked)
{
    std::optional<ModuleID> imported = moduleImportedByMock(mocked);
    if (!imported)
        return false;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool isFile = isCommonJSModuleOrFacade(mocked);
    if (*imported == mocked && !isFile && hasNotBegun(mocked)) {
        enterImporter(walk, mocked, walk.isTurnOfDeferredImport, FormAheadOfLink::ImporterOfOriginal);
        return false;
    }
    size_t numberOfModulesAhead = walk.evaluation.numberOfModulesAhead();
    bool didRunFile = false;
    if (*imported != mocked) {
        WalkOfImports below { moduleWithExportsOf(*imported), m_innermostWalkOfImports, walk.visited, walk.reached, walk.evaluation, walk.pendingFactoryModules, walk.walkOfLoader, walk.recordOfRoot };
        below.stopsAtChangeOfAnswers = walk.stopsAtChangeOfAnswers;
        didRunFile = runImportedFiles(below, true);
        walk.outcome = std::max(walk.outcome, below.outcome);
    } else if (isFile)
        didRunFile = runOriginalInItsTurn(walk, mocked);
    RETURN_IF_EXCEPTION(scope, true);
    if (!walk.isTurnOfDeferredImport && walk.outcome != ImportOrderWalk::Outcome::NeedsImports && !walk.evaluation.didStopAtDeferredModule())
        passImporterOfOriginal(walk, mocked, numberOfModulesAhead);
    return didRunFile;
}

void Modules::runFileInItsTurn(ModuleID moduleOrFacade)
{
    if (!hasNotBegun(moduleOrFacade) || data(fileOf(moduleOrFacade)).isOfLibrary())
        return;
    BitVector reached;
    EvaluationAheadOfLink evaluation(m_instance.globalObject, nullptr, std::nullopt);
    WalkOfImports walk { moduleOrFacade, m_innermostWalkOfImports, nullptr, reached, evaluation, PendingFactoryModules::Evaluate };
    SetForScope innermostWalk(m_innermostWalkOfImports, &walk);
    walk.moduleInTurn = moduleOrFacade;
    takeTurnOfFile(walk, moduleWithExportsOf(fileOf(moduleOrFacade)));
}

bool Modules::takeTurnOfFile(WalkOfImports& walk, ModuleID imported)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ModuleID file = fileOf(imported);
    if (isDisplacedRunKept(imported, walk.root)) [[unlikely]]
        return false;
    bool hasFactoryToCall = isMocked(file) && !activeMock(file);
    auto isImportedPastMock = [&] {
        if (fileOf(walk.root) != file)
            return hasBypassingImporterBelow(walk.root, imported);
        for (const LoadInFlight& load : m_loadsInFlight) {
            if (hasBypassingImporterBelow(load.root, imported))
                return true;
        }
        return false;
    };
    if (walk.pendingFactoryModules == PendingFactoryModules::Skip && hasFactoryToCall && (walk.path.isEmpty() || !isFileImportedPastMock(imported, walk.path.last().module))) [[unlikely]] {
        walk.outcome = std::max(walk.outcome, ImportOrderWalk::Outcome::PassedInPart);
        didLeaveFactoryForLater(walk, file);
        RELEASE_AND_RETURN(scope, hasImportOfMockToReach(walk, file) && takeTurnOfImportOfMock(walk, file));
    }
    if (m_filesWithFailureLeftToImports.get(file.index())) [[unlikely]] {
        JSValue error = ensureLoaded(file).errorLeftToImports.get();
        bool takesTurn = evaluateModulesAheadOfFile(walk);
        RETURN_IF_EXCEPTION(scope, true);
        if (!takesTurn)
            return true;
        walk.evaluation.setDidModuleThrow();
        throwException(m_instance.globalObject, scope, error);
        return true;
    }
    bool isOriginalDue = activeMock(imported) && hasBypassingImporterBelow(walk.root, imported);
    ModuleStatus statusAtStart = status(imported);
    if (statusAtStart == ModuleStatus::Evaluating || (statusAtStart == ModuleStatus::Evaluated && status(file) == ModuleStatus::Evaluating)) [[unlikely]] {
        if (walk.evaluation.stopsAtDeferredModule()) {
            walk.outcome = std::max(walk.outcome, ImportOrderWalk::Outcome::PassedInPart);
            return false;
        }
        scope.release();
        captureExportsSoFar(imported);
        return false;
    }
    bool hasTakenTurn = statusAtStart == ModuleStatus::Evaluated && (imported != file || m_filesThatTookTheirTurn.get(file.index()));
    bool isAnsweredByMock = activeMock(imported) || (hasFactoryToCall && !isImportedPastMock());
    bool repeatsMocks = statusAtStart == ModuleStatus::Evaluated && !isAnsweredByMock && m_modulesNeedingMockReinstall.get(file.index());
    if (isOriginalDue || repeatsMocks || (!hasTakenTurn && !isAnsweredByMock)) {
        uint64_t clock = vm.importOrderState().clock;
        bool takesTurn = evaluateModulesAheadOfFile(walk);
        RETURN_IF_EXCEPTION(scope, true);
        if (!takesTurn)
            return clock != vm.importOrderState().clock;
        if (clock != vm.importOrderState().clock) {
            scope.release();
            takeTurnOfFile(walk, imported);
            return true;
        }
    }
    if (imported == file)
        m_filesThatTookTheirTurn.set(file.index());
    auto captureExportsOfFile = [&] {
        if (data(file).isImportableDuringEvaluation()) [[unlikely]] {
            for (ExportsSoFar capturedBefore : { ExportsSoFar::OfThisRun, ExportsSoFar::OfRunThatThrew }) {
                captureExportsOfFinishedFile(file, capturedBefore);
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
        if (imported != file ? data(imported).capturesExports() : data(file).capturesExports() || recordIfExists(file))
            RELEASE_AND_RETURN(scope, captureExports(imported));
    };
    if (statusAtStart == ModuleStatus::Evaluated && !isOriginalDue && !repeatsMocks) {
        captureExportsOfFile();
        return false;
    }

    SetForScope fileStarted(walk.fileStarted, hasNotBegun(file) || isMockLingering(file) ? std::optional(file) : std::nullopt);
    if (walk.fileStarted && (fileOf(walk.root) == file || data(file).isEntryPoint() || m_setsMainModulePath) && !isMocked(file)) {
        bool isEntryPoint = m_fileThatRanAsMain != file && (host().isEntryPoint ? host().isEntryPoint(m_instance.globalObject, m_table.fullPath(file)) : isMainEntryPoint(file));
        m_filesStartedAsEntryPoint.set(file.index(), isEntryPoint);
        if (isEntryPoint) {
            forgetFileThatRanAsMain();
            m_fileThatRanAsMain = file;
        }
    }
    VM::ImportOrderState& importOrder = vm.importOrderState();
    SetForScope isRunningDeferredModule(importOrder.isRunningDeferredModule, true);
    ++importOrder.clock;
    auto didRun = makeScopeExit([&] {
        ++importOrder.clock;
        if (!vm.exceptionForInspection()) [[likely]]
            return;
        if (!isAnsweredByMock)
            walk.evaluation.setDidModuleThrow();
        leaveFailureToImports(file);
    });
    if (repeatsMocks) [[unlikely]] {
        reinstallBodyMocks(file);
        RETURN_IF_EXCEPTION(scope, true);
    } else if (!isOriginalDue) [[likely]] {
        if (data(imported).capturesExportsBehindEvaluation())
            captureExportsBehindEvaluation(imported);
        if (hasFactoryToCall && isImportedPastMock()) [[unlikely]]
            evaluatePastPendingMock(imported);
        else if (storedStatus(imported) < StoredModuleStatus::InRecord && data(imported).evaluatesItself())
            evaluateByCallingBody(imported);
        else
            evaluateModuleInRecord(imported);
        RETURN_IF_EXCEPTION(scope, true);
        isOriginalDue = activeMock(imported) && hasBypassingImporterBelow(walk.root, imported);
    }
    if (isOriginalDue) [[unlikely]] {
        runFileBesideMock(file);
        RETURN_IF_EXCEPTION(scope, true);
    }
    if (status(file) == ModuleStatus::Evaluating) [[unlikely]] {
        ensureLoaded(file).exportsSoFar = ExportsSoFar::OfThisRun;
        scope.release();
        updateCapturedExports(imported);
    } else if (status(imported) == ModuleStatus::Evaluated)
        captureExportsOfFile();
    return true;
}

void Modules::didFailToLoadGraph(ModuleID root)
{
    m_modulesWhoseRecordFailedToLoad.set(root.index());
    m_loadsInFlight.didLoad(root);
    if (!m_levelsOfLoads.isEmpty()) [[unlikely]] {
        m_levelsOfLoads.removeIf([&](auto& entry) {
            return entry.value->plan->found[0].module == root;
        });
    }
    resumeGraphsDeferredForImports();
}

void Modules::didLoadGraph(ModuleID root)
{
    m_loadsInFlight.didLoad(root);
    resumeGraphsDeferredForImports();
}

unsigned Modules::numberOfGraphsBeingLoaded()
{
    m_loadsInFlight.removeSettled();
    return m_loadsInFlight.numberBeingLoaded();
}

bool Modules::comesToLoadInFlight(ModuleID required)
{
    while (LoadInFlight* load = m_loadsInFlight.takeWalkedWithoutMarks()) {
        if (RefPtr plan = std::exchange(load->planWithoutMarks, nullptr))
            markModulesOfPlan(*plan);
        else if (!std::exchange(load->hasMarksBelowRoot, true))
            markModulesBelowWalkedRoot(load->root);
    }
    bool loaderGoesThroughGraph = m_loadsInFlight.hasWalkedLoad();
    if (!loaderGoesThroughGraph && !m_loadsInFlight.hasWaitingLoad())
        return false;
    auto isWaitingRoot = [&](ModuleID module) {
        return (module == required || data(module).isImportedByModule()) && m_loadsInFlight.hasWaitingRoot(module) && status(module) != ModuleStatus::Evaluating;
    };

    const uint32_t isSeen = takeMarkOfSearch();
    PlacesSeenByPlans& placesSeen = m_placesSeenByPlans;
    placesSeen[required.index()] = isSeen;
    Vector<ModuleID, 16> modules { required };
    while (!modules.isEmpty()) {
        ModuleID module = modules.takeLast();
        if (isWaitingRoot(module))
            return true;
        bool isNotLoaded = this->isNotLoaded(module);
        if (loaderGoesThroughGraph && m_modulesBelowWalkedRoots.get(module.index()) && (isNotLoaded || hasNotBegunAsFile(module)))
            return true;
        if (!isNotLoaded)
            continue;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (request.isHoisted || request.isRequire)
                continue;
            ModuleID target = request.targetModule();
            if (std::exchange(placesSeen[target.index()], isSeen) == isSeen)
                continue;
            if (isLoadedModuleWithoutRecord(target) && !isWaitingRoot(target))
                continue;
            modules.append(target);
        }
    }
    return false;
}

bool Modules::hasNotBegunAsFile(ModuleID module) const
{
    return hasNotBegun(module) && isCommonJSModuleOrFacade(module);
}

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

void Modules::didStartLoad(JSPromise* load, ModuleID root, SettlingOfLoad settling)
{
    if (load->status() != JSPromise::Status::Pending)
        return;
    m_loadsInFlight.removeSettled();
    m_loadsInFlight.append(*m_instance.vm, load, root, settling).loaderStepsAtCall = m_resumingCallLoaderSteps.value_or(m_loaderStepCount);
}

bool Modules::needsLoaderForRequire(ModuleID module)
{
    if (!isLoadedWithRequests(module) || data(module).isFacade())
        return false;
    if (data(module).isLoadedByLoader() || statementThatEndsLoad(module) || m_linkFailures.contains(module) || (m_mocks && failsWhateverFactoriesDo(module)))
        return true;
    return numberOfGraphsBeingLoaded() && comesToLoadInFlight(module);
}

bool Modules::isNotLoaded(ModuleID module)
{
    if (!isLoadedWithRequests(module))
        return false;
    if (ModuleRecord* record = recordIfExists(module))
        return record->status() == CyclicModuleRecord::Status::New || record->status() == CyclicModuleRecord::Status::Unlinked;
    return storedStatus(module) == StoredModuleStatus::Unlinked && isNotLoadedWithoutStatus(module, data(module)) && !wasFetchedByFailedLoad(module) && !wasLoadedForWalk(module);
}

void Modules::forgetThatModuleWasLoaded(ModuleID module)
{
    m_modulesLoadedBesideGroups.clear(module.index());
    m_modulesLinkedAheadOfImporters.clear(module.index());
    forgetKeptImports(module);
    m_importsLeftToCopies.remove(module.index());
    if (LoadedModule* loaded = loadedIfExists(module))
        loaded->whatKeptImportsStandFor.set(*this, nullptr);
    m_loadsThatKeepImports.removeAllMatching([&](const LoadThatKeepsImports& load) { return load.root == module; });
    for (LoadThatKeepsImports& load : m_loadsThatKeepImports)
        load.unlinkedImporters.clear(module.index());
    for (WalkOfImports* walk = m_innermostWalkOfImports; walk; walk = walk->outer) {
        walk->reached.clear(module.index());
        walk->modulesBelowRoot = std::nullopt;
    }
    m_modulesLoadedForWalksThatFailed.clear(module.index());
    m_heldFilesReachedByWalks.clear(module.index());
    m_filesThatTookTheirTurn.clear(module.index());
}

void Modules::noteModulesLoadedWith(ModuleID root)
{
    if (!data(root).isESModuleOfProgram() || (data(root).isFacade() && !m_loadsModulesIndividually) || wasLoadedWithGraph(root))
        return;
    if (m_rootWhoseLoadIsSwept && (!m_originalBeingEvaluated || importsStatically(root, *m_rootWhoseLoadIsSwept)))
        return;
    if (m_loadsModulesIndividually) {
        noteLoadOfEachModuleWith(root);
        return;
    }
    struct Graph {
        const ModuleData& data(uint32_t module) const { return modules.data(ModuleID(module)); }
        std::span<const ModuleRequestData> requests(uint32_t module) const { return modules.m_table.requests(ModuleID(module)); }
        ModuleNumbers groupsLoadedWith(uint32_t head) const { return modules.m_table.groupsLoadedWith(ModuleID(head)); }
        uint32_t moduleLoadedBy(const ModuleRequestData& request) const { return modules.moduleLoadedBy(request).index(); }
        const Modules& modules;
    };
    noteGroupsLoadedWith(Graph { *this }, root.index(), m_loadedGroups, m_modulesLoadedBesideGroups);
}

void Modules::startLoadingModulesIndividually()
{
    if (m_loadsModulesIndividually)
        return;
    uint32_t moment = ++m_instance.vm->aotState().momentOfLoads;
    m_momentsOfLoads.fill(0, m_table.size());
    m_loadedInPreload.ensureSize(m_table.size());
    m_modulesLoadedBesideGroups.ensureSize(m_table.size());
    for (uint32_t module = 0; module < m_table.size(); ++module) {
        if (!wasLoadedWithGraph(ModuleID(module)))
            continue;
        m_modulesLoadedBesideGroups.quickSet(module);
        m_momentsOfLoads[module] = moment;
        m_loadedInPreload.quickSet(module, !m_hasBegunTestFile);
    }
    m_loadedGroups.fill(0);
    for (uint32_t module = 0; module < m_table.size(); ++module) {
        if (!m_storedStatuses[module] && storedStatus(ModuleID(module)) == StoredModuleStatus::Linked)
            setStoredStatus(ModuleID(module), StoredModuleStatus::Linked);
    }
    ++m_linkEpoch;
    m_loadsModulesIndividually = true;
}

void Modules::noteLoadOfEachModuleWith(ModuleID root)
{
    uint32_t moment = ++m_instance.vm->aotState().momentOfLoads;
    bool isAheadOfTestFiles = !m_hasBegunTestFile;
    m_modulesLoadedBesideGroups.ensureSize(m_table.size());
    m_modulesLoadedBesideGroups.quickSet(root.index());
    Vector<uint32_t, 32> found { root.index() };
    while (!found.isEmpty()) {
        uint32_t module = found.takeLast();
        const LoadedModule* held = isMockLoadedInPreload(ModuleID(module)) ? loadedIfExists(fileOf(ModuleID(module))) : nullptr;
        bool isPreloadEntry = held && held->preloadMock && held->activeMock.get() == held->preloadMock.get();
        didGiveUpMomentLinked(std::exchange(m_momentsOfLoads[module], isPreloadEntry ? 1 : moment));
        m_loadedInPreload.quickSet(module, isAheadOfTestFiles || isPreloadEntry);
        bool isMockAlone = standsForWholeModule(ModuleID(module));
        m_modulesLoadedAsMocksAlone.set(module, isMockAlone);
        if (isMockAlone)
            continue;
        for (const ModuleRequestData& request : m_table.requests(ModuleID(module))) {
            if (request.isRequire || request.isHoisted)
                continue;
            ModuleID imported = request.targetModule();
            if (data(imported).isUncompiledCode()) [[unlikely]]
                imported = moduleLoadedBy(request);
            if (isAheadOfTestFiles && data(imported).isOfLibrary())
                m_libraryModulesAskedForInPreload.set(imported.index());
            recordVariantRequest(imported);
            if (!data(imported).isESModuleOfProgram())
                continue;
            recordEntryRequest(imported, ModuleID(module), LoadKind::Import);
            if (activeMock(imported))
                noteImportUnderMockInPlace(imported);
            if (!m_modulesLoadedBesideGroups.quickSet(imported.index()))
                found.append(imported.index());
        }
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Modules::isLeftAsLoaded(ModuleID module) const
{
    ModuleRecord* record = isCommonJSModuleOrFacade(module) ? nullptr : recordIfExists(module);
    return record && record->status() == CyclicModuleRecord::Status::Unlinked && !data(module).capturesExports();
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Modules::isModuleWithoutRecord(const ModuleData& moduleData, StoredModuleStatus stored) const
{
    return moduleData.kind == ModuleKind::SourceText && stored != StoredModuleStatus::InRecord && !moduleData.isFacade();
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Modules::isFoundWithoutRecord(ModuleID module, const ModuleData& moduleData, StoredModuleStatus stored)
{
    return stored == StoredModuleStatus::Unlinked && !moduleData.isOfLibrary() && isNotLoadedWithoutStatus(module, moduleData) && !wasLoadedForWalk(module);
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Modules::isNeitherOfProgramNorData(const ModuleData& moduleData, StoredModuleStatus stored) const
{
    return stored != StoredModuleStatus::InRecord && !moduleData.hasExportTable() && moduleData.kind != ModuleKind::CommonJs;
}

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

uint32_t Modules::countStepsOfWholeGraph(const PlanOfLoad& plan, std::span<uint32_t> firstRequestOfPlace, std::span<uint32_t> allRequestedPlaces)
{
    struct Place {
        uint32_t lowestLevelBelow;
        uint32_t laterRequests;
        uint32_t levelGoneThrough;
        uint32_t firstCount;
        uint32_t levelComeToAgain;
        uint32_t mostLaterRequestsComeWith;
        uint32_t levelGoneThroughAgain;
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

    Vector<uint32_t> storageOfImportersOfPlaces;
    storageOfImportersOfPlaces.fill(0, numberOfPlaces + 1);
    std::span<uint32_t> importersOfPlace = storageOfImportersOfPlaces.mutableSpan();
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

    Vector<uint32_t> storageOfCounts(numberOfPlaces);
    std::span<uint32_t> counts = storageOfCounts.mutableSpan();
    counts[0] = 0;
    uint32_t numberAskedFor = 1;
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

uint32_t Modules::takeMarkOfSearch()
{
    return m_placesSeenByPlans.takeMark(m_table.size());
}

bool Modules::tryFindAliasedCompiledModules(ModuleID root, UncheckedKeyHashMap<ModuleID, ModuleID>& aliasedModules)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!m_evictedFiles.isEmpty() || !m_modulesWithPendingMock.isEmpty() || (host().hasResolvePlugins && host().hasResolvePlugins(m_instance.globalObject)))
        return false;
    BitVector visited(m_table.size());
    visited.quickSet(root.index());
    Vector<ModuleID, 16> toFollow { root };
    while (!toFollow.isEmpty()) {
        ModuleID importer = toFollow.takeLast();
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            ModuleID target = request.targetModule();
            if (request.isRequire || request.isHoisted || !data(target).reachesUncompiledCode())
                continue;
            if (data(target).isUncompiledCode()) {
                if (aliasedModules.contains(target))
                    continue;
                if (fetchParametersOf(target))
                    return false;
                String path = resolvedKey(target).string();
                if (scope.exception()) [[unlikely]] {
                    TRY_CLEAR_EXCEPTION(scope, false);
                    return false;
                }
                std::optional<ModuleID> compiled = m_table.findByPath(path);
                if (!compiled || isReplacedByHost(*compiled, LoadKind::Import) || delegatesToHost(*compiled))
                    return false;
                ModuleID behind = moduleWithExportsOf(fileOf(*compiled));
                if (isMocked(behind) || isMocked(fileOf(behind)))
                    return false;
                aliasedModules.add(target, behind);
                target = behind;
                if (!data(target).reachesUncompiledCode())
                    continue;
            }
            if (status(target) == ModuleStatus::Unlinked && !isMocked(target) && !m_modulesWithPendingMock.get(target.index()) && !visited.quickSet(target.index()))
                toFollow.append(target);
        }
    }
    return true;
}

bool Modules::planLevelsOfLoad(ModuleID root)
{
    using Found = PlanOfLoad::Found;
    using Level = PlanOfLoad::Level;
    if (!m_queuedFetches.isEmpty())
        return false;
    UncheckedKeyHashMap<ModuleID, ModuleID> aliasedModules;
    if (data(root).reachesUncompiledCode() && !tryFindAliasedCompiledModules(root, aliasedModules))
        return false;

    auto isInHandsOfLoader = [&](ModuleID module) {
        if (m_levelsOfLoads.contains(module))
            return true;
        ModuleRecord* record = recordIfExists(module);
        if (!record || record->status() != CyclicModuleRecord::Status::New)
            return false;
        return record->isRequestedByWalkedModule() || m_loadsInFlight.hasRoot(module);
    };
    const uint32_t isNotFound = takeMarkOfSearch();
    const uint32_t firstPlaceFound = isNotFound + 1;
    PlacesSeenByPlans& placesSeen = m_placesSeenByPlans;
    Ref<PlanOfLoad> plan = adoptRef(*new PlanOfLoad);
    auto didEndSearch = makeScopeExit([&] {
        m_placesSeenByPlans.setNextMark(firstPlaceFound + plan->found.size());
    });
    auto& found = plan->found;
    auto& levels = plan->levels;
    Vector<uint32_t, 64> firstRequestOfPlace;
    Vector<uint32_t, 256> requestedPlaces;
    placesSeen[root.index()] = firstPlaceFound;
    found.append(Found { root, 0, 0, false, false, false, false, false, false });
    levels.append(Level { 0, 0, 0, false });
    while (true) {
        Level next { static_cast<uint32_t>(found.size()), 0, 0, false };
        uint32_t firstPlaceOfNext = firstPlaceFound + next.first;
        for (uint32_t importer = levels.last().first; importer < next.first; ++importer) {
            firstRequestOfPlace.append(requestedPlaces.size());
            if (found[importer].isFile || isMocked(found[importer].module))
                continue;
            uint32_t laterRequests = found[importer].laterRequests;
            for (const ModuleRequestData& request : m_table.requests(found[importer].module)) {
                if (request.isHoisted || request.isRequire)
                    continue;
                ModuleID target = request.targetModule();
                uint32_t* placeOfTarget = &placesSeen[target.index()];
                bool isAliased = *placeOfTarget < isNotFound && data(target).isUncompiledCode();
                if (isAliased) [[unlikely]] {
                    auto behind = aliasedModules.find(target);
                    if (behind == aliasedModules.end())
                        return false;
                    target = behind->value;
                    placeOfTarget = &placesSeen[target.index()];
                }
                uint32_t& placeSeen = *placeOfTarget;
                if (placeSeen >= isNotFound) {
                    if (placeSeen == isNotFound)
                        continue;
                    requestedPlaces.append(placeSeen - firstPlaceFound);
                    if (isAliased) {
                        found[placeSeen - firstPlaceFound].leadsToFile = true;
                        found[placeSeen - firstPlaceFound].isAliased = true;
                    }
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
                    if (isInHandsOfLoader(target))
                        return false;
                    isFile = isCommonJSModuleOrFacade(target) || targetData.kind == ModuleKind::Data;
                    isFound = isFile || isFileOfProgram(target);
                }
                if (!isFound) {
                    placeSeen = isNotFound;
                    continue;
                }
                placeSeen = firstPlaceFound + found.size();
                requestedPlaces.append(found.size());
                bool isCommonJSFile = isFile && isCommonJSModuleOrFacade(target);
                found.append(Found { target, importer, laterRequests, isFile, isAliased, isAliased, isAliased && (!isFile || isCommonJSFile), false, false });
                next.hasFiles |= isFile;
                if (laterRequests > next.laterRequests) {
                    next.laterRequests = laterRequests;
                    next.placeOfMostLaterRequests = found.size() - 1;
                }
            }
        }
        if (next.first == found.size())
            break;
        levels.append(next);
    }
    auto isFoundWithoutRecordNow = [&](ModuleID module) {
        return isModuleWithoutRecord(data(module), storedStatus(module)) && isFoundWithoutRecord(module, data(module), storedStatus(module));
    };
    if (m_levelsOfLoads.size() <= found.size()) {
        for (ModuleID onWay : m_levelsOfLoads.keys()) {
            if (placesSeen[onWay.index()] >= isNotFound && isFoundWithoutRecordNow(onWay))
                return false;
        }
    } else {
        for (const Found& one : found) {
            if (m_levelsOfLoads.contains(one.module) && isFoundWithoutRecordNow(one.module))
                return false;
        }
    }

    if (plan->asksAcrossLevels) {
        firstRequestOfPlace.append(requestedPlaces.size());
        plan->stepsOfWholeGraph = countStepsOfWholeGraph(plan.get(), firstRequestOfPlace.mutableSpan(), requestedPlaces.mutableSpan());
    }
    for (size_t place = found.size(); --place;)
        found[found[place].importer].leadsToFile |= found[place].leadsToFile;
    layWayOfLoad(plan.get(), nullptr);
    bool marksWait = false;
    m_loadsInFlight.forEachOfRoot(root, [&](LoadInFlight& load) {
        load.hasMarksBelowRoot = true;
        load.planWithoutMarks = plan.ptr();
        m_loadsInFlight.willMark(load);
        marksWait = true;
    });
    if (!marksWait)
        markModulesOfPlan(plan.get());
    return true;
}

void Modules::layWayOfLoad(PlanOfLoad& plan, LevelOfLoad* above)
{
    using Found = PlanOfLoad::Found;
    auto& found = plan.found;
    auto& levels = plan.levels;
    size_t highestLevel = above ? above->depth + 1 : 0;

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
    for (size_t depth = levels.size(); depth-- > highestLevel; onWay = &found[onWay->importer]) {
        std::unique_ptr<LevelOfLoad>& planned = m_levelsOfLoads.ensure(onWay->module, [] {
            return makeUnique<LevelOfLoad>();
        }).iterator->value;
        planned->next = below;
        planned->plan = &plan;
        planned->depth = depth;
        planned->place = onWay - found.span().data();
        if (levels[depth].hasFiles) {
            for (Found& file : plan.level(depth)) {
                if (file.isFile && !file.leadsToFile && &file != onWay && isLeftToLoad(plan, file))
                    planned->filesWithoutCode.append(file.module);
            }
        }
        below = onWay->module;
    }
    if (above)
        above->next = below;
}

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
        root->setExtraLoadSteps(plan.steps - stepsToLevel);
}

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

void Modules::takeUpWaysOfLoads()
{
    m_levelsOfLoads.removeIf([&](auto& entry) {
        return !entry.value->isAskedFor;
    });
}

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
    ensureLoaded(module).momentCapturedAheadOfTurn = m_instance.vm->importOrderState().clock;
    captureExports(module);
}

void Modules::didLoadModuleOfLevel(ModuleID moduleOnWay)
{
    generateNativeModuleForLoader(moduleOnWay);
    auto found = m_levelsOfLoads.find(moduleOnWay);
    if (found == m_levelsOfLoads.end()) [[likely]]
        return;
    for (ModuleID file : std::exchange(found->value->filesWithoutCode, { }))
        m_filesLoadedWithoutLoader.set(file.index());
    if (!isLoadedWithRequests(moduleOnWay) || isCommonJSModuleOrFacade(moduleOnWay)) {
        m_filesLoadedWithoutLoader.set(moduleOnWay.index());
        m_levelsOfLoads.remove(moduleOnWay);
    }
}

bool Modules::wasLoadedWithEarlierGraph(ModuleID module)
{
    if (!isCommonJSModuleOrFacade(module))
        return isLoadedWithRequests(module) && !isNotLoaded(module);
    if (isEvictedForImports(module))
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
    ModuleRecord* record = ensureRecord(module);
    record->retain();
    if (record->status() == CyclicModuleRecord::Status::New)
        record->setStatus(CyclicModuleRecord::Status::Unlinked);
    return record;
}

void Modules::settleImportsOfModulesThatHaveRun(ModuleID rootOfEvaluation)
{
    if (m_modulesAskedForWhileTheyRan.isEmpty()) [[likely]]
        return;
    VM& vm = *m_instance.vm;
    m_modulesAskedForWhileTheyRan.removeAllMatching([&](ModuleID module) {
        if (status(module) == ModuleStatus::Evaluating)
            return false;
        if (module == rootOfEvaluation)
            return true;
        ModuleRecord* record = m_records.get(module);
        JSPromise* capability = record ? record->topLevelCapability() : nullptr;
        if (!capability || capability->status() != JSPromise::Status::Pending)
            return true;
        record->adoptStatusIfNeeded();
        if (record->status() != CyclicModuleRecord::Status::Evaluated)
            return true;
        if (JSValue error = record->evaluationError())
            capability->reject(vm, error);
        else
            capability->fulfill(vm, jsUndefined());
        return true;
    });
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
    ModuleRecord* record = m_records.get(root);
    JSPromise* capability = record ? record->topLevelCapability() : nullptr;
    if (capability && capability->status() != JSPromise::Status::Pending)
        capability = nullptr;
    if (!exception) {
        if (capability) [[unlikely]] {
            record->adoptStatusIfNeeded();
            capability->fulfill(vm, jsUndefined());
        }
        return;
    }
    for (AbstractModuleRecord* member : stack) {
        auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(member);
        cyclic->setStatus(CyclicModuleRecord::Status::Evaluated);
        cyclic->setEvaluationError(vm, exception->value());
        if (auto* compiled = dynamicDowncast<ModuleRecord>(member)) {
            compiled->didChangeStatus();
            compiled->retain();
        }
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

    runImportedFiles(root, JSModuleLoader::EvaluationOfLoad { referrerAsyncOrder.value_or(-1), importPromise });
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (status(root) == ModuleStatus::Failed)
        throwEvaluationError(root);
    else if (linkWalk)
        link(*linkWalk);
    else if (!isLinked)
        link(root);
    if (!scope.exception() && moduleData.kind == ModuleKind::CommonJs)
        runFileInItsTurn(root);
    if (!scope.exception() && m_waitingImportersToBeLinked.contains(root)) [[unlikely]] {
        linkAheadOfImporters(moduleAheadOfLink(root));
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        didLinkImportersThatWaitBelow(root);
        RELEASE_AND_RETURN(scope, loader()->promiseForTurnToBeEvaluated(realm, ensureRecord(root)));
    }
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    JSPromise* evaluation = evaluateLinkedModule(root, referrerAsyncOrder, importPromise);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, evaluation ? evaluation : JSPromise::resolvedPromise(realm, jsUndefined()));
}

// Returns null if it has run to its end.
JSPromise* Modules::evaluateLinkedModule(ModuleID root, std::optional<int64_t> referrerAsyncOrder, JSPromise* importPromise)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& moduleData = data(root);

    if (moduleData.hasExportTable() && (recordIfExists(root) || (moduleData.needsEvaluation() && !(moduleData.isSimple() && moduleData.isSynchronous()) && !moduleData.evaluatesItself()))) {
        ModuleRecord* record = ensureRecord(root);
        if (record->status() == CyclicModuleRecord::Status::New || record->status() == CyclicModuleRecord::Status::Unlinked)
            record->setStatus(CyclicModuleRecord::Status::Linked);
        RELEASE_AND_RETURN(scope, record->evaluate(realm, referrerAsyncOrder.value_or(-1), importPromise));
    }

    if (moduleData.hasExportTable() && !moduleData.needsEvaluation()) {
        noteModulesLoadedWith(root);
        if (!isReplacedByMock(root) || mocksInGraph(root).loadsRootOriginal()) {
            linkInstances(root);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        }
        setStoredStatus(root, StoredModuleStatus::Evaluated);
        if (runsBesideMockFor(root, std::nullopt)) [[unlikely]] {
            noteSecondInstanceOfOriginal(root);
            didEvaluateOriginalUnderMock(root);
            m_originalsDueUnderMock.clear(root.index());
            m_importsMadeByOwnFactory.removeAll(root);
            m_modulesThatOwnFactoryMayLoad.clear(root.index());
        }
    } else if (moduleData.hasExportTable() && status(root) != ModuleStatus::Evaluated) {
        evaluateWithOwnStack(root);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    return nullptr;
}

bool Modules::isMainCommonJSModuleImport(ModuleID module)
{
    return isCommonJSModuleOrFacade(module) && host().isMainModuleImport && host().isMainModuleImport(m_instance.globalObject, module);
}

JSPromise* Modules::evaluateMainModule(ModuleID entryPoint, JSPromise* evaluation)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!host().isMainModuleImport || mainModuleWaitsForItsImport() || !host().isMainModuleImport(realm, entryPoint))
        return evaluation;

    switch (evaluation->status()) {
    case JSPromise::Status::Pending: {
        JSPromise* capability = JSPromise::create(vm, realm->promiseStructure());
        evaluation->markAsHandled();
        m_importOfMainModule = entryPoint;
        m_evaluationOfImportOfMainModule.set(vm, evaluation);
        m_capabilityOfMainModule.set(vm, capability);
        return capability;
    }
    case JSPromise::Status::Fulfilled:
        host().didEvaluateMainModule(realm, entryPoint);
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
    host().didEvaluateMainModule(m_instance.globalObject, m_importOfMainModule);
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

void Modules::recordEvaluationIfNothingIsLeftToRun(ModuleID module)
{
    ASSERT(status(module) == ModuleStatus::Linked);
    if (!isPassedOverByEvaluation(data(module)))
        return;
    BitVector seen(m_table.size());
    seen.quickSet(module.index());
    Vector<ModuleID, 16> passedOver { module };
    while (!passedOver.isEmpty()) {
        for (uint32_t number : m_table.evaluationRequests(passedOver.takeLast())) {
            ModuleID required(number);
            if (seen.quickSet(number) || status(required) == ModuleStatus::Evaluated)
                continue;
            if (!isPassedOverByEvaluation(data(required)))
                return;
            passedOver.append(required);
        }
    }
    setStatus(module, ModuleStatus::Evaluated);
}

bool Modules::isLeftToStartsOfImporters(ModuleID module) const
{
    return status(module) == ModuleStatus::Linked && isPassedOverByEvaluation(data(module));
}

void Modules::noteThatGraphBegins(ModuleID root)
{
    ++m_numberOfGraphsBegun;
    if (!m_table.listsNamesOfStatements())
        return;
    Vector<ModuleID, 32> toFollow;
    auto begins = [&](ModuleID file) {
        if (isMocked(file) || activeMock(file) || m_modulesWithPendingMock.get(file.index()))
            return;
        if (!m_filesWhoseGraphHasBegun.set(file.index()))
            toFollow.append(file);
    };
    begins(fileOf(root));
    while (!toFollow.isEmpty()) {
        ModuleID importer = toFollow.takeLast();
        if (!isLoadedWithRequests(importer))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer))
            begins(fileOf(request.targetModule()));
    }
}

void Modules::leaveStepsOfLoaderToJobs()
{
    auto& steps = m_instance.vm->m_synchronousModuleQueue->tasks;
    for (const VM::SynchronousModuleTask& step : steps)
        m_instance.globalObject->queueMicrotask(*m_instance.vm, step.task, step.payload, step.arg0, step.arg1, step.arg2, step.arg3);
    steps.shrink(0);
}

static JSC_DECLARE_HOST_FUNCTION(evictedLoadJob);
static JSC_DECLARE_HOST_FUNCTION(detachedFactorySettledJob);
static JSC_DECLARE_HOST_FUNCTION(detachedFactoryFailedJob);
static JSC_DECLARE_HOST_FUNCTION(loaderWaitFinishedJob);

JSC_DEFINE_HOST_FUNCTION(loaderWaitFinishedJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didFinishLoaderWait(callFrame->argument(1).asUInt32());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(evictedLoadJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->finishEvictedLoad(callFrame->argument(1).asUInt32());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(detachedFactorySettledJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didSettleDetachedFactory(callFrame->argument(1).asUInt32());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(detachedFactoryFailedJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didFailDetachedFactory(callFrame->argument(1).asUInt32(), callFrame->argument(0));
    return JSValue::encode(jsUndefined());
}

void Modules::didSettleDetachedFactory(unsigned number)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto find = [&] { return m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; }); };
    size_t left = find();
    if (left == notFound)
        return;
    if (m_detachedLoads[left].wasEvicted) [[unlikely]]
        return;
    --m_detachedLoads[left].pendingFactoryCount;
    ModuleID required = m_detachedLoads[left].required;
    bool isForgotten = !m_detachedLoads[left].keepsError;
    didResumeAfterFactories();
    if (m_detachedLoads[left].isForOriginal) {
        if (m_detachedLoads[left].pendingFactoryCount)
            return;
        if (m_detachedLoads[left].hasBegunToEvaluate)
            RELEASE_AND_RETURN(scope, evaluateDetachedOriginal(left));
        m_detachedLoads[left].wait->fulfill(vm, jsUndefined());
        return;
    }
    BitVector modulesToRetry = std::exchange(m_detachedLoads[left].modulesToRetry, BitVector());
    for (size_t file : modulesToRetry)
        clearLoadErrorsForMock(ModuleID(file));
    bool factoryHasNotSettled;
    {
        SetForScope resumingDetachedLoad(m_resumingDetachedLoad, std::optional(required));
        SetForScope modulesBeingRetried(m_modulesBeingRetried, &modulesToRetry);
        factoryHasNotSettled = runMocksInGraph(required);
    }
    Exception* thrown = scope.exception();
    if (thrown && (vm.isTerminationException(thrown) || !scope.tryClearException()))
        return;
    if (isForgotten)
        clearMockLoadErrors();
    left = find();
    if (left == notFound)
        return;
    if (thrown) {
        restoreRequiredValue(required);
        m_detachedLoads[left].wait->reject(vm, thrown->value());
        return;
    }
    auto laterCall = [&](const std::pair<ModuleID, uint64_t>& call) {
        uint64_t later = pendingFactoryCallID(call.first);
        return later == call.second ? 0 : later;
    };
    bool asksLaterMock = factoryHasNotSettled && m_detachedLoads[left].awaitedFactoryCalls.containsIf([&](const auto& call) { return !!laterCall(call); });
    if (factoryHasNotSettled && (asksLaterMock || !modulesToRetry.isEmpty())) {
        unsigned heard = subscribeToPendingFactories(required, number);
        RETURN_IF_EXCEPTION(scope, void());
        left = find();
        if (left == notFound)
            return;
        m_detachedLoads[left].pendingFactoryCount += heard;
        if (heard) {
            for (auto& call : m_detachedLoads[left].awaitedFactoryCalls) {
                if (uint64_t later = laterCall(call))
                    call.second = later;
            }
        }
        for (size_t file : modulesToRetry) {
            m_detachedLoads[left].awaitedFactoryCalls.removeAllMatching([&](const auto& call) { return call.first.index() == file; });
            if (uint64_t call = pendingFactoryCallID(ModuleID(file)))
                m_detachedLoads[left].awaitedFactoryCalls.append({ ModuleID(file), call });
        }
    }
    startLoaderWaits(number);
    RETURN_IF_EXCEPTION(scope, void());
    left = find();
    if (left == notFound || m_detachedLoads[left].pendingFactoryCount || m_detachedLoads[left].pendingLoaderWaits)
        return;
    RELEASE_AND_RETURN(scope, finishDetachedLoad(number));
}

void Modules::evaluateDetachedOriginal(size_t index)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Strong<JSPromise> wait = m_detachedLoads[index].wait;
    ModuleID required = m_detachedLoads[index].required;
    m_detachedLoads[index].isForOriginal = false;
    if (m_detachedLoads[index].isRefusedOriginal)
        requireOriginalUnderMock(required);
    else
        evaluateOriginalUnderMock(required);
    if (Exception* failure = scope.exception()) {
        JSValue error = failure->value();
        if (vm.isTerminationException(failure) || !scope.tryClearException())
            return;
        if (!errorOfModuleThatRanBesideMock(required))
            ensureLoaded(required).errorOfModuleThatRanBesideMock.set(*this, error);
        clearMockLoadError(required);
        wait->reject(vm, error);
        return;
    }
    wait->fulfill(vm, jsUndefined());
}

void Modules::finishDetachedLoad(unsigned number)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto find = [&] { return m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; }); };
    size_t left = find();
    if (left == notFound)
        return;
    ModuleID required = m_detachedLoads[left].required;
    if (std::exchange(m_detachedLoads[left].hasBegunToEvaluate, false) && (status(required) == ModuleStatus::Unlinked || status(required) == ModuleStatus::Linked) && !isEvicted(required) && !isMocked(required)) {
        JSPromise* evaluation = evaluate(required, std::nullopt, nullptr);
        TRY_CLEAR_EXCEPTION(scope, void());
        if (evaluation)
            evaluation->markAsHandled();
        left = find();
        if (left == notFound)
            return;
    }
    bool wasMockReplaced = m_detachedLoads[left].wasEndedByLaterMock && m_detachedLoads[left].awaitedFactoryCalls.containsIf([&](const auto& call) { return isMockLingering(call.first); });
    restoreRequiredValue(required);
    m_detachedLoads[left].wait->fulfill(*m_instance.vm, jsUndefined());
    if (!m_modulesToEvictOnceLoaded.isEmpty()) {
        BitVector leftBehind = staticImportClosure(required, MockTraversal::Stop);
        leftBehind.exclude(modulesRequestedByImports(false));
        for (ModuleID root : m_factoryBlockedImportRoots)
            leftBehind.exclude(staticImportClosure(root, MockTraversal::Stop));
        for (const ImportAwaitingSource& call : m_importsAwaitingSource)
            leftBehind.exclude(staticImportClosure(call.module, MockTraversal::Stop));
        Vector<ModuleID, 2> neverRan;
        m_modulesToEvictOnceLoaded.removeAllMatching([&](ModuleID module) {
            if (!leftBehind.get(module.index()) || isMocked(module) || status(module) > ModuleStatus::Linked)
                return false;
            neverRan.append(module);
            return true;
        });
        for (ModuleID module : neverRan)
            tryReleaseModule(module);
    }
    RELEASE_AND_RETURN(scope, evictLoadedModules(wasMockReplaced));
}

void Modules::restoreRequiredValue(ModuleID required)
{
    LoadedModule* loaded = loadedIfExists(required);
    if (!loaded || !loaded->requiredValueBeforeRefusal)
        return;
    if (!loaded->requiredValue)
        loaded->requiredValue.set(*this, loaded->requiredValueBeforeRefusal.get());
    loaded->requiredValueBeforeRefusal.set(*this, JSValue());
}

void Modules::didFinishLoaderWait(unsigned number)
{
    size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; });
    if (left == notFound)
        return;
    DetachedLoad& load = m_detachedLoads[left];
    if (--load.pendingLoaderWaits || load.pendingFactoryCount)
        return;
    finishDetachedLoad(number);
}

void Modules::finishLoadsAwaitingLoader(std::optional<ModuleID> changing)
{
    if (m_detachedLoads.isEmpty()) [[likely]]
        return;
    Vector<unsigned, 2> due;
    for (const DetachedLoad& load : m_detachedLoads) {
        if (!load.pendingFactoryCount && load.pendingLoaderWaits && load.wait->status() == JSPromise::Status::Pending && (!changing || load.required == *changing || load.timedMocks.get(changing->index())))
            due.append(load.number);
    }
    for (unsigned number : due)
        finishDetachedLoad(number);
}

void Modules::startLoaderWaits(unsigned number)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; });
    if (left == notFound)
        return;
    Vector<unsigned, 2> waits;
    for (const auto& call : m_detachedLoads[left].awaitedFactoryCalls) {
        if (!activeMock(call.first) || m_detachedLoads[left].timedMocks.get(call.first.index()))
            continue;
        m_detachedLoads[left].timedMocks.set(call.first.index());
        waits.append(4 + loaderStepsToMock(m_detachedLoads[left].required, call.first));
    }
    m_detachedLoads[left].pendingLoaderWaits += waits.size();
    for (unsigned jobs : waits) {
        JSPromise* turn = JSPromise::resolvedPromise(realm, jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
        for (unsigned job = 1; job < jobs; ++job) {
            JSPromise* next = JSPromise::create(vm, realm->promiseStructure());
            turn->performPromiseThen(vm, realm, jsUndefined(), jsUndefined(), next);
            turn = next;
        }
        JSFunction* didFinish = JSFunction::create(vm, realm, 2, String(), loaderWaitFinishedJob, ImplementationVisibility::Private);
        turn->performPromiseThenWithContext(vm, realm, didFinish, didFinish, jsUndefined(), jsNumber(number));
    }
}

unsigned Modules::loaderStepsToMock(ModuleID root, ModuleID mock) const
{
    struct Request {
        uint32_t firstRequester { 0 };
        unsigned level { 0 };
    };
    UncheckedKeyHashMap<uint32_t, Request, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> requests;
    Vector<ModuleID, 32> inLevelOrder { root };
    requests.set(root.index(), Request { root.index(), 0 });
    for (size_t next = 0; next < inLevelOrder.size(); ++next) {
        ModuleID module = inLevelOrder[next];
        if (module != root && isMocked(module))
            continue;
        unsigned level = requests.get(module.index()).level;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            ModuleID requested = moduleLoadedBy(request);
            if (requests.add(requested.index(), Request { module.index(), level + 1 }).isNewEntry)
                inLevelOrder.append(requested);
        }
    }
    UncheckedKeyHashMap<uint32_t, std::optional<unsigned>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> steps;
    BitVector isOpen(m_table.size());
    auto visit = [&](auto& self, ModuleID module) -> std::optional<unsigned> {
        if (module == mock)
            return 0;
        if (auto known = steps.find(module.index()); known != steps.end())
            return known->value;
        if ((module != root && isMocked(module)) || isOpen.quickSet(module.index()))
            return std::nullopt;
        std::optional<unsigned> longest;
        unsigned level = requests.get(module.index()).level;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            ModuleID requested = moduleLoadedBy(request);
            std::optional<unsigned> below = self(self, requested);
            if (!below)
                continue;
            uint32_t first = requests.get(requested.index()).firstRequester;
            bool isJoiningPendingFetch = first != module.index() && (requested == mock || requests.get(first).level == level);
            longest = std::max(longest.value_or(0), 1 + *below + isJoiningPendingFetch);
        }
        isOpen.quickClear(module.index());
        steps.set(module.index(), longest);
        return longest;
    };
    return visit(visit, root).value_or(1);
}

void Modules::didFailDetachedFactory(unsigned number, JSValue error)
{
    size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; });
    if (left == notFound)
        return;
    didResumeAfterFactories();
    JSPromise* wait = m_detachedLoads[left].wait.get();
    if (m_detachedLoads[left].keepsError)
        cacheDetachedFactoryError(m_detachedLoads[left].required, error);
    restoreRequiredValue(m_detachedLoads[left].required);
    wait->reject(*m_instance.vm, error);
}

unsigned Modules::subscribeToPendingFactories(ModuleID required, unsigned number, bool isForOriginal)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue state;
    Vector<JSPromise*, 4> heard;
    while (true) {
        JSPromise* factory = isForOriginal && isMocked(required) ? pendingFactoryInOriginalGraph(required, PendingFactoryRequester::DetachedLoad, state) : pendingFactoryInGraph(required, PendingFactoryRequester::DetachedLoad, state);
        RETURN_IF_EXCEPTION(scope, 0);
        if (!factory || heard.contains(factory))
            break;
        heard.append(factory);
    }
    for (JSPromise* factory : heard) {
        JSFunction* didSettle = JSFunction::create(vm, realm, 2, String(), detachedFactorySettledJob, ImplementationVisibility::Private);
        JSFunction* didFail = JSFunction::create(vm, realm, 2, String(), detachedFactoryFailedJob, ImplementationVisibility::Private);
        factory->performPromiseThenWithContext(vm, realm, didSettle, didFail, jsUndefined(), jsNumber(number));
    }
    return heard.size();
}

void Modules::cacheDetachedFactoryError(ModuleID required, JSValue error)
{
    keepErrorOfLoad(required, error, [&](ModuleID each) { return isMocked(each) && m_modulesWithDeferredMock.get(each.index()); });
    if (!m_modulesWithMockLoadError.set(required.index()))
        ensureLoaded(required).cachedLoadError.set(*this, error);
}

bool Modules::hasUnheardSettlement(ModuleID required) const
{
    return m_detachedLoads.containsIf([&](const DetachedLoad& left) {
        return left.required == required && left.pendingFactoryCount && !left.wasAwaitedMockRemoved && left.wait->status() == JSPromise::Status::Pending && left.awaitedFactoryCalls.containsIf([&](const auto& call) { return pendingFactoryCallID(call.first) != call.second; });
    });
}

bool Modules::isStillBlockedOnFactories(ModuleID required) const
{
    bool isInFlight = m_detachedLoads.containsIf([&](const auto& left) { return left.required == required && left.wait->status() == JSPromise::Status::Pending; });
    bool waits = false;
    for (const RejectedRequire& pair : m_rejectedRequires) {
        if (pair.required != required || pair.mocked == required || m_modulesWithActiveMock.get(pair.mocked.index()))
            continue;
        uint64_t call = pendingFactoryCallID(pair.mocked);
        if (!call && hasUnheardSettlement(required)) {
            waits = true;
            continue;
        }
        if (!call || (!isInFlight && !m_rejectedRequires.containsIf([&](const RejectedRequire& other) { return other.required == required && other.mocked == pair.mocked && other.call == call; })))
            return false;
        waits = true;
    }
    return waits;
}

bool Modules::hasOnlyStaleFactoryCalls(const DetachedLoad& left)
{
    return !left.awaitedFactoryCalls.isEmpty() && std::ranges::none_of(left.awaitedFactoryCalls, [&](const auto& call) { return pendingFactoryCallID(call.first) == call.second; });
}

bool Modules::isAwaitedByDetachedLoad(ModuleID module) const
{
    return m_unsettledLoadModules.get(module.index()) || m_detachedLoads.containsIf([&](const DetachedLoad& load) {
        return load.wait->status() == JSPromise::Status::Pending && load.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == module; });
    });
}

void Modules::releaseAfterEvaluation(ModuleID awaited)
{
    for (const DetachedLoad& load : m_detachedLoads) {
        if (load.wait->status() != JSPromise::Status::Pending || !load.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == awaited; }))
            continue;
        for (size_t index : staticImportClosure(load.required, MockTraversal::Continue)) {
            if (hasInstances(ModuleID(index)) && staticImportClosure(ModuleID(index), MockTraversal::Continue).get(awaited.index()))
                m_modulesToReleaseAfterEvaluation.set(index);
        }
    }
}

void Modules::evictOnceLoaded(ModuleID mocked)
{
    BitVector requested(m_table.size());
    for (const DetachedLoad& load : m_detachedLoads) {
        if (load.wait->status() == JSPromise::Status::Pending && !load.isForOriginal && load.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == mocked; }))
            requested.merge(staticImportClosure(load.required, MockTraversal::Continue));
    }
    if (!requested.get(mocked.index()))
        return;
    listImportersAndRequirers();
    Vector<ModuleID, 8> work { mocked };
    requested.quickClear(mocked.index());
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (!m_modulesToEvictOnceLoaded.contains(module))
            m_modulesToEvictOnceLoaded.append(module);
        for (uint32_t importer : m_importersByStatement[module.index()]) {
            if (requested.quickClear(importer))
                work.append(ModuleID(importer));
        }
    }
}

BitVector Modules::modulesRequestedByImports(bool wasMockReplaced)
{
    BitVector requested(m_table.size());
    auto addRequests = [&](ModuleID root) {
        if (!wasMockReplaced || !m_modulesToEvictOnceLoaded.contains(root))
            requested.merge(staticImportClosure(root, MockTraversal::Stop));
    };
    m_batchedFetches.forEachCall([&](const BatchedFetch& call) {
        if (!wasMockReplaced)
            addRequests(call.module);
    });
    for (const QueuedFetch& fetch : m_queuedFetches) {
        if (!wasMockReplaced)
            addRequests(fetch.module);
    }
    for (LoadInFlight& load : m_loadsInFlight) {
        if (m_loadsInFlight.isBeingLoaded(load))
            addRequests(load.root);
    }
    return requested;
}

void Modules::evictLoadedModules(bool wasMockReplaced, IsAnswerToRequireOwed isAnswerToRequireOwed)
{
    if (m_isPassOverFailedModulesDue && !m_instance.vm->m_synchronousModuleQueue) [[unlikely]] {
        m_isPassOverFailedModulesDue = false;
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
        evictFailedModules(m_instance.globalObject, std::nullopt);
        if (scope.exception()) [[unlikely]]
            TRY_CLEAR_EXCEPTION(scope, void());
    }
    if (m_modulesToEvictOnceLoaded.isEmpty() || m_instance.vm->m_synchronousModuleQueue) [[likely]]
        return;
    BitVector requested = modulesRequestedByImports(wasMockReplaced);
    if (m_isResumingFactoryChainImport && m_currentImportRequestedModule)
        requested.set(m_currentImportRequestedModule.index());
    Vector<ModuleID, 2> loaded;
    m_modulesToEvictOnceLoaded.removeAllMatching([&](ModuleID module) {
        ModuleStatus current = status(module);
        bool isLoaded = isMocked(module) ? !!activeMock(module) : current == ModuleStatus::Evaluated || current == ModuleStatus::Failed;
        if (!isLoaded || requested.get(module.index()) || staysUntilLoadEnds(module))
            return false;
        if (isAnswerToRequireOwed == IsAnswerToRequireOwed::Yes && m_importsKeptAheadOfLink.contains(module.index()))
            return false;
        loaded.append(module);
        return true;
    });
    for (DetachedLoad& load : m_detachedLoads) {
        if (load.wait->status() != JSPromise::Status::Pending || load.isForOriginal || !loaded.contains(load.required) || std::exchange(load.wasEvicted, true))
            continue;
        JSFunction* job = JSFunction::create(*m_instance.vm, m_instance.globalObject, 2, String(), evictedLoadJob, ImplementationVisibility::Private);
        JSPromise* turn = JSPromise::create(*m_instance.vm, m_instance.globalObject->promiseStructure());
        turn->fulfill(*m_instance.vm, jsUndefined());
        turn->performPromiseThenWithContext(*m_instance.vm, m_instance.globalObject, job, job, jsUndefined(), jsNumber(load.number));
    }
    for (ModuleID module : loaded)
        releaseImportersOf(module, ReleasedImporters::All);
    for (ModuleID module : loaded) {
        if (!isMocked(module))
            m_modulesEvictedOnceLoaded.set(module.index());
        tryReleaseModule(module);
    }
}

void Modules::evictAtNextLoad(ModuleID module)
{
    startLoadingModulesIndividually();
    if (!m_modulesToEvictOnceLoaded.contains(module))
        m_modulesToEvictOnceLoaded.append(module);
}

void Modules::finishEvictedLoad(unsigned number)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; });
    if (left == notFound)
        return;
    ModuleID required = m_detachedLoads[left].required;
    bool requestsMock = m_detachedLoads[left].awaitedFactoryCalls.containsIf([&](const auto& call) { return m_modulesWithPendingMock.get(call.first.index()); });
    restoreRequiredValue(required);
    m_detachedLoads[left].wait->fulfill(*m_instance.vm, jsUndefined());
    if (!requestsMock || isEvicted(required) || isMocked(required))
        return;
    runMocksInGraph(required);
    if (scope.exception()) [[unlikely]]
        TRY_CLEAR_EXCEPTION(scope, void());
}

void Modules::didPatchPendingMock(ModuleID mocked)
{
    for (DetachedLoad& load : m_detachedLoads)
        load.wasAwaitedMockRemoved |= load.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == mocked; });
    evictOnceLoaded(mocked);
}

void Modules::evictAbandonedModules()
{
    for (ModuleID root : m_loadsThatKeepImports.map([](const LoadThatKeepsImports& load) { return load.root; })) {
        bool waitsForFactory = false;
        for (size_t index : staticImportClosure(root, MockTraversal::Stop))
            waitsForFactory |= isMockPending(fileOf(ModuleID(index)));
        if (waitsForFactory)
            didFailLoadThatKeptImports(root);
    }
    m_loadsThatKeepImports.clear();
    m_filesReadBeforeChangeOfPlugins.clearAll();
    m_namesAnsweredBeforeChangeOfPlugins.clear();
    m_importsLeftToCopies.clear();
    evictLoadedModules();
    m_modulesToEvictOnceLoaded.clear();
}

void Modules::noteLoadsOfEndedTestFile()
{
    m_modulesReleasedByLoader.merge(std::exchange(m_modulesToReleaseAtNextTestFile, { }));
    for (const DetachedLoad& load : m_detachedLoads) {
        for (auto& [awaited, call] : load.awaitedFactoryCalls) {
            if (load.wait->status() == JSPromise::Status::Pending && hasOrphanedFetch(awaited) && status(awaited) == ModuleStatus::Evaluated)
                m_unsettledLoadModules.set(awaited.index());
        }
    }
}

bool Modules::canReleaseLoads(ModuleID testFile)
{
    bool handsOnNames = data(testFile).hasDynamicStarExport();
    for (const ModuleExportData& entry : m_table.exports(testFile))
        handsOnNames |= entry.kind() == ModuleExportKind::Indirect;
    bool holdsLoads = false;
    for (const ModuleRequestData& request : m_table.imports(testFile))
        holdsLoads |= data(request.targetModule()).isUncompiledCode();
    return holdsLoads && !handsOnNames;
}

void Modules::releaseLoadsOfEndedTestFiles()
{
    if (m_resetWithReleasedLoads == m_momentOfModuleReset || m_testFilesHoldingLoads.isEmpty() || isSomeLoadUnderWay()) [[likely]]
        return;
    m_resetWithReleasedLoads = m_momentOfModuleReset;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    for (size_t index : BitVector(m_testFilesHoldingLoads)) {
        ModuleID testFile(index);
        if (!isEvicted(testFile) || m_momentOfModuleReset <= m_startMoments.getOptional(testFile).value_or(0) || isMocked(testFile))
            continue;
        if (data(testFile).kind == ModuleKind::CommonJs) {
            m_testFilesHoldingLoads.clear(index);
            if (m_filesToLoadAgain.get(index))
                discardModuleObject(testFile);
            continue;
        }
        ModuleStatus current = status(testFile);
        if (current != ModuleStatus::Evaluated && current != ModuleStatus::Failed)
            continue;
        m_testFilesHoldingLoads.clear(index);
        releaseLoads(testFile);
        ensureInstance(testFile);
        if (scope.exception()) [[unlikely]]
            (void)scope.tryClearException();
    }
}

void Modules::releaseLoads(ModuleID testFile)
{
    Environment* released = currentInstance(testFile);
    if (released) {
        releaseInstance(testFile);
        m_releasedTestFiles.clear(testFile.index());
    }
    if (ModuleRecord* record = m_records.get(testFile)) {
        if (released)
            record->bindModuleInstance(*m_instance.vm, released);
        loader()->removeEntriesForRecord(record);
        takeOutRecord(testFile);
    }
    setStoredStatus(testFile, StoredModuleStatus::Evaluated);
    for (const ModuleRequestData& request : m_table.imports(testFile)) {
        ModuleID external = request.targetModule();
        if (!data(external).isUncompiledCode())
            continue;
        if (LoadedModule* loaded = loadedIfExists(external))
            loaded->uncompiledRecord.set(*this, nullptr);
        discardNamespace(external);
    }
    if (LoadedModule* loaded = loadedIfExists(testFile))
        loaded->importMeta.set(*this, nullptr);
}

void Modules::releaseInstancesOfEndedTestFiles()
{
    if (m_releasedTestFiles.isEmpty()) [[likely]]
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    for (size_t index : std::exchange(m_releasedTestFiles, { })) {
        ModuleID testFile(index);
        if (!hasInstances(testFile) || status(testFile) == ModuleStatus::Evaluating || data(testFile).isImportedByModule())
            continue;
        if (currentInstance(testFile)) {
            if (!m_table.exports(testFile).empty() || data(testFile).hasDynamicStarExport())
                continue;
            releaseInstance(testFile);
        }
        ensureInstance(testFile);
        if (scope.exception()) [[unlikely]]
            (void)scope.tryClearException();
        m_releasedTestFiles.clear(index);
    }
}

void Modules::endStaleDetachedLoads(ModuleID mocked)
{
    if (m_detachedLoads.isEmpty()) [[likely]]
        return;
    uint64_t settlingCallID = pendingFactoryCallID(mocked);
    for (DetachedLoad& left : m_detachedLoads)
        left.wasEndedByLaterMock |= left.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == mocked && call.second != settlingCallID; });
}

void Modules::markFetchOrphaned(ModuleID file)
{
    m_modulesWithOrphanedFetch.set(file.index());
    for (DetachedLoad& left : m_detachedLoads) {
        if (left.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == file; }))
            left.orphanedFetchModule = file;
    }
}

void Modules::willRejectRequireAgain(ModuleID asked, ModuleID above)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    m_blockedRequiresWithError.clear(asked.index());
    m_detachedLoads.removeAllMatching([](const auto& left) { return left.wait->status() != JSPromise::Status::Pending; });
    bool joinsFailingLoad = hasUnheardSettlement(above);
    bool isHeardByWait = m_detachedLoads.containsIf([&](const auto& left) { return left.required == above && !hasOnlyStaleFactoryCalls(left); });
    if (isHeardByWait || m_detachedLoads.containsIf([&](const auto& left) { return left.required == above && !hasEnded(left); })) {
        JSValue state;
        JSPromise* original = pendingFactoryInGraph(asked, PendingFactoryRequester::RejectedRequire, state);
        RETURN_IF_EXCEPTION(scope, void());
        if (original)
            original->performPromiseThen(vm, realm, jsUndefined(), jsUndefined(), JSPromise::create(vm, realm->promiseStructure()));
    }
    if (!isHeardByWait && !joinsFailingLoad)
        RELEASE_AND_RETURN(scope, startDetachedLoad(above));
}

void Modules::startDetachedLoad(ModuleID required, bool isForOriginal)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    m_detachedLoads.removeAllMatching([](const auto& left) { return left.wait->status() != JSPromise::Status::Pending; });
    if (size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.required == required && !hasOnlyStaleFactoryCalls(each); }); left != notFound) {
        m_detachedLoads[left].wait->performPromiseThen(vm, realm, jsUndefined(), jsUndefined(), JSPromise::create(vm, realm->promiseStructure()));
        return;
    }
    unsigned number = ++m_lastDetachedLoadNumber;
    unsigned heard = subscribeToPendingFactories(required, number, isForOriginal);
    RETURN_IF_EXCEPTION(scope, void());
    if (!heard)
        return;
    JSPromise* wait = JSPromise::create(vm, realm->promiseStructure());
    wait->markAsHandled();
    BitVector unmockedModules(m_table.size());
    BitVector closure = isForOriginal ? importClosureOfOriginal(required) : staticImportClosure(required, MockTraversal::Stop);
    for (size_t below : closure) {
        if (!isMocked(ModuleID(below)) && (!isForOriginal || below != required.index()))
            unmockedModules.set(below);
    }
    Vector<std::pair<ModuleID, uint64_t>, 1> awaitedFactoryCalls;
    bool hasBegunToEvaluate = true;
    for (size_t below : closure) {
        if (isForOriginal && below == required.index())
            continue;
        if (uint64_t call = isMocked(ModuleID(below)) ? pendingFactoryCallID(ModuleID(below)) : 0)
            awaitedFactoryCalls.append({ ModuleID(below), call });
        bool shouldRejectEveryRequire = false;
        hasBegunToEvaluate &= m_awaitedGeneratedModules.get(below) || !host().pendingFactoryCallID(realm, ModuleID(below), shouldRejectEveryRequire);
    }
    m_detachedLoads.append({ required, Strong<JSPromise>(vm, wait), !isForOriginal, number, heard, WTF::move(unmockedModules), WTF::move(awaitedFactoryCalls) });
    DetachedLoad& load = m_detachedLoads.last();
    load.isForOriginal = isForOriginal;
    load.hasBegunToEvaluate = hasBegunToEvaluate;
}

void Modules::evaluateSynchronously(ModuleID module, SynchronousEvaluationReason reason, bool* didStopAtChangeOfAnswers)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool factoryHasNotSettled;
    {
        SetForScope evaluatesSynchronously(m_evaluatesSynchronously, true);
        bool& requireRunsMocks = m_instance.vm->aotState().requireRunsMocks;
        SetForScope requireRunsMocksBelow(requireRunsMocks, requireRunsMocks || reason == SynchronousEvaluationReason::Require);
        factoryHasNotSettled = runMocksInGraph(module);
    }
    RETURN_IF_EXCEPTION(scope, void());
    const ModuleData& moduleData = data(module);

    auto throwAsyncModuleError = [&] {
        if (reason == SynchronousEvaluationReason::Require) {
            clearDeferredRequireMark(module);
            clearRejectedFirstRequire(module);
        }
        throwException(realm, scope, host().createAsyncModuleRequiredError(realm, pathOfRefusedRequire(module)));
    };
    if (factoryHasNotSettled && reason == SynchronousEvaluationReason::Require) [[unlikely]] {
        recordRejectedRequire(module);
        throwAsyncModuleError();
        return;
    }

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
        if (moduleData.hasTopLevelAwait())
            throwAsyncModuleError();
        return;
    case ModuleStatus::EvaluatingAsync:
        if (!isMocked(module) || !m_originalsEvaluatedUnderMock.get(module.index()))
            throwAsyncModuleError();
        return;
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linked:
        break;
    }
    auto noteFailedRequire = makeScopeExit([&] {
        if (reason != SynchronousEvaluationReason::Require || !m_instance.vm->exceptionForInspection() || status(module) > ModuleStatus::Linked) [[likely]]
            return;
        clearDeferredRequireMark(module);
        clearRejectedFirstRequire(module);
        if (m_table.anyModuleHasInstances())
            noteFetchOfFailedLoad(module);
    });

    bool runsStepsOfLoader = reason == SynchronousEvaluationReason::Require;
    bool needsLoader = runsStepsOfLoader && needsLoaderForRequire(module);
    std::optional<VM::SynchronousModuleQueueScope> queue;
    if (runsStepsOfLoader)
        queue.emplace(*m_instance.vm);
    SetForScope isInSynchronousLoad(m_isInSynchronousLoad, true);
    auto leaveLoad = [&] {
        if (!queue)
            return;
        leaveStepsOfLoaderToJobs();
        queue.reset();
    };
    auto didLeaveLoad = makeScopeExit(leaveLoad);
    auto runStepsOfLoader = [&] {
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
    };

    JSPromise* promise = nullptr;
    if (needsLoader) {
        takeUpWaysOfLoads();
        SetForScope isInHostRequest(m_currentHostRequest, true);
        promise = loader()->loadModuleSync(realm, moduleKey(module), nullptr, nullptr);
    } else if (statusAtStart == ModuleStatus::Unlinked && moduleData.reachesUncompiledCode()) {
        ModuleRecord* record = registerWithLoader(module);
        RETURN_IF_EXCEPTION(scope, void());
        SetForScope rootLoadedByCompiledCode(m_rootLoadedByCompiledCode, std::optional(module));
        promise = loader()->loadModuleSync(realm, record->moduleKey(), nullptr, nullptr);
    } else {
        if (moduleData.kind != ModuleKind::Data)
            runImportedFiles(module, reason == SynchronousEvaluationReason::Require ? std::optional(JSModuleLoader::EvaluationOfLoad { }) : std::nullopt, nullptr, didStopAtChangeOfAnswers);
        if (scope.exception()) [[unlikely]] {
            keepFailureToLink(module);
            return runStepsOfLoader();
        }
        if (didStopAtChangeOfAnswers && *didStopAtChangeOfAnswers) [[unlikely]]
            return runStepsOfLoader();
        if (status(module) != statusAtStart) {
            runStepsOfLoader();
            RETURN_IF_EXCEPTION(scope, void());
            leaveLoad();
            RELEASE_AND_RETURN(scope, evaluateSynchronously(module, reason));
        }
        link(module);
        if (scope.exception()) [[unlikely]]
            RELEASE_AND_RETURN(scope, keepFailureToLink(module));
        if (moduleData.isSynchronous()) {
            evaluateWithOwnStack(module);
            return runStepsOfLoader();
        }
        promise = evaluate(module, std::nullopt, nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        if (runsStepsOfLoader)
            JSModuleLoader::drainSynchronousModuleQueue(realm);
    }
    RETURN_IF_EXCEPTION(scope, void());
    leaveLoad();
    RETURN_IF_EXCEPTION(scope, void());
    retireRecordsOfSettledLoads();

    if (promise->status() == JSPromise::Status::Fulfilled) {
        if (didStopAtChangeOfAnswers)
            *didStopAtChangeOfAnswers = hasGivenWayToCopy(module);
        return;
    }
    promise->markAsHandled();
    if (promise->status() == JSPromise::Status::Rejected)
        throwException(realm, scope, promise->result());
    else if (status(module) == ModuleStatus::Failed)
        RELEASE_AND_RETURN(scope, throwEvaluationError(module));
    else if (moduleData.hasTopLevelAwait() || (status(module) != ModuleStatus::Evaluating && status(module) != ModuleStatus::Evaluated))
        throwAsyncModuleError();
}

void Modules::evaluateSynchronouslyUnlessItGivesWay(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool didStopAtChangeOfAnswers = false;
    do {
        evaluateSynchronously(module, SynchronousEvaluationReason::Require, m_table.listsNamesOfStatements() ? &didStopAtChangeOfAnswers : nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        askAgainAboutGraphOf(module);
    } while (std::exchange(didStopAtChangeOfAnswers, false) && !hasGivenWayToCopy(module));
}

JSValue Modules::requireNativeModule(ModuleID module)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (JSValue cached = value(module))
        return cached;
    JSValue exports;
    if (host().requireNativeModule) {
        HostRequestScope uncompiledModuleRequest(*this);
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

JSValue Modules::execute(ModuleID module, ReadsExports readsExports)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);

    if (moduleData.kind == ModuleKind::CommonJs) {
        evaluateCommonJS(module);
        RETURN_IF_EXCEPTION(scope, { });
        if (readsExports == ReadsExports::No)
            return jsUndefined();
        RELEASE_AND_RETURN(scope, exportsSeenByLoader(module));
    }
    if (isNativeModule(module))
        RELEASE_AND_RETURN(scope, requireNativeModule(module));

    switch (status(module)) {
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linked:
        break;
    case ModuleStatus::Evaluating:
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
        if (std::optional<uint32_t> number = moduleData.registryID(); m_admitsInspector && number && !scope.exception() && value.isObject() && m_table.recordOfRuntimeSourceIsHandedOutFrozen(*number))
            objectConstructorFreeze(realm, asObject(value));
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
        HostRequestScope uncompiledModuleRequest(*this);
        value = host().requireSpecifier(realm, requirer, pathString(module));
    }
    if (scope.exception()) [[unlikely]] {
        setStatus(module, ModuleStatus::Unlinked);
        return { };
    }
    setValue(module, value);
    setStatus(module, ModuleStatus::Evaluated);
    if (std::optional<uint32_t> number = moduleData.registryID(); number && moduleData.kind == ModuleKind::RuntimeSource && value.isObject())
        m_state.builtinCells->didLoadModule(RuntimeModuleID(*number), asObject(value));
    return value;
}

void Modules::didMockEvictDataModule(ModuleID file)
{
    if (data(file).kind != ModuleKind::Data)
        return;
    m_mockEvictedDataModules.set(file.index());
}

void Modules::didEvictDataFile(ModuleID file)
{
    if (data(file).kind == ModuleKind::Data && status(file) == ModuleStatus::Evaluated && !m_loadedInPreload.get(file.index()) && !isEvicted(file) && !hasIntactOriginal(file)) {
        m_evictedDataFiles.set(file.index());
        m_hasEvictedDataFiles = true;
    }
}

void Modules::didLoadDataFileSlow(ModuleID file, LoadKind way)
{
    if (data(file).kind != ModuleKind::Data || isMocked(file))
        return;
    if (m_modulesThatRanBesideMockAlone.clear(file.index()))
        m_mockedEvictedDataFiles.set(file.index());
    if (!m_evictedDataFiles.clear(file.index()))
        return;
    m_dataModulesLoadedBeforeTestFile.clear(file.index());
    if (LoadedModule* loaded = way == LoadKind::Import ? loadedIfExists(file) : nullptr)
        loaded->requiredValue.set(*this, JSValue());
}

bool Modules::wasEvictedByRemovedMock(ModuleID file) const
{
    return m_mockEvictedDataModules.get(file.index()) && !isPatchedByMock(file) && !activeMock(file) && !hasPendingMock(file);
}

ModuleID Modules::formThatRequireFinds(ModuleID target)
{
    if (wasRequired(target) || isEvicted(target))
        return target;
    Identifier key = keyWithoutForm(target);
    Vector<ModuleID, 2> forms = m_table.importAttributeVariants(key.string());
    forms.removeAllMatching([&](ModuleID form) {
        return isEvicted(form);
    });
    if (forms.isEmpty())
        return target;
    std::optional<ModuleID> file = findByName(key);
    if (file && (isEvicted(*file) || wasEvictedByRemovedMock(*file)))
        file = std::nullopt;
    size_t mockedForm = forms.findIf([&](ModuleID form) {
        return isMocked(form);
    });
    if (data(target).isOtherFormOfFile() && mockedForm == notFound && !(file && isMocked(*file)))
        return target;
    if (file && (wasRequired(*file) || isMocked(*file)))
        return *file;
    for (ModuleID form : forms) {
        if (!data(form).isOtherFormOfFile() && wasRequired(form))
            return form;
    }
    if (mockedForm != notFound)
        return forms[mockedForm];
    if (file && (!hasNotBegun(*file) || isHeldByLoader(*file)))
        return *file;
    for (ModuleID form : forms) {
        bool wasImported = data(form).kind == ModuleKind::Data ? isHeldByLoader(form) : hasBeenImported(form);
        if (!data(form).isOtherFormOfFile() && (status(form) == ModuleStatus::Evaluated || wasImported) && !wasEvictedByRemovedMock(form))
            return form;
    }
    return target;
}

ALWAYS_INLINE void Modules::noteWayOfRequire(ModuleID target, ModuleID referrer, JSObject* staleRequirerObject)
{
    if (m_loadsModulesIndividually) [[unlikely]]
        noteRequireThatRunTimeAnswers(target, referrer);
    if (!m_modulesPendingReset.isEmpty() && data(referrer).kind == ModuleKind::CommonJs) [[unlikely]]
        m_requiresSinceReset.appendIfNotContains(std::pair { fileOf(target), referrer });
    if (m_hasReloadableModules && recordsRequireEdges(referrer) && (!staleRequirerObject || requirerIndex(referrer) != referrer.index())) [[unlikely]]
        noteRequireEdge(target, referrer);
}

JSValue Modules::require(ModuleID target, ModuleID referrer, RequiredBy requiredBy, JSObject* staleRequirerObject, ReadsExports readsExports)
{
    if (m_loadsModulesIndividually) [[unlikely]] {
        evictLoadedModules();
        recordVariantRequest(target);
    }
    SetForScope noCurrentImportCall(m_currentImportCallerModule, ModuleID());
    ModuleID prerecordedRequirer = m_prerecordedRequireTarget == target ? m_prerecordedRequirer : ModuleID();
    m_prerecordedRequireTarget = ModuleID();
    m_prerecordedRequirer = ModuleID();
    SetForScope currentRequirer(m_currentRequirer, referrer ? referrer : prerecordedRequirer);
    bool isFirstRequire = !m_modulesWithKnownParent.get(target.index()) && noteEntryParent(target, referrer);
    bool wasMockPending = isFirstRequire && m_hasReloadableModules && (data(target).kind == ModuleKind::CommonJs || hasIntactOriginal(target)) && isMockPending(target);
    auto recordCompletedRequire = makeScopeExit([&] {
        if (m_hasBegunTestFile && referrer && !m_instance.vm->exceptionForInspection() && isRequireUnderPendingMock(target, referrer)) [[unlikely]] {
            m_requirersUnderPendingMocks.set(fileOf(referrer).index());
            if (isPatchedByMock(fileOf(target)))
                tellIfPatchIsPending(target);
        }
        m_completedRequireTarget = prerecordedRequirer && !m_instance.vm->exceptionForInspection() ? target : ModuleID();
        if (isFirstRequire && (m_instance.vm->exceptionForInspection() || (wasMockPending && !activeMock(target)))) [[unlikely]]
            forgetEntryParent(target);
        if (m_hasReloadableModules && referrer && m_instance.vm->exceptionForInspection()) [[unlikely]]
            forgetRequireEdge(target, referrer);
    });
    if (m_mocks) [[unlikely]]
        recordEntryRequest(target, referrer, LoadKind::Require);
    if (auto* willLoad = m_mocks || m_hasBegunTestFile ? host().willLoad : nullptr) [[unlikely]]
        willLoad(m_instance.globalObject, referrer ? fileOf(referrer) : ModuleID(), fileOf(target), LoadKind::Require);
    if (referrer)
        noteWayOfRequire(target, referrer, staleRequirerObject);
    if (m_hasReloadableModules && referrer && recordsRequireEdges(referrer) && (m_activeMockCount || isMockPending(fileOf(target))) && !m_showsWaysBetweenModuleObjects) [[unlikely]] {
        JSValue answered = answerRequire(target, referrer, requiredBy, FormThatAnswers::TheOneThatLoaderFinds, readsExports);
        if (answered)
            noteAnsweredRequire(target, referrer, answered);
        return answered;
    }
    if (!referrer || !m_showsWaysBetweenModuleObjects) [[likely]]
        return answerRequire(target, referrer, requiredBy, FormThatAnswers::TheOneThatLoaderFinds, readsExports);
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    recordRequire<RequireResolver::Runtime>(target, referrer, false, staleRequirerObject, false);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue required = answerRequire(target, referrer, requiredBy, FormThatAnswers::TheOneThatLoaderFinds, readsExports);
    RETURN_IF_EXCEPTION(scope, { });
    if (m_hasReloadableModules && m_activeMockCount && recordsRequireEdges(referrer)) [[unlikely]]
        noteAnsweredRequire(target, referrer, required);
    recordRequire<RequireResolver::Runtime>(target, referrer, false, staleRequirerObject, true);
    RETURN_IF_EXCEPTION(scope, { });
    return required;
}

bool Modules::didRequireModuleObject(ModuleID target, ModuleID referrer)
{
    if (isEvicted(target) || !isFileOfProgram(target))
        return false;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSObject* staleRequirerObject = takeHostRequestRequirerObject();
    noteWayOfRequire(target, referrer, staleRequirerObject);
    for (bool isAfterLoad : { false, true }) {
        recordRequire<RequireResolver::Runtime>(target, referrer, false, staleRequirerObject, isAfterLoad);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

bool Modules::hasPlannedLoadReached(ModuleID importer) const
{
    for (const std::unique_ptr<LevelOfLoad>& level : m_levelsOfLoads.values()) {
        if (!level->plan)
            continue;
        PlanOfLoad& plan = *level->plan;
        uint32_t numberOfLevels = std::min<uint32_t>(plan.levelsRequested, plan.levels.size());
        for (; plan.levelsReached < numberOfLevels; ++plan.levelsReached) {
            for (const PlanOfLoad::Found& found : plan.level(plan.levelsReached))
                plan.modulesComeTo.set(found.module.index());
        }
        if (plan.modulesComeTo.get(importer.index()))
            return true;
    }
    return false;
}

bool Modules::hasBeenImportedSinceReset(ModuleID target) const
{
    if (!m_momentOfModuleReset || !data(target).isOfLibrary())
        return hasBeenImported(target);
    return firstImportSinceReset(target).has_value();
}

std::optional<uint32_t> Modules::firstImportSinceReset(ModuleID target) const
{
    uint32_t moment = std::max(m_momentOfModuleReset, m_momentsMocksInPlaceOfLibraryModulesEnded.get(target));
    std::optional<uint32_t> first;
    auto noteImport = [&](uint32_t imported) {
        if (imported >= moment)
            first = std::min(first.value_or(imported), imported);
    };
    if (ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(target)); entry && entry->record() && !entry->record()->hasLiveExports())
        noteImport(std::max(moment, m_momentsLibraryModulesWereExposed.get(target)));
    if (auto released = m_lastLinksOfReleasedImporters.find(target); released != m_lastLinksOfReleasedImporters.end())
        noteImport(released->value);
    for (const ImportOfHeldFileData& import : m_table.importsOfHeldFile(target)) {
        if (ModuleID importer(import.importer); wasImporterLoaded(importer))
            noteImport(momentLinkedTo(importer, target));
    }
    return first;
}

bool Modules::hasPendingMockAfterReset(ModuleID module) const
{
    return m_momentOfModuleReset && data(module).isOfLibrary() && m_modulesWithPendingMock.get(module.index()) && hasBeenImported(module) && !hasBeenImportedSinceReset(module);
}

bool Modules::hasBeenImported(ModuleID target, ImportCountsFrom countsFrom) const
{
    if (namespaceIfCreated(target, ModulePhase::Evaluation))
        return true;
    uint32_t momentMockInItsPlaceEnded = m_momentsMocksInPlaceOfLibraryModulesEnded.get(target);
    if (auto released = m_lastLinksOfReleasedImporters.find(target); released != m_lastLinksOfReleasedImporters.end() && released->value >= momentMockInItsPlaceEnded)
        return true;
    bool countsFromItsTurnOn = countsFrom == ImportCountsFrom::ItsTurn && (data(target).kind == ModuleKind::Data || m_instance.vm->importOrderState().isRunningDeferredModule);
    if ((countsFromItsTurnOn || countsFrom == ImportCountsFrom::ItsFetch) && wasReachedByWalk(target))
        return true;
    return std::ranges::any_of(m_table.importsOfHeldFile(target), [&](const ImportOfHeldFileData& import) {
        ModuleID importer(import.importer);
        if (activeMock(importer) && !wasOriginalEvaluated(importer))
            return false;
        bool hasComeToImport = keepsImport(importer, target) || (countsFromItsTurnOn ? wasImporterLinked(importer) : wasImporterLoaded(importer) || hasPlannedLoadReached(importer));
        return hasComeToImport && momentLinkedTo(importer, target) >= momentMockInItsPlaceEnded;
    });
}

bool Modules::wasImporterLinked(ModuleID importer) const
{
    if (status(importer) > ModuleStatus::Linked || (m_entryPointIsLinked && data(importer).isLinkedWithEntryPoint() && !m_modulesLinkedAfterEntryPoint.get(importer.index())))
        return true;
    return wasLoadedWithGraph(importer);
}

bool Modules::wasImporterLoaded(ModuleID importer) const
{
    if (wasImporterLinked(importer))
        return true;
    if (ModuleRecord* record = recordIfExists(importer); record && record->status() != CyclicModuleRecord::Status::New)
        return true;
    return data(importer).isESModuleOfProgram() && wasLoadedForWalk(importer);
}

JSValue Modules::loadEvictedFile(ModuleID file, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool storesExports = m_filesToLoadAgain.get(file.index());
    bool storesStatus = storesExports && !recordIfExists(file);
    m_filesToLoadAgain.clear(file.index());
    if (storesStatus)
        setStoredStatus(file, StoredModuleStatus::Evaluating);
    JSString* path = pathString(file);
    if (String text = m_table.fullPath(file); text.contains('?')) [[unlikely]]
        path = jsString(*m_instance.vm, text.left(text.reverseFind('?')));
    moduleObject(file);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = requireSpecifier(path, referrer ? referrer : file, NamedByBuild::CompiledModule);
    if (scope.exception()) [[unlikely]] {
        if (storesExports) {
            m_filesToLoadAgain.set(file.index());
            setStoredStatus(file, StoredModuleStatus::InRecord);
        }
        return { };
    }
    if (storesStatus)
        setStoredStatus(file, StoredModuleStatus::Evaluated);
    if (storesExports) {
        recaptureExports(file);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return value;
}

JSValue Modules::exportsOnObjectMadeByRequire(const LoadedModule& loaded)
{
    JSObject* entry = loaded.moduleObjectFromRequire;
    if (!entry)
        return { };
    return entry->get(m_instance.globalObject, m_instance.vm->propertyNames->exports);
}

JSValue Modules::requireThroughCache(ModuleID target, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(target);
    if (JSValue kept = loaded.valueInRequireCache) {
        JSValue stored = exportsOnObjectMadeByRequire(loaded);
        RETURN_IF_EXCEPTION(scope, { });
        return stored ? stored : kept;
    }
    JSValue answered = isMockedLibraryModule(target) ? answerRequireWithMock(target, referrer) : answerRequire(target);
    RETURN_IF_EXCEPTION(scope, { });
    addToRequireCache(target, answered);
    if (referrer)
        ensureWaysTo(target).firstRequirer = referrer;
    return answered;
}

bool Modules::isMockedLibraryModule(ModuleID module)
{
    return m_libraryModulesWithMock.get(module.index()) && host().replacesNativeModule && host().replacesNativeModule(m_instance.globalObject, m_table.fullPath(module));
}

JSValue Modules::answerRequireWithMock(ModuleID target, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (referrer && m_libraryModulesThatMockReplaces.get(target.index()))
        RELEASE_AND_RETURN(scope, requireSpecifier(pathString(target), referrer, NamedByBuild::UncompiledModule));
    JSValue answer = answerOfRequireToMock(target);
    RETURN_IF_EXCEPTION(scope, { });
    if (answer)
        return answer;
    RELEASE_AND_RETURN(scope, moduleNamespace(target, ModulePhase::Evaluation));
}

JSValue Modules::answerOfRequireToMock(ModuleID target)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* exports = ensureLoaded(target).exportsOfMock;
    if (!exports)
        return { };
    if (ensureLoaded(target).exportsOfMockAreOfCommonJSModule)
        return exports;
    RELEASE_AND_RETURN(scope, requiredExportsOfMock(exports));
}

JSValue Modules::requiredExportsOfMock(JSObject* exports)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue esModule = exports->getIfPropertyExists(realm, vm.propertyNames->__esModule);
    RETURN_IF_EXCEPTION(scope, { });
    if (!esModule || !esModule.toBoolean(realm))
        return exports;
    JSValue defaultExport = exports->getIfPropertyExists(realm, vm.propertyNames->defaultKeyword);
    RETURN_IF_EXCEPTION(scope, { });
    return defaultExport && !defaultExport.isUndefined() ? defaultExport : JSValue(exports);
}

JSValue Modules::answerRequire(ModuleID target, ModuleID referrer, RequiredBy requiredBy, FormThatAnswers formThatAnswers, ReadsExports readsExports)
{
    if (!m_mockedImports.isEmpty()) [[unlikely]]
        clearRetainedImportNamespaces();
    if (m_importersRetainedAboveMock.get(target.index())) [[unlikely]]
        tellOfFilesThatStayedLoadedBelow(target);
    if (m_modulesReleasedByLoader.get(target.index()) && !hasInstances(target)) [[unlikely]]
        bringBackWhatLoaderLetGoBelow(target);
    if (isEvaluatedUnderPendingPatch(target, referrer)) [[unlikely]]
        tellThatFileDidNotRunAgain(fileOf(target));
    if (!m_modulesWithRunningFactory.isEmpty()) [[unlikely]]
        tellIfFactoryIsRunning(target, LoadKind::Require);
    if (JSValue settled = settledAnswerOfRequire(target, requiredBy))
        return settled;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool loadsEvictedDataFile = false;
    bool asksForOtherFormOfFile = data(target).isOtherFormOfFile();
    if (data(target).kind == ModuleKind::Data) [[unlikely]] {
        if (formThatAnswers == FormThatAnswers::TheOneThatLoaderFinds && m_table.mayHaveAttributeVariants())
            target = formThatRequireFinds(target);
        loadsEvictedDataFile = isEvictedDataFile(target);
        didLoadDataFile(target, LoadKind::Require);
    }
    removeUnusedLingeringMocks(target);
    RETURN_IF_EXCEPTION(scope, { });
    if (!m_importsAwaitingSource.isEmpty()) [[unlikely]] {
        walkInPlaceOfCallsHeldForTextOf(fileOf(target));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (isMockLingering(target) && m_modulesWithPendingMock.get(target.index()) && !wasStartedByWalk(target) && isResolvedForInFlightLoads(target)) [[unlikely]] {
        removeLingeringMock(target);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (JSValue error = mockLoadError(target)) [[unlikely]] {
        throwException(m_instance.globalObject, scope, error);
        return { };
    }
    const ModuleData& targetData = data(target);
    if (m_mocks) [[unlikely]]
        noteLoadBegunByFactories(target);
    bool hasPendingFactory = tellHostOfRequire(target);
    if (m_mocks && referrer && host().isRequiredByOwnFactory && hasPendingFactoryFromLoad(target)) [[unlikely]] {
        if (host().isRequiredByOwnFactory(m_instance.globalObject, target, referrer)) {
            JSObject* original = wasOriginalEvaluated(target) ? nullptr : actualKeyNamespace(target);
            RETURN_IF_EXCEPTION(scope, { });
            if (original)
                return original;
            RELEASE_AND_RETURN(scope, requireOriginalUnderMock(target));
        }
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.fullPath(target)));
        return { };
    }
    bool isCallFromDependency = referrer && isMocked(target) && status(fileOf(referrer)) == ModuleStatus::Evaluating && importsStatically(target, fileOf(referrer));
    if (m_originalBeingEvaluated == target || (isCallFromDependency && m_modulesWithRunningFactory.get(target.index()))) [[unlikely]] {
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.fullPath(target)));
        return { };
    }
    if (isRequestedByOwnFactory(target)) [[unlikely]] {
        JSValue original = isDataFile(target) && !wasOriginalEvaluated(target) && host().requireOriginal && !refusesOriginalsBesideMocks() ? host().requireOriginal(m_instance.globalObject, target, true) : JSValue();
        RETURN_IF_EXCEPTION(scope, { });
        if (original)
            return original;
        RELEASE_AND_RETURN(scope, answerOwnFactoryRequire(target));
    }
    rejectRequireInSweep(target, referrer);
    RETURN_IF_EXCEPTION(scope, { });
    if (m_mocks && referrer && targetData.isESModuleOfProgram()) [[unlikely]] {
        captureEvaluatingRequirer(referrer, target);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (m_mocks && referrer && isMocked(target) && targetData.isESModuleOfProgram() && !targetData.isFacade() && isOfPackage(referrer) && requireBypassesMock(target, referrer)) [[unlikely]] {
        if (m_modulesWithPendingMock.get(target.index()) && takeFailureBeforeMock(target)) {
            forgetLoadThatFailed(target);
            m_originalsEvaluatedWhenMocked.clear(target.index());
            m_originalsEvaluatedUnderMock.clear(target.index());
        }
        JSValue original = requireOriginalUnderMock(target);
        if (scope.exception() && !wasOriginalEvaluated(target) && !errorOfModuleThatRanBesideMock(target)) [[unlikely]]
            noteRefusedOriginal(target);
        RETURN_IF_EXCEPTION(scope, { });
        return original;
    }
    if (m_mocks && isMocked(target) && targetData.isESModuleOfProgram() && !targetData.isFacade() && !hasIntactOriginal(target)) [[unlikely]] {
        bool isOverActiveMock = activeMock(target) && !hasRemovedMock(target);
        bool isPending = m_modulesWithPendingMock.get(target.index());
        if ((isOverActiveMock || (isPending && wasOriginalEvaluated(target))) && host().shouldBypassMock && host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(target), String(), false)) {
            auto waiting = m_pendingMockCalls.find(target);
            if (!isOverActiveMock || !isPending || waiting == m_pendingMockCalls.end() || !waiting->value.keepsActiveMock || m_evictedActiveMocks.get(target.index()))
                RELEASE_AND_RETURN(scope, requireOriginalUnderMock(target));
            if (isMockedByFactoryThatTakesOriginal(target))
                m_filesWithRejectedRequire.set(fileOf(target).index());
            JSObject* stashed = moduleNamespace(target, ModulePhase::Evaluation);
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, whatRequireMakesOfNamespace(stashed));
        }
    }
    if (m_mocks && targetData.kind == ModuleKind::CommonJs && isMocked(target) && !targetData.isOfLibrary() && status(target) == ModuleStatus::Linked && !activeMock(target) && (m_modulesWithRunningFactory.get(target.index()) || (!m_modulesWithPendingMock.get(target.index()) && host().isRequestedByPendingFactory && host().isRequestedByPendingFactory(m_instance.globalObject, target)))) [[unlikely]] {
        if (readsExports == ReadsExports::Yes && commonJSOriginalNamedBy(actualKey(target)) && host().requireOriginal) {
            JSValue original = host().requireOriginal(m_instance.globalObject, target, true);
            RETURN_IF_EXCEPTION(scope, { });
            if (original)
                return original;
        }
        m_originalsBeingEvaluated.set(target.index());
        evaluateCommonJS(target);
        m_originalsBeingEvaluated.clear(target.index());
        if (scope.exception()) [[unlikely]] {
            discardFailedEvaluation(target);
            return { };
        }
        return commonJSExports(target);
    }
    if (m_mocks && readsExports == ReadsExports::Yes && m_modulesWithPendingMock.get(target.index()) && host().requireOriginal && (isEvictedForRequireAlone(target) || (status(target) <= ModuleStatus::Linked && (isDataFile(target) || (targetData.mayRunAsOriginal() && commonJSOriginalNamedBy(actualKey(target))))))) [[unlikely]] {
        JSValue original = host().requireOriginal(m_instance.globalObject, target, false);
        RETURN_IF_EXCEPTION(scope, { });
        if (original)
            return original;
    }
    if (m_mocks && needsOriginalBeforeFactory(target) && host().shouldBypassMock && host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(target), String(), false)) [[unlikely]] {
        JSObject* original = actualKeyNamespace(target);
        RETURN_IF_EXCEPTION(scope, { });
        if (original)
            return original;
    }
    auto exportsWhileRequired = [&](LoadedModule& loaded) {
        if (!loaded.exportsWhileRequired)
            loaded.exportsWhileRequired.set(*this, constructEmptyObject(m_instance.globalObject));
        return loaded.exportsWhileRequired.get();
    };
    LoadedModule* requiredAboveMocks = hasPendingMocks() && goesThroughImports(target) && !isMocked(target) ? &ensureLoaded(target) : nullptr;
    if (requiredAboveMocks && requiredAboveMocks->isBeingRequired) [[unlikely]]
        return exportsWhileRequired(*requiredAboveMocks);
    bool isRequiredByProgram = requiredBy == RequiredBy::Program || (referrer && !data(referrer).isOfLibrary());
    if (m_loadsModulesIndividually && targetData.isESModuleOfProgram()) [[unlikely]]
        willFetchESModules(target);
    if (m_mocks && m_modulesWithPendingMock.get(target.index()) && (hasFailedWithoutMock(target) || takeFailureBeforeMock(target))) [[unlikely]]
        forgetLoadThatFailed(target);
    if (!m_rejectedRequires.isEmpty() && (!targetData.isOfLibrary() || isRequiredByProgram) && targetData.kind == ModuleKind::SourceText && !isMocked(target) && !vm.m_synchronousModuleQueue && !m_blockedRequiresWithError.get(target.index()) && !hasFailedToLoadForGood(target) && !m_isResumingFactoryChainImport && isStillBlockedOnFactories(target)) [[unlikely]] {
        willRejectRequireAgain(target, target);
        RETURN_IF_EXCEPTION(scope, { });
        for (const RejectedRequire& pair : m_rejectedRequires) {
            if (pair.required == target && pair.mocked != target)
                m_requiresRejectedAgain.add(static_cast<uint64_t>(target.index()) << 32 | pair.mocked.index());
        }
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.fullPath(target)));
        return { };
    }
    bool wasHeldBeforeWalk = holdsLinksOfLoadThatMayFail(target);
    SetForScope requiredRootWhoseImportsThrew(m_requiredRootWhoseImportsThrew, std::nullopt);
    if (!targetData.isOfLibrary() || isRequiredByProgram) {
        SetForScope requireRunsMocks(vm.aotState().requireRunsMocks, true);
        SetForScope currentRequireTarget(m_currentRequireTarget, target);
        SetForScope requiringModule(m_requiringModule, referrer);
        std::optional<SetForScope<bool>> isBeingRequired;
        if (requiredAboveMocks) [[unlikely]]
            isBeingRequired.emplace(requiredAboveMocks->isBeingRequired, true);
        std::optional<VM::SynchronousModuleQueueScope> loadsOfFactories;
        if (m_mocks && targetData.kind == ModuleKind::SourceText && !isMocked(target) && !vm.m_synchronousModuleQueue) [[unlikely]]
            loadsOfFactories.emplace(vm);
        SetForScope shouldContinuePastPendingFactories(m_shouldContinuePastPendingFactories, !!loadsOfFactories);
        bool factoryHasNotSettled = runMocksInGraph(target);
        if (loadsOfFactories) [[unlikely]] {
            if (auto mocked = hasPendingFactory && !factoryHasNotSettled && !scope.exception() && !failsWhateverFactoriesDo(target) ? mockWhoseFactoryHoldsUpRequire(target) : std::nullopt) {
                if (!evaluateCommonJSModulesBeforeFactories(target))
                    m_requiredRootWhoseImportsThrew = target;
                else
                    factoryHasNotSettled = !m_rejectedRequires.containsIf([&](const RejectedRequire& pair) { return pair.mocked == *mocked && pair.call == pendingFactoryCallID(*mocked); });
            }
            if (!scope.exception())
                JSModuleLoader::drainSynchronousModuleQueue(m_instance.globalObject);
            leaveStepsOfLoaderToJobs();
            if (Exception* thrown = scope.exception(); thrown && !vm.isTerminationException(thrown) && scope.tryClearException()) {
                loadsOfFactories.reset();
                willRejectRequireAgain(target, target);
                if (!scope.exception() || scope.tryClearException()) {
                    recordRejectedRequire(target);
                    m_blockedRequiresWithError.set(target.index());
                    throwException(m_instance.globalObject, scope, thrown);
                }
                return { };
            }
        }
        RETURN_IF_EXCEPTION(scope, { });
        if (factoryHasNotSettled) [[unlikely]] {
            recordRejectedRequire(target);
            if (loadsOfFactories) {
                loadsOfFactories.reset();
                willRejectRequireAgain(target, target);
                RETURN_IF_EXCEPTION(scope, { });
            }
            throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.fullPath(target)));
            return { };
        }
    } else
        evaluateModulesOwedBelow(target);
    RETURN_IF_EXCEPTION(scope, { });
    auto hearsHowRequireEnds = makeScopeExit([&] {
        if (scope.exception() && isFailedESModule(target)) [[unlikely]]
            m_loadRoots.set(target.index());
        if (wasHeldBeforeWalk || !holdsLinksOfLoadThatMayFail(target)) [[likely]]
            return;
        if (scope.exception())
            bringBackLinksOfLoadThatFailed(target);
        else
            letLinksOfLoadStand(target);
    });
    bool mayBeRefused = (hasPendingFactory || !m_rejectedRequires.isEmpty()) && !failsWhateverFactoriesDo(target);
    if (mayBeRefused) [[unlikely]] {
        resolveUncompiledImports(target);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (mayBeRefused && hasPendingFactory) [[unlikely]]
        recordRejectedRequire(target, false);
    if (mayBeRefused && !m_rejectedRequires.isEmpty() && (!targetData.isOfLibrary() || isRequiredByProgram)) [[unlikely]] {
        JSValue original = answerRequireWhileFactoryHasNotSettled(target, referrer);
        RETURN_IF_EXCEPTION(scope, { });
        if (original)
            return original;
    }
    if (m_activeMockCount) [[unlikely]] {
        if (getsOriginalBesideMock(target, referrer))
            RELEASE_AND_RETURN(scope, answerRequireWithOriginal(target, referrer));
        if (referrer && activeMock(target) && isDataFile(target) && readsExports == ReadsExports::Yes && host().requireOriginal && requiresOriginalUnderMock(target, referrer)) {
            JSValue original = host().requireOriginal(m_instance.globalObject, target, true);
            RETURN_IF_EXCEPTION(scope, { });
            if (original)
                return original;
        }
        if (m_originalsBeingEvaluated.get(target.index()) && activeMock(target))
            return ensureLoaded(target).answerOfMockWhileOriginalRuns.get();
        if (activeMock(target)) {
            noteAskUnderMockInPlace(target);
            noteRequireOfMockLaidInPreload(target);
        }
    }
    if (delegatesToHost(target, LoadKind::Require)) [[unlikely]]
        RELEASE_AND_RETURN(scope, loadEvictedFile(fileOf(target), referrer));
    if (targetData.isOfLibrary() && isRequiredByProgram && isKeptInRequireCache(target)) [[unlikely]]
        RELEASE_AND_RETURN(scope, requireThroughCache(target, referrer));
    if (targetData.isOfLibrary() && isRequiredByProgram && isMockedLibraryModule(target)) [[unlikely]]
        RELEASE_AND_RETURN(scope, answerRequireWithMock(target, referrer));

    if (targetData.isUncompiledCode()) {
        m_uncompiledNamespacesNotShownYet.clear(target.index());
        RELEASE_AND_RETURN(scope, requireSpecifier(pathString(target), *targetData.referrerModule(), NamedByBuild::UncompiledModule));
    }
    if (standsForFileOutsideImage(target)) {
        JSObject* requirer = moduleObject(*targetData.referrerModule());
        RETURN_IF_EXCEPTION(scope, { });
        HostRequestScope uncompiledModuleRequest(*this);
        RELEASE_AND_RETURN(scope, host().requireSpecifier(m_instance.globalObject, requirer, pathString(target)));
    }
    if (!targetData.hasExportTable()) {
        JSValue executed = execute(target, readsExports);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_ASSERT(executed);
        return executed;
    }

    if (JSValue thrownByBody = hasIntactOriginal(target) && !activeMock(target) ? errorOfModuleThatRanBesideMock(target) : JSValue()) [[unlikely]] {
        reportRetainedFailure(target);
        throwException(m_instance.globalObject, scope, thrownByBody);
        return { };
    }
    willRequireESModule(target);
    noteCallThatComesToModuleItself(target);
    LoadedModule& loaded = ensureLoaded(target);
    if (loaded.isBeingRequired)
        return exportsWhileRequired(loaded);
    bool isAnsweredAsByLoader = requiredBy == RequiredBy::Program && targetData.loaderReturnsNamespace();
    bool answersDefaultExport = !isAnsweredAsByLoader && !isMocked(target) && (targetData.requireResult == RequireResult::DefaultExport || (targetData.requireResult == RequireResult::NamespaceIfImported && !loaded.requiredValue && (loadsEvictedDataFile || !isHeldByLoader(target))));
    if (loaded.isKeptByRequire && loaded.requiredValue && !isAnsweredAsByLoader && status(target) == ModuleStatus::Failed) [[unlikely]]
        return loaded.requiredValue.get();
    bool wasBeingRequired = false;
    bool isFailedUnderMock = status(target) == ModuleStatus::Failed && isMocked(target) && !m_mocksWithoutFactory.get(target.index());
    if (status(target) != ModuleStatus::Evaluated && !isFailedUnderMock) {
        wasBeingRequired = loaded.isBeingRequired = status(target) != ModuleStatus::Evaluating;
        bool wasKeptByRequire = std::exchange(loaded.isKeptByRequire, true);
        bool mayHaveMocksToRun = false;
        evaluateSynchronously(target, SynchronousEvaluationReason::Require, isTestRun() ? &mayHaveMocksToRun : nullptr);
        loaded.isBeingRequired = false;
        if (scope.exception()) [[unlikely]] {
            recordRejectedRequire(target);
            loaded.isKeptByRequire = wasKeptByRequire;
            if (!wasKeptByRequire)
                loaded.moduleObjectFromRequire.set(*this, nullptr);
            didEndLoadOfRequire();
            return { };
        }
        if (mayHaveMocksToRun) [[unlikely]] {
            loaded.isKeptByRequire = wasKeptByRequire;
            askAgainAboutGraphOf(target);
            if (requiredBy == RequiredBy::Program && m_moduleWhoseCopyCallerLoads == target && !isMocked(target) && delegatesToHost(target, LoadKind::Require) && hasGivenWayToCopy(target)) {
                loaded.moduleObjectFromRequire.set(*this, nullptr);
                return { };
            }
            RELEASE_AND_RETURN(scope, answerRequire(target, referrer, requiredBy, formThatAnswers, readsExports));
        }
    } else if (JSObject* exports = nullptr; m_hasBegunTestFile && requiresPatch(target, exports)) [[unlikely]] {
        if (exports)
            RELEASE_AND_RETURN(scope, requiredExportsOfMock(exports));
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.fullPath(target)));
        return { };
    }
    if (m_hasBegunTestFile) [[unlikely]]
        tellIfPatchIsPending(target);
    if (wasBeingRequired && !m_detachedLoads.isEmpty() && !m_modulesWithPendingMock.isEmpty()) [[unlikely]] {
        BitVector below = staticImportClosure(target, MockTraversal::Stop);
        for (size_t mocked : m_modulesWithPendingMock) {
            if (below.get(mocked) && m_table.hasFilePath(ModuleID(mocked)) && host().didRunOriginal)
                host().didRunOriginal(m_instance.globalObject, m_table.fullPath(ModuleID(mocked)), OriginalRun::RunsAmongLoads);
        }
    }
    auto evictsLoadedModules = makeScopeExit([&] {
        if (!scope.exception())
            evictLoadedModules(false, IsAnswerToRequireOwed::Yes);
        if (wasBeingRequired)
            didEndLoadOfRequire();
    });
    auto answer = [&](JSValue answered) {
        if (wasBeingRequired && loaded.isKeptByRequire && loaded.moduleObjectFromRequire) [[unlikely]] {
            loaded.moduleObjectFromRequire->putDirect(vm, vm.propertyNames->exports, answered);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, answered);
        }
        return answered;
    };
    if (!wasBeingRequired && loaded.requiredValue && loaded.moduleObjectFromRequire) [[unlikely]] {
        JSValue held = exportsOnObjectMadeByRequire(loaded);
        RETURN_IF_EXCEPTION(scope, { });
        if (held && held != loaded.exportsGivenToObjectMadeByRequire.get())
            return held;
    }
    if (JSValue answered = loaded.requiredValue; answered && !isAnsweredAsByLoader)
        return answer(answered);

    JSValue result;
    if (isMocked(target)) [[unlikely]] {
        result = answerOfRequireToMock(target);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (result) {
    } else if (answersDefaultExport) {
        std::optional<unsigned> index = m_table.findExport(target, vm.propertyNames->defaultKeyword.impl());
        RELEASE_ASSERT(index);
        if (m_table.exports(target)[*index].staysIn) {
            refuseToHandOutExportOf(target, vm.propertyNames->defaultKeyword.string());
            return { };
        }
        result = readExport(target, *index);
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        RELEASE_ASSERT(targetData.requireResult != RequireResult::ModuleExports);
        JSObject* namespaceObject = moduleNamespace(target, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        result = whatRequireMakesOfNamespace(namespaceObject);
        RETURN_IF_EXCEPTION(scope, { });
        markNamespaceAsRequired(target);
    }
    if (asksForOtherFormOfFile && !targetData.isOtherFormOfFile()) [[unlikely]]
        return result;
    if (!isAnsweredAsByLoader)
        loaded.requiredValue.set(*this, result);
    loaded.wasRequiredByHost |= requiredBy == RequiredBy::Program;
    return answer(result);
}

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
    detachFailedModuleObject(module);
    RETURN_IF_EXCEPTION(scope, void());
    if (moduleStatus == ModuleStatus::Unlinked) {
        link(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (m_mocks && isFileRequestedByOwnFactory(module)) [[unlikely]]
        m_filesEvaluatedByOwnFactory.set(module.index());

    ModuleRecord* record = recordIfExists(module);
    if (!record && moduleData.evaluatesItself()) {
        evaluateByCallingBody(module);
        RETURN_IF_EXCEPTION(scope, void());
    } else {
        setStatus(module, ModuleStatus::Evaluating);
        callBodyUnderOwnMark(module, moduleData.evaluatesItself());
        if (Exception* exception = scope.exception()) [[unlikely]] {
            didThrowInBody(module, exception->value());
            return;
        }
        setStatus(module, ModuleStatus::Evaluated);
    }
    if (moduleData.isImportableDuringEvaluation()) [[unlikely]] {
        captureExportsOfFinishedFile(module, ExportsSoFar::OfThisRun);
        RETURN_IF_EXCEPTION(scope, void());
    }
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
    stopWhereExportsLeaveCompiledCode(module);
    return exportsGivenBackToCompiledCode(module);
}

JSValue Modules::exportsGivenBackToCompiledCode(ModuleID module) const
{
    ASSERT(data(module).kind == ModuleKind::CommonJs);
    JSValue exports = data(module).hasValue() ? ProgramStatics::readVariable(m_instance, data(module).value) : JSValue();
    return exports ? exports : jsUndefined();
}

JSValue Modules::exportsSeenByLoader(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    bool isExportsVariableBound = loaded && loaded->moduleObjectBoundToExports.get() == loaded->requirer.get();
    if (!loaded || !loaded->requirer || (data(module).hasValue() && !isExportsVariableBound) || data(module).hasUnescapedExports()) [[likely]]
        return exportsGivenBackToCompiledCode(module);
    return loaded->requirer->get(m_instance.globalObject, m_instance.vm->propertyNames->exports);
}

bool Modules::isInRequireMap(ModuleID file)
{
    const ModuleData& fileData = data(file);
    if (hasNotBegun(file) || status(file) == ModuleStatus::Failed || !(isFileOfProgram(file) || fileData.kind == ModuleKind::Data))
        return false;
    if (fileData.kind == ModuleKind::CommonJs)
        return true;
    LoadedModule* loaded = loadedIfExists(file);
    return firstRequirerOf(file) || (loaded && loaded->firstUncompiledRequirer);
}

std::optional<OptionSet<ModuleObjectTrait>> Modules::newModuleObjectTraits(ModuleID module)
{
    if (data(module).kind != ModuleKind::CommonJs || ensureLoaded(module).requirer)
        return std::nullopt;
    stopWhereExportsLeaveCompiledCode(module);
    OptionSet<ModuleObjectTrait> traits;
    if (data(module).hasValue())
        traits.add(ModuleObjectTrait::HasExportsVariable);
    if (m_filesStartedAsEntryPoint.get(module.index()))
        traits.add(ModuleObjectTrait::IsMain);
    return traits;
}

void Modules::adoptModuleObject(ModuleID module, const ModuleObjects& objects)
{
    LoadedModule& loaded = ensureLoaded(module);
    RELEASE_ASSERT(!loaded.requirer && objects.module);
    notePrototypeOfModuleObjects(objects.module);
    loaded.requirer.set(*this, objects.module);
    loaded.requireFunction.set(*this, objects.require);
}

void Modules::releaseModuleObject(ModuleID module)
{
    if (status(module) > ModuleStatus::Linked || isEvicted(module))
        return;
    LoadedModule& loaded = ensureLoaded(module);
    loaded.requirer.set(*this, nullptr);
    loaded.moduleObjectBoundToExports.set(*this, nullptr);
    loaded.exportsWhenVariableAndModuleObjectAgreed.set(*this, JSValue());
    loaded.structureOfModuleObjectLastLookedAt.set(*this, nullptr);
    loaded.idOfStructureOfModuleObjectLastLookedAt = StructureID();
    loaded.isExportsVariableTakenOffModuleObject = false;
    loaded.isOrderOfStoreAndDeedUnknown = false;
    loaded.requireFunction.set(*this, JSValue());
}

JSObject* Modules::moduleObjectWithExportsVariable(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && data(module).kind == ModuleKind::CommonJs && data(module).hasValue() ? loaded->requirer.get() : nullptr;
}

void Modules::didBindExportsVariable(ModuleID module)
{
    LoadedModule& loaded = ensureLoaded(module);
    loaded.moduleObjectBoundToExports.set(*this, loaded.requirer.get());
    if (data(module).kind == ModuleKind::CommonJs)
        loaded.exportsWhenVariableAndModuleObjectAgreed.set(*this, exportsGivenBackToCompiledCode(module));
}

JSObject* Modules::moduleObject(ModuleID module)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.requirer)
        return loaded.requirer;
    if (loaded.failedModuleObject) [[unlikely]] {
        if (status(module) <= ModuleStatus::Linked)
            return loaded.failedModuleObject;
        detachFailedModuleObject(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    ModuleObjects objects;
    if (data(module).kind == ModuleKind::CommonJs) {
        host().createModuleObject(realm, pathString(module), module, *newModuleObjectTraits(module), objects);
    } else {
        JSObject* meta = importMeta(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        host().createRequirer(realm, meta, module, objects);
    }
    RETURN_IF_EXCEPTION(scope, nullptr);
    notePrototypeOfModuleObjects(objects.module);
    loaded.requirer.set(*this, objects.module);
    if (data(module).kind == ModuleKind::CommonJs) {
        loaded.moduleObjectBoundToExports.set(*this, objects.module);
        loaded.exportsWhenVariableAndModuleObjectAgreed.set(*this, exportsGivenBackToCompiledCode(module));
    }
    loaded.requireFunction.set(*this, objects.require);
    return objects.module;
}

JSObject* Modules::moduleObjectMadeByRequire(ModuleID module, ModuleObjectCreator creator)
{
    if (data(module).kind == ModuleKind::CommonJs)
        return moduleObject(module);
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSObject* made = ensureLoaded(module).moduleObjectFromRequire)
        return made;

    JSValue answered;
    if (creator == ModuleObjectCreator::RequireCache && isMocked(module) && data(module).isESModuleOfProgram() && !isInRequireMap(module)) {
        answered = moduleNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ensureLoaded(module).requiredValue.set(*this, answered);
    } else {
        answered = answerRequire(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (JSObject* made = ensureLoaded(module).moduleObjectFromRequire)
        return made;
    OptionSet<ModuleObjectTrait> traits { ModuleObjectTrait::IsNotCommonJSModule };
    bool answersNamespace = data(module).kind == ModuleKind::SourceText || (data(module).kind == ModuleKind::Data && dynamicDowncast<ModuleNamespace>(answered));
    bool isCreatedByRequireCache = creator == ModuleObjectCreator::RequireCache && !isInRequireMap(module);
    if (answersNamespace && !isCreatedByRequireCache)
        traits.add(ModuleObjectTrait::IsRequiredESModule);
    ModuleObjects objects;
    host().createModuleObject(m_instance.globalObject, isFormOfDataFile(module) ? jsString(vm, keyInRequireCache(module)) : pathString(module), module, traits, objects);
    RETURN_IF_EXCEPTION(scope, nullptr);
    notePrototypeOfModuleObjects(objects.module);
    objects.module->putDirect(vm, vm.propertyNames->exports, answered);
    ensureLoaded(module).moduleObjectFromRequire.set(*this, objects.module);
    ensureLoaded(module).isCreatedByRequireCache = isCreatedByRequireCache;
    ensureLoaded(module).exportsGivenToObjectMadeByRequire.set(*this, answered);
    return objects.module;
}

namespace {

class StandInForModuleObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | OverridesGetPrototype | ProhibitsPropertyCaching;

    template<typename CellType, SubspaceAccess>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        STATIC_ASSERT_ISO_SUBSPACE_SHARABLE(StandInForModuleObject, Base);
        return &vm.plainObjectSpace();
    }

    static StandInForModuleObject* create(VM& vm, JSGlobalObject* realm, const String& messageOfRefusal)
    {
        Structure* structure = Structure::create(vm, realm, jsNull(), TypeInfo(ObjectType, StructureFlags), info());
        auto* standIn = new (NotNull, allocateCell<StandInForModuleObject>(vm)) StandInForModuleObject(vm, structure);
        standIn->finishCreation(vm);
        standIn->putDirect(vm, vm.propertyNames->message, jsString(vm, messageOfRefusal));
        return standIn;
    }

    DECLARE_INFO;

    static bool getOwnPropertySlot(JSObject* object, JSGlobalObject* realm, PropertyName name, PropertySlot& slot)
    {
        if (slot.internalMethodType() == PropertySlot::InternalMethodType::VMInquiry)
            return Base::getOwnPropertySlot(object, realm, name, slot);
        return isAnsweredByModuleObject(object, realm, name) && refuse(object);
    }
    static void getOwnPropertyNames(JSObject* object, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode) { refuse(object); }
    static bool put(JSCell* cell, JSGlobalObject* realm, PropertyName name, JSValue value, PutPropertySlot& slot)
    {
        if (slot.thisValue() != JSValue(cell) && !isAnsweredByModuleObject(asObject(cell), realm, name))
            return Base::put(cell, realm, name, value, slot);
        return refuse(cell);
    }
    static bool putByIndex(JSCell* cell, JSGlobalObject*, unsigned, JSValue, bool) { return refuse(cell); }
    static bool deleteProperty(JSCell* cell, JSGlobalObject* realm, PropertyName name, DeletePropertySlot&) { return !isAnsweredByModuleObject(asObject(cell), realm, name) || refuse(cell); }
    static bool defineOwnProperty(JSObject* object, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool) { return refuse(object); }
    static JSValue getPrototype(JSObject* object, JSGlobalObject*)
    {
        JSObject* prototype = modulesOf(object->globalObject())->prototypeOfModuleObjects();
        return prototype ? JSValue(prototype) : jsNull();
    }
    static bool setPrototype(JSObject* object, JSGlobalObject*, JSValue, bool) { return refuse(object); }

private:
    StandInForModuleObject(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    static bool isAnsweredByModuleObject(JSObject* standIn, JSGlobalObject* realm, PropertyName name)
    {
        VM& vm = realm->vm();
        if (name == vm.propertyNames->exports || name == Identifier::fromString(vm, "require"_s))
            return true;
        JSObject* prototype = modulesOf(standIn->globalObject())->prototypeOfModuleObjects();
        if (!prototype)
            return false;
        PropertySlot slot(prototype, PropertySlot::InternalMethodType::VMInquiry, &vm);
        return prototype->methodTable()->getOwnPropertySlot(prototype, realm, name, slot);
    }

    static bool refuse(JSCell* cell)
    {
        JSObject* standIn = asObject(cell);
        JSGlobalObject* realm = standIn->globalObject();
        VM& vm = realm->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        throwException(realm, scope, createError(realm, asString(standIn->getDirect(vm, vm.propertyNames->message))->tryGetValue()));
        return false;
    }
};

const ClassInfo StandInForModuleObject::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(StandInForModuleObject) };

} // namespace

void Modules::notePrototypeOfModuleObjects(JSObject* freshModuleObject)
{
    JSValue prototype = freshModuleObject->getPrototypeDirect();
    if (m_prototypeOfModuleObjects || !prototype.isObject())
        return;
    m_prototypeOfModuleObjects = asObject(prototype);
    m_storedReferences.recordAll();
}

JSObject* Modules::moduleObjectOrPlaceholder(ModuleID module)
{
    if (!hidesModuleObject(module))
        return moduleObjectMadeByRequire(module);
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.standInForModuleObject)
        loaded.standInForModuleObject.set(*this, StandInForModuleObject::create(*m_instance.vm, m_instance.globalObject, moduleObjectRefusedMessage(m_table.fullPath(module))));
    return loaded.standInForModuleObject;
}

JSObject* Modules::moduleObjectIfCreated(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded)
        return nullptr;
    if (hidesModuleObject(module))
        return loaded->standInForModuleObject;
    return data(module).kind == ModuleKind::CommonJs ? loaded->requirer.get() : loaded->moduleObjectFromRequire.get();
}

JSObject* Modules::objectThatRanAsMain()
{
    if (m_didMainFail)
        return ensureLoaded(m_fileThatRanAsMain).moduleObjectThatFailedAsMain.get();
    return canExposeModuleObject(m_fileThatRanAsMain) ? moduleObject(m_fileThatRanAsMain) : nullptr;
}

void Modules::forgetFileThatRanAsMain()
{
    if (std::exchange(m_didMainFail, false))
        ensureLoaded(m_fileThatRanAsMain).moduleObjectThatFailedAsMain.set(*this, nullptr);
    m_fileThatRanAsMain = ModuleID();
}

JSObject* Modules::mainModuleObject(ModuleID main)
{
    ModuleID file = fileOf(main);
    if (!data(file).isEntryPoint() && !m_setsMainModulePath)
        return nullptr;
    if (data(file).kind == ModuleKind::CommonJs)
        return canExposeModuleObject(file) && wasRequestedByLoader(file) ? moduleObject(file) : nullptr;
    bool hasModuleObjectFromRequire = m_setsMainModulePath && !data(file).isOfLibrary() && m_modulesMarkedAsRequired.get(file.index());
    return hasModuleObjectFromRequire ? moduleObjectMadeByRequire(file) : nullptr;
}

bool Modules::findWhetherExportsVariableIsTakenOffModuleObject(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    bool isCommonJSFile = data(module).kind == ModuleKind::CommonJs;
    JSObject* moduleObject = !loaded ? nullptr : isCommonJSFile ? loaded->requirer.get() : loaded->moduleObjectFromRequire.get();
    if (!moduleObject)
        return false;
    VM& vm = *m_instance.vm;
    if (isCommonJSFile && loaded->moduleObjectBoundToExports.get() == moduleObject && !loaded->wasModuleObjectMaterialized && !isMocked(module) && data(module).hasValue() && !data(module).hasUnescapedExports()) {
        JSValue laid = moduleObject->getDirect(vm, vm.propertyNames->exports);
        if (!laid || !laid.isCustomGetterSetter()) {
            loaded->isExportsVariableTakenOffModuleObject = true;
            if (exportsGivenBackToCompiledCode(module) != loaded->exportsWhenVariableAndModuleObjectAgreed.get())
                loaded->isOrderOfStoreAndDeedUnknown = true;
            loaded->idOfStructureOfModuleObjectLastLookedAt = StructureID();
            loaded->structureOfModuleObjectLastLookedAt.set(*this, nullptr);
            return !loaded->isOrderOfStoreAndDeedUnknown;
        }
        loaded->exportsWhenVariableAndModuleObjectAgreed.set(*this, exportsGivenBackToCompiledCode(module));
    }
    loaded->isExportsVariableTakenOffModuleObject = false;
    loaded->isOrderOfStoreAndDeedUnknown = false;
    bool isRemembered = !isMocked(module) && !moduleObject->structure()->isDictionary();
    loaded->idOfStructureOfModuleObjectLastLookedAt = isRemembered ? moduleObject->structureID() : StructureID();
    loaded->structureOfModuleObjectLastLookedAt.set(*this, isRemembered ? moduleObject->structure() : nullptr);
    return false;
}

void Modules::didMaterializeModuleObject(ModuleID module)
{
    ensureLoaded(module).wasModuleObjectMaterialized = true;
}

void Modules::noteExportsStoredThroughModuleObject(ModuleID module, JSValue exports)
{
    if (LoadedModule* loaded = loadedIfExists(module))
        loaded->exportsWhenVariableAndModuleObjectAgreed.set(*this, exports);
}

void Modules::readExportsOffModuleObjectIntoVariable(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSObject* moduleObject = loadedIfExists(module)->requirer.get();
    JSValue exports = moduleObject->get(m_instance.globalObject, m_instance.vm->propertyNames->exports);
    RETURN_IF_EXCEPTION(scope, void());
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->isExportsVariableTakenOffModuleObject || loaded->requirer.get() != moduleObject)
        return;
    setValue(module, exports);
    loaded->exportsWhenVariableAndModuleObjectAgreed.set(*this, exports);
}

NEVER_INLINE void Modules::lookAtExportsOfModuleObject(ModuleID module, RequireResolver resolver)
{
    if (findWhetherExportsVariableIsTakenOffModuleObject(module) && resolver == RequireResolver::CompiledCode)
        readExportsOffModuleObjectIntoVariable(module);
}

template<Modules::RequireResolver resolver>
void Modules::recordRequire(ModuleID target, ModuleID referrer, bool isFromCreatedRequire, JSObject* staleRequirerObject, bool isAfterLoad)
{
    if constexpr (resolver == RequireResolver::CompiledCode) {
        if (m_loadsModulesIndividually) [[unlikely]] {
            recordVariantRequest(target);
            if (data(target).kind == ModuleKind::CommonJs && !m_modulesWithKnownParent.get(target.index()) && !isMockPending(target))
                noteEntryParent(target, referrer);
        }
        if (m_mocks) [[unlikely]] {
            recordEntryRequest(target, referrer, LoadKind::Require);
            restoreOwnFactoryOriginal(target);
            bool isAfterRequire = m_prerecordedRequireTarget == target || m_completedRequireTarget == target;
            m_completedRequireTarget = ModuleID();
            m_prerecordedRequireTarget = isAfterRequire ? ModuleID() : target;
            m_prerecordedRequirer = isAfterRequire ? ModuleID() : referrer;
        }
        if (auto* willLoad = m_mocks || m_hasBegunTestFile ? host().willLoad : nullptr) [[unlikely]]
            willLoad(m_instance.globalObject, referrer ? fileOf(referrer) : ModuleID(), fileOf(target), LoadKind::Require);
    }
    const ModuleData& targetData = data(target);
    bool isCommonJSFile = targetData.kind == ModuleKind::CommonJs;
    if (resolver == RequireResolver::CompiledCode && m_activeMockCount) [[unlikely]]
        noteRequestOfActiveMock(target);
    if (resolver == RequireResolver::CompiledCode && m_modulesReleasedByLoader.get(target.index()) && !hasInstances(target)) [[unlikely]]
        bringBackWhatLoaderLetGoBelow(target);
    if (resolver == RequireResolver::CompiledCode && m_hasReloadableModules && targetData.isReloadable()) [[unlikely]]
        noteRequireDuringPatch(target);
    if (resolver == RequireResolver::CompiledCode && isCommonJSFile && !m_mockedFilesWithReleasedRequirers.isEmpty()) [[unlikely]]
        tellIfMockedFileWasKept(target);
    if (resolver == RequireResolver::CompiledCode && m_hasReloadableModules && recordsRequireEdges(referrer) && (!staleRequirerObject || requirerIndex(referrer) != referrer.index())) [[unlikely]]
        noteRequireEdge(target, referrer);
    if (!isCommonJSFile)
        markNamespaceAsRequired(target);
    if (!m_showsWaysBetweenModuleObjects || !(isFileOfProgram(target) || targetData.kind == ModuleKind::Data) || targetData.isOtherFormOfFile())
        return;
    if (isCommonJSFile)
        noteRequireOfFile(target);
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (isCommonJSFile && status(target) <= ModuleStatus::Linked) {
        LoadedModule* required = loadedIfExists(target);
        JSObject* failedModuleObject = required ? required->failedModuleObject.get() : nullptr;
        if (failedModuleObject) [[unlikely]] {
            detachFailedModuleObject(target);
            RETURN_IF_EXCEPTION(scope, void());
            if (referrer == target)
                required->firstUncompiledRequirer.set(*this, failedModuleObject);
        }
    }
    auto isAnsweredByLoader = [&] {
        return targetData.kind == ModuleKind::SourceText || targetData.requireResult == RequireResult::Namespace || (targetData.requireResult == RequireResult::NamespaceIfImported && isHeldByLoader(target));
    };
    bool isChildOfOtherObject = isFromCreatedRequire && data(referrer).kind == ModuleKind::CommonJs;
    if (staleRequirerObject) [[unlikely]] {
        detachFailedModuleObject(referrer);
        RETURN_IF_EXCEPTION(scope, void());
        uint64_t staleRequirerPair = 1ull << 62 | (static_cast<uint64_t>(referrer.index()) + 1) << 32 | target.index();
        bool isLeftUnlisted = m_recordedRequires.remove(staleRequirerPair);
        if (!isAfterLoad) {
            LoadedModule* required = loadedIfExists(target);
            bool isFirstESModuleRequire = isAnsweredByLoader() && (isLeftUnlisted || (!firstRequirerOf(target) && !(required && required->firstUncompiledRequirer)));
            recordRequireFromUncompiledCode(target, staleRequirerObject);
            if (!isFirstESModuleRequire && !isEvicted(target))
                return;
            m_recordedRequires.set(staleRequirerPair, isFirstESModuleRequire ? RecordedRequire::BegunFirstESModuleRequire : RecordedRequire::BegunForEvictedModule);
            if (m_recordedRequireIndex)
                m_recordedRequireIndex->addRequire(staleRequirerPair);
            return;
        }
        if (isLeftUnlisted || isChildOfOtherObject || !host().appendModuleChildren)
            return;
        JSObject* objectOfRequired = moduleObjectOrPlaceholder(target);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, host().appendModuleChildren(m_instance.globalObject, staleRequirerObject, objectOfRequired));
    }
    uint64_t pair = static_cast<uint64_t>(isChildOfOtherObject) << 63 | (static_cast<uint64_t>(referrer.index()) + 1) << 32 | target.index();
    auto recorded = m_recordedRequires.add(pair, isEvicted(target) ? RecordedRequire::BegunForEvictedModule : RecordedRequire::Begun);
    if (recorded.isNewEntry) {
        if (m_recordedRequireIndex) [[unlikely]]
            m_recordedRequireIndex->addRequire(pair);
        findWhetherExportsVariableIsTakenOffModuleObject(referrer);
        LoadedModule* required = loadedIfExists(target);
        if (firstRequirerOf(target) || (required && (required->firstUncompiledRequirer || (required->isCreatedByRequireCache && required->moduleObjectFromRequire))) || (isCommonJSFile && wasRequestedByLoader(target)))
            return;
        ensureWaysTo(target).firstRequirer = referrer;
        bool isListed = required && std::exchange(required->hasStaleFirstRequire, false) && filesFirstRequiredBy(referrer).contains(target);
        if (!isListed) {
            m_firstRequires.append({ referrer, target });
            if (m_recordedRequireIndex) [[unlikely]]
                m_recordedRequireIndex->addFirstRequire(referrer, target);
        }
        if (isAnsweredByLoader())
            recorded.iterator->value = RecordedRequire::BegunFirstESModuleRequire;
        return;
    }
    switch (recorded.iterator->value) {
    case RecordedRequire::BegunFirstESModuleRequire:
        if (status(target) == ModuleStatus::Failed)
            return;
        recorded.iterator->value = RecordedRequire::ReturnedAndNotListed;
        return;
    case RecordedRequire::ReturnedAndNotListed:
        recorded.iterator->value = isEvicted(target) ? RecordedRequire::BegunForEvictedModule : RecordedRequire::Begun;
        return;
    case RecordedRequire::Begun:
    case RecordedRequire::BegunForEvictedModule: {
        if (status(target) == ModuleStatus::Failed && !isListedBeforeItHasRun(target))
            return;
        bool isListedByHost = recorded.iterator->value == RecordedRequire::BegunForEvictedModule;
        recorded.iterator->value = RecordedRequire::Returned;
        if (!isListedByHost && !isChildOfOtherObject)
            ensureWaysTo(referrer).requiredModules.append(target);
        findWhetherExportsVariableIsTakenOffModuleObject(referrer);
        lookAtExportsOfModuleObject(target, resolver);
        RETURN_IF_EXCEPTION(scope, void());
        if (LoadedModule* required = loadedIfExists(target); required && required->isBeingRequired && data(referrer).kind == ModuleKind::CommonJs && canExposeModuleObject(referrer)) [[unlikely]] {
            moduleObject(referrer);
            RETURN_IF_EXCEPTION(scope, void());
        }
        LoadedModule* requirer = loadedIfExists(referrer);
        if (requirer && requirer->requirer && host().appendModuleChildren && !isChildOfOtherObject)
            RELEASE_AND_RETURN(scope, host().appendModuleChildren(m_instance.globalObject, requirer->requirer, nullptr));
        return;
    }
    case RecordedRequire::Returned: {
        LoadedModule* requirer = loadedIfExists(referrer);
        JSObject* objectOfRequired = moduleObjectIfCreated(target);
        if (objectOfRequired && !hidesModuleObject(target) && objectOfRequired->structureID() != loadedIfExists(target)->idOfStructureOfModuleObjectLastLookedAt) [[unlikely]] {
            lookAtExportsOfModuleObject(target, resolver);
            RETURN_IF_EXCEPTION(scope, void());
            requirer = loadedIfExists(referrer);
            objectOfRequired = moduleObjectIfCreated(target);
        }
        if (requirer && requirer->requirer && objectOfRequired && !isEvicted(target) && host().appendModuleChildren && !isChildOfOtherObject) {
            host().appendModuleChildren(m_instance.globalObject, requirer->requirer, objectOfRequired);
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

template void Modules::recordRequire<Modules::RequireResolver::CompiledCode>(ModuleID, ModuleID, bool, JSObject*, bool);
template void Modules::recordRequire<Modules::RequireResolver::Runtime>(ModuleID, ModuleID, bool, JSObject*, bool);

bool Modules::isCreatedRequireFunction(JSValue value)
{
    auto* bound = dynamicDowncast<JSBoundFunction>(value);
    return bound && bound->boundThis().isObject();
}

JSObject* Modules::findStaleRequirerObject(ModuleID file, JSValue moduleObjectOrRequire)
{
    JSObject* object = moduleObjectOrRequire.getObject();
    if (isCreatedRequireFunction(moduleObjectOrRequire))
        return uncheckedDowncast<JSBoundFunction>(object)->boundThis().getObject();
    bool isOfLatestRun = !object || object == moduleObjectIfCreated(file);
    return isOfLatestRun ? nullptr : object;
}

void Modules::recordRequireFromUncompiledCode(ModuleID target, JSObject* requirerModuleObject)
{
    bool isCommonJSFile = data(target).kind == ModuleKind::CommonJs;
    if (!m_showsWaysBetweenModuleObjects || !(isFileOfProgram(target) || data(target).kind == ModuleKind::Data) || data(target).isOtherFormOfFile())
        return;
    if (isCommonJSFile)
        noteRequireOfFile(target);
    if (!canExposeModuleObject(target))
        return;
    LoadedModule& required = ensureLoaded(target);
    if (firstRequirerOf(target) || required.firstUncompiledRequirer || (required.isCreatedByRequireCache && required.moduleObjectFromRequire) || (isCommonJSFile && wasRequestedByLoader(target)))
        return;
    required.firstUncompiledRequirer.set(*this, requirerModuleObject);
}

bool Modules::wasRequestedByLoader(ModuleID file)
{
    return status(file) > ModuleStatus::Linked || wasStartedByWalk(file);
}

void Modules::forgetRequestByLoader(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (data(file).kind == ModuleKind::CommonJs && status(file) <= ModuleStatus::Linked && !isEvicted(file))
        discardModuleObject(file);
}

void Modules::layValueInPlaceOfExportsVariable(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    if (!loaded || !loaded->requirer || !data(file).hasValue())
        return;
    VM& vm = *m_instance.vm;
    JSValue laid = loaded->requirer->getDirect(vm, vm.propertyNames->exports);
    if (laid && laid.isCustomGetterSetter())
        loaded->requirer->putDirect(vm, vm.propertyNames->exports, commonJSExports(file));
}

void Modules::discardModuleObject(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    if (loaded)
        loaded->standInForModuleObject.set(*this, nullptr);
    if (!loaded || !loaded->requirer)
        return;
    layValueInPlaceOfExportsVariable(file);
    loaded->requirer.set(*this, nullptr);
    loaded->moduleObjectBoundToExports.set(*this, nullptr);
    loaded->exportsWhenVariableAndModuleObjectAgreed.set(*this, JSValue());
    loaded->structureOfModuleObjectLastLookedAt.set(*this, nullptr);
    loaded->idOfStructureOfModuleObjectLastLookedAt = StructureID();
    loaded->isExportsVariableTakenOffModuleObject = false;
    loaded->isOrderOfStoreAndDeedUnknown = false;
    loaded->requireFunction.set(*this, JSValue());
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
    VMState& state = m_instance.vm->aotState();
    if (hostLoadsSynchronously(file)) {
        state.filesWithKnownFirstLoad.clear(file.index());
        return true;
    }
    if (!state.filesWithKnownFirstLoad.set(file.index()))
        state.filesFirstLoadedSynchronously.set(file.index(), m_filesLoadedByRequire.get(file.index()) || m_instance.vm->synchronousModuleQueue());
    return state.filesFirstLoadedSynchronously.get(file.index());
}

bool Modules::isFetchedInsideCall(ModuleID file) const
{
    if (hostLoadsSynchronously(file))
        return true;
    return m_instance.vm->aotState().filesWhoseTextHostKeeps.get(file.index()) && host().hasSourceCache && host().hasSourceCache(m_instance.globalObject);
}

bool Modules::isOfKindWhoseTextHostKeeps(ModuleID file) const
{
    if (m_instance.vm->aotState().filesStartedByHost.get(file.index()) || m_table.dependsOnMoreThanSource(file))
        return false;
    ModuleKind kind = data(file).kind;
    return kind == ModuleKind::SourceText || kind == ModuleKind::CommonJs || m_dataFilesThatHostPrintsAsJavaScript.get(file.index());
}

bool Modules::delegatesToStartingHost(ModuleID module)
{
    m_instance.vm->aotState().filesStartedByHost.set(fileOf(module).index());
    if (data(module).kind == ModuleKind::CommonJs || (hasInstances(module) && canReleaseLoads(module)))
        m_testFilesHoldingLoads.set(module.index());
    if (m_hasBegunTestFile)
        m_startMoments.set(fileOf(module), m_instance.vm->aotState().momentOfLoads);
        m_evictedTestFiles.clear(fileOf(module).index());
    return isEvicted(module);
}

void Modules::noteTextsThatHostKeeps()
{
    if (std::exchange(m_hasNotedTextsThatHostKeeps, true))
        return;
    VMState& state = m_instance.vm->aotState();
    for (size_t index = 0; index < m_storedStatuses.size(); ++index) {
        if (m_storedStatuses[index] <= static_cast<uint8_t>(StoredModuleStatus::Linked)) [[likely]]
            continue;
        ModuleID file = fileOf(ModuleID(static_cast<uint32_t>(index)));
        if (m_filesWhoseTextHostDropped.get(file.index()))
            continue;
        state.filesWhoseTextHostKeeps.set(file.index());
        bool isUnderMock = isMocked(file) || m_modulesWithActiveMock.get(file.index()) || m_modulesWithPendingMock.get(file.index()) || m_originalsEvaluatedUnderMock.get(file.index());
        if (!isEvicted(file) && isOfKindWhoseTextHostKeeps(file) && !isUnderMock)
            state.filesWhoseKeptTextWasCompiled.set(file.index());
    }
}

bool Modules::hostLoadsSynchronously(ModuleID file) const
{
    return host().loadsSynchronously && host().loadsSynchronously(m_instance.globalObject, file);
}

void Modules::forgetFirstLoad(ModuleID module)
{
    ModuleID file = fileOf(module);
    m_instance.vm->aotState().filesWhoseTextHostKeeps.clear(file.index());
    m_instance.vm->aotState().filesWhoseKeptTextWasCompiled.clear(file.index());
    m_instance.vm->aotState().modulesWithHostCachedSource.clear(file.index());
    m_filesWhoseTextHostDropped.set(file.index());
    if (m_instance.vm->aotState().filesWithKnownFirstLoad.clear(file.index()))
        m_filesWhoseNextLoadIsFirst.set(file.index());
}

void Modules::willLoadCopyFromDisk(ModuleID module, LoadKind way)
{
    ModuleID file = fileOf(module);
    m_instance.vm->aotState().requesterRejectsUncompiledCode = false;
    m_filesWhoseTextHostDropped.clear(file.index());
    if (!m_filesWhoseNextLoadIsFirst.clear(file.index()))
        return;
    VMState& state = m_instance.vm->aotState();
    state.filesWithKnownFirstLoad.set(file.index());
    state.filesFirstLoadedSynchronously.set(file.index(), way == LoadKind::Require || m_instance.vm->synchronousModuleQueue() || hostLoadsSynchronously(file));
}

JSObject* Modules::parentModuleObject(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (loaded && loaded->firstUncompiledRequirer && !m_filesStartedAsEntryPoint.get(module.index()))
        return loaded->firstUncompiledRequirer;
    std::optional<ModuleID> firstRequirer = firstRequirerOf(module);
    if (!firstRequirer || m_filesStartedAsEntryPoint.get(module.index()))
        return nullptr;
    return hidesModuleObject(*firstRequirer) ? moduleObjectOrPlaceholder(*firstRequirer) : moduleObject(*firstRequirer);
}

bool Modules::forEachNewChild(ModuleID module, const ScopedLambda<void(JSObject*)>& functor)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    WaysToModule* ways = waysToIfRecorded(module);
    if (!ways)
        return true;
    while (ways->numberOfChildrenShown < ways->requiredModules.size()) {
        unsigned place = ways->numberOfChildrenShown;
        ModuleID child = ways->requiredModules[place];
        if (data(child).isESModuleOfProgram() && status(child) <= ModuleStatus::Linked && !activeMock(child)) {
            ways->numberOfChildrenShown = place + 1;
            continue;
        }
        JSObject* childObject = moduleObjectOrPlaceholder(child);
        RETURN_IF_EXCEPTION(scope, true);
        if (ways->numberOfChildrenShown != place || place >= ways->requiredModules.size() || ways->requiredModules[place] != child)
            continue;
        ways->numberOfChildrenShown = place + 1;
        functor(childObject);
        RETURN_IF_EXCEPTION(scope, true);
    }
    return true;
}

void Modules::clearNewChildren(ModuleID module)
{
    if (WaysToModule* ways = waysToIfRecorded(module))
        ways->numberOfChildrenShown = ways->requiredModules.size();
}

JSValue Modules::requireSpecifier(JSValue specifier, ModuleID referrer, NamedByBuild namedByBuild, JSObject* staleRequirerObject)
{
    bool isNamedByBuild = namedByBuild != NamedByBuild::No;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    if (isNamedByBuild)
        admitCodeNamedBy(specifier);
    else
        willLoadByName(realm, specifier);
    RETURN_IF_EXCEPTION(scope, { });
    JSObject* requirer = moduleObject(referrer);
    RETURN_IF_EXCEPTION(scope, { });
    SetForScope isInHostRequest(m_currentHostRequest, namedByBuild == NamedByBuild::CompiledModule ? HostRequest(true) : isNamedByBuild ? HostRequest::forUncompiledModule() : this->isInHostRequest() ? m_currentHostRequest : HostRequest());
    SetForScope requesterRejectsUncompiledCode(m_instance.vm->aotState().requesterRejectsUncompiledCode, !isNamedByBuild && moduleRejectsUncompiledCode(referrer));
    auto recordRestoredReference = makeScopeExit([&] {
        if (m_hostRequestRequirerObject)
            m_storedReferences.recordAll();
    });
    SetForScope hostRequestRequirerObject(m_hostRequestRequirerObject, staleRequirerObject);
    if (staleRequirerObject)
        m_storedReferences.recordAll();
    RELEASE_AND_RETURN(scope, host().requireSpecifier(realm, requirer, specifier));
}

JSValue Modules::resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm form)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSObject* holder = form == ResolvedSpecifierForm::URL ? importMeta(referrer) : moduleObject(referrer);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, host().resolveSpecifier(m_instance.globalObject, holder, specifier, form));
}

void Modules::willRequireESModule(ModuleID module)
{
    if (m_modulesMarkedAsRequired.get(module.index()))
        return;
    if (namespaceIfCreated(module, ModulePhase::Evaluation))
        return;
    if (data(module).isImportedAsNamespace() && !isNotLoaded(module))
        return;
    m_modulesWithExtensibleNamespace.set(module.index());
}

void Modules::markNamespaceAsRequired(ModuleID module)
{
    if (!data(module).hasExportTable() || m_modulesMarkedAsRequired.get(module.index()))
        return;
    willRequireESModule(module);
    m_modulesMarkedAsRequired.set(module.index());
    ModuleStatus current = status(module);
    bool isMarkDeferred = current != ModuleStatus::Evaluated && (current != ModuleStatus::Evaluating || data(module).hasTopLevelAwait());
    m_modulesWithDeferredMark.set(module.index(), isMarkDeferred);
    auto* namespaceObject = uncheckedDowncast<ModuleNamespace>(namespaceIfCreated(module, ModulePhase::Evaluation));
    if (!namespaceObject || namespaceObject->hasExport(m_instance.vm->propertyNames->__esModule.impl()))
        return;
    if (isMarkDeferred)
        namespaceObject->setMarkerDeferred(true);
    else
        namespaceObject->setESModuleMarker(true);
}

void Modules::clearDeferredRequireMark(ModuleID module)
{
    if (!m_modulesWithDeferredMark.get(module.index()) || status(module) == ModuleStatus::Evaluated)
        return;
    m_modulesWithDeferredMark.clear(module.index());
    m_modulesMarkedAsRequired.clear(module.index());
    if (auto* namespaceObject = uncheckedDowncast<ModuleNamespace>(namespaceIfCreated(module, ModulePhase::Evaluation)))
        namespaceObject->setMarkerDeferred(false);
}

void Modules::clearRejectedFirstRequire(ModuleID required)
{
    std::optional<ModuleID> requirer = firstRequirerOf(required);
    LoadedModule* loaded = loadedIfExists(required);
    bool hasUncompiledFirstRequirer = !requirer && loaded && loaded->firstUncompiledRequirer;
    if (!requirer && !hasUncompiledFirstRequirer)
        return;
    bool hadNotReturned = m_recordedRequires.removeIf([&](const auto& recorded) {
        return static_cast<uint32_t>(recorded.key) == required.index() && recorded.value == RecordedRequire::BegunFirstESModuleRequire;
    });
    if (!hadNotReturned)
        return;
    if (hasUncompiledFirstRequirer) {
        loaded->firstUncompiledRequirer.set(*this, nullptr);
        return;
    }
    forgetFirstRequirerOf(required);
    m_recordedRequireIndex = nullptr;
    m_firstRequires.removeAllMatching([&](const FirstRequire& first) {
        return first.requirer == *requirer && first.required == required;
    });
}

JSObject* Modules::namespaceForRequire(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation, false);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (auto* names = uncheckedDowncast<ModuleNamespace>(namespaceObject); !names->hasExport(vm.propertyNames->__esModule.impl()))
        names->setESModuleMarker(true);
    return namespaceObject;
}

JSValue Modules::whatRequireMakesOfNamespace(JSObject* namespaceObject)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_moduleExportsName.isNull())
        m_moduleExportsName = Identifier::fromString(vm, "module.exports"_s);
    JSValue esModule = readExportForRequire(namespaceObject, vm.propertyNames->__esModule);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue moduleExports;
    if (esModule) {
        moduleExports = readExportForRequire(namespaceObject, m_moduleExportsName);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return !moduleExports || moduleExports.isUndefinedOrNull() ? JSValue(namespaceObject) : moduleExports;
}

JSValue Modules::readExportForRequire(JSObject* namespaceObject, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto* compiledNamespace = uncheckedDowncast<ModuleNamespace>(namespaceObject);
    std::optional<unsigned> index = compiledNamespace->find(name.impl());
    if (!index && !compiledNamespace->hasExport(name.impl()))
        return jsUndefined();

    Environment* instance = compiledNamespace->bindModuleInstance();
    RETURN_IF_EXCEPTION(scope, { });
    JSValue exported = index ? readExport(compiledNamespace->module(), *index, MockMode::Use, instance) : namespaceObject->get(m_instance.globalObject, name);
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, { });
        return { };
    }
    return exported;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

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
#include "AOTRuntimeHooks.h"
#include "AOTRuntimeReporter.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTStubEntrypoints.h"
#include "AOTVMState.h"
#include "AsyncContextSwapScope.h"
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "GlobalObjectMethodTable.h"
#include "JSBoundFunction.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
#include "MicrotaskQueueInlines.h"
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
        }
        if (moduleStatus == ModuleStatus::Failed)
            throwEvaluationError(module);
        if ((moduleStatus == ModuleStatus::Evaluated || moduleStatus == ModuleStatus::EvaluatingAsync) && !frames.isEmpty())
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
    SetForScope isStatementHostRequest(m_isStatementHostRequest, false);
    if (!moduleData.isOfLibrary())
        didStartThread(&vm);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_activeMockCount && m_rootWhoseFilesRun != std::optional(module)) [[unlikely]] {
        runOriginalsOwedTo(module);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!m_formsAnsweredFirst.isEmpty()) [[unlikely]] {
        if (moduleData.kind == ModuleKind::Data) {
            bool hasTaken = takeWhatFormAnsweredFirstHolds(module);
            RETURN_IF_EXCEPTION(scope, { });
            if (hasTaken)
                return jsUndefined();
        }
        giveStatementsFormsThatLoaderHas(module);
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

auto Modules::noteModulesThatMayBeginUnder(uint8_t mark, ModuleID root) -> Vector<ModuleThatMayBegin, 8>
{
    if (m_marksOfEvaluationsUnderOthers.isEmpty())
        m_marksOfEvaluationsUnderOthers.fill(0, m_table.size());
    Vector<ModuleThatMayBegin, 8> found;
    auto note = [&](ModuleID module) {
        uint8_t& noted = m_marksOfEvaluationsUnderOthers[module.index()];
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

JSValue Modules::callBodyUnderOwnMark(ModuleID module, bool bodyStoresStatus, bool runsImportedFilesAlone)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ASSERT(!bodyStoresStatus || data(module).evaluatesItself());
    if (std::optional<ModuleID> file = m_table.moduleBehindFacade(module)) {
        detachFailedModuleObject(*file);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto releasesSteps = makeScopeExit([&] {
        if (!m_deferredLinkSteps.isEmpty()) [[unlikely]]
            resumeDeferredLinkSteps(m_table.moduleBehindFacade(module).value_or(module));
    });
    SetForScope rootWhoseFilesRun(m_rootWhoseFilesRun, runsImportedFilesAlone ? std::optional(module) : std::nullopt);
    SetForScope wordOfCompiledCode(m_instance.modules.runsImportedFilesAlone, runsImportedFilesAlone ? 1u : 0u);

    bool isInRecord = bodyStoresStatus && storedStatus(module) == StoredModuleStatus::InRecord;
    if (isInRecord)
        setStoredStatus(module, StoredModuleStatus::Linked);
    uint32_t outerMark = m_instance.modules.evaluatingMark;
    uint8_t mark = static_cast<uint8_t>(std::min<uint32_t>(outerMark + 1, UINT8_MAX));
    bool isUnderAnotherEvaluation = outerMark != static_cast<uint32_t>(StoredModuleStatus::Evaluating);
    Vector<ModuleThatMayBegin, 8> mayBeginHere;
    if (isUnderAnotherEvaluation && bodyStoresStatus) [[unlikely]]
        mayBeginHere = noteModulesThatMayBeginUnder(mark, module);
    m_instance.modules.evaluatingMark = mark;
    if (ModuleID file = m_table.moduleBehindFacade(module).value_or(module); data(file).isImportableDuringEvaluation()) [[unlikely]] {
        ensureLoaded(file).synchronousLoadCountAtStart = m_synchronousLoadCount;
        ensureLoaded(file).beganInsideSynchronousLoad = m_activeSynchronousLoad;
    }
    JSValue result = callBody(module);
    m_instance.modules.evaluatingMark = outerMark;
    for (auto [left, markBefore] : mayBeginHere) {
        if (hasNotBegun(left))
            m_marksOfEvaluationsUnderOthers[left.index()] = markBefore;
    }
    Exception* exception = scope.exception();
    if (!exception) [[likely]] {
        if (isInRecord)
            setStoredStatus(module, StoredModuleStatus::InRecord);
        return result;
    }

    Vector<ModuleID, 16> beingEvaluated;
    std::span<const uint8_t> statuses = m_storedStatuses;
    for (size_t index = WTF::find(statuses, mark); index != notFound; index = WTF::find(statuses, mark, index + 1))
        beingEvaluated.append(ModuleID(static_cast<uint32_t>(index)));
    ModulesThatWaitForCycles haveRun;
    for (ModuleID failed : beingEvaluated) {
        if (isOnCycleOfImports(failed))
            lookAtCycleOf(failed, haveRun);
    }
    if (isInRecord)
        setStoredStatus(module, StoredModuleStatus::InRecord);
    for (ModuleID failed : beingEvaluated) {
        if (!isInRecord || failed != module)
            didThrowInBody(failed, exception->value());
    }
    for (size_t index : haveRun.modules) {
        if (ModuleID failed(static_cast<uint32_t>(index)); markOfEvaluationThatBegan(failed) == mark)
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

bool Modules::hasSynchronousLoadFetched(ModuleID file)
{
    std::optional<ModuleID> facade = m_table.facadeOf(file);
    if (!m_activeSynchronousLoad || !facade)
        return false;
    for (EagerLoad* load = m_innermostEagerLoad; load; load = load->outer) {
        std::optional<ModuleID> running = load->runsBeforeLoaderSteps ? evaluatingModuleOrFacade(*load) : std::nullopt;
        if (running && fileOf(*running) != file && wasRequestedByLoad(*load, file))
            return true;
    }
    if (!m_instance.vm->synchronousModuleQueue())
        return false;
    ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(*facade));
    return entry && entry->status() == ModuleRegistryEntry::Status::Fetching && !entry->record();
}

bool Modules::tryDiscardFailureDuringFetch(ModuleID file)
{
    std::optional<ModuleID> facade = data(file).kind == ModuleKind::CommonJs ? m_table.facadeOf(file) : std::nullopt;
    ModuleRegistryEntry* entry = facade ? loader()->registryEntry(moduleKey(*facade)) : nullptr;
    if (!entry || entry->status() != ModuleRegistryEntry::Status::Fetching)
        return false;
    discardFailedEvaluation(file);
    recordIfExists(file)->setEvaluationError(*m_instance.vm, JSValue());
    return true;
}

bool Modules::takeThrowAfterFetch(ModuleID moduleOrFacade)
{
    LoadedModule* loaded = loadedIfExists(fileOf(moduleOrFacade));
    if (!loaded || !loaded->throwAfterFetchLoad) [[likely]]
        return false;
    uint32_t load = std::exchange(loaded->throwAfterFetchLoad, 0);
    return load == m_outermostSynchronousLoad && fileOf(moduleOrFacade) != moduleOrFacade && hasNotBegunAsFile(fileOf(moduleOrFacade));
}

void Modules::makeRecordWithoutExports(ModuleID facade)
{
    captureNoExports(fileOf(facade));
    ensureLoaded(facade).didDropError = true;
    setStatus(facade, ModuleStatus::Evaluated);
}

void Modules::discardFailedEvaluation(ModuleID file)
{
    saveFailedModuleObject(file);
    discardModuleObject(file);
    forgetTemplateObjects(file);
    if (LoadedModule* loaded = loadedIfExists(file)) {
        loaded->requiredValue.set(*this, JSValue());
        loaded->firstUncompiledRequirer.set(*this, nullptr);
        loaded->synchronousLoadCountAtStart = std::nullopt;
        loaded->hasStaleFirstRequire = true;
    }
    forgetWhatFileRequired(file);
    if (WaysToModule* ways = waysToIfRecorded(file))
        ways->firstRequirer = std::nullopt;
    for (uint64_t key : recordedRequireIndex().byTarget.take(file.index()))
        m_recordedRequires.remove(key);
    m_pendingFirstRequireTargets.removeAll(file);
    setStatus(file, ModuleStatus::Linked);
}

bool Modules::isAwaitedByImportFromBeforeItRan(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    return isAwaitedByImport(file) && !(loaded && loaded->isAwaitedByCallThatComesInLaterJob);
}

bool Modules::isAwaitedByImport(ModuleID file)
{
    for (ModuleID moduleOrFacade : { file, m_table.facadeOf(file).value_or(file) }) {
        if (m_modulesOfQueuedImports.get(moduleOrFacade.index()) || m_loadsInFlight.hasRoot(moduleOrFacade) || std::ranges::contains(batchedFetches(), moduleOrFacade, &BatchedFetch::module))
            return true;
    }
    return false;
}

void Modules::didThrowInBody(ModuleID module, JSValue error)
{
    if (m_originalsBeingEvaluated.get(module.index())) [[unlikely]]
        return;
    auto forgetMainModule = makeScopeExit([&] {
        if (m_mainCommonJSModule == module)
            resetFailedMainModule();
    });
    if (data(module).kind == ModuleKind::CommonJs && m_instance.vm->hasPendingTerminationException()) [[unlikely]] {
        setStatus(module, ModuleStatus::Evaluated);
        return;
    }
    if (data(module).kind == ModuleKind::CommonJs && (mayBeEvicted(module) || (data(module).runsAgainAfterThrow() && !isAwaitedByImportFromBeforeItRan(module))) && status(module) == ModuleStatus::Evaluating && !loaderHasFileThatThrew(module)) [[unlikely]] {
        if (mayBeEvicted(module))
            evict(module);
        else {
            if (hasSynchronousLoadFetched(module))
                ensureLoaded(module).throwAfterFetchLoad = m_outermostSynchronousLoad;
            if (LoadedModule* loaded = loadedIfExists(module); loaded && !loaded->wasFetchedDuringEvaluation)
                loaded->wasLoadedDuringEvaluation = false;
            discardFailedEvaluation(module);
        }
        return;
    }
    if (data(module).cachesEvaluationError())
        setFailed(module, error);
    else
        setStatus(module, ModuleStatus::Evaluated);
    if (isCommonJSModuleOrFacade(module) && wasImportedDuringEvaluation(m_table.moduleBehindFacade(module).value_or(module)))
        dropErrorOfFile(module);
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
    if (std::ranges::none_of(members, [&](uint32_t member) { return m_storedStatuses[member] > evaluating; }))
        return;

    Vector<uint8_t> evaluationOf(members.size(), [&](size_t place) -> uint8_t {
        ModuleID member(members[place]);
        uint8_t stored = m_storedStatuses[member.index()];
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
        const Vector<uint8_t>& evaluationOf;

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
        std::optional<ModuleID> importer = m_rootWhoseFilesRun;
        if (!importer && m_innermostEvaluation && !m_innermostEvaluation->frames.isEmpty())
            importer = m_innermostEvaluation->frames.last().module;
        if (importer && isDisplacedRunKept(module, *importer))
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
        rejectRequireInSweep(module);
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
    if (m_modulesWithPendingMock.get(module.index()) && !(isMockLingering(module) && isEvaluatedByLoader(module))) [[unlikely]] {
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
        if (!m_innermostEagerLoad)
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
        scope.release();
        loadEvictedFile(module);
        return false;
    }
    if (isRunningFilesAheadOfLoader(module))
        return false;
    ModuleStatus moduleStatus = status(module);
    if (moduleStatus == ModuleStatus::Failed && tryDiscardFailureDuringFetch(module)) [[unlikely]]
        moduleStatus = status(module);
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

void Modules::forEachEvaluatingCommonJSModule(const Invocable<void(ModuleID)> auto& functor) const
{
    forEachEvaluatingModuleOrFacade([&](ModuleID moduleOrFacade) {
        functor(fileOf(moduleOrFacade));
    });
}

void Modules::forEachEvaluatingModuleOrFacade(const Invocable<void(ModuleID)> auto& functor) const
{
    for (const EagerLoad* load = m_innermostEagerLoad; load; load = load->outer) {
        if (std::optional<ModuleID> moduleOrFacade = evaluatingModuleOrFacade(*load))
            functor(*moduleOrFacade);
    }
}

bool Modules::isBegunForStatement(ModuleID moduleOrFacade) const
{
    ModuleID file = fileOf(moduleOrFacade);
    if (moduleOrFacade != file)
        return true;
    std::optional<ModuleID> facade = m_table.facadeOf(file);
    return facade && status(*facade) == ModuleStatus::Evaluating;
}

std::optional<ModuleID> Modules::evaluatingModuleOrFacade(const EagerLoad& load) const
{
    auto isBeingEvaluated = [&](ModuleID moduleOrFacade) {
        return status(moduleOrFacade) == ModuleStatus::Evaluating || status(fileOf(moduleOrFacade)) == ModuleStatus::Evaluating;
    };
    if (load.moduleOrFacade)
        return isBeingEvaluated(*load.moduleOrFacade) ? load.moduleOrFacade : std::nullopt;
    for (const ModuleRequestData& request : m_table.requests(load.root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSModuleOrFacade(imported)) {
            if (endsFilesThatRunAhead(request))
                break;
            continue;
        }
        if (isBeingEvaluated(imported))
            return imported;
    }
    return std::nullopt;
}

bool Modules::shouldDeferAsyncLoad(ModuleID moduleOrFacade) const
{
    return !m_instance.vm->synchronousModuleQueue() && isEvaluatedByLoader(fileOf(moduleOrFacade));
}

bool Modules::isEvaluatedByLoader(ModuleID file) const
{
    bool found = false;
    forEachEvaluatingCommonJSModule([&](ModuleID evaluated) {
        found |= evaluated == file;
    });
    return found;
}

bool Modules::anyImportedModule(ModuleID root, EvaluatedModules evaluatedModules, const Invocable<bool(ModuleID)> auto& predicate) const
{
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<uint32_t, 32> found { root.index() };
    for (size_t index = 0; index < found.size(); ++index) {
        ModuleID module(found[index]);
        if (predicate(module))
            return true;
        if (!data(module).isESModuleOfProgram() || (evaluatedModules == EvaluatedModules::Skip && status(module) == ModuleStatus::Evaluated))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (!request.isRequire && !request.isHoisted && !seen.quickSet(request.target()))
                found.append(request.target());
        }
    }
    return false;
}

bool Modules::importsEagerlyEvaluatingModule(ModuleID root) const
{
    Vector<ModuleID, 4> running;
    forEachEvaluatingCommonJSModule([&](ModuleID file) {
        running.append(file);
    });
    return !running.isEmpty() && anyImportedModule(root, EvaluatedModules::Visit, [&](ModuleID module) {
        return running.contains(fileOf(module));
    });
}

bool Modules::importsEvaluatingCommonJSModule(ModuleID root) const
{
    return anyImportedModule(root, EvaluatedModules::Skip, [&](ModuleID module) {
        return isCommonJSModuleOrFacade(module) && (status(module) == ModuleStatus::Evaluating || status(fileOf(module)) == ModuleStatus::Evaluating);
    });
}

bool Modules::isLinkStepBlocked(const VM::SynchronousModuleTask& step, std::optional<ModuleID> finishedModule) const
{
    if (static_cast<JSPromise::Status>(step.payload) != JSPromise::Status::Fulfilled)
        return false;
    auto* compiled = dynamicDowncast<ModuleRecord>(step.task == InternalMicrotask::ModuleLoadLinkEvaluateSettled ? step.arg1 : step.arg0);
    if (!compiled)
        return false;
    ModuleID root = compiled->module();
    if (root == m_innermostSyncLoadRoot)
        return false;
    return anyImportedModule(root, EvaluatedModules::Skip, [&](ModuleID module) {
        if (module == root || !isCommonJSModuleOrFacade(module))
            return false;
        ModuleID file = fileOf(module);
        if (file == finishedModule || status(file) != ModuleStatus::Evaluating)
            return false;
        LoadedModule* loaded = loadedIfExists(file);
        return !loaded || !loaded->wasLoadedDuringEvaluation;
    });
}

bool Modules::tryDeferLinkStep(const VM::SynchronousModuleTask& step)
{
    if (!isLinkStepBlocked(step, std::nullopt))
        return false;
    VM& vm = *m_instance.vm;
    m_deferredLinkSteps.append({ step.task, step.payload, { Strong<Unknown>(vm, step.arg0), Strong<Unknown>(vm, step.arg1), Strong<Unknown>(vm, step.arg2), Strong<Unknown>(vm, step.arg3) } });
    return true;
}

bool Modules::resumeDeferredLinkSteps(std::optional<ModuleID> finishedModule)
{
    VM& vm = *m_instance.vm;
    return m_deferredLinkSteps.removeAllMatching([&](const DeferredLinkStep& held) {
        VM::SynchronousModuleTask step { held.task, held.payload, held.arguments[0].get(), held.arguments[1].get(), held.arguments[2].get(), held.arguments[3].get() };
        if (isLinkStepBlocked(step, finishedModule))
            return false;
        if (VM::SynchronousModuleQueue* queue = vm.synchronousModuleQueue())
            queue->tasks.append(step);
        else
            m_instance.globalObject->queueMicrotask(vm, step.task, step.payload, step.arg0, step.arg1, step.arg2, step.arg3);
        return true;
    });
}

bool Modules::reachesEagerLoadGraph(EagerLoad& files, ModuleID required)
{
    auto forEachImportOfModuleOnItsWay = [&](ModuleID module, const Invocable<void(ModuleID)> auto& functor) {
        if (!isLoadedWithRequests(module) || !isNotLoaded(module))
            return;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (!request.isHoisted && !request.isRequire && !request.isDeferred)
                functor(request.targetModule());
        }
    };
    Vector<ModuleID, 32> modules;
    if (!files.modulesOfLoad.get(files.root.index())) {
        files.modulesOfLoad.set(files.root.index());
        modules.append(files.root);
        while (!modules.isEmpty()) {
            forEachImportOfModuleOnItsWay(modules.takeLast(), [&](ModuleID imported) {
                if (!files.modulesOfLoad.set(imported.index()))
                    modules.append(imported);
            });
        }
    }
    const uint32_t isSeen = takeMarkOfSearch();
    PlacesSeenByPlans& placesSeen = m_placesSeenByPlans;
    placesSeen[required.index()] = isSeen;
    modules.append(required);
    while (!modules.isEmpty()) {
        ModuleID module = modules.takeLast();
        if (files.modulesOfLoad.get(module.index()) && (hasNotBegunAsFile(module) || (isLoadedWithRequests(module) && isNotLoaded(module))))
            return true;
        forEachImportOfModuleOnItsWay(module, [&](ModuleID imported) {
            if (std::exchange(placesSeen[imported.index()], isSeen) != isSeen)
                modules.append(imported);
        });
    }
    return false;
}

bool Modules::importsFileBeingEvaluated(ModuleID root) const
{
    if (!m_innermostEagerLoad) [[likely]]
        return false;
    for (const ModuleRequestData& request : m_table.requests(root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSModuleOrFacade(imported)) {
            if (endsFilesThatRunAhead(request))
                return false;
            continue;
        }
        if (shouldDeferAsyncLoad(imported))
            return true;
    }
    return false;
}

bool Modules::hasKey(ModuleID file, const Identifier& key)
{
    std::optional<ModuleID> facade = m_table.facadeOf(file);
    return moduleKey(file) == key || (facade && moduleKey(*facade) == key);
}

std::optional<ModuleID> Modules::findEvaluatingCommonJSModule(const Identifier& key)
{
    if (!m_innermostEagerLoad || m_instance.vm->synchronousModuleQueue()) [[likely]]
        return std::nullopt;
    std::optional<ModuleID> found;
    forEachEvaluatingCommonJSModule([&](ModuleID file) {
        if (file != m_moduleBeingTransferred && hasKey(file, key))
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

bool Modules::hasDeferredLoads(ModuleID moduleOrFacade) const
{
    if (!hasDeferredLoads()) [[likely]]
        return false;
    ModuleID file = fileOf(moduleOrFacade);
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
    SetForScope isInHostRequest(m_currentHostRequest, true);
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

void Modules::resumeDeferredLoadsWithoutImporter(ModuleID moduleOrFacade)
{
    if (!hasDeferredLoads(moduleOrFacade)) [[likely]]
        return;
    ModuleID file = fileOf(moduleOrFacade);
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
    constexpr uint8_t jobsUntilLoaded = 4;
    resumeDeferredImports(file, jobsUntilLoaded);
    if (!m_deferredRequests.containsIf([&](const DeferredRequest& request) { return request.file == file; }))
        return;
    ModuleRecord* record = recordOfCommonJSFile(m_table.facadeOf(file).value_or(file));
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

void Modules::collectOuterRequests(EagerLoad& files)
{
    Vector<EagerLoad*, 8> loads;
    for (EagerLoad* load = &files; load->outer && !load->hasCollectedOuterRequests; load = load->outer)
        loads.append(load);
    for (EagerLoad* load : loads | std::views::reverse) {
        EagerLoad& outer = *load->outer;
        load->hasCollectedOuterRequests = true;
        load->requestedByOuterLoads = outer.requestedByOuterLoads;
        if (!outer.transferredModule && wasRequestedByLoad(outer, fileOf(outer.root)))
            load->requestedByOuterLoads.merge(outer.requested);
    }
}

bool Modules::wasRequestedByEnclosingLoad(const Identifier& key)
{
    EagerLoad* around = m_innermostEagerLoad;
    if (around && m_moduleBeingTransferred)
        around = around->outer;
    std::optional<ModuleID> asked = around ? findByKey(key) : std::nullopt;
    if (!asked)
        return false;
    if (!around->hasCollectedOuterRequests)
        collectOuterRequests(*around);
    ModuleID file = fileOf(*asked);
    return around->requestedByOuterLoads.get(file.index()) || (!around->transferredModule && wasRequestedByLoad(*around, file));
}

bool Modules::wasRequestedByOuterLoad(ModuleID module)
{
    EagerLoad& files = *m_innermostEagerLoad;
    if (!files.hasCollectedOuterRequests)
        collectOuterRequests(files);
    if (files.requestedByOuterLoads.get(fileOf(module).index()))
        return true;
    return isBeingFetched(module);
}

bool Modules::isFetchedSynchronously(ModuleID module) const
{
    return m_instance.vm->synchronousModuleQueue() && recordIfExists(module) && isBeingFetched(module);
}

bool Modules::isBeingFetched(ModuleID module) const
{
    ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(module));
    return entry && entry->status() == ModuleRegistryEntry::Status::Fetching && !entry->record();
}

Vector<uint32_t, 16> Modules::placesOfFilesAsSynchronousLoadMakesThem(ModuleID root, const Vector<uint32_t, 16>& places)
{
    enum class State : uint8_t { Unrequested, Requested, Fetched, Made, Recorded, Settled };
    enum class Job : uint8_t { SettleFetch, MakeRecord, StoreRecord, LoadRequests };
    UncheckedKeyHashMap<uint32_t, State, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> states;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lastWalks;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> ranks;
    auto stateOf = [&](ModuleID module) {
        if (auto found = states.find(module.index()); found != states.end())
            return found->value;
        bool isFile = isCommonJSModuleOrFacade(module);
        if (isFile ? !hasNotBegunAsFile(module) : !isNotLoaded(module) || isMocked(module))
            return isFile ? State::Recorded : State::Settled;
        return wasRequestedByOuterLoad(module) ? State::Requested : State::Unrequested;
    };
    Vector<std::pair<Job, ModuleID>, 64> jobs;
    Vector<ModuleID, 32> toWalk;
    Vector<ModuleID, 32> walked;
    std::span<const ModuleRequestData> requests = m_table.requests(root);
    constexpr uint32_t hasNoRank = std::numeric_limits<uint32_t>::max();
    for (uint32_t place : places)
        ranks.add(requests[place].target(), hasNoRank);
    uint32_t numberOfRanks = 0;
    auto make = [&](ModuleID file) {
        if (auto rank = ranks.find(file.index()); rank != ranks.end() && rank->value == hasNoRank)
            rank->value = numberOfRanks++;
    };
    states.set(root.index(), State::Recorded);
    jobs.append({ Job::LoadRequests, root });
    for (uint32_t next = 0; next < jobs.size() && numberOfRanks < places.size(); ++next) {
        auto [job, file] = jobs[next];
        State state = stateOf(file);
        if (job == Job::SettleFetch && state == State::Requested) {
            states.set(file.index(), State::Fetched);
            jobs.append({ Job::MakeRecord, file });
        } else if (job == Job::MakeRecord && state == State::Fetched) {
            states.set(file.index(), State::Made);
            make(file);
            jobs.append({ Job::StoreRecord, file });
        } else if (job == Job::StoreRecord && state == State::Made) {
            states.set(file.index(), State::Recorded);
            if (!isCommonJSModuleOrFacade(file))
                jobs.append({ Job::LoadRequests, file });
        } else if (job == Job::LoadRequests && state == State::Recorded) {
            bool isAllRecorded = true;
            toWalk.append(file);
            while (!toWalk.isEmpty()) {
                ModuleID importer = toWalk.takeLast();
                if (stateOf(importer) == State::Settled || std::exchange(lastWalks.add(importer.index(), 0).iterator->value, next + 1) == next + 1)
                    continue;
                walked.append(importer);
                for (const ModuleRequestData& request : m_table.requests(importer)) {
                    if (request.isHoisted || request.isRequire || request.isDeferred)
                        continue;
                    ModuleID target = request.targetModule();
                    bool isFile = isCommonJSModuleOrFacade(target);
                    if (!isFile && !isLoadedWithRequests(target))
                        continue;
                    switch (stateOf(target)) {
                    case State::Unrequested:
                        states.set(target.index(), State::Requested);
                        jobs.append({ Job::SettleFetch, target });
                        isAllRecorded = false;
                        break;
                    case State::Requested:
                        states.set(target.index(), State::Fetched);
                        jobs.append({ Job::MakeRecord, target });
                        isAllRecorded = false;
                        break;
                    case State::Fetched:
                        make(target);
                        [[fallthrough]];
                    case State::Made:
                        states.set(target.index(), State::Recorded);
                        if (isFile)
                            break;
                        jobs.append({ Job::LoadRequests, target });
                        toWalk.append(target);
                        break;
                    case State::Recorded:
                        if (!isFile)
                            toWalk.append(target);
                        break;
                    case State::Settled:
                        break;
                    }
                }
            }
            for (ModuleID importer : std::exchange(walked, { })) {
                if (isAllRecorded)
                    states.set(importer.index(), State::Settled);
            }
        }
    }
    Vector<uint32_t, 16> inOrder = places;
    AOT::stableSort(inOrder, { }, [&](uint32_t place) {
        return ranks.get(requests[place].target());
    });
    return inOrder;
}

bool Modules::evaluateImportedCommonJSModules(ModuleID root, EagerLoadJobPolicy jobs, ErrorOfImportedFile error, ModuleGraphLoadingState* load, PendingFactoryModules pendingFactoryModules)
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
        if (loadEndsEarly) {
            m_rootAwaitingLoader = root;
            return false;
        }
    }

    MocksInGraph mocks = mocksInGraph(root);

    bool didCaptureLibraryModules = false;
    for (const ModuleRequestData& request : m_table.requests(root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSModuleOrFacade(imported)) {
            if (endsFilesThatRunAhead(request))
                break;
            continue;
        }
        if (mocks.hides(imported)) [[unlikely]]
            continue;
        if (!std::exchange(didCaptureLibraryModules, true)) {
            captureExportsOfLibraryModules(root);
            RETURN_IF_EXCEPTION(scope, false);
        }
        if (data(imported).capturesExportsBehindEvaluation() && (m_loadsModulesIndividually || !isSettledForLinks(imported))) {
            captureExportsBehindEvaluation(imported);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }

    bool didRunFile = false;
    EagerLoad eagerLoad { root, std::nullopt, m_innermostEagerLoad, jobs == EagerLoadJobPolicy::DeferJobs, load };
    eagerLoad.runsBeforeLoaderSteps = jobs == EagerLoadJobPolicy::DeferLoaderSteps;
    eagerLoad.loadQueue = vm.synchronousModuleQueue();
    JSValue asyncContextOfLoad = load ? load->asyncContext() : JSValue();
    SetForScope innermostEagerLoad(m_innermostEagerLoad, &eagerLoad);
    SetForScope modulesWithEagerLoads(vm.aotState().modulesWithEagerLoads, eagerLoad.runsBeforeLoaderJobs ? this : vm.aotState().modulesWithEagerLoads);
    if (jobs == EagerLoadJobPolicy::DeferJobs && data(root).evaluatesItself() && data(root).isEntryPoint() && !doOtherJobsWait() && !m_rootWhoseFilesRun && !ModuleLoaderAsyncContextScope::wouldEnter(vm, asyncContextOfLoad)) {
        ensureRecord(root)->retain();
        if (storedStatus(root) == StoredModuleStatus::InRecord) {
            bool hadQueuedTicks = hasQueuedTicks();
            callBodyUnderOwnMark(root, false, true);
            didRunFile = true;
            if (eagerLoad.transferredModule) [[unlikely]] {
                ModuleID file = *eagerLoad.transferredModule;
                if (Exception* exception = scope.exception()) {
                    if (vm.isTerminationException(exception))
                        return true;
                    TRY_CLEAR_EXCEPTION(scope, true);
                }
                JSValue errorOfFile;
                if (status(file) == ModuleStatus::Failed) {
                    if (wasImportedDuringEvaluation(fileOf(file)))
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
                if (doOtherJobsWait()) {
                    transferGraphToLoader(eagerLoad, *failedFile);
                    RETURN_IF_EXCEPTION(scope, true);
                    if (eagerLoad.transferredModule) {
                        JSValue errorOfFile;
                        if (wasImportedDuringEvaluation(fileOf(*failedFile)))
                            dropErrorOfFile(*failedFile);
                        else
                            errorOfFile = exception->value();
                        finishStepInWhichFileRan(*failedFile, errorOfFile);
                        return true;
                    }
                    holdJobsBehind(0, *failedFile);
                }
                if (!wasImportedDuringEvaluation(m_table.moduleBehindFacade(*failedFile).value_or(*failedFile))) {
                    m_rootAwaitingLoader = root;
                    return true;
                }
                dropErrorOfFile(*failedFile);
            }
            if (m_moduleWithDeferredJobs || hasDeferredLoads()) {
                m_rootAwaitingLoader = root;
                return true;
            }
            if (!hadQueuedTicks && hasQueuedTicks()) [[unlikely]] {
                std::optional<ModuleID> lastFile;
                for (const ModuleRequestData& request : m_table.imports(root)) {
                    if (!request.isHoisted)
                        break;
                    if (isCommonJSModuleOrFacade(request.targetModule()) && status(request.targetModule()) == ModuleStatus::Evaluated)
                        lastFile = request.targetModule();
                }
                if (lastFile) {
                    transferGraphToLoader(eagerLoad, *lastFile);
                    RETURN_IF_EXCEPTION(scope, true);
                    if (eagerLoad.transferredModule) {
                        finishStepInWhichFileRan(*lastFile, JSValue());
                        return true;
                    }
                }
            }
        }
    }
    std::span<const ModuleRequestData> requests = m_table.requests(root);
    Vector<uint32_t, 16> placesOfFiles;
    for (uint32_t place = 0; jobs == EagerLoadJobPolicy::DeferLoaderSteps && place < requests.size() && requests[place].isHoisted; ++place) {
        if (hasNotBegunAsFile(requests[place].targetModule()))
            placesOfFiles.append(place);
    }
    Vector<uint32_t, 16> placesInOrder = placesOfFiles.size() > 1 ? placesOfFilesAsSynchronousLoadMakesThem(root, placesOfFiles) : placesOfFiles;
    size_t nextFile = 0;
    for (size_t place = 0; place < requests.size(); ++place) {
        size_t index = place;
        if (nextFile < placesOfFiles.size() && placesOfFiles[nextFile] == place)
            index = placesInOrder[nextFile++];
        const ModuleRequestData& request = requests[index];
        if (request.isHoisted && m_storedStatuses[request.target()] == static_cast<uint8_t>(StoredModuleStatus::Evaluated) && !activeMock(request.targetModule()))
            continue;
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSModuleOrFacade(imported)) {
            if (endsFilesThatRunAhead(request))
                break;
            continue;
        }
        if (mocks.hides(imported)) [[unlikely]]
            continue;
        if (isDisplacedRunKept(imported, root)) [[unlikely]]
            continue;
        if (ModuleID file = fileOf(imported); pendingFactoryModules == PendingFactoryModules::Skip && isMocked(file) && !activeMock(file) && pendingFactoryCallID(file)) [[unlikely]]
            continue;
        bool isOriginalDue = activeMock(imported) && hasBypassingImporterBelow(root, imported);
        ModuleStatus statusAtStart = status(imported);
        if (!isOriginalDue && statusAtStart != ModuleStatus::Unlinked && statusAtStart != ModuleStatus::Linked && statusAtStart != ModuleStatus::Failed)
            continue;
        if (takeThrowAfterFetch(imported)) [[unlikely]] {
            makeRecordWithoutExports(imported);
            continue;
        }
        didRunFile |= statusAtStart != ModuleStatus::Failed;
        size_t numberOfJobs = queuedJobCount(jobs);
        bool hadQueuedTicks = hasQueuedTicks();
        eagerLoad.moduleOrFacade = imported;
        eagerLoad.jobCountBeforeModule = numberOfJobs;
        std::optional<ModuleLoaderAsyncContextScope> asyncContextOfFile;
        if (jobs == EagerLoadJobPolicy::DeferJobs)
            asyncContextOfFile.emplace(vm, m_instance.globalObject, asyncContextOfLoad);
        if (!isOriginalDue) [[likely]] {
            if (ModuleID file = fileOf(imported); isMocked(file) && !activeMock(file) && hasBypassingImporterBelow(root, imported)) [[unlikely]] {
                evaluatePastPendingMock(imported);
                RETURN_IF_EXCEPTION(scope, didRunFile);
            } else if (storedStatus(imported) < StoredModuleStatus::InRecord && data(imported).evaluatesItself())
                evaluateByCallingBody(imported);
            else
                evaluateModuleInRecord(imported);
            isOriginalDue = !scope.exception() && activeMock(imported) && hasBypassingImporterBelow(root, imported);
        }
        if (isOriginalDue) [[unlikely]] {
            runFileBesideMock(fileOf(imported));
            RETURN_IF_EXCEPTION(scope, didRunFile);
        }
        asyncContextOfFile.reset();
        Exception* exception = scope.exception();
        bool didQueueJobsBeforeLoader = eagerLoad.runsBeforeLoaderJobs && (queuedJobCount(jobs) != numberOfJobs || (!hadQueuedTicks && hasQueuedTicks()));
        if (didQueueJobsBeforeLoader && !(exception && vm.isTerminationException(exception))) [[unlikely]] {
            TRY_CLEAR_EXCEPTION(scope, didRunFile);
            transferGraphToLoader(eagerLoad, imported);
            RETURN_IF_EXCEPTION(scope, didRunFile);
            if (exception && !eagerLoad.transferredModule)
                throwException(m_instance.globalObject, scope, exception);
        }
        if (eagerLoad.transferredModule) [[unlikely]] {
            JSValue errorOfFile;
            if (exception) {
                if (vm.isTerminationException(exception))
                    return didRunFile;
                TRY_CLEAR_EXCEPTION(scope, didRunFile);
                if (wasImportedDuringEvaluation(fileOf(imported)))
                    dropErrorOfFile(imported);
                else
                    errorOfFile = exception->value();
            }
            finishStepInWhichFileRan(imported, errorOfFile);
            return didRunFile;
        }
        bool hasDeferredLoads = this->hasDeferredLoads(imported);
        if (hasDeferredLoads) [[unlikely]] {
            numberOfJobs = 0;
            if (error == ErrorOfImportedFile::IsThrown) {
                resumeDeferredLoadsWithoutImporter(imported);
                exception = scope.exception();
                hasDeferredLoads = false;
            }
        }
        if (exception) [[unlikely]] {
            if (statusAtStart == ModuleStatus::Failed || vm.isTerminationException(exception) || !wasImportedDuringEvaluation(m_table.moduleBehindFacade(imported).value_or(imported))) {
                if (error == ErrorOfImportedFile::IsLeftToLoader && !vm.isTerminationException(exception)) {
                    TRY_CLEAR_EXCEPTION(scope, didRunFile);
                    if (jobs == EagerLoadJobPolicy::DeferLoaderSteps && imported != fileOf(imported) && status(imported) != ModuleStatus::Failed)
                        setFailed(imported, exception->value());
                    if (queuedJobCount(jobs) != numberOfJobs)
                        holdJobsBehind(numberOfJobs, imported, jobs);
                    m_rootAwaitingLoader = root;
                }
                return didRunFile;
            }
            TRY_CLEAR_EXCEPTION(scope, didRunFile);
            dropErrorOfFile(imported);
        }
        bool didQueueJobs = queuedJobCount(jobs) != numberOfJobs;
        if (!didQueueJobs && !hasDeferredLoads) [[likely]]
            continue;
        if (didQueueJobs)
            holdJobsBehind(numberOfJobs, imported, jobs);
        m_rootAwaitingLoader = root;
        return didRunFile;
    }
    if (didRunFile)
        m_rootsLoadedWithoutLoader.append(root);
    return didRunFile;
}

void Modules::holdJobsBehind(size_t numberOfJobs, ModuleID file, EagerLoadJobPolicy jobs)
{
    VM& vm = *m_instance.vm;
    m_moduleWithDeferredJobs = m_table.moduleBehindFacade(file).value_or(file);
    if (jobs == EagerLoadJobPolicy::DeferLoaderSteps) {
        ASSERT(!numberOfJobs);
        m_deferredLoaderSteps = vm.m_synchronousModuleQueue;
        return;
    }
    if (!m_deferredJobs)
        m_deferredJobs = MicrotaskQueue::create(vm);
    vm.defaultMicrotaskQueue().moveTasksAfter(numberOfJobs, *m_deferredJobs);
}

size_t Modules::queuedJobCount(EagerLoadJobPolicy jobs) const
{
    VM& vm = *m_instance.vm;
    switch (jobs) {
    case EagerLoadJobPolicy::KeepQueued:
        return 0;
    case EagerLoadJobPolicy::DeferJobs:
        return vm.defaultMicrotaskQueue().size();
    case EagerLoadJobPolicy::DeferLoaderSteps:
        return vm.m_synchronousModuleQueue->tasks.size();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::transferGraphsToLoader()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<EagerLoad*, 2> ahead;
    for (EagerLoad* files = m_innermostEagerLoad; files; files = files->outer) {
        if (files->runsBeforeLoaderJobs)
            ahead.append(files);
    }
    for (EagerLoad* files : ahead | std::views::reverse) {
        transferGraphToLoader(*files);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void Modules::transferGraphToLoader(EagerLoad& files, std::optional<ModuleID> finishedFile)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    files.runsBeforeLoaderJobs = false;
    std::optional<ModuleID> running = finishedFile ? finishedFile : evaluatingModuleOrFacade(files);
    if (!running || !files.load)
        return;

    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    Ref<MicrotaskQueue> jobsOfFile = MicrotaskQueue::create(vm);
    queue.moveTasksAfter(files.jobCountBeforeModule, jobsOfFile.get());
    m_canJobsOfFileTellWhenTextsCome |= !jobsOfFile->isEmpty() && isTestRun();
    m_rootAwaitingLoader = files.root;
    files.transferredModule = running;
    {
        DeferTerminationForAWhile deferTermination(vm);
        SetForScope moduleBeingTransferred(m_moduleBeingTransferred, fileOf(*running));
        SetForScope noSynchronousLoad(vm.m_suspendedSynchronousModuleQueue, vm.m_synchronousModuleQueue);
        ModuleRecord* record = recordIfExists(files.root);
        files.load->removeVisited(record);
        files.load->setDrainingInnerLoad(false);
        loader()->innerModuleLoading(m_instance.globalObject, files.load, record);
        record->didResumeLoadReentrantly(*files.load);
        scope.assertNoException();
        runStepsOfLoaderInFrontOf(fileOf(*running));
        scope.assertNoException();
    }
    jobsOfFile->moveTasksAfter(0, queue);
}

void Modules::transferSynchronousLoad(EagerLoad& files)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    files.runsBeforeLoaderSteps = false;
    if (!files.moduleOrFacade || !vm.synchronousModuleQueue() || !vm.synchronousModuleQueue()->tasks.isEmpty())
        return;
    ModuleID running = *files.moduleOrFacade;
    m_rootAwaitingLoader = files.root;
    files.transferredModule = running;
    DeferTerminationForAWhile deferTermination(vm);
    SetForScope moduleBeingTransferred(m_moduleBeingTransferred, fileOf(running));
    SetForScope isInHostRequest(m_currentHostRequest, true);
    takeUpWaysOfLoads();
    JSPromise* promise = loader()->loadModule(m_instance.globalObject, moduleKey(files.root), nullptr, nullptr, { ModuleLoadFlag::Evaluate, ModuleLoadFlag::Synchronous });
    scope.assertNoException();
    m_transferredSynchronousLoads.append({ files.root, Strong<JSPromise>(vm, promise) });
    runSynchronousStepsInFrontOf(fileOf(running));
    scope.assertNoException();
    VM::SynchronousModuleQueue* innermost = vm.synchronousModuleQueue();
    VM::SynchronousModuleQueue* queue = innermost->prev;
    while (queue && queue != files.loadQueue)
        queue = queue->prev;
    if (queue)
        queue->tasks.appendVector(std::exchange(innermost->tasks, { }));
}

void Modules::runSynchronousStepsInFrontOf(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& steps = vm.synchronousModuleQueue()->tasks;
    auto isUnfinishedCommonJSModule = [&](const Identifier& key) {
        std::optional<ModuleID> fetched = findByKey(key);
        return fetched && isCommonJSModuleOrFacade(*fetched) && status(fileOf(*fetched)) < ModuleStatus::Evaluated;
    };
    Vector<VM::SynchronousModuleTask, 2> unfinishedModuleSteps;
    size_t taken = 0;
    while (taken < steps.size()) {
        if (steps[taken].task == InternalMicrotask::ModuleLoadLinkEvaluateSettled)
            break;
        VM::SynchronousModuleTask step = steps[taken++];
        if (step.task == InternalMicrotask::ModuleRegistryFetchSettled) {
            const Identifier& key = uncheckedDowncast<ModuleRegistryEntry>(step.arg2)->key();
            if (hasKey(file, key))
                break;
            if (isUnfinishedCommonJSModule(key)) {
                unfinishedModuleSteps.append(step);
                continue;
            }
        }
        runInternalMicrotask(m_instance.globalObject, vm, step.task, step.payload, step.arg0, step.arg1, step.arg2, step.arg3);
        scope.assertNoException();
    }
    steps.removeAt(0, taken);
    steps.insertVector(0, unfinishedModuleSteps);
}

void Modules::runStepsOfLoaderInFrontOf(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    DeferTerminationForAWhile deferTermination(vm);
    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    Ref<MicrotaskQueue> turn = MicrotaskQueue::create(vm);
    Ref<MicrotaskQueue> unfinishedModuleSteps = MicrotaskQueue::create(vm);
    auto isUnfinishedCommonJSModule = [&](const Identifier& key) {
        std::optional<ModuleID> fetched = findByKey(key);
        return fetched && isCommonJSModuleOrFacade(*fetched) && status(fileOf(*fetched)) < ModuleStatus::Evaluated;
    };
    while (!queue.isEmpty()) {
        queue.moveTasksAfter(0, turn.get());
        size_t processedStepCount = 0;
        bool hasComeToFile = !turn->allOf([&](const QueuedTask& job) {
            if (job.globalObject()->microtaskRunnability() == QueuedTaskResult::Discard) {
                ++processedStepCount;
                return true;
            }
            auto arguments = job.arguments();
            if (job.job() == InternalMicrotask::ModuleRegistryFetchSettled) {
                const Identifier& key = uncheckedDowncast<ModuleRegistryEntry>(arguments[2])->key();
                if (hasKey(file, key))
                    return false;
                if (isUnfinishedCommonJSModule(key)) {
                    ++processedStepCount;
                    unfinishedModuleSteps->enqueue(QueuedTask(job));
                    return true;
                }
            }
            ++processedStepCount;
            runInternalMicrotask(job.globalObject(), vm, job.job(), job.payload(), arguments[0], arguments[1], arguments[2], arguments[3]);
            scope.assertNoException();
            return true;
        });
        if (hasComeToFile) {
            Ref<MicrotaskQueue> newlyQueuedTasks = MicrotaskQueue::create(vm);
            queue.moveTasksAfter(0, newlyQueuedTasks.get());
            unfinishedModuleSteps->moveTasksAfter(0, queue);
            turn->moveTasksAfter(processedStepCount + 1, queue);
            newlyQueuedTasks->moveTasksAfter(0, queue);
        }
        turn->clear();
        if (hasComeToFile)
            return;
    }
    unfinishedModuleSteps->moveTasksAfter(0, queue);
}

void Modules::finishStepInWhichFileRan(ModuleID moduleOrFacade, JSValue error)
{
    VM& vm = *m_instance.vm;
    ModuleID file = fileOf(moduleOrFacade);
    for (ModuleID requested : { m_table.facadeOf(file).value_or(file), file }) {
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

void RuntimeHooks::transferEagerLoadsToLoader(VM& vm)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    vm.aotState().modulesWithEagerLoads->transferGraphsToLoader();
    static_cast<void>(scope.exception());
}

bool Modules::stopLoadingBehindFile(ModuleID file)
{
    if (!m_rootWhoseFilesRun)
        return false;
    if (m_innermostEagerLoad && m_innermostEagerLoad->runsBeforeLoaderJobs)
        transferGraphToLoader(*m_innermostEagerLoad, file);
    if (m_innermostEagerLoad && m_innermostEagerLoad->transferredModule)
        return true;
    holdJobsBehind(0, file);
    return true;
}

void Modules::resumeDeferredJobs()
{
    if (!m_moduleWithDeferredJobs)
        return;
    m_moduleWithDeferredJobs = std::nullopt;
    if (VM::SynchronousModuleQueue* steps = std::exchange(m_deferredLoaderSteps, nullptr)) {
        if (steps != m_instance.vm->m_synchronousModuleQueue)
            m_instance.vm->m_synchronousModuleQueue->tasks.appendVector(std::exchange(steps->tasks, { }));
        return;
    }
    m_deferredJobs->moveTasksAfter(0, m_instance.vm->defaultMicrotaskQueue());
}

void Modules::didFailToLoadGraph(ModuleID root)
{
    m_loadsInFlight.didLoad(root);
    if (!m_levelsOfLoads.isEmpty()) [[unlikely]] {
        m_levelsOfLoads.removeIf([&](auto& entry) {
            return entry.value->plan->found[0].module == root;
        });
    }
    resumeGraphsDeferredForImports();
    if (m_rootAwaitingLoader != root)
        return;
    m_rootAwaitingLoader = std::nullopt;
    resumeDeferredJobs();
    resumeDeferredLoadsOfFinishedFiles();
}

void Modules::didLoadGraph(ModuleID root)
{
    if (m_rootAwaitingLoader == root) [[unlikely]] {
        m_rootAwaitingLoader = std::nullopt;
        resumeDeferredLoadsOfFinishedFiles();
    }
    m_loadsInFlight.didLoad(root);
    resumeGraphsDeferredForImports();
}

unsigned Modules::numberOfGraphsBeingLoaded()
{
    m_loadsInFlight.removeSettledIf([&](const LoadInFlight& load) {
        if (m_rootAwaitingLoader == load.root)
            m_rootAwaitingLoader = std::nullopt;
        return true;
    });
    bool hostLoadsGraph = m_rootAwaitingLoader && !m_loadsInFlight.hasRoot(*m_rootAwaitingLoader);
    unsigned number = hostLoadsGraph + m_loadsInFlight.numberBeingLoaded();
    m_canJobsOfFileTellWhenTextsCome &= !!number;
    return number;
}

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
    PlacesSeenByPlans& placesSeen = m_placesSeenByPlans;
    placesSeen[required.index()] = isSeen;
    Vector<ModuleID, 16> modules { required };
    while (!modules.isEmpty()) {
        ModuleID module = modules.takeLast();
        if (rootsThatWait.contains(module))
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
            if (isLoadedModuleWithoutRecord(target) && !rootsThatWait.contains(target))
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
    m_loadsInFlight.removeSettledIf([&](const LoadInFlight& earlier) {
        return m_rootAwaitingLoader != earlier.root;
    });
    m_loadsInFlight.append(*m_instance.vm, load, root, settling).loaderStepsAtCall = m_resumingCallLoaderSteps.value_or(m_loaderStepCount);
}

Modules::LoaderForRequire Modules::loaderForRequire(ModuleID module)
{
    if (!isLoadedWithRequests(module))
        return LoaderForRequire::IsNotNeeded;
    if (data(module).isLoadedByLoader() || statementThatEndsLoad(module))
        return LoaderForRequire::IsNeeded;
    if (numberOfGraphsBeingLoaded() && (m_rootAwaitingLoader || comesToLoadInFlight(module)))
        return LoaderForRequire::IsNeeded;
    for (const EagerLoad* files = m_innermostEagerLoad; files; files = files->outer) {
        if (files->moduleOrFacade == files->root)
            return LoaderForRequire::ComesBehindImportedFiles;
    }
    return LoaderForRequire::IsNotNeeded;
}

bool Modules::isNotLoaded(ModuleID module)
{
    if (!isLoadedWithRequests(module))
        return false;
    if (ModuleRecord* record = recordIfExists(module))
        return record->status() == CyclicModuleRecord::Status::New || record->status() == CyclicModuleRecord::Status::Unlinked;
    return storedStatus(module) == StoredModuleStatus::Unlinked && isNotLoadedWithoutStatus(module, data(module)) && !wasFetchedByFailedLoad(module);
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
    return stored == StoredModuleStatus::Unlinked && !moduleData.isOfLibrary() && isNotLoadedWithoutStatus(module, moduleData);
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
                found.append(Found { target, importer, laterRequests, isFile, isAliased || isCommonJSFile, isAliased, isAliased && (!isFile || isCommonJSFile), false, false });
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
    if (!isLoadedWithRequests(moduleOnWay) || isCommonJSModuleOrFacade(moduleOnWay)) {
        m_filesLoadedWithoutLoader.set(moduleOnWay.index());
        m_levelsOfLoads.remove(moduleOnWay);
    }
}

bool Modules::wasLoadedWithEarlierGraph(ModuleID module)
{
    if (!isCommonJSModuleOrFacade(module))
        return isLoadedWithRequests(module) && !isNotLoaded(module);
    if (m_rootAwaitingLoader || status(module) != ModuleStatus::Evaluated)
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

void Modules::noteLoadOfFile(ModuleID module, LoadOfFile load)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID file = m_table.moduleBehindFacade(module).value_or(module);
    if ((status(file) != ModuleStatus::Evaluating && status(module) != ModuleStatus::Evaluating) || shouldDeferAsyncLoad(file) || file == m_moduleBeingTransferred)
        return;
    bool isFetchedInsideCall = load == LoadOfFile::IsRootOfImport && (hostLoadsSynchronously(file) || (host().hasSourceCache && host().hasSourceCache(m_instance.globalObject)));
    if (load == LoadOfFile::IsRootOfImport && isFileOfProgram(file) && !m_instance.vm->synchronousModuleQueue() && !isFetchedInsideCall)
        return;
    ensureLoaded(file).wasLoadedDuringEvaluation = true;
    ensureLoaded(file).wasLoadedSynchronouslyDuringEvaluation |= !!m_instance.vm->synchronousModuleQueue();
    if (isFetchedInsideCall && !m_instance.vm->synchronousModuleQueue()) {
        ensureLoaded(file).wasFetchedDuringEvaluation = true;
        return;
    }
    if (!data(file).isImportableDuringEvaluation() || load == LoadOfFile::IsRequested)
        return;
    std::optional<ModuleID> facade = m_table.facadeOf(file);
    if (!facade) {
        if (!didCaptureExports(file))
            RELEASE_AND_RETURN(scope, captureExports(file));
        return;
    }
    std::optional<uint32_t> countAtStart = ensureLoaded(file).synchronousLoadCountAtStart;
    if (load != LoadOfFile::IsRootOfImport && countAtStart && m_activeSynchronousLoad > *countAtStart && data(*facade).capturesExports() && !didCaptureExports(*facade))
        RELEASE_AND_RETURN(scope, captureExports(*facade));
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
    ModuleID file = m_table.moduleBehindFacade(module).value_or(module);
    bool hasBegun = status(module) >= ModuleStatus::Evaluating || status(file) >= ModuleStatus::Evaluating;
    bool hasTurn = !m_instance.vm->synchronousModuleQueue() && !isFetchedInsideCall(file) && isAskedForBehindLoadInFlight(module);
    bool isModuleToStatement = load == LoadOfFile::IsRequested && data(file).isImportedAsESModule() && !m_instance.vm->synchronousModuleQueue();
    if (((load == LoadOfFile::IsRootOfImport && hasTurn) || isModuleToStatement) && !hasBegun && !record->runsWhenLoaded())
        record->setRunsInItsTurn();
    else
        record->setRunsWhenLoaded();
    return record;
}

bool Modules::isAskedForBehindLoadInFlight(ModuleID module)
{
    ModuleID file = m_table.moduleBehindFacade(module).value_or(module);
    unsigned numberOfOwnLoads = 0;
    auto countOwnLoad = [&](LoadInFlight& load) {
        numberOfOwnLoads += m_loadsInFlight.isBeingLoaded(load);
    };
    m_loadsInFlight.forEachOfRoot(module, countOwnLoad);
    if (file != module)
        m_loadsInFlight.forEachOfRoot(file, countOwnLoad);
    return m_loadsInFlight.numberBeingLoaded() > numberOfOwnLoads;
}

void Modules::runFileThatWaitsForItsTurn(ModuleID module)
{
    if (data(m_table.moduleBehindFacade(module).value_or(module)).isImportedAsESModule())
        return;
    for (ModuleID held : { module, m_table.moduleBehindFacade(module).value_or(module) }) {
        if (ModuleRecord* record = recordIfExists(held); record && record->takeRunsInItsTurn()) [[unlikely]]
            return evaluateFilesOfLevel(held, true);
    }
}

void Modules::evaluateLoadedCommonJSFile(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isCommonJSModuleOrFacade(module))
        return;
    if (m_moduleWithDeferredJobs && m_moduleWithDeferredJobs == m_table.moduleBehindFacade(module).value_or(module)) [[unlikely]]
        resumeDeferredJobs();
    ModuleStatus statusAtStart = status(module);
    if (statusAtStart == ModuleStatus::Evaluating)
        RELEASE_AND_RETURN(scope, noteLoadOfFile(module));
    if (LoadedModule* loaded = loadedIfExists(fileOf(module)); loaded && std::exchange(loaded->wasFetchedDuringEvaluation, false) && status(fileOf(module)) != ModuleStatus::Evaluated) {
        for (ModuleID moduleOrFacade : { fileOf(module), module }) {
            if (status(moduleOrFacade) != ModuleStatus::Failed)
                setStatus(moduleOrFacade, ModuleStatus::Evaluated);
            dropErrorOfFile(moduleOrFacade);
        }
        return;
    }
    if (takeThrowAfterFetch(module)) [[unlikely]]
        return makeRecordWithoutExports(module);
    noteLoadOfFile(module);
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, void());
    if (statusAtStart == ModuleStatus::Failed && loader()->isMakingModuleInline(moduleKey(module)))
        return;
    if (statusAtStart != ModuleStatus::Unlinked && statusAtStart != ModuleStatus::Linked && statusAtStart != ModuleStatus::Failed)
        return;
    {
        EagerLoad eagerLoad { module, module, m_innermostEagerLoad };
        SetForScope innermostEagerLoad(m_innermostEagerLoad, &eagerLoad);
        m_loadsInFlight.didRunFile();
        bool shouldBypassPendingMock = false;
        if (isMocked(fileOf(module)) && !activeMock(fileOf(module))) [[unlikely]] {
            for (LoadInFlight& load : m_loadsInFlight)
                shouldBypassPendingMock |= hasBypassingImporterBelow(load.root, module);
        }
        if (shouldBypassPendingMock) [[unlikely]]
            evaluatePastPendingMock(module);
        else if (storedStatus(module) < StoredModuleStatus::InRecord && data(module).evaluatesItself())
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
        bool takesExportsOfFile = data(module).kind == ModuleKind::CommonJs ? !m_table.facadeOf(module) : data(module).capturesExports();
        if (takesExportsOfFile && !didCaptureExports(module))
            RELEASE_AND_RETURN(scope, captureExports(module));
        return;
    }
    if (statusAtStart == ModuleStatus::Failed || vm.isTerminationException(exception) || !wasImportedDuringEvaluation(m_table.moduleBehindFacade(module).value_or(module)))
        return;
    TRY_CLEAR_EXCEPTION(scope, void());
    dropErrorOfFile(module);
}

void Modules::didDiscardModuleError(const Identifier& key)
{
    std::optional<ModuleID> module = findByKey(key);
    if (!module || !isCommonJSModuleOrFacade(*module))
        return;
    ModuleID file = fileOf(*module);
    ensureLoaded(file).wasReachedBySecondLoad = true;
    if (std::optional<ModuleID> facade = m_table.facadeOf(file))
        dropErrorOfFile(*facade);
    dropErrorOfFile(file);
}

void Modules::dropErrorOfFile(ModuleID moduleOrFacade)
{
    ModuleID file = m_table.moduleBehindFacade(moduleOrFacade).value_or(moduleOrFacade);
    if (data(file).isImportableDuringEvaluation())
        ensureLoaded(file).isAnsweredWithNamespace = true;
    captureNoExports(file);
    if (status(moduleOrFacade) != ModuleStatus::Failed)
        return;
    recordIfExists(moduleOrFacade)->setEvaluationError(*m_instance.vm, JSValue());
    setStatus(moduleOrFacade, ModuleStatus::Evaluated);
    ensureLoaded(moduleOrFacade).didDropError = true;
}

void Modules::captureNoExports(ModuleID file)
{
    bool hasEmptyRecord = ensureLoaded(file).wasReachedBySecondLoad;
    for (ModuleID module : { m_table.facadeOf(file).value_or(file), file }) {
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

bool Modules::wasImportedDuringEvaluation(ModuleID file) const
{
    if (isEvicted(file))
        return false;
    if (LoadedModule* loaded = loadedIfExists(file); loaded && (loaded->wasLoadedDuringEvaluation || loaded->wasReachedBySecondLoad))
        return true;
    return std::ranges::any_of(m_table.importsOfHeldFile(file), [&](const ImportOfHeldFileData& import) {
        return status(ModuleID(import.importer)) > ModuleStatus::Linked;
    });
}

bool Modules::loaderHasFileThatThrew(ModuleID file)
{
    if (isEvaluatedByLoader(file) || wasLoadedByImport(file))
        return true;
    LoadedModule* loaded = loadedIfExists(file);
    return loaded && (loaded->wasLoadedSynchronouslyDuringEvaluation || loaded->wasReachedBySecondLoad);
}

void Modules::evaluateCommonJSForLoader(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::optional<ModuleID> facade = m_table.facadeOf(file);
    bool hadCapturedExports = facade && didCaptureExports(*facade);
    evaluateCommonJS(file);
    Exception* exception = scope.exception();
    if (!exception) [[likely]] {
        if (facade) {
            m_filesLoadedWithoutLoader.set(facade->index());
            bool wasImportedBack = !hadCapturedExports && didCaptureExports(*facade);
            evaluateLoadedCommonJSFile(*facade);
            RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, void());
            if (wasImportedBack) [[unlikely]]
                RELEASE_AND_RETURN(scope, copyExportsOfFinishedFile(*facade));
        }
        return;
    }
    if (vm.isTerminationException(exception) || !wasImportedDuringEvaluation(file))
        return;
    TRY_CLEAR_EXCEPTION(scope, void());
    dropErrorOfFile(file);
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

    evaluateImportedCommonJSModules(root);
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
    RELEASE_AND_RETURN(scope, JSPromise::resolvedPromise(realm, jsUndefined()));
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
        if (status(target) == ModuleStatus::Unlinked || status(target) == ModuleStatus::Linked)
            path.append(Importer { m_table.requests(target) });
    }
}

void Modules::leaveStepsOfLoaderToJobs()
{
    auto& steps = m_instance.vm->m_synchronousModuleQueue->tasks;
    for (const VM::SynchronousModuleTask& step : steps)
        m_instance.globalObject->queueMicrotask(*m_instance.vm, step.task, step.payload, step.arg0, step.arg1, step.arg2, step.arg3);
    steps.shrink(0);
}

static JSC_DECLARE_HOST_FUNCTION(detachedLoadEvaluatedJob);
static JSC_DECLARE_HOST_FUNCTION(evictedLoadJob);
static JSC_DECLARE_HOST_FUNCTION(rootsDueBehindImportJob);
static JSC_DECLARE_HOST_FUNCTION(removedMockLoadJob);
static JSC_DECLARE_HOST_FUNCTION(detachedFactorySettledJob);
static JSC_DECLARE_HOST_FUNCTION(detachedFactoryFailedJob);
static JSC_DECLARE_HOST_FUNCTION(loaderWaitFinishedJob);

JSC_DEFINE_HOST_FUNCTION(loaderWaitFinishedJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didFinishLoaderWait(callFrame->argument(1).asUInt32());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(removedMockLoadJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->finishRemovedMockLoad(callFrame->argument(1).asUInt32());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(evictedLoadJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->finishEvictedLoad(callFrame->argument(1).asUInt32());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(detachedLoadEvaluatedJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didEvaluateDetachedLoad(callFrame->argument(1).asUInt32());
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
        bool hasFailedBelow = m_detachedLoads[left].awaitedFactoryCalls.containsIf([&](const auto& call) { return !activeMock(call.first); });
        bool isLoaderEvaluating = std::ranges::all_of(m_detachedLoads[left].awaitedFactoryCalls, [&](const auto& call) { return m_awaitedGeneratedModules.get(call.first.index()); });
        if (!hasFailedBelow && !isLoaderEvaluating && !std::exchange(m_detachedLoads[left].hasWaitedForLoader, true)) {
            unsigned loaderEvaluationJobs = 5;
            for (const auto& call : m_detachedLoads[left].awaitedFactoryCalls)
                loaderEvaluationJobs = std::max(loaderEvaluationJobs, 4 + loaderStepsToMock(required, call.first));
            JSGlobalObject* realm = m_instance.globalObject;
            JSPromise* turn = JSPromise::resolvedPromise(realm, jsUndefined());
            RETURN_IF_EXCEPTION(scope, void());
            for (unsigned job = 1; job < loaderEvaluationJobs; ++job) {
                JSPromise* next = JSPromise::create(vm, realm->promiseStructure());
                turn->performPromiseThen(vm, realm, jsUndefined(), jsUndefined(), next);
                turn = next;
            }
            ++m_detachedLoads[left].pendingFactoryCount;
            JSFunction* didSettle = JSFunction::create(vm, realm, 2, String(), detachedFactorySettledJob, ImplementationVisibility::Private);
            turn->performPromiseThenWithContext(vm, realm, didSettle, didSettle, jsUndefined(), jsNumber(number));
            return;
        }
        RELEASE_AND_RETURN(scope, evaluateDetachedOriginal(left));
    }
    BitVector modulesToRetry = std::exchange(m_detachedLoads[left].modulesToRetry, BitVector());
    for (size_t file : modulesToRetry)
        clearLoadErrorsForMock(ModuleID(file));
    bool factoryHasNotSettled;
    {
        SetForScope resumingDetachedLoad(m_resumingDetachedLoad, std::optional(required));
        SetForScope modulesBeingRetried(m_modulesBeingRetried, &modulesToRetry);
        factoryHasNotSettled = runMocksInGraph(required);
        left = find();
        bool wasEvaluatedByRequire = !scope.exception() && left != notFound && m_detachedLoads[left].wasEvaluatedByRequire;
        for (size_t file : modulesToRetry) {
            if (!wasEvaluatedByRequire || scope.exception() || !m_modulesWithPendingMock.get(file))
                continue;
            factoryHasNotSettled |= runPendingMock(ModuleID(file));
        }
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
    if (left == notFound)
        return;
    if (m_detachedLoads[left].pendingFactoryCount)
        return;
    ModuleStatus current = status(required);
    bool wasMockLaidSince = false;
    for (size_t fetched : m_detachedLoads[left].unmockedModules)
        wasMockLaidSince |= isMocked(ModuleID(fetched));
    bool someMockBelowIsNotInPlace = false;
    bool isOverRemovedMock = false;
    for (size_t below : staticImportClosure(required, MockTraversal::Stop)) {
        bool isServedByLingeringMock = isMockLingering(ModuleID(below)) && !m_modulesWhoseFactoryWasCalled.get(below);
        someMockBelowIsNotInPlace |= (m_modulesWithPendingMock.get(below) && !isServedByLingeringMock) || (isMocked(ModuleID(below)) && !activeMock(ModuleID(below)));
        isOverRemovedMock |= isServedByLingeringMock && m_modulesWithPendingMock.get(below) && wasOriginalEvaluated(ModuleID(below));
    }
    if ((current == ModuleStatus::Unlinked || current == ModuleStatus::Linked) && !m_detachedLoads[left].wasEvaluatedByRequire && !isEvicted(required) && !isMocked(required) && !wasMockLaidSince && !someMockBelowIsNotInPlace) {
        if (isOverRemovedMock && m_detachedLoads[left].wasAwaitedMockRemoved && host().didRunOriginal) [[unlikely]]
            host().didRunOriginal(m_instance.globalObject, m_table.fullPath(fileOf(required)), OriginalRun::RunsAmongLoads);
        m_detachedLoads[left].isEvaluationDue = true;
        if (m_detachedLoads[left].pendingLoaderWaits)
            return;
    }
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

void Modules::evaluateDetachedLoad(unsigned number)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.number == number; });
    if (left == notFound || !std::exchange(m_detachedLoads[left].isEvaluationDue, false))
        return;
    Vector<ModuleID, 4> evictedImporters = unevaluatedEvictedImporters(m_detachedLoads[left]);
    for (ModuleID root : evaluationOrderedRoots(m_detachedLoads[left])) {
        if (status(root) != ModuleStatus::Unlinked && status(root) != ModuleStatus::Linked)
            continue;
        JSPromise* evaluation = evaluate(root, std::nullopt, nullptr);
        if (Exception* failure = scope.exception()) {
            if (vm.isTerminationException(failure) || !scope.tryClearException())
                return;
        } else if (evaluation)
            evaluation->markAsHandled();
    }
    BitVector requested = evictedImporters.isEmpty() ? BitVector() : modulesRequestedByImports(false);
    for (ModuleID importer : evictedImporters) {
        if (m_modulesToEvictOnceLoaded.contains(importer))
            continue;
        if (requested.get(importer.index()))
            m_modulesToEvictOnceLoaded.append(importer);
        else
            tryReleaseModule(importer);
    }
}

bool Modules::isEvictedModuleImported()
{
    auto leaves = [&](ModuleID root) { return m_modulesToEvictOnceLoaded.contains(root); };
    if (m_factoryBlockedImportRoots.containsIf(leaves) || m_importsAwaitingSource.containsIf([&](const ImportAwaitingSource& each) { return leaves(each.module); }))
        return true;
    for (const BatchedFetch& fetch : batchedFetches()) {
        if (leaves(fetch.module))
            return true;
    }
    for (const BatchedFetch& fetch : m_callsToMakeAgain) {
        if (leaves(fetch.module))
            return true;
    }
    for (const QueuedFetch& fetch : m_queuedFetches) {
        if (leaves(fetch.module))
            return true;
    }
    return false;
}

JSC_DEFINE_HOST_FUNCTION(rootsDueBehindImportJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->evaluateRootsDueBehind(ModuleID(callFrame->argument(1).asUInt32()));
    return JSValue::encode(jsUndefined());
}

void Modules::didEvaluateForLoader(ModuleID imported)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    JSFunction* job = JSFunction::create(vm, realm, 2, String(), rootsDueBehindImportJob, ImplementationVisibility::Private);
    JSPromise* turn = JSPromise::create(vm, realm->promiseStructure());
    turn->fulfill(vm, jsUndefined());
    turn->performPromiseThenWithContext(vm, realm, job, job, jsUndefined(), jsNumber(imported.index()));
}

void Modules::evaluateRootsDueBehind(ModuleID imported)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<ModuleID, 1> due;
    m_rootsDueBehindImports.removeAllMatching([&](ModuleID root) {
        if (!staticImportClosure(root, MockTraversal::Stop).get(imported.index()))
            return false;
        due.append(root);
        return true;
    });
    for (ModuleID root : due) {
        if ((status(root) != ModuleStatus::Unlinked && status(root) != ModuleStatus::Linked) || isEvicted(root) || isMocked(root))
            continue;
        JSPromise* evaluation = evaluate(root, std::nullopt, nullptr);
        if (Exception* failure = scope.exception()) {
            if (vm.isTerminationException(failure) || !scope.tryClearException())
                return;
        } else if (evaluation)
            evaluation->markAsHandled();
    }
}

void Modules::finishDetachedLoad(unsigned number)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto find = [&] { return m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; }); };
    size_t left = find();
    if (left == notFound)
        return;
    bool wasMockReplaced = m_detachedLoads[left].wasEndedByLaterMock && m_detachedLoads[left].awaitedFactoryCalls.containsIf([&](const auto& call) { return isMockLingering(call.first); });
    if (wasMockReplaced && isEvictedModuleImported()) {
        for (ModuleID root : evaluationOrderedRoots(m_detachedLoads[left])) {
            if (!m_rootsDueBehindImports.contains(root))
                m_rootsDueBehindImports.append(root);
        }
        restoreRequiredValue(m_detachedLoads[left].required);
        m_detachedLoads[left].wait->fulfill(vm, jsUndefined());
        return;
    }
    ModuleID required = m_detachedLoads[left].required;
    evaluateDetachedLoad(number);
    RETURN_IF_EXCEPTION(scope, void());
    if (!m_modulesToEvictOnceLoaded.isEmpty()) [[unlikely]] {
        finishDetachedLoadsUnder(required);
        RETURN_IF_EXCEPTION(scope, void());
    }
    left = find();
    if (left == notFound)
        return;
    restoreRequiredValue(m_detachedLoads[left].required);
    m_detachedLoads[left].wait->fulfill(vm, jsUndefined());
    evictLoadedModules(wasMockReplaced);
}

Vector<ModuleID, 4> Modules::unevaluatedEvictedImporters(const DetachedLoad& load) const
{
    Vector<ModuleID, 4> importers;
    if (load.mocksRemovedSince.isEmpty())
        return importers;
    for (size_t index : staticImportClosure(load.required, MockTraversal::Stop)) {
        ModuleID module(index);
        if (isMocked(module) || (status(module) != ModuleStatus::Unlinked && status(module) != ModuleStatus::Linked))
            continue;
        BitVector reached = staticImportClosure(module, MockTraversal::Stop);
        if (load.mocksRemovedSince.containsIf([&](ModuleID mocked) { return mocked != module && reached.get(mocked.index()); }))
            importers.append(module);
    }
    return importers;
}

void Modules::finishDetachedLoadsUnder(ModuleID required)
{
    if (isMocked(required))
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector loaded = staticImportClosure(required, MockTraversal::Stop);
    Vector<unsigned, 2> due;
    for (DetachedLoad& load : m_detachedLoads) {
        if (load.wait->status() != JSPromise::Status::Pending || load.wasEvicted || load.isForOriginal || load.required == required || load.awaitedFactoryCalls.isEmpty())
            continue;
        ModuleStatus current = status(load.required);
        if ((current != ModuleStatus::Unlinked && current != ModuleStatus::Linked) || isEvicted(load.required) || isMocked(load.required))
            continue;
        load.isEvaluationDue = std::ranges::all_of(load.awaitedFactoryCalls, [&](const auto& call) {
            auto [mocked, factoryCall] = call;
            return loaded.get(mocked.index()) && activeMock(mocked) && !m_modulesWithPendingMock.get(mocked.index()) && host().isFactoryCallPending && host().isFactoryCallPending(m_instance.globalObject, factoryCall);
        });
        if (load.isEvaluationDue)
            due.append(load.number);
    }
    for (unsigned number : due) {
        evaluateDetachedLoad(number);
        RETURN_IF_EXCEPTION(scope, void());
        size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.number == number; });
        if (left == notFound)
            continue;
        m_detachedLoads[left].wasEvaluatedByRequire = true;
        JSFunction* job = JSFunction::create(*m_instance.vm, m_instance.globalObject, 2, String(), detachedLoadEvaluatedJob, ImplementationVisibility::Private);
        JSPromise* turn = JSPromise::create(*m_instance.vm, m_instance.globalObject->promiseStructure());
        turn->fulfill(*m_instance.vm, jsUndefined());
        turn->performPromiseThenWithContext(*m_instance.vm, m_instance.globalObject, job, job, jsUndefined(), jsNumber(number));
    }
}

void Modules::didEvaluateDetachedLoad(unsigned number)
{
    size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; });
    if (left == notFound || !m_detachedLoads[left].modulesToRetry.isEmpty())
        return;
    restoreRequiredValue(m_detachedLoads[left].required);
    m_detachedLoads[left].wait->fulfill(*m_instance.vm, jsUndefined());
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
    if (--load.pendingLoaderWaits || load.pendingFactoryCount || !load.isEvaluationDue)
        return;
    finishDetachedLoad(number);
}

void Modules::finishLoadsAwaitingLoader(std::optional<ModuleID> changing)
{
    if (m_detachedLoads.isEmpty()) [[likely]]
        return;
    Vector<unsigned, 2> due;
    for (const DetachedLoad& load : m_detachedLoads) {
        if (load.isEvaluationDue && load.pendingLoaderWaits && load.wait->status() == JSPromise::Status::Pending && (!changing || load.required == *changing || load.timedMocks.get(changing->index())))
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

Vector<ModuleID, 2> Modules::evaluationOrderedRoots(const DetachedLoad& load) const
{
    if (load.laterRootsBelow.isEmpty()) [[likely]]
        return { load.required };
    struct Readiness {
        bool needsMock { false };
        unsigned steps { 0 };
        bool operator<(const Readiness& other) const { return needsMock != other.needsMock ? other.needsMock : steps < other.steps; }
    };
    Vector<std::pair<ModuleID, ModuleID>, 32> visits { { load.required, ModuleID() } };
    BitVector isVisited(m_table.size());
    isVisited.quickSet(load.required.index());
    for (size_t next = 0; next < visits.size(); ++next) {
        ModuleID module = visits[next].first;
        if (isMocked(module))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            ModuleID requested = moduleLoadedBy(request);
            if (!isVisited.quickSet(requested.index()))
                visits.append({ requested, module });
        }
    }
    UncheckedKeyHashMap<uint32_t, Readiness, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> readiness;
    for (size_t next = visits.size(); next--;) {
        auto [module, firstRequester] = visits[next];
        Readiness own = readiness.get(module.index());
        own.needsMock |= load.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == module; });
        readiness.set(module.index(), own);
        if (!firstRequester)
            continue;
        Readiness above { own.needsMock, own.steps + 1 };
        if (readiness.get(firstRequester.index()) < above)
            readiness.set(firstRequester.index(), above);
    }
    Vector<ModuleID, 2> roots;
    for (size_t next = load.laterRootsBelow.size(); next--;)
        roots.append(load.laterRootsBelow[next]);
    std::ranges::stable_sort(roots, [&](ModuleID first, ModuleID second) { return readiness.get(first.index()) < readiness.get(second.index()); });
    roots.append(load.required);
    return roots;
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
        JSPromise* factory = isForOriginal && isMocked(required) ? pendingFactoryInOriginalGraph(required, state) : pendingFactoryInGraph(required, PendingFactoryRequester::DetachedLoad, state);
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
    for (const BatchedFetch& fetch : batchedFetches()) {
        if (!wasMockReplaced)
            addRequests(fetch.module);
    }
    for (const BatchedFetch& fetch : m_callsToMakeAgain)
        addRequests(fetch.module);
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

void Modules::evictLoadedModules(bool wasMockReplaced)
{
    if (m_modulesToEvictOnceLoaded.isEmpty() || m_instance.vm->m_synchronousModuleQueue) [[likely]]
        return;
    BitVector requested = modulesRequestedByImports(wasMockReplaced);
    if (m_isResumingFactoryChainImport && m_currentImportRequestedModule)
        requested.set(m_currentImportRequestedModule.index());
    Vector<ModuleID, 2> loaded;
    m_modulesToEvictOnceLoaded.removeAllMatching([&](ModuleID module) {
        ModuleStatus current = status(module);
        bool isLoaded = isMocked(module) ? !!activeMock(module) : current == ModuleStatus::Evaluated || current == ModuleStatus::Failed;
        if (!isLoaded || requested.get(module.index()))
            return false;
        loaded.append(module);
        return true;
    });
    for (DetachedLoad& load : m_detachedLoads) {
        if (load.wait->status() != JSPromise::Status::Pending || load.isForOriginal || load.wasEvaluatedByRequire || !loaded.contains(load.required) || std::exchange(load.wasEvicted, true))
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
    m_rootsDueBehindImports.clear();
    evictLoadedModules();
    m_modulesToEvictOnceLoaded.clear();
}

void Modules::finishDetachedLoadsOfRemovedMocks(ModuleID required)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isMock = isMocked(required);
    Vector<unsigned, 2> due;
    for (DetachedLoad& load : m_detachedLoads) {
        if (load.wait->status() != JSPromise::Status::Pending || load.isForOriginal || !load.wasAwaitedMockRemoved || load.required == required || isMocked(load.required) || isEvicted(load.required) || status(load.required) > ModuleStatus::Linked)
            continue;
        bool isLoaded = std::ranges::all_of(load.awaitedFactoryCalls, [&](const auto& call) {
            return isMocked(call.first) ? activeMock(call.first) && !m_modulesWithPendingMock.get(call.first.index()) : status(call.first) == ModuleStatus::Evaluated;
        });
        if (!isLoaded)
            continue;
        if (!isMock) {
            load.isEvaluationDue = true;
            due.append(load.number);
            continue;
        }
        JSFunction* job = JSFunction::create(vm, m_instance.globalObject, 2, String(), removedMockLoadJob, ImplementationVisibility::Private);
        JSPromise* turn = JSPromise::create(vm, m_instance.globalObject->promiseStructure());
        turn->fulfill(vm, jsUndefined());
        turn->performPromiseThenWithContext(vm, m_instance.globalObject, job, job, jsUndefined(), jsNumber(load.number));
    }
    for (unsigned number : due) {
        evaluateDetachedLoad(number);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void Modules::finishRemovedMockLoad(unsigned number)
{
    size_t left = m_detachedLoads.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; });
    if (left == notFound || m_detachedLoads[left].wasEvicted || status(m_detachedLoads[left].required) > ModuleStatus::Linked || isMocked(m_detachedLoads[left].required))
        return;
    m_detachedLoads[left].isEvaluationDue = true;
    finishDetachedLoad(number);
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
    for (DetachedLoad& earlier : m_detachedLoads) {
        if (earlier.required == asked || !staticImportClosure(earlier.required, MockTraversal::Stop).get(asked.index()))
            continue;
        earlier.laterRootsBelow.removeAll(asked);
        earlier.laterRootsBelow.append(asked);
    }
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
    for (size_t below : closure) {
        if (uint64_t call = isMocked(ModuleID(below)) && (!isForOriginal || below != required.index()) ? pendingFactoryCallID(ModuleID(below)) : 0)
            awaitedFactoryCalls.append({ ModuleID(below), call });
    }
    m_detachedLoads.append({ required, Strong<JSPromise>(vm, wait), !isForOriginal, number, heard, WTF::move(unmockedModules), WTF::move(awaitedFactoryCalls) });
    m_detachedLoads.last().isForOriginal = isForOriginal;
}

void Modules::evaluateSynchronously(ModuleID module, SynchronousEvaluationReason reason)
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
    if (reason == SynchronousEvaluationReason::Require && !m_detachedLoads.isEmpty()) [[unlikely]] {
        finishDetachedLoadsUnder(module);
        RETURN_IF_EXCEPTION(scope, void());
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
        if (moduleData.hasTopLevelAwait()) {
            throwAsyncModuleError();
            return;
        }
        RELEASE_AND_RETURN(scope, captureExportsOfFilesBeingEvaluated(module));
    case ModuleStatus::EvaluatingAsync:
        if (!isMocked(module) || !m_originalsEvaluatedUnderMock.get(module.index()))
            throwAsyncModuleError();
        return;
    case ModuleStatus::Unlinked:
        break;
    case ModuleStatus::Linked:
        captureExportsOfFilesBeingEvaluated(module);
        RETURN_IF_EXCEPTION(scope, void());
        break;
    }

    bool hasEagerLoadCycle = reason == SynchronousEvaluationReason::Require && m_innermostEagerLoad && importsEagerlyEvaluatingModule(module);
    if (reason == SynchronousEvaluationReason::Require && m_innermostEagerLoad && m_innermostEagerLoad->runsBeforeLoaderSteps && (hasEagerLoadCycle || reachesEagerLoadGraph(*m_innermostEagerLoad, module))) [[unlikely]] {
        transferSynchronousLoad(*m_innermostEagerLoad);
        RETURN_IF_EXCEPTION(scope, void());
    }
    hasEagerLoadCycle &= !m_rootAwaitingLoader;
    if (std::optional<ModuleID> root = hasEagerLoadCycle ? rootOfRunningFileThatHoldsCycleBelow(module) : std::nullopt) [[unlikely]] {
        evaluateEagerLoadRoot(*root);
        RETURN_IF_EXCEPTION(scope, void());
        if (status(module) != statusAtStart)
            RELEASE_AND_RETURN(scope, evaluateSynchronously(module, reason));
    }
    bool runsStepsOfLoader = reason == SynchronousEvaluationReason::Require;
    LoaderForRequire loaderForRequire = runsStepsOfLoader ? this->loaderForRequire(module) : LoaderForRequire::IsNotNeeded;
    bool needsLoader = loaderForRequire != LoaderForRequire::IsNotNeeded;
    std::optional<VM::SynchronousModuleQueueScope> queue;
    if (runsStepsOfLoader)
        queue.emplace(*m_instance.vm);
    int64_t firstOrder = m_instance.vm->incrementModuleAsyncEvaluationCount();
    SetForScope activeSynchronousLoad(m_activeSynchronousLoad, ++m_synchronousLoadCount);
    SetForScope innermostSyncLoadRoot(m_innermostSyncLoadRoot, std::optional(module));
    SetForScope outermostSynchronousLoad(m_outermostSynchronousLoad, m_outermostSynchronousLoad ? m_outermostSynchronousLoad : m_activeSynchronousLoad);
    auto leaveLoad = [&] {
        if (!queue)
            return;
        if (m_rootAwaitingLoader == module)
            m_rootAwaitingLoader = std::nullopt;
        VM::SynchronousModuleQueue* steps = m_instance.vm->m_synchronousModuleQueue;
        if (m_deferredLoaderSteps == steps) {
            m_deferredLoaderSteps = nullptr;
            m_moduleWithDeferredJobs = std::nullopt;
        }
        leaveStepsOfLoaderToJobs();
        queue.reset();
    };
    auto didLeaveLoad = makeScopeExit(leaveLoad);
    if (loaderForRequire != LoaderForRequire::IsNeeded) {
        evaluateImportedCommonJSModules(module, m_instance.vm->synchronousModuleQueue() ? EagerLoadJobPolicy::DeferLoaderSteps : EagerLoadJobPolicy::KeepQueued, ErrorOfImportedFile::IsLeftToLoader);
        RETURN_IF_EXCEPTION(scope, void());
        needsLoader |= m_rootAwaitingLoader == module;
        if (!needsLoader && status(module) != statusAtStart)
            RELEASE_AND_RETURN(scope, evaluateSynchronously(module, reason));
    }

    JSPromise* promise = nullptr;
    if (size_t handed = m_transferredSynchronousLoads.findIf([&](const TransferredLoad& load) { return load.root == module; }); handed != notFound) [[unlikely]] {
        promise = m_transferredSynchronousLoads[handed].promise.get();
        m_transferredSynchronousLoads.removeAt(handed);
        SetForScope isInHostRequest(m_currentHostRequest, true);
        JSModuleLoader::drainSynchronousModuleQueue(realm);
    } else if (needsLoader) {
        takeUpWaysOfLoads();
        SetForScope isInHostRequest(m_currentHostRequest, true);
        promise = loader()->loadModuleSync(realm, moduleKey(module), nullptr, nullptr);
    } else if (statusAtStart == ModuleStatus::Unlinked && moduleData.reachesUncompiledCode()) {
        ModuleRecord* record = registerWithLoader(module);
        RETURN_IF_EXCEPTION(scope, void());
        SetForScope rootLoadedByCompiledCode(m_rootLoadedByCompiledCode, std::optional(module));
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
            else if (hasEagerLoadCycle) [[unlikely]]
                RELEASE_AND_RETURN(scope, evaluateEagerLoadRoot(m_innermostEagerLoad->root));
            return;
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
        if (hasEagerLoadCycle) [[unlikely]]
            RELEASE_AND_RETURN(scope, evaluateEagerLoadRoot(m_innermostEagerLoad->root));
        return;
    }
    promise->markAsHandled();
    if (promise->status() == JSPromise::Status::Rejected)
        throwException(realm, scope, promise->result());
    else if (status(module) == ModuleStatus::Failed)
        RELEASE_AND_RETURN(scope, throwEvaluationError(module));
    else if (moduleData.hasTopLevelAwait() || (status(module) != ModuleStatus::Evaluating && status(module) != ModuleStatus::Evaluated)) {
        if (reason == SynchronousEvaluationReason::Require)
            markRefusedEvaluation(module, firstOrder);
        throwAsyncModuleError();
    }
}

void Modules::markRefusedEvaluation(ModuleID required, int64_t firstOrder)
{
    Vector<ModuleID, 8> work { required };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        ModuleRecord* record = recordIfExists(module);
        if (!record || record->wasRequireRefused() || record->status() != CyclicModuleRecord::Status::EvaluatingAsync)
            continue;
        auto order = record->asyncEvaluationOrder();
        if (!order.hasOrder() || order.order() < firstOrder)
            continue;
        record->setWasRequireRefused();
        for (const ModuleRequestData& request : m_table.imports(module))
            work.append(request.targetModule());
    }
}

bool ModuleRecord::wasRequireRefused(CyclicModuleRecord* module)
{
    auto* compiled = dynamicDowncast<ModuleRecord>(module);
    return !compiled || compiled->m_wasRequireRefused;
}

void Modules::evaluateEagerLoadRoot(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!hasNotBegun(root))
        return;
    JSPromise* evaluation = evaluate(root, std::nullopt, nullptr);
    RETURN_IF_EXCEPTION(scope, void());
    evaluation->markAsHandled();
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

bool Modules::wasImportedByStatement(ModuleID file) const
{
    for (unsigned index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (data(importer).kind != ModuleKind::SourceText || !wasImporterLoaded(importer))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (request.targetModule() == file)
                return true;
        }
    }
    return false;
}

ModuleID Modules::formThatRequireFinds(ModuleID target, bool& answersNamespace)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto wasRequired = [&](ModuleID form) {
        const LoadedModule* loaded = loadedIfExists(form);
        return loaded && (loaded->requiredValue || loaded->wasRequiredByHost);
    };
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
    if (file && isEvicted(*file))
        file = std::nullopt;
    if (file && wasRequired(*file))
        return *file;
    for (ModuleID form : forms) {
        if (wasRequired(form))
            return form;
    }
    if (file && !hasNotBegun(*file) && !wasEvictedByRemovedMock(*file))
        return *file;
    auto wasImported = [&](ModuleID form) {
        return (status(form) == ModuleStatus::Evaluated || hasBeenImported(form)) && !wasEvictedByRemovedMock(form);
    };
    bool loaderHoldsEntryWithoutType = loader()->moduleMap().contains(ModuleMapKey { key.impl(), ScriptFetchParameters::Type::JavaScript });
    if (!file && loaderHoldsEntryWithoutType && status(target) == ModuleStatus::Evaluated && !wasEvictedByRemovedMock(target))
        return target;
    auto first = m_formsAnsweredFirst.find(ModuleMapKey { key.impl(), ScriptFetchParameters::Type::HostDefined });
    if (first != m_formsAnsweredFirst.end() && forms.contains(first->value.module) && status(first->value.module) == ModuleStatus::Evaluated && !wasEvictedByRemovedMock(first->value.module)) {
        answersNamespace = true;
        return first->value.module;
    }
    if (forms.containsIf(wasImported) && file && (hasBeenImported(*file) || (data(*file).requireResult == RequireResult::Namespace && wasImportedByStatement(*file))) && !wasEvictedByRemovedMock(*file))
        return *file;
    for (bool isJSON : { false, true }) {
        for (ModuleID form : forms) {
            if (m_table.fullPath(form).endsWith("?json"_s) != isJSON || !wasImported(form))
                continue;
            if (!isUnevaluatedTextForm(form)) {
                answersNamespace = true;
                return form;
            }
            if (file && data(*file).kind == ModuleKind::Data) {
                moduleNamespace(*file, ModulePhase::Evaluation);
                RETURN_IF_EXCEPTION(scope, target);
                answersNamespace = true;
                return *file;
            }
        }
    }
    return target;
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
    if (referrer && m_loadsModulesIndividually) [[unlikely]]
        noteRequireThatRunTimeAnswers(target, referrer);
    if (m_hasReloadableModules && referrer && recordsRequireEdges(referrer) && (!staleRequirerObject || requirerIndex(referrer) != referrer.index())) [[unlikely]]
        noteRequireEdge(target, referrer);
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

bool Modules::isImportResolvedByLoader(ModuleID file) const
{
    std::span<const UncompiledModuleData> externals = m_table.modulesOfUncompiledCode();
    if (externals.empty() || m_instance.vm->synchronousModuleQueue())
        return false;
    Identifier key = moduleKey(file);
    return std::ranges::any_of(externals, [&](const UncompiledModuleData& external) {
        ModuleID standIn(external.module);
        LoadedModule* loaded = loadedIfExists(standIn);
        if (!loaded || loaded->resolvedKey != key)
            return false;
        std::optional<ModuleID> importer = data(standIn).referrerModule();
        ModuleRecord* record = importer ? recordIfExists(*importer) : nullptr;
        return record && record->status() == CyclicModuleRecord::Status::New;
    });
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
            noteImport(momentLinked(importer));
    }
    return first;
}

bool Modules::hasPendingMockAfterReset(ModuleID module) const
{
    return m_momentOfModuleReset && data(module).isOfLibrary() && m_modulesWithPendingMock.get(module.index()) && hasBeenImported(module) && !hasBeenImportedSinceReset(module);
}

bool Modules::hasBeenImported(ModuleID target) const
{
    if (namespaceIfCreated(target, ModulePhase::Evaluation))
        return true;
    if (isFetchedSynchronously(target))
        return false;
    if (m_dataFilesImportedByUncompiledCode.get(target.index()) || isImportResolvedByLoader(target))
        return true;
    uint32_t momentMockInItsPlaceEnded = m_momentsMocksInPlaceOfLibraryModulesEnded.get(target);
    if (auto released = m_lastLinksOfReleasedImporters.find(target); released != m_lastLinksOfReleasedImporters.end() && released->value >= momentMockInItsPlaceEnded)
        return true;
    return std::ranges::any_of(m_table.importsOfHeldFile(target), [&](const ImportOfHeldFileData& import) {
        ModuleID importer(import.importer);
        if (activeMock(importer) && !wasOriginalEvaluated(importer))
            return false;
        return (wasImporterLoaded(importer) || hasPlannedLoadReached(importer)) && momentLinked(importer) >= momentMockInItsPlaceEnded;
    });
}

bool Modules::hasEvaluationPassedImport(ModuleID importer, ModuleID target) const
{
    if (status(importer) != ModuleStatus::Evaluating)
        return status(importer) > ModuleStatus::Linked;
    for (const ModuleRequestData& request : m_table.imports(importer)) {
        if (request.targetModule() == target)
            return true;
        if (data(request.targetModule()).isESModuleOfProgram() && status(request.targetModule()) <= ModuleStatus::Evaluating)
            return false;
    }
    return true;
}

bool Modules::isUnevaluatedTextForm(ModuleID form) const
{
    if (!m_table.fullPath(form).endsWith("?text"_s) || status(form) == ModuleStatus::Evaluated || (m_mocks && namespaceIfCreated(form, ModulePhase::Evaluation)))
        return false;
    return std::ranges::none_of(m_table.importsOfHeldFile(form), [&](const ImportOfHeldFileData& import) {
        return hasEvaluationPassedImport(ModuleID(import.importer), form);
    });
}

void Modules::runDataFileAgainForImport(ModuleID file, ImportResolver resolver)
{
    LoadedModule* loaded = loadedIfExists(file);
    if (!loaded || recordIfExists(file))
        return;
    if (resolver == ImportResolver::Build && !loaded->wasRequiredByHost && !data(file).isDataFoundByName() && !isTestRun())
        return;
    reevaluateDataFile(file);
}

void Modules::reevaluateDataFile(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    if (data(file).kind != ModuleKind::Data || !loaded || !loaded->requiredValue || isMocked(file))
        return;
    if (status(file) != ModuleStatus::Evaluated || hasBeenImported(file))
        return;
    setStatus(file, ModuleStatus::Linked);
    evaluateSynchronously(file, SynchronousEvaluationReason::Require);
}

bool Modules::wasImporterLoaded(ModuleID importer) const
{
    if (status(importer) > ModuleStatus::Linked || (m_entryPointIsLinked && data(importer).isLinkedWithEntryPoint() && !m_modulesLinkedAfterEntryPoint.get(importer.index())))
        return true;
    if (ModuleRecord* record = recordIfExists(importer); record && record->status() != CyclicModuleRecord::Status::New)
        return true;
    return data(importer).isESModuleOfProgram() && (wasLoadedWithGraph(importer) || isBelowRootWhoseFilesRun(importer));
}

bool Modules::isBelowRootWhoseFilesRun(ModuleID wanted) const
{
    Vector<ModuleID, 4> running;
    forEachEvaluatingCommonJSModule([&](ModuleID file) {
        running.append(file);
    });
    for (const EagerLoad* files = m_innermostEagerLoad; files; files = files->outer) {
        BitVector seen(m_table.size());
        seen.quickSet(files->root.index());
        Vector<uint32_t, 32> found { files->root.index() };
        size_t endOfLevel = 1;
        unsigned level = 0;
        std::optional<unsigned> levelOfWanted;
        bool hasComeToFile = false;
        for (size_t index = 0; index < found.size() && !hasComeToFile; ++index) {
            if (index == endOfLevel) {
                ++level;
                endOfLevel = found.size();
            }
            ModuleID module(found[index]);
            if (module == wanted)
                levelOfWanted = level;
            if (running.contains(fileOf(module))) {
                hasComeToFile = true;
                bool readsTextsAsAskedFor = m_instance.vm->synchronousModuleQueue() || isFetchedInsideCall(fileOf(module));
                if (levelOfWanted && (!readsTextsAsAskedFor || *levelOfWanted + 1 < level))
                    return true;
                continue;
            }
            if (!data(module).isESModuleOfProgram())
                continue;
            for (const ModuleRequestData& request : m_table.requests(module)) {
                if (!request.isRequire && !request.isHoisted && !seen.quickSet(request.target()))
                    found.append(request.target());
            }
        }
        if (levelOfWanted && !hasComeToFile)
            return true;
    }
    return false;
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
    if (isEvaluatedUnderPendingPatch(target)) [[unlikely]]
        tellThatFileDidNotRunAgain(fileOf(target));
    if (!m_modulesWithRunningFactory.isEmpty()) [[unlikely]]
        tellIfFactoryIsRunning(target, LoadKind::Require);
    if (JSValue settled = settledAnswerOfRequire(target, requiredBy))
        return settled;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool loadsEvictedDataFile = false;
    bool answersNamespace = false;
    if (data(target).kind == ModuleKind::Data) [[unlikely]] {
        if (formThatAnswers == FormThatAnswers::TheOneThatLoaderFinds && m_table.mayHaveAttributeVariants()) {
            target = formThatRequireFinds(target, answersNamespace);
            RETURN_IF_EXCEPTION(scope, { });
        }
        loadsEvictedDataFile = isEvictedDataFile(target);
        didLoadDataFile(target, LoadKind::Require);
    }
    removeUnusedLingeringMocks(target);
    RETURN_IF_EXCEPTION(scope, { });
    if (!m_importsAwaitingSource.isEmpty()) [[unlikely]] {
        walkInPlaceOfCallsHeldForTextOf(fileOf(target));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (isMockLingering(target) && m_modulesWithPendingMock.get(target.index()) && !isEvaluatedByLoader(target) && isResolvedForInFlightLoads(target)) [[unlikely]] {
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
    rejectRequireInSweep(target);
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
            if (!isOverActiveMock || !isPending || waiting == m_pendingMockCalls.end() || !waiting->value.keepsActiveMock)
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
    if (m_mocks && readsExports == ReadsExports::Yes && m_modulesWithPendingMock.get(target.index()) && status(target) <= ModuleStatus::Linked && host().requireOriginal && (isDataFile(target) || (targetData.mayRunAsOriginal() && commonJSOriginalNamedBy(actualKey(target))))) [[unlikely]] {
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
    bool isRequiredByProgram = requiredBy == RequiredBy::Program || (referrer && !data(referrer).isOfLibrary());
    if (m_loadsModulesIndividually && targetData.isESModuleOfProgram()) [[unlikely]]
        willFetchESModules(target);
    if (m_mocks && m_modulesWithPendingMock.get(target.index()) && (hasFailedWithoutMock(target) || takeFailureBeforeMock(target))) [[unlikely]]
        forgetLoadThatFailed(target);
    if (!m_rejectedRequires.isEmpty() && (!targetData.isOfLibrary() || isRequiredByProgram) && targetData.kind == ModuleKind::SourceText && !isMocked(target) && !vm.m_synchronousModuleQueue && !m_blockedRequiresWithError.get(target.index()) && !m_isResumingFactoryChainImport && isStillBlockedOnFactories(target)) [[unlikely]] {
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
    if (!targetData.isOfLibrary() || isRequiredByProgram) {
        SetForScope requireRunsMocks(vm.aotState().requireRunsMocks, true);
        SetForScope currentRequireTarget(m_currentRequireTarget, target);
        SetForScope requiringModule(m_requiringModule, referrer);
        std::optional<VM::SynchronousModuleQueueScope> loadsOfFactories;
        if (m_mocks && targetData.kind == ModuleKind::SourceText && !isMocked(target) && !vm.m_synchronousModuleQueue) [[unlikely]]
            loadsOfFactories.emplace(vm);
        SetForScope shouldContinuePastPendingFactories(m_shouldContinuePastPendingFactories, !!loadsOfFactories);
        bool factoryHasNotSettled = runMocksInGraph(target);
        if (factoryHasNotSettled && !scope.exception()) [[unlikely]]
            evaluateImportedCommonJSModules(target, EagerLoadJobPolicy::KeepQueued, ErrorOfImportedFile::IsThrown, nullptr, PendingFactoryModules::Skip);
        if (loadsOfFactories) [[unlikely]] {
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
    if (hasPendingFactory || !m_rejectedRequires.isEmpty()) [[unlikely]] {
        resolveUncompiledImports(target);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (hasPendingFactory) [[unlikely]]
        recordRejectedRequire(target, false);
    if (!m_rejectedRequires.isEmpty() && (!targetData.isOfLibrary() || isRequiredByProgram)) [[unlikely]] {
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
    if (delegatesToHost(target)) [[unlikely]]
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
        if (LoadedModule* loaded = loadedIfExists(target); loaded && loaded->isAnsweredWithNamespace && !isMocked(target) && !activeMock(target)) [[unlikely]] {
            JSValue held = exportsOnObjectMadeByRequire(*loaded);
            RETURN_IF_EXCEPTION(scope, { });
            if (held && held != loaded->exportsGivenToObjectMadeByRequire.get())
                return held;
            RELEASE_AND_RETURN(scope, namespaceForRequire(target));
        }
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
    if (loaded.isBeingRequired) {
        if (!loaded.exportsWhileRequired)
            loaded.exportsWhileRequired.set(*this, constructEmptyObject(m_instance.globalObject));
        return loaded.exportsWhileRequired.get();
    }
    bool isAnsweredAsByLoader = requiredBy == RequiredBy::Program && targetData.loaderReturnsNamespace();
    bool answersDefaultExport = !isAnsweredAsByLoader && !answersNamespace && !isMocked(target) && (targetData.requireResult == RequireResult::DefaultExport || (targetData.requireResult == RequireResult::NamespaceIfImported && !loaded.requiredValue && (loadsEvictedDataFile || !hasBeenImported(target))));
    if (loaded.isKeptByRequire && loaded.requiredValue && !isAnsweredAsByLoader && status(target) == ModuleStatus::Failed) [[unlikely]]
        return loaded.requiredValue.get();
    bool wasBeingRequired = false;
    bool isFailedUnderMock = status(target) == ModuleStatus::Failed && isMocked(target) && !m_mocksWithoutFactory.get(target.index());
    if (status(target) != ModuleStatus::Evaluated && !isFailedUnderMock) {
        wasBeingRequired = loaded.isBeingRequired = status(target) != ModuleStatus::Evaluating;
        bool wasKeptByRequire = std::exchange(loaded.isKeptByRequire, true);
        evaluateSynchronously(target, SynchronousEvaluationReason::Require);
        loaded.isBeingRequired = false;
        if (scope.exception()) [[unlikely]] {
            recordRejectedRequire(target);
            loaded.isKeptByRequire = wasKeptByRequire;
            if (!wasKeptByRequire)
                loaded.moduleObjectFromRequire.set(*this, nullptr);
            return { };
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
    if (!m_modulesToEvictOnceLoaded.isEmpty() && !vm.m_synchronousModuleQueue) [[unlikely]] {
        finishDetachedLoadsOfRemovedMocks(target);
        RETURN_IF_EXCEPTION(scope, { });
    }
    auto evictsLoadedModules = makeScopeExit([&] {
        if (!scope.exception())
            evictLoadedModules();
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
    if (!isAnsweredAsByLoader)
        loaded.requiredValue.set(*this, result);
    loaded.wasRequiredByHost |= requiredBy == RequiredBy::Program;
    if (isFetchedSynchronously(target)) {
        reevaluateDataFile(target);
        RETURN_IF_EXCEPTION(scope, { });
    } else if (!m_table.importsOfHeldFile(target).empty()) {
        runDataFileAgainForImport(target, ImportResolver::Build);
        RETURN_IF_EXCEPTION(scope, { });
    }
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
    if (moduleStatus == ModuleStatus::Failed) {
        if (!tryDiscardFailureDuringFetch(module))
            RELEASE_AND_RETURN(scope, throwEvaluationError(module));
        moduleStatus = status(module);
    }
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
    if (!record && moduleData.evaluatesItself())
        RELEASE_AND_RETURN(scope, evaluateByCallingBody(module));

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

static std::optional<ModuleID> firstFileThatRunsWithGraphOf(const Modules& modules, ModuleID root)
{
    const ModuleTable& table = modules.table();
    if (modules.isMocked(table.moduleBehindFacade(root).value_or(root)))
        return std::nullopt;
    if (modules.isCommonJSModuleOrFacade(root))
        return table.moduleBehindFacade(root).value_or(root);
    for (const ModuleRequestData& request : table.requests(root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (modules.isMocked(table.moduleBehindFacade(imported).value_or(imported)))
            break;
        if (!request.isDeferred && modules.isCommonJSModuleOrFacade(imported))
            return table.moduleBehindFacade(imported).value_or(imported);
        if (!request.isHoisted)
            break;
    }
    return std::nullopt;
}

std::optional<ModuleID> Modules::mainCommonJSModule()
{
    lookForMainCommonJSModule();
    return m_mainCommonJSModule;
}

void Modules::decideMainCommonJSModule(std::optional<ModuleID> file)
{
    m_mainCommonJSModule = file;
    m_isMainCommonJSModuleDecided = true;
    m_pendingFirstRequireTargets.clear();
    m_firstFilesOfLoads.clear();
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

void Modules::resetFailedMainModule()
{
    ModuleID failed = *std::exchange(m_mainCommonJSModule, std::nullopt);
    m_failedMainModules.set(failed.index());
    m_isMainCommonJSModuleDecided = false;
    for (uint32_t index = 0; index < m_table.size() && !m_isMainCommonJSModuleDecided; ++index) {
        ModuleID file(index);
        m_isMainCommonJSModuleDecided = file != failed && isInRequireMap(file);
    }
}

bool Modules::isMainCommonJSModule(ModuleID file)
{
    return mainCommonJSModule() == file || (status(file) == ModuleStatus::Failed && m_failedMainModules.get(file.index()));
}

void Modules::lookForMainCommonJSModule()
{
    if (m_isMainCommonJSModuleDecided)
        return;
    if (m_firstModuleRequestedByHost && !std::exchange(m_didSearchBelowEntryPoint, true)) {
        if (std::optional<ModuleID> file = firstFileThatRunsWithGraphOf(*this, *m_firstModuleRequestedByHost))
            return decideMainCommonJSModule(file);
    }
    for (ModuleID file : m_firstFilesOfLoads) {
        if (!wasRequestedByLoader(file) || status(file) == ModuleStatus::Failed)
            continue;
        bool someFileHasModuleObject = std::ranges::any_of(m_pendingFirstRequireTargets, [&](ModuleID required) {
            return status(required) != ModuleStatus::Failed;
        });
        return decideMainCommonJSModule(someFileHasModuleObject ? std::nullopt : std::optional(file));
    }
}

void Modules::noteLoadOfGraph(ModuleID root)
{
    if (!m_showsWaysBetweenModuleObjects)
        return;
    lookForMainCommonJSModule();
    if (m_isMainCommonJSModuleDecided)
        return;
    if (std::optional<ModuleID> file = firstFileThatRunsWithGraphOf(*this, root)) {
        m_firstFilesOfLoads.append(*file);
        lookForMainCommonJSModule();
    }
}

void Modules::didImportFileFromUncompiledCode(ModuleID moduleOrFacade)
{
    noteRequestByLoader(moduleOrFacade);
    noteLoadOfGraph(moduleOrFacade);
}

std::optional<OptionSet<ModuleObjectTrait>> Modules::newModuleObjectTraits(ModuleID module)
{
    if (data(module).kind != ModuleKind::CommonJs || ensureLoaded(module).requirer)
        return std::nullopt;
    stopWhereExportsLeaveCompiledCode(module);
    OptionSet<ModuleObjectTrait> traits;
    if (data(module).hasValue())
        traits.add(ModuleObjectTrait::HasExportsVariable);
    if (isMainCommonJSModule(module))
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
    if (data(module).kind == ModuleKind::CommonJs && !ensureLoaded(module).isAnsweredWithNamespace)
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
    host().createModuleObject(m_instance.globalObject, pathString(module), module, traits, objects);
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
    if (!m_showsWaysBetweenModuleObjects || !(isFileOfProgram(target) || targetData.kind == ModuleKind::Data))
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
    bool isChildOfOtherObject = isFromCreatedRequire && data(referrer).kind == ModuleKind::CommonJs;
    if (staleRequirerObject) [[unlikely]] {
        detachFailedModuleObject(referrer);
        RETURN_IF_EXCEPTION(scope, void());
        uint64_t staleRequirerPair = 1ull << 62 | (static_cast<uint64_t>(referrer.index()) + 1) << 32 | target.index();
        bool wasFirstESModuleRequire = m_recordedRequires.remove(staleRequirerPair);
        if (!isAfterLoad) {
            LoadedModule* required = loadedIfExists(target);
            bool isFirstESModuleRequire = targetData.kind == ModuleKind::SourceText && (wasFirstESModuleRequire || (!firstRequirerOf(target) && !(required && required->firstUncompiledRequirer)));
            recordRequireFromUncompiledCode(target, staleRequirerObject);
            if (isFirstESModuleRequire)
                m_recordedRequires.set(staleRequirerPair, RecordedRequire::BegunFirstESModuleRequire);
            if (isFirstESModuleRequire && m_recordedRequireIndex)
                m_recordedRequireIndex->addRequire(staleRequirerPair);
            return;
        }
        if (!m_isMainCommonJSModuleDecided && status(target) != ModuleStatus::Failed) {
            LoadedModule* required = loadedIfExists(target);
            bool mayLoseEntry = isBeingEvaluated(target) && (targetData.kind != ModuleKind::SourceText || (required && required->isBeingRequired));
            if (!mayLoseEntry)
                decideMainCommonJSModule(std::nullopt);
        }
        if (wasFirstESModuleRequire || isChildOfOtherObject || !host().appendModuleChildren)
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
        lookForMainCommonJSModule();
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
        if (targetData.kind == ModuleKind::SourceText)
            recorded.iterator->value = RecordedRequire::BegunFirstESModuleRequire;
        if (!m_isMainCommonJSModuleDecided)
            m_pendingFirstRequireTargets.append(target);
        return;
    }
    switch (recorded.iterator->value) {
    case RecordedRequire::BegunFirstESModuleRequire:
        if (status(target) == ModuleStatus::Failed)
            return;
        recorded.iterator->value = RecordedRequire::ReturnedAndNotListed;
        if (!m_isMainCommonJSModuleDecided)
            decideMainCommonJSModule(std::nullopt);
        return;
    case RecordedRequire::ReturnedAndNotListed:
        recorded.iterator->value = isEvicted(target) ? RecordedRequire::BegunForEvictedModule : RecordedRequire::Begun;
        return;
    case RecordedRequire::Begun:
    case RecordedRequire::BegunForEvictedModule: {
        if (status(target) == ModuleStatus::Failed)
            return;
        bool isListedByHost = recorded.iterator->value == RecordedRequire::BegunForEvictedModule;
        recorded.iterator->value = RecordedRequire::Returned;
        if (!m_isMainCommonJSModuleDecided && !isBeingEvaluated(target))
            decideMainCommonJSModule(std::nullopt);
        if (!isListedByHost && !isChildOfOtherObject)
            ensureWaysTo(referrer).requiredModules.append(target);
        findWhetherExportsVariableIsTakenOffModuleObject(referrer);
        lookAtExportsOfModuleObject(target, resolver);
        RETURN_IF_EXCEPTION(scope, void());
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
    if (!m_showsWaysBetweenModuleObjects || !(isFileOfProgram(target) || data(target).kind == ModuleKind::Data))
        return;
    if (isCommonJSFile)
        noteRequireOfFile(target);
    if (!canExposeModuleObject(target))
        return;
    lookForMainCommonJSModule();
    LoadedModule& required = ensureLoaded(target);
    if (firstRequirerOf(target) || required.firstUncompiledRequirer || (required.isCreatedByRequireCache && required.moduleObjectFromRequire) || (isCommonJSFile && wasRequestedByLoader(target)))
        return;
    required.firstUncompiledRequirer.set(*this, requirerModuleObject);
    if (!m_isMainCommonJSModuleDecided)
        m_pendingFirstRequireTargets.append(target);
}

void Modules::noteRequestByLoader(ModuleID moduleOrFacade)
{
    if (!m_showsWaysBetweenModuleObjects)
        return;
    ModuleID file = m_table.moduleBehindFacade(moduleOrFacade).value_or(moduleOrFacade);
    if (data(file).kind == ModuleKind::CommonJs)
        m_filesRequestedByLoader.set(file.index());
}

bool Modules::wasRequestedByLoader(ModuleID file)
{
    if (m_filesRequestedByLoader.get(file.index()))
        return true;
    bool wasRequested = status(file) > ModuleStatus::Linked;
    if (!wasRequested) {
        std::optional<ModuleID> facade = m_table.facadeOf(file);
        wasRequested = facade && status(*facade) > ModuleStatus::Linked;
        if (facade || data(file).isImportedByModule()) {
            for (EagerLoad* load = m_innermostEagerLoad; load && !wasRequested; load = load->outer)
                wasRequested = wasRequestedByLoad(*load, file);
        }
    }
    if (wasRequested)
        m_filesRequestedByLoader.set(file.index());
    return wasRequested;
}

bool Modules::wasRequestedByLoad(EagerLoad& load, ModuleID file)
{
    std::optional<ModuleID> running = evaluatingModuleOrFacade(load);
    if (!running)
        return false;
    if (load.requested.isEmpty()) {
        load.requested.ensureSize(m_table.size());
        load.requested.quickSet(fileOf(load.root).index());
        load.nextLevelImporters.append(load.root);
    }
    while (!load.requested.quickGet(fileOf(*running).index()) && !load.nextLevelImporters.isEmpty()) {
        for (ModuleID importer : std::exchange(load.nextLevelImporters, { })) {
            for (const ModuleRequestData& request : m_table.requests(importer)) {
                if (request.isHoisted || request.isRequire)
                    continue;
                ModuleID target = fileOf(request.targetModule());
                if (!load.requested.quickSet(target.index()) && isLoadedWithRequests(target) && !(m_mocks && m_mocks->modules.quickGet(target.index())))
                    load.nextLevelImporters.append(target);
            }
        }
    }
    return load.requested.quickGet(file.index());
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
    m_filesRequestedByLoader.clear(file.index());
    for (EagerLoad* load = m_innermostEagerLoad; load; load = load->outer) {
        if (wasRequestedByLoad(*load, file))
            load->requested.quickClear(file.index());
    }
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
    if (loaded && loaded->firstUncompiledRequirer && !isMainCommonJSModule(module))
        return loaded->firstUncompiledRequirer;
    std::optional<ModuleID> firstRequirer = firstRequirerOf(module);
    if (!firstRequirer || isMainCommonJSModule(module))
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
    m_pendingFirstRequireTargets.removeAll(required);
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

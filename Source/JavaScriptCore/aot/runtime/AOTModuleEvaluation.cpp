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
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "GlobalObjectMethodTable.h"
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

    if (context.stack.isEmpty())
        didBeginWalkOnEmptyStack(root);
    else if (passImporterEnteredAboveMock(root, root, context.stack)) [[unlikely]]
        return;
    runMocksBelow(root);
    RETURN_IF_EXCEPTION(scope, void());
    noteModulesLoadedWith(root);
    noteThatGraphBegins(root);
    MocksBelowRoot mocks = mocksBelow(root);

    if (status(root) == ModuleStatus::Evaluating && !m_modulesAskedForWhileTheyRan.contains(root)) [[unlikely]]
        m_modulesAskedForWhileTheyRan.append(root);
    auto settleImports = makeScopeExit([&] {
        settleImportsOfModulesThatHaveRun(root);
    });
    EvaluationFrames frames;
    EvaluationInProgress evaluation { frames, m_innermostEvaluation };
    SetForScope innermostEvaluation(m_innermostEvaluation, &evaluation);

    auto pushRecord = [&](EvaluationFrame& frame, ModuleRecord* record) {
        RELEASE_ASSERT(frame.stackPosition <= context.stack.size());
        frame.record = record;
        record->setHoldsStatus();
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
        if (!m_modulesThatRanBesideMock.get(importer) && !isBelowModuleThatRunsBesideMock.get(importer))
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
        }
        if (moduleStatus == ModuleStatus::Failed)
            throwEvaluationError(module);
        if ((moduleStatus == ModuleStatus::Evaluated || moduleStatus == ModuleStatus::EvaluatingAsync) && !frames.isEmpty())
            pass(module);
        if (moduleStatus != ModuleStatus::Linked)
            return false;
        if (!frames.isEmpty() && status(module) == ModuleStatus::Linked && passImporterEnteredAboveMock(module, root, context.stack)) [[unlikely]]
            return false;
        if (isMocked(module) && !runsBesideMock && !hasMockLaidOver(module)) [[unlikely]] {
            setStatus(module, ModuleStatus::Evaluated);
            return false;
        }
        if (runsBesideMock) [[unlikely]] {
            m_modulesThatRanBesideMock.set(module.index());
            m_modulesOwedBesideMock.clear(module.index());
            m_importsMadeByOwnFactory.removeAll(module);
            m_modulesThatOwnFactoryMayLoad.clear(module.index());
        }

        m_modulesWhoseFailedRunWasForgotten.clear(module.index());
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
                if (isLinked && isMocked(required) && !hasMockLaidOver(required)) [[unlikely]] {
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
    if (!body || (isMocked(module) && !m_filesWhoseOriginalRuns.get(module.index()) && !m_modulesThatRanBesideMock.get(module.index()) && !hasMockLaidOver(module)))
        return jsUndefined();

    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    ClosureTable& closures = m_state.objects->closures;
    SetForScope isAskingHost(m_isAskingHost, false);
    SetForScope isStatementAskingHost(m_isStatementAskingHost, false);
    if (!moduleData.isOfLibrary())
        didStartThread(&vm);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_numberOfMocksInPlace && m_rootWhoseFilesRun != std::optional(module)) [[unlikely]] {
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
    if ((moduleData.kind == ModuleKind::SourceText && !moduleData.hasTopLevelAwait()) || moduleData.kind == ModuleKind::CommonJs) {
        EncodedJSValue function = JSValue::encode(jsNumber(static_cast<int32_t>(closureNumberOf(*body))));
        m_instance.modules.moduleThatRunTimeStartsPlusOne = module.index() + 1;
        auto forgetsStart = makeScopeExit([&] {
            m_instance.modules.moduleThatRunTimeStartsPlusOne = 0;
        });
        CallData entry = CallData::forCompiledCode(moduleBodyEntrypoint().taggedPtr(), realm);
        RELEASE_AND_RETURN(scope, callCompiledCode(vm, closures.calleeOfModuleBodies(vm, *body), entry, jsUndefined(), nullptr, ArgList(&function, 1)));
    }
    RELEASE_AND_RETURN(scope, callCompiledCode(realm, closures.createClosureForOneCall(vm, *body), jsUndefined(), ArgList()));
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
    if (ModuleID file = m_table.fileBehindFace(module).value_or(module); data(file).mayBeImportedWhileItRuns()) [[unlikely]] {
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

void Modules::leaveModuleObjectToRunThatThrew(ModuleID file)
{
    SuspendExceptionScope errorStillThrown(*m_instance.vm);
    DeferTerminationForAWhile noTerminationArrives(*m_instance.vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    JSObject* objectOfRun = objectThatStandsForModuleIfMade(file);
    for (const FirstRequire& first : m_firstRequires) {
        if (first.requirer != file || firstRequirerOf(first.required) != file || ensureLoaded(first.required).firstRequirerOutsideImage)
            continue;
        if (!objectOfRun) {
            objectOfRun = objectThatStandsForModule(file);
            if (scope.exception()) [[unlikely]]
                return;
        }
        ensureLoaded(first.required).firstRequirerOutsideImage.set(*this, objectOfRun);
    }
    if (objectOfRun && !keepsModuleObjectToItself(file) && host().didForgetRunOfModuleObject) {
        host().didForgetRunOfModuleObject(m_instance.globalObject, objectOfRun, false);
        ensureLoaded(file).moduleObjectOfRunThatThrew.set(*this, objectOfRun);
    }
    static_cast<void>(scope.exception());
}

void Modules::unbindModuleObjectOfRunThatThrew(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    if (!loaded || !loaded->moduleObjectOfRunThatThrew) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSObject* objectOfRun = loaded->moduleObjectOfRunThatThrew;
    loaded->moduleObjectOfRunThatThrew.set(*this, nullptr);
    for (const FirstRequire& first : m_firstRequires) {
        if (first.requirer == file && firstRequirerOf(first.required) == file && !ensureLoaded(first.required).firstRequirerOutsideImage)
            ensureLoaded(first.required).firstRequirerOutsideImage.set(*this, objectOfRun);
    }
    host().didForgetRunOfModuleObject(m_instance.globalObject, objectOfRun, true);
    RETURN_IF_EXCEPTION(scope, void());
    forgetWhatFileRequired(file);
}

void Modules::forgetWhatFileRequired(ModuleID file)
{
    if (WaysToModule* ways = waysToIfRecorded(file)) {
        ways->requiredModules.clear();
        ways->numberOfChildrenShown = 0;
    }
    m_recordedRequires.removeIf([&](const auto& recorded) {
        return recorded.key >> 32 == static_cast<uint64_t>(file.index()) + 1;
    });
}

bool Modules::hasSynchronousLoadFetched(ModuleID file)
{
    std::optional<ModuleID> face = m_table.faceOfFile(file);
    if (!m_activeSynchronousLoad || !face)
        return false;
    for (FilesBeingRun* load = m_innermostFilesBeingRun; load; load = load->outer) {
        std::optional<ModuleID> running = load->isAheadOfStepsOfLoader ? fileOrFaceEvaluatedIn(*load) : std::nullopt;
        if (running && fileOf(*running) != file && hasLoaderAskedFor(*load, file))
            return true;
    }
    if (!m_instance.vm->synchronousModuleQueue())
        return false;
    ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(*face));
    return entry && entry->status() == ModuleRegistryEntry::Status::Fetching && !entry->record();
}

bool Modules::takeThrowBehindFetch(ModuleID fileOrFace)
{
    LoadedModule* loaded = loadedIfExists(fileOf(fileOrFace));
    if (!loaded || !loaded->outermostSynchronousLoadWhenItThrewBehindFetch) [[likely]]
        return false;
    uint32_t load = std::exchange(loaded->outermostSynchronousLoadWhenItThrewBehindFetch, 0);
    return load == m_outermostSynchronousLoad && fileOf(fileOrFace) != fileOrFace && hasNotBegunAsFile(fileOf(fileOrFace));
}

void Modules::makeRecordWithoutExports(ModuleID face)
{
    captureNoExports(fileOf(face));
    ensureLoaded(face).didDropError = true;
    setStatus(face, ModuleStatus::Evaluated);
}

void Modules::forgetRunThatThrew(ModuleID file)
{
    leaveModuleObjectToRunThatThrew(file);
    letGoOfModuleObject(file);
    forgetTemplateObjects(file);
    if (LoadedModule* loaded = loadedIfExists(file)) {
        loaded->requiredValue.set(*this, JSValue());
        loaded->firstRequirerOutsideImage.set(*this, nullptr);
        loaded->synchronousLoadCountAtStart = std::nullopt;
        loaded->hasFirstRequireOfRunThatThrew = true;
    }
    forgetWhatFileRequired(file);
    if (WaysToModule* ways = waysToIfRecorded(file))
        ways->firstRequirer = std::nullopt;
    m_recordedRequires.removeIf([&](const auto& recorded) {
        return static_cast<uint32_t>(recorded.key) == file.index();
    });
    m_modulesFirstRequiredByCallsInProgress.removeAll(file);
    setStatus(file, ModuleStatus::Linked);
}

bool Modules::isAwaitedByImportFromBeforeItRan(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    return isAwaitedByImport(file) && !(loaded && loaded->isAwaitedByCallThatComesInLaterJob);
}

bool Modules::isAwaitedByImport(ModuleID file)
{
    for (ModuleID fileOrFace : { file, m_table.faceOfFile(file).value_or(file) }) {
        if (m_modulesOfQueuedImports.get(fileOrFace.index()) || m_loadsInFlight.hasRoot(fileOrFace) || std::ranges::contains(textsThatWait(), fileOrFace, &TextThatWaits::module))
            return true;
    }
    return false;
}

void Modules::didThrowInBody(ModuleID module, JSValue error)
{
    if (m_filesWhoseOriginalRuns.get(module.index())) [[unlikely]]
        return;
    if (data(module).kind == ModuleKind::CommonJs && m_instance.vm->hasPendingTerminationException()) [[unlikely]] {
        setStatus(module, ModuleStatus::Evaluated);
        return;
    }
    if (data(module).kind == ModuleKind::CommonJs && (mayBeEvicted(module) || (data(module).runsAgainAfterThrow() && !isAwaitedByImportFromBeforeItRan(module))) && status(module) == ModuleStatus::Evaluating && !loaderHasFileThatThrew(module)) [[unlikely]] {
        if (mayBeEvicted(module))
            evict(module);
        else {
            if (hasSynchronousLoadFetched(module))
                ensureLoaded(module).outermostSynchronousLoadWhenItThrewBehindFetch = m_outermostSynchronousLoad;
            forgetRunThatThrew(module);
        }
        return;
    }
    if (data(module).keepsError())
        setFailed(module, error);
    else
        setStatus(module, ModuleStatus::Evaluated);
    if (isCommonJSFileOrFace(module) && wasImportedWhileItRan(m_table.fileBehindFace(module).value_or(module)))
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
    countEvent(*m_instance.vm, RuntimeCounterID::ModulesForCyclesThatWait, members.size());
    if (std::ranges::none_of(members, [&](uint32_t member) { return m_storedStatuses[member] > evaluating; }))
        return;

    Vector<uint8_t> evaluationOf(members.size(), [&](size_t place) -> uint8_t {
        ModuleID member(members[place]);
        uint8_t stored = m_storedStatuses[member.index()];
        if (stored < evaluated || stored == evaluating || !isOnCycleOfImports(member))
            return 0;
        return stored == evaluated ? markOfEvaluationThatBegan(member) : stored;
    });

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
        if (evaluationOf[place] && m_storedStatuses[members[place]] != evaluated)
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
    bool someFactoryHasNotSettled = tellHostOfRequire(module);
    if (storedStatus(module) != StoredModuleStatus::InRecord) {
        ModuleID file = fileOf(module);
        if (!isChangedOnDisk(file)) [[likely]] {
            m_storedStatuses[module.index()] = static_cast<uint8_t>(m_instance.modules.evaluatingMark);
            return true;
        }
        m_filesChangedOutOfSightOfBuild.set(file.index());
        JSGlobalObject* realm = m_instance.globalObject;
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        JSPromise* loaded = nullptr;
        {
            SetForScope isAskingHost(m_isAskingHost, true);
            loaded = loader()->loadModuleSync(realm, keyWithoutForm(file), nullptr, nullptr);
        }
        RETURN_IF_EXCEPTION(scope, false);
        if (loaded->status() != JSPromise::Status::Rejected) {
            throwTypeError(realm, scope, makeString("Cannot load '"_s, m_table.pathText(file), "': it was written after this test run was compiled"_s));
            return false;
        }
        loaded->markAsHandled();
        throwException(realm, scope, loaded->result());
        return false;
    }
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    endMocksThatStoodForLoadsInFlight();
    RETURN_IF_EXCEPTION(scope, false);
    if (m_filesThatStayLoadedAboveMock.get(module.index())) [[unlikely]]
        tellOfFilesThatStayedLoadedBelow(module);
    if (storedStatus(module) == StoredModuleStatus::Evaluated) [[unlikely]]
        return false;
    if (storedStatus(module) != StoredModuleStatus::InRecord) [[unlikely]]
        RELEASE_AND_RETURN(scope, beginModuleInRecord(module));
    if (m_modulesWithMockToRun.get(module.index()) && !(standsForLoadsInFlight(module) && isEvaluatedByLoader(module))) [[unlikely]] {
        SetForScope requireRunsMocks(m_instance.vm->aotState().requireRunsMocks, true);
        SetForScope moduleThatRequireAsksFor(m_moduleThatRequireAsksFor, module);
        SetForScope moduleThatRequires(m_moduleThatRequires, ModuleID());
        auto waiting = m_momentsWaitingMocksWereLaid.find(module);
        bool liesInRequireMap = waiting != m_momentsWaitingMocksWereLaid.end() && waiting->value.importersKeepMockThatStands && wasRequiredUnderMockInPlace(module);
        bool factoryHasNotSettled = runMockThatWaits(module);
        RETURN_IF_EXCEPTION(scope, false);
        if (factoryHasNotSettled) [[unlikely]] {
            throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.pathText(module)));
            return false;
        }
        if (mockInPlace(module)) {
            ensureLoaded(module).wasRequiredUnderMockInPlace = true;
            noteRequireOfMockLaidInPreload(module);
        }
        if (!liesInRequireMap && m_modulesWithMockToRun.get(module.index()) && getsOriginalBesideMock(module, module)) {
            JSValue original = answerRequireWithOriginal(module, module);
            RETURN_IF_EXCEPTION(scope, false);
            setCommonJSExports(module, original);
        }
    }
    if (someFactoryHasNotSettled) [[unlikely]] {
        bool refusesEveryRequire = false;
        if (callOfFactoryThatHasNotSettled(module, &refusesEveryRequire) && refusesEveryRequire) {
            throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.pathText(module)));
            return false;
        }
    }
    if (mockInPlace(module)) [[unlikely]] {
        noteAskUnderMockInPlace(module);
        if (!m_innermostFilesBeingRun)
            ensureLoaded(module).wasRequiredUnderMockInPlace = true;
        if (!isBegunForStatement(module))
            noteRequireOfMockLaidInPreload(module);
    }
    if (m_filesToLoadAgain.get(module.index())) [[unlikely]] {
        JSGlobalObject* realm = m_instance.globalObject;
        JSPromise* loaded = nullptr;
        {
            SetForScope isAskingHost(m_isAskingHost, true);
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
        record->clearHoldsStatus();
        m_storedStatuses[module.index()] = static_cast<uint8_t>(m_instance.modules.evaluatingMark);
        return true;
    }
    scope.release();
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
    for (const FilesBeingRun* load = m_innermostFilesBeingRun; load; load = load->outer) {
        if (std::optional<ModuleID> fileOrFace = fileOrFaceEvaluatedIn(*load))
            functor(*fileOrFace);
    }
}

bool Modules::isBegunForStatement(ModuleID fileOrFace) const
{
    ModuleID file = fileOf(fileOrFace);
    if (fileOrFace != file)
        return true;
    std::optional<ModuleID> face = m_table.faceOfFile(file);
    return face && status(*face) == ModuleStatus::Evaluating;
}

std::optional<ModuleID> Modules::fileOrFaceEvaluatedIn(const FilesBeingRun& load) const
{
    auto isBeingEvaluated = [&](ModuleID fileOrFace) {
        return status(fileOrFace) == ModuleStatus::Evaluating || status(fileOf(fileOrFace)) == ModuleStatus::Evaluating;
    };
    if (load.fileOrFace)
        return isBeingEvaluated(*load.fileOrFace) ? load.fileOrFace : std::nullopt;
    for (const ModuleRequestData& request : m_table.requests(load.root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
            if (endsFilesThatRunAhead(request))
                break;
            continue;
        }
        if (isBeingEvaluated(imported))
            return imported;
    }
    return std::nullopt;
}

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

bool Modules::importsFileThatRunsAhead(ModuleID root) const
{
    Vector<ModuleID, 4> running;
    forEachFileEvaluatedByLoader([&](ModuleID file) {
        running.append(file);
    });
    if (running.isEmpty())
        return false;
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Vector<uint32_t, 32> found { root.index() };
    for (size_t index = 0; index < found.size(); ++index) {
        ModuleID module(found[index]);
        if (running.contains(fileOf(module)))
            return true;
        if (!data(module).isESModuleOfProgram())
            continue;
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (!request.isRequire && !request.isHoisted && !seen.quickSet(request.target()))
                found.append(request.target());
        }
    }
    return false;
}

bool Modules::comesToLoadOf(FilesBeingRun& files, ModuleID required)
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
    if (!m_innermostFilesBeingRun) [[likely]]
        return false;
    for (const ModuleRequestData& request : m_table.requests(root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
            if (endsFilesThatRunAhead(request))
                return false;
            continue;
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
        if (isCommonJSFileOrFace(module))
            return hasNotBegunAsFile(module) ? State::Unrequested : State::Recorded;
        return isNotLoaded(module) && !isMocked(module) ? State::Unrequested : State::Settled;
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
            if (!isCommonJSFileOrFace(file))
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
                    bool isFile = isCommonJSFileOrFace(target);
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

bool Modules::evaluateImportedCommonJSFiles(ModuleID root, JobsOfImportedFiles jobs, ErrorOfImportedFile error, ModuleGraphLoadingState* load)
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
            m_rootWhoseFilesRanAhead = root;
            return false;
        }
    }

    MocksBelowRoot mocks = mocksBelow(root);

    bool didCaptureLibraryModules = false;
    for (const ModuleRequestData& request : m_table.requests(root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
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
        if (data(imported).capturesExportsBehindEvaluation()) {
            captureExportsBehindEvaluation(imported);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }

    bool didRunFile = false;
    FilesBeingRun filesBeingRun { root, std::nullopt, m_innermostFilesBeingRun, jobs == JobsOfImportedFiles::WaitForLoader, load };
    filesBeingRun.isAheadOfStepsOfLoader = jobs == JobsOfImportedFiles::StepsOfLoaderWaitForLoader;
    SetForScope innermostFilesBeingRun(m_innermostFilesBeingRun, &filesBeingRun);
    SetForScope modulesWhoseFilesRunAheadOfLoader(vm.aotState().modulesWhoseFilesRunAheadOfLoader, filesBeingRun.isAheadOfJobsOfLoader ? this : vm.aotState().modulesWhoseFilesRunAheadOfLoader);
    if (jobs == JobsOfImportedFiles::WaitForLoader && data(root).evaluatesItself() && data(root).isEntryPoint() && !doOtherJobsWait() && !m_rootWhoseFilesRun) {
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
                if (doOtherJobsWait()) {
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
    std::span<const ModuleRequestData> requests = m_table.requests(root);
    Vector<uint32_t, 16> placesOfFiles;
    for (uint32_t place = 0; jobs == JobsOfImportedFiles::StepsOfLoaderWaitForLoader && place < requests.size() && requests[place].isHoisted; ++place) {
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
        if (request.isHoisted && m_storedStatuses[request.target()] == static_cast<uint8_t>(StoredModuleStatus::Evaluated) && !mockInPlace(request.targetModule()))
            continue;
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (request.isDeferred || !isCommonJSFileOrFace(imported)) {
            if (endsFilesThatRunAhead(request))
                break;
            continue;
        }
        if (mocks.hides(imported)) [[unlikely]]
            continue;
        bool isOwedBesideMock = mockInPlace(imported) && hasImporterBelowThatGetsOriginal(root, imported);
        ModuleStatus statusAtStart = status(imported);
        if (!isOwedBesideMock && statusAtStart != ModuleStatus::Unlinked && statusAtStart != ModuleStatus::Linked && statusAtStart != ModuleStatus::Failed)
            continue;
        if (takeThrowBehindFetch(imported)) [[unlikely]] {
            makeRecordWithoutExports(imported);
            continue;
        }
        didRunFile |= statusAtStart != ModuleStatus::Failed;
        size_t numberOfJobs = numberOfJobsThatWouldWait(jobs);
        bool hadQueuedTicks = hasQueuedTicks();
        filesBeingRun.fileOrFace = imported;
        filesBeingRun.numberOfJobsInFrontOfFile = numberOfJobs;
        if (!isOwedBesideMock) [[likely]] {
            if (storedStatus(imported) < StoredModuleStatus::InRecord && data(imported).evaluatesItself())
                evaluateByCallingBody(imported);
            else
                evaluateModuleInRecord(imported);
            isOwedBesideMock = !scope.exception() && mockInPlace(imported) && hasImporterBelowThatGetsOriginal(root, imported);
        }
        if (isOwedBesideMock) [[unlikely]] {
            runFileBesideMock(fileOf(imported));
            RETURN_IF_EXCEPTION(scope, didRunFile);
        }
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
            numberOfJobs = 0;
            if (error == ErrorOfImportedFile::IsThrown) {
                resumeDeferredLoadsWithoutImporter(imported);
                exception = scope.exception();
                hasDeferredLoads = false;
            }
        }
        if (exception) [[unlikely]] {
            if (statusAtStart == ModuleStatus::Failed || vm.isTerminationException(exception) || !wasImportedWhileItRan(m_table.fileBehindFace(imported).value_or(imported))) {
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

void Modules::holdJobsBehind(size_t numberOfJobs, ModuleID file, JobsOfImportedFiles jobs)
{
    VM& vm = *m_instance.vm;
    m_fileWhoseJobsWaitForLoader = m_table.fileBehindFace(file).value_or(file);
    if (jobs == JobsOfImportedFiles::StepsOfLoaderWaitForLoader) {
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

void Modules::handGraphToLoader(FilesBeingRun& files, std::optional<ModuleID> finishedFile)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    files.isAheadOfJobsOfLoader = false;
    std::optional<ModuleID> running = finishedFile ? finishedFile : fileOrFaceEvaluatedIn(files);
    if (!running || !files.load)
        return;

    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    Ref<MicrotaskQueue> jobsOfFile = MicrotaskQueue::create(vm);
    queue.moveTasksBehind(files.numberOfJobsInFrontOfFile, jobsOfFile.get());
    m_canJobsOfFileTellWhenTextsCome |= !jobsOfFile->isEmpty() && isTestRun();
    m_rootWhoseFilesRanAhead = files.root;
    files.fileHandedToLoader = running;
    {
        DeferTerminationForAWhile deferTermination(vm);
        SetForScope fileBeingHandedToLoader(m_fileBeingHandedToLoader, fileOf(*running));
        SetForScope noSynchronousLoad(vm.m_synchronousModuleQueueSetAside, vm.m_synchronousModuleQueue);
        ModuleRecord* record = recordIfExists(files.root);
        files.load->forgetVisited(record);
        files.load->setDrainingInnerLoad(false);
        loader()->innerModuleLoading(m_instance.globalObject, files.load, record);
        record->didGoOnWithLoadWhileRequestsWereListed(*files.load);
        scope.assertNoException();
        runStepsOfLoaderInFrontOf(fileOf(*running));
        scope.assertNoException();
    }
    jobsOfFile->moveTasksBehind(0, queue);
}

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
    auto hasFileYetToEnd = [&](const Identifier& key) {
        std::optional<ModuleID> fetched = findByKey(key);
        return fetched && isCommonJSFileOrFace(*fetched) && status(fileOf(*fetched)) < ModuleStatus::Evaluated;
    };
    Vector<VM::SynchronousModuleTask, 2> stepsOfFilesYetToEnd;
    size_t taken = 0;
    while (taken < steps.size()) {
        if (steps[taken].task == InternalMicrotask::ModuleLoadLinkEvaluateSettled)
            break;
        VM::SynchronousModuleTask step = steps[taken++];
        if (step.task == InternalMicrotask::ModuleRegistryFetchSettled) {
            const Identifier& key = uncheckedDowncast<ModuleRegistryEntry>(step.arg2)->key();
            if (hasKey(file, key))
                break;
            if (hasFileYetToEnd(key)) {
                stepsOfFilesYetToEnd.append(step);
                continue;
            }
        }
        runInternalMicrotask(m_instance.globalObject, vm, step.task, step.payload, step.arg0, step.arg1, step.arg2, step.arg3);
        scope.assertNoException();
    }
    steps.removeAt(0, taken);
    steps.insertVector(0, stepsOfFilesYetToEnd);
}

void Modules::runStepsOfLoaderInFrontOf(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    DeferTerminationForAWhile deferTermination(vm);
    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    Ref<MicrotaskQueue> turn = MicrotaskQueue::create(vm);
    Ref<MicrotaskQueue> stepsOfFilesYetToEnd = MicrotaskQueue::create(vm);
    auto hasFileYetToEnd = [&](const Identifier& key) {
        std::optional<ModuleID> fetched = findByKey(key);
        return fetched && isCommonJSFileOrFace(*fetched) && status(fileOf(*fetched)) < ModuleStatus::Evaluated;
    };
    while (!queue.isEmpty()) {
        queue.moveTasksBehind(0, turn.get());
        size_t stepsTaken = 0;
        bool hasComeToFile = !turn->allOf([&](const QueuedTask& job) {
            if (job.globalObject()->microtaskRunnability() == QueuedTaskResult::Discard) {
                ++stepsTaken;
                return true;
            }
            auto arguments = job.arguments();
            if (job.job() == InternalMicrotask::ModuleRegistryFetchSettled) {
                const Identifier& key = uncheckedDowncast<ModuleRegistryEntry>(arguments[2])->key();
                if (hasKey(file, key))
                    return false;
                if (hasFileYetToEnd(key)) {
                    ++stepsTaken;
                    stepsOfFilesYetToEnd->enqueue(QueuedTask(job));
                    return true;
                }
            }
            ++stepsTaken;
            runInternalMicrotask(job.globalObject(), vm, job.job(), job.payload(), arguments[0], arguments[1], arguments[2], arguments[3]);
            scope.assertNoException();
            return true;
        });
        if (hasComeToFile) {
            Ref<MicrotaskQueue> queuedInThisTurn = MicrotaskQueue::create(vm);
            queue.moveTasksBehind(0, queuedInThisTurn.get());
            stepsOfFilesYetToEnd->moveTasksBehind(0, queue);
            turn->moveTasksBehind(stepsTaken + 1, queue);
            queuedInThisTurn->moveTasksBehind(0, queue);
        }
        turn->clear();
        if (hasComeToFile)
            return;
    }
    stepsOfFilesYetToEnd->moveTasksBehind(0, queue);
}

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

void Modules::didFailToLoadGraph(ModuleID root)
{
    m_loadsInFlight.didLoad(root);
    if (!m_levelsOfLoads.isEmpty()) [[unlikely]] {
        m_levelsOfLoads.removeIf([&](auto& entry) {
            return entry.value->plan->found[0].module == root;
        });
    }
    letGoGraphsHeldBehindEarlierCalls();
    if (m_rootWhoseFilesRanAhead != root)
        return;
    m_rootWhoseFilesRanAhead = std::nullopt;
    queueJobsThatWaitForLoader();
    resumeDeferredLoadsOfFinishedFiles();
}

void Modules::didLoadGraph(ModuleID root)
{
    if (m_rootWhoseFilesRanAhead == root) [[unlikely]] {
        m_rootWhoseFilesRanAhead = std::nullopt;
        resumeDeferredLoadsOfFinishedFiles();
    }
    m_loadsInFlight.didLoad(root);
    letGoGraphsHeldBehindEarlierCalls();
}

unsigned Modules::numberOfGraphsBeingLoaded()
{
    m_loadsInFlight.removeSettledIf([&](const LoadInFlight& load) {
        if (m_rootWhoseFilesRanAhead == load.root)
            m_rootWhoseFilesRanAhead = std::nullopt;
        return true;
    });
    bool hostLoadsGraph = m_rootWhoseFilesRanAhead && !m_loadsInFlight.hasRoot(*m_rootWhoseFilesRanAhead);
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
    return isCommonJSFileOrFace(module) && (status(module) == ModuleStatus::Unlinked || status(module) == ModuleStatus::Linked);
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
        return m_rootWhoseFilesRanAhead != earlier.root;
    });
    m_loadsInFlight.append(*m_instance.vm, load, root, settling).stepsOfLoaderAtCall = m_stepsOfLoaderAtCallWhoseTextHasCome.value_or(m_stepsOfLoader);
}

Modules::LoaderForRequire Modules::loaderForRequire(ModuleID module)
{
    if (!isLoadedWithRequests(module))
        return LoaderForRequire::IsNotNeeded;
    if (data(module).isLoadedByLoader() || statementThatEndsLoad(module))
        return LoaderForRequire::IsNeeded;
    if (numberOfGraphsBeingLoaded() && (m_rootWhoseFilesRanAhead || comesToLoadInFlight(module)))
        return LoaderForRequire::IsNeeded;
    for (const FilesBeingRun* files = m_innermostFilesBeingRun; files; files = files->outer) {
        if (files->fileOrFace == files->root)
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
    return storedStatus(module) == StoredModuleStatus::Unlinked && isNotLoadedWithoutStatus(module, data(module));
}

void Modules::noteModulesLoadedWith(ModuleID root)
{
    if (!data(root).isESModuleOfProgram() || (data(root).isFaceOfFile() && !m_goesModuleByModule) || wasLoadedWithGraph(root) || m_rootWhoseLoadIsSwept)
        return;
    if (m_goesModuleByModule) {
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

void Modules::goModuleByModuleFromNowOn()
{
    if (m_goesModuleByModule)
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
    m_goesModuleByModule = true;
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
        didGiveUpMomentLinked(std::exchange(m_momentsOfLoads[module], moment));
        m_loadedInPreload.quickSet(module, isAheadOfTestFiles);
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
            if (!data(imported).isESModuleOfProgram())
                continue;
            noteAskForKey(imported, ModuleID(module), LoadByName::Import);
            if (mockInPlace(imported))
                noteImportUnderMockInPlace(imported);
            if (!m_modulesLoadedBesideGroups.quickSet(imported.index()))
                found.append(imported.index());
        }
    }
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Modules::isLeftAsLoaded(ModuleID module) const
{
    ModuleRecord* record = isCommonJSFileOrFace(module) ? nullptr : recordIfExists(module);
    return record && record->status() == CyclicModuleRecord::Status::Unlinked && !data(module).capturesExports();
}

ALWAYS_INLINE_INTO_OTHER_FILES bool Modules::isModuleWithoutRecord(const ModuleData& moduleData, StoredModuleStatus stored) const
{
    return moduleData.kind == ModuleKind::SourceText && stored != StoredModuleStatus::InRecord && !moduleData.isFaceOfFile();
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
    if (m_placesSeenByPlans.size() != m_table.size() || m_firstPlaceOfNextPlan > std::numeric_limits<uint32_t>::max() - 3 - m_table.size()) {
        m_placesSeenByPlans.forgetAll(m_table.size());
        m_firstPlaceOfNextPlan = 1;
    }
    return m_firstPlaceOfNextPlan++;
}

bool Modules::planLevelsOfLoad(ModuleID root)
{
    using Found = PlanOfLoad::Found;
    using Level = PlanOfLoad::Level;
    if (data(root).reachesUncompiledCode() || !m_textsOnTheirWay.isEmpty())
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
        m_firstPlaceOfNextPlan = firstPlaceFound + plan->found.size();
    });
    auto& found = plan->found;
    auto& levels = plan->levels;
    Vector<uint32_t, 64> firstRequestOfPlace;
    Vector<uint32_t, 256> requestedPlaces;
    placesSeen[root.index()] = firstPlaceFound;
    found.append(Found { root, 0, 0, false, false, false, false });
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
                found.append(Found { target, importer, laterRequests, isFile, isFile && isCommonJSFileOrFace(target), false, false });
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
        root->setStepsBehindLoad(plan.steps - stepsToLevel);
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
    if (!isLoadedWithRequests(moduleOnWay) || isCommonJSFileOrFace(moduleOnWay)) {
        m_filesLoadedWithoutLoader.set(moduleOnWay.index());
        m_levelsOfLoads.remove(moduleOnWay);
    }
}

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

void Modules::noteLoadOfFile(ModuleID module, LoadOfFile load)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID file = m_table.fileBehindFace(module).value_or(module);
    if ((status(file) != ModuleStatus::Evaluating && status(module) != ModuleStatus::Evaluating) || asyncLoadMustWaitFor(file) || file == m_fileBeingHandedToLoader)
        return;
    bool isFetchedInsideCall = load == LoadOfFile::IsRootOfImport && (hostLoadsSynchronously(file) || (host().keepsTexts && host().keepsTexts(m_instance.globalObject)));
    if (load == LoadOfFile::IsRootOfImport && isFileOfProgram(file) && !m_instance.vm->synchronousModuleQueue() && !isFetchedInsideCall)
        return;
    ensureLoaded(file).wasLoadedWhileItRan = true;
    ensureLoaded(file).wasLoadedSynchronouslyWhileItRan |= !!m_instance.vm->synchronousModuleQueue();
    if (isFetchedInsideCall && !m_instance.vm->synchronousModuleQueue()) {
        ensureLoaded(file).wasFetchedWhileItRan = true;
        return;
    }
    if (!data(file).mayBeImportedWhileItRuns() || load == LoadOfFile::IsRequested)
        return;
    std::optional<ModuleID> face = m_table.faceOfFile(file);
    if (!face) {
        if (!didCaptureExports(file))
            RELEASE_AND_RETURN(scope, captureExports(file));
        return;
    }
    std::optional<uint32_t> countAtStart = ensureLoaded(file).synchronousLoadCountAtStart;
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
    ModuleID file = m_table.fileBehindFace(module).value_or(module);
    bool hasBegun = status(module) >= ModuleStatus::Evaluating || status(file) >= ModuleStatus::Evaluating;
    bool hasTurn = !m_instance.vm->synchronousModuleQueue() && !isFetchedInsideCall(file) && isAskedForBehindLoadInFlight(module);
    bool isModuleToStatement = load == LoadOfFile::IsRequested && data(file).isModuleToImports() && !m_instance.vm->synchronousModuleQueue();
    if (((load == LoadOfFile::IsRootOfImport && hasTurn) || isModuleToStatement) && !hasBegun && !record->runsWhenLoaded())
        record->setRunsInItsTurn();
    else
        record->setRunsWhenLoaded();
    return record;
}

bool Modules::isAskedForBehindLoadInFlight(ModuleID module)
{
    ModuleID file = m_table.fileBehindFace(module).value_or(module);
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
    if (data(m_table.fileBehindFace(module).value_or(module)).isModuleToImports())
        return;
    for (ModuleID held : { module, m_table.fileBehindFace(module).value_or(module) }) {
        if (ModuleRecord* record = recordIfExists(held); record && record->takeRunsInItsTurn()) [[unlikely]]
            return evaluateFilesOfLevel(held, true);
    }
}

void Modules::evaluateLoadedCommonJSFile(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isCommonJSFileOrFace(module))
        return;
    if (m_fileWhoseJobsWaitForLoader && m_fileWhoseJobsWaitForLoader == m_table.fileBehindFace(module).value_or(module)) [[unlikely]]
        queueJobsThatWaitForLoader();
    ModuleStatus statusAtStart = status(module);
    if (statusAtStart == ModuleStatus::Evaluating)
        RELEASE_AND_RETURN(scope, noteLoadOfFile(module));
    if (LoadedModule* loaded = loadedIfExists(fileOf(module)); loaded && std::exchange(loaded->wasFetchedWhileItRan, false) && status(fileOf(module)) != ModuleStatus::Evaluated) {
        for (ModuleID fileOrFace : { fileOf(module), module }) {
            if (status(fileOrFace) != ModuleStatus::Failed)
                setStatus(fileOrFace, ModuleStatus::Evaluated);
            dropErrorOfFile(fileOrFace);
        }
        return;
    }
    if (takeThrowBehindFetch(module)) [[unlikely]]
        return makeRecordWithoutExports(module);
    noteLoadOfFile(module);
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, void());
    if (statusAtStart == ModuleStatus::Failed && loader()->isMakingRecordInline(moduleKey(module)))
        return;
    if (statusAtStart != ModuleStatus::Unlinked && statusAtStart != ModuleStatus::Linked && statusAtStart != ModuleStatus::Failed)
        return;
    {
        FilesBeingRun filesBeingRun { module, module, m_innermostFilesBeingRun };
        SetForScope innermostFilesBeingRun(m_innermostFilesBeingRun, &filesBeingRun);
        m_loadsInFlight.didRunFile();
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
        bool takesExportsOfFile = data(module).kind == ModuleKind::CommonJs ? !m_table.faceOfFile(module) : data(module).capturesExports();
        if (takesExportsOfFile && !didCaptureExports(module))
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

void Modules::captureNoExports(ModuleID file)
{
    bool hasEmptyRecord = ensureLoaded(file).wasReachedBySecondLoad;
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

bool Modules::wasImportedWhileItRan(ModuleID file) const
{
    if (isEvicted(file))
        return false;
    if (LoadedModule* loaded = loadedIfExists(file); loaded && (loaded->wasLoadedWhileItRan || loaded->wasReachedBySecondLoad))
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
    return loaded && (loaded->wasLoadedSynchronouslyWhileItRan || loaded->wasReachedBySecondLoad);
}

void Modules::evaluateCommonJSForLoader(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::optional<ModuleID> face = m_table.faceOfFile(file);
    bool hadCapturedExports = face && didCaptureExports(*face);
    evaluateCommonJS(file);
    Exception* exception = scope.exception();
    if (!exception) [[likely]] {
        if (face) {
            m_filesLoadedWithoutLoader.set(face->index());
            bool wasImportedBack = !hadCapturedExports && didCaptureExports(*face);
            evaluateLoadedCommonJSFile(*face);
            RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, void());
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
        record->takeStatus();
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
            record->takeStatus();
            capability->fulfill(vm, jsUndefined());
        }
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
        if (record->status() == CyclicModuleRecord::Status::New || record->status() == CyclicModuleRecord::Status::Unlinked)
            record->setStatus(CyclicModuleRecord::Status::Linked);
        RELEASE_AND_RETURN(scope, record->evaluate(realm, referrerAsyncOrder.value_or(-1), importPromise));
    }

    if (moduleData.hasExportTable() && !moduleData.needsEvaluation()) {
        noteModulesLoadedWith(root);
        setStoredStatus(root, StoredModuleStatus::Evaluated);
        if (runsBesideMockFor(root, std::nullopt)) [[unlikely]] {
            m_modulesThatRanBesideMock.set(root.index());
            m_modulesOwedBesideMock.clear(root.index());
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
    if (!host().isImportOfMainModule || mainModuleWaitsForItsImport() || !host().isImportOfMainModule(realm, entryPoint))
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
        host().runMainModule(realm, entryPoint);
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
    host().runMainModule(m_instance.globalObject, m_importOfMainModule);
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
        if (isMocked(file) || mockInPlace(file) || m_modulesWithMockToRun.get(file.index()))
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

static JSC_DECLARE_HOST_FUNCTION(didSettleFactoriesThatRequireLeftInModuleEvaluation);
static JSC_DECLARE_HOST_FUNCTION(didFailFactoryThatRequireLeftInModuleEvaluation);

JSC_DEFINE_HOST_FUNCTION(didSettleFactoriesThatRequireLeftInModuleEvaluation, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didSettleFactoryThatRequireLeft(callFrame->argument(1).asUInt32());
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(didFailFactoryThatRequireLeftInModuleEvaluation, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didFailFactoryThatRequireLeft(callFrame->argument(1).asUInt32(), callFrame->argument(0));
    return JSValue::encode(jsUndefined());
}

void Modules::didSettleFactoryThatRequireLeft(unsigned number)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto find = [&] { return m_waitsThatRequiresLeft.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; }); };
    size_t left = find();
    if (left == notFound)
        return;
    --m_waitsThatRequiresLeft[left].factoriesYetToSettle;
    ModuleID required = m_waitsThatRequiresLeft[left].required;
    bool isForgotten = !m_waitsThatRequiresLeft[left].keepsError;
    didGoOnBehindFactories();
    runMocksBelow(required);
    Exception* thrown = scope.exception();
    if (thrown && (vm.isTerminationException(thrown) || !scope.tryClearException()))
        return;
    if (isForgotten)
        dropErrorsOfLoadsEndedByMocks();
    left = find();
    if (left == notFound)
        return;
    if (thrown) {
        m_waitsThatRequiresLeft[left].wait->reject(vm, thrown->value());
        return;
    }
    if (m_waitsThatRequiresLeft[left].factoriesYetToSettle)
        return;
    ModuleStatus current = status(required);
    bool wasMockLaidSince = false;
    for (size_t fetched : m_waitsThatRequiresLeft[left].fetchedAsThemselves)
        wasMockLaidSince |= isMocked(ModuleID(fetched));
    bool liesOnCycle = false;
    bool someMockBelowIsNotInPlace = false;
    for (size_t below : reachedByStatements(required, AtMock::Stops)) {
        liesOnCycle |= below != required.index() && reachedByStatements(ModuleID(below), AtMock::Stops).get(required.index());
        someMockBelowIsNotInPlace |= m_modulesWithMockToRun.get(below) || (isMocked(ModuleID(below)) && !mockInPlace(ModuleID(below)));
    }
    if ((current == ModuleStatus::Unlinked || current == ModuleStatus::Linked) && !isEvicted(required) && !isMocked(required) && !wasMockLaidSince && !liesOnCycle && !someMockBelowIsNotInPlace) {
        JSPromise* evaluation = evaluate(required, std::nullopt, nullptr);
        if (Exception* failure = scope.exception()) {
            if (vm.isTerminationException(failure) || !scope.tryClearException())
                return;
        } else if (evaluation)
            evaluation->markAsHandled();
        left = find();
        if (left == notFound)
            return;
    }
    m_waitsThatRequiresLeft[left].wait->fulfill(vm, jsUndefined());
}

void Modules::didFailFactoryThatRequireLeft(unsigned number, JSValue error)
{
    size_t left = m_waitsThatRequiresLeft.findIf([&](const auto& each) { return each.number == number && each.wait->status() == JSPromise::Status::Pending; });
    if (left == notFound)
        return;
    didGoOnBehindFactories();
    JSPromise* wait = m_waitsThatRequiresLeft[left].wait.get();
    if (m_waitsThatRequiresLeft[left].keepsError)
        keepErrorOfFactoryThatRequireLeft(m_waitsThatRequiresLeft[left].required, error);
    wait->reject(*m_instance.vm, error);
}

unsigned Modules::hearFactoriesBelowLoadThatRequireLeft(ModuleID required, unsigned number)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue state;
    Vector<JSPromise*, 4> heard;
    while (true) {
        JSPromise* factory = factoryThatWaitsBelow(required, FactoryAskedForBy::LoadThatRequireLeft, state);
        RETURN_IF_EXCEPTION(scope, 0);
        if (!factory || heard.contains(factory))
            break;
        heard.append(factory);
    }
    for (JSPromise* factory : heard) {
        JSFunction* didSettle = JSFunction::create(vm, realm, 2, String(), didSettleFactoriesThatRequireLeftInModuleEvaluation, ImplementationVisibility::Private);
        JSFunction* didFail = JSFunction::create(vm, realm, 2, String(), didFailFactoryThatRequireLeftInModuleEvaluation, ImplementationVisibility::Private);
        factory->performPromiseThenWithContext(vm, realm, didSettle, didFail, jsUndefined(), jsNumber(number));
    }
    return heard.size();
}

void Modules::keepErrorOfFactoryThatRequireLeft(ModuleID required, JSValue error)
{
    keepErrorOfLoad(required, error, [&](ModuleID each) { return isMocked(each) && m_modulesWithMockThatWaited.get(each.index()); });
    if (!m_modulesWithLoadEndedByMock.set(required.index()))
        ensureLoaded(required).errorKeptByLoader.set(*this, error);
}

bool Modules::stillWaitsForWhatItGaveUpOn(ModuleID required) const
{
    bool isInFlight = m_waitsThatRequiresLeft.containsIf([&](const auto& left) { return left.required == required && left.wait->status() == JSPromise::Status::Pending; });
    bool waits = false;
    for (const RequireThatGaveUp& pair : m_requiresThatGaveUp) {
        if (pair.required != required || pair.mocked == required || m_modulesWithMockInPlace.get(pair.mocked.index()))
            continue;
        uint64_t call = callOfFactoryThatHasNotSettled(pair.mocked);
        if (!call || (!isInFlight && !m_requiresThatGaveUp.containsIf([&](const RequireThatGaveUp& other) { return other.required == required && other.mocked == pair.mocked && other.call == call; })))
            return false;
        waits = true;
    }
    return waits;
}

bool Modules::isOfCallsThatAreGone(const WaitThatRequireLeft& left)
{
    return !left.callsOfFactories.isEmpty() && std::ranges::none_of(left.callsOfFactories, [&](const auto& call) { return callOfFactoryThatHasNotSettled(call.first) == call.second; });
}

void Modules::endWaitsThatWentOnForLaterMockOf(ModuleID mocked)
{
    if (m_waitsThatRequiresLeft.isEmpty()) [[likely]]
        return;
    uint64_t callThatSettles = callOfFactoryThatHasNotSettled(mocked);
    for (WaitThatRequireLeft& left : m_waitsThatRequiresLeft)
        left.hasEndedWithLaterMock |= left.callsOfFactories.containsIf([&](const auto& call) { return call.first == mocked && call.second != callThatSettles; });
}

void Modules::noteThatFetchInFlightOutlivesMock(ModuleID file)
{
    m_filesWhoseFetchInFlightOutlivesMock.set(file.index());
    for (WaitThatRequireLeft& left : m_waitsThatRequiresLeft) {
        if (left.callsOfFactories.containsIf([&](const auto& call) { return call.first == file; }))
            left.fileWhoseMockWasTakenOff = file;
    }
}

void Modules::refuseRequireAgain(ModuleID asked, ModuleID above)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    m_filesWhoseRequireThrewBesideFactories.clear(asked.index());
    m_waitsThatRequiresLeft.removeAllMatching([](const auto& left) { return left.wait->status() != JSPromise::Status::Pending; });
    bool isHeardByWait = m_waitsThatRequiresLeft.containsIf([&](const auto& left) { return left.required == above && !isOfCallsThatAreGone(left); });
    if (isHeardByWait || m_waitsThatRequiresLeft.containsIf([&](const auto& left) { return left.required == above && !hasEnded(left); })) {
        JSValue state;
        JSPromise* original = factoryThatWaitsBelow(asked, FactoryAskedForBy::RequireThatIsRefused, state);
        RETURN_IF_EXCEPTION(scope, void());
        if (original)
            original->performPromiseThen(vm, realm, jsUndefined(), jsUndefined(), JSPromise::create(vm, realm->promiseStructure()));
    }
    if (!isHeardByWait)
        RELEASE_AND_RETURN(scope, waitForFactoriesThatRequireLeft(above));
}

void Modules::waitForFactoriesThatRequireLeft(ModuleID required)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    m_waitsThatRequiresLeft.removeAllMatching([](const auto& left) { return left.wait->status() != JSPromise::Status::Pending; });
    if (size_t left = m_waitsThatRequiresLeft.findIf([&](const auto& each) { return each.required == required && !isOfCallsThatAreGone(each); }); left != notFound) {
        m_waitsThatRequiresLeft[left].wait->performPromiseThen(vm, realm, jsUndefined(), jsUndefined(), JSPromise::create(vm, realm->promiseStructure()));
        return;
    }
    unsigned number = ++m_numberOfWaitsThatRequiresHaveLeft;
    unsigned heard = hearFactoriesBelowLoadThatRequireLeft(required, number);
    RETURN_IF_EXCEPTION(scope, void());
    if (!heard)
        return;
    JSPromise* wait = JSPromise::create(vm, realm->promiseStructure());
    wait->markAsHandled();
    BitVector fetchedAsThemselves(m_table.size());
    for (size_t below : reachedByStatements(required, AtMock::Stops)) {
        if (!isMocked(ModuleID(below)))
            fetchedAsThemselves.set(below);
    }
    Vector<std::pair<ModuleID, uint64_t>, 1> callsOfFactories;
    for (size_t below : reachedByStatements(required, AtMock::Stops)) {
        if (uint64_t call = isMocked(ModuleID(below)) ? callOfFactoryThatHasNotSettled(ModuleID(below)) : 0)
            callsOfFactories.append({ ModuleID(below), call });
    }
    m_waitsThatRequiresLeft.append({ required, Strong<JSPromise>(vm, wait), true, number, heard, WTF::move(fetchedAsThemselves), WTF::move(callsOfFactories) });
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
        factoryHasNotSettled = runMocksBelow(module);
    }
    RETURN_IF_EXCEPTION(scope, void());
    const ModuleData& moduleData = data(module);

    auto throwAsyncModuleError = [&] {
        throwException(realm, scope, host().createAsyncModuleRequiredError(realm, m_table.pathText(module)));
    };
    if (factoryHasNotSettled && reason == SynchronousEvaluationReason::Require) [[unlikely]] {
        noteThatRequireGaveUpOn(module);
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
        if (moduleData.hasTopLevelAwait()) {
            throwAsyncModuleError();
            return;
        }
        RELEASE_AND_RETURN(scope, captureExportsOfFilesBeingEvaluated(module));
    case ModuleStatus::EvaluatingAsync:
        if (!isMocked(module) || !m_modulesThatRanBesideMock.get(module.index()))
            throwAsyncModuleError();
        return;
    case ModuleStatus::Unlinked:
        break;
    case ModuleStatus::Linked:
        captureExportsOfFilesBeingEvaluated(module);
        RETURN_IF_EXCEPTION(scope, void());
        break;
    }

    bool givesLoaderFileThatRunsAhead = reason == SynchronousEvaluationReason::Require && m_innermostFilesBeingRun && importsFileThatRunsAhead(module);
    if (reason == SynchronousEvaluationReason::Require && m_innermostFilesBeingRun && m_innermostFilesBeingRun->isAheadOfStepsOfLoader && (givesLoaderFileThatRunsAhead || comesToLoadOf(*m_innermostFilesBeingRun, module))) [[unlikely]] {
        handSynchronousLoadToLoader(*m_innermostFilesBeingRun);
        RETURN_IF_EXCEPTION(scope, void());
    }
    givesLoaderFileThatRunsAhead &= !m_rootWhoseFilesRanAhead;
    if (std::optional<ModuleID> root = givesLoaderFileThatRunsAhead ? rootOfRunningFileThatHoldsCycleBelow(module) : std::nullopt) [[unlikely]] {
        evaluateRootThatWaitedForFile(*root);
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
    SetForScope activeSynchronousLoad(m_activeSynchronousLoad, ++m_synchronousLoadCount);
    SetForScope outermostSynchronousLoad(m_outermostSynchronousLoad, m_outermostSynchronousLoad ? m_outermostSynchronousLoad : m_activeSynchronousLoad);
    auto leaveLoad = [&] {
        if (!queue)
            return;
        if (m_rootWhoseFilesRanAhead == module)
            m_rootWhoseFilesRanAhead = std::nullopt;
        VM::SynchronousModuleQueue* steps = m_instance.vm->m_synchronousModuleQueue;
        if (m_stepsThatWaitForLoader == steps) {
            m_stepsThatWaitForLoader = nullptr;
            m_fileWhoseJobsWaitForLoader = std::nullopt;
        }
        leaveStepsOfLoaderToJobs();
        queue.reset();
    };
    auto didLeaveLoad = makeScopeExit(leaveLoad);
    if (loaderForRequire != LoaderForRequire::IsNeeded) {
        evaluateImportedCommonJSFiles(module, m_instance.vm->synchronousModuleQueue() ? JobsOfImportedFiles::StepsOfLoaderWaitForLoader : JobsOfImportedFiles::StayQueued, ErrorOfImportedFile::IsLeftToLoader);
        RETURN_IF_EXCEPTION(scope, void());
        needsLoader |= m_rootWhoseFilesRanAhead == module;
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
        SetForScope rootThatCompiledCodeLoads(m_rootThatCompiledCodeLoads, std::optional(module));
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
            else if (givesLoaderFileThatRunsAhead) [[unlikely]]
                RELEASE_AND_RETURN(scope, evaluateRootThatWaitedForFile(m_innermostFilesBeingRun->root));
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
        if (givesLoaderFileThatRunsAhead) [[unlikely]]
            RELEASE_AND_RETURN(scope, evaluateRootThatWaitedForFile(m_innermostFilesBeingRun->root));
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

void Modules::evaluateRootThatWaitedForFile(ModuleID root)
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
        RequestToHostScope requestToHost(*this);
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

void Modules::didMockTakeDataFileOutOfLoader(ModuleID file)
{
    if (data(file).kind != ModuleKind::Data)
        return;
    m_dataFilesThatMockTookOutOfLoader.set(file.index());
}

bool Modules::isOutOfLoaderBehindMock(ModuleID file) const
{
    return m_dataFilesThatMockTookOutOfLoader.get(file.index()) && !hasMockLaidOver(file) && !mockInPlace(file) && !hasMockThatWaitsOrRuns(file);
}

ModuleID Modules::formThatRequireFinds(ModuleID target)
{
    auto wasRequired = [&](ModuleID form) {
        const LoadedModule* loaded = loadedIfExists(form);
        return loaded && (loaded->requiredValue || loaded->wasRequiredByHost);
    };
    if (wasRequired(target) || isEvicted(target))
        return target;
    Identifier key = keyWithoutForm(target);
    Vector<ModuleID, 2> forms = m_table.formsThatAttributesAskFor(key.string());
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
    if (file && !hasNotBegun(*file) && !isOutOfLoaderBehindMock(*file))
        return *file;
    bool loaderHoldsEntryWithoutType = loader()->moduleMap().contains(ModuleMapKey { key.impl(), ScriptFetchParameters::Type::JavaScript });
    if (!file && loaderHoldsEntryWithoutType && status(target) == ModuleStatus::Evaluated && !isOutOfLoaderBehindMock(target))
        return target;
    auto first = m_formsAnsweredFirst.find(ModuleMapKey { key.impl(), ScriptFetchParameters::Type::HostDefined });
    if (first != m_formsAnsweredFirst.end() && forms.contains(first->value.module) && status(first->value.module) == ModuleStatus::Evaluated && !isOutOfLoaderBehindMock(first->value.module))
        return first->value.module;
    for (ModuleID form : forms) {
        if (m_table.pathText(form).endsWith("?json"_s) && status(form) == ModuleStatus::Evaluated && !isOutOfLoaderBehindMock(form))
            return form;
    }
    return target;
}

JSValue Modules::require(ModuleID target, ModuleID referrer, RequiredBy requiredBy)
{
    SetForScope noImportAsks(m_moduleThatWroteImportThatAsks, ModuleID());
    ModuleID recordedAhead = m_targetOfRequireRecordedAhead == target ? m_requirerRecordedAhead : ModuleID();
    m_targetOfRequireRecordedAhead = ModuleID();
    m_requirerRecordedAhead = ModuleID();
    SetForScope requirerOfRequireUnderWay(m_requirerOfRequireUnderWay, referrer ? referrer : recordedAhead);
    auto notesAnswer = makeScopeExit([&] {
        m_targetOfRequireJustAnswered = recordedAhead && !m_instance.vm->exceptionForInspection() ? target : ModuleID();
    });
    if (m_mocks) [[unlikely]]
        noteAskForKey(target, referrer, LoadByName::Require);
    if (auto* willLoad = m_mocks || m_hasBegunTestFile ? host().willLoad : nullptr) [[unlikely]]
        willLoad(m_instance.globalObject, referrer ? fileOf(referrer) : ModuleID(), fileOf(target), LoadByName::Require);
    if (referrer && m_goesModuleByModule) [[unlikely]]
        noteRequireThatRunTimeAnswers(target, referrer);
    if (!referrer || !m_showsWaysBetweenModuleObjects) [[likely]]
        return answerRequire(target, referrer, requiredBy, FormThatAnswers::TheOneThatLoaderFinds);
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    recordRequire<RequireAnsweredBy::RunTime>(target, referrer);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue required = answerRequire(target, referrer, requiredBy, FormThatAnswers::TheOneThatLoaderFinds);
    RETURN_IF_EXCEPTION(scope, { });
    recordRequire<RequireAnsweredBy::RunTime>(target, referrer);
    RETURN_IF_EXCEPTION(scope, { });
    return required;
}

bool Modules::isHeldForImport(ModuleID target) const
{
    if (namespaceIfCreated(target, ModulePhase::Evaluation))
        return true;
    if (m_dataFilesImportedByUncompiledCode.get(target.index()))
        return true;
    uint32_t momentMockInItsPlaceEnded = m_momentsMocksInPlaceOfLibraryModulesEnded.get(target);
    return std::ranges::any_of(m_table.importsOfHeldFile(target), [&](const ImportOfHeldFileData& import) {
        ModuleID importer(import.importer);
        if (mockInPlace(importer) && !hasRunItself(importer))
            return false;
        return wasImporterLoaded(importer) && momentLinked(importer) >= momentMockInItsPlaceEnded;
    });
}

void Modules::runDataFileAgainForImport(ModuleID file, ImportAnsweredBy answeredBy)
{
    LoadedModule* loaded = loadedIfExists(file);
    if (data(file).kind != ModuleKind::Data || !loaded || !loaded->requiredValue || recordIfExists(file) || isMocked(file))
        return;
    if (answeredBy == ImportAnsweredBy::Build && !loaded->wasRequiredByHost && !data(file).isDataFoundByName() && !isTestRun())
        return;
    if (status(file) != ModuleStatus::Evaluated || isHeldForImport(file))
        return;
    setStatus(file, ModuleStatus::Linked);
    evaluateSynchronously(file, SynchronousEvaluationReason::Require);
}

bool Modules::wasImporterLoaded(ModuleID importer) const
{
    if (status(importer) > ModuleStatus::Linked || (m_entryPointIsLinked && data(importer).isLinkedWithEntryPoint()))
        return true;
    if (ModuleRecord* record = recordIfExists(importer); record && record->status() != CyclicModuleRecord::Status::New)
        return true;
    return data(importer).isESModuleOfProgram() && (wasLoadedWithGraph(importer) || isBelowRootWhoseFilesRun(importer));
}

bool Modules::isBelowRootWhoseFilesRun(ModuleID wanted) const
{
    Vector<ModuleID, 4> running;
    forEachFileEvaluatedByLoader([&](ModuleID file) {
        running.append(file);
    });
    for (const FilesBeingRun* files = m_innermostFilesBeingRun; files; files = files->outer) {
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
    if (String text = m_table.pathText(file); text.contains('?')) [[unlikely]]
        path = jsString(*m_instance.vm, text.left(text.reverseFind('?')));
    moduleObject(file);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = requireSpecifier(path, referrer ? referrer : file, NamedByBuild::FileOfImage);
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
        captureExportsAgain(file);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return value;
}

JSValue Modules::exportsOnObjectMadeByRequire(const LoadedModule& loaded)
{
    JSObject* entry = loaded.objectMadeByRequire;
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
    return m_libraryModulesWithMock.get(module.index()) && host().replacesNativeModule && host().replacesNativeModule(m_instance.globalObject, m_table.pathText(module));
}

JSValue Modules::answerRequireWithMock(ModuleID target, ModuleID referrer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (referrer && m_libraryModulesThatMockReplaces.get(target.index()))
        RELEASE_AND_RETURN(scope, requireSpecifier(pathString(target), referrer, NamedByBuild::WhatLiesOutsideImage));
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
    JSValue esModule = exports->getIfPropertyExists(realm, vm.propertyNames->__esModule);
    RETURN_IF_EXCEPTION(scope, { });
    if (!esModule || !esModule.toBoolean(realm))
        return exports;
    JSValue defaultExport = exports->getIfPropertyExists(realm, vm.propertyNames->defaultKeyword);
    RETURN_IF_EXCEPTION(scope, { });
    return defaultExport && !defaultExport.isUndefined() ? defaultExport : JSValue(exports);
}

JSValue Modules::answerRequire(ModuleID target, ModuleID referrer, RequiredBy requiredBy, FormThatAnswers formThatAnswers)
{
    if (!m_callsOfImportUnderMocks.isEmpty()) [[unlikely]]
        forgetNamespacesKeptForCallsOfImport();
    if (m_filesThatStayLoadedAboveMock.get(target.index())) [[unlikely]]
        tellOfFilesThatStayedLoadedBelow(target);
    if (JSValue settled = settledAnswerOfRequire(target, requiredBy))
        return settled;
    if (formThatAnswers == FormThatAnswers::TheOneThatLoaderFinds && data(target).kind == ModuleKind::Data) [[unlikely]]
        target = formThatRequireFinds(target);
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    endMocksThatStoodForLoadsInFlight();
    RETURN_IF_EXCEPTION(scope, { });
    if (!m_importsWhoseMocksRunWhenTheirTextsHaveCome.isEmpty()) [[unlikely]] {
        walkInPlaceOfCallsHeldForTextOf(fileOf(target));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (standsForLoadsInFlight(target) && m_modulesWithMockToRun.get(target.index()) && !isEvaluatedByLoader(target) && haveLoadsInFlightTakenAnswerThatStandsOn(target)) [[unlikely]] {
        endMockThatStandsForLoadsInFlight(target);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (JSValue error = errorOfLoadEndedByMock(target); error && !hasBegunToLoad(target)) [[unlikely]] {
        throwException(m_instance.globalObject, scope, error);
        return { };
    }
    const ModuleData& targetData = data(target);
    if (m_mocks) [[unlikely]]
        noteLoadBegunByFactories(target);
    bool someFactoryHasNotSettled = tellHostOfRequire(target);
    if (isAskedForByOwnFactory(target)) [[unlikely]]
        RELEASE_AND_RETURN(scope, requireModuleItselfBesideMock(target));
    if (m_mocks && referrer && isMocked(target) && targetData.isESModuleOfProgram() && !targetData.isFaceOfFile() && isOfPackage(referrer) && host().isLeftToItselfByMock && host().isLeftToItselfByMock(m_instance.globalObject, m_table.pathText(target), m_table.pathText(referrer))) [[unlikely]]
        RELEASE_AND_RETURN(scope, requireModuleItselfBesideMock(target));
    if (m_mocks && isMocked(target) && targetData.isESModuleOfProgram() && !targetData.isFaceOfFile() && m_modulesWithMockToRun.get(target.index()) && !isLeftAloneByMock(target) && hasRunItself(target) && host().givesOriginalBesideMock && host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(target), String(), false)) [[unlikely]]
        RELEASE_AND_RETURN(scope, requireModuleItselfBesideMock(target));
    if (m_mocks && targetData.kind == ModuleKind::CommonJs && isMocked(target) && !targetData.isOfLibrary() && status(target) == ModuleStatus::Linked && !mockInPlace(target) && (m_modulesWhoseFactoryRuns.get(target.index()) || (!m_modulesWithMockToRun.get(target.index()) && host().isCalledFromFactoryThatWaits && host().isCalledFromFactoryThatWaits(m_instance.globalObject, target)))) [[unlikely]] {
        m_filesWhoseOriginalRuns.set(target.index());
        evaluateCommonJS(target);
        m_filesWhoseOriginalRuns.clear(target.index());
        if (scope.exception()) [[unlikely]] {
            forgetRunThatThrew(target);
            return { };
        }
        return commonJSExports(target);
    }
    if (m_mocks && needsOriginalAheadOfFactory(target) && host().givesOriginalBesideMock && host().givesOriginalBesideMock(m_instance.globalObject, m_table.pathText(target), String(), false)) [[unlikely]] {
        JSObject* original = originalThatLoaderHolds(target);
        RETURN_IF_EXCEPTION(scope, { });
        if (original)
            return original;
    }
    bool isRequiredByProgram = requiredBy == RequiredBy::Program || (referrer && !data(referrer).isOfLibrary());
    if (m_mocks && (isMocked(target) || m_modulesWithMockToRun.get(target.index())) && hasFailedWithoutMock(target)) [[unlikely]]
        forgetLoadThatFailed(target);
    if (!m_requiresThatGaveUp.isEmpty() && (!targetData.isOfLibrary() || isRequiredByProgram) && targetData.kind == ModuleKind::SourceText && !isMocked(target) && !vm.m_synchronousModuleQueue && !m_filesWhoseRequireThrewBesideFactories.get(target.index()) && !m_importOfFactorysOwnAsks && stillWaitsForWhatItGaveUpOn(target)) [[unlikely]] {
        refuseRequireAgain(target, target);
        RETURN_IF_EXCEPTION(scope, { });
        throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.pathText(target)));
        return { };
    }
    bool wasHeldBeforeWalk = holdsLinksOfLoadThatMayFail(target);
    if (!targetData.isOfLibrary() || isRequiredByProgram) {
        SetForScope requireRunsMocks(vm.aotState().requireRunsMocks, true);
        SetForScope moduleThatRequireAsksFor(m_moduleThatRequireAsksFor, target);
        SetForScope moduleThatRequires(m_moduleThatRequires, referrer);
        std::optional<VM::SynchronousModuleQueueScope> loadsOfFactories;
        if (m_mocks && targetData.kind == ModuleKind::SourceText && !isMocked(target) && !vm.m_synchronousModuleQueue) [[unlikely]]
            loadsOfFactories.emplace(vm);
        SetForScope walkGoesOnPastFactoriesThatHaveNotSettled(m_walkGoesOnPastFactoriesThatHaveNotSettled, !!loadsOfFactories);
        bool factoryHasNotSettled = runMocksBelow(target);
        if (loadsOfFactories) [[unlikely]] {
            if (!scope.exception())
                JSModuleLoader::drainSynchronousModuleQueue(m_instance.globalObject);
            leaveStepsOfLoaderToJobs();
            if (Exception* thrown = scope.exception(); thrown && !vm.isTerminationException(thrown) && scope.tryClearException()) {
                loadsOfFactories.reset();
                refuseRequireAgain(target, target);
                if (!scope.exception() || scope.tryClearException()) {
                    noteThatRequireGaveUpOn(target);
                    m_filesWhoseRequireThrewBesideFactories.set(target.index());
                    throwException(m_instance.globalObject, scope, thrown);
                }
                return { };
            }
        }
        RETURN_IF_EXCEPTION(scope, { });
        if (factoryHasNotSettled) [[unlikely]] {
            noteThatRequireGaveUpOn(target);
            if (loadsOfFactories) {
                loadsOfFactories.reset();
                refuseRequireAgain(target, target);
                RETURN_IF_EXCEPTION(scope, { });
            }
            throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.pathText(target)));
            return { };
        }
    } else
        evaluateModulesOwedBelow(target);
    RETURN_IF_EXCEPTION(scope, { });
    auto hearsHowRequireEnds = makeScopeExit([&] {
        if (wasHeldBeforeWalk || !holdsLinksOfLoadThatMayFail(target)) [[likely]]
            return;
        if (scope.exception())
            bringBackLinksOfLoadThatFailed(target);
        else
            letLinksOfLoadStand(target);
    });
    if (someFactoryHasNotSettled) [[unlikely]]
        noteThatRequireGaveUpOn(target, false);
    if (!m_requiresThatGaveUp.isEmpty() && (!targetData.isOfLibrary() || isRequiredByProgram)) [[unlikely]] {
        JSValue original = answerRequireWhileFactoryHasNotSettled(target, referrer);
        RETURN_IF_EXCEPTION(scope, { });
        if (original)
            return original;
    }
    if (m_numberOfMocksInPlace) [[unlikely]] {
        if (getsOriginalBesideMock(target, referrer))
            RELEASE_AND_RETURN(scope, answerRequireWithOriginal(target, referrer));
        if (m_filesWhoseOriginalRuns.get(target.index()) && mockInPlace(target))
            return ensureLoaded(target).answerOfMockWhileOriginalRuns.get();
        if (mockInPlace(target)) {
            noteAskUnderMockInPlace(target);
            noteRequireOfMockLaidInPreload(target);
        }
    }
    if (isLeftToHost(target)) [[unlikely]]
        RELEASE_AND_RETURN(scope, loadEvictedFile(fileOf(target), referrer));
    if (targetData.isOfLibrary() && isRequiredByProgram && isKeptInRequireCache(target)) [[unlikely]]
        RELEASE_AND_RETURN(scope, requireThroughCache(target, referrer));
    if (targetData.isOfLibrary() && isRequiredByProgram && isMockedLibraryModule(target)) [[unlikely]]
        RELEASE_AND_RETURN(scope, answerRequireWithMock(target, referrer));

    if (targetData.isUncompiledCode()) {
        m_uncompiledNamespacesNotShownYet.clear(target.index());
        RELEASE_AND_RETURN(scope, requireSpecifier(pathString(target), *targetData.referrerModule(), NamedByBuild::WhatLiesOutsideImage));
    }
    if (standsForFileOutsideImage(target)) {
        JSObject* requirer = moduleObject(*targetData.referrerModule());
        RETURN_IF_EXCEPTION(scope, { });
        RequestToHostScope requestToHost(*this);
        RELEASE_AND_RETURN(scope, host().requireSpecifier(m_instance.globalObject, requirer, pathString(target)));
    }
    if (!targetData.hasExportTable()) {
        if (LoadedModule* loaded = loadedIfExists(target); loaded && loaded->isAnsweredWithNamespace && !isMocked(target) && !mockInPlace(target)) [[unlikely]] {
            JSValue held = exportsOnObjectMadeByRequire(*loaded);
            RETURN_IF_EXCEPTION(scope, { });
            if (held && held != loaded->exportsGivenToObjectMadeByRequire.get())
                return held;
            RELEASE_AND_RETURN(scope, namespaceThatAnswersRequire(target));
        }
        JSValue executed = execute(target);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_ASSERT(executed);
        return executed;
    }

    if (JSValue thrownByBody = isLeftAloneByMock(target) && !mockInPlace(target) ? errorOfModuleThatRanBesideMock(target) : JSValue()) [[unlikely]] {
        tellThatFailureStayedBehindMock(target);
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
    bool isAnsweredAsByLoader = requiredBy == RequiredBy::Program && targetData.answersLoaderWithNamespace();
    bool answersDefaultExport = !isAnsweredAsByLoader && !isMocked(target) && (targetData.requireResult == RequireResult::DefaultExport || (targetData.requireResult == RequireResult::NamespaceIfImported && !loaded.requiredValue && !isHeldForImport(target)));
    if (loaded.isKeptByRequire && loaded.requiredValue && !isAnsweredAsByLoader && status(target) == ModuleStatus::Failed) [[unlikely]]
        return loaded.requiredValue.get();
    bool wasBeingRequired = false;
    if (status(target) != ModuleStatus::Evaluated) {
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
        if (wasBeingRequired && loaded.isKeptByRequire && loaded.objectMadeByRequire) [[unlikely]] {
            loaded.objectMadeByRequire->putDirect(vm, vm.propertyNames->exports, answered);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, answered);
        }
        return answered;
    };
    if (!wasBeingRequired && loaded.requiredValue && loaded.objectMadeByRequire) [[unlikely]] {
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
    if (!m_table.importsOfHeldFile(target).empty()) {
        runDataFileAgainForImport(target, ImportAnsweredBy::Build);
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
    if (moduleStatus == ModuleStatus::Failed)
        RELEASE_AND_RETURN(scope, throwEvaluationError(module));
    RELEASE_ASSERT(moduleStatus == ModuleStatus::Unlinked || moduleStatus == ModuleStatus::Linked);
    unbindModuleObjectOfRunThatThrew(module);
    RETURN_IF_EXCEPTION(scope, void());
    if (moduleStatus == ModuleStatus::Unlinked) {
        link(module);
        RETURN_IF_EXCEPTION(scope, void());
    }

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
    bool isVariableLaidOnObject = loaded && loaded->moduleObjectOverExportsVariable.get() == loaded->requirer.get();
    if (!loaded || !loaded->requirer || (data(module).hasValue() && !isVariableLaidOnObject) || data(module).exportsStayInCompiledCode()) [[likely]]
        return exportsGivenBackToCompiledCode(module);
    return loaded->requirer->get(m_instance.globalObject, m_instance.vm->propertyNames->exports);
}

static std::optional<ModuleID> firstFileThatRunsWithGraphOf(const Modules& modules, ModuleID root)
{
    const ModuleTable& table = modules.table();
    if (modules.isCommonJSFileOrFace(root))
        return table.fileBehindFace(root).value_or(root);
    for (const ModuleRequestData& request : table.requests(root)) {
        if (request.isRequire)
            continue;
        ModuleID imported = request.targetModule();
        if (!request.isDeferred && modules.isCommonJSFileOrFace(imported))
            return table.fileBehindFace(imported).value_or(imported);
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
    m_modulesFirstRequiredByCallsInProgress.clear();
    m_firstFilesOfLoads.clear();
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
        if (!wasRequestedByLoader(file))
            continue;
        bool someFileHasModuleObject = std::ranges::any_of(m_modulesFirstRequiredByCallsInProgress, [&](ModuleID required) {
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

void Modules::didImportFileFromUncompiledCode(ModuleID fileOrFace)
{
    noteRequestByLoader(fileOrFace);
    noteLoadOfGraph(fileOrFace);
}

std::optional<OptionSet<ModuleObjectTrait>> Modules::traitsOfModuleObjectYetToBeMade(ModuleID module)
{
    if (data(module).kind != ModuleKind::CommonJs || ensureLoaded(module).requirer)
        return std::nullopt;
    stopWhereExportsLeaveCompiledCode(module);
    OptionSet<ModuleObjectTrait> traits;
    if (data(module).hasValue())
        traits.add(ModuleObjectTrait::HasExportsVariable);
    if (mainCommonJSModule() == module)
        traits.add(ModuleObjectTrait::IsMain);
    return traits;
}

void Modules::takeModuleObject(ModuleID module, const ModuleObjects& objects)
{
    LoadedModule& loaded = ensureLoaded(module);
    RELEASE_ASSERT(!loaded.requirer && objects.module);
    notePrototypeOfModuleObjects(objects.module);
    loaded.requirer.set(*this, objects.module);
    loaded.requireFunction.set(*this, objects.require);
}

void Modules::giveBackModuleObject(ModuleID module)
{
    if (status(module) > ModuleStatus::Linked || isEvicted(module))
        return;
    LoadedModule& loaded = ensureLoaded(module);
    loaded.requirer.set(*this, nullptr);
    loaded.moduleObjectOverExportsVariable.set(*this, nullptr);
    loaded.exportsWhenVariableAndModuleObjectAgreed.set(*this, JSValue());
    loaded.structureOfModuleObjectLastLookedAt.set(*this, nullptr);
    loaded.idOfStructureOfModuleObjectLastLookedAt = StructureID();
    loaded.isExportsVariableTakenOffModuleObject = false;
    loaded.isOrderOfStoreAndDeedUnknown = false;
    loaded.requireFunction.set(*this, JSValue());
}

JSObject* Modules::moduleObjectOverExportsVariableIfMade(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && data(module).kind == ModuleKind::CommonJs && data(module).hasValue() ? loaded->requirer.get() : nullptr;
}

void Modules::didLayExportsVariableOnModuleObject(ModuleID module)
{
    LoadedModule& loaded = ensureLoaded(module);
    loaded.moduleObjectOverExportsVariable.set(*this, loaded.requirer.get());
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
    if (loaded.moduleObjectOfRunThatThrew) [[unlikely]] {
        if (status(module) <= ModuleStatus::Linked)
            return loaded.moduleObjectOfRunThatThrew;
        unbindModuleObjectOfRunThatThrew(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }

    ModuleObjects objects;
    if (data(module).kind == ModuleKind::CommonJs) {
        host().createModuleObject(realm, pathString(module), module, *traitsOfModuleObjectYetToBeMade(module), objects);
    } else {
        JSObject* meta = importMeta(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        host().createRequirer(realm, meta, module, objects);
    }
    RETURN_IF_EXCEPTION(scope, nullptr);
    notePrototypeOfModuleObjects(objects.module);
    loaded.requirer.set(*this, objects.module);
    if (data(module).kind == ModuleKind::CommonJs) {
        loaded.moduleObjectOverExportsVariable.set(*this, objects.module);
        loaded.exportsWhenVariableAndModuleObjectAgreed.set(*this, exportsGivenBackToCompiledCode(module));
    }
    loaded.requireFunction.set(*this, objects.require);
    return objects.module;
}

JSObject* Modules::moduleObjectMadeByRequire(ModuleID module)
{
    if (data(module).kind == ModuleKind::CommonJs && !ensureLoaded(module).isAnsweredWithNamespace)
        return moduleObject(module);
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSObject* made = ensureLoaded(module).objectMadeByRequire)
        return made;

    JSValue answered = answerRequire(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (JSObject* made = ensureLoaded(module).objectMadeByRequire)
        return made;
    OptionSet<ModuleObjectTrait> traits { ModuleObjectTrait::IsNoCommonJSFile };
    if (data(module).kind == ModuleKind::SourceText)
        traits.add(ModuleObjectTrait::IsESModule);
    ModuleObjects objects;
    host().createModuleObject(m_instance.globalObject, pathString(module), module, traits, objects);
    RETURN_IF_EXCEPTION(scope, nullptr);
    notePrototypeOfModuleObjects(objects.module);
    objects.module->putDirect(vm, vm.propertyNames->exports, answered);
    ensureLoaded(module).objectMadeByRequire.set(*this, objects.module);
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

JSObject* Modules::objectThatStandsForModule(ModuleID module)
{
    if (!keepsModuleObjectToItself(module))
        return moduleObjectMadeByRequire(module);
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.standInForModuleObject)
        loaded.standInForModuleObject.set(*this, StandInForModuleObject::create(*m_instance.vm, m_instance.globalObject, moduleObjectRefusedMessage(m_table.pathText(module))));
    return loaded.standInForModuleObject;
}

JSObject* Modules::objectThatStandsForModuleIfMade(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded)
        return nullptr;
    if (keepsModuleObjectToItself(module))
        return loaded->standInForModuleObject;
    return data(module).kind == ModuleKind::CommonJs ? loaded->requirer.get() : loaded->objectMadeByRequire.get();
}

JSObject* Modules::mainModuleObject(ModuleID main)
{
    ModuleID file = fileOf(main);
    if (!data(file).isEntryPoint() && !m_storesPathOfMainModule)
        return nullptr;
    if (data(file).kind == ModuleKind::CommonJs)
        return mayHandOutModuleObject(file) && wasRequestedByLoader(file) ? moduleObject(file) : nullptr;
    bool hasObjectMadeByRequire = m_storesPathOfMainModule && !data(file).isOfLibrary() && m_modulesWithMarkOfRequire.get(file.index());
    return hasObjectMadeByRequire ? moduleObjectMadeByRequire(file) : nullptr;
}

bool Modules::findWhetherExportsVariableIsTakenOffModuleObject(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    bool isCommonJSFile = data(module).kind == ModuleKind::CommonJs;
    JSObject* moduleObject = !loaded ? nullptr : isCommonJSFile ? loaded->requirer.get() : loaded->objectMadeByRequire.get();
    if (!moduleObject)
        return false;
    VM& vm = *m_instance.vm;
    if (isCommonJSFile && loaded->moduleObjectOverExportsVariable.get() == moduleObject && !loaded->isModuleObjectHeldByCompiledCode && !isMocked(module) && data(module).hasValue() && !data(module).exportsStayInCompiledCode()) {
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

void Modules::noteModuleObjectHeldByCompiledCode(ModuleID module)
{
    ensureLoaded(module).isModuleObjectHeldByCompiledCode = true;
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

NEVER_INLINE void Modules::lookAtExportsOfModuleObject(ModuleID module, RequireAnsweredBy answeredBy)
{
    if (findWhetherExportsVariableIsTakenOffModuleObject(module) && answeredBy == RequireAnsweredBy::CompiledCode)
        readExportsOffModuleObjectIntoVariable(module);
}

template<Modules::RequireAnsweredBy answeredBy>
void Modules::recordRequire(ModuleID target, ModuleID referrer)
{
    if constexpr (answeredBy == RequireAnsweredBy::CompiledCode) {
        if (m_mocks) [[unlikely]] {
            noteAskForKey(target, referrer, LoadByName::Require);
            bool comesBehindRequire = m_targetOfRequireRecordedAhead == target || m_targetOfRequireJustAnswered == target;
            m_targetOfRequireJustAnswered = ModuleID();
            m_targetOfRequireRecordedAhead = comesBehindRequire ? ModuleID() : target;
            m_requirerRecordedAhead = comesBehindRequire ? ModuleID() : referrer;
        }
        if (auto* willLoad = m_mocks || m_hasBegunTestFile ? host().willLoad : nullptr) [[unlikely]]
            willLoad(m_instance.globalObject, referrer ? fileOf(referrer) : ModuleID(), fileOf(target), LoadByName::Require);
    }
    const ModuleData& targetData = data(target);
    bool isCommonJSFile = targetData.kind == ModuleKind::CommonJs;
    if (!isCommonJSFile)
        markNamespaceAsRequired(target);
    if (!m_showsWaysBetweenModuleObjects || !(isFileOfProgram(target) || targetData.kind == ModuleKind::Data))
        return;
    if (isCommonJSFile)
        noteRequireOfFile(target);
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (isCommonJSFile && status(target) <= ModuleStatus::Linked) {
        LoadedModule* required = loadedIfExists(target);
        JSObject* objectOfRunThatThrew = required ? required->moduleObjectOfRunThatThrew.get() : nullptr;
        if (objectOfRunThatThrew) [[unlikely]] {
            unbindModuleObjectOfRunThatThrew(target);
            RETURN_IF_EXCEPTION(scope, void());
            if (referrer == target)
                required->firstRequirerOutsideImage.set(*this, objectOfRunThatThrew);
        }
    }
    uint64_t pair = (static_cast<uint64_t>(referrer.index()) + 1) << 32 | target.index();
    auto recorded = m_recordedRequires.add(pair, isEvicted(target) ? RecordedRequire::BegunAndLeftToHost : RecordedRequire::Begun);
    if (recorded.isNewEntry) {
        findWhetherExportsVariableIsTakenOffModuleObject(referrer);
        lookForMainCommonJSModule();
        LoadedModule* required = loadedIfExists(target);
        if (firstRequirerOf(target) || (required && required->firstRequirerOutsideImage) || (isCommonJSFile && wasRequestedByLoader(target)))
            return;
        ensureWaysTo(target).firstRequirer = referrer;
        bool isListed = required && std::exchange(required->hasFirstRequireOfRunThatThrew, false) && m_firstRequires.containsIf([&](const FirstRequire& first) {
            return first.requirer == referrer && first.required == target;
        });
        if (!isListed)
            m_firstRequires.append({ referrer, target });
        if (targetData.kind == ModuleKind::SourceText)
            recorded.iterator->value = RecordedRequire::BegunAndMadeObjectOfESModule;
        if (!m_isMainCommonJSModuleDecided)
            m_modulesFirstRequiredByCallsInProgress.append(target);
        return;
    }
    switch (recorded.iterator->value) {
    case RecordedRequire::BegunAndMadeObjectOfESModule:
        if (status(target) == ModuleStatus::Failed)
            return;
        recorded.iterator->value = RecordedRequire::ReturnedAndNotListed;
        if (!m_isMainCommonJSModuleDecided)
            decideMainCommonJSModule(std::nullopt);
        return;
    case RecordedRequire::ReturnedAndNotListed:
        recorded.iterator->value = isEvicted(target) ? RecordedRequire::BegunAndLeftToHost : RecordedRequire::Begun;
        return;
    case RecordedRequire::Begun:
    case RecordedRequire::BegunAndLeftToHost: {
        if (status(target) == ModuleStatus::Failed)
            return;
        bool isListedByHost = recorded.iterator->value == RecordedRequire::BegunAndLeftToHost;
        recorded.iterator->value = RecordedRequire::Returned;
        if (!m_isMainCommonJSModuleDecided)
            decideMainCommonJSModule(std::nullopt);
        if (!isListedByHost)
            ensureWaysTo(referrer).requiredModules.append(target);
        findWhetherExportsVariableIsTakenOffModuleObject(referrer);
        lookAtExportsOfModuleObject(target, answeredBy);
        RETURN_IF_EXCEPTION(scope, void());
        LoadedModule* requirer = loadedIfExists(referrer);
        if (requirer && requirer->requirer && host().showChildren)
            RELEASE_AND_RETURN(scope, host().showChildren(m_instance.globalObject, requirer->requirer, nullptr));
        return;
    }
    case RecordedRequire::Returned: {
        LoadedModule* requirer = loadedIfExists(referrer);
        JSObject* objectOfRequired = objectThatStandsForModuleIfMade(target);
        if (objectOfRequired && !keepsModuleObjectToItself(target) && objectOfRequired->structureID() != loadedIfExists(target)->idOfStructureOfModuleObjectLastLookedAt) [[unlikely]] {
            lookAtExportsOfModuleObject(target, answeredBy);
            RETURN_IF_EXCEPTION(scope, void());
            requirer = loadedIfExists(referrer);
            objectOfRequired = objectThatStandsForModuleIfMade(target);
        }
        if (requirer && requirer->requirer && objectOfRequired && !isEvicted(target) && host().showChildren) {
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

template void Modules::recordRequire<Modules::RequireAnsweredBy::CompiledCode>(ModuleID, ModuleID);
template void Modules::recordRequire<Modules::RequireAnsweredBy::RunTime>(ModuleID, ModuleID);

void Modules::recordRequireFromOutsideImage(ModuleID target, JSObject* moduleObjectOfRequirer)
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
    if (firstRequirerOf(target) || required.firstRequirerOutsideImage || (isCommonJSFile && wasRequestedByLoader(target)))
        return;
    required.firstRequirerOutsideImage.set(*this, moduleObjectOfRequirer);
    if (!m_isMainCommonJSModuleDecided)
        m_modulesFirstRequiredByCallsInProgress.append(target);
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
                wasRequested = hasLoaderAskedFor(*load, file);
        }
    }
    if (wasRequested)
        m_filesRequestedByLoader.set(file.index());
    return wasRequested;
}

bool Modules::hasLoaderAskedFor(FilesBeingRun& load, ModuleID file)
{
    std::optional<ModuleID> running = fileOrFaceEvaluatedIn(load);
    if (!running)
        return false;
    if (load.requested.isEmpty()) {
        load.requested.ensureSize(m_table.size());
        load.requested.quickSet(fileOf(load.root).index());
        load.importersOfNextLevel.append(load.root);
    }
    while (!load.requested.quickGet(fileOf(*running).index()) && !load.importersOfNextLevel.isEmpty()) {
        for (ModuleID importer : std::exchange(load.importersOfNextLevel, { })) {
            for (const ModuleRequestData& request : m_table.requests(importer)) {
                if (request.isHoisted || request.isRequire)
                    continue;
                ModuleID target = fileOf(request.targetModule());
                if (!load.requested.quickSet(target.index()) && isLoadedWithRequests(target) && !(m_mocks && m_mocks->modules.quickGet(target.index())))
                    load.importersOfNextLevel.append(target);
            }
        }
    }
    return load.requested.quickGet(file.index());
}

void Modules::forgetRequestByLoader(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (data(file).kind == ModuleKind::CommonJs && status(file) <= ModuleStatus::Linked && !isEvicted(file))
        letGoOfModuleObject(file);
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

void Modules::letGoOfModuleObject(ModuleID file)
{
    m_filesRequestedByLoader.clear(file.index());
    for (FilesBeingRun* load = m_innermostFilesBeingRun; load; load = load->outer) {
        if (hasLoaderAskedFor(*load, file))
            load->requested.quickClear(file.index());
    }
    LoadedModule* loaded = loadedIfExists(file);
    if (loaded)
        loaded->standInForModuleObject.set(*this, nullptr);
    if (!loaded || !loaded->requirer)
        return;
    layValueInPlaceOfExportsVariable(file);
    loaded->requirer.set(*this, nullptr);
    loaded->moduleObjectOverExportsVariable.set(*this, nullptr);
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
    return m_instance.vm->aotState().filesWhoseTextHostKeeps.get(file.index()) && host().keepsTexts && host().keepsTexts(m_instance.globalObject);
}

bool Modules::isOfKindWhoseTextHostKeeps(ModuleID file) const
{
    if (m_instance.vm->aotState().filesStartedByHost.get(file.index()) || m_table.dependsOnMoreThanSource(file))
        return false;
    ModuleKind kind = data(file).kind;
    return kind == ModuleKind::SourceText || kind == ModuleKind::CommonJs || m_dataFilesThatHostPrintsAsJavaScript.get(file.index());
}

bool Modules::isLeftToHostThatStarts(ModuleID module)
{
    m_instance.vm->aotState().filesStartedByHost.set(fileOf(module).index());
    return isEvicted(module);
}

void Modules::noteTextsThatHostKeeps()
{
    if (std::exchange(m_hasNotedTextsThatHostKeeps, true))
        return;
    VMState& state = m_instance.vm->aotState();
    for (size_t index = 0; index < m_storedStatuses.size(); ++index) {
        ModuleID file = fileOf(ModuleID(static_cast<uint32_t>(index)));
        if (m_storedStatuses[index] <= static_cast<uint8_t>(StoredModuleStatus::Linked) || m_filesWhoseTextHostDropped.get(file.index()))
            continue;
        state.filesWhoseTextHostKeeps.set(file.index());
        bool isUnderMock = isMocked(file) || m_modulesWithMockInPlace.get(file.index()) || m_modulesWithMockToRun.get(file.index()) || m_modulesThatRanBesideMock.get(file.index());
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
    m_instance.vm->aotState().filesWhoseTextHostHoldsItself.clear(file.index());
    m_filesWhoseTextHostDropped.set(file.index());
    if (m_instance.vm->aotState().filesWithKnownFirstLoad.clear(file.index()))
        m_filesWhoseNextLoadIsFirst.set(file.index());
}

void Modules::willLoadCopyFromDisk(ModuleID module, LoadByName way)
{
    ModuleID file = fileOf(module);
    m_filesWhoseTextHostDropped.clear(file.index());
    if (!m_filesWhoseNextLoadIsFirst.clear(file.index()))
        return;
    VMState& state = m_instance.vm->aotState();
    state.filesWithKnownFirstLoad.set(file.index());
    state.filesFirstLoadedSynchronously.set(file.index(), way == LoadByName::Require || m_instance.vm->synchronousModuleQueue() || hostLoadsSynchronously(file));
}

JSObject* Modules::parentModuleObject(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (loaded && loaded->firstRequirerOutsideImage && mainCommonJSModule() != module)
        return loaded->firstRequirerOutsideImage;
    std::optional<ModuleID> firstRequirer = firstRequirerOf(module);
    if (!firstRequirer || mainCommonJSModule() == module)
        return nullptr;
    return keepsModuleObjectToItself(*firstRequirer) ? objectThatStandsForModule(*firstRequirer) : moduleObject(*firstRequirer);
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
        JSObject* childObject = objectThatStandsForModule(child);
        RETURN_IF_EXCEPTION(scope, true);
        if (ways->numberOfChildrenShown != place || place >= ways->requiredModules.size() || ways->requiredModules[place] != child)
            continue;
        ways->numberOfChildrenShown = place + 1;
        functor(childObject);
        RETURN_IF_EXCEPTION(scope, true);
    }
    return true;
}

void Modules::dropNewChildren(ModuleID module)
{
    if (WaysToModule* ways = waysToIfRecorded(module))
        ways->numberOfChildrenShown = ways->requiredModules.size();
}

JSValue Modules::requireSpecifier(JSValue specifier, ModuleID referrer, NamedByBuild namedByBuild)
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
    SetForScope isAskingHost(m_isAskingHost, namedByBuild == NamedByBuild::FileOfImage ? RequestToHost(true) : isNamedByBuild ? RequestToHost::forWhatLiesOutsideImage() : this->isAskingHost() ? m_isAskingHost : RequestToHost());
    SetForScope askerLoadsCompiledFilesAlone(m_instance.vm->aotState().askerLoadsCompiledFilesAlone, !isNamedByBuild && loadsCompiledFilesAlone(referrer));
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
    if (m_modulesWithMarkOfRequire.get(module.index()))
        return;
    if (namespaceIfCreated(module, ModulePhase::Evaluation))
        return;
    if (data(module).isImportedAsNamespace() && !isNotLoaded(module))
        return;
    m_modulesWithExtensibleNamespace.set(module.index());
}

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

JSObject* Modules::namespaceThatAnswersRequire(ModuleID module)
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

    JSValue exported = index ? readExport(compiledNamespace->module(), *index) : namespaceObject->get(m_instance.globalObject, name);
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, { });
        return { };
    }
    return exported;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

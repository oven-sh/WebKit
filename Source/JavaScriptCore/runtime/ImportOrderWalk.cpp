/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "ImportOrderWalk.h"

#if USE(BUN_JSC_ADDITIONS)

#if ENABLE(AOT)
#include "AOTModuleRecord.h"
#endif
#include "CyclicModuleRecord.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "JSPromise.h"
#include "ModuleGraphLoadingState.h"
#include "ModuleRegistryEntry.h"
#include "SyntheticModuleRecord.h"
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

namespace JSC {

static ModuleMapKey keyOfRequest(const AbstractModuleRecord::ModuleRequest& request)
{
    return { request.m_specifier.impl(), request.type() };
}

static ModuleRegistryEntry* entryOfKey(AbstractModuleRecord* importer, UniquedStringImpl* key, ScriptFetchParameters::Type typeOfRequest)
{
    JSModuleLoader* loader = importer->moduleLoader();
    return loader->moduleMap().get(loader->keyInModuleMap(key, typeOfRequest)).get();
}

static bool isYetToBeLinked(CyclicModuleRecord* module)
{
    return module->status() <= CyclicModuleRecord::Status::Unlinked;
}

static ModuleRegistryEntry* entryOf(AbstractModuleRecord* record)
{
    return record->moduleLoader()->entryOf(record);
}

static bool hasFailedForGood(CyclicModuleRecord* module)
{
    if (module->status() != CyclicModuleRecord::Status::New) [[likely]]
        return false;
    ModuleRegistryEntry* entry = entryOf(module);
    return entry && entry->isFailureFinal() && entry->status() >= ModuleRegistryEntry::Status::FetchFailed;
}

// ModuleRegistryEntry::error() copies a fetch error, without its stack or what else the host put on it.
static JSValue errorOfEntryWithoutRecord(JSGlobalObject* globalObject, ModuleRegistryEntry* entry)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue error = entry->fetchError();
    if (!error) {
        error = entry->error(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!error)
        error = entry->ensureModulePromise(globalObject)->result();
    if (auto* instance = dynamicDowncast<ErrorInstance>(error))
        RELEASE_AND_RETURN(scope, JSModuleLoader::maybeDuplicateFetchError(globalObject, instance));
    return error;
}

static bool unloadImportsOf(AbstractModuleRecord* failed, CyclicModuleRecord* importer)
{
    Locker locker { importer->cellLock() };
    return importer->loadedModules().removeIf([&](auto& loaded) {
        if (loaded.value.m_module.get() != failed)
            return false;
        importer->resolvedRequests().set(loaded.key, AbstractModuleRecord::ResolvedRequest { failed->moduleKey().impl(), AbstractModuleRecord::ResolvedRequest::State::Failed });
        return true;
    });
}

static bool isDeferredModuleYetToRun(AbstractModuleRecord* record)
{
    auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(record);
    return synthetic && synthetic->deferredModule() && !synthetic->isRunningDeferredModule();
}

static bool isCompiled([[maybe_unused]] VM& vm, [[maybe_unused]] AbstractModuleRecord* record)
{
#if ENABLE(AOT)
    return AOT::compiledModule(vm, record);
#else
    return false;
#endif
}

ImportOrderWalk::ImportOrderWalk(JSGlobalObject* globalObject, AbstractModuleRecord* root, Mode mode, RefPtr<ScriptFetcher>&& scriptFetcher, VisitedRecords& visited, EvaluationAheadOfLink& evaluation)
    : m_globalObject(globalObject)
    , m_vm(globalObject->vm())
    , m_state(m_vm.importOrderState())
    , m_root(root)
    , m_loaderOfRoot(root->moduleLoader())
    , m_outer(std::exchange(m_state.innermostWalk, this))
    , m_scriptFetcher(WTF::move(scriptFetcher))
    , m_visited(visited)
    , m_evaluation(evaluation)
    , m_mode(mode)
{
}

ImportOrderWalk::~ImportOrderWalk()
{
    m_state.innermostWalk = m_outer;
}

JSPromise* ImportOrderWalk::run(JSGlobalObject* globalObject, AbstractModuleRecord* root, RefPtr<ScriptFetcher>&& scriptFetcher, bool mayHaveImportsInFlight, JSValue errorOfLoad, std::optional<JSModuleLoader::EvaluationOfLoad> evaluationOfLoad)
{
    VisitedRecords visited;
    EvaluationAheadOfLink evaluation(globalObject, scriptFetcher, evaluationOfLoad);
    if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(root)) {
        ImportOrderWalk walk(globalObject, root, Mode::WaitsForImports, nullptr, visited, evaluation);
        walk.reachSynthetic(synthetic);
        walk.didEnd();
        return nullptr;
    }
    auto* module = uncheckedDowncast<CyclicModuleRecord>(root);
    // Everything below a record that has been linked was reached when that link started.
    if (!isYetToBeLinked(module) && !isCompiled(globalObject->vm(), module))
        return nullptr;
    ImportOrderWalk walk(globalObject, root, Mode::WaitsForImports, WTF::move(scriptFetcher), visited, evaluation);
    walk.m_errorOfHost = errorOfLoad;
    JSPromise* importInFlight = walk.run(module, mayHaveImportsInFlight);
    bool goesOn = importInFlight;
#if ENABLE(AOT)
    if (auto* compiled = AOT::compiledModule(walk.m_vm, module))
        goesOn |= compiled->hasGivenWayToCopy();
#endif
    if (goesOn)
        walk.m_loaderOfRoot->didStopImportOrderWalk(walk.m_vm);
    else
        walk.didEnd();
    return importInFlight;
}

auto ImportOrderWalk::runAheadOfLoad(JSGlobalObject* globalObject, CyclicModuleRecord* root) -> JSModuleLoader::RunAheadOfLoad
{
    if (!isYetToBeLinked(root))
        return { };
    VisitedRecords visited;
    EvaluationAheadOfLink evaluation(globalObject, nullptr, std::nullopt, EvaluationAheadOfLink::RunsAheadOfLoad::Yes);
    ImportOrderWalk walk(globalObject, root, Mode::RunsAheadOfLoad, nullptr, visited, evaluation);
    uint64_t clock = walk.m_state.clock;
    constexpr bool mayHaveImportsInFlight = true;
    JSPromise* importInFlight = walk.run(root, mayHaveImportsInFlight);
    walk.didEnd();
    return { clock != walk.m_state.clock || walk.m_didRequestImportInFlight, importInFlight };
}

JSPromise* ImportOrderWalk::runAheadOfFetches(JSGlobalObject* globalObject, CyclicModuleRecord* root, JSModuleLoader::PassedImports& passed)
{
    if (!isYetToBeLinked(root) || isBeingReached(globalObject->vm(), root))
        return nullptr;
    if (passed.areAllThatCanBePassed && passed.lastChange == globalObject->vm().importOrderState().lastChange)
        return nullptr;
    VisitedRecords visited;
    EvaluationAheadOfLink evaluation(globalObject, nullptr, std::nullopt, EvaluationAheadOfLink::RunsAheadOfLoad::Yes);
    ImportOrderWalk walk(globalObject, root, Mode::RunsAheadOfLoad, nullptr, visited, evaluation);
    walk.m_passed = &passed;
    constexpr bool mayHaveImportsInFlight = true;
    JSPromise* importInFlight = walk.run(root, mayHaveImportsInFlight);
    passed.areAllThatCanBePassed = evaluation.didStopAtDeferredModule();
    walk.didEnd();
    return importInFlight;
}

void ImportOrderWalk::runWithoutWaiting(JSGlobalObject* globalObject, AbstractModuleRecord* root, VisitedRecords& visited, AbstractModuleRecord::IsBehindWalk isBehindWalk, EvaluationAheadOfLink* evaluationOfWalkThatCameToRoot)
{
    if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(root)) {
        if (synthetic->isReadyToBeLinked()) [[likely]]
            return;
    } else if (!isYetToBeLinked(uncheckedDowncast<CyclicModuleRecord>(root)) && !isCompiled(globalObject->vm(), root))
        return;
    EvaluationAheadOfLink noEvaluation(globalObject, nullptr, std::nullopt);
    ImportOrderWalk walk(globalObject, root, Mode::KeepsAnswers, nullptr, visited, evaluationOfWalkThatCameToRoot ? *evaluationOfWalkThatCameToRoot : noEvaluation);
    walk.m_isBehindWalk = isBehindWalk == AbstractModuleRecord::IsBehindWalk::Yes;
    walk.reachImport(root);
    walk.didEnd();
}

void ImportOrderWalk::didEnd()
{
    // No job has run since the walk around it came here.
    if (m_outer && m_outer->m_loaderOfRoot == m_loaderOfRoot && (m_mode == Mode::KeepsAnswers || m_vm.synchronousModuleQueue()))
        return m_loaderOfRoot->didStopImportOrderWalk(m_vm);
    m_loaderOfRoot->didEndImportOrderWalk(m_vm);
}

bool ImportOrderWalk::isBeingReached(VM& vm, AbstractModuleRecord* record)
{
    if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(record))
        return synthetic->isRunningDeferredModule();
    for (ImportOrderWalk* walk = vm.importOrderState().innermostWalk; walk; walk = walk->m_outer) {
        if (walk->m_path.contains(record))
            return true;
    }
    return false;
}

bool ImportOrderWalk::isOnPathOfWalk(VM& vm, AbstractModuleRecord* record, const ImportOrderWalk* otherThan)
{
#if ENABLE(AOT)
    if (auto* compiled = AOT::compiledModule(vm, record))
        return compiled->isOnPathOfWalk(otherThan);
#endif
    for (ImportOrderWalk* walk = vm.importOrderState().innermostWalk; walk; walk = walk->m_outer) {
        if (walk != otherThan && walk->m_path.contains(record))
            return true;
    }
    return false;
}

JSPromise* ImportOrderWalk::pendingLoadBelow(CyclicModuleRecord* root)
{
    VisitedRecords visited;
    Vector<AbstractModuleRecord*, 16> worklist { root };
    while (!worklist.isEmpty()) {
        auto* module = dynamicDowncast<CyclicModuleRecord>(worklist.takeLast());
        if (!module || module->status() != CyclicModuleRecord::Status::New || isCompiled(module->vm(), module) || hasFailedForGood(module) || !visited.add(module).isNewEntry)
            continue;
        for (auto& [request, resolved] : module->resolvedRequests()) {
            ModuleRegistryEntry* entry = entryOfKey(module, resolved.key.get(), request.second);
            if (!entry)
                continue;
            if (JSPromise* promise = entry->loadPromise(); promise && promise->status() == JSPromise::Status::Pending)
                return promise;
            worklist.append(entry->record());
        }
        for (auto& loaded : module->loadedModules().values())
            worklist.append(loaded.m_module.get());
    }
    return nullptr;
}

auto ImportOrderWalk::modulesYetToBeLinkedBelow(CyclicModuleRecord* root) -> VisitedRecords
{
    VisitedRecords visited;
    Vector<AbstractModuleRecord*, 16> worklist { root };
    while (!worklist.isEmpty()) {
        auto* module = dynamicDowncast<CyclicModuleRecord>(worklist.takeLast());
        if (!module || !isYetToBeLinked(module) || !visited.add(module).isNewEntry)
            continue;
        for (auto& [request, resolved] : module->resolvedRequests()) {
            if (ModuleRegistryEntry* entry = entryOfKey(module, resolved.key.get(), request.second))
                worklist.append(entry->record());
        }
        for (auto& loaded : module->loadedModules().values())
            worklist.append(loaded.m_module.get());
    }
    return visited;
}

Vector<CyclicModuleRecord*> ImportOrderWalk::modulesAboutToBeLinked(VM& vm)
{
    Vector<CyclicModuleRecord*> modules;
    if (!EvaluationAheadOfLink::isUnderWay(vm))
        return modules;
    Vector<AbstractModuleRecord*, 16> worklist;
    for (ImportOrderWalk* walk = vm.importOrderState().innermostWalk; walk; walk = walk->m_outer) {
        if (walk->m_evaluation.isEvaluatingModule())
            worklist.append(walk->m_root);
    }
    VisitedRecords visited;
    while (!worklist.isEmpty()) {
        auto* module = dynamicDowncast<CyclicModuleRecord>(worklist.takeLast());
        if (!module || !isYetToBeLinked(module) || !visited.add(module).isNewEntry)
            continue;
        if (!isCompiled(vm, module))
            modules.append(module);
        for (auto& loaded : module->loadedModules().values())
            worklist.append(loaded.m_module.get());
    }
    return modules;
}

bool ImportOrderWalk::wouldComeToRunningModule(JSGlobalObject* globalObject, AbstractModuleRecord* root)
{
    auto isRunning = [&](AbstractModuleRecord* record) {
        if (auto* module = dynamicDowncast<CyclicModuleRecord>(record))
            return module->status() == CyclicModuleRecord::Status::Evaluating;
        auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(record);
        return synthetic && synthetic->deferredModule() && (synthetic->isRunningDeferredModule() || synthetic->deferredModule()->isBeingRunByHost(globalObject, synthetic));
    };
    if (isRunning(root))
        return true;
    auto* cyclicRoot = dynamicDowncast<CyclicModuleRecord>(root);
    if (!cyclicRoot)
        return false;
#if ENABLE(AOT)
    if (auto* compiled = AOT::compiledModule(globalObject->vm(), root); compiled && compiled->isOrImportsRunningFile())
        return true;
#endif
    for (AbstractModuleRecord* module : modulesYetToBeLinkedBelow(cyclicRoot)) {
        if (EvaluationAheadOfLink::waitsForEvaluationOfImport(globalObject, EvaluationAheadOfLink::moduleOf(globalObject->vm(), uncheckedDowncast<CyclicModuleRecord>(module))))
            return true;
#if ENABLE(AOT)
        if (auto* compiled = AOT::compiledModule(globalObject->vm(), module); compiled && compiled->isOrImportsRunningFile())
            return true;
#endif
        for (auto& [request, resolved] : module->resolvedRequests()) {
            if (ModuleRegistryEntry* entry = entryOfKey(module, resolved.key.get(), request.second); entry && isRunning(entry->record()))
                return true;
        }
        for (auto& loaded : module->loadedModules().values()) {
            if (isRunning(loaded.m_module.get()))
                return true;
        }
    }
    return false;
}

void ImportOrderWalk::forEachLoaderOfWalkInProgress(VM& vm, NOESCAPE const Function<void(JSModuleLoader*)>& function)
{
    for (ImportOrderWalk* walk = vm.importOrderState().innermostWalk; walk; walk = walk->m_outer)
        function(walk->m_loaderOfRoot);
}

bool ImportOrderWalk::hasPostponedImportOf(JSModuleLoader* loader, const Identifier& key)
{
    for (ImportOrderWalk* walk = loader->vm().importOrderState().innermostWalk; walk; walk = walk->m_outer) {
        for (auto& [module, request] : walk->m_postponed) {
            if (module->moduleLoader() == loader && module->resolvedRequests().get(keyOfRequest(request)).key == key.impl())
                return true;
        }
    }
    return false;
}

void ImportOrderWalk::keepImportsOfWhatHasBegun(VM& vm)
{
    if (vm.importOrderState().isInBatchOfChanges)
        return;
    for (ImportOrderWalk* walk = vm.importOrderState().innermostWalk; walk; walk = walk->m_outer) {
        for (auto& [module, request] : walk->m_postponed) {
            if (isCompiled(vm, module) || !isYetToBeLinked(module) || module->loadedModules().contains(keyOfRequest(request)))
                continue;
            if (Answer answer = walk->answerTo(module, request); answer.kind == Answer::Kind::Record)
                walk->takeAnswer(module, request, answer.entry->record(), Turn::HasCome);
        }
    }
    for (CyclicModuleRecord* module : modulesAboutToBeLinked(vm)) {
        for (auto& loaded : module->loadedModules().values()) {
            // The host has run what supplies them.
            if (loaded.m_module->hasLiveExports())
                loaded.m_module->setWasReached();
            loaded.m_wasReached |= loaded.m_module->wasReached() || loaded.m_module->hasStartedLinking();
        }
    }
}

uint64_t ImportOrderWalk::noteChange(VM& vm)
{
    keepImportsOfWhatHasBegun(vm);
    auto& state = vm.importOrderState();
    return state.lastChange = ++state.clock;
}

JSPromise* ImportOrderWalk::run(CyclicModuleRecord* root, bool mayHaveImportsInFlight)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    [[maybe_unused]] uint64_t clockAtStart = m_state.clock;
    for (bool needsImports = mayHaveImportsInFlight || (root->status() == CyclicModuleRecord::Status::New && !isCompiled(m_vm, root)); ; needsImports = true) {
#if ENABLE(AOT)
        if (auto* compiled = m_mode == Mode::WaitsForImports ? AOT::compiledModule(m_vm, root) : nullptr; compiled && (compiled->tryGiveWayToCopyAfterChangeOfPlugins() || (m_state.lastChange > clockAtStart && compiled->tryGiveWayToCopyFromDisk()))) [[unlikely]]
            return nullptr;
#endif
        // No module runs while one that may precede it is in flight, so where the walk yields to other jobs does not depend on how long a fetch takes.
        if (needsImports) [[unlikely]] {
            m_visited.clear();
            m_hasFailureAhead = false;
            m_importInFlightInScript = nullptr;
            if (m_passed && std::exchange(m_passed->lastChange, m_state.lastChange) != m_state.lastChange)
                forgetPassedImports();
#if ENABLE(AOT)
            m_keysPostponedForCompiledModules.shrink(0);
#endif
            m_hasFailureAhead = !requestImportsBelow(root) && !(m_passed && m_importInFlightInScript);
            if (scope.exception()) [[unlikely]]
                break;
            if (m_importInFlight)
                return m_importInFlight;
            if (m_passed) {
                if (m_hasFailureAhead || (!m_importInFlightInScript && !m_hasImportHeldBackAhead))
                    return nullptr;
            } else if (m_importInFlightInScript && !m_hasFailureAhead)
                return m_importInFlightInScript;
            m_visited.clear();
            m_postponed.shrink(0);
            m_importHeldBack = nullptr;
#if ENABLE(AOT)
            m_didRequestImports = true;
            m_compiledModulesToLink.clear();
            m_hasFactoryBehindImportHeldBack = false;
            m_didLeaveFactoryToFetch = false;
#endif
        }

        m_evaluation.beginPass(m_passed && m_passed->includeModuleToEvaluate);
        m_evaluation.setHasFailureAhead(m_hasFailureAhead);
        Outcome outcome = reachImport(root);
        EXCEPTION_ASSERT(!!scope.exception() == (outcome == Outcome::Threw));
#if ENABLE(AOT)
        if (!m_compiledModulesToLink.isEmpty() && m_mode == Mode::WaitsForImports) {
            bool hasRunAllItCan = outcome == Outcome::Passed || (outcome == Outcome::PassedInPart && m_postponed.isEmpty() && !m_importHeldBack);
            if (hasRunAllItCan && m_factoriesAheadOfFailure)
                return m_factoriesAheadOfFailure;
            if (hasRunAllItCan && !m_didCallFactories && (outcome == Outcome::PassedInPart || mayHaveImportsInFlight || clockAtStart != m_state.clock)) {
                JSPromise* pendingFactory = nullptr;
                outcome = std::max(outcome == Outcome::PassedInPart ? Outcome::NeedsImports : outcome, callFactoriesOfCompiledModules(pendingFactory));
                EXCEPTION_ASSERT(!!scope.exception() == (outcome == Outcome::Threw));
                if (pendingFactory)
                    return pendingFactory;
            }
            if (outcome == Outcome::Passed) {
                prepareCompiledModulesForLink();
                if (scope.exception()) [[unlikely]]
                    break;
            }
        } else if (!m_compiledModulesToLink.isEmpty() && outcome == Outcome::PassedInPart && hasPassedImportHeldBack() && !m_hasFactoryBehindImportHeldBack && !m_passed && !m_vm.synchronousModuleQueue()) {
            JSPromise* pendingFactory = nullptr;
            outcome = std::max(outcome, callFactoriesOfCompiledModules(pendingFactory));
            EXCEPTION_ASSERT(!!scope.exception() == (outcome == Outcome::Threw));
            if (pendingFactory)
                return pendingFactory;
            if (ModuleRegistryEntry* entry = outcome == Outcome::Threw && !m_vm.isTerminationException(scope.exception()) ? entryOf(root) : nullptr)
                entry->failToLoad(m_globalObject, scope.exception()->value(), ModuleRegistryEntry::IsFinal::Yes, m_evaluation.didModuleThrow());
        }
#endif
        if (outcome == Outcome::Passed) [[likely]]
            return nullptr;
        if (outcome == Outcome::Threw) {
            askForPostponedImportsAfterThrow();
            break;
        }
        if (m_didStopAtImportInFlight)
            return m_importInFlightInScript;
        if (outcome == Outcome::NeedsImports && !m_isFailureBehindPostponedImports)
            continue;
        if (m_mode != Mode::WaitsForImports)
            return m_passed ? m_importHeldBack : nullptr;
        if (m_postponed.isEmpty())
            return m_importHeldBack;
        askForPostponedImports();
        if (scope.exception()) [[unlikely]]
            break;
        if (std::exchange(m_isFailureBehindPostponedImports, false))
            ++m_state.clock;
    }

    if (ModuleRegistryEntry* entry = m_didFailToTakeOverImport || m_evaluation.didFailToLink() || isOnPathOfWalk(m_vm, root) ? nullptr : entryOf(root))
        entry->setIsFailureFinal();
    return nullptr;
}

auto ImportOrderWalk::reach(CyclicModuleRecord* module) -> Outcome
{
    using Status = CyclicModuleRecord::Status;
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    if (!m_visited.add(module).isNewEntry) {
        m_evaluation.didComeBackTo(module);
        return Outcome::Passed;
    }
    if (!m_vm.isSafeToRecurse()) [[unlikely]] {
        throwException(m_globalObject, scope, createModuleGraphTooDeepError(m_globalObject));
        return Outcome::Threw;
    }
    if (hasFailedForGood(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, failAgain(module));

    module->setWasReached();
    unsigned index = m_evaluation.enter(module);
    m_path.append(module);
    Outcome outcome = reachImportsOf(module);
    m_path.removeLast();
    EXCEPTION_ASSERT(!!scope.exception() == (outcome == Outcome::Threw));
    m_evaluation.leave(outcome == Outcome::Passed);

    switch (outcome) {
    case Outcome::Passed:
        if (module->status() == Status::New && m_mode != Mode::RunsAheadOfLoad) [[unlikely]]
            didLoad(module);
        break;
    case Outcome::PassedInPart:
    case Outcome::NeedsImports:
        // It is loading again, so a load that comes to it visits what it imports.
        if (module->status() == Status::Unlinked)
            module->setStatus(Status::New);
        break;
    case Outcome::Threw:
        if (module->status() != Status::New || m_didFailToTakeOverImport || !m_evaluation.isFailureOfImportOf(index) || m_vm.isTerminationException(scope.exception()))
            break;
        if (ModuleRegistryEntry* entry = entryOf(module))
            failToLoad(entry, scope.exception()->value());
        break;
    }
    return outcome;
}

auto ImportOrderWalk::reachImportsOf(CyclicModuleRecord* module) -> Outcome
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    Outcome outcome = Outcome::Passed;
    const Vector<ModuleRequest>& requests = module->requestedModules();
    for (unsigned index = numberOfImportsPassed(module); index < requests.size(); ++index) {
        const ModuleRequest& request = requests[index];
        if (didHostJustFail(module, request)) [[unlikely]] {
            // The load was rejected with the first of its failures in time, which need not be this one.
            Answer answer = answerTo(module, request);
            JSValue error = answer.kind == Answer::Kind::Failure && answer.entry ? errorOfEntryWithoutRecord(m_globalObject, answer.entry) : m_errorOfHost;
            RETURN_IF_EXCEPTION(scope, Outcome::Threw);
            RELEASE_AND_RETURN(scope, failToImport(module, request, error));
        }
        AbstractModuleRecord* imported = module->requestsPrelinkedModule(index) ? module->prelinkedRequestedModule(index) : nullptr;
        if (!imported) {
            auto loaded = module->loadedModules().find(keyOfRequest(request));
            if (loaded != module->loadedModules().end()) [[likely]] {
                if (isObsolete(module, loaded->value)) [[unlikely]]
                    return Outcome::NeedsImports;
                imported = loaded->value.m_module.get();
                loaded->value.m_wasReached |= !isDeferredModuleYetToRun(imported);
            } else {
                Outcome outcomeOfImport = reachImportThatHasNotLoaded(module, request, imported);
                RETURN_IF_EXCEPTION(scope, Outcome::Threw);
                if (outcomeOfImport == Outcome::NeedsImports || m_evaluation.didStopAtDeferredModule())
                    return outcomeOfImport;
                if (outcomeOfImport == Outcome::PassedInPart) {
                    if (!isYetToBeLinked(module)) [[unlikely]]
                        return Outcome::Passed;
                    outcome = outcomeOfImport;
                    m_evaluation.didLeaveImportForLater();
                    continue;
                }
            }
        }

        uint64_t lastChange = m_state.lastChange;
        size_t numberOfModulesAhead = m_evaluation.numberOfModulesAhead();
        bool isBelowDeferredImport = m_evaluation.isBelowDeferredImport();
        m_evaluation.setIsBelowDeferredImport(isBelowDeferredImport || request.m_phase == AbstractModuleRecord::ModulePhase::Defer);
        auto didLeaveImport = makeScopeExit([&] {
            m_evaluation.setIsBelowDeferredImport(isBelowDeferredImport);
        });
        m_importOfDeferredModule = isDeferredModuleYetToRun(imported) ? &request : nullptr;
        Outcome outcomeOfImport = reachImport(imported);
        RETURN_IF_EXCEPTION(scope, Outcome::Threw);
        if (outcomeOfImport == Outcome::NeedsImports || m_evaluation.didStopAtDeferredModule())
            return outcomeOfImport;
        if (lastChange != m_state.lastChange && m_mode != Mode::KeepsAnswers) [[unlikely]]
            return Outcome::NeedsImports;
        // A load that a deferred module started has linked it.
        if (!isYetToBeLinked(module)) [[unlikely]]
            return Outcome::Passed;
        if (hasFailedForGood(module)) [[unlikely]]
            RELEASE_AND_RETURN(scope, failAgain(module));
        if (outcomeOfImport == Outcome::PassedInPart) {
            outcome = outcomeOfImport;
            m_evaluation.didLeaveImportForLater();
        }
        if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(imported))
            m_evaluation.didPassImport(EvaluationAheadOfLink::moduleOf(m_vm, cyclic), numberOfModulesAhead);
        if (m_passed && outcome == Outcome::Passed) [[unlikely]] {
            m_passed->countByImporter.set(module, index + 1);
            m_passed->includeModuleToEvaluate = m_evaluation.hasLeftModuleForLater();
        }
    }
    return outcome;
}

void ImportOrderWalk::forgetPassedImports()
{
    m_passed->countByImporter.clear();
    m_passed->includeModuleToEvaluate = false;
    m_passed->areAllThatCanBePassed = false;
}

auto ImportOrderWalk::reachImport(AbstractModuleRecord* imported) -> Outcome
{
    if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(imported))
        return synthetic->isReadyToBeLinked() ? Outcome::Passed : reachSynthetic(synthetic);
#if ENABLE(AOT)
    if (auto* compiled = AOT::compiledModule(m_vm, imported))
        return reachCompiled(compiled);
#endif
    auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(imported);
    return isYetToBeLinked(cyclic) ? reach(cyclic) : Outcome::Passed;
}

auto ImportOrderWalk::reachImportThatHasNotLoaded(CyclicModuleRecord* module, const ModuleRequest& request, AbstractModuleRecord*& imported) -> Outcome
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    if (module->loadedAt() < m_state.lastChangeOfResolution && m_mode != Mode::KeepsAnswers)
        return Outcome::NeedsImports;

    Answer answer = answerTo(module, request);
    if (answer.kind == Answer::Kind::Record) {
        imported = answer.entry->record();
        takeAnswer(module, request, imported, Turn::HasCome);
        return Outcome::Passed;
    }

    if (answer.kind == Answer::Kind::Failure) {
        if (!answer.entry) {
            if (JSValue error = failureToResolve(module->moduleLoader(), module->moduleKey(), request.m_specifier, Turn::HasCome))
                RELEASE_AND_RETURN(scope, failToImport(module, request, error));
        } else if (!answer.entry->isFailureObsolete(m_vm) || m_mode == Mode::KeepsAnswers) {
            JSValue error = errorOfEntryWithoutRecord(m_globalObject, answer.entry);
            RETURN_IF_EXCEPTION(scope, Outcome::Threw);
            RELEASE_AND_RETURN(scope, failToImport(module, request, error));
        }
    } else if (m_mode == Mode::KeepsAnswers) {
        throwTypeError(m_globalObject, scope, makeString("Cannot link a module whose import '"_s, request.m_specifier.string(), "' is still loading."_s));
        return Outcome::Threw;
    } else if (answer.kind == Answer::Kind::Postponed) {
        postpone(module, request);
        RELEASE_AND_RETURN(scope, passImportLeftForLater(module, request));
    } else if (answer.kind == Answer::Kind::HeldBack) {
        m_importHeldBack = answer.entry->ensureModulePromise(m_globalObject);
        RELEASE_AND_RETURN(scope, passImportLeftForLater(module, request));
    } else if (answer.kind == Answer::Kind::InFlight)
        return reachImportInFlight(answer);

    JSValue error = ask(module, request, Turn::HasCome);
    RETURN_IF_EXCEPTION(scope, Outcome::Threw);
    if (error)
        RELEASE_AND_RETURN(scope, failToImport(module, request, error));
    return Outcome::NeedsImports;
}

void ImportOrderWalk::takeAnswer(CyclicModuleRecord* module, const ModuleRequest& request, AbstractModuleRecord* imported, Turn turn)
{
    bool wasLeftForLater = module->resolvedRequests().take(keyOfRequest(request)).wasLeftForLater;
    module->setImportedModule(m_globalObject, request, imported);
    module->loadedModules().find(keyOfRequest(request))->value.m_wasReached = turn == Turn::IsToCome ? wasLeftForLater : !isDeferredModuleYetToRun(imported);
}

void ImportOrderWalk::postpone(CyclicModuleRecord* module, const ModuleRequest& request)
{
    m_postponed.append({ module, request });
    m_importersThatPostponed.appendWithCrashOnOverflow(module);
}

auto ImportOrderWalk::passImportLeftForLater(CyclicModuleRecord* module, const ModuleRequest& request) -> Outcome
{
    module->resolvedRequests().find(keyOfRequest(request))->value.wasLeftForLater = true;
#if ENABLE(AOT)
    if (AOT::ModuleRecord* compiled = compiledModuleBehindFetch(module, request)) {
        uint64_t lastChange = m_state.lastChange;
        bool isBelowDeferredImport = m_evaluation.isBelowDeferredImport();
        m_evaluation.setIsBelowDeferredImport(isBelowDeferredImport || request.m_phase == AbstractModuleRecord::ModulePhase::Defer);
        m_evaluation.didLeaveImportForLater();
        Outcome outcome = compiled->reachImportsAheadOfFetch(*this);
        m_evaluation.didPassImport(compiled->moduleAheadOfLink(), m_evaluation.numberOfModulesAhead());
        m_evaluation.setIsBelowDeferredImport(isBelowDeferredImport);
        return std::max(outcome, lastChange == m_state.lastChange ? Outcome::PassedInPart : Outcome::NeedsImports);
    }
#endif
    return Outcome::PassedInPart;
}

auto ImportOrderWalk::evaluateModulesAheadOfDeferredModule() -> Outcome
{
    using KeepsAnswers = EvaluationAheadOfLink::KeepsAnswers;
    using Result = EvaluationAheadOfLink::Result;

    if (m_evaluation.stopsAtDeferredModule()) [[unlikely]]
        return Outcome::PassedInPart;
    switch (m_evaluation.evaluateModulesAhead(m_mode == Mode::KeepsAnswers ? KeepsAnswers::Yes : KeepsAnswers::No)) {
    case Result::IsDone:
        return Outcome::Passed;
    case Result::ChangedAnswers:
        return Outcome::NeedsImports;
    case Result::Threw:
        return Outcome::Threw;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

auto ImportOrderWalk::reachSynthetic(SyntheticModuleRecord* record) -> Outcome
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    const ModuleRequest* request = std::exchange(m_importOfDeferredModule, nullptr);

    if (JSValue error = record->errorOfGenerator()) [[unlikely]] {
        // No load in progress led to the importer when the module failed.
        if (m_mode != Mode::KeepsAnswers && !m_path.isEmpty() && unloadImportsOf(record, m_path.last()))
            return Outcome::NeedsImports;
        Outcome outcome = evaluateModulesAheadOfDeferredModule();
        RETURN_IF_EXCEPTION(scope, Outcome::Threw);
        if (outcome != Outcome::Passed)
            return outcome;
        m_evaluation.setDidModuleThrow();
        RELEASE_AND_RETURN(scope, fail(record, error));
    }

    if (record->isRunningDeferredModule()) [[unlikely]] {
        if (m_mode == Mode::RunsAheadOfLoad)
            return Outcome::PassedInPart;
        // Reading what it has exported so far can call getters, so a load does it once.
        if (record->moduleEnvironmentMayBeNull() && (m_isBehindWalk || m_visited.contains(record)))
            return Outcome::Passed;
        if (m_evaluation.stopsAtDeferredModule())
            return Outcome::PassedInPart;
    } else if (record->deferredModule()) {
        Outcome outcome = evaluateModulesAheadOfDeferredModule();
        RETURN_IF_EXCEPTION(scope, Outcome::Threw);
        if (outcome != Outcome::Passed)
            return outcome;
        if (request) {
            if (auto loaded = m_path.last()->loadedModules().find(keyOfRequest(*request)); loaded != m_path.last()->loadedModules().end())
                loaded->value.m_wasReached = true;
        }
        // One of them has loaded it.
        if (record->isReadyToBeLinked() || record->errorOfGenerator()) [[unlikely]]
            RELEASE_AND_RETURN(scope, reachImport(record));
    }

    if (record->deferredModule()) {
        record->setWasReached();
        SetForScope isRunningDeferredModule(m_state.isRunningDeferredModule, true);
        ++m_state.clock;
        record->runDeferredModule(m_globalObject);
        ++m_state.clock;
        if (record->deferredModule()) [[unlikely]]
            m_visited.add(record);
        else if (ModuleRegistryEntry* entry = entryOf(record))
            entry->releaseSource();
    } else {
        record->copyMutableExportsIfOutdated(m_globalObject);
        record->setWasReached();
    }

    Exception* exception = scope.exception();
    if (!exception) [[likely]]
        return Outcome::Passed;
    m_evaluation.setDidModuleThrow();
    // If the generator did not throw it for good, it is the failure of the import and not of the module.
    if (!record->errorOfGenerator())
        return Outcome::Threw;
    TRY_CLEAR_EXCEPTION(scope, Outcome::Threw);
    RELEASE_AND_RETURN(scope, fail(record, exception->value()));
}

#if ENABLE(AOT)
auto ImportOrderWalk::importsThatWalksHaveReached(AbstractModuleRecord* imported, std::span<AbstractModuleRecord* const> importsOfCompiledModules) -> ImportsOfModules
{
    VM& vm = imported->vm();
    ImportsOfModules imports;
    auto worklist = rootsOfLoadsInProgress(imported->moduleLoader());
    worklist.append(importsOfCompiledModules);
    VisitedRecords visited;
    while (!worklist.isEmpty()) {
        auto* module = dynamicDowncast<CyclicModuleRecord>(worklist.takeLast());
        if (!module || !visited.add(module).isNewEntry || (!isCompiled(vm, module) && !isYetToBeLinked(module)))
            continue;
        for (auto& [request, loaded] : module->loadedModules()) {
            if (loaded.m_module.get() != imported)
                worklist.append(loaded.m_module.get());
            else if (loaded.m_wasReached && !isCompiled(vm, module))
                imports.append({ module, request });
        }
    }
    return imports;
}

auto ImportOrderWalk::reachCompiled(AOT::ModuleRecord* module) -> Outcome
{
    if (module != m_root && m_mode != Mode::KeepsAnswers && (module->tryGiveWayToCopyAfterChangeOfPlugins() || module->tryGiveWayToCopyFromDisk())) [[unlikely]]
        return Outcome::NeedsImports;
    return module->reachImports(*this);
}

AOT::ModuleRecord* ImportOrderWalk::compiledModuleBehindFetch(CyclicModuleRecord* importer, const ModuleRequest& request) const
{
    if (!m_vm.hasCompiledCode() || importer->moduleLoader() != m_globalObject->moduleLoader() || request.type() != ScriptFetchParameters::Type::JavaScript)
        return nullptr;
    return AOT::ModuleRecord::behindFetchOf(m_globalObject, importer->resolvedRequests().get(keyOfRequest(request)).key.get());
}

void ImportOrderWalk::willLinkBehindWalk(AOT::ModuleRecord* module)
{
    m_compiledModulesToLink.appendWithCrashOnOverflow(module);
}

auto ImportOrderWalk::callFactoriesOfCompiledModules(JSPromise*& pendingFactory) -> Outcome
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    uint64_t lastChange = m_state.lastChange;
    for (size_t index = 0; index < m_compiledModulesToLink.size() && !pendingFactory; ++index) {
        pendingFactory = uncheckedDowncast<AOT::ModuleRecord>(m_compiledModulesToLink.at(index))->callFactoriesInGraph();
        RETURN_IF_EXCEPTION(scope, Outcome::Threw);
    }
    m_didCallFactories = true;
    return lastChange == m_state.lastChange ? Outcome::Passed : Outcome::NeedsImports;
}

void ImportOrderWalk::prepareCompiledModulesForLink()
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    VisitedRecords visited;
    for (size_t index = 0; index < m_compiledModulesToLink.size(); ++index) {
        uncheckedDowncast<AOT::ModuleRecord>(m_compiledModulesToLink.at(index))->captureSyntheticExports(visited);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

auto ImportOrderWalk::reachImportThatHasNotLoaded(AOT::ModuleRecord* module, const ModuleRequest& request) -> Outcome
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    ASSERT(m_mode != Mode::KeepsAnswers);

    if (m_keysPostponedForCompiledModules.contains(request.m_specifier)) {
        postpone(module, request);
        return Outcome::PassedInPart;
    }
    Answer answer = answerWithoutRecord(module->moduleLoader()->getRegisteredMayBeNull(request.m_specifier, request.type()));
    if (answer.kind == Answer::Kind::HeldBack) {
        if (!m_didRequestImports && takesOver(answer))
            return Outcome::NeedsImports;
        m_importHeldBack = answer.entry->ensureModulePromise(m_globalObject);
        return Outcome::PassedInPart;
    }
    if (answer.kind == Answer::Kind::InFlight)
        return reachImportInFlight(answer);

    JSValue error;
    if (answer.kind == Answer::Kind::Failure && !answer.entry->isFailureObsolete(m_vm))
        error = errorOfEntryWithoutRecord(m_globalObject, answer.entry);
    else
        error = ask(module, request, Turn::HasCome);
    RETURN_IF_EXCEPTION(scope, Outcome::Threw);
    if (!error)
        return Outcome::NeedsImports;
    if (ModuleRegistryEntry* entry = module->moduleLoader()->getRegisteredMayBeNull(request.m_specifier, request.type()); answerWithoutRecord(entry).kind == Answer::Kind::Failure && m_postponed.isEmpty())
        entry->setIsFailureFinal();
    RELEASE_AND_RETURN(scope, failToImport(module, error));
}

bool ImportOrderWalk::requestImportThatHasNotLoaded(AOT::ModuleRecord* module, const ModuleRequest& request)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    auto entry = [&] {
        return module->moduleLoader()->getRegisteredMayBeNull(request.m_specifier, request.type());
    };
    Answer answer = answerWithoutRecord(entry());
    if (answer.kind == Answer::Kind::None || takesOver(answer)) {
        JSValue error = ask(module, request, Turn::IsToCome);
        RETURN_IF_EXCEPTION(scope, false);
        if (error)
            return false;
        if (!m_promiseOfLastRequest) {
            m_keysPostponedForCompiledModules.append(request.m_specifier);
            return true;
        }
        if (entry() && entry()->record())
            RELEASE_AND_RETURN(scope, requestImportsBelowImport(entry()->record()));
        answer = answerWithoutRecord(entry());
        if (answer.kind == Answer::Kind::None)
            m_importInFlight = m_promiseOfLastRequest;
    }
    return noteImportInFlight(answer) && answer.kind != Answer::Kind::Failure;
}

bool ImportOrderWalk::willCallFactoriesAheadOfFailure()
{
    return m_mode == Mode::WaitsForImports && m_vm.synchronousModuleQueue() && !m_didCallFactories && !std::exchange(m_didCallFactoriesAheadOfFailure, true);
}

auto ImportOrderWalk::didCallFactoriesAheadOfFailure(JSPromise* pendingFactories) -> Outcome
{
    m_factoriesAheadOfFailure = pendingFactories;
    ++m_state.clock;
    return Outcome::NeedsImports;
}

auto ImportOrderWalk::failToImport(AOT::ModuleRecord* module, JSValue error) -> Outcome
{
    if (!m_postponed.isEmpty() && m_mode == Mode::WaitsForImports) {
        m_isFailureBehindPostponedImports = true;
        return Outcome::NeedsImports;
    }
    m_didFailToImport = true;
    return fail(module, error);
}
#endif

void ImportOrderWalk::askForPostponedImports()
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    for (auto& [module, request] : std::exchange(m_postponed, { })) {
        if (!isYetToBeLinked(module) || module->loadedModules().contains(keyOfRequest(request)) || (!isCompiled(m_vm, module) && answerTo(module, request).kind != Answer::Kind::Postponed))
            continue;
        JSValue error = ask(module, request, Turn::IsLast);
        RETURN_IF_EXCEPTION(scope, void());
        if (!error)
            continue;
        // It fails when it comes to the import again.
        if (isCompiled(m_vm, module) && answerWithoutRecord(module->moduleLoader()->getRegisteredMayBeNull(request.m_specifier, request.type())).kind == Answer::Kind::Failure)
            return;
        if (Answer answer = answerTo(module, request); answer.kind == Answer::Kind::Failure && answer.entry && answer.entry->wasErrorThrownByModule())
            return;
        scope.release();
        failToImport(module, request, error);
        return;
    }
}

// They precede what has thrown.
void ImportOrderWalk::askForPostponedImportsAfterThrow()
{
    if (m_postponed.isEmpty() || m_mode != Mode::WaitsForImports || m_vm.hasPendingTerminationException())
        return;
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    Exception* exception = scope.exception();
    TRY_CLEAR_EXCEPTION(scope, void());
    askForPostponedImports();
    TRY_CLEAR_EXCEPTION(scope, void());
    throwException(m_globalObject, scope, exception);
}

// Returns false at the first import that has failed. What follows it is of no use unless a module that precedes it makes up for the failure.
bool ImportOrderWalk::requestImportsBelow(CyclicModuleRecord* module)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    if (!m_visited.add(module).isNewEntry)
        return true;
    if (!m_vm.isSafeToRecurse()) [[unlikely]] {
        throwException(m_globalObject, scope, createModuleGraphTooDeepError(m_globalObject));
        return false;
    }
    if (hasFailedForGood(module))
        return false;
#if ENABLE(AOT)
    if (auto* compiled = AOT::compiledModule(m_vm, module))
        RELEASE_AND_RETURN(scope, compiled->requestImportsBelow(*this));
#endif

    dropObsoleteAnswers(module);

    const Vector<ModuleRequest>& requests = module->requestedModules();
    for (unsigned index = numberOfImportsPassed(module); index < requests.size() && isYetToBeLinked(module); ++index) {
        const ModuleRequest& request = requests[index];
        auto loadedModule = [&] -> AbstractModuleRecord* {
            if (AbstractModuleRecord* prelinked = module->prelinkedRequestedModule(index))
                return prelinked;
            auto loaded = module->loadedModules().find(keyOfRequest(request));
            return loaded == module->loadedModules().end() ? nullptr : loaded->value.m_module.get();
        };

        if (didHostJustFail(module, request))
            return false;
        AbstractModuleRecord* imported = loadedModule();
        if (!imported) {
            Answer answer = answerTo(module, request);
            if (answer.kind == Answer::Kind::None || takesOver(answer)) {
                JSValue error = ask(module, request, Turn::IsToCome);
                RETURN_IF_EXCEPTION(scope, false);
                if (error)
                    return false;
                imported = loadedModule();
                answer = imported ? Answer { } : answerTo(module, request);
                if (!imported && answer.kind == Answer::Kind::None && m_promiseOfLastRequest)
                    m_importInFlight = m_promiseOfLastRequest;
            }
            if (answer.kind == Answer::Kind::Failure)
                return false;
            if (answer.kind == Answer::Kind::Record) {
                imported = answer.entry->record();
                takeAnswer(module, request, imported, Turn::IsToCome);
            }
#if ENABLE(AOT)
            else if (answer.kind == Answer::Kind::Postponed || answer.kind == Answer::Kind::HeldBack)
                imported = compiledModuleBehindFetch(module, request);
#endif
            if (!noteImportInFlight(answer))
                return false;
        }
#if ENABLE(AOT)
        if (auto* compiled = imported != m_root ? AOT::compiledModule(m_vm, imported) : nullptr; compiled && compiled->tryGiveWayToCopyAfterChangeOfPlugins() && module->loadedModules().contains(keyOfRequest(request))) [[unlikely]] {
            dropObsoleteAnswers(module);
            --index;
            continue;
        }
#endif

        bool shouldContinue = !imported || requestImportsBelowImport(imported);
        RETURN_IF_EXCEPTION(scope, false);
        if (!shouldContinue)
            return false;
    }
    return true;
}

bool ImportOrderWalk::requestImportsBelowImport(AbstractModuleRecord* imported)
{
    if (auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(imported))
        return !synthetic->errorOfGenerator();
    auto* cyclic = uncheckedDowncast<CyclicModuleRecord>(imported);
    return !isYetToBeLinked(cyclic) || requestImportsBelow(cyclic);
}

void ImportOrderWalk::dropObsoleteAnswers(CyclicModuleRecord* module)
{
    using ResolvedRequest = AbstractModuleRecord::ResolvedRequest;

    const Vector<ModuleRequest>& requests = module->requestedModules();
    bool didChangeResolution = module->loadedAt() < m_state.lastChangeOfResolution;
    if (didChangeResolution) {
        for (auto& resolved : module->resolvedRequests().values())
            resolved.state = ResolvedRequest::State::Obsolete;
        for (const ModuleRequest& request : requests)
            failureToResolve(module->moduleLoader(), module->moduleKey(), request.m_specifier, Turn::IsToCome);
    }
    for (unsigned index = 0; index < requests.size() && module->loadedAt() < m_state.lastChange; ++index) {
        if (module->requestsPrelinkedModule(index))
            continue;
        auto loaded = module->loadedModules().find(keyOfRequest(requests[index]));
        if (loaded == module->loadedModules().end() || !isObsolete(module, loaded->value))
            continue;
        ResolvedRequest toAskAgain { loaded->value.m_module->moduleKey().impl(), ResolvedRequest::State::Obsolete };
#if ENABLE(AOT)
        if (auto* compiled = AOT::compiledModule(m_vm, loaded->value.m_module.get()); compiled && compiled->hasGivenWayToCopy() && (loaded->value.m_wasReached || !didChangeResolution))
            toAskAgain = { WTF::move(toAskAgain.key), ResolvedRequest::State::Loading, loaded->value.m_wasReached, ++m_state.numberOfResolvedRequests };
#endif
        module->resolvedRequests().set(loaded->key, WTF::move(toAskAgain));
        Locker locker { module->cellLock() };
        module->loadedModules().remove(loaded);
        module->setStatus(CyclicModuleRecord::Status::New);
    }
    module->setLoadedAt(m_state.clock);
}

ALWAYS_INLINE bool ImportOrderWalk::isObsolete(CyclicModuleRecord* module, const AbstractModuleRecord::LoadedModuleRequest& loaded) const
{
    uint64_t loadedAt = module->loadedAt();
    if (loadedAt >= m_state.lastChange) [[likely]]
        return false;
    if (module->hasStartedLinking() || m_mode == Mode::KeepsAnswers)
        return false;
    AbstractModuleRecord* imported = loaded.m_module.get();
    if (loadedAt < imported->supersededAt() && (!loaded.m_wasReached || imported->isSupersededForEarlierImports()))
        return true;
    return !loaded.m_wasReached && loadedAt < m_state.lastChangeOfResolution;
}

// A synchronous load that takes over an import in flight fails by itself, and so does a load of a module whose key was given to another.
ALWAYS_INLINE bool ImportOrderWalk::didHostJustFail(CyclicModuleRecord* module, const ModuleRequest& request) const
{
    if (!m_errorOfHost) [[likely]]
        return false;
    if (module->resolvedRequests().get(keyOfRequest(request)).failedAt != m_state.clock)
        return false;
    return m_vm.synchronousModuleQueue() || answerTo(module, request).kind != Answer::Kind::Failure;
}

// For a request that is not in [[LoadedModules]].
auto ImportOrderWalk::answerTo(CyclicModuleRecord* module, const ModuleRequest& request) const -> Answer
{
    using State = AbstractModuleRecord::ResolvedRequest::State;

    JSModuleLoader* loader = module->moduleLoader();
    if (loader->m_resolutionFailures.contains({ module->moduleKey().impl(), request.m_specifier.impl() }))
        return { Answer::Kind::Failure };
    auto resolved = module->resolvedRequests().find(keyOfRequest(request));
    if (resolved == module->resolvedRequests().end() || resolved->value.state == State::Obsolete)
        return { };
    ModuleRegistryEntry* entry = entryOfKey(module, resolved->value.key.get(), request.type());
    if (entry && entry->record())
        return { Answer::Kind::Record, entry };
    if (resolved->value.state == State::Postponed)
        return { Answer::Kind::Postponed };
    return answerWithoutRecord(entry);
}

auto ImportOrderWalk::answerWithoutRecord(ModuleRegistryEntry* entry) const -> Answer
{
    using Status = ModuleRegistryEntry::Status;
    using FetchWaitsFor = JSModuleLoader::FetchWaitsFor;

    if (!entry || entry->status() == Status::New)
        return { };
    if (entry->status() >= Status::FetchFailed || entry->ensureModulePromise(m_globalObject)->status() == JSPromise::Status::Rejected)
        return { Answer::Kind::Failure, entry };
    bool isHeldBack = entry->fetchWaitsFor() == FetchWaitsFor::DeferredModules;
    return { isHeldBack ? Answer::Kind::HeldBack : Answer::Kind::InFlight, entry };
}

JSValue ImportOrderWalk::failureToResolve(JSModuleLoader* loader, const Identifier& importer, const Identifier& specifier, Turn turn)
{
    auto failure = loader->m_resolutionFailures.find({ importer.impl(), specifier.impl() });
    if (failure == loader->m_resolutionFailures.end())
        return { };
    bool isObsolete = turn == Turn::IsToCome ? failure->value.time < m_state.lastChangeOfResolution : failure->value.time != m_state.clock;
    if (!isObsolete || m_mode == Mode::KeepsAnswers)
        return failure->value.error.get();
    Locker locker { loader->cellLock() };
    loader->m_resolutionFailures.remove(failure);
    return { };
}

// A synchronous load takes over what is in flight, unless it has found that it cannot.
bool ImportOrderWalk::takesOver(const Answer& answer) const
{
    if ((answer.kind != Answer::Kind::InFlight && answer.kind != Answer::Kind::HeldBack) || !m_vm.synchronousModuleQueue() || m_passed)
        return false;
    return !answer.entry->wasLoadStartedSynchronously() || answer.entry->ensureFetchPromise(m_globalObject)->status() != JSPromise::Status::Pending;
}

// Fetches tend to complete in the order they started, so the last one makes for the fewest waits.
// Returns false if it is as far as the walk goes.
bool ImportOrderWalk::noteImportInFlight(const Answer& answer)
{
    m_hasImportHeldBackAhead |= answer.kind == Answer::Kind::HeldBack;
    if (answer.kind != Answer::Kind::InFlight)
        return true;
    bool waitsForScript = answer.entry->fetchWaitsFor() == JSModuleLoader::FetchWaitsFor::Script;
    (waitsForScript ? m_importInFlightInScript : m_importInFlight) = answer.entry->ensureModulePromise(m_globalObject);
    return !waitsForScript || !m_passed;
}

auto ImportOrderWalk::reachImportInFlight(const Answer& answer) -> Outcome
{
    m_didStopAtImportInFlight = (!noteImportInFlight(answer) || m_hasFailureAhead) && !m_importInFlight;
    return Outcome::NeedsImports;
}

// Returns the error if the request fails on the spot.
JSValue ImportOrderWalk::ask(CyclicModuleRecord* module, const ModuleRequest& request, Turn turn)
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    bool useImportMap = true;
#if ENABLE(AOT)
    auto* compiled = AOT::compiledModule(m_vm, module);
    useImportMap = !compiled;
    if (compiled) {
        compiled->forgetAnswerTo(request);
        // The request is for a key, which can read like a specifier that the module could not resolve.
        failureToResolve(module->moduleLoader(), module->moduleKey(), request.m_specifier, turn);
    } else
#endif
        module->setStatus(CyclicModuleRecord::Status::New);
    ASSERT(isYetToBeLinked(module) && !module->loadedModules().contains(keyOfRequest(request)));
    JSPromise* loadPromise = JSPromise::create(m_vm, m_globalObject->promiseStructure());
    loadPromise->markAsHandled();
    auto* loadingState = ModuleGraphLoadingState::create(m_vm, loadPromise, m_scriptFetcher);
    OptionSet<JSModuleLoader::RequestFlag> flags;
    if (turn != Turn::IsToCome)
        flags.add(JSModuleLoader::RequestFlag::ForgetsObsoleteFailure);
    if (turn == Turn::IsLast)
        flags.add(JSModuleLoader::RequestFlag::CannotBePostponed);

    // Otherwise a host that changes how it resolves each time it is asked would be asked forever.
    SetForScope isRunningDeferredModule(m_state.isRunningDeferredModule, false);
#if ENABLE(AOT)
    uint8_t flagsBefore = compiled ? compiled->willLoadImportedModule(request.m_specifier) : 0;
#endif
    JSPromise* promise = module->moduleLoader()->hostLoadImportedModule(m_globalObject, module, request, loadingState, m_scriptFetcher, useImportMap, flags);
#if ENABLE(AOT)
    if (compiled)
        compiled->didLoadImportedModule(flagsBefore);
#endif
    if (Exception* exception = scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, { });
        m_errorOfHost = exception->value();
        // If so, it was thrown for an import of the imported module.
        if (module->loadedModules().contains(keyOfRequest(request)))
            return { };
        module->didFailForRequest(request);
        return m_errorOfHost;
    }
    m_promiseOfLastRequest = promise;
    if (!promise)
        return { };
    m_didRequestImportInFlight |= loadPromise->status() == JSPromise::Status::Pending;
    if (module->loadedModules().contains(keyOfRequest(request)))
        return { };
    promise->performPromiseThenWithInternalMicrotask(m_vm, InternalMicrotask::ModuleGraphLoadingError, nullptr, loadingState);
    return promise->status() == JSPromise::Status::Rejected ? promise->result() : JSValue();
}

void ImportOrderWalk::didLoad(CyclicModuleRecord* module)
{
    module->setStatus(CyclicModuleRecord::Status::Unlinked);
    // The error of a module that has started linking is that of the link.
    if (ModuleRegistryEntry* entry = module->hasStartedLinking() ? nullptr : entryOf(module))
        entry->forgetFailureToLoad(m_globalObject);
}

void ImportOrderWalk::failToLoad(ModuleRegistryEntry* entry, JSValue error)
{
    using IsFinal = ModuleRegistryEntry::IsFinal;
    if (!entry->isFailureFinal())
        entry->failToLoad(m_globalObject, error, isOnPathOfWalk(m_vm, entry->record(), this) ? IsFinal::No : IsFinal::Yes, m_evaluation.didModuleThrow());
}

void ImportOrderWalk::failToImportModuleThatThrew(CyclicModuleRecord* importer, JSValue error)
{
#if ENABLE(AOT)
    m_didFailToImport = true;
#endif
    fail(importer, error);
}

auto ImportOrderWalk::failToImport(CyclicModuleRecord* module, const ModuleRequest& request, JSValue error) -> Outcome
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    // The host may supply it when it is asked for those.
    if (!m_postponed.isEmpty() && m_mode == Mode::WaitsForImports) {
        m_isFailureBehindPostponedImports = true;
        return Outcome::NeedsImports;
    }

    Answer answer = answerTo(module, request);
    // What becomes of the module depends on the load that was taken over.
    if (answer.kind == Answer::Kind::InFlight || answer.kind == Answer::Kind::HeldBack || answer.kind == Answer::Kind::Record) {
        m_didFailToTakeOverImport = true;
        throwException(m_globalObject, scope, error);
        return Outcome::Threw;
    }
    if (answer.kind == Answer::Kind::Failure && answer.entry) {
        answer.entry->setIsFailureFinal();
        if (answer.entry->wasErrorThrownByModule()) {
            Outcome outcome = evaluateModulesAheadOfDeferredModule();
            RETURN_IF_EXCEPTION(scope, Outcome::Threw);
            if (outcome != Outcome::Passed)
                return outcome;
            m_evaluation.setDidModuleThrow();
        }
    }
#if ENABLE(AOT)
    m_didFailToImport = true;
#endif
    RELEASE_AND_RETURN(scope, fail(module, error));
}

auto ImportOrderWalk::failAgain(CyclicModuleRecord* module) -> Outcome
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    ModuleRegistryEntry* entry = entryOf(module);
    JSValue error = entry->error(m_globalObject);
    RETURN_IF_EXCEPTION(scope, Outcome::Threw);
    if (entry->wasErrorThrownByModule()) {
        Outcome outcome = evaluateModulesAheadOfDeferredModule();
        RETURN_IF_EXCEPTION(scope, Outcome::Threw);
        if (outcome != Outcome::Passed)
            return outcome;
        m_evaluation.setDidModuleThrow();
    }
    RELEASE_AND_RETURN(scope, fail(module, error));
}

Vector<AbstractModuleRecord*, 16> ImportOrderWalk::rootsOfLoadsInProgress(JSModuleLoader* loader)
{
    Vector<AbstractModuleRecord*, 16> roots;
    loader->forgetLoadsThatHaveEnded();
    for (auto& load : loader->m_loadsInProgress) {
        if (ModuleRegistryEntry* entry = loader->getRegisteredMayBeNull(load.root.key, load.root.type))
            roots.append(entry->record());
    }
    for (ImportOrderWalk* walk = loader->vm().importOrderState().innermostWalk; walk; walk = walk->m_outer)
        roots.append(walk->m_root);
    return roots;
}

// Leaves the loads in progress as a load that failed at `failed` does: whatever imports it has failed to load too.
auto ImportOrderWalk::fail(AbstractModuleRecord* failed, JSValue error) -> Outcome
{
    auto scope = DECLARE_THROW_SCOPE(m_vm);

    JSModuleLoader* loader = failed->moduleLoader();
    auto worklist = rootsOfLoadsInProgress(loader);
    for (auto& load : loader->m_loadsInProgress)
        load.passed = { };
    for (ImportOrderWalk* walk = this; walk; walk = walk->m_outer) {
        if (walk->m_passed)
            walk->forgetPassedImports();
    }
    using ImportsOfModule = Vector<std::pair<ModuleRegistryEntry*, const ModuleRequest*>, 2>;
    HashMap<AbstractModuleRecord*, ImportsOfModule> importers;
    VisitedRecords visited;
    while (!worklist.isEmpty()) {
        auto* importer = dynamicDowncast<CyclicModuleRecord>(worklist.takeLast());
        if (!importer || !visited.add(importer).isNewEntry)
            continue;
        if (isCompiled(m_vm, importer)) {
            for (auto& loaded : importer->loadedModules().values())
                worklist.append(loaded.m_module.get());
            continue;
        }
        if (!isYetToBeLinked(importer) || importer->hasStartedLinking())
            continue;
        ModuleRegistryEntry* entry = entryOf(importer);
        const Vector<ModuleRequest>& requests = importer->requestedModules();
        for (unsigned index = 0; index < requests.size(); ++index) {
            AbstractModuleRecord* imported = importer->prelinkedRequestedModule(index);
            if (!imported)
                imported = importer->loadedModules().get(keyOfRequest(requests[index])).m_module.get();
            if (!imported) {
                if (Answer answer = answerTo(importer, requests[index]); answer.kind == Answer::Kind::Record)
                    imported = answer.entry->record();
            }
            if (!imported)
                continue;
            worklist.append(imported);
            if (entry && !entry->isFailureFinal())
                importers.add(imported, ImportsOfModule { }).iterator->value.append({ entry, &requests[index] });
        }
    }

    if (ModuleRegistryEntry* entry = entryOf(failed))
        failToLoad(entry, error);
    VisitedRecords unloaded;
    worklist.append(failed);
    while (!worklist.isEmpty()) {
        AbstractModuleRecord* imported = worklist.takeLast();
        if (!unloaded.add(imported).isNewEntry)
            continue;
        imported->moduleLoader()->removeFromLoadedModules(imported);
        for (auto [entry, request] : importers.take(imported)) {
            auto* importer = uncheckedDowncast<CyclicModuleRecord>(entry->record());
            unloadImportsOf(imported, importer);
            importer->setStateOfResolvedRequest(*request, AbstractModuleRecord::ResolvedRequest::State::Failed);
            importer->setStatus(CyclicModuleRecord::Status::New);
            // Another walk may yet have to run what precedes the failure in an importer that this one did not come through.
            if (m_path.contains(importer))
                failToLoad(entry, error);
            else
                entry->failToLoad(m_globalObject, error, ModuleRegistryEntry::IsFinal::No, m_evaluation.didModuleThrow());
            worklist.append(importer);
        }
    }

    throwException(m_globalObject, scope, error);
    return Outcome::Threw;
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)

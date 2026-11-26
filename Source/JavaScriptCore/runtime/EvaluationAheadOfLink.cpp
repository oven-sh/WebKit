/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "EvaluationAheadOfLink.h"

#if USE(BUN_JSC_ADDITIONS)

#if ENABLE(AOT)
#include "AOTModuleRecord.h"
#endif
#include "CyclicModuleRecord.h"
#include "ImportOrderWalk.h"
#include "JSCInlines.h"
#include "JSPromise.h"
#include <ranges>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>

namespace JSC {

static CyclicModuleRecord* recordOf(EvaluationAheadOfLink::Module module)
{
    return uncheckedDowncast<CyclicModuleRecord>(module.asCell());
}

auto EvaluationAheadOfLink::moduleOf([[maybe_unused]] VM& vm, CyclicModuleRecord* record) -> Module
{
#if ENABLE(AOT)
    if (auto* compiled = AOT::compiledModule(vm, record))
        return compiled->moduleAheadOfLink();
#endif
    return record;
}

EvaluationAheadOfLink::EvaluationAheadOfLink(JSGlobalObject* globalObject, RefPtr<ScriptFetcher> scriptFetcher, std::optional<JSModuleLoader::EvaluationOfLoad> evaluation, RunsAheadOfLoad runsAheadOfLoad)
    : m_globalObject(globalObject)
    , m_scriptFetcher(WTF::move(scriptFetcher))
    , m_evaluation(evaluation)
    , m_runsAheadOfLoad(runsAheadOfLoad)
{
}

void EvaluationAheadOfLink::beginPass(bool hasLeftModuleForLater)
{
    ASSERT(m_path.isEmpty());
    m_stack.shrink(0);
    m_indicesOnStack.clear();
    m_modulesAhead.clear();
    m_indicesOfImporters.shrink(0);
    m_numberOfModulesEvaluated = 0;
    m_holdUp = std::nullopt;
    m_hasLeftImportForLater = false;
    m_hasLeftModuleForLater = hasLeftModuleForLater;
    m_didStopAtDeferredModule = false;
}

unsigned EvaluationAheadOfLink::enter(Module module, MayBeOnCycle mayBeOnCycle)
{
    unsigned index = ++m_lastIndex;
    bool isOnStack = m_evaluation && mayBeOnCycle == MayBeOnCycle::Yes;
    if (isOnStack) {
        m_stack.append(module);
        m_indicesOnStack.add(JSValue::encode(module), index);
    }
    // What it imports behind the module that is being evaluated comes behind that module.
    if (m_evaluation && waitsForEvaluationOfImport(m_globalObject, module)) [[unlikely]]
        holdUpUntilWalkEnds();
    m_path.append({ module, index, std::exchange(m_ancestorIndex, index), isOnStack });
    return index;
}

void EvaluationAheadOfLink::leave(bool hasPassedItsImports)
{
    Importer left = m_path.takeLast();
    if (left.isOnStack && hasPassedItsImports) {
        if (m_ancestorIndex < left.index)
            holdUpUntilEndOf(m_ancestorIndex);
        else {
            if (m_holdUp && left.index <= m_holdUp->untilEndOf)
                m_holdUp = std::nullopt;
            for (Module member; member != left.module;) {
                member = m_stack.takeLast();
                m_indicesOnStack.remove(JSValue::encode(member));
            }
        }
    }
    m_ancestorIndex = std::min(m_ancestorIndex, left.ancestorIndexOfImporter);
}

void EvaluationAheadOfLink::didComeBackTo(Module module)
{
    if (unsigned index = m_indicesOnStack.get(JSValue::encode(module)))
        m_ancestorIndex = std::min(m_ancestorIndex, index);
}

bool EvaluationAheadOfLink::isOnPath(Module module) const
{
    return m_path.containsIf([&](const Importer& importer) {
        return importer.module == module;
    });
}

bool EvaluationAheadOfLink::cameThroughRecordBelowRoot() const
{
    return m_path.size() > 1 && std::ranges::any_of(m_path.subspan(1), [](const Importer& importer) {
        return importer.module.isCell();
    });
}

bool EvaluationAheadOfLink::isToBeEvaluated(Module module) const
{
    using Status = CyclicModuleRecord::Status;
#if ENABLE(AOT)
    if (!module.isCell())
        return AOT::isToBeEvaluatedAheadOfLink(m_globalObject, module, leavesModulesForLater());
#endif
    CyclicModuleRecord* record = recordOf(module);
#if ENABLE(AOT)
    if (auto* compiled = leavesModulesForLater() ? AOT::compiledModule(m_globalObject->vm(), record) : nullptr; compiled && compiled->keepsToModuleWithNothingLeftToRun())
        return false;
#endif
    if (record->status() <= Status::Unlinked || record->status() == Status::Linked)
        return true;
    return record->status() == Status::Evaluated && (record->evaluationError() || (record->cycleRoot() && record->cycleRoot()->evaluationError()));
}

void EvaluationAheadOfLink::didPassImport(Module module, size_t numberOfModulesAheadOfIt)
{
    if (m_isBelowDeferredImport)
        return;
    if (leavesModulesForLater()) [[unlikely]] {
        m_hasLeftModuleForLater = m_hasLeftModuleForLater || (isToBeEvaluated(module) && !isOnPath(module));
        return;
    }
    if (!m_evaluation || m_holdUp || m_indicesOnStack.contains(JSValue::encode(module)))
        return;
    // A walk that has a list of the modules may only tell of one while it is being evaluated.
    if (!isToBeEvaluated(module) && module != m_moduleBeingEvaluated)
        return;
    // Evaluating it evaluates what it imports.
    while (m_modulesAhead.size() > numberOfModulesAheadOfIt)
        m_modulesAhead.removeLast();
    m_indicesOfImporters.shrink(numberOfModulesAheadOfIt);
    m_numberOfModulesEvaluated = std::min(m_numberOfModulesEvaluated, numberOfModulesAheadOfIt);
    m_modulesAhead.appendWithCrashOnOverflow(module);
    m_indicesOfImporters.append(m_path.last().index);
}

void EvaluationAheadOfLink::holdUpUntilEndOf(unsigned index)
{
    if (m_holdUp)
        m_holdUp->untilEndOf = std::min(m_holdUp->untilEndOf, index);
    else
        m_holdUp = { m_modulesAhead.size(), index };
}

bool EvaluationAheadOfLink::stopsAtDeferredModule()
{
    // A load that has failed does not wait for what was left for later.
    m_didStopAtDeferredModule = m_hasLeftModuleForLater && (runsAheadOfLoad() || !m_hasFailureAhead);
    return m_didStopAtDeferredModule;
}

auto EvaluationAheadOfLink::evaluateModulesAhead(KeepsAnswers keepsAnswers) -> Result
{
    VM& vm = m_globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    size_t end = m_holdUp ? m_holdUp->numberOfModulesAhead : m_modulesAhead.size();
    for (size_t index = m_numberOfModulesEvaluated; index < end; ++index) {
        link(m_modulesAhead.at(index));
        RETURN_IF_EXCEPTION(scope, Result::Threw);
    }

    uint64_t lastChange = vm.importOrderState().lastChange;
    while (m_numberOfModulesEvaluated < end) {
        size_t index = m_numberOfModulesEvaluated++;
        if (!isToBeEvaluated(m_modulesAhead.at(index)))
            continue;
        JSPromise* promise = evaluate(m_modulesAhead.at(index));
        RETURN_IF_EXCEPTION(scope, Result::Threw);
        if (promise && promise->status() == JSPromise::Status::Rejected) [[unlikely]]
            RELEASE_AND_RETURN(scope, failImporter(m_indicesOfImporters[index], promise->result()));
        if (lastChange != vm.importOrderState().lastChange && keepsAnswers == KeepsAnswers::No) [[unlikely]]
            return Result::ChangedAnswers;
    }
    return Result::IsDone;
}

void EvaluationAheadOfLink::link(Module module)
{
    auto scope = DECLARE_THROW_SCOPE(m_globalObject->vm());
    auto noteFailure = makeScopeExit([&] {
        m_didFailToLink = !!scope.exception();
    });
#if ENABLE(AOT)
    if (!module.isCell()) {
        AOT::linkAheadOfImporters(m_globalObject, module);
        return;
    }
#endif
    CyclicModuleRecord* record = recordOf(module);
#if ENABLE(AOT)
    // No load need have visited a record that stands beside its compiled module.
    if (record->status() == CyclicModuleRecord::Status::New && AOT::compiledModule(m_globalObject->vm(), record))
        record->setStatus(CyclicModuleRecord::Status::Unlinked);
#endif
    if (record->status() != CyclicModuleRecord::Status::Unlinked)
        return;
#if ENABLE(AOT)
    // The compiled modules below it take the exports of theirs in that walk.
    if (m_globalObject->vm().hasCompiledCode()) {
        ImportOrderWalk::VisitedRecords visited;
        ImportOrderWalk::runWithoutWaiting(m_globalObject, record, visited, AbstractModuleRecord::IsBehindWalk::Yes);
        RETURN_IF_EXCEPTION(scope, void());
    }
#endif
    record->linkAfterDeferredModules(m_globalObject, m_scriptFetcher);
}

// Returns null if it has run to its end.
JSPromise* EvaluationAheadOfLink::evaluate(Module module)
{
    VM& vm = m_globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    VM::ImportOrderState& state = vm.importOrderState();

    for (const Importer& importer : m_path) {
        if (importer.wasLinkedMeanwhile && importer.module.isCell() && recordOf(importer.module)->status() == CyclicModuleRecord::Status::Linked) [[unlikely]]
            recordOf(importer.module)->setStatus(CyclicModuleRecord::Status::Evaluating);
    }
    JSPromise* promise;
    {
        SetForScope isRunningDeferredModule(state.isRunningDeferredModule, true);
        m_outer = state.innermostEvaluationAheadOfLink;
        SetForScope innermost(state.innermostEvaluationAheadOfLink, this);
        SetForScope moduleBeingEvaluated(m_moduleBeingEvaluated, module);
        ++state.clock;
#if ENABLE(AOT)
        if (!module.isCell())
            promise = AOT::evaluateAheadOfImporters(m_globalObject, module, *m_evaluation);
        else
#endif
            promise = recordOf(module)->AbstractModuleRecord::evaluate(m_globalObject, m_evaluation->referrerAsyncOrder, m_evaluation->dynamicImportPromise);
        ++state.clock;
    }
    takeBackImportersLinkedMeanwhile();
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (promise)
        promise->markAsHandled();
    return promise;
}

// See AbstractModuleRecord::innerModuleLinking().
void EvaluationAheadOfLink::takeBackImportersLinkedMeanwhile()
{
    for (Importer& importer : m_path) {
        if (waitsForEvaluationOfImport(m_globalObject, importer.module)) [[unlikely]]
            continue;
#if ENABLE(AOT)
        if (!importer.module.isCell()) {
            importer.wasLinkedMeanwhile |= AOT::takeBackImporterLinkedMeanwhile(m_globalObject, importer.module);
            continue;
        }
#endif
        if (CyclicModuleRecord* record = recordOf(importer.module); record->status() == CyclicModuleRecord::Status::Evaluating) [[unlikely]] {
            record->setStatus(CyclicModuleRecord::Status::Linked);
            importer.wasLinkedMeanwhile = true;
        }
    }
}

auto EvaluationAheadOfLink::failImporter(unsigned index, JSValue error) -> Result
{
    VM& vm = m_globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (const Importer& importer : m_path | std::views::reverse) {
        if (importer.index > index)
            continue;
        // If the module has had it linked, it fails when it is evaluated.
        if (importer.wasLinkedMeanwhile) {
            m_numberOfModulesEvaluated = m_modulesAhead.size();
            return Result::IsDone;
        }
        m_indexOfImporterThatFailed = importer.index;
        m_didModuleThrow = true;
        if (importer.module.isCell()) {
            scope.release();
            vm.importOrderState().innermostWalk->failToImportModuleThatThrew(recordOf(importer.module), error);
        } else
            throwException(m_globalObject, scope, error);
        return Result::Threw;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool EvaluationAheadOfLink::waitsForEvaluationOfImport(JSGlobalObject* globalObject, Module module)
{
    for (EvaluationAheadOfLink* evaluation = globalObject->vm().importOrderState().innermostEvaluationAheadOfLink; evaluation; evaluation = evaluation->m_outer) {
#if ENABLE(AOT)
        if (!module.isCell()) {
            if (evaluation->m_globalObject != globalObject)
                continue;
            AOT::catchUpWithListsOfImports(globalObject);
        }
#endif
        if (evaluation->isOnPath(module))
            return true;
    }
    return false;
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)

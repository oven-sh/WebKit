/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTInstance.h"
#include "AOTModuleLinkWalk.h"
#include "AOTModuleRecord.h"
#include "AOTObject.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTPendingStars.h"
#include "AOTProgramStatics.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "JSCellButterfly.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "PropertyNameArray.h"
#include "SyntheticModuleRecord.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

using ModulePhase = AbstractModuleRecord::ModulePhase;
using StarExportResolution = AbstractModuleRecord::Resolution;

JSValue Modules::storedValue(JSGlobalObject* realm, JSObject* table, const Identifier& name)
{
    if (std::optional<uint32_t> index = parseIndex(name))
        return table->getDirectIndex(realm, *index);
    return table->getDirect(realm->vm(), name);
}

void Modules::forEachCapturedExport(ModuleID module, const ScopedLambda<void(const Identifier&, JSValue)>& functor)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* captured = capturedExports(module);
    RETURN_IF_EXCEPTION(scope, void());
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(captured, m_instance.globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : names) {
        JSValue value = storedValue(m_instance.globalObject, captured, name);
        functor(name, value == JSValue(captured) ? JSValue() : value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// The search for a name of such a module can leave the image.
static bool findsNamesThroughRecords(const ModuleData& exporter)
{
    return exporter.isUncompiledCode() || (exporter.hasDynamicStarExport() && !exporter.capturesExports());
}

StarExportResolution::Type Modules::resolveRequiredName(ModuleID exporter, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& exporterData = data(exporter);

    if (findsNamesThroughRecords(exporterData)) {
        AbstractModuleRecord* record = recordForResolution(exporter);
        RETURN_IF_EXCEPTION(scope, StarExportResolution::Type::Error);
        RELEASE_AND_RETURN(scope, record->resolveExport(m_instance.globalObject, name).type);
    }

    // A deferred export is not read for the answer.
    RELEASE_ASSERT(exporterData.capturesExports() || exporterData.hasExportTable());
    bool hasExport = exporterData.capturesExports() ? hasCapturedExport(exporter, name) : !!m_table.findExport(exporter, name.impl());
    RETURN_IF_EXCEPTION(scope, StarExportResolution::Type::Error);
    return hasExport ? StarExportResolution::Type::Resolved : StarExportResolution::Type::NotFound;
}

void Modules::checkRequiredNames(std::span<const ModuleID> modules)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    for (ModuleID module : modules) {
        if (status(module) != ModuleStatus::Unlinked)
            continue;
        for (const RequiredNameData& requiredName : m_table.requiredNames(module)) {
            ModuleID exporter = requiredName.exporterModule();
            if (m_mocks) [[unlikely]]
                noteLoadBehindMock(exporter, false);
            if (isFoundInTextOfFile(requiredName)) {
                LoadedModule* loaded = loadedIfExists(exporter);
                if (!loaded || !loaded->exportsWhatMockNamesAlone)
                    continue;
                bool isExported = hasCapturedExport(exporter, identifier(requiredName.name));
                RETURN_IF_EXCEPTION(scope, void());
                if (isExported || dropExportThatFindsNothing(module, requiredName))
                    continue;
                RELEASE_AND_RETURN(scope, throwExportNotFound(requiredName));
            }
            if (data(exporter).hasExportTable()) {
                // A mock that stands for a whole ES module takes away what the build has found. So does a link that has dropped the export.
                if (!m_exportsLeftOut.isEmpty()) [[unlikely]] {
                    std::optional<unsigned> index = m_table.findExport(exporter, identifier(requiredName.name).impl());
                    if (index && isLeftOut(exporter, *index, TablesBehindStars::HaveName)) {
                        if (dropExportThatFindsNothing(module, requiredName))
                            continue;
                        RELEASE_AND_RETURN(scope, throwExportNotFound(requiredName));
                    }
                }
                if (requiredName.mayBeAbsent && !data(exporter).hasDynamicStarExport())
                    continue;
            }
            // A file that has no face and captures nothing is asked for `default` alone, which it has until its error is dropped.
            if (data(exporter).kind == ModuleKind::CommonJs && !data(exporter).capturesExports()) {
                bool isExported = !ensureLoaded(exporter).didDropError;
                if (ensureLoaded(exporter).exportsWhatMockNamesAlone) {
                    isExported = hasCapturedExport(exporter, identifier(requiredName.name));
                    RETURN_IF_EXCEPTION(scope, void());
                }
                if (isExported || dropExportThatFindsNothing(module, requiredName))
                    continue;
                RELEASE_AND_RETURN(scope, throwExportNotFound(requiredName));
            }
            StarExportResolution::Type resolution = resolveRequiredName(exporter, identifier(requiredName.name));
            RETURN_IF_EXCEPTION(scope, void());
            switch (resolution) {
            case StarExportResolution::Type::Resolved:
                continue;
            case StarExportResolution::Type::Ambiguous:
                RELEASE_AND_RETURN(scope, throwExportAmbiguous(requiredName));
            case StarExportResolution::Type::NotFound:
            case StarExportResolution::Type::Error:
                if (requiredName.mayBeAbsent && data(exporter).exportRule != SyntheticExportRule::RuntimeModule)
                    continue;
                // A CommonJS file has `default` from the start, and loses it with its other names.
                if (isCommonJSFileOrFace(exporter) && identifier(requiredName.name) == m_instance.vm->propertyNames->defaultKeyword && !ensureLoaded(exporter).didDropError && !ensureLoaded(fileOf(exporter)).didDropError && !ensureLoaded(exporter).exportsWhatMockNamesAlone)
                    continue;
                if (resolution == StarExportResolution::Type::NotFound && dropExportThatFindsNothing(module, requiredName))
                    continue;
                RELEASE_AND_RETURN(scope, throwExportNotFound(requiredName));
            }
        }
    }
}

bool Modules::isNativeModule(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    if (moduleData.kind != ModuleKind::Runtime || !host().captureNativeModuleExports)
        return false;
    return !BuiltinTable::singleton().at(RuntimeModuleID(*moduleData.registryID())).flags.contains(RuntimeModuleFlag::HasSource);
}

// Of a module that needs a link. Any other one captures exports or is uncompiled, and that is all a link does with it.
static bool linkFollowsRequestsOf(const ModuleData& data)
{
    return data.capturesExportsBehindEvaluation() || (!data.capturesExports() && !data.isUncompiledCode());
}

// A CommonJS file has a rule of its own only where ES modules bind its names. Whoever else asks for its namespace object is the host, for an entry point.
static SyntheticExportRule captureRuleOf(const ModuleData& data)
{
    if (data.exportRule == SyntheticExportRule::None && data.kind == ModuleKind::CommonJs)
        return SyntheticExportRule::CommonJs;
    return data.exportRule;
}

static void checkSafeToRecurseWhileLinking(JSGlobalObject* realm, ThrowScope& scope)
{
    if (!realm->vm().isSafeToRecurse()) [[unlikely]]
        throwException(realm, scope, createModuleGraphTooDeepError(realm));
}

static bool hasUnvisitedRequests(AbstractModuleRecord* record, const VisitedModuleRecords& visited)
{
    auto* cyclic = dynamicDowncast<CyclicModuleRecord>(record);
    return cyclic && cyclic->status() == CyclicModuleRecord::Status::Unlinked && !visited.contains(record);
}

ALWAYS_INLINE void ModuleLinkWalk::append(ModuleID module, StepKind kind, bool resolvesNames)
{
    if (m_steps.size() == m_steps.capacity()) [[unlikely]]
        m_steps.reserveCapacity(m_steps.capacity() * 2);
    m_steps.unsafeAppendWithoutCapacityCheck(Step { module.index(), static_cast<uint32_t>(kind), resolvesNames, 0 });
}

ModuleLinkWalk::ModuleLinkWalk(const Modules& modules, ModuleID root)
    : m_root(root)
    , m_linkEpoch(modules.linkEpoch())
{
    struct Frame {
        uint32_t enter;
        uint32_t nextRequest;
        uint32_t endOfRequests;
    };

    const ModuleTable& table = modules.table();
    if (table.data(root).hasLinkPlan()) {
        if (modules.status(root) == ModuleStatus::Unlinked)
            m_plan = table.entryPointLinkPlan();
        return;
    }
    // With the cycle linked the walk is short, and most steps of the plan are done.
    if (auto plan = table.linkPlanOfRoot(root); plan && modules.status(root) == ModuleStatus::Unlinked && modules.status(plan->memberOfCycle) == ModuleStatus::Unlinked) {
        m_plan = plan->steps;
        m_memberOfCycle = plan->memberOfCycle;
        return;
    }

    Vector<Frame, 64> path;
    auto record = [&](ModuleID module) ALWAYS_INLINE_LAMBDA {
        const ModuleData& moduleData = table.data(module);
        if (linkFollowsRequestsOf(moduleData) && modules.status(module) == ModuleStatus::Unlinked) {
            path.append(Frame { static_cast<uint32_t>(m_steps.size()), moduleData.firstLinkRequest, table.endOfLinkRequests(module) });
            append(module, StepKind::Enter);
        }
        // Also of a linked file: a module of the graph that is being evaluated may import it back.
        if (moduleData.capturesExports())
            append(module, StepKind::CapturesExports);
        else if (moduleData.isUncompiledCode())
            append(module, StepKind::IsUncompiled);
    };

    if (modules.isSettledForLinks(root))
        return;
    record(root);
    if (path.isEmpty())
        return;

    ModuleNumbers requests = table.allLinkRequests();
    // What was found to be settled when the walk began, and what the walk has come to: several modules of a graph request the same module.
    Vector<uint64_t> passedOver;
    std::span<const uint64_t> modulesPassedOver = modules.modulesSettledForLinks();
    auto passOverFromNowOn = [&](ModuleID module) ALWAYS_INLINE_LAMBDA {
        passedOver[module.index() / 64] |= 1ull << (module.index() % 64);
    };
    while (!path.isEmpty()) {
        Frame& frame = path.last();
        uint32_t next = frame.nextRequest;
        uint32_t end = frame.endOfRequests;
        size_t depth = path.size();
        while (true) {
            // Most of what a root of a large program reaches was linked with an earlier graph.
            next = static_cast<uint32_t>(requests.findFirstOutside(modulesPassedOver, next, end));
            if (next == end)
                break;
            ModuleID target(requests[next++]);
            if (passedOver.isEmpty()) {
                passedOver = modulesPassedOver;
                modulesPassedOver = passedOver.span();
                passOverFromNowOn(root);
                if (target == root)
                    continue;
            }
            passOverFromNowOn(target);
            if (modules.isSettledForLinks(target))
                continue;
            // record() may move the frames.
            path[depth - 1].nextRequest = next;
            record(target);
            if (path.size() != depth)
                break;
        }
        if (path.size() != depth)
            continue;

        Step& enter = m_steps[path.last().enter];
        ModuleID module(enter.module);
        enter.leave = static_cast<uint32_t>(m_steps.size());
        append(module, StepKind::Leave, !table.data(module).capturesExports() && (!table.requiredNames(module).empty() || !table.syntheticExports(module).empty()));
        path.removeLast();
    }
}

// What has no work left by now has a step that does nothing. A replay goes past a module that another link has linked since, or that has run since, and past all that the walk found below it: a walk made now
// would find none of that if all of it is settled too.
bool ModuleLinkWalk::bringUpToDate(const Modules& modules)
{
    if (m_linkEpoch != modules.linkEpoch())
        return false;
    if (hasPlan())
        return modules.status(m_root) == ModuleStatus::Unlinked && (!m_memberOfCycle || modules.status(*m_memberOfCycle) == ModuleStatus::Unlinked);
    std::span<Step> steps = m_steps.mutableSpan();
    for (size_t index = 0; index < steps.size(); ++index) {
        const Step& enter = steps[index];
        if (static_cast<StepKind>(enter.kind) != StepKind::Enter || modules.status(ModuleID(enter.module)) == ModuleStatus::Unlinked)
            continue;
        if (!index)
            return false;
        size_t leave = enter.leave;
        bool isFollowedByOwnCapture = static_cast<StepKind>(steps[index + 1].kind) == StepKind::CapturesExports && steps[index + 1].module == enter.module;
        size_t firstBelow = index + 1 + isFollowedByOwnCapture;
        for (const Step& below : steps.subspan(firstBelow, leave - firstBelow)) {
            if (static_cast<StepKind>(below.kind) != StepKind::Leave && !modules.isSettledForLinks(ModuleID(below.module)))
                return false;
        }
        if (isFollowedByOwnCapture)
            std::swap(steps[index], steps[index + 1]);
        index = leave;
    }
    return true;
}

// A module that follows requests is never unlinked again. One that captures exports has work until it is evaluated and its importers read what was captured last.
bool Modules::checkIsSettledForLinks(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    bool isSettled = !moduleData.needsLink();
    if (!isSettled && moduleData.capturesExports()) {
        LoadedModule* loaded = status(module) == ModuleStatus::Evaluated ? loadedIfExists(module) : nullptr;
        isSettled = loaded && (loaded->capturedExports || loaded->capturedFields) && !loaded->bindsEarlierCapture && !loaded->isBeingLoadedByLoader;
    } else if (!isSettled)
        isSettled = !moduleData.isUncompiledCode() && status(module) != ModuleStatus::Unlinked;
    if (isSettled)
        m_modulesSettledForLinks[module.index() / 64] |= 1ull << (module.index() % 64);
    return isSettled;
}

static void reach(const ModuleTable& table, const BitVector& mocked, Vector<ModuleID, 32>&& work, BitVector& reached)
{
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()) || mocked.quickGet(module.index()))
            continue;
        for (const ModuleRequestData& request : table.requests(module)) {
            // The list of a root holds the files of its whole graph, and says nothing of who imports them.
            if (!request.isHoisted)
                work.append(request.targetModule());
        }
    }
}

void Modules::willMock(ModuleID mocked)
{
    if (isMocked(mocked))
        return;
    BitVector modules = m_mocks ? m_mocks->modules : BitVector(m_table.size());
    modules.quickSet(mocked.index());

    // The files of a test run mock the same modules in the same order, as far as their preloads do it.
    Vector<Box<MockedModules>>& kept = m_instance.vm->aotState().parts[m_instance.program->position()].mockedModules;
    unsigned position = m_numberOfMockedSets++;
    if (position < kept.size() && kept[position]->modules == modules) {
        m_mocks = kept[position];
        return;
    }
    kept.shrink(std::min<size_t>(position, kept.size()));

    Box<MockedModules> mocks = Box<MockedModules>::create();
    mocks->modules = WTF::move(modules);
    BitVector below(m_table.size());
    Vector<ModuleID, 32> work;
    for (size_t module : mocks->modules) {
        for (const ModuleRequestData& request : m_table.requests(ModuleID(module))) {
            if (!request.isHoisted)
                work.append(request.targetModule());
        }
    }
    // Past other mocked modules too.
    reach(m_table, BitVector(m_table.size()), WTF::move(work), below);
    below.merge(mocks->modules);
    mocks->positionBelow.fill(MockedModules::notBelow, m_table.size());
    for (size_t module : below)
        mocks->positionBelow[module] = mocks->numberBelow++;

    if (m_mocks)
        mocks->namedByListsAlone = m_mocks->namedByListsAlone;
    else {
        BitVector isAskedFor(m_table.size());
        for (const ModuleRequestData& request : m_table.allRequests()) {
            if (!request.isHoisted)
                isAskedFor.quickSet(request.targetModule().index());
        }
        mocks->namedByListsAlone = BitVector(m_table.size());
        for (uint32_t module = 0; module < m_table.size(); ++module) {
            if (!isAskedFor.quickGet(module))
                mocks->namedByListsAlone.quickSet(module);
        }
    }
    if (position == kept.size())
        kept.append(mocks);
    m_mocks = WTF::move(mocks);
}

const BitVector& Modules::MocksBelowRoot::reachedFromRoot() const
{
    if (!m_reached) {
        m_reached = m_mocks->reachedFromRoots.ensure(m_root, [&] {
            BitVector reached(m_table.size());
            reach(m_table, m_mocks->modules, { m_root }, reached);
            Box<BitVector> reachedBelow = Box<BitVector>::create(m_mocks->numberBelow);
            for (size_t module : reached) {
                if (uint32_t position = m_mocks->positionBelow[module]; position != MockedModules::notBelow)
                    reachedBelow->quickSet(position);
            }
            return reachedBelow;
        }).iterator->value;
    }
    return *m_reached;
}

// JavaScriptCore's loader creates a module of the run time when it fetches it, in an earlier job than the one that links and evaluates the importing graph.
bool Modules::loadRuntimeModules(ModuleID root)
{
    return loadRuntimeModules(ModuleLinkWalk(*this, root));
}

bool Modules::loadRuntimeModules(const ModuleLinkWalk& walk, NativeModuleLoading nativeModuleLoading)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    bool didRunModule = false;
    MocksBelowRoot mocks = mocksBelow(walk.root());
    walk.replay(scope, [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    }, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (leaf != ModuleLinkWalk::Leaf::CapturesExports || data(module).exportRule != SyntheticExportRule::RuntimeModule)
            return;
        if (mocks.hides(module)) [[unlikely]]
            return;
        if (isNativeModule(module)) {
            if (nativeModuleLoading == NativeModuleLoading::Eager && !didCaptureExports(module)) {
                didRunModule = true;
                captureExports(module);
            }
            return;
        }
        if (exportsWhatMockNamesAlone(module))
            return;
        LoadedModule* loaded = loadedIfExists(module);
        if (status(module) != ModuleStatus::Evaluated || (loaded && loaded->errorKeptByLoader)) {
            didRunModule = true;
            executeForLoader(module);
        }
    }, [](ModuleID, bool) { });
    return didRunModule;
}

void Modules::link(ModuleID root)
{
    if (!data(root).needsLink() || (linkFollowsRequestsOf(data(root)) && status(root) != ModuleStatus::Unlinked))
        return;
    link(ModuleLinkWalk(*this, root));
}

void Modules::link(const ModuleLinkWalk& walk)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    runMocksBelow(walk.root());
    RETURN_IF_EXCEPTION(scope, void());

    VisitedModuleRecords visited;
    captureSyntheticExports(walk, visited);
    RETURN_IF_EXCEPTION(scope, void());

    ModuleLinkingStack stack;
    LinkContext context { stack };
    finishLink(walk, context);
    if (scope.exception()) [[unlikely]] {
        for (CyclicModuleRecord* record : stack)
            record->setStatus(CyclicModuleRecord::Status::Unlinked);
        return;
    }
    ASSERT(stack.isEmpty());
}

void Modules::captureSyntheticExports(ModuleID root, VisitedModuleRecords& visited)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::unique_ptr<ModuleLinkWalk> walk = takeLinkWalk(root);
    captureSyntheticExports(*walk, visited);
    RETURN_IF_EXCEPTION(scope, void());
    keepLinkWalk(WTF::move(walk));
}

void Modules::captureSyntheticExports(const ModuleLinkWalk& walk, VisitedModuleRecords& visited)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    // A link runs no code of the program: a file runs where the evaluation comes to it. Only a module that finds its names in what another module turns out to export cannot wait for that.
    bool resolvesNames = false;
    walk.replay(scope, [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    }, [](ModuleLinkWalk::Leaf, ModuleID) { }, [&](ModuleID module, bool moduleResolvesNames) {
        resolvesNames |= moduleResolvesNames && !findsItsNamesInLibraryAlone(module);
    });

    // What only a mocked module imports never runs.
    MocksBelowRoot mocks = mocksBelow(walk.root());
    // Only an uncompiled module asks which modules are on the path.
    bool needsPath = data(walk.root()).reachesUncompiledCode();
    Vector<ModuleID, 16> path;
    size_t visitedPathLength = 0;

    walk.replay(scope, [&](ModuleID module) {
        if (status(module) != ModuleStatus::Unlinked)
            return false;
        ModuleRecord* record = recordIfExists(module);
        if (record && !visited.add(record).isNewEntry)
            return false;
        if (needsPath) [[unlikely]]
            path.append(module);
        return true;
    }, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (mocks.hides(module)) [[unlikely]]
            return;
        if (leaf == ModuleLinkWalk::Leaf::CapturesExports) {
            if (module != walk.root()) {
                bindLatestCapture(module);
                RETURN_IF_EXCEPTION(scope, void());
            }
            if (data(module).capturesExportsBehindEvaluation() && !resolvesNames)
                captureExportsBehindEvaluation(module);
            else {
                captureExports(module);
                if (LoadedModule* loaded = loadedIfExists(module))
                    loaded->isBeingLoadedByLoader = false;
            }
            return;
        }
        ASSERT(needsPath);
        AbstractModuleRecord* record = uncompiledRecord(module);
        RETURN_IF_EXCEPTION(scope, void());
        checkSafeToRecurseWhileLinking(realm, scope);
        RETURN_IF_EXCEPTION(scope, void());
        // An uncompiled module that imports back must not walk a module on the path again: its own next request comes before theirs.
        if (hasUnvisitedRequests(record, visited)) {
            for (; visitedPathLength < path.size(); ++visitedPathLength)
                visited.add(ensureRecord(path[visitedPathLength]));
        }
        record->generateDeferredSyntheticModules(realm, visited);
    }, [&](ModuleID, bool) {
        if (!needsPath) [[likely]]
            return;
        path.removeLast();
        visitedPathLength = std::min(visitedPathLength, path.size());
    });
}

void Modules::finishLink(ModuleID root, LinkContext& context)
{
    finishLink(*takeLinkWalk(root), context);
}

void Modules::finishLink(const ModuleLinkWalk& walk, LinkContext& context)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    auto isUnlinked = [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    };

    Vector<ModuleID, 16> modulesThatResolveNames;
    MocksBelowRoot mocks = mocksBelow(walk.root());
    bool didEnterRoot = walk.replay(scope, isUnlinked, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (mocks.hides(module)) [[unlikely]]
            return;
        if (leaf == ModuleLinkWalk::Leaf::CapturesExports) {
            // Every CommonJS file of a graph runs before the graph fails for a name that one of them lacks: a module of the graph checks the names behind them.
            if (!data(module).capturesExportsBehindEvaluation())
                captureExports(module);
            return;
        }
        AbstractModuleRecord* record = uncompiledRecord(module);
        RETURN_IF_EXCEPTION(scope, void());
        checkSafeToRecurseWhileLinking(realm, scope);
        RETURN_IF_EXCEPTION(scope, void());
        unsigned index = record->innerModuleLinking(realm, context.stack, context.index, context.scriptFetcher);
        RETURN_IF_EXCEPTION(scope, void());
        context.index = index;
        JSObject* namespaceObject = uncompiledNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, void());
        setValue(module, namespaceObject);
    }, [&](ModuleID module, bool resolvesNames) {
        // The body of a mocked module reads no name of what it imports.
        if (resolvesNames && !isMocked(module) && !mocks.hides(module))
            modulesThatResolveNames.append(module);
    });
    RETURN_IF_EXCEPTION(scope, void());
    if (!didEnterRoot)
        return;

    checkRequiredNames(modulesThatResolveNames.span());
    RETURN_IF_EXCEPTION(scope, void());

    // The link of an uncompiled module can come back to a module of this walk. That nested walk has linked what it reached.
    for (ModuleID module : modulesThatResolveNames) {
        if (!isUnlinked(module))
            continue;
        fillDeferredExports(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (data(walk.root()).hasLinkPlan()) {
        didLinkEntryPoint();
        return;
    }
    // No status says of the modules between the root and the steps that they are linked, as none says so of a module without work in a link.
    if (walk.hasPlan()) {
        setStatus(walk.root(), ModuleStatus::Linked);
        if (isUnlinked(*walk.memberOfCycle()))
            setStatus(*walk.memberOfCycle(), ModuleStatus::Linked);
        for (ModuleID module : modulesThatResolveNames) {
            if (isUnlinked(module))
                setStatus(module, ModuleStatus::Linked);
        }
        return;
    }
    walk.replay(scope, isUnlinked, [](ModuleLinkWalk::Leaf, ModuleID) { }, [&](ModuleID module, bool) {
        setStatus(module, ModuleStatus::Linked);
    });
}

bool Modules::didCaptureExports(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && (loaded->capturedExports || loaded->capturedFields);
}

// An ES importer gets copies of what the file exports when an ES graph first links it, and the file runs where that graph has it: in front of the ES modules, in the order of the imports.
void Modules::captureExportsBehindEvaluation(ModuleID module)
{
    if (didCaptureExports(module))
        return;
    switch (status(module)) {
    case ModuleStatus::Unlinked:
    case ModuleStatus::Linking:
    case ModuleStatus::Linked:
        ensureRecord(module);
        return;
    case ModuleStatus::Evaluating:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Evaluated:
        captureExports(module);
        return;
    case ModuleStatus::Failed:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The loader has the names of a module of the library when it fetches the module, ahead of the files that the graph runs while it loads.
// A native module is generated where the graph is linked.
void Modules::captureExportsOfLibraryModules(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::unique_ptr<ModuleLinkWalk> walk = takeLinkWalk(root);
    walk->replay(scope, [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    }, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (leaf == ModuleLinkWalk::Leaf::CapturesExports && data(module).exportRule == SyntheticExportRule::RuntimeModule && !isNativeModule(module))
            captureExports(module);
    }, [](ModuleID, bool) { });
    RETURN_IF_EXCEPTION(scope, void());
    keepLinkWalk(WTF::move(walk));
}

// Bun's module loader keeps what the load of a module threw, and every later import gets it. require() does not go through it.
JSValue Modules::executeForLoader(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue error = ensureLoaded(module).errorKeptByLoader) {
        throwException(m_instance.globalObject, scope, error);
        return { };
    }
    JSValue value = execute(module);
    if (Exception* exception = scope.exception(); exception && !vm.isTerminationException(exception)) [[unlikely]]
        ensureLoaded(module).errorKeptByLoader.set(*this, exception->value());
    return value;
}

// What the names are found in.
JSValue Modules::evaluateForCapture(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!data(module).hasExportTable())
        RELEASE_AND_RETURN(scope, data(module).kind == ModuleKind::RuntimeSource ? executeForLoader(module) : execute(module));

    std::optional<ModuleID> file = m_table.fileBehindFace(module);
    RELEASE_ASSERT(file);
    // A require() started the file, which is still running, and its face has not begun: there is nothing to evaluate.
    bool isFileRunningWithoutFace = status(*file) == ModuleStatus::Evaluating && (status(module) == ModuleStatus::Unlinked || status(module) == ModuleStatus::Linked);
    if (!isFileRunningWithoutFace) {
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, exportsSeenByLoader(*file));
}

// Bun's module loader runs the generator once per realm and caches what it throws. require() does not go through it.
JSObject* Modules::generateNativeModuleExports(ModuleID module, WTF::Vector<Identifier, 4>& names, MarkedArgumentBuffer& values)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (status(module) == ModuleStatus::Failed) {
        scope.release();
        throwEvaluationError(module);
        return nullptr;
    }
    JSObject* deferredExportsSource = host().captureNativeModuleExports(m_instance.globalObject, *data(module).registryID(), names, values);
    if (Exception* exception = scope.exception()) [[unlikely]] {
        if (!vm.isTerminationException(exception) && !didCaptureExports(module))
            setFailed(module, exception->value());
        return nullptr;
    }
    if (status(module) == ModuleStatus::Unlinked || status(module) == ModuleStatus::Linked)
        setStatus(module, ModuleStatus::Evaluated);
    size_t defaultExport = names.find(vm.propertyNames->defaultKeyword);
    if (defaultExport != notFound && values.at(defaultExport).isObject())
        m_state.builtinCells->didLoadNativeModule(RuntimeModuleID(*data(module).registryID()), asObject(values.at(defaultExport)));
    return deferredExportsSource;
}

void Modules::captureExports(ModuleID module)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);

    if (loaded.capturedExports || loaded.capturedFields)
        return;

    WTF::Vector<Identifier, 4> names;
    MarkedArgumentBuffer values;
    JSObject* deferredExportsSource = nullptr;
    if (isNativeModule(module)) {
        deferredExportsSource = generateNativeModuleExports(module, names, values);
        RETURN_IF_EXCEPTION(scope, void());
    } else {
        JSValue value = evaluateForCapture(module);
        RETURN_IF_EXCEPTION(scope, void());
        // The body can require() an ES module that imports this module back. That link captured the exports as they were then.
        if (loaded.capturedExports || loaded.capturedFields)
            return;
        bool hasCapturedFields = tryCaptureFields(module, value);
        RETURN_IF_EXCEPTION(scope, void());
        if (hasCapturedFields)
            RELEASE_AND_RETURN(scope, fillCapturedExports(module));

        KeyListingThatShowsNoOrder listing(value.getObject());
        deferredExportsSource = host().captureSyntheticExports(realm, captureRuleOf(data(module)), value, names, values);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (values.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(realm, scope);
        return;
    }
    RELEASE_ASSERT(names.size() == values.size());
    if (loaded.capturedExports || loaded.capturedFields)
        return;

    // No program sees the object, so no export is equal to it: it stands for the value of a deferred export.
    JSObject* captured = createTableOfExports();
    for (unsigned i = 0; i < names.size(); ++i) {
        JSValue exported = values.at(i);
        RELEASE_ASSERT(exported || deferredExportsSource);
        captured->putDirectMayBeIndex(realm, names[i], exported ? exported : JSValue(captured));
        RETURN_IF_EXCEPTION(scope, void());
    }
    loaded.capturedExports.set(*this, captured);
    loaded.deferredExportsSource.set(*this, deferredExportsSource);
    RELEASE_AND_RETURN(scope, fillCapturedExports(module));
}

// The body of the face ran the file, which required an ES module that imports it back, and that link captured the exports. The load that started the body copies them
// once more now that the body has returned. `bun run` keeps the first namespace, so only the getters that the copy runs can be observed.
void Modules::copyExportsOfFinishedFile(ModuleID face)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::optional<ModuleID> file = m_table.fileBehindFace(face);
    // Captured fields hold no accessor.
    if (!file || !ensureLoaded(face).capturedExports)
        return;
    // A synchronous load that makes the record of the file replaces the one in the registry, and captureExportsAgain() copies for it.
    if (loader()->isMakingRecordInline(moduleKey(face)) || loader()->isMakingRecordInline(moduleKey(*file)))
        return;
    JSValue value = exportsSeenByLoader(*file);
    RETURN_IF_EXCEPTION(scope, void());
    WTF::Vector<Identifier, 4> names;
    MarkedArgumentBuffer values;
    KeyListingThatShowsNoOrder listing(value.getObject());
    scope.release();
    host().captureSyntheticExports(m_instance.globalObject, captureRuleOf(data(face)), value, names, values);
}

// `bun run` makes a record of a file each time its loader comes to the file, with what the file exports at that moment, and the registry has the last one. The modules that are linked keep the bindings they have.
void Modules::captureExportsAgain(ModuleID fileOrFace)
{
    ModuleID module = m_table.faceOfFile(fileOrFace).value_or(fileOrFace);
    if (!isCommonJSFileOrFace(module) || !didCaptureExports(module))
        return;
    LoadedModule& loaded = ensureLoaded(module);
    // The body threw, so the load that started it made no record of its own. The loader of `bun run` keeps the one of the link that came while the file ran.
    if (loaded.didDropError || loaded.exportsWhatMockNamesAlone)
        return;
    loaded.capturedExports.set(*this, nullptr);
    loaded.deferredExportsSource.set(*this, nullptr);
    loaded.capturedFields.set(*this, nullptr);
    loaded.capturedStars.set(*this, nullptr);
    loaded.sourceOfCapturedFields.set(*this, nullptr);
    loaded.bindsEarlierCapture = true;
    m_modulesSettledForLinks[module.index() / 64] &= ~(1ull << (module.index() % 64));
    ++m_linkEpoch;
    if (namespaceIfCreated(module, ModulePhase::Evaluation)) {
        if (data(module).holdsNamespaceInValue())
            m_modulesWithEarlierNamespaceInValue.set(module.index());
        loaded.namespaceObject.set(*this, nullptr);
    }
    captureExports(module);
}

void Modules::exportWhatMockNamesAlone(ModuleID file, bool hasRun)
{
    ModuleID module = moduleWithExportsOf(file);
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.exportsWhatMockNamesAlone) {
        if (loaded.wasLoadedBehindMock) {
            ensureLoaded(file).answersRequireWithNamespaceOfMock = !loaded.wasRequiredBehindMock;
            return;
        }
        // Compiled code reads the status of a CommonJS file in line, so nobody hears of a require() that it answers itself. Whoever got the namespace object that way
        // reads the values of the later mocks through it, under the names that it has.
        if (JSObject* earlier = namespaceIfCreated(module, ModulePhase::Evaluation)) {
            auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
            if (!loaded.namespacesOfEarlierMocks) {
                JSArray* list = constructEmptyArray(m_instance.globalObject, nullptr);
                RETURN_IF_EXCEPTION(scope, void());
                loaded.namespacesOfEarlierMocks.set(*this, list);
            }
            uncheckedDowncast<JSArray>(loaded.namespacesOfEarlierMocks.get())->push(m_instance.globalObject, earlier);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (data(module).holdsNamespaceInValue() && !m_modulesWithEarlierNamespaceInValue.get(module.index()))
            setValue(module, JSValue());
        loaded.namespaceObject.set(*this, nullptr);
        hasRun = false;
    } else if (didCaptureExports(module) || namespaceIfCreated(module, ModulePhase::Evaluation))
        return;
    loaded.capturedExports.set(*this, createTableOfExports());
    loaded.exportsWhatMockNamesAlone = true;
    ensureLoaded(file).answersRequireWithNamespaceOfMock = !hasRun;
}

void Modules::noteLoadBehindMock(ModuleID fileOrFace, bool isRequired)
{
    LoadedModule* loaded = loadedIfExists(moduleWithExportsOf(fileOf(fileOrFace)));
    if (!loaded || !loaded->exportsWhatMockNamesAlone)
        return;
    loaded->wasLoadedBehindMock = true;
    loaded->wasRequiredBehindMock |= isRequired;
}

bool Modules::exportsWhatMockNamesAlone(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->exportsWhatMockNamesAlone;
}

// The table of the mock stands for a capture, so no capture fills the variables. A member that is read off the namespace object is undefined where the mock lacks it, as where the file does.
void Modules::fillNamesThatMockLacks(ModuleID file)
{
    ModuleID module = moduleWithExportsOf(file);
    if (exportsWhatMockNamesAlone(module))
        fillCapturedExports(module);
}

void Modules::answerRequireWithNamespaceOfMock(ModuleID file)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    LoadedModule* loaded = loadedIfExists(file);
    if (!loaded || !std::exchange(loaded->answersRequireWithNamespaceOfMock, false))
        return;
    ModuleID module = moduleWithExportsOf(file);
    JSValue marker = capturedValue(module, vm.propertyNames->__esModule);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue answer = marker && marker.toBoolean(m_instance.globalObject) ? capturedValue(module, vm.propertyNames->defaultKeyword) : JSValue();
    RETURN_IF_EXCEPTION(scope, void());
    if (!answer) {
        SetForScope isAnswering(m_isAnsweringRequireOfMock, true);
        answer = moduleNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, void());
    }
    setCommonJSExports(file, answer);
}

// All importers of a file read one variable for a name. A module that is linked now finds what the registry has.
void Modules::bindLatestCapture(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->bindsEarlierCapture) [[likely]]
        return;
    loaded->bindsEarlierCapture = false;
    if (m_modulesWithEarlierNamespaceInValue.get(module.index())) {
        JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, void());
        m_modulesWithEarlierNamespaceInValue.clear(module.index());
        setNamespace(module, ModulePhase::Evaluation, namespaceObject);
        loaded->namespaceObject.set(*this, nullptr);
    }
    RELEASE_AND_RETURN(scope, fillCapturedExports(module));
}

// The host lists the own properties of what a CommonJS module exports and reads each of them: Bun's populateESMExports().
bool Modules::tryCaptureFields(ModuleID module, JSValue value)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    SyntheticExportRule rule = captureRuleOf(data(module));
    if (rule == SyntheticExportRule::None)
        return false;
    Object* exports = compiledObjectOrNull(value);
    if (!exports || (exports->hasExtraProperties() && !hasPendingStars(*exports)))
        return false;
    const LayoutData& layout = exports->layout();
    if (layout.kind() != LayoutKind::Record)
        return false;
    // `module.exports = require("dep")`: the object may be of another image, whose table has the names of its fields.
    RuntimeLayoutTable& layouts = exports->state().objects->layouts;
    for (const FieldData& field : layout.fields()) {
        if (field.isAccessor() || (layouts.hasStringKey(field) && parseIndex(*layouts.nameOf(vm, field))))
            return false;
    }

    bool isMarkedAsESModule = false;
    if (rule == SyntheticExportRule::CommonJs) {
        PropertySlot slot(exports, PropertySlot::InternalMethodType::VMInquiry, &vm);
        bool hasMark = exports->getPropertySlot(realm, vm.propertyNames->__esModule, slot);
        RETURN_IF_EXCEPTION(scope, false);
        if (hasMark) {
            JSValue mark = slot.getValue(realm, vm.propertyNames->__esModule);
            TRY_CLEAR_EXCEPTION(scope, false);
            isMarkedAsESModule = mark && !mark.isUndefinedOrNull() && mark.pureToBoolean() == TriState::True;
        }
    }

    JSCellButterfly* stars = nullptr;
    if (exports->hasExtraProperties()) {
        stars = snapshotOfPendingStars(vm, *exports);
        if (!stars)
            return false;
    }
    JSCellButterfly* fields = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, layout.numberOfFields);
    for (const FieldData& field : layout.fields()) {
        if (field.isPrivate())
            continue;
        JSValue held = exports->readInline(field);
        if (!held)
            held = exports->read(field);
        if (held)
            fields->setIndex(vm, layout.indexOf(field), held);
    }
    LoadedModule& loaded = ensureLoaded(module);
    loaded.capturedFields.set(*this, fields);
    loaded.capturedStars.set(*this, stars);
    loaded.sourceOfCapturedFields.set(*this, exports);
    loaded.capturedFieldsAreOfESModule = isMarkedAsESModule;
    loaded.capturedFieldsAreOfRuntimeModule = rule == SyntheticExportRule::RuntimeModule;
    return true;
}

// Nothing where the host would not have listed the name.
JSValue Modules::capturedField(const LoadedModule& loaded, UniquedStringImpl* name) const
{
    VM& vm = *m_instance.vm;
    const CommonIdentifiers& names = *vm.propertyNames;
    bool isMarked = loaded.capturedFieldsAreOfESModule;
    bool isDefault = name == names.defaultKeyword.impl();
    // Of one of its own modules the host takes the own names that can be listed, and the module itself as `default` where it has none: Bun's appendInternalModuleExports().
    bool isOfRuntimeModule = loaded.capturedFieldsAreOfRuntimeModule;
    JSCellButterfly* stars = isOfRuntimeModule ? nullptr : loaded.capturedStars.get();
    Object& source = *uncheckedDowncast<Object>(loaded.sourceOfCapturedFields.get());
    // Without the mark the module itself is the default export, whatever it has under that name.
    if (!isOfRuntimeModule && isDefault && !isMarked)
        return &source;
    if (!isOfRuntimeModule && ((isMarked && name == names.__esModule.impl()) || (stars && name == names.constructor.impl())))
        return { };
    const LayoutData& layout = source.layout();
    const FieldData* field = source.state().objects->layouts.field(vm, layout, name);
    JSValue held = field && !field->isPrivate() ? loaded.capturedFields->get(layout.indexOf(*field)) : JSValue();
    if (held && (isOfRuntimeModule || (stars && isMarked)) && field->flags & fieldIsNotEnumerable)
        held = JSValue();
    if (!held && stars)
        held = valueInSnapshotOfPendingStars(vm, source, stars, name);
    return !held && isDefault ? JSValue(&source) : held;
}

void Modules::makeTableOfCapturedFields(LoadedModule& loaded)
{
    VM& vm = *m_instance.vm;
    Object& source = *uncheckedDowncast<Object>(loaded.sourceOfCapturedFields.get());
    RuntimeLayoutTable& layouts = source.state().objects->layouts;
    const LayoutData& layout = source.layout();
    UniquedStringImpl* defaultName = vm.propertyNames->defaultKeyword.impl();

    JSObject* captured = createTableOfExports();
    // Nobody sees the order of the names in the table.
    for (const FieldData& field : layout.fields()) {
        if (field.isPrivate() || !layouts.hasStringKey(field))
            continue;
        UniquedStringImpl* name = layouts.nameOf(vm, field);
        JSValue held = name == defaultName ? JSValue() : capturedField(loaded, name);
        if (held)
            captured->putDirect(vm, Identifier::fromUid(vm, name), held);
    }
    if (loaded.capturedStars) {
        forEachBindingInSnapshotOfPendingStars(vm, *uncheckedDowncast<Object>(loaded.sourceOfCapturedFields.get()), loaded.capturedStars, [&](UniquedStringImpl* name, JSValue value) {
            if (name != vm.propertyNames->constructor.impl())
                captured->putDirect(vm, Identifier::fromUid(vm, name), value);
        });
    }
    captured->putDirect(vm, vm.propertyNames->defaultKeyword, capturedField(loaded, defaultName));
    loaded.capturedExports.set(*this, captured);
}

// A name that the module turns out to lack fails the link of each module that binds it, and of no other: it stands among the names that such a module requires, or the module checks it behind the file.
void Modules::fillCapturedExports(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.bindsEarlierCapture) [[unlikely]]
        return;

    for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(module)) {
        // Only a variable binds the name. Without one, a deferred export stays unread.
        if (!syntheticExport.hasVariable)
            continue;
        const Identifier& name = identifier(syntheticExport.name);
        if (syntheticExport.isBoundByRequiredName && isDeferredExport(loaded, name))
            continue;
        JSValue found = capturedValue(module, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (found || syntheticExport.absentIsUndefined)
            ProgramStatics::writeVariable(m_instance, syntheticExport.variable, found ? found : jsUndefined());
    }
}

bool Modules::isDeferredExport(const LoadedModule& loaded, const Identifier& name) const
{
    JSObject* captured = loaded.capturedExports;
    return captured && storedValue(m_instance.globalObject, captured, name) == JSValue(captured);
}

// A module of the library has its names once it is loaded: no file of the program has to run for them. Nor for a name that the text of a file stores, until a mock stands for the file:
// then the link fails behind the other files of the graph, as every link that misses a name of a file.
bool Modules::findsItsNamesInLibraryAlone(ModuleID module) const
{
    if (!m_table.syntheticExports(module).empty())
        return false;
    return std::ranges::all_of(m_table.requiredNames(module), [&](const RequiredNameData& requiredName) {
        if (isFoundInTextOfFile(requiredName)) {
            LoadedModule* exporter = loadedIfExists(requiredName.exporterModule());
            return !exporter || !exporter->exportsWhatMockNamesAlone;
        }
        const ModuleData& exporter = data(requiredName.exporterModule());
        // The build has found the name in the table of an ES module.
        if (requiredName.mayBeAbsent && exporter.hasExportTable() && !exporter.hasDynamicStarExport())
            return true;
        return exporter.exportRule == SyntheticExportRule::RuntimeModule;
    });
}

// Compiled code reads a name that is found through records from a namespace object, which fills a deferred export when it is first asked for the name.
static void fillDeferredExportFoundThroughRecords(JSGlobalObject* realm, AbstractModuleRecord* exporter, const Identifier& name)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    AbstractModuleRecord::Resolution resolution = exporter->resolveExport(realm, name);
    RETURN_IF_EXCEPTION(scope, void());
    if (resolution.type != AbstractModuleRecord::Resolution::Type::Resolved || resolution.localName == vm.propertyNames->starNamespacePrivateName)
        return;
    auto* compiled = dynamicDowncast<ModuleRecord>(resolution.moduleRecord);
    if (!compiled)
        RELEASE_AND_RETURN(scope, SyntheticModuleRecord::materializeLazyExport(realm, resolution.moduleRecord, resolution.localName));
    // The body of an ES module fills its variables.
    if (resolution.compiledExportIndex)
        return;
    scope.release();
    compiled->readExport(realm, resolution.localName, std::nullopt);
}

// The host reads a deferred export when it links a module that binds the name, and later importers get what it read then: SyntheticModuleRecord::materializeLazyExport().
void Modules::fillDeferredExports(ModuleID importer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    for (const RequiredNameData& requiredName : m_table.requiredNames(importer)) {
        ModuleID exporter = requiredName.exporterModule();
        // The loader may not have come to the exporter: two statements that share a place in it get what the first one made.
        if (JSValue held = value(exporter); held && data(exporter).kind == ModuleKind::External) {
            if (auto* inImageBelow = dynamicDowncast<ModuleNamespace>(held)) {
                bool isOfLibraryModule = inImageBelow->readDeferredExport(m_instance.globalObject, identifier(requiredName.name));
                RETURN_IF_EXCEPTION(scope, void());
                if (isOfLibraryModule)
                    continue;
            }
        }
        if (findsNamesThroughRecords(data(exporter))) {
            // The check of the names passes over some of them, and the loader may not have come to their exporter.
            LoadedModule* loaded = loadedIfExists(exporter);
            if (data(exporter).isUncompiledCode() && (!loaded || !loaded->uncompiledRecord))
                continue;
            AbstractModuleRecord* record = recordForResolution(exporter);
            RETURN_IF_EXCEPTION(scope, void());
            fillDeferredExportFoundThroughRecords(m_instance.globalObject, record, identifier(requiredName.name));
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        if (data(exporter).exportRule != SyntheticExportRule::RuntimeModule)
            continue;
        for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(exporter)) {
            if (syntheticExport.name != requiredName.name)
                continue;
            if (!syntheticExport.hasVariable || ProgramStatics::readVariableWithoutAllocating(m_instance, syntheticExport.variable))
                break;
            JSValue found = capturedValue(exporter, identifier(syntheticExport.name));
            RETURN_IF_EXCEPTION(scope, void());
            ProgramStatics::writeVariable(m_instance, syntheticExport.variable, found ? found : jsUndefined());
            break;
        }
    }
}

auto Modules::collectDynamicExports(ModuleID module) -> Vector<DynamicExport>
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);
    Vector<DynamicExport> result;
    auto sortByName = [](Vector<DynamicExport>& exports) {
        AOT::sort(exports, [](const DynamicExport& a, const DynamicExport& b) {
            return codePointCompare(StringView(*a.name.impl()), StringView(*b.name.impl())) < 0;
        });
    };

    if (moduleData.capturesExports() || moduleData.kind == ModuleKind::CommonJs) {
        forEachCapturedExport(module, [&](const Identifier& name, JSValue value) {
            result.append({ name, value });
        });
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        if (LoadedModule* loaded = loadedIfExists(module); loaded && loaded->exportsAddedByMock) {
            JSObject* added = loaded->exportsAddedByMock;
            PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            JSObject::getOwnPropertyNames(added, realm, names, DontEnumPropertiesMode::Include);
            RETURN_IF_EXCEPTION(scope, { });
            for (const Identifier& name : names)
                result.append({ name, storedValue(realm, added, name) });
        }
        if (!moduleData.hasDynamicStarExport()) {
            sortByName(result);
            return result;
        }
        // The namespace object of a module behind the stars may ask for this one.
        if (!vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(realm, scope);
            return result;
        }

        // JavaScriptCore says which binding a name ends at. Where the value of that binding is read is found here: in the first module behind the stars that lists the name.
        AbstractModuleRecord* record = recordForResolution(module);
        IdentifierSet listedNames;
        auto isExportedThroughStar = [&](const Identifier& name) {
            if (name == vm.propertyNames->defaultKeyword || m_table.findExport(module, name.impl()) || !listedNames.add(name.impl()).isNewEntry)
                return false;
            return record->resolveExport(realm, name).type == StarExportResolution::Type::Resolved;
        };
        auto appendLiveExports = [&](ModuleID source, auto&& names) {
            JSObject* namespaceObject = nullptr;
            for (const Identifier& name : names) {
                bool isExported = isExportedThroughStar(name);
                RETURN_IF_EXCEPTION(scope, void());
                if (!isExported)
                    continue;
                if (!namespaceObject) {
                    namespaceObject = moduleNamespace(source, ModulePhase::Evaluation);
                    RETURN_IF_EXCEPTION(scope, void());
                }
                result.append({ name, JSValue(), namespaceObject });
            }
        };

        BitVector seen(m_table.size());
        seen.quickSet(module.index());
        Vector<ModuleID, 8> pending { module };
        while (!pending.isEmpty()) {
            for (const ModuleRequestData& request : m_table.requests(pending.takeLast())) {
                ModuleID source = request.targetModule();
                if (!request.isDynamicStarExport || seen.quickSet(source.index()))
                    continue;
                const ModuleData& sourceData = data(source);
                if (sourceData.capturesExports()) {
                    // A spy or a mock replaces what the file exports, and the name behind the star is that binding: it is read where the file has it.
                    // Only the names are taken here. A deferred export stays unread until somebody reads the name, as behind a star in plain Bun.
                    Vector<Identifier, 8> names;
                    forEachCapturedExport(source, [&](const Identifier& name, JSValue) {
                        names.append(name);
                    });
                    RETURN_IF_EXCEPTION(scope, { });
                    appendLiveExports(source, names);
                } else if (sourceData.isUncompiledCode()) {
                    JSObject* namespaceObject = uncompiledNamespace(source, ModulePhase::Evaluation);
                    RETURN_IF_EXCEPTION(scope, { });
                    // With DontEnumPropertiesMode::Include the namespace object reads no binding, so a binding in its dead zone does not throw.
                    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
                    namespaceObject->methodTable()->getOwnPropertyNames(namespaceObject, realm, names, DontEnumPropertiesMode::Include);
                    RETURN_IF_EXCEPTION(scope, { });
                    appendLiveExports(source, names);
                } else {
                    // The table of a module lists what `export *` takes from a table that the build saw beside it. A module of the run time has its table from another project, and its names are read through its namespace object.
                    RELEASE_ASSERT(sourceData.hasExportTable());
                    appendLiveExports(source, m_table.exports(source) | std::views::transform([&](const ModuleExportData& entry) {
                        return identifier(entry.name);
                    }));
                    RETURN_IF_EXCEPTION(scope, { });
                    WTF::Vector<Identifier> addedByMock;
                    appendNamesAddedByMock(source, addedByMock);
                    RETURN_IF_EXCEPTION(scope, { });
                    appendLiveExports(source, addedByMock);
                    pending.append(source);
                }
                RETURN_IF_EXCEPTION(scope, { });
            }
        }

        BitVector& ambiguousExports = ensureLoaded(module).ambiguousExports;
        std::span<const ModuleExportData> exports = m_table.exports(module);
        for (unsigned index = 0; index < exports.size(); ++index) {
            if (!exports[index].isFromStarExport)
                continue;
            StarExportResolution resolution = record->resolveExport(realm, identifier(exports[index].name));
            RETURN_IF_EXCEPTION(scope, { });
            if (resolution.type == StarExportResolution::Type::Ambiguous)
                ambiguousExports.set(index);
        }
    }

    sortByName(result);
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

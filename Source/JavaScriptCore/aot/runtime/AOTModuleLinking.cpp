/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

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

StarExportResolution::Type Modules::resolveRequiredName(ModuleID exporter, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const ModuleData& exporterData = data(exporter);

    if (exporterData.isUncompiledCode() || (exporterData.hasDynamicStarExport() && !exporterData.capturesExports())) {
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
            StarExportResolution::Type resolution = resolveRequiredName(exporter, identifier(requiredName.name));
            RETURN_IF_EXCEPTION(scope, void());
            switch (resolution) {
            case StarExportResolution::Type::Resolved:
                continue;
            case StarExportResolution::Type::Ambiguous:
                RELEASE_AND_RETURN(scope, throwExportAmbiguous(exporter, requiredName.name));
            case StarExportResolution::Type::NotFound:
            case StarExportResolution::Type::Error:
                if (requiredName.mayBeAbsent)
                    continue;
                RELEASE_AND_RETURN(scope, throwExportNotFound(exporter, requiredName.name));
            }
        }
    }
}

static bool isNativeModule(const ModuleData& data)
{
    if (data.kind != ModuleKind::Runtime)
        return false;
    return !BuiltinTable::singleton().at(RuntimeModuleID(*data.registryID())).flags.contains(RuntimeModuleFlag::HasSource);
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

// What has no work left by now has a step that does nothing. A replay goes past an ES module that another link has linked since and past all that the walk found below it: a walk made now would find none of it
// if all of that is settled too.
bool ModuleLinkWalk::stillHolds(const Modules& modules) const
{
    if (m_linkEpoch != modules.linkEpoch())
        return false;
    if (hasPlan())
        return modules.status(m_root) == ModuleStatus::Unlinked && (!m_memberOfCycle || modules.status(*m_memberOfCycle) == ModuleStatus::Unlinked);
    std::span<const Step> steps = m_steps.span();
    for (size_t index = 0; index < steps.size(); ++index) {
        const Step& step = steps[index];
        if (static_cast<StepKind>(step.kind) != StepKind::Enter || modules.status(ModuleID(step.module)) == ModuleStatus::Unlinked)
            continue;
        if (!index || modules.data(ModuleID(step.module)).capturesExports())
            return false;
        for (const Step& below : steps.subspan(index + 1, step.leave - index - 1)) {
            if (static_cast<StepKind>(below.kind) != StepKind::Leave && !modules.isSettledForLinks(ModuleID(below.module)))
                return false;
        }
        index = step.leave;
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
        isSettled = loaded && (loaded->capturedExports || loaded->capturedFields) && !loaded->bindsEarlierCapture;
    } else if (!isSettled)
        isSettled = !moduleData.isUncompiledCode() && status(module) != ModuleStatus::Unlinked;
    if (isSettled)
        m_modulesSettledForLinks[module.index() / 64] |= 1ull << (module.index() % 64);
    return isSettled;
}

// JavaScriptCore's loader creates a module of the run time when it fetches it, in an earlier job than the one that links and evaluates the importing graph.
bool Modules::loadRuntimeModules(ModuleID root)
{
    return loadRuntimeModules(ModuleLinkWalk(*this, root));
}

bool Modules::loadRuntimeModules(const ModuleLinkWalk& walk)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    bool didRunModule = false;
    walk.replay(scope, [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    }, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (leaf != ModuleLinkWalk::Leaf::CapturesExports)
            return;
        if (data(module).exportRule == SyntheticExportRule::RuntimeModule && status(module) != ModuleStatus::Evaluated) {
            didRunModule = true;
            execute(module);
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
        if (leaf == ModuleLinkWalk::Leaf::CapturesExports) {
            if (module != walk.root()) {
                bindLatestCapture(module);
                RETURN_IF_EXCEPTION(scope, void());
            }
            if (data(module).capturesExportsBehindEvaluation() && !resolvesNames)
                captureExportsBehindEvaluation(module);
            else
                captureExports(module);
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
    bool didEnterRoot = walk.replay(scope, isUnlinked, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
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
        if (resolvesNames)
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

// What the names are found in.
JSValue Modules::evaluateForCapture(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!data(module).hasExportTable())
        RELEASE_AND_RETURN(scope, execute(module));

    std::optional<ModuleID> file = m_table.fileBehindFace(module);
    RELEASE_ASSERT(file);
    evaluateSynchronously(module, SynchronousEvaluationReason::Require);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, exportsSeenByLoader(*file));
}

void Modules::captureExports(ModuleID module)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);

    if (loaded.capturedExports || loaded.capturedFields)
        return;

    JSValue value = evaluateForCapture(module);
    RETURN_IF_EXCEPTION(scope, void());
    // The body can require() an ES module that imports this module back. That link captured the exports as they were then.
    if (loaded.capturedExports || loaded.capturedFields)
        return;
    bool hasCapturedFields = tryCaptureFields(module, value);
    RETURN_IF_EXCEPTION(scope, void());
    if (hasCapturedFields)
        RELEASE_AND_RETURN(scope, fillCapturedExports(module));

    WTF::Vector<Identifier, 4> names;
    MarkedArgumentBuffer values;
    KeyListingThatShowsNoOrder listing(value.getObject());
    auto* captureNativeModuleExports = host().captureNativeModuleExports;
    JSObject* deferredExportsSource = captureNativeModuleExports && isNativeModule(data(module))
        ? captureNativeModuleExports(realm, *data(module).registryID(), names, values)
        : host().captureSyntheticExports(realm, captureRuleOf(data(module)), value, names, values);
    RETURN_IF_EXCEPTION(scope, void());
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

// `bun run` makes a record of a file each time its loader comes to the file, with what the file exports at that moment, and the registry has the last one. The modules that are linked keep the bindings they have.
void Modules::captureExportsAgain(ModuleID fileOrFace)
{
    ModuleID module = m_table.faceOfFile(fileOrFace).value_or(fileOrFace);
    if (!isCommonJSFileOrFace(module) || !didCaptureExports(module))
        return;
    LoadedModule& loaded = ensureLoaded(module);
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
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
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
    const FieldData* field = m_state.objects->layouts.field(vm, layout, name);
    JSValue held = field && !field->isPrivate() ? loaded.capturedFields->get(layout.indexOf(*field)) : JSValue();
    if (held && (isOfRuntimeModule || (stars && isMarked)) && field->flags & fieldIsNotEnumerable)
        held = JSValue();
    if (!field && stars)
        held = valueInSnapshotOfPendingStars(vm, source, stars, name);
    return !held && isDefault ? JSValue(&source) : held;
}

void Modules::makeTableOfCapturedFields(LoadedModule& loaded)
{
    VM& vm = *m_instance.vm;
    RuntimeLayoutTable& layouts = m_state.objects->layouts;
    const LayoutData& layout = uncheckedDowncast<Object>(loaded.sourceOfCapturedFields.get())->layout();
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

// A module of the library has its names once it is loaded: no file of the program has to run for them.
bool Modules::findsItsNamesInLibraryAlone(ModuleID module) const
{
    if (!m_table.syntheticExports(module).empty())
        return false;
    return std::ranges::all_of(m_table.requiredNames(module), [&](const RequiredNameData& requiredName) {
        return data(requiredName.exporterModule()).exportRule == SyntheticExportRule::RuntimeModule;
    });
}

// The host reads a deferred export when it links a module that binds the name, and later importers get what it read then: SyntheticModuleRecord::materializeLazyExport().
void Modules::fillDeferredExports(ModuleID importer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    for (const RequiredNameData& requiredName : m_table.requiredNames(importer)) {
        ModuleID exporter = requiredName.exporterModule();
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

    if (moduleData.capturesExports() || moduleData.kind == ModuleKind::CommonJs) {
        forEachCapturedExport(module, [&](const Identifier& name, JSValue value) {
            result.append({ name, value });
        });
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        if (!moduleData.hasDynamicStarExport())
            return result;
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
                    forEachCapturedExport(source, [&](const Identifier& name, JSValue value) {
                        bool isExported = isExportedThroughStar(name);
                        RETURN_IF_EXCEPTION(scope, void());
                        if (!isExported)
                            return;
                        if (!value) {
                            value = capturedValue(source, name);
                            RETURN_IF_EXCEPTION(scope, void());
                        }
                        if (value)
                            result.append({ name, value });
                    });
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

    AOT::sort(result, [](const DynamicExport& a, const DynamicExport& b) {
        return codePointCompare(StringView(*a.name.impl()), StringView(*b.name.impl())) < 0;
    });
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

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
#include "AOTModuleStarExports.h"
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

JSValue StarExportResolver::storedValue(JSGlobalObject* realm, JSObject* table, const Identifier& name)
{
    if (std::optional<uint32_t> index = parseIndex(name))
        return table->getDirectIndex(realm, *index);
    return table->getDirect(realm->vm(), name);
}

void StarExportResolver::forEachCapturedExport(Modules& modules, ModuleID module, const ScopedLambda<void(const Identifier&, JSValue)>& functor)
{
    Instance& instance = modules.instance();
    VM& vm = *instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* captured = modules.capturedExports(module);
    RETURN_IF_EXCEPTION(scope, void());
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(captured, instance.globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : names) {
        JSValue value = storedValue(instance.globalObject, captured, name);
        functor(name, value == JSValue(captured) ? JSValue() : value);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// The module that stands for a CommonJS file among ES modules has the names that the file turns out to export, where the build did not find them.
bool StarExportResolver::isSolvedFromTable(ModuleID module) const
{
    const ModuleData& data = m_modules.data(module);
    return data.hasExportTable() && !data.capturesExports();
}

// Each importer of a host value has an External record of its own. The value says which of them are one module.
ModuleID StarExportResolver::representativeOf(ModuleID module)
{
    if (!m_modules.data(module).isHostValue())
        return module;
    JSValue value = m_modules.value(module);
    JSObject* hostValue = value ? value.getObject() : nullptr;
    if (!hostValue)
        return module;
    return m_hostValueModules.add(hostValue, module).iterator->value;
}

// export { x } from "./c.cjs" and export * from "./c.cjs" end at one binding. An export entry names it by the variable that holds the captured value.
StarBinding StarExportResolver::bindingOfCapturedName(ModuleID source, UniquedStringImpl* name)
{
    for (const SyntheticExportData& syntheticExport : m_modules.table().syntheticExports(source)) {
        if (syntheticExport.hasVariable && m_modules.identifier(syntheticExport.name).impl() == name)
            return VariableBinding { syntheticExport.variable };
    }
    return CapturedBinding { representativeOf(source), name };
}

StarBinding StarExportResolver::bindingOf(const ModuleExportData& moduleExport)
{
    Instance& instance = m_modules.instance();
    VM& vm = *instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable:
        return VariableBinding { moduleExport.variable };
    case ModuleExportKind::Namespace: {
        ModuleID target = moduleExport.targetModule();
        if (!m_modules.data(target).isUncompiledCode())
            return NamespaceBinding { representativeOf(target), moduleExport.isDeferred ? ModulePhase::Defer : ModulePhase::Evaluation };
        AbstractModuleRecord* record = m_modules.uncompiledRecord(target);
        RETURN_IF_EXCEPTION(scope, { });
        return UncompiledBinding { record, vm.propertyNames->starNamespacePrivateName.impl() };
    }
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = m_modules.table().indirectExport(moduleExport);
        const Identifier& importName = m_modules.identifier(indirectExport.importName);
        AbstractModuleRecord* record = m_modules.uncompiledRecord(indirectExport.targetModule());
        RETURN_IF_EXCEPTION(scope, { });
        StarExportResolution resolution = record->resolveExport(instance.globalObject, importName);
        RETURN_IF_EXCEPTION(scope, { });
        if (resolution.type == StarExportResolution::Type::Resolved)
            RELEASE_AND_RETURN(scope, bindingOf(resolution));
        return UncompiledBinding { record, importName.impl() };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StarBinding StarExportResolver::bindingOf(const StarExportResolution& resolution)
{
    VM& vm = *m_modules.instance().vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(resolution.type == StarExportResolution::Type::Resolved);

    UniquedStringImpl* localName = resolution.localName.impl();
    auto* compiled = dynamicDowncast<ModuleRecord>(resolution.moduleRecord);
    if (!compiled)
        return UncompiledBinding { resolution.moduleRecord, localName };

    ModuleID module = compiled->module();
    if (resolution.compiledExportIndex)
        RELEASE_AND_RETURN(scope, bindingOf(m_modules.table().exports(module)[*resolution.compiledExportIndex]));
    if (resolution.localName == vm.propertyNames->starNamespacePrivateName)
        return NamespaceBinding { representativeOf(module), ModulePhase::Evaluation };
    if (m_modules.data(module).capturesExports())
        return bindingOfCapturedName(module, localName);
    return UncompiledBinding { compiled, localName };
}

bool StarExportResolver::join(StarCandidates& candidates, UniquedStringImpl* name, const StarCandidate& incoming)
{
    ASSERT(incoming.ownEntry == OwnExportEntry::None);
    auto result = candidates.add(name, incoming);
    if (result.isNewEntry)
        return true;
    StarCandidate& existing = result.iterator->value;
    if (existing.ownEntry == OwnExportEntry::Local || existing.isAmbiguous)
        return false;
    if (incoming.isAmbiguous || !(existing.binding == incoming.binding)) {
        existing.isAmbiguous = true;
        return true;
    }
    if (existing.capturedValue || existing.liveSource)
        return false;
    if (!incoming.capturedValue && !incoming.liveSource)
        return false;
    existing.capturedValue = incoming.capturedValue;
    existing.liveSource = incoming.liveSource;
    return true;
}

// `export *` lists the names and binds none of them, so a deferred export stays unread.
void StarExportResolver::addCapturedCandidates(ModuleID source, StarCandidates& candidates, ExportedNames exportedNames)
{
    forEachCapturedExport(m_modules, source, [&](const Identifier& name, JSValue value) {
        VM& vm = *m_modules.instance().vm;
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (exportedNames == ExportedNames::WithoutDefault && name == vm.propertyNames->defaultKeyword)
            return;
        StarBinding binding = bindingOfCapturedName(source, name.impl());
        auto* capturedBinding = std::get_if<CapturedBinding>(&binding);
        if (!value && !(capturedBinding && capturedBinding->module == source)) {
            value = m_modules.capturedValue(source, name);
            RETURN_IF_EXCEPTION(scope, void());
        }
        join(candidates, name.impl(), { binding, value });
    });
}

// GetExportedNames() of the specification.
static WTF::Vector<Identifier> exportedNamesOf(JSGlobalObject* realm, AbstractModuleRecord* root)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    WTF::Vector<Identifier> names;
    UncheckedKeyHashSet<UniquedStringImpl*> listedNames;
    UncheckedKeyHashSet<AbstractModuleRecord*> exportStarSet;
    WTF::Vector<AbstractModuleRecord*, 8> pending;
    pending.append(root);
    while (!pending.isEmpty()) {
        AbstractModuleRecord* record = pending.takeLast();
        if (!exportStarSet.add(record).isNewEntry)
            continue;
        auto list = [&](const Identifier& name) {
            if (record != root && name == vm.propertyNames->defaultKeyword)
                return;
            if (listedNames.add(name.impl()).isNewEntry)
                names.append(name);
        };
        if (auto* compiled = dynamicDowncast<ModuleRecord>(record)) {
            WTF::Vector<Identifier> compiledNames;
            compiled->collectExportedNames(realm, compiledNames);
            RETURN_IF_EXCEPTION(scope, { });
            for (const Identifier& name : compiledNames)
                list(name);
            continue;
        }
        for (const auto& entry : record->exportEntries())
            list(entry.value.exportName);
        for (const auto& [moduleName, moduleRequestType] : record->starExportEntries()) {
            AbstractModuleRecord* target = record->hostResolveImportedModule(realm, Identifier::fromUid(vm, moduleName.get()), moduleRequestType);
            RETURN_IF_EXCEPTION(scope, { });
            pending.append(target);
        }
    }
    return names;
}

void StarExportResolver::addUncompiledCandidates(ModuleID source, StarCandidates& candidates, ExportedNames exportedNames)
{
    Instance& instance = m_modules.instance();
    VM& vm = *instance.vm;
    JSGlobalObject* realm = instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);

    AbstractModuleRecord* record = m_modules.uncompiledRecord(source);
    RETURN_IF_EXCEPTION(scope, void());
    JSObject* namespaceObject = m_modules.uncompiledNamespace(source, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, void());

    // With DontEnumPropertiesMode::Include the namespace object reads no binding, so a binding in its dead zone does not throw.
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    namespaceObject->methodTable()->getOwnPropertyNames(namespaceObject, realm, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    UncheckedKeyHashSet<UniquedStringImpl*> namespaceKeys;
    for (const Identifier& name : names) {
        namespaceKeys.add(name.impl());
        if (exportedNames == ExportedNames::WithoutDefault && name == vm.propertyNames->defaultKeyword)
            continue;
        StarExportResolution resolution = record->resolveExport(realm, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (resolution.type != StarExportResolution::Type::Resolved)
            continue;
        StarBinding binding = bindingOf(resolution);
        RETURN_IF_EXCEPTION(scope, void());
        join(candidates, name.impl(), { binding, JSValue(), namespaceObject });
    }
    // A name that is ambiguous in one target of `export *` is ambiguous in the exporter, and it is no key of the namespace object.
    WTF::Vector<Identifier> exported = exportedNamesOf(realm, record);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : exported) {
        if (namespaceKeys.contains(name.impl()))
            continue;
        StarExportResolution resolution = record->resolveExport(realm, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (resolution.type == StarExportResolution::Type::Ambiguous)
            join(candidates, name.impl(), { StarBinding { }, JSValue(), nullptr, true });
    }
}

void StarExportResolver::addSourceCandidates(ModuleID source, StarCandidates& candidates, ExportedNames exportedNames)
{
    const ModuleData& data = m_modules.data(source);
    RELEASE_ASSERT(!isSolvedFromTable(source));
    if (data.isUncompiledCode()) {
        addUncompiledCandidates(source, candidates, exportedNames);
        return;
    }
    RELEASE_ASSERT(data.capturesExports());
    addCapturedCandidates(source, candidates, exportedNames);
}

void StarExportResolver::addOwnCandidates(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_modules.instance().vm);
    const ModuleTable& table = m_modules.table();
    StarCandidates& candidates = m_candidates.find(module)->value;

    for (const ModuleExportData& moduleExport : table.exports(module)) {
        OwnExportEntry ownEntry = moduleExport.isFromStarExport ? OwnExportEntry::FromStarExport : OwnExportEntry::Local;
        StarBinding binding = bindingOf(moduleExport);
        RETURN_IF_EXCEPTION(scope, void());
        candidates.add(m_modules.identifier(moduleExport.name).impl(), StarCandidate { binding, JSValue(), nullptr, false, ownEntry });
    }
    for (const ModuleRequestData& request : table.requests(module)) {
        ModuleID target = request.targetModule();
        if (!request.isDynamicStarExport || isSolvedFromTable(target))
            continue;
        addSourceCandidates(target, candidates, ExportedNames::WithoutDefault);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool StarExportResolver::addCandidatesOfStarTargets(ModuleID module)
{
    VM& vm = *m_modules.instance().vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    UniquedStringImpl* defaultKeyword = vm.propertyNames->defaultKeyword.impl();
    StarCandidates& candidates = m_candidates.find(module)->value;
    bool didChange = false;

    for (const ModuleRequestData& request : m_modules.table().requests(module)) {
        ModuleID target = request.targetModule();
        if (!request.isDynamicStarExport || target == module || !isSolvedFromTable(target))
            continue;
        for (auto& entry : m_candidates.find(target)->value) {
            if (entry.key.get() == defaultKeyword)
                continue;
            StarCandidate incoming { entry.value.binding, entry.value.capturedValue, entry.value.liveSource, entry.value.isAmbiguous };
            // The table of a module lists what `export *` takes from a table that the build saw beside it. A module of the run time has its table from another project, and its names are read through its namespace object.
            bool hasValueSource = incoming.isAmbiguous || incoming.capturedValue || incoming.liveSource || std::get_if<CapturedBinding>(&incoming.binding);
            if (!hasValueSource && !candidates.contains(entry.key.get())) {
                incoming.liveSource = m_modules.moduleNamespace(target, ModulePhase::Evaluation);
                RETURN_IF_EXCEPTION(scope, false);
            }
            didChange |= join(candidates, entry.key.get(), incoming);
        }
    }
    return didChange;
}

// Returns true if the isDynamicStarExport requests among the collected modules form a cycle.
bool StarExportResolver::collectUnsolvedModules(ModuleID root, Vector<ModuleID, 8>& postOrder)
{
    bool hasCycle = false;
    ModuleRequestWalk walk(m_modules.table(), root);
    while (true) {
        ModuleRequestWalk::Step step = walk.next();
        switch (step.kind) {
        case ModuleRequestWalk::StepKind::Request: {
            ModuleID target = step.request->targetModule();
            if (!step.request->isDynamicStarExport || !isSolvedFromTable(target) || m_candidates.contains(target))
                break;
            if (walk.hasVisited(target))
                hasCycle |= walk.isOnPath(target);
            else
                walk.enter(target);
            break;
        }
        case ModuleRequestWalk::StepKind::Leave:
            postOrder.append(step.module);
            break;
        case ModuleRequestWalk::StepKind::Done:
            return hasCycle;
        }
    }
}

void StarExportResolver::solve(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_modules.instance().vm);
    Vector<ModuleID, 8> postOrder;
    bool hasCycle = collectUnsolvedModules(root, postOrder);

    for (ModuleID module : postOrder)
        m_candidates.add(module, StarCandidates { });
    for (ModuleID module : postOrder) {
        addOwnCandidates(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    // join() only moves a candidate forward (absent, one binding, ambiguous), so the passes end.
    bool didChange;
    do {
        didChange = false;
        for (ModuleID module : postOrder) {
            didChange |= addCandidatesOfStarTargets(module);
            RETURN_IF_EXCEPTION(scope, void());
        }
    } while (didChange && hasCycle);
}

const StarCandidates* StarExportResolver::candidatesOf(ModuleID module)
{
    Instance& instance = m_modules.instance();
    VM& vm = *instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!m_candidates.contains(module)) {
        // A star export cycle through uncompiled code comes back here in another resolver.
        if (!vm.isSafeToRecurse()) [[unlikely]] {
            throwStackOverflowError(instance.globalObject, scope);
            return nullptr;
        }
        if (isSolvedFromTable(module))
            solve(module);
        else
            addSourceCandidates(module, m_candidates.add(module, StarCandidates { }).iterator->value, ExportedNames::All);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return &m_candidates.find(module)->value;
}

const StarCandidate* StarExportResolver::find(ModuleID exporter, UniquedStringImpl* name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_modules.instance().vm);

    const StarCandidates* candidates = candidatesOf(exporter);
    RETURN_IF_EXCEPTION(scope, nullptr);
    auto candidate = candidates->find(name);
    return candidate == candidates->end() ? nullptr : &candidate->value;
}

JSValue StarExportResolver::capturedValueOf(const StarCandidate& candidate)
{
    VM& vm = *m_modules.instance().vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (candidate.capturedValue)
        return candidate.capturedValue;
    auto* captured = std::get_if<CapturedBinding>(&candidate.binding);
    if (!captured)
        return { };
    RELEASE_AND_RETURN(scope, m_modules.capturedValue(captured->module, Identifier::fromUid(vm, captured->name)));
}

StarExportResolution::Type StarExportResolver::resolve(ModuleID exporter, const Identifier& name)
{
    Instance& instance = m_modules.instance();
    auto scope = DECLARE_THROW_SCOPE(*instance.vm);

    // It exports no stars, and a deferred export is not read for the answer.
    if (m_modules.data(exporter).exportRule == SyntheticExportRule::RuntimeModule) {
        bool hasExport = m_modules.hasCapturedExport(exporter, name);
        RETURN_IF_EXCEPTION(scope, StarExportResolution::Type::Error);
        return hasExport ? StarExportResolution::Type::Resolved : StarExportResolution::Type::NotFound;
    }

    if (m_modules.data(exporter).isUncompiledCode()) {
        AbstractModuleRecord* record = m_modules.uncompiledRecord(exporter);
        RETURN_IF_EXCEPTION(scope, StarExportResolution::Type::Error);
        RELEASE_AND_RETURN(scope, record->resolveExport(instance.globalObject, name).type);
    }

    const StarCandidate* candidate = find(exporter, name.impl());
    RETURN_IF_EXCEPTION(scope, StarExportResolution::Type::Error);
    if (!candidate)
        return StarExportResolution::Type::NotFound;
    return candidate->isAmbiguous ? StarExportResolution::Type::Ambiguous : StarExportResolution::Type::Resolved;
}

void StarExportResolver::checkRequiredNames(std::span<const ModuleID> modules)
{
    auto scope = DECLARE_THROW_SCOPE(*m_modules.instance().vm);

    for (ModuleID module : modules) {
        if (m_modules.status(module) != ModuleStatus::Unlinked)
            continue;
        for (const RequiredNameData& requiredName : m_modules.table().requiredNames(module)) {
            ModuleID exporter = requiredName.exporterModule();
            StarExportResolution::Type resolution = resolve(exporter, m_modules.identifier(requiredName.name));
            RETURN_IF_EXCEPTION(scope, void());
            switch (resolution) {
            case StarExportResolution::Type::Resolved:
                continue;
            case StarExportResolution::Type::Ambiguous:
                RELEASE_AND_RETURN(scope, m_modules.throwExportAmbiguous(exporter, requiredName.name));
            case StarExportResolution::Type::NotFound:
            case StarExportResolution::Type::Error:
                if (requiredName.mayBeAbsent)
                    continue;
                RELEASE_AND_RETURN(scope, m_modules.throwExportNotFound(exporter, requiredName.name));
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
        if (!moduleData.needsLink())
            return;
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

    record(root);
    if (path.isEmpty())
        return;

    ModuleNumbers requests = table.allLinkRequests();
    BitVector seen(table.size());
    seen.quickSet(root.index());
    while (!path.isEmpty()) {
        Frame& frame = path.last();
        uint32_t next = frame.nextRequest;
        uint32_t end = frame.endOfRequests;
        size_t depth = path.size();
        while (next != end && path.size() == depth) {
            ModuleID target(requests[next++]);
            if (seen.quickSet(target.index()))
                continue;
            // record() may move the frames.
            path[depth - 1].nextRequest = next;
            record(target);
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
    captureSyntheticExports(ModuleLinkWalk(*this, root), visited);
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
    finishLink(ModuleLinkWalk(*this, root), context);
}

void Modules::finishLink(const ModuleLinkWalk& walk, LinkContext& context)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    auto isUnlinked = [&](ModuleID module) {
        return status(module) == ModuleStatus::Unlinked;
    };

    std::optional<std::pair<ModuleID, uint32_t>> firstAbsentExport;
    Vector<ModuleID, 16> modulesThatResolveNames;
    bool didEnterRoot = walk.replay(scope, isUnlinked, [&](ModuleLinkWalk::Leaf leaf, ModuleID module) {
        if (leaf == ModuleLinkWalk::Leaf::CapturesExports) {
            // Every CommonJS file of a graph runs before the graph fails for a name that one of them lacks: a module of the graph checks the names behind them.
            if (data(module).capturesExportsBehindEvaluation())
                return;
            captureExports(module);
            RETURN_IF_EXCEPTION(scope, void());
            std::optional<uint32_t> absentName = ensureLoaded(module).firstAbsentExport;
            if (absentName && !firstAbsentExport)
                firstAbsentExport = std::pair { module, *absentName };
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

    if (firstAbsentExport)
        RELEASE_AND_RETURN(scope, throwExportNotFound(firstAbsentExport->first, firstAbsentExport->second));

    StarExportResolver resolver(*this);
    resolver.checkRequiredNames(modulesThatResolveNames.span());
    RETURN_IF_EXCEPTION(scope, void());

    // The link of an uncompiled module can come back to a module of this walk. That nested walk has linked what it reached.
    for (ModuleID module : modulesThatResolveNames) {
        if (!isUnlinked(module))
            continue;
        fillStarExports(module, resolver);
        RETURN_IF_EXCEPTION(scope, void());
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

void Modules::fillCapturedExports(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);

    for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(module)) {
        const Identifier& name = identifier(syntheticExport.name);
        // Only a variable binds the name. Without one, a deferred export stays unread.
        if (syntheticExport.hasVariable) {
            if (syntheticExport.isBoundByRequiredName && isDeferredExport(loaded, name))
                continue;
            JSValue found = capturedValue(module, name);
            RETURN_IF_EXCEPTION(scope, void());
            if (found) {
                ProgramStatics::writeVariable(m_instance, syntheticExport.variable, found);
                continue;
            }
        } else {
            bool hasExport = hasCapturedExport(module, name);
            RETURN_IF_EXCEPTION(scope, void());
            if (hasExport)
                continue;
        }
        if (syntheticExport.absentIsUndefined) {
            if (syntheticExport.hasVariable)
                ProgramStatics::writeVariable(m_instance, syntheticExport.variable, jsUndefined());
            continue;
        }
        if (!loaded.firstAbsentExport)
            loaded.firstAbsentExport = static_cast<uint32_t>(syntheticExport.name);
    }
}

bool Modules::isDeferredExport(const LoadedModule& loaded, const Identifier& name) const
{
    JSObject* captured = loaded.capturedExports;
    return captured && StarExportResolver::storedValue(m_instance.globalObject, captured, name) == JSValue(captured);
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

void Modules::fillStarExports(ModuleID module, StarExportResolver& resolver)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!data(module).hasExportTable())
        return;

    for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(module)) {
        if (!syntheticExport.hasVariable)
            continue;
        const StarCandidate* candidate = resolver.find(module, identifier(syntheticExport.name).impl());
        RETURN_IF_EXCEPTION(scope, void());
        if (candidate && !candidate->isAmbiguous) {
            JSValue value = resolver.capturedValueOf(*candidate);
            RETURN_IF_EXCEPTION(scope, void());
            RELEASE_ASSERT(value);
            ProgramStatics::writeVariable(m_instance, syntheticExport.variable, value);
            continue;
        }
        if (syntheticExport.absentIsUndefined) {
            ProgramStatics::writeVariable(m_instance, syntheticExport.variable, jsUndefined());
            continue;
        }
        if (candidate)
            RELEASE_AND_RETURN(scope, throwExportAmbiguous(module, syntheticExport.name));
        RELEASE_AND_RETURN(scope, throwExportNotFound(module, syntheticExport.name));
    }
}

auto Modules::collectDynamicExports(ModuleID module) -> Vector<DynamicExport>
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);
    Vector<DynamicExport> result;

    if (moduleData.capturesExports() || moduleData.kind == ModuleKind::CommonJs) {
        StarExportResolver::forEachCapturedExport(*this, module, [&](const Identifier& name, JSValue value) {
            result.append({ name, value });
        });
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        if (!moduleData.hasDynamicStarExport())
            return result;

        StarExportResolver resolver(*this);
        const StarCandidates* candidates = resolver.candidatesOf(module);
        RETURN_IF_EXCEPTION(scope, { });
        for (auto& entry : *candidates) {
            if (entry.value.ownEntry != OwnExportEntry::None || entry.value.isAmbiguous)
                continue;
            if (entry.value.liveSource) {
                result.append({ Identifier::fromUid(vm, entry.key.get()), JSValue(), entry.value.liveSource });
                continue;
            }
            JSValue value = resolver.capturedValueOf(entry.value);
            RETURN_IF_EXCEPTION(scope, { });
            if (value)
                result.append({ Identifier::fromUid(vm, entry.key.get()), value });
        }

        BitVector& ambiguousExports = ensureLoaded(module).ambiguousExports;
        std::span<const ModuleExportData> exports = m_table.exports(module);
        for (unsigned index = 0; index < exports.size(); ++index) {
            if (exports[index].isFromStarExport && candidates->find(identifier(exports[index].name).impl())->value.isAmbiguous)
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

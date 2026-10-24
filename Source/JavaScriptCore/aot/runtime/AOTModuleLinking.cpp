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
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

namespace {

enum class LinkWork : uint8_t { None, CaptureExports, UncompiledRecord, Requests, RequestsAndCapture };

struct AbsentExport {
    ModuleID module;
    uint32_t name;
};

} // anonymous namespace

static bool isNativeModule(const ModuleData& data)
{
    if (data.kind != ModuleKind::Runtime)
        return false;
    return !BuiltinTable::singleton().at(RuntimeModuleID(*data.registryID())).flags.contains(RuntimeModuleFlag::HasSource);
}

static LinkWork linkWorkOf(const ModuleData& data)
{
    if (!data.needsLink())
        return LinkWork::None;
    if (data.capturesExportsBehindEvaluation())
        return LinkWork::RequestsAndCapture;
    if (data.capturesExports())
        return LinkWork::CaptureExports;
    if (data.isUncompiledCode())
        return LinkWork::UncompiledRecord;
    return LinkWork::Requests;
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
        LinkWork work = linkWorkOf(moduleData);
        switch (work) {
        case LinkWork::None:
            return;
        case LinkWork::CaptureExports:
            append(module, StepKind::CapturesExports);
            return;
        case LinkWork::UncompiledRecord:
            append(module, StepKind::IsUncompiled);
            return;
        case LinkWork::Requests:
        case LinkWork::RequestsAndCapture:
            if (modules.status(module) != ModuleStatus::Unlinked) {
                // A module of the graph that is being evaluated may import it back.
                if (work == LinkWork::RequestsAndCapture)
                    append(module, StepKind::CapturesExports);
                return;
            }
            path.append(Frame { static_cast<uint32_t>(m_steps.size()), moduleData.firstLinkRequest, table.endOfLinkRequests(module) });
            append(module, StepKind::Enter);
            if (work == LinkWork::RequestsAndCapture)
                append(module, StepKind::CapturesExports);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
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

static void throwUnresolvedName(Modules& modules, const UnresolvedName& unresolved)
{
    switch (unresolved.resolution) {
    case NameResolution::NotFound:
        modules.throwExportNotFound(unresolved.exporter, unresolved.name);
        return;
    case NameResolution::Ambiguous:
        modules.throwExportAmbiguous(unresolved.exporter, unresolved.name);
        return;
    case NameResolution::Found:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    switch (linkWorkOf(data(root))) {
    case LinkWork::None:
        return;
    case LinkWork::Requests:
    case LinkWork::RequestsAndCapture:
        if (status(root) != ModuleStatus::Unlinked)
            return;
        break;
    case LinkWork::CaptureExports:
    case LinkWork::UncompiledRecord:
        break;
    }
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

    std::optional<AbsentExport> firstAbsentExport;
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
                firstAbsentExport = AbsentExport { module, *absentName };
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
        RELEASE_AND_RETURN(scope, throwExportNotFound(firstAbsentExport->module, firstAbsentExport->name));

    StarExportResolver resolver(*this);
    std::optional<UnresolvedName> unresolved = resolver.findUnresolvedName(modulesThatResolveNames.span());
    RETURN_IF_EXCEPTION(scope, void());
    if (unresolved)
        RELEASE_AND_RETURN(scope, throwUnresolvedName(*this, *unresolved));

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
    if (loaded.capturedFieldsAreOfRuntimeModule) {
        const LayoutData& layout = uncheckedDowncast<Object>(loaded.sourceOfCapturedFields.get())->layout();
        const FieldData* field = m_state.objects->layouts.field(vm, layout, name);
        JSValue held = field && !field->isPrivate() && !(field->flags & fieldIsNotEnumerable) ? loaded.capturedFields->get(layout.indexOf(*field)) : JSValue();
        return !held && isDefault ? JSValue(loaded.sourceOfCapturedFields) : held;
    }
    // Without the mark the module itself is the default export, whatever it has under that name.
    if (isDefault && !isMarked)
        return loaded.sourceOfCapturedFields.get();
    if ((isMarked && name == names.__esModule.impl()) || (loaded.capturedStars && name == names.constructor.impl()))
        return { };
    Object& source = *uncheckedDowncast<Object>(loaded.sourceOfCapturedFields.get());
    const LayoutData& layout = source.layout();
    const FieldData* field = m_state.objects->layouts.field(vm, layout, name);
    JSValue held = field && !field->isPrivate() ? loaded.capturedFields->get(layout.indexOf(*field)) : JSValue();
    if (held && loaded.capturedStars && isMarked && field->flags & fieldIsNotEnumerable)
        held = JSValue();
    if (!field && loaded.capturedStars)
        held = valueInSnapshotOfPendingStars(vm, source, loaded.capturedStars, name);
    if (!held && isDefault)
        return loaded.sourceOfCapturedFields.get();
    return held;
}

void Modules::makeTableOfCapturedFields(LoadedModule& loaded)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
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
    UNUSED_PARAM(realm);
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
        } else if (hasCapturedExport(module, name))
            continue;
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

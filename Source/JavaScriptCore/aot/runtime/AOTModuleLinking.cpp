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
#include "AOTProgramStatics.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

namespace {

enum class LinkWork : uint8_t { None, CaptureExports, UncompiledRecord, Requests };

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
    if (data.capturesExports())
        return LinkWork::CaptureExports;
    if (data.isUncompiledCode())
        return LinkWork::UncompiledRecord;
    return LinkWork::Requests;
}

static void checkSafeToRecurse(JSGlobalObject* realm, ThrowScope& scope)
{
    if (!realm->vm().isSafeToRecurse()) [[unlikely]]
        throwRangeError(realm, scope, "Maximum call stack size exceeded"_s);
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
    Vector<Frame, 64> path;
    auto record = [&](ModuleID module) ALWAYS_INLINE_LAMBDA {
        const ModuleData& moduleData = table.data(module);
        switch (linkWorkOf(moduleData)) {
        case LinkWork::None:
            return;
        case LinkWork::CaptureExports:
            append(module, StepKind::CapturesExports);
            return;
        case LinkWork::UncompiledRecord:
            append(module, StepKind::IsUncompiled);
            return;
        case LinkWork::Requests:
            if (modules.status(module) != ModuleStatus::Unlinked)
                return;
            path.append(Frame { static_cast<uint32_t>(m_steps.size()), moduleData.firstRequest, table.endOfRequests(module) });
            append(module, StepKind::Enter);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    record(root);
    if (path.isEmpty())
        return;

    std::span<const ModuleRequestData> requests = table.allRequests();
    BitVector seen(table.size());
    seen.quickSet(root.index());
    while (!path.isEmpty()) {
        Frame& frame = path.last();
        uint32_t next = frame.nextRequest;
        uint32_t end = frame.endOfRequests;
        size_t depth = path.size();
        while (next != end && path.size() == depth) {
            ModuleID target = requests[next++].targetModule();
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
        append(module, StepKind::Leave, !table.requiredNames(module).empty() || !table.syntheticExports(module).empty());
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
// Returns whether a module ran.
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
    if (data(root).needsLink())
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
            captureExports(module);
            return;
        }
        ASSERT(needsPath);
        AbstractModuleRecord* record = uncompiledRecord(module);
        RETURN_IF_EXCEPTION(scope, void());
        checkSafeToRecurse(realm, scope);
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
            captureExports(module);
            RETURN_IF_EXCEPTION(scope, void());
            std::optional<uint32_t> absentName = ensureLoaded(module).firstAbsentExport;
            if (absentName && !firstAbsentExport)
                firstAbsentExport = AbsentExport { module, *absentName };
            return;
        }
        AbstractModuleRecord* record = uncompiledRecord(module);
        RETURN_IF_EXCEPTION(scope, void());
        checkSafeToRecurse(realm, scope);
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
    }
    walk.replay(scope, isUnlinked, [](ModuleLinkWalk::Leaf, ModuleID) { }, [&](ModuleID module, bool) {
        setStatus(module, ModuleStatus::Linked);
    });
}

void Modules::captureExports(ModuleID module)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);

    if (loaded.capturedExports)
        return;

    JSValue value = execute(module);
    RETURN_IF_EXCEPTION(scope, void());

    Vector<Identifier, 4> names;
    MarkedArgumentBuffer values;
    auto* captureNativeModuleExports = host().captureNativeModuleExports;
    if (captureNativeModuleExports && isNativeModule(data(module)))
        captureNativeModuleExports(realm, *data(module).registryID(), names, values);
    else
        host().captureSyntheticExports(realm, data(module).exportRule, value, names, values);
    RETURN_IF_EXCEPTION(scope, void());
    if (values.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(realm, scope);
        return;
    }
    RELEASE_ASSERT(names.size() == values.size());
    // The body can require() an ES module that imports this module back. That link captured the exports as they were then.
    if (loaded.capturedExports)
        return;

    JSObject* captured = constructEmptyObject(vm, realm->nullPrototypeObjectStructure());
    for (unsigned i = 0; i < names.size(); ++i) {
        captured->putDirectMayBeIndex(realm, names[i], values.at(i));
        RETURN_IF_EXCEPTION(scope, void());
    }
    loaded.capturedExports = captured;
    RELEASE_AND_RETURN(scope, fillCapturedExports(module, captured));
}

void Modules::fillCapturedExports(ModuleID module, JSObject* captured)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);

    for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(module)) {
        JSValue found = StarExportResolver::capturedValue(realm, captured, identifier(syntheticExport.name));
        RETURN_IF_EXCEPTION(scope, void());
        if (found) {
            if (syntheticExport.hasVariable)
                ProgramStatics::writeVariable(m_instance, syntheticExport.variable, found);
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

    if (moduleData.capturesExports()) {
        StarExportResolver::forEachCapturedExport(m_instance.globalObject, capturedExports(module), [&](const Identifier& name, JSValue value) {
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

    std::sort(result.begin(), result.end(), [](const DynamicExport& a, const DynamicExport& b) {
        return codePointCompare(StringView(*a.name.impl()), StringView(*b.name.impl())) < 0;
    });
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

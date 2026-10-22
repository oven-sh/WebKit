/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModuleStarExports.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleRecord.h"
#include "ExceptionHelpers.h"
#include "JSCInlines.h"
#include "PropertyNameArray.h"

namespace JSC { namespace AOT {

using ModulePhase = AbstractModuleRecord::ModulePhase;
using Resolution = AbstractModuleRecord::Resolution;

StarExportResolver::StarExportResolver(Modules& modules)
    : m_modules(modules)
{
}

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
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(captured, instance.globalObject, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : names) {
        JSValue value = storedValue(instance.globalObject, captured, name);
        functor(name, value == JSValue(captured) ? JSValue() : value);
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
        Resolution resolution = record->resolveExport(instance.globalObject, importName);
        RETURN_IF_EXCEPTION(scope, { });
        switch (resolution.type) {
        case Resolution::Type::Resolved:
            RELEASE_AND_RETURN(scope, bindingOf(resolution));
        case Resolution::Type::NotFound:
        case Resolution::Type::Ambiguous:
        case Resolution::Type::Error:
            return UncompiledBinding { record, importName.impl() };
        }
        break;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

StarBinding StarExportResolver::bindingOf(const Resolution& resolution)
{
    VM& vm = *m_modules.instance().vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(resolution.type == Resolution::Type::Resolved);

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
    Instance& instance = m_modules.instance();
    VM& vm = *instance.vm;
    JSGlobalObject* realm = instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* captured = m_modules.capturedExports(source);

    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(captured, realm, names, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : names) {
        if (exportedNames == ExportedNames::WithoutDefault && name == vm.propertyNames->defaultKeyword)
            continue;
        StarBinding binding = bindingOfCapturedName(source, name.impl());
        JSValue value = storedValue(realm, captured, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (value == JSValue(captured)) {
            auto* capturedBinding = std::get_if<CapturedBinding>(&binding);
            if (capturedBinding && capturedBinding->module == source)
                value = JSValue();
            else {
                value = m_modules.capturedValue(source, name);
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
        join(candidates, name.impl(), { binding, value });
    }
}

// GetExportedNames() of the specification.
static Vector<Identifier> exportedNamesOf(JSGlobalObject* realm, AbstractModuleRecord* root)
{
    VM& vm = realm->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Vector<Identifier> names;
    UncheckedKeyHashSet<UniquedStringImpl*> listedNames;
    UncheckedKeyHashSet<AbstractModuleRecord*> exportStarSet;
    Vector<AbstractModuleRecord*, 8> pending;
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
            Vector<Identifier> compiledNames;
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
        Resolution resolution = record->resolveExport(realm, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (resolution.type != Resolution::Type::Resolved)
            continue;
        StarBinding binding = bindingOf(resolution);
        RETURN_IF_EXCEPTION(scope, void());
        join(candidates, name.impl(), { binding, JSValue(), namespaceObject });
    }
    // A name that is ambiguous in one target of `export *` is ambiguous in the exporter, and it is no key of the namespace object.
    Vector<Identifier> exported = exportedNamesOf(realm, record);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : exported) {
        if (namespaceKeys.contains(name.impl()))
            continue;
        Resolution resolution = record->resolveExport(realm, name);
        RETURN_IF_EXCEPTION(scope, void());
        if (resolution.type == Resolution::Type::Ambiguous)
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
    if (candidate == candidates->end())
        return nullptr;
    return &candidate->value;
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

NameResolution StarExportResolver::resolve(ModuleID exporter, uint32_t nameIdentifier)
{
    Instance& instance = m_modules.instance();
    auto scope = DECLARE_THROW_SCOPE(*instance.vm);
    const Identifier& name = m_modules.identifier(nameIdentifier);

    // It exports no stars, and a deferred export is not read for the answer.
    if (m_modules.data(exporter).exportRule == SyntheticExportRule::RuntimeModule)
        return m_modules.hasCapturedExport(exporter, name) ? NameResolution::Found : NameResolution::NotFound;

    if (m_modules.data(exporter).isUncompiledCode()) {
        AbstractModuleRecord* record = m_modules.uncompiledRecord(exporter);
        RETURN_IF_EXCEPTION(scope, NameResolution::NotFound);
        Resolution resolution = record->resolveExport(instance.globalObject, name);
        RETURN_IF_EXCEPTION(scope, NameResolution::NotFound);
        switch (resolution.type) {
        case Resolution::Type::Resolved:
            return NameResolution::Found;
        case Resolution::Type::Ambiguous:
            return NameResolution::Ambiguous;
        case Resolution::Type::NotFound:
        case Resolution::Type::Error:
            return NameResolution::NotFound;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const StarCandidate* candidate = find(exporter, name.impl());
    RETURN_IF_EXCEPTION(scope, NameResolution::NotFound);
    if (!candidate)
        return NameResolution::NotFound;
    return candidate->isAmbiguous ? NameResolution::Ambiguous : NameResolution::Found;
}

std::optional<UnresolvedName> StarExportResolver::findUnresolvedName(std::span<const ModuleID> modules)
{
    auto scope = DECLARE_THROW_SCOPE(*m_modules.instance().vm);

    for (ModuleID module : modules) {
        if (m_modules.status(module) != ModuleStatus::Unlinked)
            continue;
        for (const RequiredNameData& requiredName : m_modules.table().requiredNames(module)) {
            ModuleID exporter = requiredName.exporterModule();
            NameResolution resolution = resolve(exporter, requiredName.name);
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            if (resolution == NameResolution::Found || (resolution == NameResolution::NotFound && requiredName.mayBeAbsent))
                continue;
            return UnresolvedName { exporter, requiredName.name, resolution };
        }
    }
    return std::nullopt;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

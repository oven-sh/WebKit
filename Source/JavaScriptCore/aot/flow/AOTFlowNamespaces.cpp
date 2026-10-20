/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowNamespaces.h"

#if ENABLE(AOT)

#include "AOTFlowModules.h"
#include "AOTFlowUnseenCode.h"
#include <algorithm>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static FlowOwnProperty propertyWithoutValue(FlowOwnLookup lookup, PropertyKeyID key)
{
    return { .lookup = lookup, .key = key };
}

static FlowOwnProperty propertyWithValue(FlowOwnPresence presence, PropertyKeyID key, FlowNodeID value)
{
    return { .lookup = FlowOwnLookup::Found, .presence = presence, .key = key, .value = value };
}

namespace {

class NamespaceExports {
    WTF_MAKE_NONCOPYABLE(NamespaceExports);
public:
    NamespaceExports(const FlowWalkInputs& inputs, FlowObjectID namespaceObject)
        : m_inputs(inputs)
        , m_object(namespaceObject)
        , m_moduleID(inputs.objects.at(namespaceObject).module)
        , m_module(inputs.program.at(m_moduleID))
    {
    }

    FlowOwnProperty find(PropertyKeyID);
    FlowOwnListing collect(Vector<FlowOwnProperty, 16>&);

private:
    FlowOwnListing listing()
    {
        if (!m_listing)
            m_listing = exportListingOf(m_inputs.program, m_moduleID);
        return *m_listing;
    }

    PropertyKeyID keyOf(const PropertyKey& key) const
    {
        if (m_inputs.isSolving())
            return m_inputs.solver->types().intern(key);
        return m_inputs.program.types.find(key).value_or(PropertyKeyID());
    }

    PropertyKeyID textKeyOf(StringID name) const { return keyOf({ .kind = PropertyKeyKind::Text, .text = name }); }
    PropertyKeyID toStringTagKey() const { return keyOf({ .kind = PropertyKeyKind::WellKnownSymbol, .symbol = WellKnownSymbol::ToStringTag }); }
    PropertyKeyID esModuleMarkerKey() const { return m_inputs.program.keyOf(BuiltinNames::__esModule); }

    PropertyKeyID keyOfExport(uint32_t exportIndex) const
    {
        if (LayoutID layout = m_module.namespaceLayout)
            return m_inputs.program.at(layout).fields[exportIndex].key;
        return textKeyOf(m_module.exports[exportIndex].name);
    }

    FlowOwnProperty creationProperty(PropertyKeyID key, FlowOwnPresence presence, NOESCAPE const Invocable<void(FlowSolver&, FlowNodeID)> auto& seed)
    {
        if (FlowSolver* solver = m_inputs.solver) {
            FlowNodeID node = solver->ensurePropertyValue(m_object, key);
            seed(*solver, node);
            solver->noteLocationEvent(node, FlowLocationEvent::InitializedAtAllocation);
            return propertyWithValue(presence, key, node);
        }
        const FlowProperty* entry = key ? m_inputs.graph.findProperty(m_object, key) : nullptr;
        if (!entry || !entry->value)
            return propertyWithoutValue(FlowOwnLookup::Unknown, key);
        return propertyWithValue(presence, key, entry->value);
    }

    const ModuleExport* findExport(PropertyKeyID) const;
    FlowOwnProperty findByText(PropertyKeyID, StringID text);
    FlowOwnProperty exportProperty(PropertyKeyID, const ModuleExport&);
    FlowOwnProperty syntheticExportProperty(PropertyKeyID, const SyntheticExport&) const;
    void exposeCapturedNames() const;
    void useModuleWithoutExportTable() const;
    FlowOwnProperty capturedProperty(PropertyKeyID) const;
    FlowOwnProperty toStringTagProperty(PropertyKeyID);
    FlowOwnProperty esModuleMarkerProperty();

    const FlowWalkInputs& m_inputs;
    FlowObjectID m_object;
    ModuleID m_moduleID;
    const Module& m_module;
    std::optional<FlowOwnListing> m_listing;
};

const ModuleExport* NamespaceExports::findExport(PropertyKeyID key) const
{
    const Program& program = m_inputs.program;
    if (LayoutID layout = m_module.namespaceLayout) {
        std::optional<uint32_t> index = program.at(layout).findField(key);
        return index ? &m_module.exports[*index] : nullptr;
    }
    auto found = std::ranges::find(m_module.exports, program.at(key).text, &ModuleExport::name);
    return found == m_module.exports.end() ? nullptr : &*found;
}

FlowOwnProperty NamespaceExports::exportProperty(PropertyKeyID key, const ModuleExport& entry)
{
    if (FlowSolver* solver = m_inputs.solver)
        useModuleBehindExport(*solver, entry, noWitness);
    bool mayBeAmbiguous = entry.isFromStarExport && listing() == FlowOwnListing::MayHaveOthers;
    FlowOwnPresence presence = mayBeAmbiguous ? FlowOwnPresence::MayBeAbsent : FlowOwnPresence::ShownByValue;

    switch (entry.kind) {
    case ModuleExportKind::Variable:
        return propertyWithValue(presence, key, m_inputs.graph.variable(entry.variable));
    case ModuleExportKind::Namespace:
        return creationProperty(key, presence, [&](FlowSolver& solver, FlowNodeID node) {
            connectNamespaceOf(solver, entry.module, entry.phase, node, noWitness);
        });
    case ModuleExportKind::Indirect:
        return propertyWithoutValue(FlowOwnLookup::Unknown, key);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowOwnProperty NamespaceExports::syntheticExportProperty(PropertyKeyID key, const SyntheticExport& entry) const
{
    if (!entry.variable)
        return capturedProperty(key);
    FlowOwnPresence presence = entry.isKnownMember ? FlowOwnPresence::ShownByValue : FlowOwnPresence::MayBeAbsent;
    return propertyWithValue(presence, key, m_inputs.graph.variable(entry.variable));
}

// An unknown value stands only for what has reached native code.
void NamespaceExports::exposeCapturedNames() const
{
    if (FlowSolver* solver = m_inputs.solver)
        exposeCapturedNameSources(*solver, m_moduleID, noWitness);
}

FlowOwnProperty NamespaceExports::capturedProperty(PropertyKeyID key) const
{
    exposeCapturedNames();
    return propertyWithoutValue(FlowOwnLookup::Unknown, key);
}

FlowOwnProperty NamespaceExports::toStringTagProperty(PropertyKeyID key)
{
    FlowOwnProperty property = creationProperty(key, FlowOwnPresence::ShownByValue, [](FlowSolver& solver, FlowNodeID node) {
        solver.joinPrimitives(node, Primitive::String);
    });
    property.enumerability = FlowOwnEnumerability::NeverEnumerable;
    return property;
}

// `require` and a store mark a namespace object that has no export of this name.
FlowOwnProperty NamespaceExports::esModuleMarkerProperty()
{
    FlowOwnProperty property = creationProperty(esModuleMarkerKey(), FlowOwnPresence::ShownByValue, [](FlowSolver& solver, FlowNodeID node) {
        solver.joinPrimitives(node, Primitive::True | Primitive::Undefined);
        solver.noteLocationEvent(node, FlowLocationEvent::WrittenByUnknownCode);
    });
    property.enumerability = FlowOwnEnumerability::NeverEnumerable;
    return property;
}

FlowOwnProperty NamespaceExports::findByText(PropertyKeyID key, StringID text)
{
    if (const ModuleExport* entry = findExport(key))
        return exportProperty(key, *entry);

    auto synthetic = std::ranges::find(m_module.syntheticExports, text, &SyntheticExport::name);
    if (synthetic != m_module.syntheticExports.end())
        return synthetic->variable ? syntheticExportProperty(key, *synthetic) : capturedProperty(key);

    if (listing() == FlowOwnListing::MayHaveOthers)
        return capturedProperty(key);
    if (key == esModuleMarkerKey())
        return esModuleMarkerProperty();
    return propertyWithoutValue(FlowOwnLookup::Absent, key);
}

// The names of a module without an export table are those that its evaluation leaves.
void NamespaceExports::useModuleWithoutExportTable() const
{
    FlowSolver* solver = m_inputs.solver;
    if (!solver || m_module.exportRule == SyntheticExportRule::None)
        return;
    useModule(*solver, m_moduleID, noWitness);
    captureExportsForImporter(*solver, m_moduleID);
}

FlowOwnProperty NamespaceExports::find(PropertyKeyID key)
{
    useModuleWithoutExportTable();
    const PropertyKey& record = m_inputs.program.at(key);
    switch (record.kind) {
    case PropertyKeyKind::Text:
        return findByText(key, record.text);
    case PropertyKeyKind::WellKnownSymbol:
        if (record.symbol == WellKnownSymbol::ToStringTag)
            return toStringTagProperty(key);
        return propertyWithoutValue(FlowOwnLookup::Absent, key);
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return propertyWithoutValue(FlowOwnLookup::Absent, key);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowOwnListing NamespaceExports::collect(Vector<FlowOwnProperty, 16>& properties)
{
    useModuleWithoutExportTable();
    for (uint32_t index = 0; index < m_module.exports.size(); ++index)
        properties.append(exportProperty(keyOfExport(index), m_module.exports[index]));
    for (const SyntheticExport& entry : m_module.syntheticExports)
        properties.append(syntheticExportProperty(textKeyOf(entry.name), entry));
    properties.append(toStringTagProperty(toStringTagKey()));
    if (listing() == FlowOwnListing::MayHaveOthers)
        exposeCapturedNames();
    else if (!findExport(esModuleMarkerKey()))
        properties.append(esModuleMarkerProperty());

    bool hasUnknownProperty = properties.containsIf([](const FlowOwnProperty& property) {
        return property.lookup == FlowOwnLookup::Unknown;
    });
    return hasUnknownProperty ? FlowOwnListing::MayHaveOthers : listing();
}

} // anonymous namespace

FlowOwnListing exportListingOf(const Program& program, ModuleID id)
{
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::SourceText:
        break;
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return FlowOwnListing::MayHaveOthers;
    }

    bool hasDynamicStarExport = module.requests.containsIf([](const ModuleRequest& request) {
        return request.isDynamicStarExport;
    });
    return hasDynamicStarExport ? FlowOwnListing::MayHaveOthers : FlowOwnListing::Complete;
}

FlowOwnProperty findNamespaceExport(const FlowWalkInputs& inputs, FlowObjectID namespaceObject, PropertyKeyID key)
{
    return NamespaceExports(inputs, namespaceObject).find(key);
}

FlowOwnListing forEachNamespaceExport(const FlowWalkInputs& inputs, FlowObjectID namespaceObject, NOESCAPE const FlowOwnPropertyFunctor& functor)
{
    Vector<FlowOwnProperty, 16> properties;
    FlowOwnListing listing = NamespaceExports(inputs, namespaceObject).collect(properties);
    for (const FlowOwnProperty& property : properties)
        functor(property);
    return listing;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

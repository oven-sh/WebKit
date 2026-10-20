/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjects.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowObjectIndexes.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <utility>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT { namespace Typed {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FlowObjectTable);

static void setOnce(FlowObjectID& entry, FlowObjectID object)
{
    RELEASE_ASSERT(!entry);
    entry = object;
}

void FlowObjectIndexesDeleter::operator()(FlowObjectIndexes* indexes) const
{
    delete indexes;
}

FlowObjectIndexes::FlowObjectIndexes(const Program& program)
{
    builtinObjects.grow(program.builtins().builtinCount());
    namespaceObjects.grow(program.modules.size());
    closuresByFunction.grow(program.functions.size());
    allocationClassesByLayout.grow(program.layouts.size());
    layoutObjects.grow(program.layouts.size());
}

void FlowObjectIndexes::didAppend(FlowObjectID object, const FlowObject& record)
{
    switch (record.kind) {
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return;
    case FlowObjectKind::Allocation:
        if (record.standsForLayout)
            setOnce(layoutObjects[record.layout.index()], object);
        return;
    case FlowObjectKind::Closure:
        closuresByFunction[record.function.index()].append(object);
        return;
    case FlowObjectKind::Prototype: {
        auto result = prototypeObjects.add(record.owner, object);
        RELEASE_ASSERT(result.isNewEntry);
        return;
    }
    case FlowObjectKind::ModuleNamespace:
        setOnce(namespaceObjects[record.module.index()], object);
        return;
    case FlowObjectKind::Builtin:
        setOnce(builtinObjects[record.builtin.index()], object);
        return;
    case FlowObjectKind::ModuleBuiltin: {
        auto result = moduleBuiltinObjects.add(std::pair { record.builtin, record.module }, object);
        RELEASE_ASSERT(result.isNewEntry);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowObjectIndexes::didAppend(AllocationClassID allocationClass, const FlowAllocationClassMembers& members)
{
    if (members.layout)
        allocationClassesByLayout[members.layout.index()].append(allocationClass);
}

FlowObjectTable::FlowObjectTable(const Program& program)
    : m_program(program)
    , m_indexes(new FlowObjectIndexes(program))
{
    FlowObject record;
    record.hasUndeclaredPart = true;
    FlowObjectID unknown = append(record);
    RELEASE_ASSERT(unknown == unknownFlowObject);
}

FlowObjectTable::~FlowObjectTable() = default;

AllocationSiteID FlowObjectTable::siteOf(FunctionID function, uint32_t siteInSummary) const
{
    size_t functionIndex = function.index();
    RELEASE_ASSERT(functionIndex + 1 < m_firstSiteOfFunction.size());
    uint32_t firstSite = m_firstSiteOfFunction[functionIndex];
    uint32_t endSite = m_firstSiteOfFunction[functionIndex + 1];
    RELEASE_ASSERT(siteInSummary < endSite - firstSite);
    return AllocationSiteID(firstSite + siteInSummary);
}

uint32_t FlowObjectTable::numberOfSitesOf(FunctionID function) const
{
    size_t functionIndex = function.index();
    RELEASE_ASSERT(functionIndex + 1 < m_firstSiteOfFunction.size());
    return m_firstSiteOfFunction[functionIndex + 1] - m_firstSiteOfFunction[functionIndex];
}

std::span<const AllocationClassID> FlowObjectTable::allocationClassesOf(LayoutID layout) const
{
    return m_indexes->allocationClassesByLayout[layout.index()].span();
}

LayoutID FlowObjectTable::allocatedLayoutOf(FlowObjectID object) const
{
    const FlowObject& record = at(object);
    if (!record.layout || !record.allocationClass)
        return record.layout;
    return at(record.allocationClass).layout;
}

std::span<const LayoutID> FlowObjectTable::derivedLayoutsOf(LayoutID declared) const
{
    auto found = m_indexes->derivedLayouts.find(declared);
    return found == m_indexes->derivedLayouts.end() ? std::span<const LayoutID> { } : found->value.span();
}

void FlowObjectTable::moveToDerivedLayout(AllocationClassID allocationClass, LayoutID derived)
{
    FlowAllocationClassMembers& members = mutableAt(allocationClass);
    RELEASE_ASSERT(members.layout && m_program.at(derived).flow.base == members.layout);
    Vector<LayoutID, 1>& derivedLayouts = m_indexes->derivedLayouts.add(members.layout, Vector<LayoutID, 1>()).iterator->value;
    if (!derivedLayouts.contains(derived))
        derivedLayouts.append(derived);

    auto& classesByLayout = m_indexes->allocationClassesByLayout;
    if (classesByLayout.size() < m_program.layouts.size()) {
        classesByLayout.grow(m_program.layouts.size());
        m_indexes->layoutObjects.grow(m_program.layouts.size());
    }
    bool wasListed = classesByLayout[members.layout.index()].removeFirst(allocationClass);
    RELEASE_ASSERT(wasListed);
    classesByLayout[derived.index()].append(allocationClass);
    members.layout = derived;
}

void FlowObjectTable::splitAllocationClass(AllocationClassID allocationClass, std::span<const std::pair<uint32_t, FlowObjectID>> objectsInRuns)
{
    RELEASE_ASSERT(!objectsInRuns.empty());
    FlowAllocationClassMembers original = at(allocationClass);
    AllocationClassID current = allocationClass;
    for (size_t index = 0; index < objectsInRuns.size(); ++index) {
        auto [run, object] = objectsInRuns[index];
        bool startsRun = !index || objectsInRuns[index - 1].first != run;
        if (startsRun) {
            FlowAllocationClassMembers members = original;
            members.sharedObject = object;
            members.copies.clear();
            if (index)
                current = appendAllocationClass(WTF::move(members));
            else
                mutableAt(current) = WTF::move(members);
        } else
            mutableAt(current).copies.append(object);
        mutableAt(object).allocationClass = current;
    }
}

bool FlowObjectTable::isMadeByBuiltinConstructor(FlowObjectID object) const
{
    AllocationSiteID site = at(object).site;
    return site && at(site).description.origin == FlowSiteOrigin::BuiltinConstructor;
}

FlowObjectID FlowObjectTable::layoutObject(LayoutID layout) const
{
    return m_indexes->layoutObjects[layout.index()];
}

FlowObjectID FlowObjectTable::findBuiltin(BuiltinID builtin) const
{
    if (!builtin)
        return { };
    return m_indexes->builtinObjects[m_program.builtins().canonicalID(builtin).index()];
}

FlowObjectID FlowObjectTable::findModuleBuiltin(BuiltinID builtin, ModuleID module) const
{
    if (!builtin || !module)
        return { };
    return m_indexes->moduleBuiltinObjects.get(std::pair { m_program.builtins().canonicalID(builtin), module });
}

FlowObjectID FlowObjectTable::findNamespace(ModuleID module) const
{
    return m_indexes->namespaceObjects[module.index()];
}

FlowObjectID FlowObjectTable::findPrototype(FlowObjectID closure) const
{
    return m_indexes->prototypeObjects.get(closure);
}

std::span<const FlowObjectID> FlowObjectTable::closuresOf(FunctionID function) const
{
    if (function.index() >= m_indexes->closuresByFunction.size())
        return { };
    return m_indexes->closuresByFunction[function.index()].span();
}

bool FlowObjectTable::isInstanceOf(FlowObjectID object, BuiltinClassID builtinClass) const
{
    const FlowObject& record = at(object);
    switch (record.kind) {
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        return record.builtinClass && m_program.builtins().isSameOrSubclassOf(record.builtinClass, builtinClass);
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Closure:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FlowObjectTable::isCallable(FlowObjectID object) const
{
    const FlowObject& record = at(object);
    const BuiltinTable& builtins = m_program.builtins();
    switch (record.kind) {
    case FlowObjectKind::Closure:
        return true;
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return isCallableBuiltin(builtins, record.builtin);
    case FlowObjectKind::Instance:
        return builtins.isSameOrSubclassOf(record.builtinClass, BuiltinClasses::Function);
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FlowObjectTable::hasCaptures(FlowObjectID object) const
{
    const FlowObject& record = at(object);
    switch (record.kind) {
    case FlowObjectKind::Closure:
        return true;
    case FlowObjectKind::Instance:
        return m_program.builtins().at(record.builtinClass).hasClosureInstances;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Allocation:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::Opaque:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FunctionID FlowObjectTable::allocatingFunctionOf(FlowObjectID object) const
{
    const FlowObject& record = at(object);
    if (record.site)
        return at(record.site).function;
    if (record.kind == FlowObjectKind::Prototype)
        return allocatingFunctionOf(record.owner);
    return { };
}

Wire::SourceLocation FlowObjectTable::locationOf(FlowObjectID object) const
{
    const FlowObject& record = at(object);
    if (record.kind == FlowObjectKind::Prototype)
        return locationOf(record.owner);
    if (!record.site)
        return noSourceLocation;

    const FlowSite& site = at(record.site);
    if (site.function.index() >= m_program.functions.size())
        return noSourceLocation;

    const Function& function = m_program.at(site.function);
    return site.description.value ? function.locationOf(site.description.value) : function.declarationLocation();
}

FlowObjectID FlowObjectTable::append(const FlowObject& record)
{
    if (m_objects.size() > FlowObjectSet::maxObjectIndex) {
        if (m_indexes->build && !std::exchange(m_indexes->didReportFullObjectIDs, true))
            m_indexes->build->reportLimitExceeded(LimitID::MaxProgramIds, m_objects.size() + 1);
        return unknownFlowObject;
    }

    FlowObjectID object(m_objects.size());
    m_objects.append(record);
    m_indexes->didAppend(object, m_objects.last());
    return object;
}

AllocationSiteID FlowObjectTable::appendSite(const FlowSite& site)
{
    AllocationSiteID id(m_sites.size());
    m_sites.append(site);
    return id;
}

AllocationClassID FlowObjectTable::appendAllocationClass(FlowAllocationClassMembers&& members)
{
    AllocationClassID id(m_classes.size());
    m_indexes->didAppend(id, members);
    m_classes.append(WTF::move(members));
    return id;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompiledStructure.h"

#if ENABLE(AOT)

#include "AOTEnvironment.h"
#include "AOTLayoutData.h"
#include "AbstractSlotVisitorInlines.h"
#include "JSCInlines.h"
#include "SlotVisitorInlines.h"
#include <algorithm>

namespace JSC { namespace AOT {

static bool hasOnlyPlainSlots(const LayoutData& layout)
{
    constexpr uint16_t flagsOfOtherFields = fieldIsStoredOnce | fieldIsPrivate | fieldIsBrand;
    return std::ranges::none_of(layout.fields(), [](const FieldData& field) {
        return field.flags & flagsOfOtherFields || field.slot.representation() == SlotRepresentation::DeferredClosure;
    });
}

CompiledStructure::CompiledStructure(VM& vm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const ObjectParameters& parameters)
    : Structure(vm, StructureVariant::Compiled, parameters.realm, parameters.prototype, typeInfo, classInfo)
    , m_layout(parameters.layout)
{
    ASSERT(typeInfo.type() == CompiledObjectType && parameters.layout);
    if (std::optional<SlotData> slot = m_layout->extraPropertiesSlot())
        m_extraPropertiesSlot = *slot;
    if (std::optional<SlotData> slot = m_layout->extensionSlot())
        m_extensionSlot = *slot;

    if (m_layout->hasFieldsWithSymbolKeys)
        m_flags.add(Flag::HasFieldsWithSymbolKeys);
    if (AOT::hasOnlyPlainSlots(*m_layout))
        m_flags.add(Flag::HasOnlyPlainSlots);
    if (parameters.hasOtherPrototype)
        m_flags.add(Flag::HasOtherPrototype);
    if (parameters.extraProperties) {
        m_flags.add(Flag::HasExtraProperties);
        m_extraPropertiesCellSize = parameters.extraProperties->cellSize;
        m_extraPropertiesStructure.setWithoutWriteBarrier(parameters.extraProperties->structureOrNull);
    }
    if (parameters.outOfLineKeyOrder) {
        m_flags.add(Flag::HasOutOfLineKeyOrder);
        m_outOfLineKeyOrder = FixedVector<uint16_t>(*parameters.outOfLineKeyOrder);
    }

    setMayBePrototype(parameters.mayBePrototype);
    setIsQuickPropertyAccessAllowedForEnumeration(false);
    setDidPreventExtensions(false);
    setHasAnyKindOfGetterSetterProperties(true);
    setHasReadOnlyOrGetterSetterPropertiesExcludingProto(true);
    setHasNonEnumerableProperties(true);
    setHasNonConfigurableProperties(true);
    setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
}

CompiledStructure::CompiledStructure(VM& vm, JSGlobalObject* realm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const EnvironmentShapes& shapes, uint16_t shapePage)
    : Structure(vm, StructureVariant::Compiled, realm, jsNull(), typeInfo, classInfo)
    , m_environmentShapes(&shapes)
    , m_environmentShapePage(shapePage)
{
    ASSERT(typeInfo.type() == CompiledEnvironmentType);
}

CompiledStructure* CompiledStructure::createForObject(VM& vm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const ObjectParameters& parameters)
{
    ASSERT(vm.structureStructure);
    CompiledStructure* structure = new (NotNull, allocateCell<CompiledStructure>(vm)) CompiledStructure(vm, typeInfo, classInfo, parameters);
    structure->finishCreation(vm);
    if (structure->extraPropertiesStructure())
        vm.writeBarrier(structure);
    return structure;
}

CompiledStructure* CompiledStructure::createForEnvironment(VM& vm, JSGlobalObject* realm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const EnvironmentShapes& shapes, uint16_t shapePage)
{
    ASSERT(vm.structureStructure);
    CompiledStructure* structure = new (NotNull, allocateCell<CompiledStructure>(vm)) CompiledStructure(vm, realm, typeInfo, classInfo, shapes, shapePage);
    structure->finishCreation(vm);
    return structure;
}

void CompiledStructure::destroy(JSCell* cell)
{
    static_cast<CompiledStructure*>(cell)->CompiledStructure::~CompiledStructure();
}

template<typename Visitor>
void CompiledStructure::visitAdditionalChildren(JSCell* cell, Visitor& visitor)
{
    visitor.append(uncheckedDowncast<CompiledStructure>(uncheckedDowncast<Structure>(cell))->m_extraPropertiesStructure);
}

template void CompiledStructure::visitAdditionalChildren(JSCell*, AbstractSlotVisitor&);
template void CompiledStructure::visitAdditionalChildren(JSCell*, SlotVisitor&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

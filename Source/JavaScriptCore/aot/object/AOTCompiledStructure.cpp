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
#include <memory>

namespace JSC { namespace AOT {

CompiledStructure::CompiledStructure(VM& vm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const ObjectParameters& parameters)
    : Structure(vm, StructureVariant::Compiled, parameters.realm, parameters.prototype, typeInfo, classInfo)
    , m_layout(parameters.layout)
    , m_listedFields(nullptr)
    , m_listedSize(0)
    , m_listedOrderAndCount(ListedFields::noOrderWord)
{
    ASSERT(typeInfo.type() == CompiledObjectType);
    setIsQuickPropertyAccessAllowedForEnumeration(false);
    setDidPreventExtensions(false);
    setHasAnyKindOfGetterSetterProperties(true);
    setHasReadOnlyOrGetterSetterPropertiesExcludingProto(true);
    setHasNonEnumerableProperties(true);
    setHasNonConfigurableProperties(true);
    setHasNonConfigurableReadOnlyOrGetterSetterProperties(true);
    initializeForObject(parameters);
}

CompiledStructure::CompiledStructure(VM& vm, const CompiledStructure& templateStructure, const ObjectParameters& parameters)
    : Structure(vm, templateStructure, parameters.prototype)
    , m_layout(parameters.layout)
    , m_listedFields(nullptr)
    , m_listedSize(0)
    , m_listedOrderAndCount(ListedFields::noOrderWord)
{
    ASSERT(templateStructure.describesObject() && templateStructure.realm() == parameters.realm);
    initializeForObject(parameters);
}

CompiledStructure::CompiledStructure(VM& vm, JSGlobalObject* realm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const EnvironmentShapes& shapes, uint16_t shapePage)
    : Structure(vm, StructureVariant::Compiled, realm, jsNull(), typeInfo, classInfo)
    , m_environmentShapes(&shapes)
    , m_listedFields(nullptr)
    , m_environmentShapePage(shapePage)
    , m_listedOrderAndCount(ListedFields::noOrderWord)
{
    ASSERT(typeInfo.type() == CompiledEnvironmentType);
}

CompiledStructure::~CompiledStructure()
{
    delete rareData();
}

void CompiledStructure::initializeForObject(const ObjectParameters& parameters)
{
    static_assert(std::endian::native == std::endian::little);
    static_assert(static_cast<uint32_t>(endOfExtraPropertiesCellSizes) << 16 == orderWordHasExtraProperties);
    static_assert(ListedFields::noOrderWord != outOfLineOrderWord);
    ASSERT(m_layout && (!parameters.heldExtraProperties || parameters.extraProperties));

    if (std::optional<SlotData> slot = m_layout->extraPropertiesSlot())
        m_extraPropertiesSlot = *slot;
    if (std::optional<SlotData> slot = m_layout->extensionSlot())
        m_extensionSlot = *slot;

    if (m_layout->hasFieldsWithSymbolKeys)
        m_flags.add(Flag::HasFieldsWithSymbolKeys);
    if (m_layout->hasOnlyPlainSlots)
        m_flags.add(Flag::HasOnlyPlainSlots);
    if (m_layout->isCopiedByWords)
        m_flags.add(Flag::IsCopiedByWords);
    RELEASE_ASSERT(isPristine() == parameters.hasOtherPrototype);
    if (parameters.hasOtherPrototype)
        m_flags.add(Flag::HasOtherPrototype);
    if (parameters.extraProperties)
        m_flags.add(Flag::HasExtraProperties);
    if (parameters.outOfLineKeyOrder)
        m_flags.add(Flag::HasOutOfLineKeyOrder);
    if (!parameters.hasOtherPrototype && !parameters.extraProperties && parameters.prototype == JSValue(parameters.realm->objectPrototype()))
        m_flags.add(Flag::HasOnlyFieldsBelowObjectPrototype);
    setMayBePrototype(parameters.mayBePrototype);
    if (holdsListedFields())
        return;

    std::unique_ptr<RareData> rareData;
    if (parameters.outOfLineKeyOrder || parameters.heldExtraProperties) {
        rareData = makeUnique<RareData>();
        if (parameters.outOfLineKeyOrder)
            rareData->outOfLineKeyOrder = FixedVector<uint16_t>(*parameters.outOfLineKeyOrder);
        rareData->heldExtraProperties.setWithoutWriteBarrier(parameters.heldExtraProperties);
    }
    uintptr_t bitsOfRareData = std::bit_cast<uintptr_t>(rareData.release());
    RELEASE_ASSERT(!(bitsOfRareData >> 48));
    m_highBitsOfRareData = static_cast<uint16_t>(bitsOfRareData >> 32);
    m_namedStructureAndRareData = { 0, static_cast<uint32_t>(bitsOfRareData) };

    if (parameters.extraProperties) {
        RELEASE_ASSERT(parameters.extraProperties->cellSize < endOfExtraPropertiesCellSizes);
        if (Structure* named = parameters.extraProperties->structureOrNull)
            m_namedStructureAndRareData.extraPropertiesStructureID = named->id().bits();
        m_extraKeysAndCellSize = { unknownNumberOfExtraKeys, parameters.extraProperties->cellSize };
    }
}

static CompiledStructure* finishCreationOfObjectStructure(VM& vm, CompiledStructure* structure)
{
    if (structure->extraPropertiesStructure() || structure->heldExtraProperties())
        vm.writeBarrier(structure);
    return structure;
}

CompiledStructure* CompiledStructure::createForObject(VM& vm, const TypeInfo& typeInfo, const ClassInfo* classInfo, const ObjectParameters& parameters)
{
    ASSERT(vm.structureStructure);
    CompiledStructure* structure = new (NotNull, allocateCell<CompiledStructure>(vm)) CompiledStructure(vm, typeInfo, classInfo, parameters);
    structure->finishCreation(vm);
    return finishCreationOfObjectStructure(vm, structure);
}

CompiledStructure* CompiledStructure::createForObject(VM& vm, const CompiledStructure& templateStructure, const ObjectParameters& parameters)
{
    CompiledStructure* structure = new (NotNull, allocateCell<CompiledStructure>(vm)) CompiledStructure(vm, templateStructure, parameters);
    structure->finishCreation(vm);
    return finishCreationOfObjectStructure(vm, structure);
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
    CompiledStructure* structure = uncheckedDowncast<CompiledStructure>(uncheckedDowncast<Structure>(cell));
    if (Structure* named = structure->extraPropertiesStructure())
        visitor.appendUnbarriered(named);
    if (const RareData* rareData = structure->rareData())
        visitor.append(rareData->heldExtraProperties);
}

template void CompiledStructure::visitAdditionalChildren(JSCell*, AbstractSlotVisitor&);
template void CompiledStructure::visitAdditionalChildren(JSCell*, SlotVisitor&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

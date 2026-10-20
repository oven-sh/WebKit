/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStructureTable.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTEnvironment.h"
#include "AOTImmortalCells.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTSlotAccess.h"
#include "AOTState.h"
#include "JSCInlines.h"
#include "MarkedSpace.h"
#include <algorithm>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

namespace {

enum class AllocatedCellKind : uint8_t { Object, Closure, Environment };

}

static AllocatedCellKind allocatedCellKindOf(const ProgramTables& tables, const AllocationData& allocation)
{
    if (allocation.shape < tables.firstEnvironmentShape)
        return AllocatedCellKind::Object;
    switch (tables.environmentForShape(allocation.shape).kind) {
    case LayoutKind::Closure:
        return AllocatedCellKind::Closure;
    case LayoutKind::Environment:
    case LayoutKind::CoroutineFrame:
        return AllocatedCellKind::Environment;
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr uint64_t orderAndShapeWordsFor(uint32_t shape, uint32_t orderWord)
{
    return static_cast<uint64_t>(shapeWordOf(shape)) << 32 | orderWord;
}

void StructureTable::fillAllocationSite(VM& vm, uint32_t number)
{
    const AllocationData& allocation = m_tables.allocations()[number];
    AllocatedCellKind kind = allocatedCellKindOf(m_tables, allocation);
    Structure* structure = this->structure(vm, allocation.shape);
    if (allocation.constructedBy != AllocationData::notConstructed) {
        ASSERT(kind == AllocatedCellKind::Object);
        ObjectModel& objects = *m_state.objects;
        FunctionID function(allocation.constructedBy);
        // Without a class object that the run time knows, the operation names the prototype. That is a constructor function that may have many closures, or captures something.
        // It is also a class with captures whose definition was forgotten: its next definition empties this record again.
        JSCell* constructor = objects.closures.hasSingletonClosure(function) ? objects.closures.closure(vm, function) : objects.closures.closureIfCreated(function);
        if (constructor) {
            structure = objectStructure(vm, allocation.shape, objects.classes.prototypeForInstancesOf(vm, asObject(constructor)));
            m_state.immortalCells->add(structure);
        }
    }
    CompleteSubspace& space = kind == AllocatedCellKind::Object ? vm.cellSpace() : vm.heap.aotClosureSpace;
    AllocationSiteRecord record {
        .header = kind == AllocatedCellKind::Environment ? Environment::headerFor(structure, allocation.shape) : cellHeaderFor(structure),
        .orderAndShapeWords = orderAndShapeWordsFor(allocation.shape, allocation.orderWord),
        .allocator = space.allocatorFor(allocation.size, AllocatorForMode::EnsureAllocator).localAllocator(),
        .unused = 0,
    };
    ObjectWords& words = m_state.instance.objects;
    if (AllocationSiteRecord* existing = words.allocationSite(number)) {
        *existing = record;
        return;
    }
    uint32_t recordIndex = ++m_numberOfAllocationSiteRecords;
    *(words.emptyAllocationSite() - recordIndex) = record;
    words.distancesToAllocationSites[number] = -safeCast<int32_t>(recordIndex * sizeof(AllocationSiteRecord));
}

void StructureTable::predictStructure(VM& vm, uint32_t number, CompiledStructure* structure)
{
    // Where the class is evaluated once, an operation that names no prototype may share the record.
    const AllocationData& allocation = m_tables.allocations()[number];
    if (allocation.constructedBy == AllocationData::notConstructed || structure->layout().shape != allocation.shape || structure->layout().evaluation() == EvaluationState::ProvenOnce)
        return;
    CompiledStructure* usual = objectStructure(vm, allocation.shape);
    AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (!record || !usual->hasOtherPrototype() || structureIDOf(number) != usual->id())
        return;
    m_state.immortalCells->add(structure);
    record->header = cellHeaderFor(structure);
}

std::optional<StructureID> StructureTable::structureIDOf(uint32_t number) const
{
    const AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (!record || !record->header)
        return std::nullopt;
    return std::bit_cast<StructureID>(static_cast<uint32_t>(record->header));
}

// Compiled code takes the slow path at a record whose allocator has no room, and the slow path fills a record without a header.
void StructureTable::forgetStructuresOf(const LayoutData& classLayout)
{
    // The shapes of the layouts that were derived from the class are numbered wherever the tests of the program want them.
    const ClassTable& classes = m_state.objects->classes;
    auto belongsToClass = [&](uint32_t shape) {
        return m_tables.hasLayoutForShape(shape) && classes.classOfInstances(m_tables.layoutForShape(shape)).index == classLayout.index;
    };
    for (uint32_t shape = 1; shape < m_tables.firstEnvironmentShape; ++shape) {
        if (m_structures[shape] && belongsToClass(shape))
            m_structures[shape] = 0;
    }

    std::span<const AllocationData> allocations = m_tables.allocations();
    for (uint32_t number = 0; number < allocations.size(); ++number) {
        AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
        if (record && allocations[number].shape < m_tables.firstEnvironmentShape && belongsToClass(allocations[number].shape))
            *record = emptyAllocationSiteRecord();
    }
}

JSCell* StructureTable::allocate(VM& vm, uint32_t number)
{
    if (!structureIDOf(number))
        fillAllocationSite(vm, number);
    Structure* structure = structureIDOf(number)->decode();
    const AllocationData& allocation = m_tables.allocations()[number];
    switch (allocatedCellKindOf(m_tables, allocation)) {
    case AllocatedCellKind::Object:
        return Object::create(vm, structure, m_tables.layoutForShape(allocation.shape), allocation.size, allocation.orderWord);
    case AllocatedCellKind::Closure:
        return Closure::createForCompiledCode(vm, structure, allocation.size);
    case AllocatedCellKind::Environment:
        return Environment::create(vm, structure, m_tables.environmentForShape(allocation.shape), allocation.size);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

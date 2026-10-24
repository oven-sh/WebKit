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
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTRuntimeCounters.h"
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

static bool fillsSameRecord(const ProgramTables& tables, uint32_t first, uint32_t second)
{
    const AllocationData& a = tables.allocations()[first];
    const AllocationData& b = tables.allocations()[second];
    return a.shape == b.shape && a.orderWord() == b.orderWord() && a.size() == b.size() && a.isBornUnfinished == b.isBornUnfinished && tables.constructorOfAllocation(first) == tables.constructorOfAllocation(second);
}

// Allocations that list other fields may ask for the same cell. The image lists the allocations of one shape next to each other.
AllocationSiteRecord* StructureTable::recordOfSameAllocation(uint32_t number) const
{
    std::span<const AllocationData> allocations = m_tables.allocations();
    const ObjectWords& words = m_state.instance.objects;
    const AllocationData& allocation = allocations[number];
    auto recordOf = [&](uint32_t other) -> AllocationSiteRecord* {
        AllocationSiteRecord* record = words.allocationSite(other);
        return record && fillsSameRecord(m_tables, other, number) ? record : nullptr;
    };
    for (uint32_t other = number; other && allocations[other - 1].shape == allocation.shape; --other) {
        if (AllocationSiteRecord* record = recordOf(other - 1))
            return record;
    }
    for (uint32_t other = number + 1; other < allocations.size() && allocations[other].shape == allocation.shape; ++other) {
        if (AllocationSiteRecord* record = recordOf(other))
            return record;
    }
    return nullptr;
}

AllocationSiteRecord& StructureTable::ensureRecord(uint32_t number)
{
    ObjectWords& words = m_state.instance.objects;
    if (AllocationSiteRecord* record = words.allocationSite(number))
        return *record;
    // Nearly every record is the first of its shape, and then there is no other to find: no object has been made yet, so the shape has no Structure.
    uint32_t shape = m_tables.allocations()[number].shape;
    bool isFirstOfShape = shape < m_tables.firstEnvironmentShape && !m_structures[shape];
    AllocationSiteRecord* record = isFirstOfShape ? nullptr : recordOfSameAllocation(number);
    if (!record) {
        record = words.emptyAllocationSite() - ++m_numberOfAllocationSiteRecords;
        *record = emptyAllocationSiteRecord();
    }
    words.distancesToAllocationSites[number] = safeCast<int32_t>(reinterpret_cast<char*>(record) - reinterpret_cast<char*>(words.emptyAllocationSite()));
    return *record;
}

// An allocation that asks for more than its layout takes makes the inner objects with the outer one.
static bool holdsInnerObjects(const ProgramTables& tables, const AllocationData& allocation)
{
    return allocation.shape < tables.firstEnvironmentShape && allocation.size() > tables.layoutForShape(allocation.shape).size;
}

static CompleteSubspace& spaceFor(VM& vm, const AllocationData& allocation, const LayoutData& layout)
{
    return allocation.size() > layout.size ? *vm.heap.aotOuterCellSpace<SubspaceAccess::OnMainThread>() : vm.cellSpace();
}

static uint64_t objectHeaderFor(Structure* structure, const AllocationData& allocation)
{
    static_assert(JSCell::typeInfoFlagsOffset() == 6);
    return cellHeaderFor(structure) | (allocation.isBornUnfinished ? static_cast<uint64_t>(TypeInfoPerCellBit) << 48 : 0);
}

AllocationSiteRecord& StructureTable::fillAllocationSite(VM& vm, uint32_t number)
{
    // Until it is filled, compiled code finds no room at the record, as at one whose Structure was forgotten.
    AllocationSiteRecord& record = ensureRecord(number);
    if (record.header)
        return record;
    const AllocationData& allocation = m_tables.allocations()[number];
    AllocatedCellKind kind = allocatedCellKindOf(m_tables, allocation);
    Structure* structure = nullptr;
    if (allocation.isConstructed) {
        ASSERT(kind == AllocatedCellKind::Object);
        ObjectModel& objects = *m_state.objects;
        FunctionID function(m_tables.constructorOfAllocation(number));
        // Without a class object that the run time knows, the operation names the prototype.
        JSCell* constructor = objects.closures.hasSingletonClosure(function) ? objects.closures.closure(vm, function) : objects.closures.closureIfCreated(function);
        if (constructor)
            structure = objectStructure(vm, allocation.shape, objects.classes.prototypeForInstancesOf(vm, asObject(constructor)));
    }
    if (!structure)
        structure = this->structure(vm, allocation.shape);
    if (holdsInnerObjects(m_tables, allocation)) {
        // The record of what a class constructs waits for the class: to fill it may make the class object.
        for (uint32_t inner : m_tables.layoutForShape(allocation.shape).allocationsOfInnerObjects()) {
            if (!m_tables.allocations()[inner].isConstructed)
                fillAllocationSite(vm, inner);
        }
    }
    CompleteSubspace& space = kind == AllocatedCellKind::Object ? spaceFor(vm, allocation, m_tables.layoutForShape(allocation.shape)) : *vm.heap.aotClosureSpace<SubspaceAccess::OnMainThread>();
    // No size class holds a cell beyond MarkedSpace::largeCutoff, so no allocator does. Compiled code finds no room and leaves every such cell to the operation.
    void* allocator = space.allocatorFor(allocation.size(), AllocatorForMode::EnsureAllocator).localAllocator();
    record = AllocationSiteRecord {
        .header = kind == AllocatedCellKind::Environment ? Environment::headerFor(structure, allocation.shape) : objectHeaderFor(structure, allocation),
        .orderAndShapeWords = orderAndShapeWordsFor(allocation.shape, allocation.orderWord()),
        .allocator = allocator ? allocator : &m_allocatorWithoutRoom,
        .unused = 0,
    };
    m_writtenRecords.record(&record);
    count(vm, RuntimeCounterID::AllocationSiteRecordsFilled);
    return record;
}

void StructureTable::predictStructure(VM&, uint32_t number, CompiledStructure* structure)
{
    // Where the class is evaluated once, an operation that names no prototype may share the record.
    const AllocationData& allocation = m_tables.allocations()[number];
    if (!allocation.isConstructed || structure->layout().shape != allocation.shape || structure->layout().evaluation() == EvaluationState::ProvenOnce)
        return;
    CompiledStructure* usual = usualStructureIfExists(allocation.shape);
    AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (!usual || !record || !usual->hasOtherPrototype() || structureIDOf(number) != usual->id())
        return;
    record->header = objectHeaderFor(structure, allocation);
    m_writtenRecords.record(record);
}

std::optional<StructureID> StructureTable::structureIDOf(uint32_t number) const
{
    const AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (!record || !record->header)
        return std::nullopt;
    return std::bit_cast<StructureID>(static_cast<uint32_t>(record->header));
}

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
        if (record && record->header && allocations[number].shape < m_tables.firstEnvironmentShape && belongsToClass(allocations[number].shape))
            *record = emptyAllocationSiteRecord();
    }
}

static Structure* structureOf(const AllocationSiteRecord& record)
{
    return std::bit_cast<StructureID>(static_cast<uint32_t>(record.header)).decode();
}

ALWAYS_INLINE AllocationSiteRecord& StructureTable::filledRecord(VM& vm, uint32_t number)
{
    AllocationSiteRecord* record = m_state.instance.objects.allocationSite(number);
    if (record && record->header) [[likely]]
        return *record;
    return fillAllocationSite(vm, number);
}

JSCell* StructureTable::allocate(VM& vm, uint32_t number)
{
    Structure* structure = structureOf(filledRecord(vm, number));
    const AllocationData& allocation = m_tables.allocations()[number];
    switch (allocatedCellKindOf(m_tables, allocation)) {
    case AllocatedCellKind::Object: {
        Object* object = Object::create(vm, structure, m_tables.layoutForShape(allocation.shape), allocation.size(), allocation.orderWord());
        object->setPerCellBit(allocation.isBornUnfinished);
        return object;
    }
    case AllocatedCellKind::Closure:
        return Closure::createForCompiledCode(vm, structure, allocation.size());
    case AllocatedCellKind::Environment:
        return Environment::create(vm, structure, m_tables.environmentForShape(allocation.shape), allocation.size());
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Compiled code goes on behind the operation as behind its own allocation: it writes every slot of the object that is not to hold zero before anybody else can see one.
JSCell* StructureTable::allocateAsCompiledCodeDoes(VM& vm, uint32_t number)
{
    static_assert(offsetOfOrderWord == sizeof(JSCell) && offsetOfShapeWord == offsetOfOrderWord + sizeof(uint32_t) && sizeof(JSCell) == sizeof(uint64_t));
    const AllocationData& allocation = m_tables.allocations()[number];
    if (allocation.shape >= m_tables.firstEnvironmentShape)
        return allocate(vm, number);
    const AllocationSiteRecord record = filledRecord(vm, number);
    Structure* structure = structureOf(record);
    auto* words = static_cast<uint64_t*>(spaceFor(vm, allocation, m_tables.layoutForShape(allocation.shape)).allocate(vm, allocation.size(), nullptr, AllocationFailureMode::Assert));
    words[0] = record.header;
    words[1] = record.orderAndShapeWords;
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(words + 2), allocation.size() - 2 * sizeof(uint64_t) });
    ensureStillAliveHere(structure);
    return reinterpret_cast<JSCell*>(words);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

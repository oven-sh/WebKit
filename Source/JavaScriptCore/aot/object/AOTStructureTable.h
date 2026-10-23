/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "AOTEnvironment.h"
#include "AOTObjectWords.h"
#include "AOTProgramTables.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "FreeList.h"
#include "LocalAllocator.h"
#include "RememberedSlot.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

struct State;

// The table and the records of the allocation sites name Structures and keep none of them alive through a full collection: the objects do that.
class StructureTable {
    WTF_MAKE_NONCOPYABLE(StructureTable);
public:
    explicit StructureTable(State&);
    ~StructureTable();

    Structure* structure(VM&, uint32_t shape);
    CompiledStructure* objectStructure(VM&, uint32_t shape);
    CompiledStructure* objectStructure(VM&, uint32_t shape, JSValue prototype);

    CompiledStructure* withPrototype(VM&, const CompiledStructure&, JSValue prototype);
    CompiledStructure* asPrototype(VM&, const CompiledStructure&);
    CompiledStructure* withExtraProperties(VM&, const CompiledStructure&, Structure* extraPropertiesStructureOrNull);
    CompiledStructure* existingWithExtraProperties(const CompiledStructure&, Structure& extraPropertiesStructure);
    CompiledStructure* holdingExtraProperties(VM&, const CompiledStructure&, JSObject* extraProperties);
    CompiledStructure* withOutOfLineKeyOrder(VM&, const CompiledStructure&, std::span<const uint16_t> entries);
    CompiledStructure* withNumberedKeyOrder(VM&, const CompiledStructure&);

    JSCell* allocate(VM&, uint32_t allocation);
    // An object has its header, its order word and its shape word, as behind the stub, and zeros behind them.
    JSCell* allocateAsCompiledCodeDoes(VM&, uint32_t allocation);
    AllocationSiteRecord& fillAllocationSite(VM&, uint32_t allocation);
    // A site that had no class object to take a prototype from predicts the first one that its operation named.
    void predictStructure(VM&, uint32_t allocation, CompiledStructure*);
    std::optional<StructureID> structureIDOf(uint32_t allocation) const;
    void forgetStructuresOf(const LayoutData& classLayout);

    template<typename Visitor> void visitRoots(Visitor&);
    size_t removeDeadEntries(VM&, CollectionScope);

    void dumpStructuresMade(PrintStream&) const;

private:
    struct Variant;
    struct VariantMaps;
    struct MadeStructures;

    static constexpr unsigned structuresRegionShift = 6;
    static constexpr unsigned recordsRegionShift = 8;

    // What compiled code takes for a LocalAllocator: it has no room and no next interval.
    struct AllocatorWithoutRoom {
        uint8_t bytesBeforeFreeList[LocalAllocator::offsetOfFreeList()];
        FreeList freeList { 0 };
    };

    AllocationSiteRecord emptyAllocationSiteRecord() { return { .header = 0, .orderAndShapeWords = 0, .allocator = &m_allocatorWithoutRoom, .unused = 0 }; }

    std::span<uint32_t> usualStructureIDs() const { return { m_structures, m_tables.firstEnvironmentShape }; }
    std::span<AllocationSiteRecord> allocationSiteRecords() const;

    CompiledStructure* usualStructureIfExists(uint32_t shape) const;
    Variant usualVariantOf(VM&, const LayoutData&);
    CompiledStructure* createUsual(VM&, const LayoutData&, const Variant&);
    CompiledStructure* create(VM&, const LayoutData&, const Variant&);
    CompiledStructure* find(const Variant&) const;
    CompiledStructure* findOrCreate(VM&, const Variant&);
    static Variant variantOf(const CompiledStructure&);

    template<typename Appender> void visitUsualStructures(Appender&, size_t begin, size_t end);
    template<typename Appender> void visitRecords(Appender&, size_t begin, size_t end);

    AllocationSiteRecord* recordOfSameAllocation(uint32_t allocation) const;
    AllocationSiteRecord& ensureRecord(uint32_t allocation);
    AllocationSiteRecord& filledRecord(VM&, uint32_t allocation);

    State& m_state;
    const ProgramTables& m_tables;
    EnvironmentShapes m_environmentShapes;
    uint32_t* m_structures { nullptr };
    uint32_t m_numberOfAllocationSiteRecords { 0 };
    AllocatorWithoutRoom m_allocatorWithoutRoom { };
    // No cell has them. Every other Structure of a compiled object starts as a copy of one: the second is for objects with another prototype than their layout's.
    std::array<CompiledStructure*, 2> m_templateStructures { };
    std::unique_ptr<VariantMaps> m_variants;
    // In a run with --aotCountsPath.
    std::unique_ptr<MadeStructures> m_made;
    Vector<StructureID> m_variantsMadeSinceCollection;
    RememberedSlotSet m_writtenStructures;
    RememberedSlotSet m_writtenRecords;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

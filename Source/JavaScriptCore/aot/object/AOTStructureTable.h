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
#include "FreeList.h"
#include "LocalAllocator.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

struct State;

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
    CompiledStructure* withOutOfLineKeyOrder(VM&, const CompiledStructure&, std::span<const uint16_t> entries);
    CompiledStructure* withNumberedKeyOrder(VM&, const CompiledStructure&);

    JSCell* allocate(VM&, uint32_t allocation);
    void fillAllocationSite(VM&, uint32_t allocation);
    // A site that had no class object to take a prototype from predicts the first one that its operation named.
    void predictStructure(VM&, uint32_t allocation, CompiledStructure*);
    std::optional<StructureID> structureIDOf(uint32_t allocation) const;
    void forgetStructuresOf(const LayoutData& classLayout);

private:
    struct Variant;
    struct VariantMaps;

    // What compiled code takes for a LocalAllocator: it has no room and no next interval.
    struct AllocatorWithoutRoom {
        uint8_t bytesBeforeFreeList[LocalAllocator::offsetOfFreeList()];
        FreeList freeList { 0 };
    };

    AllocationSiteRecord emptyAllocationSiteRecord() { return { .header = 0, .orderAndShapeWords = 0, .allocator = &m_allocatorWithoutRoom, .unused = 0 }; }

    CompiledStructure* find(const Variant&) const;
    CompiledStructure* findOrCreate(VM&, const Variant&);
    static Variant variantOf(const CompiledStructure&);

    State& m_state;
    const ProgramTables& m_tables;
    EnvironmentShapes m_environmentShapes;
    uint32_t* m_structures { nullptr };
    uint32_t m_numberOfAllocationSiteRecords { 0 };
    AllocatorWithoutRoom m_allocatorWithoutRoom { };
    std::unique_ptr<VariantMaps> m_variants;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

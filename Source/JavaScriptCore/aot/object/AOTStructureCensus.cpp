/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStructureCensus.h"

#if ENABLE(AOT)

#include "AOTCompiledStructure.h"
#include "DeferGCInlines.h"
#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "MarkedBlockInlines.h"
#include "MarkedSpaceInlines.h"
#include "PreventCollectionScope.h"
#include <algorithm>
#include <tuple>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/PrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

namespace {

enum class KeptBy : uint8_t { OwnCells, LaterStructure, SomethingElse };

struct StructureGroup {
    uint64_t structures { 0 };
    uint64_t bytes { 0 };
    uint64_t cells { 0 };
};

} // anonymous namespace

static ASCIILiteral nameOf(KeptBy keptBy)
{
    switch (keptBy) {
    case KeptBy::OwnCells:
        return "own-cells"_s;
    case KeptBy::LaterStructure:
        return "later-structure"_s;
    case KeptBy::SomethingElse:
        return "something-else"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral variantOf(Structure* structure)
{
    if (!structure->isCompiledStructure())
        return structure->isDictionary() ? "dictionary"_s : structure->mayBePrototype() ? "prototype"_s : "plain"_s;
    auto* compiled = uncheckedDowncast<CompiledStructure>(structure);
    if (!compiled->describesObject())
        return "compiled-other"_s;
    // The usual prototype of a layout is an object, so any other Structure without a prototype has the flag.
    if (compiled->storedPrototype().isNull() && !compiled->hasOtherPrototype())
        return "compiled-template"_s;
    if (compiled->heldExtraProperties())
        return "compiled-holding-extra-properties"_s;
    if (compiled->hasExtraProperties())
        return "compiled-with-extra-properties"_s;
    if (compiled->hasOutOfLineKeyOrder())
        return "compiled-with-key-order"_s;
    if (compiled->mayBePrototype())
        return "compiled-prototype"_s;
    return compiled->hasOtherPrototype() ? "compiled-other-prototype"_s : "compiled"_s;
}

void dumpStructureCensus(VM& vm, PrintStream& out)
{
    PreventCollectionScope preventCollection(vm.heap);
    DeferGCForAWhile deferGC(vm);
    UncheckedKeyHashMap<Structure*, uint64_t> cellsByStructure;
    Vector<std::pair<Structure*, size_t>> structures;
    {
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* heapCell, HeapCell::Kind kind) {
            if (!isJSCellKind(kind) || heapCell->isZapped())
                return IterationStatus::Continue;
            JSCell* cell = static_cast<JSCell*>(heapCell);
            ++cellsByStructure.add(cell->structure(), 0).iterator->value;
            if (cell->type() == StructureType)
                structures.append({ uncheckedDowncast<Structure>(cell), heapCell->cellSize() });
            return IterationStatus::Continue;
        });
    }

    UncheckedKeyHashSet<Structure*> earlierInChain;
    for (auto& [structure, size] : structures) {
        if (!cellsByStructure.contains(structure))
            continue;
        for (Structure* earlier = structure->previousID(); earlier && earlierInChain.add(earlier).isNewEntry; earlier = earlier->previousID()) { }
    }

    using Key = std::tuple<String, uint8_t, uint8_t, ASCIILiteral>;
    Vector<std::pair<Key, StructureGroup>> groups;
    for (auto& [structure, size] : structures) {
        uint64_t cells = cellsByStructure.get(structure);
        KeptBy keptBy = cells ? KeptBy::OwnCells : earlierInChain.contains(structure) ? KeptBy::LaterStructure : KeptBy::SomethingElse;
        Key key { String::fromLatin1(structure->classInfoForCells()->className.characters()), static_cast<uint8_t>(structure->transitionKind()), static_cast<uint8_t>(keptBy), variantOf(structure) };
        size_t index = groups.findIf([&](auto& group) {
            return group.first == key;
        });
        if (index == notFound) {
            index = groups.size();
            groups.append({ key, { } });
        }
        StructureGroup& group = groups[index].second;
        ++group.structures;
        group.bytes += size;
        group.cells += cells;
    }
    std::ranges::sort(groups, [](auto& a, auto& b) {
        return a.second.bytes > b.second.bytes;
    });
    for (auto& [key, group] : groups) {
        auto& [className, transition, keptBy, variant] = key;
        out.println("AOT live: structures class ", makeStringByReplacingAll(className, ' ', '_'), " variant ", variant, " made-by ", TransitionKindDump(static_cast<TransitionKind>(transition)), " kept-by ", nameOf(static_cast<KeptBy>(keptBy)),
            " structures ", group.structures, " bytes ", group.bytes, " cells ", group.cells);
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

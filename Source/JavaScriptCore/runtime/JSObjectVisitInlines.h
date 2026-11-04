/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003-2024 Apple Inc. All rights reserved.
 *  Copyright (C) 2007 Eric Seidel (eric@webkit.org)
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "ArrayStorage.h"
#include "ButterflyInlines.h"
#include "JSCellButterfly.h"
#include "JSObjectInlines.h"
#include "SparseArrayValueMap.h"
#include "StructureInlines.h"
#include <wtf/Atomics.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

// Compiled code makes an array and its first storage as one cell: the storage lies behind the cell of the array. Such storage is no allocation of its own:
// nobody marks it, and behind its vector there is no room that a size class left over.
// Storage with an allocation of its own comes from another space than its owner, so the two never share a block.
ALWAYS_INLINE bool isStorageInOwnCell(const JSObject* owner, const void* baseOfButterfly)
{
    uintptr_t cell = std::bit_cast<uintptr_t>(owner);
    uintptr_t base = std::bit_cast<uintptr_t>(baseOfButterfly);
    if (base == cell + sizeof(JSObjectWithButterfly))
        return true;
    return !((cell ^ base) & MarkedBlock::blockMask) && !owner->isPreciseAllocation();
}

template<typename Visitor>
ALWAYS_INLINE void JSObjectWithButterfly::markAuxiliaryAndVisitOutOfLineProperties(Visitor& visitor, Butterfly* butterfly, Structure* structure, PropertyOffset maxOffset)
{
    // We call this when we found everything without races.
    ASSERT(structure);
    
    if (!butterfly)
        return;

    if (isCopyOnWrite(structure->indexingMode())) {
        if (!JSCellButterfly::isOutsideHeap(butterfly))
            visitor.append(std::bit_cast<WriteBarrier<JSCell>>(JSCellButterfly::fromButterfly(butterfly)));
        return;
    }

    bool hasIndexingHeader = structure->hasIndexingHeader(this);
    size_t preCapacity;
    if (hasIndexingHeader)
        preCapacity = structure->hasElementsStartOffset() ? butterfly->indexingHeader()->elementsStartOffset(Structure::outOfLineCapacity(maxOffset)) : butterfly->preCapacity(structure);
    else
        preCapacity = 0;
    
    HeapCell* base = std::bit_cast<HeapCell*>(
        butterfly->base(preCapacity, Structure::outOfLineCapacity(maxOffset)));
    
    if (!isStorageInOwnCell(this, base)) [[likely]]
        visitor.markAuxiliary(base);
    
    unsigned outOfLineSize = Structure::outOfLineSize(maxOffset);
    visitor.appendValuesHidden(butterfly->propertyStorage() - outOfLineSize, outOfLineSize);
}

template<typename Visitor>
ALWAYS_INLINE Structure* JSObjectWithButterfly::visitButterfly(Visitor& visitor)
{
    static const char* const raceReason = "JSObjectWithButterfly::visitButterfly";
    Structure* result = visitButterflyImpl(visitor);
    if (!result)
        visitor.didRace(this, raceReason);
    return result;
}

template<typename Visitor>
ALWAYS_INLINE Structure* JSObjectWithButterfly::visitButterflyImpl(Visitor& visitor)
{
    Butterfly* butterfly;
    Structure* structure;
    PropertyOffset maxOffset;

    auto visitElements = [&] (IndexingType indexingMode) ALWAYS_INLINE_LAMBDA {
        switch (indexingMode) {
        // We don't need to visit the elements for CopyOnWrite butterflies since they we marked the JSCellButterfly acting as our butterfly.
        case ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES:
            visitor.appendValuesHidden(butterfly->contiguous().data(), butterfly->publicLength());
            break;
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            visitor.appendValuesHidden(butterfly->arrayStorage()->m_vector, butterfly->arrayStorage()->vectorLength());
            if (butterfly->arrayStorage()->m_sparseMap)
                visitor.append(butterfly->arrayStorage()->m_sparseMap);
            break;
        default:
            break;
        }
    };

    if (visitor.mutatorsAreStopped()) {
        butterfly = this->butterfly();
        structure = this->structure();
        maxOffset = structure->maxOffset();
        
        markAuxiliaryAndVisitOutOfLineProperties(visitor, butterfly, structure, maxOffset);
        visitElements(structure->indexingMode());

        return structure;
    }
    
    // We want to ensure that we only scan the butterfly if we have an exactly matched structure and an
    // exactly matched size. The mutator is required to perform the following shenanigans when
    // reallocating the butterfly with a concurrent collector, with all fencing necessary to ensure
    // that this executes as if under sequential consistency:
    //
    //     object->structure = nuke(object->structure)
    //     object->butterfly = newButterfly
    //     structure->m_offset = newMaxOffset
    //     object->structure = newStructure
    //
    // It's OK to skip this when reallocating the butterfly in a way that does not affect the m_offset.
    // We have other protocols in place for that.
    //
    // Note that the m_offset can change without the structure changing, but in that case the mutator
    // will still store null to the structure.
    //
    // The collector will ensure that it always sees a matched butterfly/structure by reading the
    // structure before and after reading the butterfly. For simplicity, let's first consider the case
    // where the only way to change the outOfLineCapacity is to change the structure. This works
    // because the mutator performs the following steps sequentially:
    //
    //     NukeStructure ChangeButterfly PutNewStructure
    //
    // Meanwhile the collector performs the following steps sequentially:
    //
    //     ReadStructureEarly ReadButterfly ReadStructureLate
    //
    // The collector is allowed to do any of these three things:
    //
    // BEFORE: Scan the object with the structure and butterfly *before* the mutator's transition.
    // AFTER: Scan the object with the structure and butterfly *after* the mutator's transition.
    // IGNORE: Ignore the butterfly and call didRace to schedule us to be revisted again in the future.
    //
    // In other words, the collector will never see any torn structure/butterfly mix. It will
    // always see the structure/butterfly before the transition or after but not in between.
    //
    // We can prove that this is correct by exhaustively considering all interleavings:
    //
    // NukeStructure ChangeButterfly PutNewStructure ReadStructureEarly ReadButterfly ReadStructureLate: AFTER, trivially.
    // NukeStructure ChangeButterfly ReadStructureEarly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because nuked structure read early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // NukeStructure ReadStructureEarly ChangeButterfly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because nuked structure read early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // NukeStructure ReadStructureEarly ReadButterfly ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // NukeStructure ReadStructureEarly ReadButterfly ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // NukeStructure ReadStructureEarly ReadButterfly ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read early
    // ReadStructureEarly NukeStructure ChangeButterfly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly NukeStructure ChangeButterfly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly NukeStructure ChangeButterfly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly NukeStructure ReadButterfly ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly NukeStructure ReadButterfly ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly NukeStructure ReadButterfly ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly ReadButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly ReadButterfly NukeStructure ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly ReadButterfly NukeStructure ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly ReadButterfly ReadStructureLate NukeStructure ChangeButterfly PutNewStructure: BEFORE, trivially.
    //
    // But we additionally have to worry about the size changing. We make this work by requiring that
    // the collector reads the size early and late as well. Lets consider the interleaving of the
    // mutator changing the size without changing the structure:
    //
    //     NukeStructure ChangeButterfly ChangeMaxOffset RestoreStructure
    //
    // Meanwhile the collector does:
    //
    //     ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate
    //
    // The collector can detect races by not only comparing the early structure to the late structure
    // (which will be the same before and after the algorithm runs) but also by comparing the early and
    // late maxOffsets. Note: the IGNORE proofs do not cite all of the reasons why the collector will
    // ignore the case, since we only need to identify one to say that we're in the ignore case.
    //
    // NukeStructure ChangeButterfly ChangeMaxOffset RestoreStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate: AFTER, trivially
    // NukeStructure ChangeButterfly ChangeMaxOffset ReadStructureEarly RestoreStructure ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ChangeMaxOffset ReadStructureEarly ReadMaxOffsetEarly RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ChangeMaxOffset ReadStructureEarly ReadMaxOffsetEarly ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ChangeMaxOffset ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ChangeMaxOffset ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ChangeMaxOffset RestoreStructure ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ChangeMaxOffset ReadMaxOffsetEarly RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ChangeMaxOffset RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ChangeButterfly ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ChangeMaxOffset RestoreStructure ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ChangeMaxOffset ReadMaxOffsetEarly RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ChangeMaxOffset RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ChangeButterfly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ChangeMaxOffset RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ChangeMaxOffset ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ChangeMaxOffset ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ChangeMaxOffset ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ReadButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ReadButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ReadButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ReadButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ReadButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ChangeButterfly ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ChangeButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeButterfly ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeButterfly ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeButterfly ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure early
    // NukeStructure ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeButterfly ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure early
    // ReadStructureEarly NukeStructure ChangeButterfly ChangeMaxOffset RestoreStructure ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate: AFTER, the ReadStructureEarly sees the same structure as after and everything else runs after.
    // ReadStructureEarly NukeStructure ChangeButterfly ChangeMaxOffset ReadMaxOffsetEarly RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: AFTER, as above and the ReadMaxOffsetEarly sees the maxOffset after.
    // ReadStructureEarly NukeStructure ChangeButterfly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: AFTER, as above and the ReadButterfly sees the right butterfly after.
    // ReadStructureEarly NukeStructure ChangeButterfly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure late
    // ReadStructureEarly NukeStructure ChangeButterfly ChangeMaxOffset ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ChangeMaxOffset RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ChangeMaxOffset ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ReadButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ChangeButterfly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ChangeMaxOffset RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ChangeMaxOffset ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ChangeMaxOffset ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ChangeMaxOffset ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ReadButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ReadButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ReadButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ReadButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ReadButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ChangeButterfly ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ChangeButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ChangeButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ChangeButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ChangeButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ChangeButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ChangeButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeButterfly ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeButterfly ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ReadStructureLate ChangeButterfly ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly NukeStructure ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeButterfly ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ChangeMaxOffset RestoreStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ChangeMaxOffset ReadButterfly RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ChangeMaxOffset ReadButterfly ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ChangeMaxOffset ReadButterfly ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ReadButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ReadButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ReadButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ReadButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ReadButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ChangeButterfly ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ChangeButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ChangeButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ChangeButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ChangeButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ChangeButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ChangeButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ReadStructureLate ChangeButterfly ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ReadStructureLate ChangeButterfly ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ReadStructureLate ChangeButterfly ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly NukeStructure ReadButterfly ReadStructureLate ReadMaxOffsetLate ChangeButterfly ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ChangeButterfly ChangeMaxOffset RestoreStructure ReadStructureLate ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ChangeButterfly ChangeMaxOffset ReadStructureLate RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ChangeButterfly ChangeMaxOffset ReadStructureLate ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ChangeButterfly ReadStructureLate ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ChangeButterfly ReadStructureLate ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ChangeButterfly ReadStructureLate ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ReadStructureLate ChangeButterfly ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ReadStructureLate ChangeButterfly ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ReadStructureLate ChangeButterfly ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly NukeStructure ReadStructureLate ReadMaxOffsetLate ChangeButterfly ChangeMaxOffset RestoreStructure: IGNORE, read nuked structure late
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate NukeStructure ChangeButterfly ChangeMaxOffset RestoreStructure ReadMaxOffsetLate: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate NukeStructure ChangeButterfly ChangeMaxOffset ReadMaxOffsetLate RestoreStructure: IGNORE, read different offsets
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate NukeStructure ChangeButterfly ReadMaxOffsetLate ChangeMaxOffset RestoreStructure: BEFORE, reads the offset before, everything else happens before
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate NukeStructure ReadMaxOffsetLate ChangeButterfly ChangeMaxOffset RestoreStructure: BEFORE, reads the offset before, everything else happens before
    // ReadStructureEarly ReadMaxOffsetEarly ReadButterfly ReadStructureLate ReadMaxOffsetLate NukeStructure ChangeButterfly ChangeMaxOffset RestoreStructure: BEFORE, trivially
    //
    // Whew.
    //
    // What the collector is doing is just the "double collect" snapshot from "The Unbounded Single-
    // Writer Algorithm" from Yehuda Afek et al's "Atomic Snapshots of Shared Memory" in JACM 1993,
    // also available here:
    //
    // http://people.csail.mit.edu/shanir/publications/AADGMS.pdf
    //
    // Unlike Afek et al's algorithm, ours does not require extra hacks to force wait-freedom (see
    // "Observation 2" in the paper). This simplifies the whole algorithm. Instead we are happy with
    // obstruction-freedom, and like any good obstruction-free algorithm, we ensure progress using
    // scheduling. We also only collect the butterfly once instead of twice; this optimization seems
    // to hold up in my proofs above and I'm not sure it's part of Afek et al's algos.
    //
    // For more background on this kind of madness, I like this paper; it's where I learned about
    // both the snapshot algorithm and obstruction-freedom:
    //
    // Lunchangco, Moir, Shavit. "Nonblocking k-compare-single-swap." SPAA '03
    // https://pdfs.semanticscholar.org/343f/7182cde7669ca2a7de3dc01127927f384ef7.pdf
    
    StructureID structureID = this->structureID();
    if (structureID.isNuked())
        return nullptr;
    structure = structureID.decode();
    maxOffset = structure->maxOffset();
    IndexingType indexingMode;
    Dependency indexingModeDependency = structure->fencedIndexingMode(indexingMode);
    Locker<JSCellLock> locker(NoLockingNecessary);
    switch (indexingMode) {
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        // We need to hold this lock to protect against changes to the innards of the butterfly
        // that can happen when the butterfly is used for array storage.
        // We do not need to hold this lock for contiguous butterflies. We do not reuse the existing
        // butterfly with contiguous shape for new array storage butterfly. When converting the butterfly
        // with contiguous shape to array storage, we always allocate a new one. Holding this lock for contiguous
        // butterflies is unnecessary since contiguous shaped butterfly never becomes broken state.
        locker = Locker { cellLock() };
        break;
    default:
        if (structure->hasElementsStartOffset()) [[unlikely]]
            locker = Locker { cellLock() };
        break;
    }
    Dependency butterflyDependency = indexingModeDependency.consume(this)->fencedButterfly(butterfly);
    if (!butterfly)
        return structure;
    if (butterflyDependency.consume(this)->structureID() != structureID)
        return nullptr;
    if (butterflyDependency.consume(structure)->maxOffset() != maxOffset)
        return nullptr;
    
    markAuxiliaryAndVisitOutOfLineProperties(visitor, butterfly, structure, maxOffset);
    ASSERT(indexingMode == structure->indexingMode());
    visitElements(indexingMode);
    
    return structure;
}

template<typename Visitor>
ALWAYS_INLINE void JSFinalObject::visitFields(JSFinalObject* thisObject, Visitor& visitor)
{
    if (Structure* structure = thisObject->visitButterfly(visitor)) {
        if (unsigned storageSize = structure->inlineSize())
            visitor.appendValuesHidden(thisObject->inlineStorage(), storageSize);
    }
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

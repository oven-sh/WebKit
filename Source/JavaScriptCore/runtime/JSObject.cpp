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

#include "config.h"
#include "JSObject.h"

#include "AllocationFailureMode.h"
#include "ArrayProfile.h"
#include "CustomGetterSetter.h"
#include "Exception.h"
#include "GCDeferralContextInlines.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "IndexingHeaderInlines.h"
#include "JSCInlines.h"
#include "IndexingTypeInlines.h"
#include "JSCellButterfly.h"
#include "JSCustomGetterFunction.h"
#include "JSCustomSetterFunction.h"
#include "JSFunction.h"
#include "Lookup.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "ProxyObject.h"
#include "ResourceExhaustion.h"
#include "TopExceptionScope.h"
#include "TypeError.h"
#include "VMInlines.h"
#include "VMTrapsInlines.h"
#include <wtf/Assertions.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {


// We keep track of the size of the last array after it was grown. We use this
// as a simple heuristic for as the value to grow the next array from size 0.
// This value is capped by the constant FIRST_VECTOR_GROW defined in
// ArrayConventions.h.
// UNGIL (AB-10 closure follow-on): GIL-off, N mutators grow ArrayStorage
// vectors concurrently (e.g. post-haveABadTime slow puts), racing this
// purely-advisory sizing hint. Relaxed atomic per the TSAN triage convention
// for advisory heuristics (TSAN-TRIAGE.md families 12-14/19): a stale value
// only perturbs the growth heuristic, never correctness.
static std::atomic<unsigned> lastArraySize { 0 };

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSObject);
STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSObjectWithButterfly);
STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSFinalObject);

const ASCIILiteral NonExtensibleObjectPropertyDefineError { "Attempting to define property on object that is not extensible."_s };
const ASCIILiteral ReadonlyPropertyWriteError { "Attempted to assign to readonly property."_s };
const ASCIILiteral ReadonlyPropertyChangeError { "Attempting to change value of a readonly property."_s };
const ASCIILiteral UnableToDeletePropertyError { "Unable to delete property."_s };
const ASCIILiteral UnconfigurablePropertyChangeAccessMechanismError { "Attempting to change access mechanism for an unconfigurable property."_s };
const ASCIILiteral UnconfigurablePropertyChangeConfigurabilityError { "Attempting to change configurable attribute of unconfigurable property."_s };
const ASCIILiteral UnconfigurablePropertyChangeEnumerabilityError { "Attempting to change enumerable attribute of unconfigurable property."_s };
const ASCIILiteral UnconfigurablePropertyChangeWritabilityError { "Attempting to change writable attribute of unconfigurable property."_s };
const ASCIILiteral PrototypeValueCanOnlyBeAnObjectOrNullTypeError { "Prototype value can only be an object or null"_s };

const ClassInfo JSObject::s_info = { "Object"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(JSObject) };

const ClassInfo JSObjectWithButterfly::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSObjectWithButterfly) };

const ClassInfo JSFinalObject::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSFinalObject) };

template<typename Visitor>
ALWAYS_INLINE void JSObjectWithButterfly::markAuxiliaryAndVisitOutOfLineProperties(Visitor& visitor, Butterfly* butterfly, Structure* structure, PropertyOffset maxOffset)
{
    // We call this when we found everything without races.
    ASSERT(structure);
    
    if (!butterfly)
        return;

    if (isCopyOnWrite(structure->indexingMode())) {
        visitor.append(std::bit_cast<WriteBarrier<JSCell>>(JSCellButterfly::fromButterfly(butterfly)));
        return;
    }

    bool hasIndexingHeader = structure->hasIndexingHeader(this);
    size_t preCapacity;
    if (hasIndexingHeader)
        preCapacity = butterfly->indexingHeader()->preCapacity(structure);
    else
        preCapacity = 0;
    
    HeapCell* base = std::bit_cast<HeapCell*>(
        butterfly->base(preCapacity, Structure::outOfLineCapacity(maxOffset)));
    
    ASSERT(Heap::heap(base) == visitor.heap());
    
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

#if USE(JSVALUE64)
    // Latched option (I22): load once per visit instead of three cross-DSO
    // global loads per visited object per mark pass.
    const bool jsThreads = Options::useJSThreads();
#endif

    auto visitElements = [&] (IndexingType indexingMode) {
        switch (indexingMode) {
        // We don't need to visit the elements for CopyOnWrite butterflies since they we marked the JSCellButterfly acting as our butterfly.
        case ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES: {
            unsigned visitBound = butterfly->publicLength();
#if USE(JSVALUE64)
            // SPEC-objectmodel review round 3 (I21/I25 - GC bound vs lock-free
            // truncation): on a SHARED-WRITTEN flat word (SW=1), a §3 dense
            // store may legally land in [newLength, vectorLength) while a
            // truncating plain setPublicLength (pop/setLength/shrink) is
            // mid-flight; bounding the value visit by publicLength would then
            // leave that store's cell unmarked FOREVER (the store's barrier
            // re-greys the object, but the revisit would use the same
            // too-small bound) - a sweep of a reachable value, resurfacing as
            // a dangling read once a later grow re-exposes the slot. Visit the
            // full storage bound instead: flag-on, every published flat
            // butterfly is hole-initialized through vectorLength (creation
            // hole-fills, T1/T2/conversion copies clear their slack, and
            // ObjectInitializationScope windows are thread-private and can
            // never be SW=1), so the extra slots hold holes or stale-but-valid
            // JSValues. SW monotonicity makes a re-loaded word's bit safe to
            // key on (over-visiting is conservative).
            if (jsThreads && butterflySharedWrite(taggedButterflyWord())) [[unlikely]]
                visitBound = butterfly->vectorLength();
#endif
            visitor.appendValuesHidden(butterfly->contiguous().data(), visitBound);
            break;
        }
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            visitor.appendValuesHidden(butterfly->arrayStorage()->m_vector, butterfly->arrayStorage()->vectorLength());
            if (butterfly->arrayStorage()->m_sparseMap)
                visitor.append(butterfly->arrayStorage()->m_sparseMap);
            break;
        default:
            break;
        }
    };

    if (visitor.mutatorIsStopped()) {
#if USE(JSVALUE64)
        // SPEC-objectmodel Task 2 audit: mask the §2 tag off the butterfly word.
        // Segmented words take the §4.5 visit (Task 6b). With the mutator
        // stopped nothing can race: the structureID is settled (transitions
        // are poll-free between nuke and restore, O2), so the visit must
        // succeed - a nullptr here would be a logic error, not a race.
        if (jsThreads) [[unlikely]] {
            uint64_t word = taggedButterflyWord();
            if (isSegmentedButterfly(word)) [[unlikely]] {
                // Review round 2: pass the settled {id, structure, maxOffset,
                // indexingMode} snapshot (the visit no longer self-loads it;
                // see the concurrent call site below). Settled because the
                // mutator is stopped and transitions are poll-free between
                // nuke and restore (O2).
                StructureID settledID = structureID();
                RELEASE_ASSERT(!settledID.isNuked());
                Structure* settledStructure = settledID.decode();
                Structure* result = visitSegmentedButterfly(visitor, this, butterflySpine(word), settledID, settledStructure, settledStructure->maxOffset(), settledStructure->indexingMode());
                RELEASE_ASSERT(result);
                return result;
            }
            butterfly = untaggedButterfly(word);
        } else
#endif
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
        break;
    }
    Dependency butterflyDependency = indexingModeDependency.consume(this)->fencedButterfly(butterfly);
    if (!butterfly)
        return structure;
    if (butterflyDependency.consume(this)->structureID() != structureID)
        return nullptr;
    if (butterflyDependency.consume(structure)->maxOffset() != maxOffset)
        return nullptr;
#if USE(JSVALUE64)
    // SPEC-objectmodel Task 2 audit: fencedButterfly loaded the TAGGED word
    // (§2); mask before dereferencing. The dependency chain is preserved: the
    // masked pointer is computed from the loaded value (M1). Segmented words ->
    // the §4.5 visit (Task 6b; review round 2): §4.5 step 2's
    // Dependency-ordered tagged-word load is the fencedButterfly above. The
    // visit RECEIVES our bracketed {early structureID, structure, maxOffset,
    // indexingMode} snapshot - the spine load above is dependency-ordered
    // after the early structureID load and the late re-checks above already
    // passed, so the pair is the same ReadStructureEarly/ReadButterfly/
    // ReadStructureLate bracket the flat path uses. The visit must NOT load
    // its own fresh structureID: a fresh load could pair a newer structure
    // (e.g. a §4.7 in-place relabel, which stops mutators but not concurrent
    // markers) with this older spine and value-visit raw double lanes as
    // JSValues. nullptr => didRace, surfaced exactly like the flat path.
    if (jsThreads) [[unlikely]] {
        uint64_t word = std::bit_cast<uint64_t>(butterfly);
        if (isSegmentedButterfly(word)) [[unlikely]]
            return visitSegmentedButterfly(visitor, this, butterflySpine(word), structureID, structure, maxOffset, indexingMode); // §4.5; nullptr => didRace.
        butterfly = untaggedButterfly(word);
    }
#endif

    markAuxiliaryAndVisitOutOfLineProperties(visitor, butterfly, structure, maxOffset);
    ASSERT(indexingMode == structure->indexingMode());
    visitElements(indexingMode);
    
    return structure;
}

size_t JSObject::estimatedSize(JSCell* cell, VM& vm)
{
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    size_t butterflyOutOfLineSize = thisObject->butterfly() ? thisObject->structure()->outOfLineSize() : 0;
    return Base::estimatedSize(cell, vm) + butterflyOutOfLineSize;
}

#if USE(JSVALUE64)

// ===== SPEC-objectmodel Task 2: flag-on regime-dispatching slow paths =====
//
// These implement the §Q dispatch for locationForOffset and the quickly-family.
// They are reached only when Options::useJSThreads() is on (I22) and are the
// E5 rule in action: interpreter/runtime slow paths never rely on elision and
// always dispatch on the loaded tagged word. The full §2-dispatch *Concurrent
// accessors of §9.5 (get/putDirectConcurrent, get/putIndexConcurrent — which
// additionally provide M5 tryDecode nuke tolerance and drive the §4.3/N2
// transition protocols) land with Task 6.

const WriteBarrierBase<Unknown>* JSObject::locationForOutOfLineOffsetConcurrent(PropertyOffset offset) const
{
    ASSERT(Options::useJSThreads());
    ASSERT(isOutOfLineOffset(offset));
    // M7(d): the caller's structureID load (the offset's provenance) must be
    // ordered before the tagged-word load, else arm64 load-load reordering can
    // pair a new structure's offset with a stale, smaller butterfly/spine
    // (I24). A loadLoadFence is the only conforming option when the structure
    // load lives in caller code (§Q). x86-64: compiler-only barrier.
    WTF::loadLoadFence();
    uint64_t word = taggedButterflyWord();
    while (true) {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            ButterflySpine* spine = butterflySpine(word);
            // I33 out-of-line clause: bound by 4 * spine->outOfLineFragmentCount.
            // Out-of-range = the loaded spine is stale (superseded by a grown
            // one) => acquire-re-load the tagged word and re-dispatch.
            if (const WriteBarrierBase<Unknown>* slot = segmentedOutOfLineSlotIfWithinBounds(spine, offset))
                return slot;
            WTF::loadLoadFence();
            word = taggedButterflyWord();
            continue;
        }
        // Flat (any TID, any SW): mask and index exactly as today. Soundness of
        // pairing the caller's structure with this possibly newer flat
        // butterfly: every offset-map-changing transition publishes the
        // butterfly atomically WITH the structure (locked DCAS, §4.2-5/§4.3-5)
        // or BEFORE it (E4 nuke order; M5), and live storage never shrinks
        // (deletes quarantine slots, I18/I30) — so a butterfly loaded after the
        // structureID always satisfies that structure's storage requirements
        // (history §15.4).
        return &untaggedButterfly(word)->propertyStorage()[offsetInOutOfLineStorage(offset)];
    }
}

// TSAN wave 4 (triage §3.10 / §8.10 jsvalue-slots residual): typed-array arm
// of the concurrent quickly accessors. JSArrayBufferView::m_length and the
// element words are intentionally-racy data words under shared-heap threading
// (SPEC-objectmodel ground truth; SAB-granularity tolerance): a foreign
// thread's trySetIndexQuicklyConcurrent element store (or a racing detach
// zeroing m_length) pairs with this thread's plain loads inside
// canGetIndexQuicklyForTypedArray / getIndexQuicklyForTypedArray, which is
// UB. Route the length read and the element load/store through relaxed
// WTF::Atomic word accesses — the updateEncodedJSValueConcurrent analog for
// non-JSValue words. These helpers are reachable only from the *Concurrent
// accessors (which assert Options::useJSThreads()), so flag-off behavior and
// codegen are untouched. Resizable/growable/auto-length views
// (!canUseRawFieldsDirectly) conservatively report "not quickly": their
// bounds derive from multi-word ArrayBuffer state that cannot be snapshotted
// with a single relaxed load; callers fall to their generic paths.

static ALWAYS_INLINE size_t typedArrayLengthRawConcurrent(const JSArrayBufferView* view)
{
    return std::bit_cast<const WTF::Atomic<size_t>*>(std::bit_cast<const uint8_t*>(view) + JSArrayBufferView::offsetOfLength())->loadRelaxed();
}

// Sized integer alias so the element access compiles to a single relaxed
// atomic word op for every adaptor type (including double and Float16,
// where Atomic<Type> itself would be exotic).
template<typename Type>
using TypedArrayElementRawWord = std::conditional_t<sizeof(Type) == 1, uint8_t,
    std::conditional_t<sizeof(Type) == 2, uint16_t,
    std::conditional_t<sizeof(Type) == 4, uint32_t, uint64_t>>>;

template<typename Adaptor>
static ALWAYS_INLINE bool canGetIndexQuicklyForTypedArrayViewConcurrent(const JSGenericTypedArrayView<Adaptor>* view, unsigned i)
{
    if (!Adaptor::canConvertToJSQuickly)
        return false;
    if (!view->canUseRawFieldsDirectly()) [[unlikely]]
        return false;
    return i < typedArrayLengthRawConcurrent(view);
}

template<typename Adaptor>
static ALWAYS_INLINE JSValue tryGetIndexQuicklyForTypedArrayViewConcurrent(const JSGenericTypedArrayView<Adaptor>* view, unsigned i)
{
    using Type = typename Adaptor::Type;
    using RawWord = TypedArrayElementRawWord<Type>;
    static_assert(sizeof(RawWord) == sizeof(Type));
    if (!canGetIndexQuicklyForTypedArrayViewConcurrent(view, i))
        return JSValue();
    RawWord raw = std::bit_cast<const WTF::Atomic<RawWord>*>(view->typedVector() + i)->loadRelaxed();
    return Adaptor::toJSValue(nullptr, std::bit_cast<Type>(raw));
}

template<typename Adaptor>
static ALWAYS_INLINE bool trySetIndexQuicklyForTypedArrayViewConcurrent(JSGenericTypedArrayView<Adaptor>* view, unsigned i, JSValue value)
{
    using Type = typename Adaptor::Type;
    using RawWord = TypedArrayElementRawWord<Type>;
    static_assert(sizeof(RawWord) == sizeof(Type));
    if (!value.isNumber())
        return false;
    // canSetIndexQuickly == canGetIndexQuickly + isNumber for the quickly family.
    if (!canGetIndexQuicklyForTypedArrayViewConcurrent(view, i))
        return false;
    // toNativeFromValue on a number is pure (no JS, no allocation).
    Type native = toNativeFromValue<Adaptor>(value);
    std::bit_cast<WTF::Atomic<RawWord>*>(view->typedVector() + i)->storeRelaxed(std::bit_cast<RawWord>(native));
    return true;
}

static ALWAYS_INLINE bool canGetIndexQuicklyForTypedArrayConcurrent(const JSObject* object, unsigned i)
{
    switch (object->type()) {
#define CASE_TYPED_ARRAY_TYPE(name) \
    case name ## ArrayType: \
        return canGetIndexQuicklyForTypedArrayViewConcurrent(uncheckedDowncast<JS ## name ## Array>(object), i);
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(CASE_TYPED_ARRAY_TYPE)
#undef CASE_TYPED_ARRAY_TYPE
    default:
        return false;
    }
}

static ALWAYS_INLINE JSValue tryGetIndexQuicklyForTypedArrayConcurrent(const JSObject* object, unsigned i, ArrayProfile* arrayProfile)
{
#if USE(LARGE_TYPED_ARRAYS)
    if (i > ArrayProfile::s_smallTypedArrayMaxLength && arrayProfile)
        arrayProfile->setMayBeLargeTypedArray();
#else
    UNUSED_PARAM(arrayProfile);
#endif
    switch (object->type()) {
#define CASE_TYPED_ARRAY_TYPE(name) \
    case name ## ArrayType: \
        return tryGetIndexQuicklyForTypedArrayViewConcurrent(uncheckedDowncast<JS ## name ## Array>(object), i);
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(CASE_TYPED_ARRAY_TYPE)
#undef CASE_TYPED_ARRAY_TYPE
    default:
        return JSValue();
    }
}

static ALWAYS_INLINE bool trySetIndexQuicklyForTypedArrayConcurrent(JSObject* object, unsigned i, JSValue value, ArrayProfile* arrayProfile)
{
    bool result;
    switch (object->type()) {
#define CASE_TYPED_ARRAY_TYPE(name) \
    case name ## ArrayType: \
        result = trySetIndexQuicklyForTypedArrayViewConcurrent(uncheckedDowncast<JS ## name ## Array>(object), i, value); \
        break;
    FOR_EACH_TYPED_ARRAY_TYPE_EXCLUDING_DATA_VIEW(CASE_TYPED_ARRAY_TYPE)
#undef CASE_TYPED_ARRAY_TYPE
    default:
        return false;
    }
    if (!result)
        return false;
#if USE(LARGE_TYPED_ARRAYS)
    if (i > ArrayProfile::s_smallTypedArrayMaxLength && arrayProfile)
        arrayProfile->setMayBeLargeTypedArray();
#else
    UNUSED_PARAM(arrayProfile);
#endif
    return true;
}

bool JSObject::canGetIndexQuicklyConcurrent(unsigned i) const
{
    ASSERT(Options::useJSThreads());
    // E5 "None first" (review round 4): the word is loaded BEFORE the indexing
    // byte in program order, and the N3 first indexed-storage install is
    // lock-free (no stop) - so a stale word==0 can pair with a FRESH indexed
    // type here. Every flat dense branch below must therefore null-check the
    // payload of the SAME loaded word and report "not quickly" (callers take
    // their generic path, which re-dispatches on fresh state). Dereferencing
    // untaggedButterfly(0) would read around address 0 - a crash from a race.
    uint64_t word = taggedButterflyWord();
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        return canGetIndexQuicklyForTypedArrayConcurrent(this, i); // §3.10: relaxed m_length read
    case ALL_UNDECIDED_INDEXING_TYPES:
        return false;
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            const WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfReadable(butterflySpine(word), i); // C4
            return slot && !!slot->get();
        }
        const Butterfly* butterfly = untaggedButterfly(word);
        if (!butterfly) [[unlikely]]
            return false; // E5 None-first: racing N3 first install (round 4).
        return i < butterfly->vectorLength() && butterfly->contiguous().at(this, i);
    }
    case ALL_DOUBLE_INDEXING_TYPES: {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            // §4.7: Double fragments hold RAW doubles (shape-keyed interpretation;
            // aligned 8B slots are tear-free at SAB granularity; holes = PNaN).
            const WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfReadable(butterflySpine(word), i); // C4
            if (!slot)
                return false;
            double value = *std::bit_cast<const double*>(slot);
            return value == value;
        }
        const Butterfly* butterfly = untaggedButterfly(word);
        if (!butterfly) [[unlikely]]
            return false; // E5 None-first (round 4).
        if (i >= butterfly->vectorLength())
            return false;
        double value = butterfly->contiguousDouble().at(this, i);
        return value == value;
    }
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        // §Q/I31: flag-on, AS-shape quickly probes answer false so callers fall
        // to their generic paths (E5 dispatch = §4.6 cell-locked access).
        return false;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
}

JSValue JSObject::getIndexQuicklyConcurrent(unsigned i) const
{
    ASSERT(Options::useJSThreads());
    uint64_t word = taggedButterflyWord();
    switch (indexingType()) {
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        JSValue value;
        if (isSegmentedButterfly(word)) [[unlikely]] {
            const WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfReadable(butterflySpine(word), i); // C4
            value = slot ? slot->get() : JSValue();
        } else {
            const Butterfly* butterfly = untaggedButterfly(word);
            // E5 None-first + C4-style bound on the SAME loaded word (round 4):
            // a stale word (racing N3 install / older flat snapshot than the
            // caller's bound check) reads as a hole, never a wild deref.
            if (!butterfly || i >= butterfly->vectorLength()) [[unlikely]]
                return jsUndefined();
            value = butterfly->contiguous().at(this, i).get();
        }
        // Race tolerance (I21/SAB semantics): a hole surfacing under a racing
        // shrink/delete reads as undefined rather than an empty JSValue.
        if (!value) [[unlikely]]
            return jsUndefined();
        if (hasInt32(indexingType()))
            return jsNumber(value.asInt32());
        return value;
    }
    case ALL_DOUBLE_INDEXING_TYPES: {
        double value;
        if (isSegmentedButterfly(word)) [[unlikely]] {
            const WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfReadable(butterflySpine(word), i); // C4
            if (!slot) [[unlikely]]
                return jsUndefined();
            value = *std::bit_cast<const double*>(slot); // §4.7 raw double
        } else {
            const Butterfly* butterfly = untaggedButterfly(word);
            if (!butterfly || i >= butterfly->vectorLength()) [[unlikely]] // E5 None-first + snapshot bound (round 4)
                return jsUndefined();
            value = butterfly->contiguousDouble().at(this, i);
        }
        if (value != value) [[unlikely]]
            return jsUndefined(); // hole (PNaN) surfaced under a race
        return JSValue(JSValue::EncodeAsDouble, value);
    }
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        // I31: flag-on, EVERY runtime AS access is cell-locked (AS never
        // segments). Re-load the word under the lock: AS-COPY (§4.6) may have
        // republished a fresh AS butterfly.
        Locker locker { cellLock() };
        const Butterfly* butterfly = untaggedButterfly(taggedButterflyWord());
        const ArrayStorage* storage = butterfly->arrayStorage();
        if (i >= storage->vectorLength()) [[unlikely]]
            return jsUndefined();
        JSValue value = storage->m_vector[i].get();
        return value ? value : jsUndefined();
    }
    case ALL_BLANK_INDEXING_TYPES: {
        // §3.10: relaxed length + element reads. A racing detach/length change
        // between the caller's canGetIndexQuicklyConcurrent and this read
        // surfaces as undefined — same race tolerance as the dense arms above —
        // instead of tripping getIndexQuicklyForTypedArray's RELEASE_ASSERT.
        JSValue result = tryGetIndexQuicklyForTypedArrayConcurrent(this, i, nullptr);
        return result ? result : jsUndefined();
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return JSValue();
    }
}

JSValue JSObject::tryGetIndexQuicklyConcurrent(unsigned i, ArrayProfile* arrayProfile) const
{
    ASSERT(Options::useJSThreads());
    uint64_t word = taggedButterflyWord();
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        // §3.10: relaxed length + element reads (single length snapshot — no
        // can/get TOCTOU against a racing detach).
        if (JSValue result = tryGetIndexQuicklyForTypedArrayConcurrent(this, i, arrayProfile))
            return result;
        break;
    case ALL_UNDECIDED_INDEXING_TYPES:
        break;
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            const WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfReadable(butterflySpine(word), i); // C4
            if (!slot)
                break;
            JSValue result = slot->get();
            ASSERT(!hasInt32(indexingType()) || result.isInt32() || !result);
            return result; // empty => caller's generic path
        }
        const Butterfly* butterfly = untaggedButterfly(word);
        if (!butterfly) [[unlikely]]
            break; // E5 None-first: racing N3 first install (round 4) => generic path.
        // Bound by vectorLength too (round 4): on a flat word that a racing
        // §4.2 conversion + T2 grow superseded, the aliased publicLength slot
        // can race past THIS snapshot's storage.
        if (i < butterfly->publicLength() && i < butterfly->vectorLength()) {
            JSValue result = butterfly->contiguous().at(this, i).get();
            ASSERT(!hasInt32(indexingType()) || result.isInt32() || !result);
            return result;
        }
        break;
    }
    case ALL_DOUBLE_INDEXING_TYPES: {
        double result;
        if (isSegmentedButterfly(word)) [[unlikely]] {
            const WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfReadable(butterflySpine(word), i); // C4
            if (!slot)
                break;
            result = WTF::atomicLoad(std::bit_cast<double*>(const_cast<WriteBarrierBase<Unknown>*>(slot)), std::memory_order_relaxed); // §4.7 raw double; relaxed atomic (intentionally racy JS value word)
        } else {
            const Butterfly* butterfly = untaggedButterfly(word);
            if (!butterfly) [[unlikely]]
                break; // E5 None-first (round 4).
            if (i >= butterfly->publicLength() || i >= butterfly->vectorLength()) // round 4: snapshot bound (aliased publicLength can race past it)
                break;
            result = WTF::atomicLoad(const_cast<double*>(&butterfly->contiguousDouble().at(this, i).m_data), std::memory_order_relaxed); // relaxed atomic (intentionally racy JS value word)
        }
        if (result != result)
            break;
        return JSValue(JSValue::EncodeAsDouble, result);
    }
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        // §Q/I31: not-quickly; callers fall to the generic (§4.6 locked) path.
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    return JSValue();
}

// Review round 2: dense-store publicLength update for FLAT words. On a SHARED
// word (SW=1) the legacy read-then-plain-store can regress publicLength under
// racing growers - T1 stores a[8]/len=9, T0 (stale len read) stores a[5]/len=6
// - hiding T1's element behind the min(publicLength, vectorLength) read bound
// (I21 "no lost properties"; i03-t5-racing-growers part (a)). Shared words
// therefore CAS-max; owner-exclusive (t, 0) words keep today's plain store
// (only the owner can be in a dense grow while SW=0: a foreign grower flips SW
// first, §3 F1). Segmented words use ButterflySpine::bumpPublicLengthToAtLeast
// at their own sites.
// AB17f (I21 publication ordering): the CAS-max bump is now a RELEASE, so the
// element store above each call is published no later than the length; the
// reader-side acquire gap (ARM64) is the KNOWN RESIDUAL recorded at
// Butterfly::bumpPublicLengthToAtLeast. The owner-exclusive (SW=0) plain
// setPublicLength arm is inside the same residual: foreign READERS of an SW=0
// word get SAB-granularity staleness at worst (spurious hole => generic path).
static ALWAYS_INLINE void updatePublicLengthAfterDenseStoreConcurrent(uint64_t word, Butterfly* butterfly, unsigned i)
{
    if (i < butterfly->publicLength()) [[likely]]
        return;
    if (butterflySharedWrite(word))
        butterfly->bumpPublicLengthToAtLeast(i + 1);
    else
        butterfly->setPublicLength(i + 1);
}

bool JSObject::trySetIndexQuicklyConcurrent(VM& vm, unsigned i, JSValue v, ArrayProfile* arrayProfile)
{
    ASSERT(Options::useJSThreads());
    // §4.8/I35 (cve fix): classify the MODE before loading the WORD, with a
    // load-load fence between them. The §4.8 materializer publishes
    // {writable header, fresh word} as one seq_cst DCAS (PA flavor: the word
    // CAS precedes the new header bytes), so a WRITABLE mode observed here
    // guarantees the fenced word load below is NOT the superseded CoW word.
    // The old order (word first, indexingMode() read at the switch) let a
    // racing materialization pair the STALE CoW word with the fresh writable
    // mode: the dense branches then stored straight into the shared
    // JSImmutableButterfly payload (sibling-visible mutation; I35/I21,
    // mc-lock-cow-materialize-race pass-1 oracle).
    IndexingType mode = indexingMode();
    if (isCopyOnWrite(mode)) [[unlikely]]
        return false; // §4.8 materialization belongs to the caller's generic path (putByIndex / convertFromCopyOnWrite).
    WTF::loadLoadFence();
    uint64_t word = taggedButterflyWord();
    // §3 F1 (review round 1; Task 7's ensureSharedWriteBit is landed): a
    // foreign write to an SW=0 FLAT word must fire writeThreadLocal and flip
    // SW BEFORE any plain store lands - otherwise the owner's T1 copying
    // resize still sees (t,0), its CAS succeeds against a payload that
    // received our store mid-copy, and the store is silently dropped (I21);
    // it would also break I12 (writeThreadLocal valid <=> no foreign write
    // ever), unsounding the watchpoint-elision argument. Scope: exactly the
    // flat dense-write branches below (Int32/Double/Contiguous, incl. their
    // publicLength bumps). CoW is excluded (WRITABLE modes never match CoW;
    // the default leg returns false to the caller's §4.8 path); AS and
    // Undecided return false below; typed-array (BLANK) stores do not touch
    // the butterfly. Segmented and SW=1 words need no flip (I3/I4).
    if ((word & butterflyPointerMask) && !isSegmentedButterfly(word)
        && !butterflySharedWrite(word) && butterflyWriterIsForeign(word) // incl. §9.6 forceButterflySWBit
        && (hasInt32(mode) || hasDouble(mode) || hasContiguous(mode))) [[unlikely]] {
        ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this));
        mode = indexingMode(); // A racing STW relabel may advance the shape while we are parked in the stop; CoW cannot reappear (monotone exit, §4.8).
        WTF::loadLoadFence(); // Same mode-before-word ordering as above.
        word = taggedButterflyWord(); // Re-dispatch on the fresh tag (SW=1 flat or segmented now).
    }
    switch (mode) {
    case ALL_BLANK_INDEXING_TYPES:
        return trySetIndexQuicklyForTypedArrayConcurrent(this, i, v, arrayProfile); // §3.10: relaxed length read + element store
    case ALL_UNDECIDED_INDEXING_TYPES:
        return false;
    case ALL_WRITABLE_INT32_INDEXING_TYPES: {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            if (!v.isInt32())
                return false; // shape transition => generic path (§4.3/§4.7, Tasks 6-8)
            WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfWithinVectorLength(butterflySpine(word), i);
            if (!slot)
                return false;
            slot->set(vm, this, v); // §4.5: fragment-slot stores use WriteBarrierBase::set on the owner
            // Review round 2: segmented words are shared by definition, so the
            // length bump must be a CAS-max - a plain read-then-store loses
            // racing growers' elements (I21; i03-t5 part (a)).
            butterflySpine(word)->bumpPublicLengthToAtLeast(i + 1);
            return true;
        }
        Butterfly* butterfly = untaggedButterfly(word);
        if (!butterfly) [[unlikely]]
            return false; // E5 None-first: racing N3 first install (round 4) => generic path.
        if (i >= butterfly->vectorLength())
            return false;
        if (!v.isInt32()) {
            convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(vm, i, v);
            return true;
        }
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(v);
        updatePublicLengthAfterDenseStoreConcurrent(word, butterfly, i);
        vm.writeBarrier(this, v);
        return true;
    }
    case ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES: {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfWithinVectorLength(butterflySpine(word), i);
            if (!slot)
                return false;
            slot->set(vm, this, v);
            butterflySpine(word)->bumpPublicLengthToAtLeast(i + 1); // Review round 2: CAS-max (shared word).
            return true;
        }
        Butterfly* butterfly = untaggedButterfly(word);
        if (!butterfly) [[unlikely]]
            return false; // E5 None-first (round 4).
        if (i >= butterfly->vectorLength())
            return false;
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(v);
        updatePublicLengthAfterDenseStoreConcurrent(word, butterfly, i);
        vm.writeBarrier(this, v);
        return true;
    }
    case ALL_WRITABLE_DOUBLE_INDEXING_TYPES: {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            if (!v.isNumber())
                return false; // shape transition => generic path (§4.7, Tasks 6-8)
            double value = v.asNumber();
            if (value != value)
                return false;
            WriteBarrierBase<Unknown>* slot = segmentedIndexedSlotIfWithinVectorLength(butterflySpine(word), i);
            if (!slot)
                return false;
            WTF::atomicStore(std::bit_cast<double*>(slot), value, std::memory_order_relaxed); // §4.7 raw double; no barrier; relaxed atomic (intentionally racy JS value word)
            butterflySpine(word)->bumpPublicLengthToAtLeast(i + 1); // Review round 2: CAS-max (shared word).
            return true;
        }
        Butterfly* butterfly = untaggedButterfly(word);
        if (!butterfly) [[unlikely]]
            return false; // E5 None-first (round 4).
        if (i >= butterfly->vectorLength())
            return false;
        if (!v.isNumber()) {
            convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
            return true;
        }
        double value = v.asNumber();
        if (value != value) {
            convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
            return true;
        }
        WTF::atomicStore(&butterfly->contiguousDouble().at(this, i).m_data, value, std::memory_order_relaxed); // relaxed atomic (intentionally racy JS value word)
        updatePublicLengthAfterDenseStoreConcurrent(word, butterfly, i);
        return true;
    }
    case NonArrayWithArrayStorage:
    case ArrayWithArrayStorage:
    case NonArrayWithSlowPutArrayStorage:
    case ArrayWithSlowPutArrayStorage:
        // §Q/I31: flag-on, AS writes are not "quickly"; callers fall to the
        // generic path (§4.6 cell-locked, Task 8).
        return false;
    default:
        // CoW modes returned false before the word load (mode saved and
        // classified up top; switch is on the saved mode), and every non-CoW
        // indexing mode is enumerated above — this leg is unreachable. Do NOT
        // re-read indexingMode() here: a fresh read would be a stale-state
        // TOCTOU re-check that is guaranteed false after the early return.
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
}

void JSObject::setIndexQuicklyConcurrent(VM& vm, unsigned i, JSValue v)
{
    ASSERT(Options::useJSThreads());
    ASSERT(!isCopyOnWrite(indexingMode()));
    uint64_t word = taggedButterflyWord();
    // §3 F1 (review round 1): same foreign-first-write rule as
    // trySetIndexQuicklyConcurrent above, covering BOTH the flat dense-write
    // branches and the AS branch (§4.6: ensureSharedWriteBit runs the AS
    // per-event stop and republishes (installerTID, 1) FLAT; it must be called
    // with no lock held - GT11 - hence before the Locker below).
    if ((word & butterflyPointerMask) && !isSegmentedButterfly(word)
        && !butterflySharedWrite(word) && butterflyWriterIsForeign(word) // incl. §9.6 forceButterflySWBit
        && (hasInt32(indexingType()) || hasDouble(indexingType()) || hasContiguous(indexingType())
            || hasAnyArrayStorage(indexingType()))) [[unlikely]] {
        ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(this));
        word = taggedButterflyWord(); // Fresh tag (SW=1 flat or segmented now).
    }
    switch (indexingType()) {
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES:
    case ALL_DOUBLE_INDEXING_TYPES: {
        if (isSegmentedButterfly(word)) [[unlikely]] {
            // Same-shape in-bounds stores succeed via the trySet path.
            if (trySetIndexQuicklyConcurrent(vm, i, v, nullptr))
                return;
            // Shape transition on a segmented object (review round 1): the
            // relabel runs as a per-event STW (I28/§4.7 - see
            // relabelIndexingShapeConcurrent), then the store lands via the
            // fresh dispatch inside the WhilePerformingSetIndex helper.
            if (hasInt32(indexingType()) && !v.isInt32()) {
                convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(vm, i, v);
                return;
            }
            if (hasDouble(indexingType()) && (!v.isNumber() || v.asNumber() != v.asNumber())) {
                convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
                return;
            }
            // In-bounds same-shape stores cannot fail: reaching here means the
            // caller violated setIndexQuickly's bounds contract on a segmented
            // word.
            RELEASE_ASSERT_NOT_REACHED();
            return;
        }
        Butterfly* butterfly = untaggedButterfly(word);
        // Round 4 note (E5 None-first audit): unlike the can*/try* probes,
        // this word CANNOT be null - setIndexQuickly's contract requires a
        // same-thread canSetIndexQuickly that dispatched on a non-null word,
        // butterfly words never return to null, and same-address loads on one
        // thread respect coherence order. Asserted, not branched.
        ASSERT(butterfly);
        if (hasInt32(indexingType())) {
            ASSERT(i < butterfly->vectorLength());
            if (!v.isInt32()) {
                convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(vm, i, v);
                return;
            }
            butterfly->contiguous().at(this, i).setWithoutWriteBarrier(v);
            updatePublicLengthAfterDenseStoreConcurrent(word, butterfly, i);
            vm.writeBarrier(this, v);
            return;
        }
        if (hasDouble(indexingType())) {
            ASSERT(i < butterfly->vectorLength());
            if (!v.isNumber()) {
                convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
                return;
            }
            double value = v.asNumber();
            if (value != value) {
                convertDoubleToContiguousWhilePerformingSetIndex(vm, i, v);
                return;
            }
            WTF::atomicStore(&butterfly->contiguousDouble().at(this, i).m_data, value, std::memory_order_relaxed); // relaxed atomic (intentionally racy JS value word)
            updatePublicLengthAfterDenseStoreConcurrent(word, butterfly, i);
            return;
        }
        ASSERT(i < butterfly->vectorLength());
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(v);
        updatePublicLengthAfterDenseStoreConcurrent(word, butterfly, i);
        vm.writeBarrier(this, v);
        return;
    }
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        // I31: flag-on, every runtime AS access is cell-locked.
        Locker locker { cellLock() };
        setIndexQuicklyForArrayStorageIndexingType(vm, i, v);
        return;
    }
    case ALL_BLANK_INDEXING_TYPES:
        // §3.10: relaxed length read + element store (same racy words as the
        // tryGet/trySet pair above, reached via this entry point). A failed
        // store means a racing detach/shrink made the index out-of-bounds —
        // per typed-array semantics that store is a no-op, so tolerate it
        // rather than re-running setIndexQuicklyForTypedArray's plain-word
        // RELEASE_ASSERT under a blessed race. Caller misuse (non-number v)
        // still trips the assert below, matching the old precondition.
        ASSERT(v.isNumber());
        trySetIndexQuicklyForTypedArrayConcurrent(this, i, v, nullptr);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

#endif // USE(JSVALUE64)

template<typename Visitor>
void JSObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    typename Visitor::DefaultMarkingViolationAssertionScope assertionScope(visitor);

    JSCell::visitChildren(thisObject, visitor);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSObject);

template<typename Visitor>
void JSObjectWithButterfly::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSObjectWithButterfly* thisObject = uncheckedDowncast<JSObjectWithButterfly>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    typename Visitor::DefaultMarkingViolationAssertionScope assertionScope(visitor);

    JSCell::visitChildren(thisObject, visitor);

    thisObject->visitButterfly(visitor);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSObjectWithButterfly);

void JSObject::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    Base::analyzeHeap(cell, analyzer);

    Structure* structure = thisObject->structure();
    for (const auto& entry : structure->getPropertiesConcurrently()) {
        JSValue toValue = thisObject->getDirect(entry.offset());
        if (toValue && toValue.isCell())
            analyzer.analyzePropertyNameEdge(thisObject, toValue.asCell(), entry.key());
    }

    Butterfly* butterfly = thisObject->butterfly();
    if (butterfly) {
        WriteBarrier<Unknown>* data = nullptr;
        uint32_t count = 0;

        switch (thisObject->indexingType()) {
        case ALL_CONTIGUOUS_INDEXING_TYPES:
            data = butterfly->contiguous().data();
            count = butterfly->publicLength();
            break;
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            data = butterfly->arrayStorage()->m_vector;
            count = butterfly->arrayStorage()->vectorLength();
            break;
        default:
            break;
        }

        for (uint32_t i = 0; i < count; ++i) {
            JSValue toValue = data[i].get();
            if (toValue && toValue.isCell())
                analyzer.analyzeIndexEdge(thisObject, toValue.asCell(), i);
        }
    }
}

template<typename Visitor>
void JSFinalObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSFinalObject* thisObject = uncheckedDowncast<JSFinalObject>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    typename Visitor::DefaultMarkingViolationAssertionScope assertionScope(visitor);
    
    JSCell::visitChildren(thisObject, visitor);

    if (Structure* structure = thisObject->visitButterfly(visitor)) {
        if (unsigned storageSize = structure->inlineSize())
            visitor.appendValuesHidden(thisObject->inlineStorage(), storageSize);
    }
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSFinalObject);

String JSObject::calculatedClassName(JSObject* object)
{
    String constructorFunctionName;
    auto* globalObject = object->realmMayBeNull();
    if (!globalObject)
        return object->structure()->classInfoForCells()->className;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    // Check for a display name of obj.constructor.
    // This is useful to get `Foo` for the `(class Foo).prototype` object.
    PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
    if (object->getOwnPropertySlot(object, globalObject, vm.propertyNames->constructor, slot)) {
        EXCEPTION_ASSERT(!scope.exception());
        if (slot.isValue()) {
            if (JSObject* ctorObject = dynamicDowncast<JSObject>(slot.getValue(globalObject, vm.propertyNames->constructor))) {
                if (JSFunction* constructorFunction = dynamicDowncast<JSFunction>(ctorObject))
                    constructorFunctionName = constructorFunction->calculatedDisplayName(vm);
                else if (InternalFunction* constructorFunction = dynamicDowncast<InternalFunction>(ctorObject))
                    constructorFunctionName = constructorFunction->calculatedDisplayName(vm);
            }
        }
    }

    EXCEPTION_ASSERT(!scope.exception() || constructorFunctionName.isNull());
    if (scope.exception()) [[unlikely]]
        scope.clearException();

    // Get the display name of obj.__proto__.constructor.
    // This is useful to get `Foo` for a `new Foo` object.
    if (constructorFunctionName.isNull()) {
        if (!object->structure()->typeInfo().overridesGetPrototype()) [[likely]] {
            JSValue protoValue = object->getPrototypeDirect();
            if (protoValue.isObject()) {
                JSObject* protoObject = asObject(protoValue);
                PropertySlot slot(protoValue, PropertySlot::InternalMethodType::VMInquiry, &vm);
                if (protoObject->getPropertySlot(globalObject, vm.propertyNames->constructor, slot)) {
                    EXCEPTION_ASSERT(!scope.exception());
                    if (slot.isValue()) {
                        if (JSObject* ctorObject = dynamicDowncast<JSObject>(slot.getValue(globalObject, vm.propertyNames->constructor))) {
                            if (JSFunction* constructorFunction = dynamicDowncast<JSFunction>(ctorObject))
                                constructorFunctionName = constructorFunction->calculatedDisplayName(vm);
                            else if (InternalFunction* constructorFunction = dynamicDowncast<InternalFunction>(ctorObject))
                                constructorFunctionName = constructorFunction->calculatedDisplayName(vm);
                        }
                    }
                }
            }
        }
    }

    EXCEPTION_ASSERT(!scope.exception() || constructorFunctionName.isNull());
    if (scope.exception()) [[unlikely]]
        scope.clearException();

    if (constructorFunctionName.isNull() || constructorFunctionName == "Object"_s) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::VMInquiry, &vm);
        if (object->getPropertySlot(globalObject, vm.propertyNames->toStringTagSymbol, slot)) {
            EXCEPTION_ASSERT(!scope.exception());
            if (slot.isValue()) {
                JSValue value = slot.getValue(globalObject, vm.propertyNames->toStringTagSymbol);
                if (value.isString()) {
                    auto tag = asString(value)->value(globalObject);
                    if (scope.exception()) [[unlikely]]
                        scope.clearException();
                    return tag;
                }
            }
        }

        if (scope.exception()) [[unlikely]]
            scope.clearException();

        String classInfoName = object->classInfo()->className;
        if (!classInfoName.isNull())
            return classInfoName;

        if (constructorFunctionName.isNull())
            return "Object"_s;
    }

    return constructorFunctionName;
}

bool JSObject::getOwnPropertySlotByIndex(JSObject* thisObject, JSGlobalObject* globalObject, unsigned i, PropertySlot& slot)
{
    VM& vm = globalObject->vm();

    // NB. The fact that we're directly consulting our indexed storage implies that it is not
    // legal for anyone to override getOwnPropertySlot() without also overriding
    // getOwnPropertySlotByIndex().
    
    if (i > MAX_ARRAY_INDEX)
        return thisObject->methodTable()->getOwnPropertySlot(thisObject, globalObject, Identifier::from(vm, i), slot);

#if USE(JSVALUE64)
    // Review round 2 (blocker fix): flag-on, this generic fallback is reached
    // for indices the quickly-family declined (segmented out-of-bounds, AS
    // shapes, ...). The legacy body below derefs the flat-only butterfly()
    // accessor - on a segmented word that masks the tag and reads garbage
    // before/inside the spine (wild reads) - and reads ArrayStorage without
    // the cell lock (I31/L5). Route through the §9.5 dispatch instead.
    if (Options::useJSThreads()) [[unlikely]] {
        switch (thisObject->indexingType()) {
        case ALL_BLANK_INDEXING_TYPES:
        case ALL_UNDECIDED_INDEXING_TYPES:
            return false;
        case ALL_INT32_INDEXING_TYPES:
        case ALL_CONTIGUOUS_INDEXING_TYPES:
        case ALL_DOUBLE_INDEXING_TYPES: {
            // Bounds by min(publicLength, vectorLength) instead of the legacy
            // raw vectorLength - identical results for dense shapes (slots at
            // or past publicLength are always holes).
            JSValue value = thisObject->tryGetIndexQuicklyConcurrent(i, nullptr);
            if (value) {
                slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), value);
                return true;
            }
            return false;
        }
        case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
            // I31/L5: every runtime AS access is cell-locked; re-load the word
            // under the lock (AS-COPY may republish). AS never segments (I31).
            Locker locker { thisObject->cellLock() };
            ArrayStorage* storage = untaggedButterfly(thisObject->taggedButterflyWord())->arrayStorage();
            if (i >= storage->length())
                return false;
            if (i < storage->vectorLength()) {
                JSValue value = storage->m_vector[i].get();
                if (value) {
                    slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), value);
                    return true;
                }
            } else if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
                // AB18-G: map mutators serialize on the MAP's cellLock (the map
                // is its own JSCell), so the object cellLock held above does NOT
                // cover find() against a putEntry-internal rehash. Locked snapshot.
                if (std::optional<SparseArrayEntry> entry = map->getEntry(i)) {
                    entry->get(thisObject, slot); // Fills the slot; runs no JS.
                    return true;
                }
            }
            return false;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return false;
        }
    }
#endif

    switch (thisObject->indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        break;

    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return false;
        
        JSValue value = butterfly->contiguous().at(thisObject, i).get();
        if (value) {
            slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), value);
            return true;
        }
        
        return false;
    }
        
    case ALL_DOUBLE_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return false;
        
        double value = butterfly->contiguousDouble().at(thisObject, i);
        if (value == value) {
            slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), JSValue(JSValue::EncodeAsDouble, value));
            return true;
        }
        
        return false;
    }
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = thisObject->butterfly()->arrayStorage();
        if (i >= storage->length())
            return false;
        
        if (i < storage->vectorLength()) {
            JSValue value = storage->m_vector[i].get();
            if (value) {
                slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), value);
                return true;
            }
        } else if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
            SparseArrayValueMap::iterator it = map->find(i);
            if (it != map->notFound()) {
                it->value.get(thisObject, slot);
                return true;
            }
        }
        break;
    }
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    
    return false;
}

#if ASSERT_ENABLED
// These needs to be unique (not inlined) for ASSERT_ENABLED builds to enable
// Structure::validateFlags() to do checks using function pointer comparisons.

bool JSObject::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    return getOwnPropertySlotImpl(object, globalObject, propertyName, slot);
}
#endif // ASSERT_ENABLED

// https://tc39.github.io/ecma262/#sec-ordinaryset
bool ordinarySetSlow(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value, JSValue receiver, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertyDescriptor ownDescriptor;
    if (object->type() != ProxyObjectType) {
        object->getOwnPropertyDescriptor(globalObject, propertyName, ownDescriptor);
        RETURN_IF_EXCEPTION(scope, false);
    }
    RELEASE_AND_RETURN(scope, ordinarySetWithOwnDescriptor(globalObject, object, propertyName, value, receiver, WTF::move(ownDescriptor), shouldThrow));
}

// https://tc39.es/ecma262/multipage/ordinary-and-exotic-objects-behaviours.html#sec-ordinarysetwithowndescriptor
bool ordinarySetWithOwnDescriptor(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, JSValue value, JSValue receiver, PropertyDescriptor&& ownDescriptor, bool shouldThrow)
{
    // If we find the receiver is not the same to the object, we fall to this slow path.
    // Currently, there are 3 candidates.
    // 1. Reflect.set can alter the receiver with an arbitrary value.
    // 2. Window Proxy.
    // 3. ES6 Proxy.

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* current = object;
    while (true) {
        if (current->type() == ProxyObjectType) {
            auto* proxy = uncheckedDowncast<ProxyObject>(current);
            PutPropertySlot slot(receiver, shouldThrow);
            RELEASE_AND_RETURN(scope, proxy->ProxyObject::put(proxy, globalObject, propertyName, value, slot));
        }

        if (current != object && isTypedArrayType(current->type())) {
            PutPropertySlot slot(receiver, shouldThrow);
            RELEASE_AND_RETURN(scope, current->methodTable()->put(current, globalObject, propertyName, value, slot));
        }

        // 9.1.9.1-2 Let ownDesc be ? O.[[GetOwnProperty]](P).
        bool ownDescriptorFound;
        if (current == object)
            ownDescriptorFound = !ownDescriptor.isEmpty();
        else {
            ownDescriptorFound = current->getOwnPropertyDescriptor(globalObject, propertyName, ownDescriptor);
            RETURN_IF_EXCEPTION(scope, false);
        }

        if (!ownDescriptorFound) {
            // 9.1.9.1-3-a Let parent be ? O.[[GetPrototypeOf]]().
            JSValue prototype = current->getPrototype(globalObject);
            RETURN_IF_EXCEPTION(scope, false);

            // 9.1.9.1-3-b If parent is not null, then
            if (!prototype.isNull()) {
                // 9.1.9.1-3-b-i Return ? parent.[[Set]](P, V, Receiver).
                current = asObject(prototype);
                continue;
            }
            // 9.1.9.1-3-c-i Let ownDesc be the PropertyDescriptor{[[Value]]: undefined, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: true}.
            ownDescriptor = PropertyDescriptor(jsUndefined(), static_cast<unsigned>(PropertyAttribute::None));
        }
        break;
    }

    // 9.1.9.1-4 If IsDataDescriptor(ownDesc) is true, then
    if (ownDescriptor.isDataDescriptor()) {
        // 9.1.9.1-4-a If ownDesc.[[Writable]] is false, return false.
        if (!ownDescriptor.writable())
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

        // 9.1.9.1-4-b If Type(Receiver) is not Object, return false.
        if (!receiver.isObject())
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

        // In OrdinarySet, the receiver may not be the same to the object.
        // So, we perform [[GetOwnProperty]] onto the receiver while we already perform [[GetOwnProperty]] onto the object.

        // 9.1.9.1-4-c Let existingDescriptor be ? Receiver.[[GetOwnProperty]](P).
        JSObject* receiverObject = asObject(receiver);
        PropertyDescriptor existingDescriptor;
        bool existingDescriptorFound = receiverObject->getOwnPropertyDescriptor(globalObject, propertyName, existingDescriptor);
        RETURN_IF_EXCEPTION(scope, false);

        // 9.1.9.1-4-d If existingDescriptor is not undefined, then
        if (existingDescriptorFound) {
            // 9.1.9.1-4-d-i If IsAccessorDescriptor(existingDescriptor) is true, return false.
            if (existingDescriptor.isAccessorDescriptor())
                return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

            // 9.1.9.1-4-d-ii If existingDescriptor.[[Writable]] is false, return false.
            if (!existingDescriptor.writable())
                return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

            // 9.1.9.1-4-d-iii Let valueDesc be the PropertyDescriptor{[[Value]]: V}.
            PropertyDescriptor valueDescriptor;
            valueDescriptor.setValue(value);

            // 9.1.9.1-4-d-iv Return ? Receiver.[[DefineOwnProperty]](P, valueDesc).
            RELEASE_AND_RETURN(scope, receiverObject->methodTable()->defineOwnProperty(receiverObject, globalObject, propertyName, valueDescriptor, shouldThrow));
        }

        // 9.1.9.1-4-e Else Receiver does not currently have a property P,
        // 9.1.9.1-4-e-i Return ? CreateDataProperty(Receiver, P, V).
        RELEASE_AND_RETURN(scope, receiverObject->methodTable()->defineOwnProperty(receiverObject, globalObject, propertyName, PropertyDescriptor(value, static_cast<unsigned>(PropertyAttribute::None)), shouldThrow));
    }

    // 9.1.9.1-5 Assert: IsAccessorDescriptor(ownDesc) is true.
    ASSERT(ownDescriptor.isAccessorDescriptor());

    // 9.1.9.1-6 Let setter be ownDesc.[[Set]].
    // 9.1.9.1-7 If setter is undefined, return false.
    JSValue setter = ownDescriptor.setter();
    if (!setter.isObject())
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

    // 9.1.9.1-8 Perform ? Call(setter, Receiver, << V >>).
    JSObject* setterObject = asObject(setter);
    MarkedArgumentBuffer args;
    args.append(value);
    ASSERT(!args.hasOverflowed());

    auto callData = JSC::getCallData(setterObject);
    scope.release();
    call(globalObject, setterObject, callData, receiver, args);

    // 9.1.9.1-9 Return true.
    return true;
}

bool setterThatIgnoresPrototypeProperties(JSGlobalObject* globalObject, JSValue thisValue, JSObject* homeObject, PropertyName propertyName, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!thisValue.isObject())
        return throwTypeError(globalObject, scope, "SetterThatIgnoresPrototypeProperties expected |this| to be an object."_s);

    JSObject* thisObject = asObject(thisValue);
    RETURN_IF_EXCEPTION(scope, { });

    if (thisObject == homeObject)
        return throwTypeError(globalObject, scope, "SetterThatIgnoresPrototypeProperties was called on a home object."_s);

    bool hasProperty = thisObject->hasOwnProperty(globalObject, propertyName);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();

    if (hasProperty) {
        PutPropertySlot slot(thisObject, shouldThrow);
        return thisObject->methodTable()->put(thisObject, globalObject, propertyName, value, slot);
    }

    return thisObject->createDataProperty(globalObject, propertyName, value, shouldThrow);
}

// https://tc39.es/ecma262/#sec-ordinaryset
bool JSObject::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    return putInlineForJSObject(cell, globalObject, propertyName, value, slot);
}

bool JSObject::putInlineSlow(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    ASSERT(!parseIndex(propertyName));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (Options::useJSThreads() && structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, this)) [[unlikely]]
        return false;

    if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
        throwStackOverflowError(globalObject, scope);
        return false;
    }

    JSObject* obj = this;
    for (;;) {
        Structure* structure = obj->structure();
        if (obj != this && structure->typeInfo().overridesPut())
            RELEASE_AND_RETURN(scope, obj->methodTable()->put(obj, globalObject, propertyName, value, slot));

        bool hasProperty = false;
        unsigned attributes;
        PutValueFunc customSetter = nullptr;
        PropertyOffset offset = structure->get(vm, propertyName, attributes);
        if (isValidOffset(offset)) {
            hasProperty = true;
            if (attributes & PropertyAttribute::CustomAccessorOrValue)
                customSetter = uncheckedDowncast<CustomGetterSetter>(obj->getDirect(offset))->setter();
        } else if (structure->hasNonReifiedStaticProperties()) {
            if (auto entry = structure->findPropertyHashEntry(propertyName)) {
                hasProperty = true;
                attributes = entry->value->attributes();

                // FIXME: Remove this after we stop defaulting to CustomValue in static hash tables.
                if (!(attributes & (PropertyAttribute::CustomAccessor | PropertyAttribute::BuiltinOrFunctionOrAccessorOrLazyPropertyOrConstant)))
                    attributes |= PropertyAttribute::CustomValue;

                if (attributes & PropertyAttribute::CustomAccessorOrValue)
                    customSetter = entry->value->propertyPutter();
            }
        }

        if (hasProperty) {
            if (attributes & PropertyAttribute::ReadOnly)
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            if (attributes & PropertyAttribute::Accessor) {
                ASSERT(isValidOffset(offset));
                // We need to make sure that we decide to cache this property before we potentially execute aribitrary JS.
                if (!this->structure()->isUncacheableDictionary())
                    slot.setCacheableSetter(obj, offset);
                RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(obj->getDirect(offset))->callSetter(globalObject, slot.thisValue(), value, slot.isStrictMode()));
            }
            if (attributes & PropertyAttribute::CustomAccessor) {
                // FIXME: Remove this after WebIDL generator is fixed to set ReadOnly for [RuntimeConditionallyReadWrite] attributes.
                if (!customSetter)
                    return false;
                ASSERT(customSetter);
                // FIXME: We should only be caching these if we're not an uncacheable dictionary:
                // https://bugs.webkit.org/show_bug.cgi?id=215347
                slot.setCustomAccessor(obj, customSetter);
                scope.release();
                customSetter(obj->realm(), JSValue::encode(slot.thisValue()), JSValue::encode(value), propertyName);
                return true;
            }
            if (attributes & PropertyAttribute::CustomValue) {
                if (!isThisValueAltered(slot, obj)) {
                    if (customSetter) {
                        // FIXME: We should only be caching these if we're not an uncacheable dictionary:
                        // https://bugs.webkit.org/show_bug.cgi?id=215347
                        slot.setCustomValue(obj, customSetter);
                        RELEASE_AND_RETURN(scope, customSetter(obj->realm(), JSValue::encode(obj), JSValue::encode(value), propertyName));
                    }
                    // Avoid PutModePut because it fails for non-extensible structures.
                    obj->putDirect(vm, propertyName, value, attributesForStructure(attributes) & ~PropertyAttribute::CustomValue, slot);
                    return true;
                }
            }
            if (attributes & PropertyAttribute::BuiltinOrFunctionOrLazyProperty) {
                if (!isThisValueAltered(slot, obj)) {
                    // Avoid PutModePut because it fails for non-extensible structures.
                    obj->putDirect(vm, propertyName, value, attributesForStructure(attributes), slot);
                    return true;
                }
            }
            // If there's an existing writable property on the base object, or on one of its 
            // prototypes, we should attempt to store the property on the receiver.
            break;
        }

        JSValue prototype = obj->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (prototype.isNull())
            break;
        obj = asObject(prototype);
    }

    scope.release();
    if (isThisValueAltered(slot, this)) [[unlikely]]
        return definePropertyOnReceiver(globalObject, propertyName, value, slot);
    return putInlineFast(globalObject, propertyName, value, slot);
}

bool JSObject::mightBeSpecialProperty(VM& vm, JSType type, UniquedStringImpl* uid)
{
    switch (type) {
    case ArrayType:
    case DerivedArrayType:
        return uid == vm.propertyNames->length.impl();
    case JSFunctionType:
        return uid == vm.propertyNames->length.impl() || uid == vm.propertyNames->name.impl() || uid == vm.propertyNames->prototype.impl();
    default:
        return true;
    }
}

static NEVER_INLINE bool definePropertyOnReceiverSlow(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, JSObject* receiver, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PropertySlot slot(receiver, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = receiver->methodTable()->getOwnPropertySlot(receiver, globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, false);

    if (hasProperty) {
        // FIXME: For an accessor with setter, the error message is misleading.
        if (slot.attributes() & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor)
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

        if (slot.attributes() & PropertyAttribute::CustomValue) {
            PutValueFunc customSetter = slot.customSetter();
            if (customSetter)
                RELEASE_AND_RETURN(scope, customSetter(receiver->realm(), JSValue::encode(receiver), JSValue::encode(value), propertyName));
        }

        PropertyDescriptor descriptor;
        descriptor.setValue(value);
        RELEASE_AND_RETURN(scope, receiver->methodTable()->defineOwnProperty(receiver, globalObject, propertyName, descriptor, shouldThrow));
    }

    RELEASE_AND_RETURN(scope, receiver->createDataProperty(globalObject, propertyName, value, shouldThrow));
}

// https://tc39.es/ecma262/#sec-ordinaryset (step 3)
bool JSObject::definePropertyOnReceiver(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    ASSERT(!parseIndex(propertyName));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* receiver = slot.thisValue().getObject();
    // FIXME: For a failure due to primitive receiver, the error message is misleading.
    if (!receiver)
        return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
    scope.release();
    if (receiver->type() == GlobalProxyType)
        receiver = uncheckedDowncast<JSGlobalProxy>(receiver)->target();

    if (slot.isTaintedByOpaqueObject() || receiver->methodTable()->defineOwnProperty != JSObject::defineOwnProperty) {
        if (mightBeSpecialProperty(vm, receiver->type(), propertyName.uid()))
            return definePropertyOnReceiverSlow(globalObject, propertyName, value, receiver, slot.isStrictMode());
    }

    if (receiver->structure()->hasAnyKindOfGetterSetterProperties()) {
        unsigned attributes;
        if (receiver->getDirectOffset(vm, propertyName, attributes) != invalidOffset && (attributes & PropertyAttribute::CustomValue))
            return definePropertyOnReceiverSlow(globalObject, propertyName, value, receiver, slot.isStrictMode());
    }

    if (receiver->hasNonReifiedStaticProperties()) [[unlikely]]
        return receiver->putInlineFastReplacingStaticPropertyIfNeeded(globalObject, propertyName, value, slot);
    return receiver->putInlineFast(globalObject, propertyName, value, slot);
}

bool JSObject::putInlineFastReplacingStaticPropertyIfNeeded(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    ASSERT(!parseIndex(propertyName));

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    Structure* structure = this->structure();
    ASSERT(structure->hasNonReifiedStaticProperties());
    if (!isValidOffset(structure->get(vm, propertyName))) {
        if (auto entry = structure->findPropertyHashEntry(propertyName)) {
            if (entry->value->attributes() & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor) {
                // FIXME: For an accessor with setter, the error message is misleading.
                return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
            }
            if (entry->value->attributes() & PropertyAttribute::CustomValue) {
                PutValueFunc customSetter = entry->value->propertyPutter();
                if (customSetter)
                    RELEASE_AND_RETURN(scope, customSetter(structure->realm(), JSValue::encode(this), JSValue::encode(value), propertyName));
            }
            // Avoid PutModePut because it fails for non-extensible structures.
            putDirect(vm, propertyName, value, attributesForStructure(entry->value->attributes()) & ~PropertyAttribute::CustomValue, slot);
            return true;
        }
    }

    RELEASE_AND_RETURN(scope, putInlineFast(globalObject, propertyName, value, slot));
}

bool JSObject::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned propertyName, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);

    if (Options::useJSThreads() && thisObject->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, thisObject)) [[unlikely]]
        return false;

    if (propertyName > MAX_ARRAY_INDEX) {
        PutPropertySlot slot(cell, shouldThrow);
        return thisObject->methodTable()->put(thisObject, globalObject, Identifier::from(vm, propertyName), value, slot);
    }

#if USE(JSVALUE64)
    // Review round 2 (blocker fix): flag-on, the legacy body below derefs the
    // flat-only butterfly() accessor (garbage on segmented words: vectorLength
    // read at spine-4, wild store at spine+8*i) and writes ArrayStorage
    // without the cell lock (I31/L5). Route through the §9.5 dispatch.
    if (Options::useJSThreads()) [[unlikely]] {
        auto* object = static_cast<JSObjectWithButterfly*>(thisObject);
        bool isArrayStorage = false;
        while (true) {
            if (isCopyOnWrite(thisObject->indexingMode())) [[unlikely]] {
                // §4.8 (I35): the owner materializes via today's
                // convertFromCopyOnWrite; a FOREIGN writer must go through
                // ensureSharedWriteBit's materialize-first protocol (the plain
                // converter would trap its owner-only publication assert).
                if (!butterflyWriterIsForeign(thisObject->taggedButterflyWord()))
                    thisObject->ensureWritable(vm);
                else
                    ensureSharedWriteBit(vm, object);
                continue;
            }
            IndexingType type = thisObject->indexingType();
            if (hasAnyArrayStorage(type)) {
                isArrayStorage = true;
                break;
            }
            if (hasUndecided(type)) {
                thisObject->convertUndecidedForValue(vm, value); // §4.7 relabel: per-event stop flag-on.
                continue;
            }
            if (hasInt32(type) && !value.isInt32()) {
                thisObject->convertInt32ForValue(vm, value);
                continue;
            }
            if (hasDouble(type) && (!value.isNumber() || value.asNumber() != value.asNumber())) {
                thisObject->convertDoubleToContiguous(vm);
                continue;
            }
            if (hasInt32(type) || hasDouble(type) || hasContiguous(type)) {
                if (object->putIndexConcurrent(vm, propertyName, value))
                    return true;
                // Sparse/overlong index, OOM, or the world moved: the generic
                // beyond-vector path (its flag-on legs are routed below /
                // through the race-safe converters).
            }
            // Blank (typed-array-less) or declined dense store: generic path.
            return thisObject->putByIndexBeyondVectorLength(globalObject, propertyName, value, shouldThrow);
        }
        if (isArrayStorage) {
            // ArrayStorage (I31/L5): the in-bounds fast path runs under the
            // cell lock, re-reading the word under it (AS-COPY republishes).
            // SlowPut interception runs JS and therefore NEVER under the lock.
            bool needsInterception = false;
            {
                Locker locker { thisObject->cellLock() };
                ArrayStorage* storage = untaggedButterfly(thisObject->taggedButterflyWord())->arrayStorage(); // I31: AS never segments.
                if (propertyName < storage->vectorLength()) {
                    WriteBarrier<Unknown>& valueSlot = storage->m_vector[propertyName];
                    if (!shouldUseSlowPut(thisObject->indexingType())) {
                        if (propertyName >= storage->length()) {
                            storage->setLength(propertyName + 1);
                            ++storage->m_numValuesInVector;
                        } else if (!valueSlot)
                            ++storage->m_numValuesInVector;
                        valueSlot.set(vm, thisObject, value);
                        return true;
                    }
                    if (propertyName < storage->length() && valueSlot) {
                        valueSlot.set(vm, thisObject, value);
                        return true;
                    }
                    needsInterception = true;
                }
            }
            if (needsInterception) {
                auto scope = DECLARE_THROW_SCOPE(vm);
                bool putResult = false;
                bool result = thisObject->attemptToInterceptPutByIndexOnHole(globalObject, propertyName, value, shouldThrow, putResult);
                RETURN_IF_EXCEPTION(scope, false);
                if (result)
                    return putResult;
                // Not intercepted: complete the store under the lock (the
                // legacy tail), tolerating a republication in between.
                Locker locker { thisObject->cellLock() };
                ArrayStorage* storage = untaggedButterfly(thisObject->taggedButterflyWord())->arrayStorage();
                if (propertyName < storage->vectorLength()) {
                    WriteBarrier<Unknown>& valueSlot = storage->m_vector[propertyName];
                    if (propertyName >= storage->length()) {
                        storage->setLength(propertyName + 1);
                        ++storage->m_numValuesInVector;
                    } else if (!valueSlot)
                        ++storage->m_numValuesInVector;
                    valueSlot.set(vm, thisObject, value);
                    return true;
                }
                // The vector was republished smaller: generic path.
            }
            return thisObject->putByIndexBeyondVectorLength(globalObject, propertyName, value, shouldThrow);
        }
    }
#endif

    thisObject->ensureWritable(vm);

    switch (thisObject->indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        break;
        
    case ALL_UNDECIDED_INDEXING_TYPES: {
        thisObject->convertUndecidedForValue(vm, value);
        // Reloop.
        return putByIndex(cell, globalObject, propertyName, value, shouldThrow);
    }
        
    case ALL_INT32_INDEXING_TYPES: {
        if (!value.isInt32()) {
            thisObject->convertInt32ForValue(vm, value);
            return putByIndex(cell, globalObject, propertyName, value, shouldThrow);
        }
        [[fallthrough]];
    }
        
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (propertyName >= butterfly->vectorLength())
            break;
        butterfly->contiguous().at(thisObject, propertyName).setWithoutWriteBarrier(value);
        if (propertyName >= butterfly->publicLength())
            butterfly->setPublicLength(propertyName + 1);
        vm.writeBarrier(thisObject, value);
        return true;
    }
        
    case ALL_DOUBLE_INDEXING_TYPES: {
        if (!value.isNumber()) {
            thisObject->convertDoubleToContiguous(vm);
            // Reloop.
            return putByIndex(cell, globalObject, propertyName, value, shouldThrow);
        }

        double valueAsDouble = value.asNumber();
        if (valueAsDouble != valueAsDouble) {
            thisObject->convertDoubleToContiguous(vm);
            // Reloop.
            return putByIndex(cell, globalObject, propertyName, value, shouldThrow);
        }
        Butterfly* butterfly = thisObject->butterfly();
        if (propertyName >= butterfly->vectorLength())
            break;
        butterfly->contiguousDouble().at(thisObject, propertyName) = valueAsDouble;
        if (propertyName >= butterfly->publicLength())
            butterfly->setPublicLength(propertyName + 1);
        return true;
    }
        
    case NonArrayWithArrayStorage:
    case ArrayWithArrayStorage: {
        ArrayStorage* storage = thisObject->butterfly()->arrayStorage();
        
        if (propertyName >= storage->vectorLength())
            break;
        
        WriteBarrier<Unknown>& valueSlot = storage->m_vector[propertyName];
        unsigned length = storage->length();
        
        // Update length & m_numValuesInVector as necessary.
        if (propertyName >= length) {
            length = propertyName + 1;
            storage->setLength(length);
            ++storage->m_numValuesInVector;
        } else if (!valueSlot)
            ++storage->m_numValuesInVector;
        
        valueSlot.set(vm, thisObject, value);
        return true;
    }
        
    case NonArrayWithSlowPutArrayStorage:
    case ArrayWithSlowPutArrayStorage: {
        ArrayStorage* storage = thisObject->butterfly()->arrayStorage();
        
        if (propertyName >= storage->vectorLength())
            break;
        
        WriteBarrier<Unknown>& valueSlot = storage->m_vector[propertyName];
        unsigned length = storage->length();

        auto scope = DECLARE_THROW_SCOPE(vm);
        
        // Update length & m_numValuesInVector as necessary.
        if (propertyName >= length) {
            bool putResult = false;
            bool result = thisObject->attemptToInterceptPutByIndexOnHole(globalObject, propertyName, value, shouldThrow, putResult);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return putResult;
            length = propertyName + 1;
            storage->setLength(length);
            ++storage->m_numValuesInVector;
        } else if (!valueSlot) {
            bool putResult = false;
            bool result = thisObject->attemptToInterceptPutByIndexOnHole(globalObject, propertyName, value, shouldThrow, putResult);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return putResult;
            ++storage->m_numValuesInVector;
        }
        
        valueSlot.set(vm, thisObject, value);
        return true;
    }
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    return thisObject->putByIndexBeyondVectorLength(globalObject, propertyName, value, shouldThrow);
}

ArrayStorage* JSObject::enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(VM& vm, ArrayStorage* storage)
{
#if USE(JSVALUE64)
    // SPEC-objectmodel §4.6/I31 (Task 8): flag-on, every ArrayStorage access
    // and relayout is cell-locked, and the butterfly republication below must
    // preserve the installer's tag verbatim (AS-COPY, T3/I17) - the word may
    // legitimately be foreign-tagged or SW=1 here (e.g. a foreign
    // defineOwnIndexedProperty entering dictionary indexing mode), which the
    // owner-only setButterfly/storeTaggedButterflyWordConcurrent path rejects.
    // The DeferGC precedes the lock per O1's sanctioned back-edge (the
    // sparse-map and resizeArray allocations run under the lock), mirroring
    // increaseVectorLength.
    std::optional<DeferGC> threadsDeferGC;
    std::optional<Locker<JSCellLock>> threadsLocker;
    if (Options::useJSThreads()) [[unlikely]] {
        threadsDeferGC.emplace(vm);
        threadsLocker.emplace(cellLock());
        storage = arrayStorage(); // Re-read under the lock: a racing AS-COPY may have republished the butterfly.
    }
#endif
    SparseArrayValueMap* map = storage->m_sparseMap.get();

    if (!map)
        map = allocateSparseIndexMap(vm);

    if (map->sparseMode())
        return storage;

    map->setSparseMode();

    unsigned usedVectorLength = std::min(storage->length(), storage->vectorLength());
    for (unsigned i = 0; i < usedVectorLength; ++i) {
        JSValue value = storage->m_vector[i].get();
        // This will always be a new entry in the map, so no need to check we can write,
        // and attributes are default so no need to set them.
        if (value)
            map->add(this, i).iterator->value.forceSet(vm, map, value, 0);
    }

    DeferGC deferGC(vm);
    Butterfly* newButterfly = storage->butterfly()->resizeArray(vm, this, structure(), 0, ArrayStorage::sizeFor(0));
    RELEASE_ASSERT(newButterfly);
    newButterfly->arrayStorage()->m_indexBias = 0;
    newButterfly->arrayStorage()->setVectorLength(0);
    newButterfly->arrayStorage()->m_sparseMap.set(vm, this, map);
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] // §4.6 AS-COPY publication form (T3/I17), under the cell lock taken above.
        publishArrayStorageButterflyLocked(vm, static_cast<JSObjectWithButterfly*>(this), newButterfly);
    else
#endif
        setButterfly(vm, newButterfly);

    return newButterfly->arrayStorage();
}

void JSObject::enterDictionaryIndexingMode(VM& vm)
{
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
    case ALL_INT32_INDEXING_TYPES:
    case ALL_DOUBLE_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        // NOTE: this is horribly inefficient, as it will perform two conversions. We could optimize
        // this case if we ever cared. Note that ensureArrayStorage() can return null if the object
        // doesn't support traditional indexed properties. At the time of writing, this just affects
        // typed arrays.
        if (ArrayStorage* storage = ensureArrayStorageSlow(vm))
            enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, storage);
        break;
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, this->butterfly()->arrayStorage());
        break;
        
    default:
        break;
    }
}

void JSObject::notifyPresenceOfIndexedAccessors(VM& vm)
{
    if (isGlobalObject()) [[unlikely]] {
        uncheckedDowncast<JSGlobalObject>(this)->globalThis()->notifyPresenceOfIndexedAccessors(vm);
        return;
    }

    if (mayInterceptIndexedAccesses())
        return;

    auto* globalObject = realmMayBeNull();
    if (!globalObject)
        return;

    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AddIndexedAccessors, &deferred));
    }

    if (!mayBePrototype())
        return;

    globalObject->haveABadTime(vm);
}

Butterfly* JSObject::createInitialIndexedStorage(VM& vm, unsigned length)
{
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    IndexingType oldType = indexingType();
    ASSERT_UNUSED(oldType, !hasIndexedProperties(oldType));
    ASSERT(!needsSlowPutIndexing());
    ASSERT(!indexingShouldBeSparse());
    Structure* structure = this->structure();
    unsigned propertyCapacity = structure->outOfLineCapacity();
    unsigned vectorLength = Butterfly::optimalContiguousVectorLength(propertyCapacity, length);
    Butterfly* newButterfly = Butterfly::createOrGrowArrayRight(
        this->butterfly(), vm, this, structure, propertyCapacity, false, 0,
        sizeof(EncodedJSValue) * vectorLength);
    newButterfly->setPublicLength(length);
    newButterfly->setVectorLength(vectorLength);
    return newButterfly;
}

Butterfly* JSObject::createInitialUndecided(VM& vm, unsigned length)
{
#if USE(JSVALUE64)
    // Review round 2: flag-on, first indexed installs publish through the
    // race-safe concurrent route (N3 loser re-dispatch instead of trapping;
    // F2 fires for shared triggers). nullptr => caller re-dispatches.
    if (Options::useJSThreads()) [[unlikely]]
        return createInitialIndexedStorageConcurrent(vm, TransitionKind::AllocateUndecided, length);
#endif
    DeferGC deferGC(vm);
    Butterfly* newButterfly = createInitialIndexedStorage(vm, length);
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateUndecided, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return newButterfly;
}

ContiguousJSValues JSObject::createInitialInt32(VM& vm, unsigned length)
{
#if USE(JSVALUE64)
    // Review round 2: see createInitialUndecided. Empty => caller re-dispatches.
    if (Options::useJSThreads()) [[unlikely]] {
        Butterfly* newButterfly = createInitialIndexedStorageConcurrent(vm, TransitionKind::AllocateInt32, length);
        if (!newButterfly)
            return ContiguousJSValues();
        return newButterfly->contiguousInt32();
    }
#endif
    DeferGC deferGC(vm);
    Butterfly* newButterfly = createInitialIndexedStorage(vm, length);
    for (unsigned i = newButterfly->vectorLength(); i--;)
        newButterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateInt32, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return newButterfly->contiguousInt32();
}

ContiguousDoubles JSObject::createInitialDouble(VM& vm, unsigned length)
{
#if USE(JSVALUE64)
    // Review round 2: see createInitialUndecided. Empty => caller re-dispatches.
    if (Options::useJSThreads()) [[unlikely]] {
        Butterfly* newButterfly = createInitialIndexedStorageConcurrent(vm, TransitionKind::AllocateDouble, length);
        if (!newButterfly)
            return ContiguousDoubles();
        return newButterfly->contiguousDouble();
    }
#endif
    DeferGC deferGC(vm);
    Butterfly* newButterfly = createInitialIndexedStorage(vm, length);
    for (unsigned i = newButterfly->vectorLength(); i--;)
        newButterfly->contiguousDouble().at(this, i) = PNaN;
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateDouble, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return newButterfly->contiguousDouble();
}

ContiguousJSValues JSObject::createInitialContiguous(VM& vm, unsigned length)
{
#if USE(JSVALUE64)
    // Review round 2: see createInitialUndecided. Empty => caller re-dispatches.
    if (Options::useJSThreads()) [[unlikely]] {
        Butterfly* newButterfly = createInitialIndexedStorageConcurrent(vm, TransitionKind::AllocateContiguous, length);
        if (!newButterfly)
            return ContiguousJSValues();
        return newButterfly->contiguous();
    }
#endif
    DeferGC deferGC(vm);
    Butterfly* newButterfly = createInitialIndexedStorage(vm, length);
    for (unsigned i = newButterfly->vectorLength(); i--;)
        newButterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return newButterfly->contiguous();
}

#if USE(JSVALUE64)
// ===== SPEC-objectmodel review round 2 (blocker fix): createInitial* family =====
//
// The legacy createInitialUndecided/Int32/Double/Contiguous publish via plain
// nukeStructureAndSetButterfly + setStructure. Flag-on that is unsound:
//   (a) two threads racing the FIRST indexed install on a shared butterfly-less
//       object both reach the N3 leg, and the loser trips
//       setButterflyConcurrent's RELEASE_ASSERT(!previous) (or
//       storeTaggedButterflyWordConcurrent's owner assert) - a release crash on
//       a legal race; the N3 contract is loser RE-DISPATCH;
//   (b) the plain structureID nuke races the locked protocols
//       (trySegmentedTransition's nuke-CAS RELEASE_ASSERTs success) and an
//       owner E4 transition (torn {structure, butterfly} pair, I21);
//   (c) a FOREIGN first indexed install is a foreign butterfly transition and
//       must fire F2 under a §10.6 stop (I10/I10b) - the legacy path never
//       fires;
//   (d) `this->butterfly()` in createInitialIndexedStorage is flat-only and
//       garbage-decodes a segmented word.
//
// This routine is the flag-on route for all four. Regime dispatch:
//   - N3 (word == 0): nuke-CAS the structureID (loser re-dispatches), fenced
//     casButterfly(0 -> (currentTID, 0)), setStructure.
//   - E4 owner (flat owner tag, SW=0, BOTH source TTL sets valid, no §9.6
//     stress): fresh flat allocation; poll-free copy window; nuke-CAS;
//     casButterfly with the EXACT expected word (T1 shape, I27 - a foreign F1
//     flip cannot complete in the poll-free window while writeThreadLocal is
//     valid, since the fire needs a stop and we never poll); setStructure.
//   - shared (foreign tag, SW=1, segmented word, sets fired, or §9.6 stress):
//     §10.6 per-event stop - re-verify, fire F2 if either TTL set is still
//     valid, copy out-of-line properties, publish via the fenced nuke order
//     (world stopped). A segmented source publishes a REPLACEMENT SPINE
//     (out-of-line fragments aliased verbatim per I6; fresh hole-filled
//     header + indexed fragments) and stays in regime 2.
//
// Returns nullptr when the caller cannot address the storage flatly: either a
// racing install/transition won (settled indexing type is no longer blank) or
// the publication ended segmented. Callers re-dispatch through the concurrent
// accessors / their generic paths; flag-off paths are untouched (I22).
Butterfly* JSObject::createInitialIndexedStorageConcurrent(VM& vm, TransitionKind transitionKind, unsigned length)
{
    ASSERT(Options::useJSThreads());
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    ASSERT(!needsSlowPutIndexing());
    ASSERT(!indexingShouldBeSparse());
    auto* object = static_cast<JSObjectWithButterfly*>(this);
    DeferGC deferGC(vm);

    auto fillFlatLanes = [&](Butterfly* butterfly, IndexingType targetType, unsigned vectorLength) {
        // Legacy createInitialUndecided leaves lanes uninitialized; here we
        // always hole-fill - the storage can become reachable by other threads
        // the instant it is published.
        if (hasDouble(targetType)) {
            for (unsigned i = vectorLength; i--;)
                butterfly->contiguousDouble().at(this, i) = PNaN;
        } else {
            for (unsigned i = vectorLength; i--;)
                butterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());
        }
    };

    while (true) {
        StructureID oldStructureID = structureID(); // RAW bits (M5).
        if (oldStructureID.isNuked())
            continue; // A racing publication is mid-flight; re-plan on the settled state.
        Structure* oldStructure = oldStructureID.decode();
        if (hasIndexedProperties(oldStructure->indexingType()))
            return nullptr; // A racing install won; the caller re-dispatches on the settled state.

        Structure* newStructure;
        {
            DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
            newStructure = Structure::nonPropertyTransition(vm, oldStructure, transitionKind, &deferred);
        }
        IndexingType targetType = newStructure->indexingType();
        unsigned propertyCapacity = oldStructure->outOfLineCapacity();
        unsigned propertySize = oldStructure->outOfLineSize();
        uint64_t word = object->taggedButterflyWord();
        auto* idAtomic = std::bit_cast<Atomic<uint32_t>*>(std::bit_cast<char*>(this) + JSCell::structureIDOffset());

        // ---- N3: first install on a butterfly-less object. CVE-audit
        // MC-LOCK S6 (CHECK-NOW item 4): a butterfly-less instance keys its
        // transition ownership on the structure's N1 transition TID (§2.1
        // N1/F2) — a FOREIGN first indexed install is a foreign butterfly
        // transition and must fire BOTH TTL sets under a §10.6 stop before
        // publication (I10/I10b/I13), exactly like the sibling
        // createArrayStorageConcurrent leg. Otherwise an owner-thread E4 plain
        // store elided on the still-valid writeThreadLocal set races our
        // lock-free publication. Route the foreign-keyed install (incl. §9.6
        // forceButterflySWBit) through the shared per-event-stop leg below.
        //
        // AB18-S3: "every relevant set is already fired" excludes E4 elision
        // races, but it does NOT make the lock-free nuke-CAS protocol sound:
        // in the fired-sets regime named adds on this same word==0 object run
        // the CELL-LOCKED protocols (AB18-R1-H routing:
        // tryStructureOnlyTransition / trySegmentedTransition FirstInstall),
        // whose lane-ownership argument assumes every non-stop writer of the
        // structureID lane holds the cell lock — they fail-stop
        // (RELEASE_ASSERT) on any CAS divergence in the semantic bytes. A
        // lock-free nuke-CAS from this leg can land in their under-lock
        // check->CAS window and trip those asserts. So this leg splits:
        //   - owner + both source TTL sets observed valid by FRESH loads
        //     inside a poll-free window (I29): lock-free publication is
        //     exclusive — a foreign locked transitioner must fire the sets
        //     under a §10.6 stop first, and that stop cannot land inside the
        //     poll-free window;
        //   - otherwise (sets fired, any TID): publish under the CELL LOCK,
        //     so the locked protocols' under-lock re-check serializes against
        //     us (lost race on either side => RESTART/re-plan, never abort).
        bool foreignButterflyLessInstall = !word
            && (currentButterflyTID() != oldStructure->transitionThreadLocalTID() || forceButterflySWBitEnabled())
            && (oldStructure->transitionThreadLocalIsStillValid() || oldStructure->writeThreadLocalIsStillValid()
                || (newStructure != oldStructure
                    && (newStructure->transitionThreadLocalIsStillValid() || newStructure->writeThreadLocalIsStillValid())));
        if (!word && !foreignButterflyLessInstall) {
            ASSERT(!propertySize);
            unsigned vectorLength = Butterfly::optimalContiguousVectorLength(propertyCapacity, length);
            Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, true, sizeof(EncodedJSValue) * vectorLength); // May GC/poll => revalidate below (I29).
            newButterfly->setPublicLength(length);
            newButterfly->setVectorLength(vectorLength);
            fillFlatLanes(newButterfly, targetType, vectorLength);
            for (unsigned k = 0; k < propertyCapacity; ++k)
                (newButterfly->propertyStorage() - (k + 1))->clear(); // outOfLineSize == 0; zero the slack so GC never sees garbage.
            WTF::storeStoreFence(); // Contents before publication.
            bool publishedLockFree = false;
            {
                AssertNoGC assertNoGC; // AB18-S3 (I29): revalidation -> publication, poll-free.
                if (this->structureID() != oldStructureID || object->taggedButterflyWord())
                    continue; // A racing transition/install moved the lane at the allocation poll: re-plan; the allocation drops unreferenced.
                if (currentButterflyTID() == oldStructure->transitionThreadLocalTID()
                    && !forceButterflySWBitEnabled()
                    && oldStructure->transitionThreadLocalIsStillValid()
                    && oldStructure->writeThreadLocalIsStillValid()) {
                    // Owner + sets valid by FRESH loads in this poll-free
                    // window: no foreign locked window can open before our
                    // publication (its step-0 set fire needs a §10.6 stop).
                    uint32_t previousBits = idAtomic->compareExchangeStrong(oldStructureID.bits(), oldStructureID.nuke().bits());
                    if (previousBits != oldStructureID.bits())
                        continue; // N3 loser (a racing transition nuked/CASed first): re-plan; the allocation is dropped unreferenced.
                    WTF::storeStoreFence();
                    if (!casButterfly(object, 0, encodeButterfly(newButterfly, currentButterflyTID(), false))) {
                        // Defensive: while we hold the nuked structureID lane no
                        // butterfly publication should land; un-nuke and re-plan.
                        idAtomic->store(oldStructureID.bits(), std::memory_order_seq_cst);
                        continue;
                    }
                    WTF::storeStoreFence();
                    setStructure(vm, newStructure);
                    publishedLockFree = true;
                }
            }
            if (!publishedLockFree) {
                // Fired-sets regime (owner or foreign): the cell-locked named
                // protocols are un-excluded racers on this lane — publish
                // under the cell lock (AB18-S3, see the leg comment above).
                bool publishedLocked = false;
                {
                    Locker locker { object->cellLock() };
                    if (this->structureID() == oldStructureID && !object->taggedButterflyWord()) {
                        uint32_t previousBits = idAtomic->compareExchangeStrong(oldStructureID.bits(), oldStructureID.nuke().bits());
                        if (previousBits == oldStructureID.bits()) {
                            WTF::storeStoreFence();
                            if (casButterfly(object, 0, encodeButterfly(newButterfly, currentButterflyTID(), false))) {
                                WTF::storeStoreFence();
                                setStructure(vm, newStructure);
                                publishedLocked = true;
                            } else {
                                // Defensive: under the cell lock with the lane
                                // nuked no butterfly publication should land;
                                // un-nuke and re-plan.
                                idAtomic->store(oldStructureID.bits(), std::memory_order_seq_cst);
                            }
                        }
                    }
                }
                if (!publishedLocked)
                    continue; // A racing transition won (re-checked under the lock): re-plan on the settled state.
            }
            vm.writeBarrier(this);
            applyForceSegmentedButterfliesStressIfNeeded(vm, object); // §9.6 N3 coverage.
            if (isSegmentedButterfly(object->taggedButterflyWord())) [[unlikely]]
                return nullptr; // Stress converted: not flat-addressable.
            return newButterfly;
        }

        bool wordSegmented = isSegmentedButterfly(word);

        // ---- E4 owner fast path: flat owner tag, SW=0, both source TTL sets
        // valid. While writeThreadLocal is valid no foreign write/transition
        // exists, and any F1/F2 fire needs a §10.6 stop, which cannot land
        // inside the poll-free window below.
        if (!wordSegmented && !butterflySharedWrite(word) && !butterflyWriterIsForeign(word)
            && oldStructure->transitionThreadLocalIsStillValid() && oldStructure->writeThreadLocalIsStillValid()
            && !forceSegmentedButterfliesEnabled()) {
            Butterfly* oldButterfly = untaggedButterfly(word);
            unsigned vectorLength = Butterfly::optimalContiguousVectorLength(propertyCapacity, length);
            Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, true, sizeof(EncodedJSValue) * vectorLength);
            newButterfly->setPublicLength(length);
            newButterfly->setVectorLength(vectorLength);
            fillFlatLanes(newButterfly, targetType, vectorLength);
            {
                AssertNoGC assertNoGC; // Straight-line copy -> publish window (I34-style; no polls).
                // AB18-S3 (I29): revalidate with FRESH loads after the
                // createUninitialized poll above, mirroring the named E4 leg
                // (tryPutDirectTransitionConcurrent). A foreign conversion /
                // locked transitioner's step-0 set fire (a §10.6 stop) can
                // land at that poll; publishing lock-free in the fired-sets
                // regime would race its cell-locked window (fail-stop
                // RELEASE_ASSERT on its side). Sets observed valid inside
                // this poll-free window => no such window can open before
                // our publication.
                if (this->structureID() != oldStructureID || object->taggedButterflyWord() != word
                    || !oldStructure->transitionThreadLocalIsStillValid()
                    || !oldStructure->writeThreadLocalIsStillValid())
                    continue; // Re-plan on the settled state; the allocation drops unreferenced.
                for (unsigned k = 0; k < propertyCapacity; ++k) {
                    if (k < propertySize)
                        (newButterfly->propertyStorage() - (k + 1))->setWithoutWriteBarrier((oldButterfly->propertyStorage() - (k + 1))->get());
                    else
                        (newButterfly->propertyStorage() - (k + 1))->clear();
                }
                WTF::storeStoreFence(); // Contents before publication.
                uint32_t previousBits = idAtomic->compareExchangeStrong(oldStructureID.bits(), oldStructureID.nuke().bits());
                if (previousBits != oldStructureID.bits())
                    continue; // Defensive (single owner): re-plan on the settled state.
                WTF::storeStoreFence();
                if (!casButterfly(object, word, encodeButterfly(newButterfly, currentButterflyTID(), false))) {
                    // The word moved (cannot happen while the sets are valid in
                    // this poll-free window; defensive): un-nuke, NEVER merge
                    // the copied payload (I21/I27) - re-plan from scratch.
                    idAtomic->store(oldStructureID.bits(), std::memory_order_seq_cst);
                    continue;
                }
                WTF::storeStoreFence();
                setStructure(vm, newStructure);
            }
            vm.writeBarrier(this);
            return newButterfly;
        }

        // ---- Shared trigger (foreign / SW=1 / segmented / sets fired / §9.6
        // stress): plan + allocate OUTSIDE the §10.6 stop (O4), publish inside.
        Butterfly* newButterfly = nullptr;
        ButterflySpine* newSpine = nullptr;
        unsigned flatVectorLength = 0;
        if (!wordSegmented) {
            flatVectorLength = Butterfly::optimalContiguousVectorLength(propertyCapacity, length);
            newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, true, sizeof(EncodedJSValue) * flatVectorLength);
            newButterfly->setPublicLength(length);
            newButterfly->setVectorLength(flatVectorLength);
            fillFlatLanes(newButterfly, targetType, flatVectorLength);
            // Out-of-line properties are copied INSIDE the stop (they may be
            // racing-written until the world stops).
        } else {
            // Segmented source: replacement spine. The source must be
            // header-less (C2: blank indexing type => no indexed fragments).
            ButterflySpine* oldSpine = butterflySpine(word);
            RELEASE_ASSERT(!oldSpine->indexedFragmentCountConcurrent() && !oldSpine->vectorLengthConcurrent()); // C2; relaxed reads (V7 TSAN getters)
            uint32_t outOfLineFragments = oldSpine->outOfLineFragmentCountConcurrent();
            uint32_t neededIndexedFragments = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(length) + 1 + (butterflyFragmentSlots - 1)) / butterflyFragmentSlots));
            uint32_t coveredVectorLength = neededIndexedFragments * butterflyFragmentSlots - 1;
            uint32_t publishedVectorLength = std::min<uint32_t>(coveredVectorLength, MAX_STORAGE_VECTOR_LENGTH);
            ASSERT(publishedVectorLength >= length);
            newSpine = static_cast<ButterflySpine*>(vm.auxiliarySpace().allocate(
                vm, ButterflySpine::allocationSize(outOfLineFragments + neededIndexedFragments), nullptr, AllocationFailureMode::Assert));
            newSpine->outOfLineFragmentCount = outOfLineFragments;
            newSpine->indexedFragmentCount = neededIndexedFragments;
            newSpine->vectorLength = publishedVectorLength;
            // V7 (TSAN): relaxed reads of the published old spine (its out-of-line
            // fragment slots are atomically re-pointed by racing property adds).
            newSpine->spineEpoch = butterflyConcurrentLoad(&oldSpine->spineEpoch) + 1;
            newSpine->aliasedAllocationBase = butterflyConcurrentLoad(&oldSpine->aliasedAllocationBase); // VERBATIM (I7)
            newSpine->aliasedAllocationSize = butterflyConcurrentLoad(&oldSpine->aliasedAllocationSize);
            for (uint32_t j = 0; j < outOfLineFragments; ++j)
                newSpine->fragments()[j] = oldSpine->outOfLineFragment(j); // Shared fragments aliased verbatim (I6); relaxed slot load.
            bool fillDouble = hasDouble(targetType);
            for (uint32_t f = 0; f < neededIndexedFragments; ++f) {
                auto* fragment = static_cast<ButterflyFragment*>(
                    vm.auxiliarySpace().allocate(vm, sizeof(ButterflyFragment), nullptr, AllocationFailureMode::Assert));
                for (size_t slotIndex = 0; slotIndex < butterflyFragmentSlots; ++slotIndex) {
                    if (fillDouble)
                        *std::bit_cast<double*>(&fragment->slots[slotIndex]) = PNaN; // §4.7 raw hole.
                    else
                        fragment->slots[slotIndex].clear();
                }
                newSpine->fragments()[outOfLineFragments + f] = fragment;
            }
            // Header slot (indexed fragment 0 slot 0): live publicLength in the
            // low half; the high half (frozen flat-era vectorLength, I9b) is 0 -
            // this storage never had a flat era.
            *std::bit_cast<uint64_t*>(&newSpine->indexedFragment(0)->slots[0]) = static_cast<uint64_t>(length);
            newSpine->validateConsistency();
        }

        bool published = false;
        jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
            // ---- Re-verify inside the stop; allocate nothing (O4).
            if (this->structureID() != oldStructureID)
                return; // RESTART: a racing transition won before the stop landed.
            if (object->taggedButterflyWord() != word)
                return; // RESTART: the word moved (SW flip / replacement spine).

            // ---- F2 (I10/I10b/I13): a shared indexed first install fires BOTH
            // sets on source and target in this same stop (chain-fired per F4).
            if (oldStructure->transitionThreadLocalIsStillValid() || oldStructure->writeThreadLocalIsStillValid())
                oldStructure->fireTransitionThreadLocal(vm, "F2: shared first indexed-storage install (review round 2)");
            if (newStructure != oldStructure
                && (newStructure->transitionThreadLocalIsStillValid() || newStructure->writeThreadLocalIsStillValid()))
                newStructure->fireTransitionThreadLocal(vm, "F2: shared first indexed-storage install (review round 2 target)");

            uint64_t newWord;
            if (!wordSegmented) {
                // Copy out-of-line properties now that nothing races.
                Butterfly* oldButterfly = untaggedButterfly(word);
                for (unsigned k = 0; k < propertyCapacity; ++k) {
                    if (k < propertySize)
                        (newButterfly->propertyStorage() - (k + 1))->setWithoutWriteBarrier((oldButterfly->propertyStorage() - (k + 1))->get());
                    else
                        (newButterfly->propertyStorage() - (k + 1))->clear();
                }
                // §4.6-style shared materialization: FLAT, tagged
                // (currentButterflyTID(), SW), SW = shared trigger or the
                // source's own SW bit. A butterfly-less (word == 0) install
                // only reaches this leg via the foreign N1 keying above —
                // shared trigger, so SW=1 (mirrors createArrayStorageConcurrent).
                bool sharedWriteBit = butterflySharedWrite(word) || butterflyWriterIsForeign(word)
                    || !(word & butterflyPointerMask);
                newWord = encodeButterfly(newButterfly, currentButterflyTID(), sharedWriteBit);
            } else
                newWord = encodeSegmentedButterfly(newSpine); // (notTTLTID, 1) - I3; out-of-line fragments aliased, nothing to copy.

            // ---- Publish (M8 fenced nuke order; world stopped - legal on PA
            // cells too, I36).
            setStructureIDDirectly(oldStructureID.nuke());
            WTF::storeStoreFence();
            std::bit_cast<Atomic<uint64_t>*>(object->butterflyAddress())->store(newWord, std::memory_order_seq_cst);
            WTF::storeStoreFence();
            setStructure(vm, newStructure);
            published = true;
        }));

        if (!published)
            continue; // RESTART: re-plan from the fresh settled state; the step-1 allocations drop unreferenced.

        vm.writeBarrier(this); // Publication barrier (I25); superseded storage is never written again.
        if (wordSegmented)
            return nullptr; // Published, but not flat-addressable.
        applyForceSegmentedButterfliesStressIfNeeded(vm, object); // §9.6 coverage for the flat publication.
        if (isSegmentedButterfly(object->taggedButterflyWord())) [[unlikely]]
            return nullptr;
        return newButterfly;
    }
}

// Review round 2 companion for the two createInitialForValueAndSet call sites
// (putByIndexBeyondVectorLength / putDirectIndexSlowOrBeyondVectorLength blank
// legs). Returns false when the value was NOT stored and the caller must
// re-dispatch its full put path (N3 loser, or a racing shape change beat the
// post-publication store).
bool JSObject::tryCreateInitialForValueAndSetConcurrent(VM& vm, unsigned index, JSValue value)
{
    ASSERT(Options::useJSThreads());
    TransitionKind transitionKind;
    if (value.isInt32())
        transitionKind = TransitionKind::AllocateInt32;
    else if (value.isDouble() && Options::allowDoubleShape() && value.asNumber() == value.asNumber())
        transitionKind = TransitionKind::AllocateDouble;
    else
        transitionKind = TransitionKind::AllocateContiguous;

    Butterfly* butterfly = createInitialIndexedStorageConcurrent(vm, transitionKind, index + 1);
    if (!butterfly) {
        // Racer won, or our publication is segmented: store through the
        // concurrent accessor; on failure the caller re-dispatches.
        return trySetIndexQuicklyConcurrent(vm, index, value, nullptr);
    }
    // Flat publication; the lanes were hole-filled and publicLength == index+1.
    // The store itself may race other threads' stores - plain SAB-granularity
    // element stores are legal on a published word.
    if (transitionKind == TransitionKind::AllocateDouble)
        butterfly->contiguousDouble().at(this, index) = value.asNumber();
    else {
        butterfly->contiguous().at(this, index).setWithoutWriteBarrier(value);
        vm.writeBarrier(this, value);
    }
    return true;
}
#endif // USE(JSVALUE64)

static Butterfly* createArrayStorageButterflyImpl(VM& vm, JSObject* intendedOwner, Structure* structure, unsigned length, unsigned vectorLength, Butterfly* oldButterfly, AllocationFailureMode mode)
{
    Butterfly* newButterfly = Butterfly::createOrGrowArrayRight(
        oldButterfly, vm, intendedOwner, structure, structure->outOfLineCapacity(), false, 0,
        ArrayStorage::sizeFor(vectorLength));
    if (!newButterfly) [[unlikely]] {
        RELEASE_ASSERT_RESOURCE_AVAILABLE(mode != AllocationFailureMode::Assert, MemoryExhaustion, "Crash intentionally because memory is exhausted.");
        return nullptr;
    }

    ArrayStorage* result = newButterfly->arrayStorage();
    result->setLength(length);
    result->setVectorLength(vectorLength);
    result->m_sparseMap.clear();
    result->m_numValuesInVector = 0;
    result->m_indexBias = 0;
    for (size_t i = vectorLength; i--;)
        result->m_vector[i].setWithoutWriteBarrier(JSValue());

    return newButterfly;
}

Butterfly* JSObject::createArrayStorageButterfly(VM& vm, JSObject* intendedOwner, Structure* structure, unsigned length, unsigned vectorLength, Butterfly* oldButterfly)
{
    return createArrayStorageButterflyImpl(vm, intendedOwner, structure, length, vectorLength, oldButterfly, AllocationFailureMode::Assert);
}

Butterfly* JSObject::tryCreateArrayStorageButterfly(VM& vm, JSObject* intendedOwner, Structure* structure, unsigned length, unsigned vectorLength, Butterfly* oldButterfly)
{
    return createArrayStorageButterflyImpl(vm, intendedOwner, structure, length, vectorLength, oldButterfly, AllocationFailureMode::ReturnNull);
}


ArrayStorage* JSObject::createArrayStorage(VM& vm, unsigned length, unsigned vectorLength)
{
#if USE(JSVALUE64)
    // SPEC-objectmodel §4.6/I31/I10b (review round 3, blocker fix): flag-on,
    // the first transition INTO ArrayStorage on a blank-indexing object is a
    // shared-capable transition like the convert*ToArrayStorage family and
    // must NOT publish via the plain nuke + storeTaggedButterflyWordConcurrent
    // below: a foreign trigger on a butterfly-bearing object would trip the
    // owner-tag RELEASE_ASSERT (release crash on a legal racy program), a
    // butterfly-less shared trigger would publish without the per-event stop
    // or F2 fire (racing E4 transitions => torn {structure, butterfly} pair,
    // I21), and the plain nuke store collides with the locked protocols'
    // nuke-CASes (which RELEASE_ASSERT success). Route through the per-event
    // stop publication, exactly like convertToArrayStorageConcurrent.
    if (Options::useJSThreads()) [[unlikely]]
        return createArrayStorageConcurrent(vm, length, vectorLength);
#endif
    DeferGC deferGC(vm);
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    IndexingType oldType = indexingType();
    ASSERT_UNUSED(oldType, !hasIndexedProperties(oldType));

    Butterfly* newButterfly = createArrayStorageButterfly(vm, this, oldStructure, length, vectorLength, this->butterfly());
    ArrayStorage* result = newButterfly->arrayStorage();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, suggestedArrayStorageTransition(), &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
    return result;
}

#if USE(JSVALUE64)
// SPEC-objectmodel §4.6/I31 (review round 3): flag-on route for the
// blank-indexing transition INTO ArrayStorage. Mirrors
// convertToArrayStorageConcurrent: plan + allocate the fresh flat AS butterfly
// OUTSIDE a §10.6 per-event stop (O4 - the closure allocates nothing),
// re-verify + copy out-of-line properties + publish INSIDE it via the M8
// fenced nuke order (world stopped; PA-legal, I36). Shared triggers (foreign
// tag, SW=1, segmented word, or a butterfly-less instance whose N1 transition
// TID is foreign) fire BOTH TTL sets (F2/I10b/I13, chain-fired per F4) and
// publish FLAT (currentButterflyTID(), 1); owner triggers preserve the SW bit
// and do not fire (§5 per-object keying). Loser re-dispatch (re-plan), never a
// plain nuke + store. If a racer installed indexed storage first, defer to
// ensureArrayStorageSlow on the settled state (indexing types only move
// toward AS, so the recursion terminates).
ArrayStorage* JSObject::createArrayStorageConcurrent(VM& vm, unsigned length, unsigned vectorLength)
{
    ASSERT(Options::useJSThreads());
    auto* object = static_cast<JSObjectWithButterfly*>(this);
    DeferGC deferGC(vm);

    while (true) {
        StructureID oldStructureID = this->structureID(); // RAW bits (M5).
        if (oldStructureID.isNuked())
            continue; // A racing publication is mid-flight; re-plan on the settled state.
        Structure* oldStructure = oldStructureID.decode();
        if (hasIndexedProperties(oldStructure->indexingType())) {
            // A racing install won. AS-flavor racers leave the storage this
            // function promises; dense racers continue converting via the
            // generic slow path.
            return ensureArrayStorageSlow(vm);
        }

        Structure* newStructure;
        {
            DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
            newStructure = Structure::nonPropertyTransition(vm, oldStructure, suggestedArrayStorageTransition(), &deferred);
        }
        unsigned propertyCapacity = oldStructure->outOfLineCapacity();
        unsigned propertySize = oldStructure->outOfLineSize();

        // Fresh flat AS butterfly, fully initialized while private (header,
        // hole-cleared vector); out-of-line properties are copied INSIDE the
        // stop (they may be racing-written until the world stops).
        Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, true, ArrayStorage::sizeFor(vectorLength));
        ArrayStorage* newStorage = newButterfly->arrayStorage();
        newStorage->setLength(length);
        newStorage->setVectorLength(vectorLength);
        newStorage->m_sparseMap.clear();
        newStorage->m_numValuesInVector = 0;
        newStorage->m_indexBias = 0;
        for (size_t i = vectorLength; i--;)
            newStorage->m_vector[i].setWithoutWriteBarrier(JSValue());

        bool published = false;
        jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
            // ---- Re-verify inside the stop; allocate nothing (O4).
            if (this->structureID() != oldStructureID)
                return; // RESTART: a racing transition won before the stop landed.
            uint64_t word = object->taggedButterflyWord();
            bool segmented = isSegmentedButterfly(word);
            ButterflySpine* spine = segmented ? butterflySpine(word) : nullptr;
            Butterfly* flat = (!segmented && (word & butterflyPointerMask)) ? untaggedButterfly(word) : nullptr;
            RELEASE_ASSERT(flat || spine || !propertySize); // Out-of-line properties require storage.

            // Shared-trigger taxonomy (§5 F2 per-object keying): segmented and
            // SW=1/foreign-tagged words; butterfly-less instances key on the
            // structure's N1 transition TID.
            bool shared;
            if (!(word & butterflyPointerMask))
                shared = currentButterflyTID() != oldStructure->transitionThreadLocalTID();
            else
                shared = segmented || butterflySharedWrite(word) || butterflyTID(word) != currentButterflyTID();

            if (shared) {
                if (oldStructure->transitionThreadLocalIsStillValid() || oldStructure->writeThreadLocalIsStillValid())
                    oldStructure->fireTransitionThreadLocal(vm, "F2: shared blank-indexing transition into ArrayStorage (§4.6, review round 3)");
                if (newStructure != oldStructure
                    && (newStructure->transitionThreadLocalIsStillValid() || newStructure->writeThreadLocalIsStillValid()))
                    newStructure->fireTransitionThreadLocal(vm, "F2: shared blank-indexing transition into ArrayStorage (§4.6 target, review round 3)");
            }

            // ---- Copy out-of-line properties (segmented sources keep them in
            // fragments; the published AS butterfly is FLAT).
            for (unsigned k = 0; k < propertySize; ++k) {
                JSValue v = segmented
                    ? spine->outOfLineSlot(k)->get()
                    : flat->propertyStorage()[-static_cast<ptrdiff_t>(k) - 1].get();
                (newButterfly->propertyStorage() - (k + 1))->setWithoutWriteBarrier(v);
            }
            for (unsigned k = propertySize; k < propertyCapacity; ++k)
                (newButterfly->propertyStorage() - (k + 1))->clear();

            // ---- Publish (M8 fenced nuke order; world stopped - PA-legal,
            // I36). Shared triggers materialize FLAT (currentButterflyTID(),
            // 1) per §4.6; owner triggers preserve the source's SW bit.
            bool sharedWriteBit = shared || ((word & butterflyPointerMask) && butterflySharedWrite(word));
            setStructureIDDirectly(oldStructureID.nuke());
            WTF::storeStoreFence();
            std::bit_cast<Atomic<uint64_t>*>(object->butterflyAddress())->store(
                encodeButterfly(newButterfly, currentButterflyTID(), sharedWriteBit), std::memory_order_seq_cst);
            WTF::storeStoreFence();
            setStructure(vm, newStructure);
            published = true;
        }));

        if (published) {
            // Publication barrier, like setButterfly (I25); superseded storage
            // (flat butterfly or spine + fragments) is never written again -
            // stale readers see a frozen snapshot (I7).
            vm.writeBarrier(this);
            return newStorage;
        }
        // RESTART: the step-1 allocations drop unreferenced; re-plan.
    }
}
#endif // USE(JSVALUE64)

ArrayStorage* JSObject::createInitialArrayStorage(VM& vm)
{
    return createArrayStorage(
        vm, 0, ArrayStorage::optimalVectorLength(0, structure()->outOfLineCapacity(), 0));
}

ContiguousJSValues JSObject::convertUndecidedToInt32(VM& vm)
{
    ASSERT(hasUndecided(indexingType()));

#if USE(JSVALUE64)
    // §4.7/I28 (review round 1): flag-on, in-place relabels run per-event STW.
    if (Options::useJSThreads()) [[unlikely]] {
        relabelIndexingShapeConcurrent(vm, TransitionKind::AllocateInt32);
        // AB18-F: a racer may have settled the shape at/past the target (the
        // relabel early-returns in that case). Re-check the settled shape and
        // bail to the generic path unless it is the expected target family —
        // returning the typed accessor over racer-settled lanes would mislabel
        // them. Mirrors the segmented bail below; callers already tolerate the
        // empty return. Shape publishes only happen inside §10.6 stops, and we
        // do not safepoint between this check and the accessor read.
        if (!hasInt32(indexingType()))
            return ContiguousJSValues();
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word))
            return ContiguousJSValues(); // Segmented lanes are reached through the spine (§9.5); flat-only callers bail to generic paths.
        return untaggedButterfly(word)->contiguousInt32();
    }
#endif
    Butterfly* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;)
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());

    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateInt32, &deferred));
    }
    return this->butterfly()->contiguousInt32();
}

ContiguousDoubles JSObject::convertUndecidedToDouble(VM& vm)
{
    ASSERT(Options::allowDoubleShape());
    ASSERT(hasUndecided(indexingType()));

#if USE(JSVALUE64)
    // §4.7/I28 (review round 1): flag-on, in-place relabels run per-event STW.
    if (Options::useJSThreads()) [[unlikely]] {
        relabelIndexingShapeConcurrent(vm, TransitionKind::AllocateDouble);
        if (!hasDouble(indexingType()))
            return ContiguousDoubles(); // AB18-F settled-shape bail; see convertUndecidedToInt32.
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word))
            return ContiguousDoubles(); // See convertUndecidedToInt32.
        return untaggedButterfly(word)->contiguousDouble();
    }
#endif
    auto* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;)
        butterfly->contiguousDouble().at(this, i) = PNaN;
    
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateDouble, &deferred));
    }
    return this->butterfly()->contiguousDouble();
}

ContiguousJSValues JSObject::convertUndecidedToContiguous(VM& vm)
{
    ASSERT(hasUndecided(indexingType()));

#if USE(JSVALUE64)
    // §4.7/I28 (review round 1): flag-on, in-place relabels run per-event STW.
    if (Options::useJSThreads()) [[unlikely]] {
        relabelIndexingShapeConcurrent(vm, TransitionKind::AllocateContiguous);
        if (!hasContiguous(indexingType()))
            return ContiguousJSValues(); // AB18-F settled-shape bail; see convertUndecidedToInt32.
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word))
            return ContiguousJSValues(); // See convertUndecidedToInt32.
        return untaggedButterfly(word)->contiguous();
    }
#endif
    auto* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;)
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(JSValue());

    WTF::storeStoreFence();
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred));
    }
    return this->butterfly()->contiguous();
}

ArrayStorage* JSObject::constructConvertedArrayStorageWithoutCopyingElements(VM& vm, unsigned neededLength)
{
    Structure* structure = this->structure();
    unsigned publicLength = this->butterfly()->publicLength();
    unsigned propertyCapacity = structure->outOfLineCapacity();

    Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, true, ArrayStorage::sizeFor(neededLength));

    // memcpy is fine since newButterfly is not tied to any object yet.
    memcpy(
        static_cast<JSValue*>(newButterfly->base(0, propertyCapacity)),
        static_cast<JSValue*>(this->butterfly()->base(0, propertyCapacity)),
        propertyCapacity * sizeof(EncodedJSValue));

    ArrayStorage* newStorage = newButterfly->arrayStorage();
    newStorage->setVectorLength(neededLength);
    newStorage->setLength(publicLength);
    newStorage->m_sparseMap.clear();
    newStorage->m_indexBias = 0;
    newStorage->m_numValuesInVector = 0;
    
    return newStorage;
}

ArrayStorage* JSObject::convertUndecidedToArrayStorage(VM& vm, TransitionKind transition)
{
#if USE(JSVALUE64)
    // SPEC-objectmodel §4.6 stops (Task 8, I31/I10): flag-on, transitions INTO
    // ArrayStorage copy + publish under a per-event §10.6 stop.
    if (Options::useJSThreads()) [[unlikely]]
        return convertToArrayStorageConcurrent(vm, transition);
#endif
    DeferGC deferGC(vm);
    ASSERT(hasUndecided(indexingType()));

    unsigned vectorLength = this->butterfly()->vectorLength();
    ArrayStorage* storage = constructConvertedArrayStorageWithoutCopyingElements(vm, vectorLength);
    
    for (unsigned i = vectorLength; i--;)
        storage->m_vector[i].setWithoutWriteBarrier(JSValue());
    
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, storage->butterfly());
        setStructure(vm, newStructure);
    }
    return storage;
}

ArrayStorage* JSObject::convertUndecidedToArrayStorage(VM& vm)
{
    return convertUndecidedToArrayStorage(vm, suggestedArrayStorageTransition());
}

ContiguousDoubles JSObject::convertInt32ToDouble(VM& vm)
{
    ASSERT(hasInt32(indexingType()));
    ASSERT(!isCopyOnWrite(indexingMode()));

#if USE(JSVALUE64)
    // §4.7/I28 (review round 1): flag-on, in-place relabels run per-event STW.
    if (Options::useJSThreads()) [[unlikely]] {
        relabelIndexingShapeConcurrent(vm, TransitionKind::AllocateDouble);
        if (!hasDouble(indexingType()))
            return ContiguousDoubles(); // AB18-F settled-shape bail; see convertUndecidedToInt32.
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word))
            return ContiguousDoubles(); // See convertUndecidedToInt32.
        return untaggedButterfly(word)->contiguousDouble();
    }
#endif
    auto* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;) {
        WriteBarrier<Unknown>* current = &butterfly->contiguous().atUnsafe(i);
        double* currentAsDouble = std::bit_cast<double*>(current);
        JSValue v = current->get();
        // NOTE: Since this may be used during initialization, v could be garbage. If it's garbage,
        // that means it will be overwritten later.
        if (!v.isInt32()) {
            *currentAsDouble = PNaN;
            continue;
        }
        *currentAsDouble = v.asInt32();
    }
    
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateDouble, &deferred));
    }
    return this->butterfly()->contiguousDouble();
}

ContiguousJSValues JSObject::convertInt32ToContiguous(VM& vm)
{
    ASSERT(hasInt32(indexingType()));

#if USE(JSVALUE64)
    // §4.7/I28 (review round 1): no lane rewrite (boxed Int32 lanes are valid
    // Contiguous lanes), but the structure publication on a possibly shared
    // object still goes through the per-event stop + F2 driver.
    if (Options::useJSThreads()) [[unlikely]] {
        relabelIndexingShapeConcurrent(vm, TransitionKind::AllocateContiguous);
        if (!hasContiguous(indexingType()))
            return ContiguousJSValues(); // AB18-F settled-shape bail; see convertUndecidedToInt32.
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word))
            return ContiguousJSValues(); // See convertUndecidedToInt32.
        return untaggedButterfly(word)->contiguous();
    }
#endif
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred));
    }
    return this->butterfly()->contiguous();
}

ArrayStorage* JSObject::convertInt32ToArrayStorage(VM& vm, TransitionKind transition)
{
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] // §4.6 stops (Task 8)
        return convertToArrayStorageConcurrent(vm, transition);
#endif
    DeferGC deferGC(vm);
    ASSERT(hasInt32(indexingType()));

    unsigned vectorLength = this->butterfly()->vectorLength();
    ArrayStorage* newStorage = constructConvertedArrayStorageWithoutCopyingElements(vm, vectorLength);
    auto* butterfly = this->butterfly();
    for (unsigned i = 0; i < vectorLength; i++) {
        JSValue v = butterfly->contiguous().at(this, i).get();
        newStorage->m_vector[i].setWithoutWriteBarrier(v);
        if (v)
            newStorage->m_numValuesInVector++;
    }
    
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newStorage->butterfly());
        setStructure(vm, newStructure);
    }
    return newStorage;
}

ArrayStorage* JSObject::convertInt32ToArrayStorage(VM& vm)
{
    return convertInt32ToArrayStorage(vm, suggestedArrayStorageTransition());
}

ContiguousJSValues JSObject::convertDoubleToContiguous(VM& vm)
{
    ASSERT(hasDouble(indexingType()));
    ASSERT(!isCopyOnWrite(indexingMode()));

#if USE(JSVALUE64)
    // §4.7/I28 (review round 1): flag-on, in-place relabels run per-event STW.
    if (Options::useJSThreads()) [[unlikely]] {
        relabelIndexingShapeConcurrent(vm, TransitionKind::AllocateContiguous);
        if (!hasContiguous(indexingType()))
            return ContiguousJSValues(); // AB18-F settled-shape bail; see convertUndecidedToInt32.
        uint64_t word = taggedButterflyWord();
        if (isSegmentedButterfly(word))
            return ContiguousJSValues(); // See convertUndecidedToInt32.
        return untaggedButterfly(word)->contiguous();
    }
#endif
    auto* butterfly = this->butterfly();
    for (unsigned i = butterfly->vectorLength(); i--;) {
        double* current = &butterfly->contiguousDouble().atUnsafe(i);
        WriteBarrier<Unknown>* currentAsValue = std::bit_cast<WriteBarrier<Unknown>*>(current);
        double value = *current;
        if (value != value) {
            currentAsValue->clear();
            continue;
        }
        JSValue v = JSValue(JSValue::EncodeAsDouble, value);
        currentAsValue->setWithoutWriteBarrier(v);
    }
    
    WTF::storeStoreFence();
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::AllocateContiguous, &deferred));
    }
    return this->butterfly()->contiguous();
}

ArrayStorage* JSObject::convertDoubleToArrayStorage(VM& vm, TransitionKind transition)
{
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] // §4.6 stops (Task 8)
        return convertToArrayStorageConcurrent(vm, transition);
#endif
    DeferGC deferGC(vm);
    ASSERT(hasDouble(indexingType()));

    unsigned vectorLength = this->butterfly()->vectorLength();
    ArrayStorage* newStorage = constructConvertedArrayStorageWithoutCopyingElements(vm, vectorLength);
    auto* butterfly = this->butterfly();
    for (unsigned i = 0; i < vectorLength; i++) {
        double value = butterfly->contiguousDouble().at(this, i);
        if (value != value) {
            newStorage->m_vector[i].clear();
            continue;
        }
        newStorage->m_vector[i].setWithoutWriteBarrier(JSValue(JSValue::EncodeAsDouble, value));
        newStorage->m_numValuesInVector++;
    }
    
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newStorage->butterfly());
        setStructure(vm, newStructure);
    }
    return newStorage;
}

ArrayStorage* JSObject::convertDoubleToArrayStorage(VM& vm)
{
    return convertDoubleToArrayStorage(vm, suggestedArrayStorageTransition());
}

ArrayStorage* JSObject::convertContiguousToArrayStorage(VM& vm, TransitionKind transition)
{
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] // §4.6 stops (Task 8)
        return convertToArrayStorageConcurrent(vm, transition);
#endif
    DeferGC deferGC(vm);
    ASSERT(hasContiguous(indexingType()));

    unsigned vectorLength = this->butterfly()->vectorLength();
    ArrayStorage* newStorage = constructConvertedArrayStorageWithoutCopyingElements(vm, vectorLength);
    auto* butterfly = this->butterfly();
    for (unsigned i = 0; i < vectorLength; i++) {
        JSValue v = butterfly->contiguous().at(this, i).get();
        newStorage->m_vector[i].setWithoutWriteBarrier(v);
        if (v)
            newStorage->m_numValuesInVector++;
    }

    // While we modify the butterfly of Contiguous Array, we do not take any cellLock here. This is because
    // (1) the old butterfly is not changed and (2) new butterfly is not changed after it is exposed to
    // the collector.
    // The mutator performs the following operations are sequentially executed by using storeStoreFence.
    //
    //     CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure
    //
    // Meanwhile the collector performs the following steps sequentially:
    //
    //     ReadStructureEarly ReadButterfly ReadStructureLate
    //
    // We list up all the patterns by writing a tiny script, and ensure all the cases are categorized into BEFORE, AFTER, and IGNORE.
    //
    // CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureEarly ReadButterfly ReadStructureLate: AFTER, trivially
    // CreateNewButterfly NukeStructure ChangeButterfly ReadStructureEarly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ChangeButterfly ReadStructureEarly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ChangeButterfly ReadStructureEarly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ChangeButterfly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ChangeButterfly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ChangeButterfly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ReadButterfly ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ReadButterfly ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read early
    // CreateNewButterfly NukeStructure ReadStructureEarly ReadButterfly ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read early
    // CreateNewButterfly ReadStructureEarly NukeStructure ChangeButterfly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because early and late structures don't match
    // CreateNewButterfly ReadStructureEarly NukeStructure ChangeButterfly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // CreateNewButterfly ReadStructureEarly NukeStructure ChangeButterfly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly NukeStructure ReadButterfly ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // CreateNewButterfly ReadStructureEarly NukeStructure ReadButterfly ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly NukeStructure ReadButterfly ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly ReadButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // CreateNewButterfly ReadStructureEarly ReadButterfly NukeStructure ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly ReadButterfly NukeStructure ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // CreateNewButterfly ReadStructureEarly ReadButterfly ReadStructureLate NukeStructure ChangeButterfly PutNewStructure: BEFORE, trivially.
    // ReadStructureEarly CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure ReadButterfly ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly CreateNewButterfly NukeStructure ChangeButterfly ReadButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly CreateNewButterfly NukeStructure ChangeButterfly ReadButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly NukeStructure ReadButterfly ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly CreateNewButterfly NukeStructure ReadButterfly ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly NukeStructure ReadButterfly ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly ReadButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly CreateNewButterfly ReadButterfly NukeStructure ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly ReadButterfly NukeStructure ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly CreateNewButterfly ReadButterfly ReadStructureLate NukeStructure ChangeButterfly PutNewStructure: BEFORE, CreateNewButterfly is not visible to collector.
    // ReadStructureEarly ReadButterfly CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure ReadStructureLate: IGNORE, because early and late structures don't match
    // ReadStructureEarly ReadButterfly CreateNewButterfly NukeStructure ChangeButterfly ReadStructureLate PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly ReadButterfly CreateNewButterfly NukeStructure ReadStructureLate ChangeButterfly PutNewStructure: IGNORE, because nuked structure read late
    // ReadStructureEarly ReadButterfly CreateNewButterfly ReadStructureLate NukeStructure ChangeButterfly PutNewStructure: BEFORE, CreateNewButterfly is not visible to collector.
    // ReadStructureEarly ReadButterfly ReadStructureLate CreateNewButterfly NukeStructure ChangeButterfly PutNewStructure: BEFORE, trivially.

    ASSERT(newStorage->butterfly() != butterfly);
    StructureID oldStructureID = this->structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);

        // Ensure new Butterfly initialization is correctly done before exposing it to the concurrent threads.
        if (isX86() || vm.heap.mutatorShouldBeFenced())
            WTF::storeStoreFence();
        nukeStructureAndSetButterfly(vm, oldStructureID, newStorage->butterfly());
        setStructure(vm, newStructure);
    }
    
    return newStorage;
}

ArrayStorage* JSObject::convertContiguousToArrayStorage(VM& vm)
{
    return convertContiguousToArrayStorage(vm, suggestedArrayStorageTransition());
}

#if USE(JSVALUE64)
// SPEC-objectmodel §4.6 "stops" (Task 8; I31/I10): flag-on, EVERY transition
// INTO ArrayStorage plans and allocates OUTSIDE a §10.6 per-event stop (O4:
// the closure allocates nothing) and copies + publishes INSIDE it. When the
// trigger is shared (foreign tag, SW=1, or segmented - the §4.6 "shared
// transitions INTO AS" case) both TTL sets fire in the same stop (F2/I10b/I13)
// and the publication is a FLAT butterfly tagged (currentButterflyTID(), 1)
// (AS never segments - I31). Owner non-shared triggers publish (currentTID,
// SW-preserved) without firing (F2 does not fire for the tag owner).
//
// Publication uses the M8 fenced nuke order - nuke, fence, 64-bit butterfly
// word store, fence, new structure - which is legal on PreciseAllocation
// cells too (I36) and race-free here because the world is stopped (concurrent
// markers tolerate it via the standard nuke/didRace protocol, exactly like
// today's nukeStructureAndSetButterfly).
ArrayStorage* JSObject::convertToArrayStorageConcurrent(VM& vm, TransitionKind transition)
{
    ASSERT(Options::useJSThreads());
    ASSERT(!isCopyOnWrite(indexingMode())); // Callers materialize first (ensureWritable / §4.8).
    auto* object = static_cast<JSObjectWithButterfly*>(this);
    DeferGC deferGC(vm);

    while (true) {
        StructureID oldStructureID = this->structureID(); // RAW bits (M5).
        if (oldStructureID.isNuked())
            continue; // A racing publication is mid-flight; re-plan on the settled state.
        Structure* oldStructure = oldStructureID.decode();
        IndexingType sourceType = oldStructure->indexingType();
        ASSERT(hasUndecided(sourceType) || hasInt32(sourceType) || hasDouble(sourceType) || hasContiguous(sourceType));

        // ---- Plan + allocate outside the stop (O4). The vector length is
        // re-validated inside; growth in between => refit (re-plan).
        uint64_t planningWord = object->taggedButterflyWord();
        RELEASE_ASSERT(planningWord & butterflyPointerMask); // Converters' precondition: indexed storage exists.
        unsigned planningVectorLength = isSegmentedButterfly(planningWord)
            ? butterflySpine(planningWord)->vectorLength
            : untaggedButterfly(planningWord)->vectorLength();

        Structure* newStructure;
        {
            DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
            newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        }
        unsigned propertyCapacity = oldStructure->outOfLineCapacity();
        unsigned propertySize = oldStructure->outOfLineSize();
        Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, true, ArrayStorage::sizeFor(planningVectorLength));
        ArrayStorage* newStorage = newButterfly->arrayStorage();

        bool published = false;
        jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
            // ---- Re-verify inside the stop; allocate nothing (O4).
            if (this->structureID() != oldStructureID)
                return; // RESTART: a racing transition won before the stop landed.
            uint64_t word = object->taggedButterflyWord();
            RELEASE_ASSERT(word & butterflyPointerMask);
            bool segmented = isSegmentedButterfly(word);
            ButterflySpine* spine = segmented ? butterflySpine(word) : nullptr;
            Butterfly* flat = segmented ? nullptr : untaggedButterfly(word);
            unsigned vectorLength = segmented ? spine->vectorLength : flat->vectorLength();
            if (vectorLength > planningVectorLength)
                return; // Refit: the step-1 allocation no longer fits; re-plan outside.
            unsigned publicLength = segmented ? segmentedPublicLength(spine) : flat->publicLength();
            bool shared = segmented || butterflySharedWrite(word) || butterflyTID(word) != currentButterflyTID();

            // ---- F2 (I10b/I13): shared triggers fire BOTH sets on source and
            // target in this same stop (fireTransitionThreadLocal also fires
            // writeThreadLocal and chain-fires per F4). The tag owner does not
            // fire (§5 F2 per-object keying).
            if (shared) {
                if (oldStructure->transitionThreadLocalIsStillValid() || oldStructure->writeThreadLocalIsStillValid())
                    oldStructure->fireTransitionThreadLocal(vm, "F2: shared transition into ArrayStorage (§4.6)");
                if (newStructure != oldStructure
                    && (newStructure->transitionThreadLocalIsStillValid() || newStructure->writeThreadLocalIsStillValid()))
                    newStructure->fireTransitionThreadLocal(vm, "F2: shared transition into ArrayStorage (§4.6 target)");
            }

            // ---- Copy out-of-line properties (segmented sources keep them in
            // fragments; the published AS butterfly is FLAT, so they move back
            // into descending flat slots).
            for (unsigned k = 0; k < propertySize; ++k) {
                JSValue v = segmented
                    ? spine->outOfLineSlot(k)->get()
                    : flat->propertyStorage()[-static_cast<ptrdiff_t>(k) - 1].get();
                (newButterfly->propertyStorage() - (k + 1))->setWithoutWriteBarrier(v);
            }
            for (unsigned k = propertySize; k < propertyCapacity; ++k)
                (newButterfly->propertyStorage() - (k + 1))->clear();

            // ---- ArrayStorage header + elements (shape-keyed; Double slots
            // are raw 8B lanes, §4.7 - boxed here exactly like today's
            // convertDoubleToArrayStorage).
            newStorage->setVectorLength(vectorLength);
            newStorage->setLength(publicLength);
            newStorage->m_sparseMap.clear();
            newStorage->m_indexBias = 0;
            unsigned numValues = 0;
            for (unsigned i = 0; i < vectorLength; ++i) {
                JSValue v;
                if (hasUndecided(sourceType))
                    v = JSValue();
                else if (hasDouble(sourceType)) {
                    double d = segmented ? *std::bit_cast<const double*>(spine->indexedSlot(i)) : flat->contiguousDouble().atUnsafe(i);
                    v = d == d ? JSValue(JSValue::EncodeAsDouble, d) : JSValue();
                } else
                    v = segmented ? spine->indexedSlot(i)->get() : flat->contiguous().atUnsafe(i).get();
                if (v) {
                    newStorage->m_vector[i].setWithoutWriteBarrier(v);
                    ++numValues;
                } else
                    newStorage->m_vector[i].clear();
            }
            newStorage->m_numValuesInVector = numValues;

            // ---- Publish (M8 fenced nuke order; world stopped). Shared
            // triggers materialize FLAT (currentButterflyTID(), 1) per §4.6;
            // owner triggers preserve the SW bit.
            bool sharedWriteBit = shared || butterflySharedWrite(word);
            setStructureIDDirectly(oldStructureID.nuke());
            WTF::storeStoreFence();
            std::bit_cast<Atomic<uint64_t>*>(object->butterflyAddress())->store(
                encodeButterfly(newButterfly, currentButterflyTID(), sharedWriteBit), std::memory_order_seq_cst);
            WTF::storeStoreFence();
            setStructure(vm, newStructure);
            published = true;
        }));

        if (published) {
            // Publication barrier, like setButterfly (I25); the superseded
            // storage (flat butterfly or spine + fragments) is never written
            // again - stale readers see a frozen snapshot (I7).
            vm.writeBarrier(this);
            return newStorage;
        }
        // Refit or RESTART: the step-1 allocations are discarded unreferenced
        // (GC reclaims them); re-plan from the fresh settled state.
    }
}

// SPEC-objectmodel §4.7/I28 (review round 1): flag-on driver for the in-place
// indexing-shape relabels. The legacy bodies rewrite every element lane of the
// CURRENT storage in place (boxed JSValue <-> raw double) and then
// setStructure - a lock-free reader walking the same storage mid-rewrite would
// reinterpret a half-rewritten lane (type-confused pointer deref). Flag-on the
// relabel therefore runs as a per-event §10.6 stop, mirroring
// convertToArrayStorageConcurrent: plan + allocate (the target Structure)
// OUTSIDE the stop (O4: the closure allocates nothing), re-verify + rewrite +
// setStructure INSIDE it. Shared triggers (foreign tag, SW=1, segmented - the
// I28 "Double-touching shape change on a shared object" case) fire BOTH TTL
// sets (F2/I10b/I13); the tag owner does not fire (§5 per-object keying). The
// butterfly word is untouched (I16; flat AND segmented words supported - the
// segmented leg rewrites through the loaded spine's fragment slots), so no
// nuke is needed; trySegmentedTransition's hasDouble(source)==hasDouble(target)
// RELEASE_ASSERT relies on exactly this stop existing for segmented relabels.
void JSObject::relabelIndexingShapeConcurrent(VM& vm, TransitionKind transition)
{
    ASSERT(Options::useJSThreads());
    ASSERT(!isCopyOnWrite(indexingMode())); // Callers materialize first (ensureWritable / §4.8).
    auto* object = static_cast<JSObjectWithButterfly*>(this);
    // DeferGCForAWhile, not DeferGC: this relabel runs under callers that are
    // themselves inside an ObjectInitializationScope no-GC region
    // (initializeIndex -> setIndexQuicklyToUndecided -> convertUndecidedTo*),
    // and ~DeferGC's decrementDeferralDepthAndGCIfNeeded() would COLLECT at
    // function exit while AssertNoGC is still in effect (Heap.cpp
    // collectIfNecessaryOrDefer assertion). DeferGCForAWhile conducts the
    // deferral past this scope; the deferred collection runs at the caller's
    // next natural GC point. Allocation coverage is unchanged
    // (Structure::nonPropertyTransition + the loop's transient allocations
    // stay deferred).
    DeferGCForAWhile deferGC(vm);

    while (true) {
        StructureID oldStructureID = this->structureID(); // RAW bits (M5).
        if (oldStructureID.isNuked())
            continue; // A racing publication is mid-flight; re-plan on the settled state.
        Structure* oldStructure = oldStructureID.decode();
        IndexingType sourceType = oldStructure->indexingType();
        // A racing thread may have advanced the shape to (or past) this
        // relabel's target between the caller's shape check and this loop
        // iteration (first entry, after spinning on a nuked StructureID, or
        // after a RESTART). The relabel lattice is monotone
        // (Undecided < Int32 < Double < Contiguous < ArrayStorage/SlowPut),
        // so if the settled shape no longer needs this transition, the racer's
        // published result already covers it: return without transitioning
        // instead of planning a transition from an invalid source (which would
        // assert here in debug and type-confuse the lane rewrite in release).
        //
        // The early return is sound without a local acquire fence ONLY because
        // every flag-on indexing-shape publication happens inside a §10.6 stop
        // (this relabel loop, convertToArrayStorageConcurrent, the CoW
        // materialize install): this thread is an entered mutator, so it was
        // parked for the racer's stop, and the park/resume handshake orders
        // our loop-top structureID read above after the racer's fenced lane
        // rewrite. The racer's storeStoreFence is store-side only and would
        // NOT by itself provide that edge. If a future path ever publishes an
        // indexing shape outside a stop, this return becomes unsafe.
        bool relabelStillNeeded;
        switch (transition) {
        case TransitionKind::AllocateInt32:
            relabelStillNeeded = hasUndecided(sourceType);
            break;
        case TransitionKind::AllocateDouble:
            relabelStillNeeded = hasUndecided(sourceType) || hasInt32(sourceType);
            break;
        case TransitionKind::AllocateContiguous:
            relabelStillNeeded = hasUndecided(sourceType) || hasInt32(sourceType) || hasDouble(sourceType);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            relabelStillNeeded = false;
            break;
        }
        if (!relabelStillNeeded)
            return; // Racer already settled the object at/past the target shape; its stop fired F2 and the write barrier.
        ASSERT(hasUndecided(sourceType) || hasInt32(sourceType) || hasDouble(sourceType));

        Structure* newStructure;
        {
            DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
            newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        }
        IndexingType targetType = newStructure->indexingType();
        ASSERT(hasInt32(targetType) || hasDouble(targetType) || hasContiguous(targetType));

        bool published = false;
        jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
            // ---- Re-verify inside the stop; allocate nothing (O4).
            if (this->structureID() != oldStructureID)
                return; // RESTART: a racing transition won before the stop landed.
            uint64_t word = object->taggedButterflyWord();
            RELEASE_ASSERT(word & butterflyPointerMask); // Relabel precondition: indexed storage exists.
            bool segmented = isSegmentedButterfly(word);
            ButterflySpine* spine = segmented ? butterflySpine(word) : nullptr;
            Butterfly* flat = segmented ? nullptr : untaggedButterfly(word);
            unsigned vectorLength = segmented ? spine->vectorLength : flat->vectorLength();
            bool shared = segmented || butterflySharedWrite(word) || butterflyTID(word) != currentButterflyTID();

            // ---- F2 (I10b/I13): shared triggers fire BOTH sets on source and
            // target in this same stop (chain-fired per F4 inside).
            if (shared) {
                if (oldStructure->transitionThreadLocalIsStillValid() || oldStructure->writeThreadLocalIsStillValid())
                    oldStructure->fireTransitionThreadLocal(vm, "F2: shared in-place indexing-shape relabel (§4.7/I28)");
                if (newStructure != oldStructure
                    && (newStructure->transitionThreadLocalIsStillValid() || newStructure->writeThreadLocalIsStillValid()))
                    newStructure->fireTransitionThreadLocal(vm, "F2: shared in-place indexing-shape relabel (§4.7/I28 target)");
            }

            // ---- Rewrite the lanes, world stopped (raw 64-bit lanes; the
            // logic matches the legacy bodies bit-for-bit per source/target).
            auto laneAt = [&](unsigned i) -> uint64_t* {
                if (segmented)
                    return std::bit_cast<uint64_t*>(spine->indexedSlot(i));
                return std::bit_cast<uint64_t*>(flat->indexingPayload<double>() + i);
            };
            for (unsigned i = 0; i < vectorLength; ++i) {
                uint64_t* lane = laneAt(i);
                if (hasUndecided(sourceType)) {
                    if (hasDouble(targetType))
                        *std::bit_cast<double*>(lane) = PNaN;
                    else
                        *lane = JSValue::encode(JSValue()); // Hole, as convertUndecidedToInt32/Contiguous.
                    continue;
                }
                if (hasInt32(sourceType)) {
                    if (hasDouble(targetType)) {
                        JSValue v = JSValue::decode(*lane);
                        // NOTE: mid-initialization lanes may be garbage (cf.
                        // legacy convertInt32ToDouble); overwritten later.
                        *std::bit_cast<double*>(lane) = v.isInt32() ? v.asInt32() : PNaN;
                    }
                    // Int32 -> Contiguous: boxed Int32 lanes are already valid
                    // Contiguous lanes; nothing to rewrite.
                    continue;
                }
                ASSERT(hasDouble(sourceType) && hasContiguous(targetType));
                double d = *std::bit_cast<double*>(lane);
                *lane = d == d ? JSValue::encode(JSValue(JSValue::EncodeAsDouble, d)) : JSValue::encode(JSValue());
            }

            WTF::storeStoreFence(); // Lanes before the type publish (M2-style).
            setStructure(vm, newStructure); // Butterfly word untouched (I16); no nuke needed.
            published = true;
        }));
        if (published) {
            vm.writeBarrier(this); // Cheap conservative re-grey; no cell values were introduced (Int32/Double lanes carry no cells).
            return;
        }
        // RESTART: re-plan from the fresh settled state.
    }
}
#endif // USE(JSVALUE64)

void JSObject::convertToIndexingTypeIfNeeded(VM& vm, IndexingType nextType)
{
    IndexingType currentType = indexingType();
    if (currentType == nextType)
        return;
    switch (currentType) {
    case ArrayWithUndecided: {
        switch (nextType) {
        case ArrayWithInt32:
            convertUndecidedToInt32(vm);
            break;
        case ArrayWithDouble:
            convertUndecidedToDouble(vm);
            break;
        case ArrayWithContiguous:
            convertUndecidedToContiguous(vm);
            break;
        case ArrayWithArrayStorage:
            convertUndecidedToArrayStorage(vm);
            break;
        }
        break;
    }
    case ArrayWithInt32: {
        switch (nextType) {
        case ArrayWithDouble:
            convertInt32ToDouble(vm);
            break;
        case ArrayWithContiguous:
            convertInt32ToContiguous(vm);
            break;
        case ArrayWithArrayStorage:
            convertInt32ToArrayStorage(vm);
            break;
        }
        break;
    }
    case ArrayWithDouble: {
        switch (nextType) {
        case ArrayWithContiguous:
            convertDoubleToContiguous(vm);
            break;
        case ArrayWithArrayStorage:
            convertDoubleToArrayStorage(vm);
            break;
        }
        break;
    }
    case ArrayWithContiguous: {
        switch (nextType) {
        case ArrayWithArrayStorage:
            convertContiguousToArrayStorage(vm);
            break;
        }
        break;
    }
    }
}

void JSObject::convertUndecidedForValue(VM& vm, JSValue value)
{
    IndexingType type = indexingTypeForValue(value);
    if (type == Int32Shape) {
        convertUndecidedToInt32(vm);
        return;
    }
    
    if (type == DoubleShape) {
        ASSERT(Options::allowDoubleShape());
        convertUndecidedToDouble(vm);
        return;
    }

    ASSERT(type == ContiguousShape);
    convertUndecidedToContiguous(vm);
}

void JSObject::createInitialForValueAndSet(VM& vm, unsigned index, JSValue value)
{
    if (value.isInt32()) {
        createInitialInt32(vm, index + 1).at(this, index).set(vm, this, value);
        return;
    }
    
    if (value.isDouble() && Options::allowDoubleShape()) {
        double doubleValue = value.asNumber();
        if (doubleValue == doubleValue) {
            createInitialDouble(vm, index + 1).at(this, index) = doubleValue;
            return;
        }
    }
    
    createInitialContiguous(vm, index + 1).at(this, index).set(vm, this, value);
}

void JSObject::convertInt32ForValue(VM& vm, JSValue value)
{
    ASSERT(!value.isInt32());
    
    if (value.isDouble() && !std::isnan(value.asDouble()) && Options::allowDoubleShape()) {
        convertInt32ToDouble(vm);
        return;
    }

    convertInt32ToContiguous(vm);
}

void JSObject::convertFromCopyOnWrite(VM& vm)
{
#if USE(JSVALUE64)
    // The flag-on route must precede the single-threaded asserts below: a
    // racing §4.8 materializer can legally win between the caller's CoW check
    // and this call (TOCTOU), making both asserts stale-state reads that
    // abort a legal racy program (mc-lock-cow-materialize-race).
    // materializeCopyOnWriteButterflyConcurrent re-verifies under the cell
    // lock and no-ops if the object already left the CoW regime.
    //
    // SPEC-objectmodel §4.8/I35 (review round 3): flag-on, the OWNER's
    // materialization must go through the same cell-locked serialization
    // point as the foreign §4.8 route. The plain nuke + CAS publication below
    // races the foreign materializer (ensureSharedWriteBit ->
    // tryMaterializeCopyOnWriteButterflyForSharedWrite), whose nuke-CAS and
    // word-stability RELEASE_ASSERTs are sound precisely because NO lock-free
    // CoW publication exists: owner-vs-foreign races would otherwise crash on
    // a legal program (their nuke CAS fails) or publish a doubly-nuked
    // {structure, butterfly} pair. The driver loops the cell-locked
    // materializer until the object is no longer CoW; the WINNER may be a
    // foreign thread, so callers must re-dispatch on the fresh tag before
    // dereferencing flat storage as their own (the §3 probes at every flat
    // fast path do exactly that).
    if (Options::useJSThreads()) [[unlikely]] {
        materializeCopyOnWriteButterflyConcurrent(vm, static_cast<JSObjectWithButterfly*>(this));
        return;
    }
#endif

    ASSERT(isCopyOnWrite(indexingMode()));
    ASSERT(structure()->indexingMode() == indexingMode());

    const bool hasIndexingHeader = true;
    Butterfly* oldButterfly = this->butterfly();
    size_t propertyCapacity = 0;
    unsigned newVectorLength = Butterfly::optimalContiguousVectorLength(propertyCapacity, std::min<size_t>(nextLength(oldButterfly->vectorLength()), MAX_STORAGE_VECTOR_LENGTH));
    Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, 0, propertyCapacity, hasIndexingHeader, newVectorLength * sizeof(JSValue));

    // memcpy is fine since newButterfly is not tied to any object yet.
    memcpy(newButterfly->propertyStorage(), oldButterfly->propertyStorage(), oldButterfly->vectorLength() * sizeof(JSValue) + sizeof(IndexingHeader));

    WTF::storeStoreFence();
    TransitionKind transition = ([&] () {
        switch (indexingType()) {
        case ArrayWithInt32:
            return TransitionKind::AllocateInt32;
        case ArrayWithDouble:
            return TransitionKind::AllocateDouble;
        case ArrayWithContiguous:
            return TransitionKind::AllocateContiguous;
        default:
            RELEASE_ASSERT_NOT_REACHED();
            return TransitionKind::AllocateContiguous;
        }
    })();
    StructureID oldStructureID = structureID();
    Structure* oldStructure = oldStructureID.decode();
    {
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, transition, &deferred);
        nukeStructureAndSetButterfly(vm, oldStructureID, newButterfly);
        setStructure(vm, newStructure);
    }
}

void JSObject::setIndexQuicklyToUndecided(VM& vm, unsigned index, JSValue value)
{
    ASSERT(index < this->butterfly()->publicLength());
    ASSERT(index < this->butterfly()->vectorLength());
    convertUndecidedForValue(vm, value);
    setIndexQuickly(vm, index, value);
}

void JSObject::convertInt32ToDoubleOrContiguousWhilePerformingSetIndex(VM& vm, unsigned index, JSValue value)
{
    ASSERT(!value.isInt32());
    convertInt32ForValue(vm, value);
    setIndexQuickly(vm, index, value);
}

void JSObject::convertDoubleToContiguousWhilePerformingSetIndex(VM& vm, unsigned index, JSValue value)
{
    ASSERT(!value.isNumber() || value.asNumber() != value.asNumber());
    convertDoubleToContiguous(vm);
    setIndexQuickly(vm, index, value);
}

ContiguousJSValues JSObject::tryMakeWritableInt32Slow(VM& vm)
{
    ASSERT(inherits(info()));

    if (isCopyOnWrite(indexingMode())) {
        if (leastUpperBoundOfIndexingTypes(indexingType() & IndexingShapeMask, Int32Shape) == Int32Shape) {
#if USE(JSVALUE64)
            // SPEC-objectmodel §4.8/I35 (map-MC-LOCK.md S4): flag-on the
            // cell-locked materializer's WINNER may be a foreign thread, and
            // the shape may have advanced before our re-read; re-dispatch on
            // the fresh tagged word instead of asserting the stale classify
            // or dereferencing flat storage as our own. This re-dispatch is
            // sound because convertFromCopyOnWrite flag-on routes through the
            // materializer driver, whose seq_cst structureID observation of
            // the winner's DCAS/I36 publication synchronizes-with it before
            // we return here.
            if (Options::useJSThreads()) [[unlikely]] {
                convertFromCopyOnWrite(vm);
                return tryMakeWritableInt32(vm);
            }
#endif
            ASSERT(hasInt32(indexingMode()));
            convertFromCopyOnWrite(vm);
            return this->butterfly()->contiguousInt32();
        }
        return ContiguousJSValues();
    }

    if (structure()->hijacksIndexingHeader())
        return ContiguousJSValues();
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        if (indexingShouldBeSparse() || needsSlowPutIndexing()) [[unlikely]]
            return ContiguousJSValues();
        return createInitialInt32(vm, 0);
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        return convertUndecidedToInt32(vm);
        
    case ALL_DOUBLE_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES:
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return ContiguousJSValues();

    case ALL_INT32_INDEXING_TYPES:
#if USE(JSVALUE64)
        // §4.8/I35 loser re-dispatch (map-MC-LOCK.md S4): the inline classify
        // observed CopyOnWriteArrayWithInt32, but a rival completed the
        // cell-locked materialization before our second indexingMode fetch.
        // The now-flat Int32 butterfly is exactly what the caller asked for;
        // a trap is not a legal-program outcome here. Re-dispatch is only
        // sound AFTER an acquiring observation of the winner's DCAS/I36
        // publication: CoW-ness is not recoverable from the relaxed
        // tagged-word load alone (CoW words are plain flat SW=0 words), so we
        // route through the materializer driver, which no-ops via seq_cst
        // structureID/word loads when the object already left the CoW regime,
        // returning with the synchronizes-with edge to whichever materializer
        // won. Flag-off this state is unreachable (single mutator) and stays
        // a CRASH().
        if (Options::useJSThreads()) [[unlikely]] {
            materializeCopyOnWriteButterflyConcurrent(vm, static_cast<JSObjectWithButterfly*>(this));
            return tryMakeWritableInt32(vm);
        }
#endif
        CRASH();
        return ContiguousJSValues();

    default:
        CRASH();
        return ContiguousJSValues();
    }
}

ContiguousDoubles JSObject::tryMakeWritableDoubleSlow(VM& vm)
{
    ASSERT(Options::allowDoubleShape());
    ASSERT(inherits(info()));

    if (isCopyOnWrite(indexingMode())) {
        if (leastUpperBoundOfIndexingTypes(indexingType() & IndexingShapeMask, DoubleShape) == DoubleShape) {
            convertFromCopyOnWrite(vm);
#if USE(JSVALUE64)
            // §4.8/I35: see tryMakeWritableInt32Slow. Re-dispatch covers both
            // the foreign-winner word and the CoW-Int32 source (the inline
            // helper falls back here with a fresh non-CoW classify, landing
            // in the ALL_INT32 arm -> convertInt32ToDouble as today). The
            // flag-on convertFromCopyOnWrite above returned only after a
            // seq_cst observation of the winner's publication, so the inline
            // helper's relaxed word load is coherence-bound to the published
            // word.
            if (Options::useJSThreads()) [[unlikely]]
                return tryMakeWritableDouble(vm);
#endif
            if (hasDouble(indexingMode()))
                return this->butterfly()->contiguousDouble();
            ASSERT(hasInt32(indexingMode()));
        } else
            return ContiguousDoubles();
    }

    if (structure()->hijacksIndexingHeader())
        return ContiguousDoubles();
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        if (indexingShouldBeSparse() || needsSlowPutIndexing()) [[unlikely]]
            return ContiguousDoubles();
        return createInitialDouble(vm, 0);
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        return convertUndecidedToDouble(vm);
        
    case ALL_INT32_INDEXING_TYPES:
        return convertInt32ToDouble(vm);
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return ContiguousDoubles();

    case ALL_DOUBLE_INDEXING_TYPES:
#if USE(JSVALUE64)
        // §4.8/I35 loser re-dispatch (map-MC-LOCK.md S4): rival completed
        // CopyOnWriteArrayWithDouble -> ArrayWithDouble between the inline
        // classify and our re-read. Acquire the winner's publication via the
        // seq_cst materializer driver (no-op once non-CoW) before
        // re-dispatching — see the ALL_INT32 arm in tryMakeWritableInt32Slow.
        // Flag-off unreachable; stays a CRASH().
        if (Options::useJSThreads()) [[unlikely]] {
            materializeCopyOnWriteButterflyConcurrent(vm, static_cast<JSObjectWithButterfly*>(this));
            return tryMakeWritableDouble(vm);
        }
#endif
        CRASH();
        return ContiguousDoubles();

    default:
        CRASH();
        return ContiguousDoubles();
    }
}

ContiguousJSValues JSObject::tryMakeWritableContiguousSlow(VM& vm)
{
    ASSERT(inherits(info()));

    if (isCopyOnWrite(indexingMode())) {
        if (leastUpperBoundOfIndexingTypes(indexingType() & IndexingShapeMask, ContiguousShape) == ContiguousShape) {
            convertFromCopyOnWrite(vm);
#if USE(JSVALUE64)
            // §4.8/I35: see tryMakeWritableInt32Slow.
            if (Options::useJSThreads()) [[unlikely]]
                return tryMakeWritableContiguous(vm);
#endif
            if (hasContiguous(indexingMode()))
                return this->butterfly()->contiguous();
            ASSERT(hasInt32(indexingMode()) || hasDouble(indexingMode()));
        } else
            return ContiguousJSValues();
    }

    if (structure()->hijacksIndexingHeader())
        return ContiguousJSValues();
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        if (indexingShouldBeSparse() || needsSlowPutIndexing()) [[unlikely]]
            return ContiguousJSValues();
        return createInitialContiguous(vm, 0);
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        return convertUndecidedToContiguous(vm);
        
    case ALL_INT32_INDEXING_TYPES:
        return convertInt32ToContiguous(vm);
        
    case ALL_DOUBLE_INDEXING_TYPES:
        return convertDoubleToContiguous(vm);

    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return ContiguousJSValues();

    case ALL_CONTIGUOUS_INDEXING_TYPES:
#if USE(JSVALUE64)
        // §4.8/I35 loser re-dispatch (map-MC-LOCK.md S4): rival completed
        // CopyOnWriteArrayWithContiguous -> ArrayWithContiguous between the
        // inline classify and our re-read. Acquire the winner's publication
        // via the seq_cst materializer driver (no-op once non-CoW) before
        // re-dispatching — see the ALL_INT32 arm in tryMakeWritableInt32Slow.
        // Flag-off unreachable; stays a CRASH().
        if (Options::useJSThreads()) [[unlikely]] {
            materializeCopyOnWriteButterflyConcurrent(vm, static_cast<JSObjectWithButterfly*>(this));
            return tryMakeWritableContiguous(vm);
        }
#endif
        CRASH();
        return ContiguousJSValues();

    default:
        CRASH();
        return ContiguousJSValues();
    }
}

ArrayStorage* JSObject::ensureArrayStorageSlow(VM& vm)
{
    ASSERT(inherits(info()));

    if (structure()->hijacksIndexingHeader())
        return nullptr;

    ensureWritable(vm);

    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
        if (indexingShouldBeSparse()) [[unlikely]]
            return ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm);
        return createInitialArrayStorage(vm);
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        ASSERT(!indexingShouldBeSparse());
        ASSERT(!needsSlowPutIndexing());
        return convertUndecidedToArrayStorage(vm);
        
    case ALL_INT32_INDEXING_TYPES:
        ASSERT(!indexingShouldBeSparse());
        ASSERT(!needsSlowPutIndexing());
        return convertInt32ToArrayStorage(vm);
        
    case ALL_DOUBLE_INDEXING_TYPES:
        ASSERT(!indexingShouldBeSparse());
        ASSERT(!needsSlowPutIndexing());
        return convertDoubleToArrayStorage(vm);
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        ASSERT(!indexingShouldBeSparse());
        ASSERT(!needsSlowPutIndexing());
        return convertContiguousToArrayStorage(vm);

    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            // GIL-off: a racing AS install can land between the caller's
            // shape check and this dispatch, and createArrayStorageConcurrent's
            // loser leg (the hasIndexedProperties re-entry in
            // createArrayStorageConcurrent) re-enters here BY DESIGN when an
            // AS-flavor racer won. The storage already exists; AS never
            // segments (I31), so the flat accessor is the same read the
            // inline ensureArrayStorage fast path performs.
            return butterfly()->arrayStorage();
        }
#endif
        RELEASE_ASSERT_NOT_REACHED(); // Flag-off: the inline ensureArrayStorage fast path filters AS before the slow call.
        return nullptr;

    default:
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

ArrayStorage* JSObject::ensureArrayStorageExistsAndEnterDictionaryIndexingMode(VM& vm)
{
    ensureWritable(vm);

    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES: {
        createArrayStorage(vm, 0, 0);
        SparseArrayValueMap* map = allocateSparseIndexMap(vm);
        map->setSparseMode();
        return arrayStorage();
    }
        
    case ALL_UNDECIDED_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, convertUndecidedToArrayStorage(vm));
        
    case ALL_INT32_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, convertInt32ToArrayStorage(vm));
        
    case ALL_DOUBLE_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, convertDoubleToArrayStorage(vm));
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, convertContiguousToArrayStorage(vm));
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        return enterDictionaryIndexingModeWhenArrayStorageAlreadyExists(vm, this->butterfly()->arrayStorage());
        
    default:
        CRASH();
        return nullptr;
    }
}

void JSObject::switchToSlowPutArrayStorage(VM& vm)
{
    ensureWritable(vm);

    switch (indexingType()) {
    case ArrayClass:
        ensureArrayStorage(vm);
        RELEASE_ASSERT(hasAnyArrayStorage(indexingType()));
        if (hasSlowPutArrayStorage(indexingType()))
            return;
        switchToSlowPutArrayStorage(vm);
        break;

    case ALL_UNDECIDED_INDEXING_TYPES:
        convertUndecidedToArrayStorage(vm, TransitionKind::AllocateSlowPutArrayStorage);
        break;
        
    case ALL_INT32_INDEXING_TYPES:
        convertInt32ToArrayStorage(vm, TransitionKind::AllocateSlowPutArrayStorage);
        break;
        
    case ALL_DOUBLE_INDEXING_TYPES:
        convertDoubleToArrayStorage(vm, TransitionKind::AllocateSlowPutArrayStorage);
        break;
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        convertContiguousToArrayStorage(vm, TransitionKind::AllocateSlowPutArrayStorage);
        break;
        
    case NonArrayWithArrayStorage:
    case ArrayWithArrayStorage: {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        Structure* newStructure = Structure::nonPropertyTransition(vm, oldStructure, TransitionKind::SwitchToSlowPutArrayStorage, &deferred);
        setStructure(vm, newStructure);
        break;
    }

    case NonArrayWithSlowPutArrayStorage:
    case ArrayWithSlowPutArrayStorage:
        // Already SlowPut: idempotent no-op. GIL-off, every caller's
        // "if (!hasSlowPutArrayStorage(indexingType())) switchToSlowPut..."
        // guard is a TOCTOU — two racing converters (e.g. two Thread.restrict
        // calls on one shared object, or restrict racing a haveABadTime
        // SlowPut conversion) can both pass the guard, and the loser used to
        // land in the default: CRASH() below. The loser must no-op.
        break;

    default:
        CRASH();
        break;
    }
}

void JSObject::setPrototypeDirect(VM& vm, JSValue prototype)
{
    ASSERT(prototype.isObject() || prototype.isNull());
    if (prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
    else if (!prototype.isNull()) [[unlikely]] // Conservative hardening.
        return;
    
    if (structure()->hasMonoProto()) {
        DeferredStructureTransitionWatchpointFire deferred(vm, structure());
        Structure* newStructure = Structure::changePrototypeTransition(vm, structure(), prototype, deferred);
        setStructure(vm, newStructure);
        // Prototype-chain gets changed for the already cached structures. Invalidate the cache.
        if (mayBePrototype()) [[unlikely]]
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Prototype);
    } else
        putDirectOffset(vm, knownPolyProtoOffset, prototype);

    if (!anyObjectInChainMayInterceptIndexedAccesses())
        return;

    // Realm is always non-nullptr since realmless Structure's objects (e.g. WasmGC Struct) cannot call setPrototypeDirect.
    if (mayBePrototype()) {
        realm()->haveABadTime(vm);
        return;
    }

    if (!hasIndexedProperties(indexingType()))
        return;
    
    if (shouldUseSlowPut(indexingType()))
        return;
    
    switchToSlowPutArrayStorage(vm);
}

// GIL-off [[SetPrototypeOf]] linearization. OrdinarySetPrototypeOf's cycle
// walk (step 8) and its [[Prototype]] install (step 9) must be atomic with
// respect to every other [[SetPrototypeOf]] in the process: two mutators
// completing a cycle in opposite directions (a->b vs b->a) can otherwise BOTH
// pass the walk against the pre-race chains and BOTH install, creating a real
// prototype cycle — after which any unbounded chain walk (including this very
// cycle check, or a property-miss walk) spins forever while holding heap
// access and trips the STW watchdog. Every user-reachable proto mutation
// funnels through setPrototypeWithCycleCheck (Object/Reflect.setPrototypeOf,
// the __proto__ setter, Object.create-less __proto__ puts, the Proxy default
// trap on its target), so one process-wide lock around {walk, install}
// linearizes them: exactly one racer installs, the loser's walk then observes
// the winner's chain and throws the spec TypeError.
//
// SCOPE (host-API exclusion, recorded at the final closure review): the
// C API's JSObjectSetPrototype IS covered — it dispatches through the
// methodTable to this function (JSObjectRef.cpp). NOT covered:
// JSGlobalObject::resetPrototype and its fixupPrototypeChainWithObjectPrototype
// chain-tail walk + setPrototypeDirect (JSGlobalObject.cpp), reachable from
// JSGlobalContextCreateInGroup (JSContextRef.cpp) and the GLib wrapper map.
// Every in-tree caller runs during global-object construction, before the
// global is shared (see the finishCreation comment in JSGlobalObject.cpp),
// but the embedder-supplied prototype's chain TAIL that fixup mutates may
// already be published. EMBEDDER RESTRICTION (GIL-off): resetPrototype must
// not be called on a published global, and the prototype handed to context
// creation must not have its chain concurrently mutated; route any
// post-publication proto change through [[SetPrototypeOf]]
// (setPrototypeWithCycleCheck). Revisit if an embedder needs locked
// resetPrototype semantics.
//
// The lock is process-global (not per-object or hand-over-hand): a cycle is a
// property of an arbitrary multi-object chain, and per-object locking in walk
// order deadlocks on the very races this serializes (a->b vs b->a acquire in
// opposite orders). Contention is acceptable — proto mutation is already a
// slow path that fires watchpoints and invalidates structure caches.
//
// Acquisition is the FunctionRareData/GILOffCompilationLocker tryLock POLL
// LOOP, not a blocking lock(): the holder allocates (changePrototypeTransition)
// and can fire Class-A watchpoints / haveABadTime inside the critical section,
// i.e. it can request or park at a stop-the-world while holding the lock, so a
// loser blocked in lock() would neither publish nor acknowledge the stop and
// would trip the STW watchdog. The loser alternates
// parkSitePollAndParkForStopTheWorld() with yield instead.
//
// Everything that can run arbitrary JS (isExtensible — Proxy trap) or throw
// stays OUTSIDE the lock; the locked region re-checks the no-op fast path,
// walks, and installs, and the walk itself never calls out (it reads
// getPrototypeDirect() only and breaks at ProxyObjectType).
static Lock s_gilOffProtoCycleLock;

class GILOffProtoCycleLocker {
    WTF_MAKE_NONCOPYABLE(GILOffProtoCycleLocker);
public:
    GILOffProtoCycleLocker(VM& vm, bool shouldLock)
        : m_shouldLock(shouldLock)
    {
        if (!m_shouldLock) [[likely]]
            return;
        if (s_gilOffProtoCycleLock.tryLock()) [[likely]]
            return;
        while (!s_gilOffProtoCycleLock.tryLock()) {
            if (JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld(vm))
                continue; // Parked across a §A.3 window; retry.
            Thread::yield();
        }
    }

    ~GILOffProtoCycleLocker()
    {
        if (m_shouldLock) [[unlikely]]
            s_gilOffProtoCycleLock.unlock();
    }

private:
    bool m_shouldLock;
};

bool JSObject::setPrototypeWithCycleCheck(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (Options::useJSThreads() && structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, this)) [[unlikely]]
        return false;

    if (this->structure()->isImmutablePrototypeExoticObject()) {
        // This implements https://tc39.github.io/ecma262/#sec-set-immutable-prototype.
        if (this->getPrototype(globalObject) == prototype)
            return true;

        return typeError(globalObject, scope, shouldThrowIfCantSet, "Cannot set prototype of immutable prototype object"_s);
    }

    // Default realm global objects should have mutable prototypes despite having
    // a Proxy globalThis.
    ASSERT(this->isGlobalObject() || JSValue(this).toThis(globalObject, ECMAMode::sloppy()) == this);

    if (this->getPrototypeDirect() == prototype)
        return true;

    bool isExtensible = this->isExtensible(globalObject);
    RETURN_IF_EXCEPTION(scope, false);

    if (!isExtensible)
        return typeError(globalObject, scope, shouldThrowIfCantSet, ReadonlyPropertyWriteError);

    // Some clients would have already done this check because of the order of the check
    // specified in their respective specifications. However, we still do this check here
    // to document and enforce this invariant about the nature of prototype.
    if (!prototype.isObject() && !prototype.isNull()) [[unlikely]]
        return typeError(globalObject, scope, shouldThrowIfCantSet, PrototypeValueCanOnlyBeAnObjectOrNullTypeError);

    // GIL-off: the cycle walk and the install must be one linearized step
    // against every concurrent [[SetPrototypeOf]] (comment above the locker
    // class). GIL-on / single-thread: shouldLock is false and this is free.
    GILOffProtoCycleLocker protoCycleLocker(vm, vm.gilOffWithProcessGate());

    // Re-check the no-op fast path under the lock: a racer that already
    // installed exactly this prototype makes us a spec no-op (step 4 of
    // OrdinarySetPrototypeOf against the now-current chain), not a TypeError.
    if (this->getPrototypeDirect() == prototype)
        return true;

    JSValue nextPrototype = prototype;
    while (nextPrototype && nextPrototype.isObject()) {
        if (nextPrototype == this)
            return typeError(globalObject, scope, shouldThrowIfCantSet, "cyclic __proto__ value"_s);
        // FIXME: The specification currently says we should check if the [[GetPrototypeOf]] internal method of nextPrototype
        // is not the ordinary object internal method. However, we currently restrict this to Proxy objects as it would allow
        // for cycles with certain HTML objects (WindowProxy, Location) otherwise.
        // https://bugs.webkit.org/show_bug.cgi?id=161534
        if (asObject(nextPrototype)->type() == ProxyObjectType) [[unlikely]]
            break; // We're done. Set the prototype.
        nextPrototype = asObject(nextPrototype)->getPrototypeDirect();
    }
    setPrototypeDirect(vm, prototype);
    return true;
}

bool JSObject::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    return object->setPrototypeWithCycleCheck(globalObject->vm(), globalObject, prototype, shouldThrowIfCantSet);
}

JSValue JSObject::getPrototype(JSObject* object, JSGlobalObject*)
{
    return object->getPrototypeDirect();
}

bool JSObject::setPrototype(VM&, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrowIfCantSet)
{
    return methodTable()->setPrototype(this, globalObject, prototype, shouldThrowIfCantSet);
}

bool JSObject::putGetter(JSGlobalObject* globalObject, PropertyName propertyName, JSValue getter, unsigned attributes)
{
    PropertyDescriptor descriptor;
    descriptor.setGetter(getter);

    ASSERT(attributes & PropertyAttribute::Accessor);
    if (!(attributes & PropertyAttribute::ReadOnly))
        descriptor.setConfigurable(true);
    if (!(attributes & PropertyAttribute::DontEnum))
        descriptor.setEnumerable(true);

    return defineOwnProperty(this, globalObject, propertyName, descriptor, true);
}

bool JSObject::putSetter(JSGlobalObject* globalObject, PropertyName propertyName, JSValue setter, unsigned attributes)
{
    PropertyDescriptor descriptor;
    descriptor.setSetter(setter);

    ASSERT(attributes & PropertyAttribute::Accessor);
    if (!(attributes & PropertyAttribute::ReadOnly))
        descriptor.setConfigurable(true);
    if (!(attributes & PropertyAttribute::DontEnum))
        descriptor.setEnumerable(true);

    return defineOwnProperty(this, globalObject, propertyName, descriptor, true);
}

bool JSObject::putDirectAccessor(JSGlobalObject* globalObject, PropertyName propertyName, GetterSetter* accessor, unsigned attributes)
{
    ASSERT(attributes & PropertyAttribute::Accessor);

    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return putDirectIndex(globalObject, index.value(), accessor, attributes, PutDirectIndexLikePutDirect);

    return putDirectNonIndexAccessor(globalObject->vm(), propertyName, accessor, attributes);
}

// FIXME: Introduce a JSObject::putDirectCustomValue() method instead of using
// JSObject::putDirectCustomAccessor() to put CustomValues.
// https://bugs.webkit.org/show_bug.cgi?id=192576
bool JSObject::putDirectCustomAccessor(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes)
{
    ASSERT(!parseIndex(propertyName));
    ASSERT(value.isCustomGetterSetter());
    if (!(attributes & PropertyAttribute::CustomAccessor))
        attributes |= PropertyAttribute::CustomValue;

    PutPropertySlot slot(this);
    bool result = putDirectInternal<PutModeDefineOwnProperty>(vm, propertyName, value, attributes, slot).isNull();

    ASSERT(slot.type() == PutPropertySlot::NewProperty);

    Structure* structure = this->structure();
    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();
    structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
    return result;
}

void JSObject::putDirectCustomGetterSetterWithoutTransition(VM& vm, PropertyName propertyName, JSValue value, unsigned attributes)
{
    ASSERT(!parseIndex(propertyName));
    ASSERT(value.isCustomGetterSetter());
    ASSERT(attributes & PropertyAttribute::CustomAccessorOrValue);

#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        // Review round 1: cell-locked form, value stored with the table edit (I9/L3).
        putDirectWithoutTransitionConcurrent(vm, propertyName, value, attributes);
        Structure* structure = this->structure();
        if (attributes & PropertyAttribute::ReadOnly)
            structure->setContainsReadOnlyProperties();
        structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
        return;
    }
#endif
    StructureID structureID = this->structureID();
    Structure* structure = structureID.decode();
    PropertyOffset offset = prepareToPutDirectWithoutTransition(vm, propertyName, attributes, structureID, structure);
    putDirectOffset(vm, offset, value);

    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();
    structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
}

bool JSObject::putDirectNonIndexAccessor(VM& vm, PropertyName propertyName, GetterSetter* accessor, unsigned attributes)
{
    ASSERT(attributes & PropertyAttribute::Accessor);
    PutPropertySlot slot(this);
    bool result = putDirectInternal<PutModeDefineOwnProperty>(vm, propertyName, accessor, attributes, slot).isNull();

    Structure* structure = this->structure();
    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();

    structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
    return result;
}

void JSObject::putDirectNonIndexAccessorWithoutTransition(VM& vm, PropertyName propertyName, GetterSetter* accessor, unsigned attributes)
{
    ASSERT(attributes & PropertyAttribute::Accessor);
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        // Review round 1: cell-locked form, value stored with the table edit (I9/L3).
        putDirectWithoutTransitionConcurrent(vm, propertyName, accessor, attributes);
        Structure* structure = this->structure();
        if (attributes & PropertyAttribute::ReadOnly)
            structure->setContainsReadOnlyProperties();
        structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
        return;
    }
#endif
    StructureID structureID = this->structureID();
    Structure* structure = structureID.decode();
    PropertyOffset offset = prepareToPutDirectWithoutTransition(vm, propertyName, attributes, structureID, structure);
    putDirectOffset(vm, offset, accessor);
    if (attributes & PropertyAttribute::ReadOnly)
        structure->setContainsReadOnlyProperties();

    structure->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(propertyName == vm.propertyNames->underscoreProto);
}

// https://tc39.es/ecma262/#sec-hasproperty
bool JSObject::hasProperty(JSGlobalObject* globalObject, PropertyName propertyName) const
{
    PropertySlot slot(this, PropertySlot::InternalMethodType::HasProperty);
    return const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
}

bool JSObject::hasProperty(JSGlobalObject* globalObject, unsigned propertyName) const
{
    PropertySlot slot(this, PropertySlot::InternalMethodType::HasProperty);
    return const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
}

bool JSObject::hasProperty(JSGlobalObject* globalObject, uint64_t propertyName) const
{
    if (propertyName <= MAX_ARRAY_INDEX) [[likely]]
        return hasProperty(globalObject, static_cast<uint32_t>(propertyName));
    ASSERT(propertyName <= maxSafeInteger());
    return hasProperty(globalObject, Identifier::from(globalObject->vm(), propertyName));
}

bool JSObject::hasEnumerableProperty(JSGlobalObject* globalObject, PropertyName propertyName) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, false);
    if (!hasProperty)
        return false;
    return !(slot.attributes() & PropertyAttribute::DontEnum) || (slot.slotBase() && slot.slotBase()->structure()->typeInfo().getOwnPropertySlotMayBeWrongAboutDontEnum());
}

bool JSObject::hasEnumerableProperty(JSGlobalObject* globalObject, unsigned propertyName) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);
    bool hasProperty = const_cast<JSObject*>(this)->getPropertySlot(globalObject, propertyName, slot);
    RETURN_IF_EXCEPTION(scope, false);
    if (!hasProperty)
        return false;
    return !(slot.attributes() & PropertyAttribute::DontEnum) || (slot.slotBase() && slot.slotBase()->structure()->typeInfo().getOwnPropertySlotMayBeWrongAboutDontEnum());
}

#if USE(JSVALUE64)
// SPEC-objectmodel §6 (Task 9): D1's release-store of jsUndefined() into a
// doomed slot, BEFORE the table edit / structure publication (I30 - never
// clear(): a tardy lock-free reader that resolved the offset earlier reads
// the old value or undefined, never an encoded-0 null-cell deref). The slot
// stays GC-visited and storage never shrinks while it is quarantined
// (PropertyTable §6 split; markAuxiliaryAndVisitOutOfLineProperties above
// visits outOfLineSize, which propertyStorageSize keeps counting).
// locationForOffset() performs the §Q regime dispatch internally (segmented
// slots resolve through the loaded spine under the I33 bound).
static ALWAYS_INLINE void storeUndefinedIntoDoomedSlotConcurrent(JSObject* object, PropertyOffset offset)
{
    ASSERT(Options::useJSThreads());
    WriteBarrierBase<Unknown>* location = object->locationForOffset(offset);
    reinterpret_cast<Atomic<uint64_t>*>(location)->store(JSValue::encode(jsUndefined()), std::memory_order_release); // M2-style: value ordered before the edit that publishes its death.
}

// SPEC-objectmodel §6 (Task 9): flag-on named-property deletion - L4 (deletes
// serialize on the cell lock), L3/I19 (dictionary table edits additionally
// hold Structure::m_lock inside removePropertyWithoutTransition; lock order
// JSCellLock < m_lock, I20), D1/I30 (jsUndefined release-store before the
// edit), F2/I10b (a delete IS a transition: foreign/shared triggers fire both
// TTL sets under the §10.6 veneer before any lock - after which no E4
// lock-free transition can race the locked publication, since E4 requires
// valid+watched source sets), and the §4.2 RESTART discipline.
//
// I34 audit (this window): the PropertyOffset is produced by structure->get()
// (an m_lock-held walk flag-on, L6(iii)); the cell-lock acquisition below may
// PARK, so the structureID is re-validated under the lock before the offset
// is trusted - on mismatch the whole operation RESTARTs from a fresh
// structure. Between that re-validation and the slot store/table edit the
// path neither polls, nor allocates in the GC heap (O1: PropertyTable
// rehash/Vector edits are fastMalloc), nor parks.
static bool deletePropertyNamedConcurrent(VM& vm, JSObject* thisObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    ASSERT(Options::useJSThreads());
    while (true) {
        // FIX-2 class-(2) poll: same rationale as putDirectInternal's loop
        // top (this RESTART loop holds heap access with no bytecode poll).
        JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld(vm);
        Structure* structure = thisObject->structure(); // M5: nuke-masked decode.
        // FIX-4: snapshot the cacheable-dictionary pinned table's edit count
        // BEFORE resolving the offset, so the under-lock {table, edit count}
        // re-validation below proves the freshness of `offset` and of the
        // plan-time clone together. Writers bump the count AFTER the in-place
        // mutation completes (PropertyTable.h protocol comment, release store
        // under the cell lock), so any edit that overlaps the window between
        // this snapshot and the under-lock recheck — including one already in
        // flight when we snapshot — is observed at the recheck and forces a
        // RESTART before anything stale is published.
        PropertyTable* plannedDictionaryTable = nullptr;
        uint32_t plannedDictionaryEditCount = 0;
        if (structure->isDictionary() && !structure->isUncacheableDictionary()) {
            plannedDictionaryTable = structure->pinnedPropertyTableForConcurrentDelete();
            RELEASE_ASSERT(plannedDictionaryTable); // Dictionary tables are pinned.
            plannedDictionaryEditCount = plannedDictionaryTable->concurrentEditCount();
        }
        unsigned attributes = 0;
        PropertyOffset offset = structure->get(vm, propertyName, attributes);
        if (!isValidOffset(offset)) {
            slot.setConfigurableMiss();
            return true;
        }
        if (attributes & PropertyAttribute::DontDelete && vm.deletePropertyMode() != VM::DeletePropertyMode::IgnoreConfigurable) {
            slot.setNonconfigurable();
            return false;
        }

        // ---- F2 step 0 (I10b/I13), §5 trigger taxonomy (per-object keying):
        // butterfly-bearing instance - foreign tag TID or SW=1 (segmented
        // always triggers); butterfly-less instance - thread != the
        // structure's N1 transition TID.
        {
            uint64_t word = thisObject->taggedButterflyWord();
            bool trigger;
            if (!(word & butterflyPointerMask))
                trigger = currentButterflyTID() != structure->transitionThreadLocalTID();
            else if (isSegmentedButterfly(word))
                trigger = true;
            else
                trigger = butterflyTID(word) != currentButterflyTID() || butterflySharedWrite(word);
            if (trigger && (structure->transitionThreadLocalIsStillValid() || structure->writeThreadLocalIsStillValid())) {
                jsThreadsStopTheWorldAndRun(vm, scopedLambda<void()>([&] {
                    // Re-check inside the stop: a racing fire may have won.
                    if (structure->transitionThreadLocalIsStillValid() || structure->writeThreadLocalIsStillValid())
                        structure->fireTransitionThreadLocal(vm, "F2: foreign/shared delete (§6 L4)");
                }));
                continue; // RESTART after the stop (§4.2 rule).
            }
        }

        if (structure->isUncacheableDictionary()) {
            // §6 L3/L4 + I19: dictionary deletes serialize on the cell lock.
            Locker cellLocker { thisObject->cellLock() };
            // I34: the acquisition above may park - re-validate before
            // trusting anything pre-lock. Dictionary structures are pinned to
            // the object, but flatten (F3, per-event STW when shared) or a
            // racing locked transition can have replaced it while we parked.
            if (thisObject->structureID() != structure->id())
                continue; // RESTART (Locker unlocks on scope exit).
            // Review round 4 (blocker fix): dictionary tables are edited IN
            // PLACE and the structureID does NOT change, so the re-validation
            // above proves nothing about the PRE-LOCK offset. Across the park
            // a racing delete may have removed the key (the loser's
            // removePropertyWithoutTransition would then return a different/
            // invalid offset - a process abort on a program-level race), and -
            // worse - a GC safepoint may have promoted the quarantined slot
            // and a racing dictionary ADD reused it for a brand-new property,
            // which the stale offset's D1 store would clobber with undefined
            // (the exact I18 deleted-slot-reuse corruption quarantining
            // exists to prevent). Resolution: RE-RESOLVE offset + attributes
            // UNDER the cell lock. Note the pre-lock structure->get() was
            // never a torn read - flag-on Structure::get routes to
            // getConcurrently, which reads the table under the structure's
            // m_lock (L6(iii), §20) - the bug was purely offset STALENESS.
            // Lock order: JSCellLock (held) < Structure::m_lock (taken inside
            // get/removePropertyWithoutTransition) - I20; getConcurrently
            // never allocates, so O1 holds under the cell lock. All dictionary
            // table mutations are cell-locked flag-on (adds: §56.5; deletes:
            // here), so the locked lookup is stable through the edit below.
            unsigned lockedAttributes = 0;
            PropertyOffset lockedOffset = structure->get(vm, propertyName, lockedAttributes);
            if (!isValidOffset(lockedOffset)) {
                // A racing delete won while we parked: this delete is a miss.
                slot.setConfigurableMiss();
                return true;
            }
            if (lockedAttributes & PropertyAttribute::DontDelete && vm.deletePropertyMode() != VM::DeletePropertyMode::IgnoreConfigurable) {
                slot.setNonconfigurable();
                return false;
            }
            storeUndefinedIntoDoomedSlotConcurrent(thisObject, lockedOffset); // D1, BEFORE the table edit; offset resolved UNDER the lock.
            PropertyOffset removedOffset = structure->removePropertyWithoutTransition(vm, propertyName, [](const GCSafeConcurrentJSLocker&, PropertyOffset, PropertyOffset) { });
            // The key was re-resolved under the cell lock and every dictionary
            // table mutation holds it, so the entry cannot have moved between
            // the locked lookup and the locked edit; out-of-line slots taken
            // here are quarantined by the table edit (addDeletedOffset,
            // I18/I30).
            RELEASE_ASSERT(removedOffset == lockedOffset);
            return true;
        }

        // ---- Non-dictionary: a delete is a structure-only transition (the
        // butterfly word is untouched - quarantine keeps maxOffset/storage
        // size, I18/I30, so no nuke is needed per GT#7). Plan + allocate the
        // transition target BEFORE locking (O1: removePropertyTransition
        // allocates in the GC heap).
        // S6 L3/L4 cacheable-dictionary staleness guard (root cause of the
        // transition-vs-write wholesale lost adds): a CACHEABLE dictionary's
        // adds mutate its pinned table IN PLACE without changing the
        // structureID, while removePropertyTransition below CLONES that table
        // at plan time. The structureID re-validation under the cell lock
        // therefore proves NOTHING about the clone's freshness - publishing a
        // stale clone silently orphans every add that landed between the
        // clone and the publication (I21/I37 violation; also the
        // maxOffset-vs-table offset-inconsistency aborts and the I30
        // reused-slot-holds-a-value asserts). Snapshot the pinned table's
        // monotonic edit count here and re-validate it under the cell lock:
        // every in-place mutation of a dictionary table holds the cell lock
        // (adds: putDirectInternal's S6 leg; deletes: this function), so an
        // unchanged {table pointer, edit count} pair under the lock proves
        // the clone is exact. Non-dictionary sources keep the plain ID
        // validation: their tables are never edited in place once published (L6).
        DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, structure);
        PropertyOffset transitionOffset = invalidOffset;
        Structure* newStructure = Structure::removePropertyTransition(vm, structure, propertyName, transitionOffset, &deferredWatchpointFire);
        // FIX-4: the transitionOffset and outOfLineCapacity checks MOVED
        // under the cell lock below. Pre-lock they race a cacheable
        // dictionary's in-place adds: removePropertyTransition clones the
        // pinned table at one instant while structure->outOfLineCapacity()
        // is re-read at another, and a racing cell-locked add can grow the
        // table's maxOffset across an out-of-line capacity boundary in
        // between (the V4 races/transition-vs-write JSObject.cpp:4148
        // abort). The invariants are only checkable once the {table, edit
        // count} snapshot has been re-validated under the cell lock, which
        // freezes the source table. Type/flags/indexing are immutable
        // per-Structure fields and remain safe to assert here.
        ASSERT(newStructure->typeInfo().type() == structure->typeInfo().type());
        ASSERT(newStructure->indexingModeIncludingHistory() == structure->indexingModeIncludingHistory());
        {
            Locker cellLocker { thisObject->cellLock() };
            if (thisObject->structureID() != structure->id())
                continue; // RESTART: a racing transition won while we planned/parked (I34 re-validation).
            if (plannedDictionaryTable
                && (!structure->isDictionary()
                    || structure->pinnedPropertyTableForConcurrentDelete() != plannedDictionaryTable
                    || plannedDictionaryTable->concurrentEditCount() != plannedDictionaryEditCount))
                continue; // RESTART: an in-place dictionary edit or a flatten landed after the snapshot (S6 L3/L4 guard; Locker unlocks on scope exit).
            // FIX-4 (amended per review): the !isDictionary() re-read above
            // closes the flatten hole — flattenDictionaryStructure renumbers
            // offsets IN PLACE (PropertyTable::renumberPropertyOffsets does
            // NOT bump concurrentEditCount), restores the SAME structureID,
            // and keeps the same pinned table, but leaves the structure
            // non-dictionary; a flatten landing while we were parked
            // mid-plan therefore forces a RESTART here instead of
            // publishing pre-flatten offsets into compacted slots.
            // With structureID, dictionary-kind, table pointer, and edit
            // count all re-validated under the cell lock — the edit count
            // snapshotted BEFORE the pre-lock get, and bumped by writers
            // AFTER their mutation completes (PropertyTable.h protocol) —
            // the source table is frozen across get, clone, and
            // publication: every in-place dictionary mutation holds this
            // cell lock and bumps the count post-mutation, so a passing
            // recheck proves no edit overlapped the window (an edit
            // mid-flight at snapshot time bumps after we snapshotted; an
            // edit mid-flight at recheck time is impossible — we hold the
            // lock it mutates under). The clone is exact and `offset` is fresh, so the
            // capacity and offset equalities are theorems, not racing
            // observations. A mismatch here is a logic error
            // (SPEC-objectmodel §4.2/§4.3), not a race: abort rather than
            // silently retry against provably frozen state, which could
            // never converge.
            RELEASE_ASSERT(newStructure->outOfLineCapacity() == structure->outOfLineCapacity());
            RELEASE_ASSERT(transitionOffset == offset); // Same frozen source table, same key.
            storeUndefinedIntoDoomedSlotConcurrent(thisObject, offset); // D1, BEFORE the publication.
            // ONE 64-bit header CAS under the §3.0 volatile-byte merge
            // discipline (mirrors tryStructureOnlyTransition's N2 publication;
            // 8B-aligned, so legal on PreciseAllocation cells too - I36). Only
            // the structureID lane changes: remove transitions preserve
            // type/flags/indexing (asserted above).
            Atomic<uint64_t>* headerAtomic = reinterpret_cast<Atomic<uint64_t>*>(static_cast<JSCell*>(thisObject));
            uint64_t expectedHeader = headerAtomic->load(std::memory_order_seq_cst);
            RELEASE_ASSERT(static_cast<uint32_t>(expectedHeader) == structure->id().bits());
            uint64_t desiredHeader = (expectedHeader & ~0xffffffffULL) | static_cast<uint64_t>(newStructure->id().bits());
            while (true) {
                uint64_t previousHeader = headerAtomic->compareExchangeStrong(expectedHeader, desiredHeader, std::memory_order_seq_cst);
                if (previousHeader == expectedHeader)
                    break;
                // Under the cell lock, with the TTL sets handled by step 0,
                // only the volatile bytes (GC cellState CAS, lock parked bit -
                // GT#2) may move; anything else is a logic error (§3.0 step 4).
                RELEASE_ASSERT(headerDiffersOnlyInVolatileBits(expectedHeader, previousHeader));
                expectedHeader = mergeVolatileHeaderBits(expectedHeader, previousHeader);
                desiredHeader = mergeVolatileHeaderBits(desiredHeader, previousHeader);
            }
        }
        vm.writeBarrier(thisObject);
        vm.writeBarrier(thisObject, newStructure);
        slot.setHit(offset);
        ASSERT(newStructure->outOfLineCapacity() || !thisObject->structure()->outOfLineCapacity());
        if (thisObject->mayBePrototype()) [[unlikely]]
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Remove);
        return true;
    }
}
#endif // USE(JSVALUE64)

// ECMA 8.6.2.5
bool JSObject::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, DeletePropertySlot& slot)
{
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    VM& vm = globalObject->vm();
    
    if (Options::useJSThreads() && thisObject->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, thisObject)) [[unlikely]]
        return false;

    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return thisObject->methodTable()->deletePropertyByIndex(thisObject, globalObject, index.value());

    unsigned attributes;

    if (thisObject->hasNonReifiedStaticProperties()) {
        if (auto entry = thisObject->findPropertyHashEntry(propertyName)) {
            // If the static table contains a non-configurable (DontDelete) property then we can return early;
            // if there is a property in the storage array it too must be non-configurable (the language does
            // not allow repacement of a non-configurable property with a configurable one).
            if (entry->value->attributes() & PropertyAttribute::DontDelete && vm.deletePropertyMode() != VM::DeletePropertyMode::IgnoreConfigurable) {
                ASSERT(!isValidOffset(thisObject->structure()->get(vm, propertyName, attributes)) || attributes & PropertyAttribute::DontDelete);
                return false;
            }
            thisObject->reifyAllStaticProperties(globalObject);
        }
    }

#if USE(JSVALUE64)
    // SPEC-objectmodel §6 (Task 9): flag-on, named deletes take the L4
    // cell-locked / D1 / F2 path above. Flag-off below is byte-for-byte
    // today's code (I22).
    if (Options::useJSThreads()) [[unlikely]]
        return deletePropertyNamedConcurrent(vm, thisObject, propertyName, slot);
#endif

    Structure* structure = thisObject->structure();

    bool propertyIsPresent = isValidOffset(structure->get(vm, propertyName, attributes));
    if (propertyIsPresent) {
        if (attributes & PropertyAttribute::DontDelete && vm.deletePropertyMode() != VM::DeletePropertyMode::IgnoreConfigurable) {
            slot.setNonconfigurable();
            return false;
        }

        PropertyOffset offset = invalidOffset;
        if (structure->isUncacheableDictionary()) {
            offset = structure->removePropertyWithoutTransition(vm, propertyName, [] (const GCSafeConcurrentJSLocker&, PropertyOffset, PropertyOffset) { });
            ASSERT(!isValidOffset(structure->get(vm, propertyName, attributes)));
            if (offset != invalidOffset)
                thisObject->locationForOffset(offset)->clear(); // Flag-off only: flag-on deletes D1-store jsUndefined() instead (I30).
        } else {
            DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, structure);
            structure = Structure::removePropertyTransition(vm, structure, propertyName, offset, &deferredWatchpointFire);
            slot.setHit(offset);
            ASSERT(structure->outOfLineCapacity() || !thisObject->structure()->outOfLineCapacity());
            thisObject->setStructure(vm, structure);
            ASSERT(!isValidOffset(structure->get(vm, propertyName, attributes)));
            if (offset != invalidOffset)
                thisObject->locationForOffset(offset)->clear();
            if (thisObject->mayBePrototype()) [[unlikely]]
                vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Remove);
        }
    } else
        slot.setConfigurableMiss();

    return true;
}

bool JSObject::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned i)
{
    VM& vm = globalObject->vm();
    JSObject* thisObject = uncheckedDowncast<JSObject>(cell);
    
    if (Options::useJSThreads() && thisObject->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, thisObject)) [[unlikely]]
        return false;

    if (i > MAX_ARRAY_INDEX)
        return JSCell::deleteProperty(thisObject, globalObject, Identifier::from(vm, i));

#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        // SPEC-objectmodel §6 L4 (Task 9): indexed deletes.
        if (hasAnyArrayStorage(thisObject->indexingType())) {
            // I31/L5: EVERY runtime access to an AS-shape object is
            // cell-locked (reads included, any SW). Element clears and
            // m_numValuesInVector are in-place scalar edits, legal in an
            // installed AS under the lock (AS-COPY never relays out in
            // place; superseded snapshots are frozen). EMPTY is the
            // legitimate AS hole marker and AS readers are locked too, so no
            // D1 jsUndefined analogue is needed here. Sparse-map structural
            // edits are runtime-only and locked on both sides (§4.6); the
            // remove below frees only fastMalloc memory (O1).
            Locker locker { thisObject->cellLock() };
            ArrayStorage* storage = thisObject->butterfly()->arrayStorage(); // Re-loaded under the lock (AS-COPY republication).
            if (i < storage->vectorLength()) {
                WriteBarrier<Unknown>& valueSlot = storage->m_vector[i];
                if (valueSlot) {
                    valueSlot.clear();
                    --storage->m_numValuesInVector;
                }
            } else if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
                // AB18-G: the object lock above does not exclude a
                // putEntry-internal rehash on the MAP's cellLock, so the probe
                // must be a locked snapshot, and remove must re-probe by key
                // under the map lock rather than consume an iterator minted by
                // an unlocked-vs-map find(). Snapshot-then-keyed-remove keeps
                // every map access locked (no memory unsafety); a racing
                // re-add between the two windows is a benign ordering race
                // (equivalent to the delete losing the race outright).
                if (std::optional<SparseArrayEntry> entry = map->getEntry(i)) {
                    if (entry->attributes() & PropertyAttribute::DontDelete)
                        return false;
                    map->remove(i);
                }
            }
            return true;
        }
        uint64_t word = thisObject->taggedButterflyWord();
        if (isSegmentedButterfly(word)) {
            // Segmented Int32/Contiguous/Double: store the shape's hole
            // encoding through the LOADED spine under the I33/C4 bound.
            // Out-of-bounds means a stale spine or beyond storage - reads of
            // [vectorLength, publicLength) are holes already (C4), so there
            // is nothing to delete (SAB-granularity staleness). The shape is
            // stable against the loaded spine: relabels touching Double on
            // shared objects run under per-event STW (I28/§4.7), which
            // cannot overlap this running mutator.
            ButterflySpine* spine = butterflySpine(word);
            if (WriteBarrierBase<Unknown>* valueSlot = segmentedIndexedSlotIfWithinVectorLength(spine, i)) {
                if (hasDouble(thisObject->indexingType()))
                    *std::bit_cast<double*>(valueSlot) = PNaN; // Raw 8B lane (§4.7); PNaN = hole, as today.
                else
                    valueSlot->clear(); // EMPTY = hole marker for Int32/Contiguous, as today.
            }
            return true;
        }
        // §4.8/I35 (review round 1; round 3 NOTE: convertFromCopyOnWrite is
        // now itself concurrent-correct flag-on - it routes through the
        // cell-locked materializer - so this early routing is required only
        // for the F1 semantics of the FOREIGN trigger, not for publication
        // safety). A FOREIGN indexed delete on a CopyOnWrite word routes
        // through ensureSharedWriteBit's CoW carve-out
        // (tryMaterializeCopyOnWriteButterflyForSharedWrite: F2-fire-first +
        // cell lock + nuke + DCAS), exactly like putIndexConcurrent. After
        // materialization the switch below re-reads indexingMode() and lands
        // on the writable branches against the freshly published private
        // butterfly. Bounds are checked first (CoW words are flat-decodable
        // by I35) so out-of-range deletes stay allocation-free, as today.
        if (isCopyOnWrite(thisObject->indexingMode())) {
            if (i >= untaggedButterfly(word)->vectorLength())
                return true; // Nothing to delete (matches the CoW legs below).
            if (butterflyWriterIsForeign(word)) // incl. §9.6 forceButterflySWBit
                ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(thisObject));
            // Owner CoW words fall through to today's convertFromCopyOnWrite
            // legs; materialized words fall through to the writable legs.
        } else if ((word & butterflyPointerMask) && !butterflySharedWrite(word)
            && butterflyWriterIsForeign(word)) { // incl. §9.6 forceButterflySWBit
            // §3 F1 (review round 1): a hole store is a WRITE - the same
            // foreign-first-write rule as trySetIndexQuicklyConcurrent.
            ensureSharedWriteBit(vm, static_cast<JSObjectWithButterfly*>(thisObject));
        }
        // Flat words (any tag) fall through to today's switch: the
        // contiguous-family branches mask via butterfly() and store the
        // shape's hole encoding in place - today's race semantics at SAB
        // granularity (writes only after the SW bit is accounted for, above).
    }
#endif

    switch (thisObject->indexingMode()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        return true;

    case CopyOnWriteArrayWithInt32:
    case CopyOnWriteArrayWithContiguous: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return true;
        thisObject->convertFromCopyOnWrite(vm);
        [[fallthrough]];
    }

    case ALL_WRITABLE_INT32_INDEXING_TYPES:
    case ALL_WRITABLE_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return true;
        butterfly->contiguous().at(thisObject, i).clear();
        return true;
    }

    case CopyOnWriteArrayWithDouble: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return true;
        thisObject->convertFromCopyOnWrite(vm);
        [[fallthrough]];
    }

    case ALL_WRITABLE_DOUBLE_INDEXING_TYPES: {
        Butterfly* butterfly = thisObject->butterfly();
        if (i >= butterfly->vectorLength())
            return true;
        butterfly->contiguousDouble().at(thisObject, i) = PNaN;
        return true;
    }
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = thisObject->butterfly()->arrayStorage();
        
        if (i < storage->vectorLength()) {
            WriteBarrier<Unknown>& valueSlot = storage->m_vector[i];
            if (valueSlot) {
                valueSlot.clear();
                --storage->m_numValuesInVector;
            }
        } else if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
            SparseArrayValueMap::iterator it = map->find(i);
            if (it != map->notFound()) {
                if (it->value.attributes() & PropertyAttribute::DontDelete)
                    return false;
                map->remove(it);
            }
        }
        
        return true;
    }
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
}

template<CachedSpecialPropertyKey key>
static ALWAYS_INLINE JSValue callToPrimitiveFunction(JSGlobalObject* globalObject, const JSObject* object, PropertyName propertyName, PreferredPrimitiveType hint)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue function = object->structure()->cachedSpecialProperty(key);
    if (!function) {
        PropertySlot slot(object, PropertySlot::InternalMethodType::Get);
        // FIXME: Remove this when we have fixed: rdar://problem/33451840
        // https://bugs.webkit.org/show_bug.cgi?id=187109.
        constexpr bool debugNullStructure = key == CachedSpecialPropertyKey::ToPrimitive;
        bool hasProperty = const_cast<JSObject*>(object)->getPropertySlot<debugNullStructure>(globalObject, propertyName, slot);
        RETURN_IF_EXCEPTION(scope, scope.exception());
        function = hasProperty ? slot.getValue(globalObject, propertyName) : jsUndefined();
        RETURN_IF_EXCEPTION(scope, scope.exception());
        object->structure()->cacheSpecialProperty(globalObject, vm, function, key, slot);
        RETURN_IF_EXCEPTION(scope, scope.exception());
    }
    if (function.isUndefinedOrNull())
        return JSValue();

    // Add optimizations for frequently called functions.
    // https://bugs.webkit.org/show_bug.cgi?id=216084
    if constexpr (key == CachedSpecialPropertyKey::ToString) {
        if (function == globalObject->objectProtoToStringFunction()) {
            if (auto result = object->structure()->cachedSpecialProperty(CachedSpecialPropertyKey::ToStringTag))
                return result;
        }
    }

    if constexpr (key == CachedSpecialPropertyKey::ValueOf) {
        if (function == globalObject->objectProtoValueOfFunction())
            return JSValue();
    }

    auto callData = JSC::getCallDataInline(function);
    if (callData.type == CallData::Type::None) {
        if constexpr (key == CachedSpecialPropertyKey::ToPrimitive)
            throwTypeError(globalObject, scope, "Symbol.toPrimitive is not a function, undefined, or null"_s);
        return scope.exception();
    }

    MarkedArgumentBuffer callArgs;
    if constexpr (key == CachedSpecialPropertyKey::ToPrimitive) {
        JSString* hintString = nullptr;
        switch (hint) {
        case NoPreference:
            hintString = vm.smallStrings.defaultString();
            break;
        case PreferNumber:
            hintString = vm.smallStrings.numberString();
            break;
        case PreferString:
            hintString = vm.smallStrings.stringString();
            break;
        }
        callArgs.append(hintString);
    } else {
        UNUSED_PARAM(hint);
    }
    ASSERT(!callArgs.hasOverflowed());

    JSValue result = call(globalObject, function, callData, const_cast<JSObject*>(object), callArgs);
    RETURN_IF_EXCEPTION(scope, scope.exception());
    ASSERT(!result.isGetterSetter());
    if (result.isObject()) {
        if constexpr (key == CachedSpecialPropertyKey::ToPrimitive)
            return throwTypeError(globalObject, scope, "Symbol.toPrimitive returned an object"_s);
        return JSValue();
    }
    return result;
}

// ECMA 7.1.1
JSValue JSObject::ordinaryToPrimitive(JSGlobalObject* globalObject, PreferredPrimitiveType hint) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Make sure that whatever default value methods there are on object's prototype chain are
    // being watched.
    // FIXME: Remove this hack for DFG.
    // https://bugs.webkit.org/show_bug.cgi?id=216117
    for (const JSObject* object = this; object; object = object->structure()->storedPrototypeObject(object))
        object->structure()->startWatchingInternalPropertiesIfNecessary(vm);

    JSValue value;
    if (hint == PreferString) {
        value = callToPrimitiveFunction<CachedSpecialPropertyKey::ToString>(globalObject, this, vm.propertyNames->toString, hint);
        EXCEPTION_ASSERT(!scope.exception() || scope.exception() == value.asCell());
        if (value)
            return value;
        value = callToPrimitiveFunction<CachedSpecialPropertyKey::ValueOf>(globalObject, this, vm.propertyNames->valueOf, hint);
        EXCEPTION_ASSERT(!scope.exception() || scope.exception() == value.asCell());
        if (value)
            return value;
    } else {
        value = callToPrimitiveFunction<CachedSpecialPropertyKey::ValueOf>(globalObject, this, vm.propertyNames->valueOf, hint);
        EXCEPTION_ASSERT(!scope.exception() || scope.exception() == value.asCell());
        if (value)
            return value;
        value = callToPrimitiveFunction<CachedSpecialPropertyKey::ToString>(globalObject, this, vm.propertyNames->toString, hint);
        EXCEPTION_ASSERT(!scope.exception() || scope.exception() == value.asCell());
        if (value)
            return value;
    }

    return throwTypeError(globalObject, scope, "No default value"_s);
}

JSValue JSObject::toPrimitive(JSGlobalObject* globalObject, PreferredPrimitiveType preferredType) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isJSArray(this)) {
        auto* array = uncheckedDowncast<JSArray>(const_cast<JSObject*>(this));
        if (array->isToPrimitiveFastAndNonObservable()) [[likely]]
            RELEASE_AND_RETURN(scope, array->fastToString(globalObject));
    }

    JSValue value = callToPrimitiveFunction<CachedSpecialPropertyKey::ToPrimitive>(globalObject, this, vm.propertyNames->toPrimitiveSymbol, preferredType);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return value;

    RELEASE_AND_RETURN(scope, ordinaryToPrimitive(globalObject, preferredType));
}

bool JSObject::getOwnStaticPropertySlot(VM& vm, PropertyName propertyName, PropertySlot& slot)
{
    for (auto* info = classInfo(); info; info = info->parentClass) {
        if (auto* table = info->staticPropHashTable) {
            if (getStaticPropertySlotFromTable(vm, table->classForThis, *table, this, propertyName, slot))
                return true;
        }
    }
    return false;
}

std::optional<Structure::PropertyHashEntry> JSObject::findPropertyHashEntry(PropertyName propertyName) const
{
    return structure()->findPropertyHashEntry(propertyName);
}

bool JSObject::hasInstance(JSGlobalObject* globalObject, JSValue value, JSValue hasInstanceValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!hasInstanceValue.isUndefinedOrNull() && hasInstanceValue != globalObject->functionProtoHasInstanceSymbolFunction()) {
        auto callData = JSC::getCallDataInline(hasInstanceValue);
        if (callData.type == CallData::Type::None) {
            throwException(globalObject, scope, createInvalidInstanceofParameterErrorHasInstanceValueNotFunction(globalObject, this));
            return false;
        }

        MarkedArgumentBuffer args;
        args.append(value);
        ASSERT(!args.hasOverflowed());
        JSValue result = call(globalObject, hasInstanceValue, callData, this, args);
        RETURN_IF_EXCEPTION(scope, false);
        return result.toBoolean(globalObject);
    }

    TypeInfo info = structure()->typeInfo();
    if (info.implementsDefaultHasInstance()) {
        JSValue prototype = get(globalObject, vm.propertyNames->prototype);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, defaultHasInstance(globalObject, value, prototype));
    }
    if (info.implementsHasInstance()) {
        if (!vm.isSafeToRecurseSoft()) [[unlikely]] {
            throwStackOverflowError(globalObject, scope);
            return false;
        }
        RELEASE_AND_RETURN(scope, methodTable()->customHasInstance(this, globalObject, value));
    }

    throwException(globalObject, scope, createInvalidInstanceofParameterErrorNotFunction(globalObject, this));
    return false;
}

bool JSObject::hasInstance(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue hasInstanceValue = get(globalObject, vm.propertyNames->hasInstanceSymbol);
    RETURN_IF_EXCEPTION(scope, false);

    RELEASE_AND_RETURN(scope, hasInstance(globalObject, value, hasInstanceValue));
}

bool JSObject::defaultHasInstance(JSGlobalObject* globalObject, JSValue value, JSValue proto)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (!value.isObject())
        return false;

    if (!proto.isObject()) {
        throwTypeError(globalObject, scope, "instanceof called on an object with an invalid prototype property."_s);
        return false;
    }

    JSObject* object = asObject(value);
    while (true) {
        JSValue objectValue = object->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!objectValue.isObject())
            return false;
        object = asObject(objectValue);
        if (proto == object)
            return true;
    }
    ASSERT_NOT_REACHED();
}

JSC_DEFINE_HOST_FUNCTION(objectPrivateFuncInstanceOf, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSValue value = callFrame->uncheckedArgument(0);
    JSValue proto = callFrame->uncheckedArgument(1);

    return JSValue::encode(jsBoolean(JSObject::defaultHasInstance(globalObject, value, proto)));
}

void JSObject::getPropertyNames(JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* object = this;
    unsigned prototypeCount = 0;

    while (true) {
        object->methodTable()->getOwnPropertyNames(object, globalObject, propertyNames, mode);
        RETURN_IF_EXCEPTION(scope, void());

        JSValue prototype = object->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (prototype.isNull())
            break;

        if (++prototypeCount > maximumPrototypeChainDepth) [[unlikely]] {
            throwStackOverflowError(globalObject, scope);
            return;
        }

        object = asObject(prototype);
    }
}

void JSObject::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    if (Options::useJSThreads() && object->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
        return;
    object->getOwnIndexedPropertyNames(globalObject, propertyNames, mode);
    object->getOwnNonIndexPropertyNames(globalObject, propertyNames, mode);
}

void JSObject::getOwnSpecialPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode)
{
    // Structure::validateFlags() breaks if this method isn't exported, which is impossible if it's inlined.
}

void JSObject::getOwnIndexedPropertyNames(JSGlobalObject*, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    JSObject* object = this;

    if (propertyNames.includeStringProperties()) {
        // Add numeric properties first per step 2 of https://tc39.es/ecma262/#sec-ordinaryownpropertykeys
        // FIXME: Filling PropertyNameArray with an identifier for every integer
        // is incredibly inefficient for large arrays. We need a different approach,
        // which almost certainly means a different structure for PropertyNameArray.
#if USE(JSVALUE64)
        // SPEC-objectmodel review round 1 (I31/L5 + §10.7): flag-on, this walk
        // must (a) never deref a segmented word through the flat-only
        // butterfly() accessor and (b) hold the cell lock across the AS header
        // walk + sparse-map iteration - a lock on only the writer side of the
        // §4.6/Task-9 mutation windows protects nothing, and an unlocked map
        // iteration races a putEntry-driven rehash (use-after-rehash). Indices
        // are collected under the lock and added after it drops (adds may
        // allocate; O1 keeps allocation out of the cell-locked window).
        if (Options::useJSThreads()) [[unlikely]] {
            switch (object->indexingType()) {
            case ALL_BLANK_INDEXING_TYPES:
            case ALL_UNDECIDED_INDEXING_TYPES:
                break;
            case ALL_INT32_INDEXING_TYPES:
            case ALL_CONTIGUOUS_INDEXING_TYPES:
            case ALL_DOUBLE_INDEXING_TYPES: {
                bool isDouble = hasDouble(object->indexingType());
                uint64_t word = object->taggedButterflyWord();
                if (isSegmentedButterfly(word)) {
                    ButterflySpine* spine = butterflySpine(word);
                    uint32_t usedLength = std::min(segmentedPublicLength(spine), segmentedVectorLength(spine)); // C4/I33 bound from the SAME spine.
                    for (uint32_t i = 0; i < usedLength; ++i) {
                        const WriteBarrierBase<Unknown>* slot = spine->indexedSlot(i);
                        if (isDouble) {
                            double value = *std::bit_cast<const double*>(slot); // Raw 8B lane (§4.7).
                            if (value != value)
                                continue;
                        } else if (!slot->get())
                            continue;
                        propertyNames.add(i);
                    }
                    break;
                }
                Butterfly* butterfly = untaggedButterfly(word);
                unsigned usedLength = std::min(butterfly->publicLength(), butterfly->vectorLength()); // Bound from the SAME loaded butterfly.
                for (unsigned i = 0; i < usedLength; ++i) {
                    if (isDouble) {
                        double value = butterfly->contiguousDouble().at(object, i);
                        if (value != value)
                            continue;
                    } else if (!butterfly->contiguous().at(object, i))
                        continue;
                    propertyNames.add(i);
                }
                break;
            }
            case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
                Vector<unsigned, 16> indexes;
                {
                    Locker locker { object->cellLock() }; // I31/L5
                    ArrayStorage* storage = object->butterfly()->arrayStorage(); // Re-loaded under the lock (AS-COPY republication).
                    unsigned usedVectorLength = std::min(storage->length(), storage->vectorLength());
                    for (unsigned i = 0; i < usedVectorLength; ++i) {
                        if (storage->m_vector[i])
                            indexes.append(i);
                    }
                    if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
                        // AB18-G: locked iteration; functor only appends to a
                        // fastMalloc Vector (no JS, no GC allocation).
                        map->forEachEntry([&](uint64_t key, const SparseArrayEntry& entry) {
                            if (mode == DontEnumPropertiesMode::Include || !(entry.attributes() & PropertyAttribute::DontEnum))
                                indexes.append(static_cast<unsigned>(key));
                        });
                    }
                }
                std::ranges::sort(indexes);
                for (unsigned index : indexes)
                    propertyNames.add(index);
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
            return;
        }
#endif
        switch (object->indexingType()) {
        case ALL_BLANK_INDEXING_TYPES:
        case ALL_UNDECIDED_INDEXING_TYPES:
            break;
            
        case ALL_INT32_INDEXING_TYPES:
        case ALL_CONTIGUOUS_INDEXING_TYPES: {
            Butterfly* butterfly = object->butterfly();
            unsigned usedLength = butterfly->publicLength();
            for (unsigned i = 0; i < usedLength; ++i) {
                if (!butterfly->contiguous().at(object, i))
                    continue;
                propertyNames.add(i);
            }
            break;
        }
            
        case ALL_DOUBLE_INDEXING_TYPES: {
            Butterfly* butterfly = object->butterfly();
            unsigned usedLength = butterfly->publicLength();
            for (unsigned i = 0; i < usedLength; ++i) {
                double value = butterfly->contiguousDouble().at(object, i);
                if (value != value)
                    continue;
                propertyNames.add(i);
            }
            break;
        }
            
        case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
            ArrayStorage* storage = object->butterfly()->arrayStorage();
            
            unsigned usedVectorLength = std::min(storage->length(), storage->vectorLength());
            for (unsigned i = 0; i < usedVectorLength; ++i) {
                if (storage->m_vector[i])
                    propertyNames.add(i);
            }
            
            if (SparseArrayValueMap* map = storage->m_sparseMap.get()) {
                auto keys = WTF::compactMap<0, UnsafeVectorOverflow>(*map, [mode](auto& entry) ->std::optional<unsigned> {
                    if (mode == DontEnumPropertiesMode::Include || !(entry.value.attributes() & PropertyAttribute::DontEnum))
                        return static_cast<unsigned>(entry.key);
                    return std::nullopt;
                });
                
                std::ranges::sort(keys);
                for (unsigned i = 0; i < keys.size(); ++i)
                    propertyNames.add(keys[i]);
            }
            break;
        }
            
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
}

void JSObject::getOwnNonIndexPropertyNames(JSGlobalObject* globalObject, PropertyNameArrayBuilder& propertyNames, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    methodTable()->getOwnSpecialPropertyNames(this, globalObject, propertyNames, mode);
    RETURN_IF_EXCEPTION(scope, void());

    scope.release();
    getNonReifiedStaticPropertyNames(vm, propertyNames, mode);
    structure()->getPropertyNamesFromStructure(vm, propertyNames, mode);
}

double JSObject::toNumber(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue primitive = toPrimitive(globalObject, PreferNumber);
    RETURN_IF_EXCEPTION(scope, 0.0); // should be picked up soon in Nodes.cpp
    RELEASE_AND_RETURN(scope, primitive.toNumber(globalObject));
}

JSString* JSObject::toString(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isJSArray(this)) {
        auto* array = uncheckedDowncast<JSArray>(const_cast<JSObject*>(this));
        if (array->isToPrimitiveFastAndNonObservable()) [[likely]]
            RELEASE_AND_RETURN(scope, array->fastToString(globalObject));
    }

    JSValue primitive = callToPrimitiveFunction<CachedSpecialPropertyKey::ToPrimitive>(globalObject, this, vm.propertyNames->toPrimitiveSymbol, PreferString);
    RETURN_IF_EXCEPTION(scope, jsEmptyString(vm));
    if (!primitive) [[likely]] {
        primitive = ordinaryToPrimitive(globalObject, PreferString);
        RETURN_IF_EXCEPTION(scope, jsEmptyString(vm));
    }

    RELEASE_AND_RETURN(scope, primitive.toString(globalObject));
}

void JSObject::seal(VM& vm)
{
    if (isSealed(vm))
        return;
    enterDictionaryIndexingMode(vm);
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::sealTransition(vm, oldStructure, &deferred));
    }
}

void JSObject::freeze(VM& vm)
{
    if (isFrozen(vm))
        return;
    enterDictionaryIndexingMode(vm);
    {
        Structure* oldStructure = structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        setStructure(vm, Structure::freezeTransition(vm, oldStructure, &deferred));
    }
}

bool JSObject::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    if (Options::useJSThreads() && object->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
        return false;
    if (!object->isStructureExtensible()) {
        // We've already set the internal [[PreventExtensions]] field to false.
        // We don't call the methodTable isExtensible here because it's not defined
        // that way in the specification. We are just doing an optimization here.
        return true;
    }

    object->enterDictionaryIndexingMode(vm);
    {
        Structure* oldStructure = object->structure();
        DeferredStructureTransitionWatchpointFire deferred(vm, oldStructure);
        object->setStructure(vm, Structure::preventExtensionsTransition(vm, oldStructure, &deferred));
    }
    return true;
}

bool JSObject::isExtensible(JSObject* obj, JSGlobalObject* globalObject)
{
    if (Options::useJSThreads() && obj->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, obj)) [[unlikely]]
        return false;
    return obj->isStructureExtensible();
}

bool JSObject::isExtensible(JSGlobalObject* globalObject)
{ 
    return methodTable()->isExtensible(this, globalObject);
}

void JSObject::reifyAllStaticProperties(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    ASSERT(!staticPropertiesReified());

    // If this object's ClassInfo has no static properties, then nothing to reify!
    // We can safely set the flag to avoid the expensive check again in the future.
    if (!TypeInfo::hasStaticPropertyTable(inlineTypeFlags())) {
        structure()->setStaticPropertiesReified(true);
        return;
    }

    if (!structure()->isDictionary())
        convertToDictionary(vm);

    for (const ClassInfo* info = classInfo(); info; info = info->parentClass) {
        const HashTable* hashTable = info->staticPropHashTable;
        if (!hashTable)
            continue;

        for (auto& value : *hashTable) {
            unsigned attributes;
            auto key = Identifier::fromString(vm, value.m_key);
            PropertyOffset offset = getDirectOffset(vm, key, attributes);
            if (!isValidOffset(offset))
                reifyStaticProperty(vm, hashTable->classForThis, key, value, *this);
        }
    }

    structure()->setStaticPropertiesReified(true);
}

NEVER_INLINE void JSObject::fillGetterPropertySlot(VM&, PropertySlot& slot, JSCell* getterSetter, unsigned attributes, PropertyOffset offset)
{
    if (structure()->isUncacheableDictionary()) {
        slot.setGetterSlot(this, attributes, uncheckedDowncast<GetterSetter>(getterSetter));
        return;
    }

    // This access is cacheable because Structure requires an attributeChangedTransition
    // if this property stops being an accessor.
    slot.setCacheableGetterSlot(this, attributes, uncheckedDowncast<GetterSetter>(getterSetter), offset);
}

static bool putIndexedDescriptor(JSGlobalObject* globalObject, SparseArrayValueMap* map, SparseArrayEntry* entryInMap, const PropertyDescriptor& descriptor, PropertyDescriptor& oldDescriptor)
{
    VM& vm = globalObject->vm();

    if (descriptor.isDataDescriptor()) {
        unsigned attributes = descriptor.attributesOverridingCurrent(oldDescriptor) & ~PropertyAttribute::Accessor;
        if (descriptor.value())
            entryInMap->forceSet(vm, map, descriptor.value(), attributes);
        else if (oldDescriptor.isAccessorDescriptor())
            entryInMap->forceSet(vm, map, jsUndefined(), attributes);
        else
            entryInMap->forceSet(map, attributes);
        return true;
    }

    if (descriptor.isAccessorDescriptor()) {
        JSObject* getter = nullptr;
        if (descriptor.getterPresent())
            getter = descriptor.getterObject();
        else if (oldDescriptor.isAccessorDescriptor())
            getter = oldDescriptor.getterObject();
        JSObject* setter = nullptr;
        if (descriptor.setterPresent())
            setter = descriptor.setterObject();
        else if (oldDescriptor.isAccessorDescriptor())
            setter = oldDescriptor.setterObject();

        GetterSetter* accessor = GetterSetter::create(vm, globalObject, getter, setter);
        entryInMap->forceSet(vm, map, accessor, descriptor.attributesOverridingCurrent(oldDescriptor) & ~PropertyAttribute::ReadOnly);
        return true;
    }

    ASSERT(descriptor.isGenericDescriptor());
    entryInMap->forceSet(map, descriptor.attributesOverridingCurrent(oldDescriptor));
    return true;
}

ALWAYS_INLINE static bool canDoFastPutDirectIndex(JSObject* object)
{
    if (TypeInfo::isArgumentsType(object->type()))
        return true;

    if (object->inSparseIndexingMode())
        return false;

    return (isJSArray(object) && !isCopyOnWrite(object->indexingMode()))
        || is<JSFinalObject>(object);
}

// https://tc39.es/ecma262/#sec-ordinarydefineownproperty
bool JSObject::defineOwnIndexedProperty(JSGlobalObject* globalObject, unsigned index, const PropertyDescriptor& descriptor, bool throwException)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(index <= MAX_ARRAY_INDEX);

    ensureWritable(vm);

    if (!inSparseIndexingMode()) {
        const PropertyDescriptor emptyAttributesDescriptor(jsUndefined(), static_cast<unsigned>(PropertyAttribute::None));
        ASSERT(emptyAttributesDescriptor.attributes() == static_cast<unsigned>(PropertyAttribute::None));

#if ASSERT_ENABLED
        if (canGetIndexQuickly(index) && canDoFastPutDirectIndex(this)) {
            DeferTermination deferScope(vm);
            PropertyDescriptor currentDescriptor;
            bool found = getOwnPropertyDescriptor(globalObject, Identifier::from(vm, index), currentDescriptor);
            scope.assertNoException();
            if (found)
                ASSERT(currentDescriptor.attributes() == emptyAttributesDescriptor.attributes());
        }
#endif
        // Fast case: we're putting a regular property to a regular array
        if (descriptor.value()
            && (!descriptor.attributes() || (canGetIndexQuickly(index) && !descriptor.attributesOverridingCurrent(emptyAttributesDescriptor)))
            && canDoFastPutDirectIndex(this)) {
            ASSERT(!descriptor.isAccessorDescriptor());
            RELEASE_AND_RETURN(scope, putDirectIndex(globalObject, index, descriptor.value(), 0, throwException ? PutDirectIndexShouldThrow : PutDirectIndexShouldNotThrow));
        }
        
        ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm);
    }

    if (descriptor.attributes() & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor))
        notifyPresenceOfIndexedAccessors(vm);

    // 1. Let current be the result of calling the [[GetOwnProperty]] internal method of O with property name P.
    // SPEC-objectmodel I31/L5 (review round 1): flag-on, the AS header read
    // and the map STRUCTURAL edit (add => possible rehash) run under the cell
    // lock so they cannot race the locked readers (deletePropertyByIndex,
    // getOwnIndexedPropertyNames, Task 8's locked walks). map->add only
    // fastMallocs (no GC allocation, no JS) so it is wrappable (O1).
    // Residual: SparseArrayValueMap::putEntry's INTERNAL add at other call
    // sites still needs the allocate-then-locked-insert split recorded in
    // INTEGRATE-objectmodel.md (SparseArrayValueMap.* is outside this part's
    // owned paths).
    SparseArrayValueMap* map = nullptr;
    auto addToMap = [&]() -> SparseArrayValueMap::AddResult {
        map = this->butterfly()->arrayStorage()->m_sparseMap.get(); // (Re-)read under the lock flag-on (AS-COPY republication).
        RELEASE_ASSERT(map);
        return map->add(this, index);
    };
    SparseArrayValueMap::AddResult result = ([&] {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            // MC-REENT S3c residual close-out (cve-reent-sparse-map-null): a
            // foreign racer can clear m_sparseMap in our decision-to-lock
            // window — the map->vector consolidation arms of
            // putByIndexBeyondVectorLengthWithoutAttributes /
            // putDirectIndexBeyondVectorLengthWithArrayStorage read
            // map->sparseMode() UNLOCKED at their density checks; a read that
            // beat ensure...'s setSparseMode walks into the locked
            // consolidation block and deallocateSparseIndexMap clears the map
            // AFTER we committed to the sparse path (an AS-COPY republication
            // that drops the header pointer is the same observable). A null
            // map under this lock is therefore a lost decision, not a broken
            // invariant: re-derive by installing a fresh sparse-mode map —
            // allocate OUTSIDE the lock (GC allocation, O1), install-if-absent
            // under it (putDirectIndexForAtomicsMissingAdd's pendingMap
            // discipline) — then let the heal loop below re-absorb whatever
            // the consolidation left in the vector. setSparseMode is flipped
            // BEFORE publication, while the map is still private: sparseMode
            // is read UNLOCKED at the consolidation density checks, so a map
            // published non-sparse opens a window for a foreign putter to
            // pass its density check against the fresh map and later flatten
            // the descriptor we are about to install via getNonSparseMode.
            // The consolidation blocks now also re-validate map identity +
            // !sparseMode under their cellLock, so no consolidator can clear
            // a sparse-mode map installed by a committed definer.
            SparseArrayValueMap* pendingMap = nullptr; // GC-visible via conservative stack scan.
            while (true) {
                {
                    Locker locker { cellLock() };
                    ArrayStorage* storage = this->butterfly()->arrayStorage();
                    SparseArrayValueMap* lockedMap = storage->m_sparseMap.get();
                    // GIL-on the GIL is never released inside this C++ op, so
                    // the map installed by ensure... above must still be here.
                    ASSERT(lockedMap || !Options::useThreadGIL());
                    if (!lockedMap && pendingMap) [[unlikely]] {
                        // Flip sparseMode while still private (see above),
                        // then publish. We ARE the define path: ensure...
                        // already chose dictionary indexing; sparseMode also
                        // makes the heal loop below migrate the consolidated
                        // vector back, so the racer's values are absorbed,
                        // never shadowed.
                        pendingMap->setSparseMode();
                        storage->m_sparseMap.set(vm, this, pendingMap);
                        lockedMap = pendingMap;
                        pendingMap = nullptr;
                    }
                    if (lockedMap) [[likely]] {
                        // MC-REENT S3c close-out: re-establish the sparse-mode invariant
                        // (vectorLength == 0, all values in the map) under this cellLock
                        // BEFORE the add. GIL-off, a racing attribute-0 indexed add
                        // (Atomics.store's helper, or a plain put) can grow and/or fill
                        // the vector in the window between this object's
                        // createArrayStorage and the allocateSparseIndexMap/setSparseMode
                        // pair (at decision time there was no map at all) — GIL-on the
                        // mode flip and the vector state are atomic, so this state is
                        // unreachable. Left in place it is heap corruption in both
                        // directions: the AS lookup is "if (i < vectorLength) vector
                        // ELSE IF (map)", so a non-empty vector slot SHADOWS the
                        // descriptor we are about to publish, and an empty grown vector
                        // makes every map entry below vectorLength UNREADABLE. Migration
                        // mirrors enterDictionaryIndexingModeWhenArrayStorageAlreadyExists'
                        // locked loop (map->add fastMallocs only — O1-wrappable, same as
                        // the call below); it runs BEFORE the add so the AddResult
                        // iterator can never dangle across a heal-induced rehash
                        // (AB18-G). A migrated value at OUR index then surfaces through
                        // !isNewEntry as the current {value, attrs 0} property and takes
                        // the reconfiguration path — exactly the store-then-define
                        // linearization the GIL-off Atomics protocol pins.
                        if (lockedMap->sparseMode() && storage->vectorLength()) [[unlikely]] {
                            unsigned usedVectorLength = std::min(storage->length(), storage->vectorLength());
                            for (unsigned j = 0; j < usedVectorLength; ++j) {
                                JSValue strayValue = storage->m_vector[j].get();
                                if (!strayValue)
                                    continue;
                                auto strayResult = lockedMap->add(this, j);
                                if (strayResult.isNewEntry)
                                    strayResult.iterator->value.forceSet(vm, lockedMap, strayValue, 0);
                                // !isNewEntry: the map entry was published through the
                                // locked map protocol AFTER the stray fill — it wins;
                                // the stray value is the older, absorbed write.
                                storage->m_vector[j].clear();
                            }
                            storage->m_numValuesInVector = 0;
                            storage->setVectorLength(0);
                        }
                        map = lockedMap;
                        return map->add(this, index);
                    }
                }
                // Null map, nothing staged: allocate outside the lock and retry.
                pendingMap = SparseArrayValueMap::create(vm);
            }
        }
#endif
        return addToMap();
    })();
    SparseArrayEntry* entryInMap = &result.iterator->value;

    // 2. Let extensible be the value of the [[Extensible]] internal property of O.
    // 3. If current is undefined and extensible is false, then Reject.
    // 4. If current is undefined and extensible is true, then
    if (result.isNewEntry) {
        if (!isStructureExtensible()) {
#if USE(JSVALUE64)
            if (Options::useJSThreads()) [[unlikely]] {
                Locker locker { cellLock() }; // I31/L5: structural edit (possible rehash) under the cell lock.
                map->remove(result.iterator);
            } else
#endif
            map->remove(result.iterator);
            return typeError(globalObject, scope, throwException, NonExtensibleObjectPropertyDefineError);
        }

        // 4.a. If IsGenericDescriptor(Desc) or IsDataDescriptor(Desc) is true, then create an own data property
        // named P of object O whose [[Value]], [[Writable]], [[Enumerable]] and [[Configurable]] attribute values
        // are described by Desc. If the value of an attribute field of Desc is absent, the attribute of the newly
        // created property is set to its default value.
        // 4.b. Else, Desc must be an accessor Property Descriptor so, create an own accessor property named P of
        // object O whose [[Get]], [[Set]], [[Enumerable]] and [[Configurable]] attribute values are described by
        // Desc. If the value of an attribute field of Desc is absent, the attribute of the newly created property
        // is set to its default value.
        // 4.c. Return true.

        PropertyDescriptor defaults(jsUndefined(), PropertyAttribute::DontDelete | PropertyAttribute::DontEnum | PropertyAttribute::ReadOnly);
        putIndexedDescriptor(globalObject, map, entryInMap, descriptor, defaults);
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            // I31/L5: the AS length bump is an in-place header write; take the
            // cell lock and re-read the storage under it (AS-COPY).
            Locker locker { cellLock() };
            ArrayStorage* storage = this->butterfly()->arrayStorage();
            if (index >= storage->length())
                storage->setLength(index + 1);
            return true;
        }
#endif
        auto* butterfly = this->butterfly();
        if (index >= butterfly->arrayStorage()->length())
            butterfly->arrayStorage()->setLength(index + 1);
        return true;
    }

    // 5. Return true, if every field in Desc is absent.
    // 6. Return true, if every field in Desc also occurs in current and the value of every field in Desc is the same value as the corresponding field in current when compared using the SameValue algorithm (9.12).
    PropertyDescriptor current;
    entryInMap->get(current);
    bool isEmptyOrEqual = descriptor.isEmpty() || descriptor.equalTo(globalObject, current);
    RETURN_IF_EXCEPTION(scope, false);
    if (isEmptyOrEqual)
        return true;

    // 7. If the [[Configurable]] field of current is false then
    if (!current.configurable()) {
        // 7.a. Reject, if the [[Configurable]] field of Desc is true.
        if (descriptor.configurablePresent() && descriptor.configurable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeConfigurabilityError);
        // 7.b. Reject, if the [[Enumerable]] field of Desc is present and the [[Enumerable]] fields of current and Desc are the Boolean negation of each other.
        if (descriptor.enumerablePresent() && current.enumerable() != descriptor.enumerable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeEnumerabilityError);
    }

    // 8. If IsGenericDescriptor(Desc) is true, then no further validation is required.
    if (!descriptor.isGenericDescriptor()) {
        // 9. Else, if IsDataDescriptor(current) and IsDataDescriptor(Desc) have different results, then
        if (current.isDataDescriptor() != descriptor.isDataDescriptor()) {
            // 9.a. Reject, if the [[Configurable]] field of current is false.
            if (!current.configurable())
                return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeAccessMechanismError);
            // 9.b. If IsDataDescriptor(current) is true, then convert the property named P of object O from a
            // data property to an accessor property. Preserve the existing values of the converted property's
            // [[Configurable]] and [[Enumerable]] attributes and set the rest of the property's attributes to
            // their default values.
            // 9.c. Else, convert the property named P of object O from an accessor property to a data property.
            // Preserve the existing values of the converted property's [[Configurable]] and [[Enumerable]]
            // attributes and set the rest of the property's attributes to their default values.
        } else if (current.isDataDescriptor() && descriptor.isDataDescriptor()) {
            // 10. Else, if IsDataDescriptor(current) and IsDataDescriptor(Desc) are both true, then
            // 10.a. If the [[Configurable]] field of current is false, then
            if (!current.configurable() && !current.writable()) {
                // 10.a.i. Reject, if the [[Writable]] field of current is false and the [[Writable]] field of Desc is true.
                if (descriptor.writable())
                    return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeWritabilityError);
                // 10.a.ii. If the [[Writable]] field of current is false, then
                // 10.a.ii.1. Reject, if the [[Value]] field of Desc is present and SameValue(Desc.[[Value]], current.[[Value]]) is false.
                if (descriptor.value()) {
                    bool isSame = sameValue(globalObject, descriptor.value(), current.value());
                    RETURN_IF_EXCEPTION(scope, false);
                    if (!isSame)
                        return typeError(globalObject, scope, throwException, ReadonlyPropertyChangeError);
                }
            }
            // 10.b. else, the [[Configurable]] field of current is true, so any change is acceptable.
        } else {
            ASSERT(current.isAccessorDescriptor() && current.getterPresent() && current.setterPresent());
            // 11. Else, IsAccessorDescriptor(current) and IsAccessorDescriptor(Desc) are both true so, if the [[Configurable]] field of current is false, then
            if (!current.configurable()) {
                // 11.i. Reject, if the [[Set]] field of Desc is present and SameValue(Desc.[[Set]], current.[[Set]]) is false.
                if (descriptor.setterPresent() && descriptor.setter() != current.setter())
                    return typeError(globalObject, scope, throwException, "Attempting to change the setter of an unconfigurable property."_s);
                // 11.ii. Reject, if the [[Get]] field of Desc is present and SameValue(Desc.[[Get]], current.[[Get]]) is false.
                if (descriptor.getterPresent() && descriptor.getter() != current.getter())
                    return typeError(globalObject, scope, throwException, "Attempting to change the getter of an unconfigurable property."_s);
            }
        }
    }

    // 12. For each attribute field of Desc that is present, set the correspondingly named attribute of the property named P of object O to the value of the field.
    putIndexedDescriptor(globalObject, map, entryInMap, descriptor, current);
    // 13. Return true.
    return true;
}

SparseArrayValueMap* JSObject::allocateSparseIndexMap(VM& vm)
{
    SparseArrayValueMap* result = SparseArrayValueMap::create(vm);
    arrayStorage()->m_sparseMap.set(vm, this, result);
    return result;
}

void JSObject::deallocateSparseIndexMap()
{
    if (ArrayStorage* arrayStorage = arrayStorageOrNull())
        arrayStorage->m_sparseMap.clear();
}

bool JSObject::attemptToInterceptPutByIndexOnHoleForPrototype(JSGlobalObject* globalObject, JSValue thisValue, unsigned i, JSValue value, bool shouldThrow, bool& putResult)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    for (JSObject* current = this; ;) {
        // This has the same behavior with respect to prototypes as JSObject::put(). It only
        // allows a prototype to intercept a put if (a) the prototype declares the property
        // we're after rather than intercepting it via an override of JSObject::put(), and
        // (b) that property is declared as ReadOnly or Accessor.
        
        ArrayStorage* storage = current->arrayStorageOrNull();
        if (storage && storage->m_sparseMap) {
            SparseArrayValueMap* map = storage->m_sparseMap.get();
#if USE(JSVALUE64)
            if (Options::useJSThreads()) [[unlikely]] {
                // AB18-G: the unlocked find() races a locked mutator's rehash;
                // take a locked snapshot instead. The guarded branches never
                // write through the snapshot: Accessor calls the setter (runs
                // JS, outside any lock, as today) and ReadOnly only throws.
                std::optional<SparseArrayEntry> entry = map->getEntry(i);
                if (entry && (entry->attributes() & (PropertyAttribute::Accessor | PropertyAttribute::ReadOnly))) {
                    scope.release();
                    putResult = entry->put(globalObject, thisValue, map, value, shouldThrow);
                    return true;
                }
            } else
#endif
            {
                SparseArrayValueMap::iterator iter = map->find(i);
                if (iter != map->notFound() && (iter->value.attributes() & (PropertyAttribute::Accessor | PropertyAttribute::ReadOnly))) {
                    scope.release();
                    putResult = iter->value.put(globalObject, thisValue, map, value, shouldThrow);
                    return true;
                }
            }
        }

        if (current->type() == ProxyObjectType) {
            scope.release();
            auto* proxy = uncheckedDowncast<ProxyObject>(current);
            putResult = proxy->putByIndexCommon(globalObject, thisValue, i, value, shouldThrow);
            return true;
        }

        if (isTypedArrayType(current->type())) {
            auto* typedArray = uncheckedDowncast<JSArrayBufferView>(current);
            if (typedArray->isOutOfBounds() || i >= typedArray->length()) {
                putResult = true;
                return true;
            }
            return false;
        }

        JSValue prototypeValue = current->getPrototype(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (prototypeValue.isNull())
            return false;
        
        current = asObject(prototypeValue);
    }
}

bool JSObject::attemptToInterceptPutByIndexOnHole(JSGlobalObject* globalObject, unsigned i, JSValue value, bool shouldThrow, bool& putResult)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue prototypeValue = getPrototype(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (prototypeValue.isNull())
        return false;
    
    RELEASE_AND_RETURN(scope, asObject(prototypeValue)->attemptToInterceptPutByIndexOnHoleForPrototype(globalObject, this, i, value, shouldThrow, putResult));
}

template<IndexingType indexingShape>
bool JSObject::putByIndexBeyondVectorLengthWithoutAttributes(JSGlobalObject* globalObject, unsigned i, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    RELEASE_ASSERT_WITH_SECURITY_IMPLICATION(!isCopyOnWrite(indexingMode()));
    ASSERT((indexingType() & IndexingShapeMask) == indexingShape);
    ASSERT(!indexingShouldBeSparse());

#if USE(JSVALUE64)
    // Review round 2 (blocker fix): flag-on, this path is reached with
    // segmented words (the quickly-family declines indices outside the loaded
    // spine), so the flat-only butterfly() deref below is unsound. Dispatch on
    // the tagged word and store through the §9.5 accessors; growth goes
    // through the §4.4 drivers (ensureLength flag-on routes to
    // ensureLengthSlowConcurrent).
    if (Options::useJSThreads()) [[unlikely]] {
        auto* object = static_cast<JSObjectWithButterfly*>(this);
        while (true) {
            uint64_t word = object->taggedButterflyWord();
            RELEASE_ASSERT(word & butterflyPointerMask);
            unsigned vectorLength;
            unsigned publicLength;
            if (isSegmentedButterfly(word)) [[unlikely]] {
                ButterflySpine* spine = butterflySpine(word);
                vectorLength = spine->vectorLength;
                publicLength = segmentedPublicLength(spine);
            } else {
                Butterfly* flat = untaggedButterfly(word);
                vectorLength = flat->vectorLength();
                publicLength = flat->publicLength();
            }
            // Density heuristic: min(publicLength, vectorLength) is an upper
            // bound on the dense element count. The legacy countElements walk
            // is unsafe on shared/segmented storage, and the count is only a
            // layout-POLICY input (dense vs ArrayStorage), never a soundness
            // input - a different estimate changes layout choice only.
            if (i > MAX_STORAGE_VECTOR_INDEX
                || (i >= MIN_SPARSE_ARRAY_INDEX && !isDenseEnoughForVector(i, std::min(publicLength, vectorLength)))
                || indexIsSufficientlyBeyondLengthForSparseMap(i, vectorLength)) {
                ASSERT(i <= MAX_ARRAY_INDEX);
                ArrayStorage* storage = ensureArrayStorageSlow(vm); // §4.6 stop-routed flag-on.
                RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, false, storage));
            }
            if ((indexingType() & IndexingShapeMask) != indexingShape) {
                // A racing relabel/conversion changed the shape: re-dispatch
                // the full put path on the settled state.
                RELEASE_AND_RETURN(scope, putByIndex(this, globalObject, i, value, false));
            }
            if (trySetIndexQuicklyConcurrent(vm, i, value, nullptr))
                return true;
            if ((indexingType() & IndexingShapeMask) != indexingShape)
                continue; // The world moved mid-store: re-classify.
            if ((hasInt32(indexingType()) && !value.isInt32())
                || (hasDouble(indexingType()) && (!value.isNumber() || value.asNumber() != value.asNumber()))) {
                // Defensive loop-breaker: the template contract (value matches
                // the shape) only ASSERTs; a racing interleaving that restores
                // a mismatched shape would otherwise spin here. Re-dispatch.
                RELEASE_AND_RETURN(scope, putByIndex(this, globalObject, i, value, false));
            }
            if (!ensureLength(vm, i + 1)) {
                throwOutOfMemoryError(globalObject, scope);
                return false;
            }
            // Grown (or a racer grew for us): loop and retry the store.
        }
    }
#endif

    auto* butterfly = this->butterfly();

    // For us to get here, the index is either greater than the public length, or greater than
    // or equal to the vector length.
    ASSERT(i >= butterfly->vectorLength());

    if (i > MAX_STORAGE_VECTOR_INDEX
        || (i >= MIN_SPARSE_ARRAY_INDEX && !isDenseEnoughForVector(i, countElements<indexingShape>(butterfly)))
        || indexIsSufficientlyBeyondLengthForSparseMap(i, butterfly->vectorLength())) {
        ASSERT(i <= MAX_ARRAY_INDEX);
        ArrayStorage* storage = ensureArrayStorageSlow(vm);
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, false, storage));
    }

    if (!ensureLength(vm, i + 1)) {
        throwOutOfMemoryError(globalObject, scope);
        return false;
    }
    butterfly = this->butterfly();

    RELEASE_ASSERT(i < butterfly->vectorLength());
    switch (indexingShape) {
    case Int32Shape:
        ASSERT(value.isInt32());
        butterfly->contiguous().at(this, i).setWithoutWriteBarrier(value);
        return true;
        
    case DoubleShape: {
        ASSERT(Options::allowDoubleShape());
        ASSERT(value.isNumber());
        double valueAsDouble = value.asNumber();
        ASSERT(valueAsDouble == valueAsDouble);
        butterfly->contiguousDouble().at(this, i) = valueAsDouble;
        return true;
    }
        
    case ContiguousShape:
        butterfly->contiguous().at(this, i).set(vm, this, value);
        return true;
        
    default:
        CRASH();
        return false;
    }
}

// Explicit instantiations needed by JSArray.cpp.
template bool JSObject::putByIndexBeyondVectorLengthWithoutAttributes<Int32Shape>(JSGlobalObject*, unsigned, JSValue);
template bool JSObject::putByIndexBeyondVectorLengthWithoutAttributes<DoubleShape>(JSGlobalObject*, unsigned, JSValue);
template bool JSObject::putByIndexBeyondVectorLengthWithoutAttributes<ContiguousShape>(JSGlobalObject*, unsigned, JSValue);

bool JSObject::putByIndexBeyondVectorLengthWithArrayStorage(JSGlobalObject* globalObject, unsigned i, JSValue value, bool shouldThrow, ArrayStorage* storage)
{
    VM& vm = globalObject->vm();
    // We're transitioning between states here, if a termination comes in we could leave the object
    // in an inconsistent state. We could still be in the middle a GC during termination so we could
    // try to mark this object and crash. It's much easier to just not think about it.
    DeferTerminationForAWhile noTermination(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(!isCopyOnWrite(indexingMode()));
    // i should be a valid array index that is outside of the current vector.
    ASSERT(i <= MAX_ARRAY_INDEX);
    ASSERT(i >= storage->vectorLength());
    
    SparseArrayValueMap* map = storage->m_sparseMap.get();
    
    // SPEC-objectmodel I31/L5 (Task 9, flag-on): in-place AS mutations below
    // (setLength, m_vector stores, m_numValuesInVector, the map->vector copy)
    // take the cell lock around each window, re-reading arrayStorage() under
    // the lock (AS-COPY may have republished a fresh AS butterfly, §4.6). The
    // GC-allocating steps (increaseVectorLength - itself internally locked -
    // allocateSparseIndexMap, map->putEntry, typeError) stay OUTSIDE the lock
    // (O1). Review round 1: each unlocked putEntry/putDirect is now followed
    // by a cell-locked map-identity revalidation (a racing map->vector copy
    // orphaning the map => the whole operation re-runs, closing the I21
    // lost-add window). putEntry's INTERNAL structural edits still race
    // cell-locked readers of the SAME map; the allocate-then-locked-insert
    // split lives in SparseArrayValueMap.{h,cpp} (outside this part's owned
    // paths) and is recorded as a ready-to-apply shared-file request in
    // INTEGRATE-objectmodel.md.
    // First, handle cases where we don't currently have a sparse map.
    if (!map) [[likely]] {
        // If the array is not extensible, we should have entered dictionary mode, and created the sparse map.
        ASSERT(isStructureExtensible());

        // Update m_length if necessary.
        if (i >= storage->length()) {
#if USE(JSVALUE64)
            if (Options::useJSThreads()) [[unlikely]] {
                Locker locker { cellLock() };
                storage = arrayStorage();
                if (i >= storage->length())
                    storage->setLength(i + 1);
            } else
#endif
            storage->setLength(i + 1);
        }

        // Check that it is sensible to still be using a vector, and then try to grow the vector.
        if (!indexIsSufficientlyBeyondLengthForSparseMap(i, storage->vectorLength())
            && isDenseEnoughForVector(i, storage->m_numValuesInVector)
            && increaseVectorLength(vm, i + 1)) [[likely]] {
            // success! - reread m_storage since it has likely been reallocated, and store to the vector.
#if USE(JSVALUE64)
            if (Options::useJSThreads()) [[unlikely]] {
                Locker locker { cellLock() };
                storage = arrayStorage();
                WriteBarrier<Unknown>& valueSlot = storage->m_vector[i];
                if (!valueSlot) // A racing locked writer may have filled it.
                    ++storage->m_numValuesInVector;
                valueSlot.set(vm, this, value);
                return true;
            }
#endif
            storage = arrayStorage();
            storage->m_vector[i].set(vm, this, value);
            ++storage->m_numValuesInVector;
            return true;
        }
        // We don't want to, or can't use a vector to hold this property - allocate a sparse map & add the value.
        map = allocateSparseIndexMap(vm);
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            // I21 (review round 1): putEntry runs unlocked (it allocates and
            // can run JS), so a racing locked map->vector copy can orphan
            // `map` mid-call and silently drop this add. Re-validate the map's
            // identity under the cell lock afterwards; on divergence re-run
            // the whole operation against the fresh state.
            bool putResult = map->putEntry(globalObject, this, i, value, shouldThrow);
            RETURN_IF_EXCEPTION(scope, false);
            bool stillInstalled;
            {
                Locker locker { cellLock() };
                ArrayStorage* freshStorage = arrayStorageOrNull();
                stillInstalled = freshStorage && freshStorage->m_sparseMap.get() == map;
            }
            if (stillInstalled) [[likely]]
                return putResult;
            RELEASE_AND_RETURN(scope, putByIndexInline(globalObject, i, value, shouldThrow));
        }
#endif
        RELEASE_AND_RETURN(scope, map->putEntry(globalObject, this, i, value, shouldThrow));
    }

    // Update m_length if necessary.
    unsigned length = storage->length();
    if (i >= length) {
        // Prohibit growing the array if length is not writable.
        if (map->lengthIsReadOnly() || !isStructureExtensible())
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
        length = i + 1;
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            Locker locker { cellLock() };
            storage = arrayStorage();
            if (length > storage->length())
                storage->setLength(length);
        } else
#endif
        storage->setLength(length);
    }

    // We are currently using a map - check whether we still want to be doing so.
    // We will continue  to use a sparse map if SparseMode is set, a vector would be too sparse, or if allocation fails.
    unsigned numValuesInArray = storage->m_numValuesInVector + map->size();
    if (map->sparseMode() || !isDenseEnoughForVector(length, numValuesInArray) || !increaseVectorLength(vm, length)) {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            // I21 (review round 1): see the fresh-map putEntry site above.
            bool putResult = map->putEntry(globalObject, this, i, value, shouldThrow);
            RETURN_IF_EXCEPTION(scope, false);
            bool stillInstalled;
            {
                Locker locker { cellLock() };
                ArrayStorage* freshStorage = arrayStorageOrNull();
                stillInstalled = freshStorage && freshStorage->m_sparseMap.get() == map;
            }
            if (stillInstalled) [[likely]]
                return putResult;
            RELEASE_AND_RETURN(scope, putByIndexInline(globalObject, i, value, shouldThrow));
        }
#endif
        RELEASE_AND_RETURN(scope, map->putEntry(globalObject, this, i, value, shouldThrow));
    }

#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        // Map -> vector copy + final store, in one locked window (I31; no GC
        // allocation inside: barriered stores + fastMalloc frees only).
        {
            Locker locker { cellLock() };
            storage = arrayStorage();
            // I21 (cve-reent-sparse-map-null, producer side): our sparseMode
            // and identity reads above were UNLOCKED and are now stale-able —
            // increaseVectorLength can block arbitrarily (GC allocation), so
            // the decision window is unbounded. Re-validate under the lock
            // that the map we are about to consolidate is still the installed
            // map and is still non-sparse: a committed definer may have
            // flipped sparseMode (enterDictionaryIndexingMode...) or
            // reinstalled a fresh sparse-mode map after a racer's
            // consolidation cleared ours. Consolidating a stale map would
            // copy orphaned entries over live state and deallocate the
            // definer's map — a silent lost define / attribute strip. On
            // divergence, drop the lock and re-run against fresh state (same
            // recovery as the stillInstalled arms above).
            if (storage->m_sparseMap.get() == map && !map->sparseMode()) [[likely]] {
                storage->m_numValuesInVector = numValuesInArray;
                WriteBarrier<Unknown>* vector = storage->m_vector;
                // AB18-G: locked iteration — an unlocked begin()/end() walk races a
                // putEntry-internal rehash. The functor does barriered stores only
                // (no JS, no GC allocation), per the forEachEntry contract. A racing
                // putEntry can insert an arbitrarily large key mid-window, so skip
                // keys beyond the vector bound instead of writing through them;
                // skipped keys stay in the orphaned map and are recovered by the
                // racer's stillInstalled re-run.
                unsigned vectorLength = storage->vectorLength();
                map->forEachEntry([&](uint64_t key, const SparseArrayEntry& entry) {
                    if (key >= vectorLength) [[unlikely]]
                        return;
                    vector[static_cast<unsigned>(key)].set(vm, this, entry.getNonSparseMode());
                });
                deallocateSparseIndexMap();
                WriteBarrier<Unknown>& valueSlot = vector[i];
                if (!valueSlot)
                    ++storage->m_numValuesInVector;
                valueSlot.set(vm, this, value);
                return true;
            }
        }
        RELEASE_AND_RETURN(scope, putByIndexInline(globalObject, i, value, shouldThrow));
    }
#endif

    // Reread m_storage after increaseVectorLength, update m_numValuesInVector.
    storage = arrayStorage();
    storage->m_numValuesInVector = numValuesInArray;

    // Copy all values from the map into the vector, and delete the map.
    WriteBarrier<Unknown>* vector = storage->m_vector;
    SparseArrayValueMap::const_iterator end = map->end();
    for (SparseArrayValueMap::const_iterator it = map->begin(); it != end; ++it)
        vector[it->key].set(vm, this, it->value.getNonSparseMode());
    deallocateSparseIndexMap();

    // Store the new property into the vector.
    WriteBarrier<Unknown>& valueSlot = vector[i];
    if (!valueSlot)
        ++storage->m_numValuesInVector;
    valueSlot.set(vm, this, value);
    return true;
}

bool JSObject::putByIndexBeyondVectorLength(JSGlobalObject* globalObject, unsigned i, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    RELEASE_ASSERT_WITH_SECURITY_IMPLICATION(!isCopyOnWrite(indexingMode()));

    // i should be a valid array index that is outside of the current vector.
    ASSERT(i <= MAX_ARRAY_INDEX);
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES: {
        if (indexingShouldBeSparse()) {
            auto* arrayStorage = ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm);
            if (!hasSlowPutArrayStorage(indexingType())) [[likely]]
                RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, shouldThrow, arrayStorage));
        } else if (indexIsSufficientlyBeyondLengthForSparseMap(i, 0) || i >= MIN_SPARSE_ARRAY_INDEX) {
            auto* arrayStorage = createArrayStorage(vm, 0, 0);
            if (!hasSlowPutArrayStorage(indexingType())) [[likely]]
                RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, shouldThrow, arrayStorage));
        } else if (needsSlowPutIndexing()) [[unlikely]] {
            // Convert the indexing type to the SlowPutArrayStorage and retry.
            createArrayStorage(vm, i + 1, getNewVectorLength(0, 0, 0, i + 1));
        } else {
#if USE(JSVALUE64)
            // Review round 2 (N3): flag-on, the first dense install must
            // re-dispatch - not trap - when it loses the install race.
            if (Options::useJSThreads()) [[unlikely]] {
                if (tryCreateInitialForValueAndSetConcurrent(vm, i, value))
                    return true;
                RELEASE_AND_RETURN(scope, putByIndex(this, globalObject, i, value, shouldThrow));
            }
#endif
            createInitialForValueAndSet(vm, i, value);
            return true;
        }
        // Fallback with SlowPutArrayStorage.
        RELEASE_AND_RETURN(scope, putByIndex(this, globalObject, i, value, shouldThrow));
    }
        
    case ALL_UNDECIDED_INDEXING_TYPES: {
        CRASH();
        break;
    }
        
    case ALL_INT32_INDEXING_TYPES:
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithoutAttributes<Int32Shape>(globalObject, i, value));
        
    case ALL_DOUBLE_INDEXING_TYPES:
        ASSERT(Options::allowDoubleShape());
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithoutAttributes<DoubleShape>(globalObject, i, value));
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithoutAttributes<ContiguousShape>(globalObject, i, value));
        
    case NonArrayWithSlowPutArrayStorage:
    case ArrayWithSlowPutArrayStorage: {
        // No own property present in the vector, but there might be in the sparse map!
        SparseArrayValueMap* map = arrayStorage()->m_sparseMap.get();
        bool putResult = false;
        if (!(map && map->contains(i))) {
            bool result = attemptToInterceptPutByIndexOnHole(globalObject, i, value, shouldThrow, putResult);
            RETURN_IF_EXCEPTION(scope, false);
            if (result)
                return putResult;
        }
        [[fallthrough]];
    }

    case NonArrayWithArrayStorage:
    case ArrayWithArrayStorage:
        RELEASE_AND_RETURN(scope, putByIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, shouldThrow, arrayStorage()));
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return false;
}

bool JSObject::putDirectIndexBeyondVectorLengthWithArrayStorage(JSGlobalObject* globalObject, unsigned i, JSValue value, unsigned attributes, PutDirectIndexMode mode, ArrayStorage* storage)
{
    VM& vm = globalObject->vm();
    // We're transitioning between states here, if a termination comes in we could leave the object
    // in an inconsistent state. We could still be in the middle a GC during termination so we could
    // try to mark this object and crash. It's much easier to just not think about it.
    DeferTerminationForAWhile noTermination(vm);
    auto scope = DECLARE_THROW_SCOPE(vm);
    
    // i should be a valid array index that is outside of the current vector.
    ASSERT(hasAnyArrayStorage(indexingType()));
    ASSERT(arrayStorage() == storage);
#if USE(JSVALUE64)
    // Flag-on, canSetIndexQuicklyForPutDirect reports AS receivers as
    // "not quickly" (SPEC-objectmodel §Q), so in-vector attribute-0 puts
    // legitimately arrive here; they are handled by the locked arm below.
    ASSERT(i >= storage->vectorLength() || attributes || Options::useJSThreads());
#else
    ASSERT(i >= storage->vectorLength() || attributes);
#endif
    ASSERT(i <= MAX_ARRAY_INDEX);

#if USE(JSVALUE64)
    if (Options::useJSThreads() && !attributes) [[unlikely]] {
        // I31: the flag-on §Q routing forwards in-vector attribute-0 AS
        // stores into this slow path. Handle them here under the cell lock
        // with the same store setIndexQuicklyConcurrent's AS arm uses, so the
        // bound check and the store sit in one locked window: every AS
        // reshape (increaseVectorLength, sparse conversion, AS-COPY) is
        // I31 cell-locked, so vectorLength cannot move between them. This
        // also matches upstream semantics exactly - flag-off, the same put is
        // setIndexQuickly -> setIndexQuicklyForArrayStorageIndexingType.
        Locker locker { cellLock() };
        storage = arrayStorage();
        if (i < storage->vectorLength()) {
            setIndexQuicklyForArrayStorageIndexingType(vm, i, value);
            return true;
        }
        // Still beyond the vector under the lock: fall through to the
        // beyond-vector logic with the freshly re-read storage.
    }
#endif

    SparseArrayValueMap* map = storage->m_sparseMap.get();

    // SPEC-objectmodel I31/L5 (Task 9, flag-on): same locked-window treatment
    // as putByIndexBeyondVectorLengthWithArrayStorage above.
    // First, handle cases where we don't currently have a sparse map.
    if (!map) [[likely]] {
        // If the array is not extensible, we should have entered dictionary mode, and created the spare map.
        ASSERT(isStructureExtensible());

        // Update m_length if necessary.
        if (i >= storage->length()) {
#if USE(JSVALUE64)
            if (Options::useJSThreads()) [[unlikely]] {
                Locker locker { cellLock() };
                storage = arrayStorage();
                if (i >= storage->length())
                    storage->setLength(i + 1);
            } else
#endif
            storage->setLength(i + 1);
        }

        // Check that it is sensible to still be using a vector, and then try to grow the vector.
        if (!attributes
            && (isDenseEnoughForVector(i, storage->m_numValuesInVector))
            && !indexIsSufficientlyBeyondLengthForSparseMap(i, storage->vectorLength()))  [[likely]] {
            if (increaseVectorLength(vm, i + 1)) {
                // success! - reread m_storage since it has likely been reallocated, and store to the vector.
#if USE(JSVALUE64)
                if (Options::useJSThreads()) [[unlikely]] {
                    Locker locker { cellLock() };
                    storage = arrayStorage();
                    WriteBarrier<Unknown>& valueSlot = storage->m_vector[i];
                    if (!valueSlot) // A racing locked writer may have filled it.
                        ++storage->m_numValuesInVector;
                    valueSlot.set(vm, this, value);
                    return true;
                }
#endif
                storage = arrayStorage();
                storage->m_vector[i].set(vm, this, value);
                ++storage->m_numValuesInVector;
                return true;
            }
        }
        // We don't want to, or can't use a vector to hold this property - allocate a sparse map & add the value.
        map = allocateSparseIndexMap(vm);
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            // I21 (review round 1): same map-identity revalidation as
            // putByIndexBeyondVectorLengthWithArrayStorage's putEntry sites.
            bool putResult = map->putDirect(globalObject, this, i, value, attributes, mode);
            RETURN_IF_EXCEPTION(scope, false);
            bool stillInstalled;
            {
                Locker locker { cellLock() };
                ArrayStorage* freshStorage = arrayStorageOrNull();
                stillInstalled = freshStorage && freshStorage->m_sparseMap.get() == map;
            }
            if (stillInstalled) [[likely]]
                return putResult;
            RELEASE_AND_RETURN(scope, putDirectIndex(globalObject, i, value, attributes, mode));
        }
#endif
        RELEASE_AND_RETURN(scope, map->putDirect(globalObject, this, i, value, attributes, mode));
    }

    // Update m_length if necessary.
    unsigned length = storage->length();
    if (i >= length) {
        if (mode != PutDirectIndexLikePutDirect) {
            // Prohibit growing the array if length is not writable.
            if (map->lengthIsReadOnly())
                return typeError(globalObject, scope, mode == PutDirectIndexShouldThrow, ReadonlyPropertyWriteError);
            if (!isStructureExtensible())
                return typeError(globalObject, scope, mode == PutDirectIndexShouldThrow, NonExtensibleObjectPropertyDefineError);
        }
        length = i + 1;
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            Locker locker { cellLock() };
            storage = arrayStorage();
            if (length > storage->length())
                storage->setLength(length);
        } else
#endif
        storage->setLength(length);
    }

    // We are currently using a map - check whether we still want to be doing so.
    // We will continue  to use a sparse map if SparseMode is set, a vector would be too sparse, or if allocation fails.
    unsigned numValuesInArray = storage->m_numValuesInVector + map->size();
    if (map->sparseMode() || attributes || !isDenseEnoughForVector(length, numValuesInArray) || !increaseVectorLength(vm, length)) {
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] {
            // I21 (review round 1): see the fresh-map putDirect site above.
            bool putResult = map->putDirect(globalObject, this, i, value, attributes, mode);
            RETURN_IF_EXCEPTION(scope, false);
            bool stillInstalled;
            {
                Locker locker { cellLock() };
                ArrayStorage* freshStorage = arrayStorageOrNull();
                stillInstalled = freshStorage && freshStorage->m_sparseMap.get() == map;
            }
            if (stillInstalled) [[likely]]
                return putResult;
            RELEASE_AND_RETURN(scope, putDirectIndex(globalObject, i, value, attributes, mode));
        }
#endif
        RELEASE_AND_RETURN(scope, map->putDirect(globalObject, this, i, value, attributes, mode));
    }

#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] {
        // Map -> vector copy + final store, in one locked window (I31).
        {
            Locker locker { cellLock() };
            storage = arrayStorage();
            // I21 (cve-reent-sparse-map-null, producer side): same locked
            // re-validation as putByIndexBeyondVectorLengthWithoutAttributes'
            // consolidation block — the sparseMode/identity reads above were
            // unlocked and the window through increaseVectorLength is
            // unbounded. Never consolidate (and never deallocate) a map that
            // is no longer the installed map or that a committed definer has
            // flipped to sparseMode; bail to the fresh-state re-run instead.
            if (storage->m_sparseMap.get() == map && !map->sparseMode()) [[likely]] {
                storage->m_numValuesInVector = numValuesInArray;
                WriteBarrier<Unknown>* vector = storage->m_vector;
                // AB18-G: locked iteration — an unlocked begin()/end() walk races a
                // putEntry-internal rehash. The functor does barriered stores only
                // (no JS, no GC allocation), per the forEachEntry contract. A racing
                // putEntry can insert an arbitrarily large key mid-window, so skip
                // keys beyond the vector bound instead of writing through them;
                // skipped keys stay in the orphaned map and are recovered by the
                // racer's stillInstalled re-run.
                unsigned vectorLength = storage->vectorLength();
                map->forEachEntry([&](uint64_t key, const SparseArrayEntry& entry) {
                    if (key >= vectorLength) [[unlikely]]
                        return;
                    vector[static_cast<unsigned>(key)].set(vm, this, entry.getNonSparseMode());
                });
                deallocateSparseIndexMap();
                WriteBarrier<Unknown>& valueSlot = vector[i];
                if (!valueSlot)
                    ++storage->m_numValuesInVector;
                valueSlot.set(vm, this, value);
                return true;
            }
        }
        RELEASE_AND_RETURN(scope, putDirectIndex(globalObject, i, value, attributes, mode));
    }
#endif

    // Reread m_storage after increaseVectorLength, update m_numValuesInVector.
    storage = arrayStorage();
    storage->m_numValuesInVector = numValuesInArray;

    // Copy all values from the map into the vector, and delete the map.
    WriteBarrier<Unknown>* vector = storage->m_vector;
    SparseArrayValueMap::const_iterator end = map->end();
    for (SparseArrayValueMap::const_iterator it = map->begin(); it != end; ++it)
        vector[it->key].set(vm, this, it->value.getNonSparseMode());
    deallocateSparseIndexMap();

    // Store the new property into the vector.
    WriteBarrier<Unknown>& valueSlot = vector[i];
    if (!valueSlot)
        ++storage->m_numValuesInVector;
    valueSlot.set(vm, this, value);
    return true;
}

bool JSObject::putDirectIndexSlowOrBeyondVectorLength(JSGlobalObject* globalObject, unsigned i, JSValue value, unsigned attributes, PutDirectIndexMode mode)
{
    VM& vm = globalObject->vm();
    ASSERT(!value.isCustomGetterSetter());

    if (!canDoFastPutDirectIndex(this)) {
        PropertyDescriptor descriptor;
        descriptor.setDescriptor(value, attributes);
        return methodTable()->defineOwnProperty(this, globalObject, Identifier::from(vm, i), descriptor, mode == PutDirectIndexShouldThrow);
    }

    // i should be a valid array index that is outside of the current vector.
    ASSERT(i <= MAX_ARRAY_INDEX);
    
    if (attributes & (PropertyAttribute::ReadOnly | PropertyAttribute::Accessor))
        notifyPresenceOfIndexedAccessors(vm);
    
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES: {
        if (indexingShouldBeSparse() || attributes) {
            return putDirectIndexBeyondVectorLengthWithArrayStorage(
                globalObject, i, value, attributes, mode,
                ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        }
        if (indexIsSufficientlyBeyondLengthForSparseMap(i, 0) || i >= MIN_SPARSE_ARRAY_INDEX) {
            return putDirectIndexBeyondVectorLengthWithArrayStorage(
                globalObject, i, value, attributes, mode, createArrayStorage(vm, 0, 0));
        }
        if (needsSlowPutIndexing()) [[unlikely]] {
            ArrayStorage* storage = createArrayStorage(vm, i + 1, getNewVectorLength(0, 0, 0, i + 1));
            storage->m_vector[i].set(vm, this, value);
            storage->m_numValuesInVector++;
            return true;
        }
        
#if USE(JSVALUE64)
        // Review round 2 (N3): loser re-dispatch instead of trapping.
        if (Options::useJSThreads()) [[unlikely]] {
            if (tryCreateInitialForValueAndSetConcurrent(vm, i, value))
                return true;
            return putDirectIndex(globalObject, i, value, attributes, mode);
        }
#endif
        createInitialForValueAndSet(vm, i, value);
        return true;
    }

    case ALL_UNDECIDED_INDEXING_TYPES: {
        convertUndecidedForValue(vm, value);
        // Reloop.
        return putDirectIndex(globalObject, i, value, attributes, mode);
    }
        
    case ALL_INT32_INDEXING_TYPES: {
        ASSERT(!indexingShouldBeSparse());
        if (attributes)
            return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        if (!value.isInt32()) {
            convertInt32ForValue(vm, value);
            return putDirectIndexSlowOrBeyondVectorLength(globalObject, i, value, attributes, mode);
        }
        putByIndexBeyondVectorLengthWithoutAttributes<Int32Shape>(globalObject, i, value);
        return true;
    }
        
    case ALL_DOUBLE_INDEXING_TYPES: {
        ASSERT(Options::allowDoubleShape());
        ASSERT(!indexingShouldBeSparse());
        if (attributes)
            return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        if (!value.isNumber()) {
            convertDoubleToContiguous(vm);
            return putDirectIndexSlowOrBeyondVectorLength(globalObject, i, value, attributes, mode);
        }
        double valueAsDouble = value.asNumber();
        if (valueAsDouble != valueAsDouble) {
            convertDoubleToContiguous(vm);
            return putDirectIndexSlowOrBeyondVectorLength(globalObject, i, value, attributes, mode);
        }
        putByIndexBeyondVectorLengthWithoutAttributes<DoubleShape>(globalObject, i, value);
        return true;
    }
        
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        ASSERT(!indexingShouldBeSparse());
        if (attributes)
            return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        putByIndexBeyondVectorLengthWithoutAttributes<ContiguousShape>(globalObject, i, value);
        return true;
    }

    case ALL_ARRAY_STORAGE_INDEXING_TYPES:
        if (attributes)
            return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, ensureArrayStorageExistsAndEnterDictionaryIndexingMode(vm));
        return putDirectIndexBeyondVectorLengthWithArrayStorage(globalObject, i, value, attributes, mode, arrayStorage());
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return false;
    }
}

bool JSObject::putDirectNativeIntrinsicGetter(VM& vm, JSGlobalObject* globalObject, Identifier name, NativeFunction nativeFunction, Intrinsic intrinsic, unsigned attributes)
{
    JSFunction* function = JSFunction::create(vm, globalObject, 0, makeString("get "_s, name.string()), nativeFunction, ImplementationVisibility::Public, intrinsic);
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, function, nullptr);
    return putDirectNonIndexAccessor(vm, name, accessor, attributes);
}

void JSObject::putDirectNativeIntrinsicGetterWithoutTransition(VM& vm, JSGlobalObject* globalObject, Identifier name, NativeFunction nativeFunction, Intrinsic intrinsic, unsigned attributes)
{
    JSFunction* function = JSFunction::create(vm, globalObject, 0, makeString("get "_s, name.string()), nativeFunction, ImplementationVisibility::Public, intrinsic);
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, function, nullptr);
    putDirectNonIndexAccessorWithoutTransition(vm, name, accessor, attributes);
}

bool JSObject::putDirectNativeFunction(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, unsigned functionLength, NativeFunction nativeFunction, ImplementationVisibility implementationVisibility, Intrinsic intrinsic, unsigned attributes)
{
    StringImpl* name = propertyName.publicName();
    if (!name)
        name = vm.propertyNames->anonymous.impl();
    ASSERT(name);

    JSFunction* function = JSFunction::create(vm, globalObject, functionLength, name, nativeFunction, implementationVisibility, intrinsic);
    return putDirect(vm, propertyName, function, attributes);
}

bool JSObject::putDirectNativeFunction(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, unsigned functionLength, NativeFunction nativeFunction, ImplementationVisibility implementationVisibility, Intrinsic intrinsic, const DOMJIT::Signature* signature, unsigned attributes)
{
    StringImpl* name = propertyName.publicName();
    if (!name)
        name = vm.propertyNames->anonymous.impl();
    ASSERT(name);

    JSFunction* function = JSFunction::create(vm, globalObject, functionLength, name, nativeFunction, implementationVisibility, intrinsic, callHostFunctionAsConstructor, signature);
    return putDirect(vm, propertyName, function, attributes);
}

void JSObject::putDirectNativeFunctionWithoutTransition(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, unsigned functionLength, NativeFunction nativeFunction, ImplementationVisibility implementationVisibility, Intrinsic intrinsic, unsigned attributes)
{
    StringImpl* name = propertyName.publicName();
    if (!name)
        name = vm.propertyNames->anonymous.impl();
    ASSERT(name);
    JSFunction* function = JSFunction::create(vm, globalObject, functionLength, name, nativeFunction, implementationVisibility, intrinsic);
    putDirectWithoutTransition(vm, propertyName, function, attributes);
}

JSFunction* JSObject::putDirectBuiltinFunction(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, FunctionExecutable* functionExecutable, unsigned attributes)
{
    StringImpl* name = propertyName.publicName();
    if (!name)
        name = vm.propertyNames->anonymous.impl();
    ASSERT(name);
    JSFunction* function = JSFunction::create(vm, globalObject, static_cast<FunctionExecutable*>(functionExecutable), globalObject);
    putDirect(vm, propertyName, function, attributes);
    return function;
}

JSFunction* JSObject::putDirectBuiltinFunctionWithoutTransition(VM& vm, JSGlobalObject* globalObject, const PropertyName& propertyName, FunctionExecutable* functionExecutable, unsigned attributes)
{
    JSFunction* function = JSFunction::create(vm, globalObject, static_cast<FunctionExecutable*>(functionExecutable), globalObject);
    putDirectWithoutTransition(vm, propertyName, function, attributes);
    return function;
}

// NOTE: This method is for ArrayStorage vectors.
ALWAYS_INLINE unsigned JSObject::getNewVectorLength(unsigned indexBias, unsigned currentVectorLength, unsigned currentLength, unsigned desiredLength)
{
    ASSERT(desiredLength <= MAX_STORAGE_VECTOR_LENGTH);

    unsigned increasedLength;
    unsigned maxInitLength = std::min(currentLength, 100000U);

    if (desiredLength < maxInitLength)
        increasedLength = maxInitLength;
    else if (!currentVectorLength)
        increasedLength = std::max(desiredLength, lastArraySize.load(std::memory_order_relaxed));
    else {
        increasedLength = timesThreePlusOneDividedByTwo(desiredLength);
    }

    ASSERT(increasedLength >= desiredLength);

    lastArraySize.store(std::min(increasedLength, FIRST_ARRAY_STORAGE_VECTOR_GROW), std::memory_order_relaxed);

    return ArrayStorage::optimalVectorLength(
        indexBias, structure()->outOfLineCapacity(),
        std::min(increasedLength, MAX_STORAGE_VECTOR_LENGTH));
}

ALWAYS_INLINE unsigned JSObject::getNewVectorLength(unsigned desiredLength)
{
    unsigned indexBias = 0;
    unsigned vectorLength = 0;
    unsigned length = 0;
    
    if (hasIndexedProperties(indexingType())) {
        if (ArrayStorage* storage = arrayStorageOrNull())
            indexBias = storage->m_indexBias;
        vectorLength = this->butterfly()->vectorLength();
        length = this->butterfly()->publicLength();
    }

    return getNewVectorLength(indexBias, vectorLength, length, desiredLength);
}

template<IndexingType indexingShape>
unsigned JSObject::countElements(Butterfly* butterfly)
{
    unsigned numValues = 0;
    for (unsigned i = butterfly->publicLength(); i--;) {
        switch (indexingShape) {
        case Int32Shape:
        case ContiguousShape:
            if (butterfly->contiguous().at(this, i))
                numValues++;
            break;
            
        case DoubleShape: {
            ASSERT(Options::allowDoubleShape());
            double value = butterfly->contiguousDouble().at(this, i);
            if (value == value)
                numValues++;
            break;
        }
            
        default:
            CRASH();
        }
    }
    return numValues;
}

unsigned JSObject::countElements()
{
    switch (indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        return 0;
        
    case ALL_INT32_INDEXING_TYPES:
        return countElements<Int32Shape>(this->butterfly());
        
    case ALL_DOUBLE_INDEXING_TYPES:
        ASSERT(Options::allowDoubleShape());
        return countElements<DoubleShape>(this->butterfly());
        
    case ALL_CONTIGUOUS_INDEXING_TYPES:
        return countElements<ContiguousShape>(this->butterfly());
        
    default:
        CRASH();
        return 0;
    }
}

bool JSObject::increaseVectorLength(VM& vm, unsigned newLength)
{
#if USE(JSVALUE64)
    // SPEC-objectmodel §4.6 (Task 8): flag-on, every ArrayStorage access is
    // cell-locked (I31/L5) and AS innards are never relaid out in place
    // (AS-COPY) - the in-place availableVectorLength/setVectorLength branch
    // below is skipped (vectorLength changes allocate a fresh AS butterfly)
    // and the grow paths publish through casButterfly (T3/I17). The DeferGC
    // precedes the lock per O1's sanctioned back-edge (allocation under the
    // cell lock only under a pre-lock DeferGC/GCDeferralContext).
    std::optional<DeferGC> threadsDeferGC;
    std::optional<Locker<JSCellLock>> threadsLocker;
    if (Options::useJSThreads()) [[unlikely]] {
        threadsDeferGC.emplace(vm);
        threadsLocker.emplace(cellLock());
    }
#endif
    ArrayStorage* storage = arrayStorage();

#if USE(JSVALUE64)
    if (Options::useJSThreads() && newLength <= storage->vectorLength()) [[unlikely]] {
        // A racing locked grower already satisfied this bound (e.g. the
        // I31 in-vector arm in putDirectIndexBeyondVectorLengthWithArrayStorage
        // declined, the lock dropped, and a peer grew the vector past i before
        // we reacquired it here). Without this guard the dense grow path would
        // run with newLength <= vectorLength: debug trips
        // ASSERT(newLength > vectorLength) below, and release could publish a
        // SHRUNK vector when getNewVectorLength(stale newLength) is smaller
        // than the fresh vectorLength, dropping in-vector elements. Both call
        // sites re-read arrayStorage() under the cell lock before storing, so
        // returning success here is safe.
        return true;
    }
    if (Options::useJSThreads()) [[unlikely]] {
        // MC-REENT S3c close-out: refuse to grow a SPARSE-MODE storage's
        // vector. Sparse mode pins vectorLength == 0 (enterDictionary
        // empties the vector), and the AS lookup is "if (i < vectorLength)
        // vector ELSE IF (map)" — a vector grown over sparse entries makes
        // every map entry below the new vectorLength UNREADABLE (and a
        // later in-vector fill SHADOWS its descriptor). Every GIL-on caller
        // checks sparseMode() before calling (the check and the grow are
        // atomic under the GIL); GIL-off the mode can flip between a
        // caller's unlocked decision and this locked body, so the refusal
        // must live here, under the same cellLock the mode-flipping path
        // (enterDictionaryIndexingModeWhenArrayStorageAlreadyExists /
        // defineOwnIndexedProperty's locked add) holds. Callers take their
        // sparse-map leg on false.
        if (SparseArrayValueMap* lockedMap = storage->m_sparseMap.get(); lockedMap && lockedMap->sparseMode())
            return false;
    }
#endif

    unsigned vectorLength = storage->vectorLength();
    unsigned availableVectorLength = storage->availableVectorLength(structure(), vectorLength);
    bool canGrowInPlace = availableVectorLength >= newLength;
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]]
        canGrowInPlace = false; // AS-COPY: in-place vectorLength changes forbidden flag-on.
#endif
    if (canGrowInPlace) {
        // The cell was already big enough for the desired length!
        for (unsigned i = vectorLength; i < availableVectorLength; ++i)
            storage->m_vector[i].clear();
        storage->setVectorLength(availableVectorLength);
        return true;
    }
    
    // This function leaves the array in an internally inconsistent state, because it does not move any values from sparse value map
    // to the vector. Callers have to account for that, because they can do it more efficiently.
    if (newLength > MAX_STORAGE_VECTOR_LENGTH)
        return false;

    if (newLength >= MIN_SPARSE_ARRAY_INDEX
        && !isDenseEnoughForVector(newLength, storage->m_numValuesInVector))
        return false;

    unsigned indexBias = storage->m_indexBias;
    ASSERT(newLength > vectorLength);
    unsigned newVectorLength = getNewVectorLength(newLength);

    // Fast case - there is no precapacity. In these cases a realloc makes sense.
    Structure* structure = this->structure();
    if (!indexBias) [[likely]] {
        DeferGC deferGC(vm);
        Butterfly* newButterfly = storage->butterfly()->growArrayRight(
            vm, this, structure, structure->outOfLineCapacity(), true,
            ArrayStorage::sizeFor(vectorLength), ArrayStorage::sizeFor(newVectorLength));
        if (!newButterfly)
            return false;
        for (unsigned i = vectorLength; i < newVectorLength; ++i)
            newButterfly->arrayStorage()->m_vector[i].clear();
        newButterfly->arrayStorage()->setVectorLength(newVectorLength);
#if USE(JSVALUE64)
        if (Options::useJSThreads()) [[unlikely]] // §4.6 AS-COPY publication form (T3/I17), under the cell lock taken above.
            publishArrayStorageButterflyLocked(vm, static_cast<JSObjectWithButterfly*>(this), newButterfly);
        else
#endif
            setButterfly(vm, newButterfly);
        return true;
    }

    // Remove some, but not all of the precapacity. Atomic decay, & capped to not overflow array length.
    DeferGC deferGC(vm);
    unsigned newIndexBias = std::min(indexBias >> 1, MAX_STORAGE_VECTOR_LENGTH - newVectorLength);
    Butterfly* newButterfly = storage->butterfly()->resizeArray(
        vm, this,
        structure->outOfLineCapacity(), true, ArrayStorage::sizeFor(vectorLength),
        newIndexBias, true, ArrayStorage::sizeFor(newVectorLength));
    if (!newButterfly)
        return false;
    for (unsigned i = vectorLength; i < newVectorLength; ++i)
        newButterfly->arrayStorage()->m_vector[i].clear();
    newButterfly->arrayStorage()->setVectorLength(newVectorLength);
    newButterfly->arrayStorage()->m_indexBias = newIndexBias;
#if USE(JSVALUE64)
    if (Options::useJSThreads()) [[unlikely]] // §4.6 AS-COPY publication form (T3/I17), under the cell lock taken above.
        publishArrayStorageButterflyLocked(vm, static_cast<JSObjectWithButterfly*>(this), newButterfly);
    else
#endif
        setButterfly(vm, newButterfly);
    return true;
}

bool JSObject::ensureLengthSlow(VM& vm, unsigned length)
{
#if USE(JSVALUE64)
    // SPEC-objectmodel §4.4 (Task 8, GT10): flag-on, this resize site runs the
    // full T1/T2 dispatch - T1 owner-only copying resize published by
    // casButterfly ((currentTID, 0) expected exactly - an SW flip mid-resize
    // fails the CAS and re-dispatches to T2, never re-copies, I27/I21), T2
    // segmented growth for foreign/SW=1/segmented words. The former T5
    // in-place vectorLength growth was removed (review round 1): flat
    // vectorLengths are immutable flag-on, so lock-free foreign readers can
    // never pair a raised bound with pre-initialization slot garbage.
    if (Options::useJSThreads()) [[unlikely]]
        return ensureLengthSlowConcurrent(vm, static_cast<JSObjectWithButterfly*>(this), length);
#endif
    if (isCopyOnWrite(indexingMode())) {
        convertFromCopyOnWrite(vm);
        if (this->butterfly()->vectorLength() >= length)
            return true;
    }

    Butterfly* butterfly = this->butterfly();
    
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    ASSERT(hasContiguous(indexingType()) || hasInt32(indexingType()) || hasDouble(indexingType()) || hasUndecided(indexingType()));
    ASSERT(length > butterfly->vectorLength());

    unsigned oldVectorLength = butterfly->vectorLength();
    unsigned newVectorLength;
    
    Structure* structure = this->structure();
    unsigned propertyCapacity = structure->outOfLineCapacity();
    
    GCDeferralContext deferralContext(vm);
    AssertNoGC assertNoGC;
    unsigned availableOldLength =
        Butterfly::availableContiguousVectorLength(propertyCapacity, oldVectorLength);
    Butterfly* newButterfly = nullptr;
    if (availableOldLength >= length) {
        // This is the case where someone else selected a vector length that caused internal
        // fragmentation. If we did our jobs right, this would never happen. But I bet we will mess
        // this up, so this defense should stay.
        newVectorLength = availableOldLength;
    } else {
        newVectorLength = Butterfly::optimalContiguousVectorLength(
            propertyCapacity, std::min<size_t>(nextLength(length), MAX_STORAGE_VECTOR_LENGTH));
        butterfly = butterfly->reallocArrayRightIfPossible(
            vm, deferralContext, this, structure, propertyCapacity, true,
            oldVectorLength * sizeof(EncodedJSValue),
            newVectorLength * sizeof(EncodedJSValue));
        if (!butterfly)
            return false;
        newButterfly = butterfly;
    }

    if (hasDouble(indexingType())) {
        for (unsigned i = oldVectorLength; i < newVectorLength; ++i)
            butterfly->indexingPayload<double>()[i] = PNaN;
    } else {
        for (unsigned i = oldVectorLength; i < newVectorLength; ++i)
            butterfly->indexingPayload<WriteBarrier<Unknown>>()[i].clear();
    }

    if (newButterfly) {
        butterfly->setVectorLength(newVectorLength);
        WTF::storeStoreFence();
        // SPEC-objectmodel Task 8: flag-on, ensureLengthSlow returns early into
        // ensureLengthSlowConcurrent above (T1 casButterfly / T2), so this
        // is the flag-off path only (I22: identical to today).
        butterflyRef().set(vm, this, newButterfly);
    } else {
        // Flag-on, in-place vectorLength-only growth does not exist (the T5
        // branch was removed in review round 1); ensureLengthSlowConcurrent
        // takes the T1 fresh-copy route instead. Flag-off only here.
        WTF::storeStoreFence();
        butterfly->setVectorLength(newVectorLength);
    }

    return true;
}

void JSObject::reallocateAndShrinkButterfly(VM& vm, unsigned length)
{
#if USE(JSVALUE64)
    // SPEC-objectmodel §4.4 (Task 8, GT10): flag-on dispatch - owner flat
    // copy-shrink published by casButterfly (I17/I27), SW=1 flat / segmented
    // in-place truncation (publicLength only), foreign SW=0 through F1 first.
    if (Options::useJSThreads()) [[unlikely]] {
        shrinkButterflyForSetLengthConcurrent(vm, static_cast<JSObjectWithButterfly*>(this), length);
        return;
    }
#endif
    ASSERT(length <= MAX_STORAGE_VECTOR_LENGTH);
    ASSERT(hasContiguous(indexingType()) || hasInt32(indexingType()) || hasDouble(indexingType()) || hasUndecided(indexingType()));
    ASSERT(this->butterfly()->vectorLength() > length);
    ASSERT(this->butterfly()->publicLength() >= length);
    ASSERT(!this->butterfly()->indexingHeader()->preCapacity(structure()));

    DeferGC deferGC(vm);
    Butterfly* newButterfly = this->butterfly()->resizeArray(vm, this, structure(), 0, ArrayStorage::sizeFor(length));
    newButterfly->setVectorLength(length);
    newButterfly->setPublicLength(length);
    WTF::storeStoreFence();
    // Flag-on, this site returned early into shrinkButterflyForSetLengthConcurrent
    // (Task 8: casButterfly form, I17); this is the flag-off path only (I22).
    butterflyRef().set(vm, this, newButterfly);
}

Butterfly* JSObject::allocateMoreOutOfLineStorage(VM& vm, size_t oldSize, size_t newSize)
{
    ASSERT(newSize > oldSize);

    // It's important that this function not rely on structure(), for the property
    // capacity, since we might have already mutated the structure in-place.

#if USE(JSVALUE64)
    // Branchless flat load: flag-off every tag bit is zero (I22) so the mask
    // is the identity and this compiles to the pre-threads single load + AND;
    // flag-on every caller (E4 sites, growOutOfLineStorageForConcurrentLockedAdd,
    // flag-off-only legacy legs) has already established flatness, which the
    // assert witnesses. This avoids the option-load + branch inside
    // JSObject::butterfly() on the hottest reallocation path.
    // The RELEASE_ASSERT is the storage-growth path's segmented-word witness
    // (word is already in a register: test + never-taken branch, no memory
    // traffic; flag-off it can never fire because all tag bits are zero).
    uint64_t word = taggedButterflyWord();
    RELEASE_ASSERT(!isSegmentedButterfly(word));
    Butterfly* oldButterfly = untaggedButterfly(word);
#else
    Butterfly* oldButterfly = this->butterfly();
#endif
    return Butterfly::createOrGrowPropertyStorage(oldButterfly, vm, this, structure(), oldSize, newSize);
}

bool JSObject::getOwnPropertyDescriptor(JSGlobalObject* globalObject, PropertyName propertyName, PropertyDescriptor& descriptor)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PropertySlot slot(this, PropertySlot::InternalMethodType::GetOwnProperty);

    bool result = methodTable()->getOwnPropertySlot(this, globalObject, propertyName, slot);
    EXCEPTION_ASSERT_UNUSED(scope, !scope.exception() || !result);
    if (!result)
        return false;

    RELEASE_AND_RETURN(scope, descriptor.setPropertySlot(globalObject, propertyName, slot));
}

bool JSObject::putDirectMayBeIndex(JSGlobalObject* globalObject, PropertyName propertyName, JSValue value)
{
    if (std::optional<uint32_t> index = parseIndex(propertyName))
        return putDirectIndex(globalObject, index.value(), value);
    return putDirect(globalObject->vm(), propertyName, value);
}

// https://tc39.es/ecma262/#sec-validateandapplypropertydescriptor
bool validateAndApplyPropertyDescriptor(JSGlobalObject* globalObject, JSObject* object, PropertyName propertyName, bool isExtensible,
    const PropertyDescriptor& descriptor, bool isCurrentDefined, const PropertyDescriptor& current, bool throwException)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // If we have a new property we can just put it on normally
    // Step 2.
    if (!isCurrentDefined) {
        // unless extensions are prevented!
        // Step 2.a
        if (!isExtensible)
            return typeError(globalObject, scope, throwException, NonExtensibleObjectPropertyDefineError);

        if (object) {
            if (descriptor.isAccessorDescriptor()) {
                unsigned attributes = (descriptor.attributes() | PropertyAttribute::Accessor) & ~PropertyAttribute::ReadOnly;
                object->putDirectAccessor(globalObject, propertyName, descriptor.slowGetterSetter(globalObject), attributes);
            } else {
                ASSERT(descriptor.isGenericDescriptor() || descriptor.isDataDescriptor());
                JSValue value = descriptor.value() ? descriptor.value() : jsUndefined();
                object->putDirect(vm, propertyName, value, descriptor.attributes() & ~PropertyAttribute::Accessor);
            }
        }

        return true;
    }
    // Step 3.
    if (descriptor.isEmpty())
        return true;

    bool isEqual = current.equalTo(globalObject, descriptor);
    RETURN_IF_EXCEPTION(scope, false);
    if (isEqual)
        return true;

    // Step 4.
    if (!current.configurable()) {
        if (descriptor.configurable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeConfigurabilityError);
        if (descriptor.enumerablePresent() && descriptor.enumerable() != current.enumerable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeEnumerabilityError);
    }

    if (descriptor.isGenericDescriptor()) {
        // Step 5.
        // Changing [[Enumerable]] and [[Configurable]] attributes of an existing property
    } else if (current.isDataDescriptor() != descriptor.isDataDescriptor()) {
        // Step 6.
        // Changing between a data property and accessor property
        if (!current.configurable())
            return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeAccessMechanismError);
    } else if (current.isDataDescriptor() && descriptor.isDataDescriptor()) {
        // Step 7.
        // Changing the value and attributes of an existing data property
        if (!current.configurable() && !current.writable()) {
            if (descriptor.writable())
                return typeError(globalObject, scope, throwException, UnconfigurablePropertyChangeWritabilityError);
            if (descriptor.value()) {
                bool isSame = sameValue(globalObject, descriptor.value(), current.value());
                RETURN_IF_EXCEPTION(scope, false);
                if (!isSame)
                    return typeError(globalObject, scope, throwException, ReadonlyPropertyChangeError);
            }

            return true;
        }
    } else {
        // Step 8.
        // Changing the accessor functions and attributes of an existing accessor property
        ASSERT(descriptor.isAccessorDescriptor());
        if (!current.configurable()) {
            if (descriptor.setterPresent() && descriptor.setter() != current.setter())
                return typeError(globalObject, scope, throwException, "Attempting to change the setter of an unconfigurable property."_s);
            if (descriptor.getterPresent() && descriptor.getter() != current.getter())
                return typeError(globalObject, scope, throwException, "Attempting to change the getter of an unconfigurable property."_s);

            return true;
        }
    }

    if (!object)
        return true;
    // Step 9.
    unsigned attributes = descriptor.attributesOverridingCurrent(current);
    if (descriptor.isAccessorDescriptor() || (current.isAccessorDescriptor() && !descriptor.isDataDescriptor())) {
        ASSERT(attributes & PropertyAttribute::Accessor);
        JSObject* getter = descriptor.getterPresent() ? descriptor.getterObject() : (current.getterPresent() ? current.getterObject() : nullptr);
        JSObject* setter = descriptor.setterPresent() ? descriptor.setterObject() : (current.setterPresent() ? current.setterObject() : nullptr);
        GetterSetter* getterSetter = GetterSetter::create(vm, globalObject, getter, setter);
        object->putDirectAccessor(globalObject, propertyName, getterSetter, attributes & ~PropertyAttribute::ReadOnly);
    } else {
        ASSERT(descriptor.isGenericDescriptor() || descriptor.isDataDescriptor());
        JSValue value = descriptor.value() ? descriptor.value() : (current.value() ? current.value() : jsUndefined());
        object->putDirect(vm, propertyName, value, attributes & ~PropertyAttribute::Accessor);
    }

    return true;
}

bool JSObject::defineOwnNonIndexProperty(JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool throwException)
{
    VM& vm  = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    PropertyDescriptor current;
    bool isCurrentDefined = getOwnPropertyDescriptor(globalObject, propertyName, current);
    RETURN_IF_EXCEPTION(throwScope, false);
    bool isExtensible = this->isExtensible(globalObject);
    RETURN_IF_EXCEPTION(throwScope, false);
    RELEASE_AND_RETURN(throwScope, validateAndApplyPropertyDescriptor(globalObject, this, propertyName, isExtensible, descriptor, isCurrentDefined, current, throwException));
}

bool JSObject::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool throwException)
{
    if (Options::useJSThreads() && object->structure()->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]]
        return false;
    // If it's an array index, then use the indexed property storage.
    if (std::optional<uint32_t> index = parseIndex(propertyName)) {
        // c. Let succeeded be the result of calling the default [[DefineOwnProperty]] internal method (8.12.9) on A passing P, Desc, and false as arguments.
        // d. Reject if succeeded is false.
        // e. If index >= oldLen
        // e.i. Set oldLenDesc.[[Value]] to index + 1.
        // e.ii. Call the default [[DefineOwnProperty]] internal method (8.12.9) on A passing "length", oldLenDesc, and false as arguments. This call will always return true.
        // f. Return true.
        return object->defineOwnIndexedProperty(globalObject, index.value(), descriptor, throwException);
    }
    
    return object->defineOwnNonIndexProperty(globalObject, propertyName, descriptor, throwException);
}

void JSObject::convertToDictionary(VM& vm)
{
    Structure* oldStructure = structure();
    DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, oldStructure);
    setStructure(vm, Structure::toCacheableDictionaryTransition(vm, oldStructure, &deferredWatchpointFire));
}

void JSObject::convertToUncacheableDictionary(VM& vm)
{
    Structure* oldStructure = structure();
    if (oldStructure->isUncacheableDictionary())
        return;
    DeferredStructureTransitionWatchpointFire deferredWatchpointFire(vm, oldStructure);
    setStructure(vm, Structure::toUncacheableDictionaryTransition(vm, oldStructure, &deferredWatchpointFire));
    if (mayBePrototype()) [[unlikely]]
        vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Change);
}


void JSObject::shiftButterflyAfterFlattening(const GCSafeConcurrentJSLocker&, VM& vm, Structure* structure, size_t outOfLineCapacityAfter)
{
    // This could interleave visitChildren because some old structure could have been a non
    // dictionary structure. We have to be crazy careful. But, we are guaranteed to be holding
    // the structure's lock right now, and that helps a bit.

    Butterfly* oldButterfly = this->butterfly();
    size_t preCapacity;
    size_t indexingPayloadSizeInBytes;
    bool hasIndexingHeader = this->hasIndexingHeader();
    if (hasIndexingHeader) [[unlikely]] {
        preCapacity = oldButterfly->indexingHeader()->preCapacity(structure);
        indexingPayloadSizeInBytes = oldButterfly->indexingHeader()->indexingPayloadSizeInBytes(structure);
    } else {
        preCapacity = 0;
        indexingPayloadSizeInBytes = 0;
    }

    Butterfly* newButterfly = Butterfly::createUninitialized(vm, this, preCapacity, outOfLineCapacityAfter, hasIndexingHeader, indexingPayloadSizeInBytes);

    // No need to copy the precapacity.
    void* currentBase = oldButterfly->base(0, outOfLineCapacityAfter);
    void* newBase = newButterfly->base(0, outOfLineCapacityAfter);

    // memcpy is fine since newButterfly is not tied to any object yet.
    memcpy(static_cast<JSValue*>(newBase), static_cast<JSValue*>(currentBase), Butterfly::totalSize(0, outOfLineCapacityAfter, hasIndexingHeader, indexingPayloadSizeInBytes));
    
    setButterfly(vm, newButterfly);
}

uint32_t JSObject::getEnumerableLength()
{
    JSObject* object = this;

    switch (object->indexingType()) {
    case ALL_BLANK_INDEXING_TYPES:
    case ALL_UNDECIDED_INDEXING_TYPES:
        // Regardless of holesMustForwardToPrototype condition, it returns zero.
        return 0;
        
    case ALL_INT32_INDEXING_TYPES:
    case ALL_CONTIGUOUS_INDEXING_TYPES: {
        Butterfly* butterfly = object->butterfly();
        unsigned enumerableLength = butterfly->publicLength();
        if (!enumerableLength)
            return 0;
        if (object->structure()->holesMustForwardToPrototype(object))
            return 0;
        for (unsigned i = 0; i < enumerableLength; ++i) {
            if (!butterfly->contiguous().at(object, i))
                return 0;
        }
        return enumerableLength;
    }
        
    case ALL_DOUBLE_INDEXING_TYPES: {
        Butterfly* butterfly = object->butterfly();
        unsigned enumerableLength = butterfly->publicLength();
        if (!enumerableLength)
            return 0;
        if (object->structure()->holesMustForwardToPrototype(object))
            return 0;
        for (unsigned i = 0; i < enumerableLength; ++i) {
            double value = butterfly->contiguousDouble().at(object, i);
            if (value != value)
                return 0;
        }
        return enumerableLength;
    }
        
    case ALL_ARRAY_STORAGE_INDEXING_TYPES: {
        ArrayStorage* storage = object->butterfly()->arrayStorage();
        if (storage->m_sparseMap.get())
            return 0;
        
        unsigned enumerableLength = std::min(storage->length(), storage->vectorLength());
        if (!enumerableLength)
            return 0;
        if (object->structure()->holesMustForwardToPrototype(object))
            return 0;
        for (unsigned i = 0; i < enumerableLength; ++i) {
            if (!storage->m_vector[i])
                return 0;
        }
        return enumerableLength;
    }
        
    default:
        RELEASE_ASSERT_NOT_REACHED();
        return 0;
    }
}

// Implements GetMethod(O, P) in section 7.3.9 of the spec.
// http://www.ecma-international.org/ecma-262/6.0/index.html#sec-getmethod
JSValue JSObject::getMethod(JSGlobalObject* globalObject, CallData& callData, const Identifier& ident, const String& errorMessage)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue method = get(globalObject, ident);
    RETURN_IF_EXCEPTION(scope, JSValue());

    if (!method.isCell()) {
        if (method.isUndefinedOrNull())
            return jsUndefined();

        throwVMTypeError(globalObject, scope, errorMessage);
        return jsUndefined();
    }

    callData = JSC::getCallDataInline(method.asCell());
    if (callData.type == CallData::Type::None) {
        throwVMTypeError(globalObject, scope, errorMessage);
        return jsUndefined();
    }

    return method;
}

bool JSObject::anyObjectInChainMayInterceptIndexedAccesses() const
{
    for (const JSObject* current = this; ;) {
        if (current->structure()->mayInterceptIndexedAccesses())
            return true;
        
        JSValue prototype = current->getPrototypeDirect();
        if (prototype.isNull())
            return false;
        
        current = asObject(prototype);
    }
}

bool JSObject::needsSlowPutIndexing() const
{
    if (anyObjectInChainMayInterceptIndexedAccesses())
        return true;
    auto* globalObject = realmMayBeNull();
    return globalObject && globalObject->isHavingABadTime();
}

TransitionKind JSObject::suggestedArrayStorageTransition() const
{
    if (needsSlowPutIndexing())
        return TransitionKind::AllocateSlowPutArrayStorage;
    
    return TransitionKind::AllocateArrayStorage;
}

void JSObject::putOwnDataPropertyBatching(VM& vm, UniquedStringImpl** properties, const EncodedJSValue* values, unsigned size)
{
    unsigned i = 0;
    Structure* structure = this->structure();
#if USE(JSVALUE64)
    // SPEC-objectmodel §9.5/§6 hardening: the batched fast path below walks
    // the transition chain and republishes storage with the legacy
    // unconditional sequence (raw flat-only butterfly() deref +
    // allocateMoreOutOfLineStorage + nukeStructureAndSetButterfly +
    // setStructure), with no §2 regime dispatch and no E4 gate. Its entry
    // points (Object.assign / copyDataProperties / object clone fast paths)
    // can run against an object another thread has promoted to the segmented
    // regime, where the flat-only butterfly() CONTRACT assertion trips (and a
    // release build would deref a ButterflySpine as a flat Butterfly).
    // Flag-on, take the routed per-property path: putOwnDataProperty funnels
    // into putDirectInternal's §6 cell-locked / E4-gated add protocols.
    // Flag-off this branch is dead (I22).
    //
    // FIXME(threads): this routing closes a latent batched-add hole but is
    // NOT the fix for the i03-i37-same-shape-add-storm assertion
    // (fix-butterfly-regime-assert): that abort is reported in
    // JSObjectWithButterfly::butterfly() (JSObject.h, derived-typed
    // receiver), which this function cannot bind, and the test never reaches
    // these batching entry points. Re-triage IDENTIFIED the caller: the
    // op_spread slow path reaches JSCellButterfly::createFromArray
    // (runtime/JSCellButterfly.h), whose Contiguous/Int32/Double fast loops
    // deref array->butterfly() with no mayBeSegmentedButterfly() guard
    // (guard idiom as in ArrayPrototype.cpp's spread fast path); a
    // same-class flat-only deref also exists in dfg/DFGOperations.cpp's
    // pop-recover path. Both files are outside this item's file scope, so
    // the guard must land there after re-scope.
    if (Options::useJSThreads()) [[unlikely]] {
        for (; i < size; ++i) {
            PutPropertySlot putPropertySlot(this, true);
            putOwnDataProperty(vm, properties[i], JSValue::decode(values[i]), putPropertySlot);
        }
        return;
    }
#endif
    if (!(structure->isDictionary() || (structure->transitionCountEstimate() + size) > Structure::s_maxTransitionLength || !structure->canPerformFastPropertyEnumerationCommon())) {
        Vector<PropertyOffset, 16> offsets(size, [&](size_t index) -> std::optional<PropertyOffset> {
            PropertyName propertyName(properties[index]);

            PropertyOffset offset;
            if (Structure* newStructure = Structure::addPropertyTransitionToExistingStructure(structure, propertyName, 0, offset)) {
                structure = newStructure;
                return offset;
            }

            unsigned currentAttributes;
            offset = structure->get(vm, propertyName, currentAttributes);
            if (offset != invalidOffset) {
                structure->didReplaceProperty(offset);
                return offset;
            }

            // If we detect that this structure requires transition watchpoint firing, then we need to stop this batching and rest of the values
            // should be put via generic way.
            if (structure->transitionWatchpointSet().isBeingWatched() && structure->transitionWatchpointSet().isStillValid()) [[unlikely]]
                return std::nullopt;

            // It will go to the cacheable dictionary case. We stop the batching here and fall though to the generic case.
            // We break here before adding offset to offsets since this property itself should be put via generic path.
            if (structure->shouldDoCacheableDictionaryTransitionForAdd(PutPropertySlot::UnknownContext)) [[unlikely]]
                return std::nullopt;

            Structure* newStructure = Structure::addNewPropertyTransition(vm, structure, propertyName, 0, offset, PutPropertySlot::UnknownContext, nullptr);

            validateOffset(offset);
            ASSERT(newStructure->isValidOffset(offset));

            structure = newStructure;
            return offset;
        }, NulloptBehavior::Abort);

        // Flush batching here. Note that it is possible that offsets.size() is not equal to size, if we stop batching due to transition-watchpoint-firing.

        Butterfly* newButterfly = this->butterfly();
        auto* oldStructure = this->structure();
        if (oldStructure->outOfLineCapacity() != structure->outOfLineCapacity()) {
            ASSERT(structure != oldStructure);
            newButterfly = allocateMoreOutOfLineStorage(vm, oldStructure->outOfLineCapacity(), structure->outOfLineCapacity());
            nukeStructureAndSetButterfly(vm, StructureID::encode(oldStructure), newButterfly);
        }

        for (unsigned index = 0; index < offsets.size(); ++index)
            putDirectOffset(vm, offsets[index], JSValue::decode(values[index]));
        setStructure(vm, structure);

        // We fall through to the generic case and consume the rest of put operations if batching stopped in the middle.
        i = offsets.size();

        if (mayBePrototype())
            vm.invalidateStructureChainIntegrity(VM::StructureChainIntegrityEvent::Add);
    }

    for (; i < size; ++i) {
        PutPropertySlot putPropertySlot(this, true);
        putOwnDataProperty(vm, properties[i], JSValue::decode(values[i]), putPropertySlot);
    }
}

ASCIILiteral JSObject::putDirectToDictionaryWithoutExtensibility(VM& vm, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    // Flag-off the loop body runs exactly once (nothing RESTARTs - I22).
    while (true) {
        unsigned currentAttributes;
        Structure* structure = this->structure();
        PropertyOffset offset = structure->get(vm, propertyName, currentAttributes);
        if (offset != invalidOffset) {
            if (currentAttributes & PropertyAttribute::ReadOnlyOrAccessorOrCustomAccessor)
                return ReadonlyPropertyChangeError;

#if USE(JSVALUE64)
            if (Options::useJSThreads()) [[unlikely]] {
                // §6 L3 (review round 1): dictionary-mode value replaces are
                // cell-locked so they cannot race a flatten's in-place
                // renumber (which holds the cell lock; an unlocked store
                // mid-renumber can land in a re-purposed slot). Re-validate
                // the structure under the lock; on mismatch redo the lookup.
                bool stored = false;
                {
                    Locker locker { cellLock() };
                    if (this->structure() == structure) {
                        putDirectOffset(vm, offset, value);
                        stored = true;
                    }
                }
                if (!stored)
                    continue; // The offset may have been renumbered; re-derive it.
            } else
#endif
            putDirectOffset(vm, offset, value);
            structure->didReplaceProperty(offset);

            // FIXME: Check attributes against PropertyAttribute::CustomAccessorOrValue. Changing GetterSetter should work w/o transition.
            // https://bugs.webkit.org/show_bug.cgi?id=214342
            ASSERT(!(currentAttributes & PropertyAttribute::AccessorOrCustomAccessorOrValue));
            slot.setExistingProperty(this, offset);
            return { };
        }

        return NonExtensibleObjectPropertyDefineError;
    }
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

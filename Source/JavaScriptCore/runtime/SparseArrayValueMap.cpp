/*
 * Copyright (C) 2011-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "SparseArrayValueMap.h"

#include "GetterSetter.h"
#include "JSCJSValueInlines.h"
#include "JSObjectInlines.h"
#include "PropertySlot.h"
#include "StructureCreateInlines.h"
#include "TypeError.h"
#include <wtf/Atomics.h>

namespace JSC {

const ClassInfo SparseArrayValueMap::s_info = { "SparseArrayValueMap"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(SparseArrayValueMap) };

// Out-of-line so the header never references JSCellLock's inline
// lock()/unlock() (defined in JSCellInlines.h, which headers must not
// include — see the declaration comment; -Wundefined-inline is an error on
// the Windows builds).
void SparseArrayValueMap::acquireCellLock() const
{
    const_cast<SparseArrayValueMap*>(this)->cellLock().lock();
}

void SparseArrayValueMap::releaseCellLock() const
{
    const_cast<SparseArrayValueMap*>(this)->cellLock().unlock();
}

SparseArrayValueMap::SparseArrayValueMap(VM& vm)
    : Base(vm, vm.sparseArrayValueMapStructure.get())
{
    // TSAN wave 5 (triage family 10 jsvalue-slots): ctor publication — a
    // GIL-off reader with a stale ref to this recycled cell can hit
    // flagsRelaxed() while these words are initialized; relaxed atomic
    // stores instead of plain NSDMI stores (see the member declarations).
    // Codegen-identical flag-off.
    WTF::atomicStore(&m_flags, static_cast<unsigned>(Normal), std::memory_order_relaxed);
    WTF::atomicStore(&m_reportedCapacity, static_cast<size_t>(0), std::memory_order_relaxed);
    // r19 (post-closeout review): publish the m_map header NSDMI stores to
    // TSAN — pairs with tsanAcquireCtorPublication() at the cellLock()
    // sites (the lock alone gives no edge back to this thread). No-op
    // outside TSAN; see the helper's comment in the header.
    TSAN_ANNOTATE_HAPPENS_BEFORE(this);
}

SparseArrayValueMap* SparseArrayValueMap::create(VM& vm)
{
    SparseArrayValueMap* result = new (NotNull, allocateCell<SparseArrayValueMap>(vm)) SparseArrayValueMap(vm);
    result->finishCreation(vm);
    return result;
}

void SparseArrayValueMap::destroy(JSCell* cell)
{
    static_cast<SparseArrayValueMap*>(cell)->SparseArrayValueMap::~SparseArrayValueMap();
}

Structure* SparseArrayValueMap::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

SparseArrayValueMap::AddResult SparseArrayValueMap::add(JSObject* array, unsigned i)
{
    AddResult result;
    size_t increasedCapacity = 0;
    {
        Locker locker { cellLock() };
        tsanAcquireCtorPublication();
        result = m_map.add(i, SparseArrayEntry());
        size_t capacity = m_map.capacity();
        if (capacity > m_reportedCapacity) {
            increasedCapacity = capacity - m_reportedCapacity;
            m_reportedCapacity = capacity;
        }
    }
    if (increasedCapacity)
        Heap::heap(array)->reportExtraMemoryAllocated(array, increasedCapacity * sizeof(Map::KeyValuePairType));
    return result;
}

void SparseArrayValueMap::remove(iterator it)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    m_map.remove(it);
}

void SparseArrayValueMap::remove(unsigned i)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    m_map.remove(i);
}

bool SparseArrayValueMap::putEntry(JSGlobalObject* globalObject, JSObject* array, unsigned i, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(value);
    
    AddResult result = add(array, i);

    // To save a separate find & add, we first always add to the sparse map.
    // In the uncommon case that this is a new property, and the array is not
    // extensible, this is not the right thing to have done - so remove again.
    if (result.isNewEntry && !array->isStructureExtensible()) {
        if (Options::useJSThreads()) [[unlikely]] {
            // AB18-G: result.iterator was minted inside add()'s critical
            // section; a racing add() can rehash m_map after add() drops the
            // cell lock, leaving it pointing into a freed table. Remove by
            // key, which re-probes under the lock.
            remove(i);
        } else
            remove(result.iterator);
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    }

    if (Options::useJSThreads()) [[unlikely]] {
        // objectmodel round 4 (§61): `entry` dangles if a racing add()
        // rehashes m_map after add()'s internal lock released. Re-find under
        // the map's cell lock; do the plain-data store under it (no JS, no GC
        // allocation); extract the GetterSetter under it and call the setter
        // OUTSIDE it (it runs JS).
        JSValue getterSetter;
        {
            Locker locker { cellLock() };
            tsanAcquireCtorPublication();
            auto it = m_map.find(i);
            if (it == m_map.end())
                return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError); // racing remove
            SparseArrayEntry& lockedEntry = it->value;
            if (!(lockedEntry.attributes() & PropertyAttribute::Accessor)) {
                if (lockedEntry.attributes() & PropertyAttribute::ReadOnly)
                    return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
                // Plain-data store under the lock (no JS, no GC allocation);
                // forceSet with the unchanged attribute word is the
                // access-control-clean spelling of Base::set (the §61 note's
                // sanctioned alternative — WriteBarrier's set is a private base).
                lockedEntry.forceSet(vm, this, value, lockedEntry.attributes());
                return true;
            }
            getterSetter = lockedEntry.get();
        }
        RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(getterSetter)->callSetter(globalObject, array, value, shouldThrow));
    }
    // AB18-G: GIL-on only — the iterator deref is hoisted below the mode
    // split so no stale-able iterator use precedes it; safe with a single
    // mutator.
    SparseArrayEntry& entry = result.iterator->value;
    RELEASE_AND_RETURN(scope, entry.put(globalObject, array, this, value, shouldThrow));
}

bool SparseArrayValueMap::putDirect(JSGlobalObject* globalObject, JSObject* array, unsigned i, JSValue value, unsigned attributes, PutDirectIndexMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(value);
    
    bool shouldThrow = (mode == PutDirectIndexShouldThrow);

    AddResult result = add(array, i);

    // To save a separate find & add, we first always add to the sparse map.
    // In the uncommon case that this is a new property, and the array is not
    // extensible, this is not the right thing to have done - so remove again.
    if (mode != PutDirectIndexLikePutDirect && result.isNewEntry && !array->isStructureExtensible()) {
        if (Options::useJSThreads()) [[unlikely]] {
            // AB18-G: see putEntry — result.iterator may dangle after a
            // racing rehash; remove by key under the lock.
            remove(i);
        } else
            remove(result.iterator);
        return typeError(globalObject, scope, shouldThrow, NonExtensibleObjectPropertyDefineError);
    }

    if (Options::useJSThreads()) [[unlikely]] {
        Locker locker { cellLock() }; // objectmodel round 4 (§61): re-find; `entry` may dangle (racing rehash).
        tsanAcquireCtorPublication();
        auto it = m_map.find(i);
        if (it == m_map.end())
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError); // racing remove
        SparseArrayEntry& lockedEntry = it->value;
        if (lockedEntry.attributes() & PropertyAttribute::ReadOnly)
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
        lockedEntry.forceSet(vm, this, value, attributes); // no JS, no GC allocation - lockable
        return true;
    }
    // AB18-G: GIL-on only — iterator deref hoisted below the mode split (see putEntry).
    SparseArrayEntry& entry = result.iterator->value;
    if (entry.attributes() & PropertyAttribute::ReadOnly)
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

    entry.forceSet(vm, this, value, attributes);
    return true;
}

JSValue SparseArrayValueMap::getConcurrently(unsigned i)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    auto iterator = m_map.find(i);
    if (iterator == m_map.end())
        return JSValue();
    return iterator->value.getConcurrently();
}

std::optional<SparseArrayEntry> SparseArrayValueMap::getEntry(unsigned i)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    auto it = m_map.find(i);
    if (it == m_map.end())
        return std::nullopt;
    // TSAN wave 5 (triage family 10 jsvalue-slots, r4 key 'data race x
    // SparseArrayValueMap::getEntry'): do NOT memcpy the shared entry into
    // the optional — unlocked relaxed-atomic writers (putIndexedDescriptor's
    // forceSet under defineOwnIndexedProperty) race a plain copy. Snapshot
    // field-wise through the relaxed accessors; the lock still serializes
    // against rehash (entry address stability), the entry words themselves
    // carry the OM §1 racy-value tolerance.
    return it->value.copySnapshotConcurrent();
}

void SparseArrayEntry::get(JSObject* thisObject, PropertySlot& slot) const
{
    JSValue value = Base::get();
    ASSERT(value);

    // TSAN wave 3 (triage §3.10 jsvalue-slots): sparse-map entry value/attribute
    // pairs are intentionally racy under shared-heap threading; the locked
    // writer (forceSet) pairs with relaxed-atomic reads here — codegen-identical
    // flag-off. Read the attribute word once so value/attributes stay a
    // self-consistent-enough pair (staleness is blessed; tearing is not).
    unsigned attributes = WTF::atomicLoad(const_cast<unsigned*>(&m_attributes), std::memory_order_relaxed);

    if (!value.isGetterSetter()) [[likely]] {
        slot.setValue(thisObject, attributes, value);
        return;
    }

    slot.setGetterSlot(thisObject, attributes, uncheckedDowncast<GetterSetter>(value));
}

void SparseArrayEntry::get(PropertyDescriptor& descriptor) const
{
    // TSAN wave 3 (triage §3.10): relaxed-atomic read of the racy attribute word.
    descriptor.setDescriptor(Base::get(), WTF::atomicLoad(const_cast<unsigned*>(&m_attributes), std::memory_order_relaxed));
}

JSValue SparseArrayEntry::getConcurrently() const
{
    // These attributes and value can be updated while executing getConcurrently.
    // But this is OK since attributes should be never weaken once it gets DontDelete and ReadOnly.
    // By emitting store-store-fence and load-load-fence between value setting and attributes setting,
    // we can ensure that the value is what we want once the attributes get ReadOnly & DontDelete:
    // once attributes get this state, the value should not be changed.
    // TSAN wave 3 (triage §3.10): Dependency::loadAndFence performs a plain
    // (non-atomic) load, which is UB against the locked plain store in
    // forceSet. Do the load as a relaxed atomic (codegen-identical: one word
    // load) and build the dependency from the loaded value with
    // Dependency::fence — same consume chain on ARM, and the atomic load
    // already defeats the cross-load CSE that loadAndFence's opaque() guards
    // against.
    unsigned attributes = WTF::atomicLoad(const_cast<unsigned*>(&m_attributes), std::memory_order_relaxed);
    Dependency attributesDependency = Dependency::fence(attributes);
    if (attributes & PropertyAttribute::Accessor)
        return JSValue();

    if (!(attributes & PropertyAttribute::ReadOnly))
        return JSValue();

    if (!(attributes & PropertyAttribute::DontDelete))
        return JSValue();

    return attributesDependency.consume(this)->Base::get();
}

bool SparseArrayEntry::put(JSGlobalObject* globalObject, JSValue thisValue, SparseArrayValueMap* map, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // TSAN wave 4 (triage §3.10): read the attribute word once, relaxed, via
    // attributes(). This path is GIL-on only (flag-on putEntry takes its
    // locked branch instead), but the accessor keeps every read of the shared
    // word atomic and codegen-identical.
    unsigned attributes = this->attributes();
    if (!(attributes & PropertyAttribute::Accessor)) {
        if (attributes & PropertyAttribute::ReadOnly)
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

        set(vm, map, value);
        return true;
    }

    RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(Base::get())->callSetter(globalObject, thisValue, value, shouldThrow));
}

JSValue SparseArrayEntry::getNonSparseMode() const
{
    // TSAN wave 4 (triage §3.10): attributes() loads relaxed — same predicate,
    // no plain read of the shared attribute word.
    ASSERT(!attributes());
    return Base::get();
}

JSValue SparseArrayEntry::get() const
{
    return Base::get();
}

template<typename Visitor>
void SparseArrayValueMap::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    SparseArrayValueMap* thisObject = uncheckedDowncast<SparseArrayValueMap>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(cell, visitor);
    {
        Locker locker { thisObject->cellLock() };
        for (auto& entry : thisObject->m_map)
            visitor.append(entry.value.asValue());
    }
    visitor.reportExtraMemoryVisited(thisObject->m_reportedCapacity * sizeof(Map::KeyValuePairType));
}

DEFINE_VISIT_CHILDREN(SparseArrayValueMap);

} // namespace JSC


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
    // r19 (post-closeout review): publish the m_set header NSDMI stores to
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

ALWAYS_INLINE SparseArrayValueMap::AddResult SparseArrayValueMap::addLocked(unsigned i, size_t& increasedCapacity)
{
    AddResult result = m_set.ensure<SparseArrayEntryTranslator>(i, [&] {
        return SparseArrayEntry(i);
    });
    size_t capacity = m_set.capacity();
    if (capacity > m_reportedCapacity) {
        increasedCapacity = capacity - m_reportedCapacity;
        m_reportedCapacity = capacity;
    }
    return result;
}

ALWAYS_INLINE void SparseArrayValueMap::reportIncreasedCapacity(JSObject* array, size_t increasedCapacity)
{
    if (increasedCapacity)
        Heap::heap(array)->reportExtraMemoryAllocated(array, increasedCapacity * sizeof(SparseArrayEntry));
}

SparseArrayValueMap::AddResult SparseArrayValueMap::add(JSObject* array, unsigned i)
{
    AddResult result;
    size_t increasedCapacity = 0;
    {
        Locker locker { cellLock() };
        tsanAcquireCtorPublication();
        result = addLocked(i, increasedCapacity);
    }
    reportIncreasedCapacity(array, increasedCapacity);
    return result;
}

bool SparseArrayValueMap::addIfAbsent(VM& vm, JSObject* array, unsigned i, JSValue value, unsigned attributes)
{
    bool isNewEntry;
    size_t increasedCapacity = 0;
    {
        Locker locker { cellLock() };
        tsanAcquireCtorPublication();
        AddResult result = addLocked(i, increasedCapacity);
        isNewEntry = result.isNewEntry;
        if (isNewEntry)
            result.iterator->forceSet(vm, this, value, attributes);
    }
    reportIncreasedCapacity(array, increasedCapacity);
    return isNewEntry;
}

void SparseArrayValueMap::setEntry(VM& vm, unsigned i, JSValue value, unsigned attributes)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    auto it = m_set.find<SparseArrayEntryTranslator>(i);
    if (it == m_set.end())
        return;
    if (value)
        entryFor(it).forceSet(vm, this, value, attributes);
    else
        entryFor(it).forceSet(this, attributes);
}

void SparseArrayValueMap::remove(iterator it)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    m_set.remove(it);
}

void SparseArrayValueMap::remove(unsigned i)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    auto it = m_set.find<SparseArrayEntryTranslator>(i);
    if (it != m_set.end())
        m_set.remove(it);
}

bool SparseArrayValueMap::putEntry(JSGlobalObject* globalObject, JSObject* array, unsigned i, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(value);

    if (Options::useJSThreads()) [[unlikely]]
        RELEASE_AND_RETURN(scope, putEntryConcurrent(globalObject, array, i, value, shouldThrow));

    AddResult result = add(array, i);
    SparseArrayEntry& entry = *result.iterator;

    // To save a separate find & add, we first always add to the sparse map.
    // In the uncommon case that this is a new property, and the array is not
    // extensible, this is not the right thing to have done - so remove again.
    if (result.isNewEntry && !array->isStructureExtensible()) {
        remove(static_cast<const_iterator>(result.iterator));
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    }
    
    RELEASE_AND_RETURN(scope, entry.put(globalObject, array, this, value, shouldThrow));
}

// Flag-on: the lookup, the insert and the data store share one window under
// the map's cell lock, so the entry is inserted only when the write is
// allowed (a concurrent reader never sees a placeholder) and no iterator
// outlives the lock. Nothing under the lock runs JS, allocates GC memory or
// throws: the verdict is carried out after the lock drops, where the setter
// call and the TypeError allocation happen. The accessor test is on the value
// word itself, as the readers do, so a stale attribute word can never turn a
// data value into a GetterSetter.
bool SparseArrayValueMap::putEntryConcurrent(JSGlobalObject* globalObject, JSObject* array, unsigned i, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    bool isExtensible = array->isStructureExtensible();
    bool isReadOnly = false;
    JSValue getterSetter;
    size_t increasedCapacity = 0;
    {
        Locker locker { cellLock() };
        tsanAcquireCtorPublication();
        auto it = m_set.find<SparseArrayEntryTranslator>(i);
        if (it == m_set.end()) {
            if (isExtensible)
                addLocked(i, increasedCapacity).iterator->forceSet(vm, this, value, 0);
            else
                isReadOnly = true;
        } else {
            SparseArrayEntry& entry = entryFor(it);
            JSValue current = entry.get();
            if (current.isGetterSetter())
                getterSetter = current;
            else if (entry.attributes() & PropertyAttribute::ReadOnly)
                isReadOnly = true;
            else
                entry.forceSet(vm, this, value, entry.attributes());
        }
    }
    reportIncreasedCapacity(array, increasedCapacity);

    if (isReadOnly)
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
    if (getterSetter)
        RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(getterSetter)->callSetter(globalObject, array, value, shouldThrow));
    return true;
}

bool SparseArrayValueMap::putDirect(JSGlobalObject* globalObject, JSObject* array, unsigned i, JSValue value, unsigned attributes, PutDirectIndexMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(value);
    
    if (Options::useJSThreads()) [[unlikely]]
        RELEASE_AND_RETURN(scope, putDirectConcurrent(globalObject, array, i, value, attributes, mode));

    bool shouldThrow = (mode == PutDirectIndexShouldThrow);

    AddResult result = add(array, i);
    SparseArrayEntry& entry = *result.iterator;

    // To save a separate find & add, we first always add to the sparse map.
    // In the uncommon case that this is a new property, and the array is not
    // extensible, this is not the right thing to have done - so remove again.
    if (mode != PutDirectIndexLikePutDirect && result.isNewEntry && !array->isStructureExtensible()) {
        remove(static_cast<const_iterator>(result.iterator));
        return typeError(globalObject, scope, shouldThrow, NonExtensibleObjectPropertyDefineError);
    }

    if (entry.attributes() & PropertyAttribute::ReadOnly)
        return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

    entry.forceSet(vm, this, value, attributes);
    return true;
}

// Flag-on: same single locked window as putEntryConcurrent; the TypeError is
// thrown after the lock drops.
bool SparseArrayValueMap::putDirectConcurrent(JSGlobalObject* globalObject, JSObject* array, unsigned i, JSValue value, unsigned attributes, PutDirectIndexMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    bool shouldThrow = (mode == PutDirectIndexShouldThrow);
    bool mayInsert = mode == PutDirectIndexLikePutDirect || array->isStructureExtensible();
    ASCIILiteral error;
    size_t increasedCapacity = 0;
    {
        Locker locker { cellLock() };
        tsanAcquireCtorPublication();
        auto it = m_set.find<SparseArrayEntryTranslator>(i);
        if (it == m_set.end()) {
            if (mayInsert)
                addLocked(i, increasedCapacity).iterator->forceSet(vm, this, value, attributes);
            else
                error = NonExtensibleObjectPropertyDefineError;
        } else if (it->attributes() & PropertyAttribute::ReadOnly)
            error = ReadonlyPropertyWriteError;
        else
            entryFor(it).forceSet(vm, this, value, attributes);
    }
    reportIncreasedCapacity(array, increasedCapacity);

    if (!error.isNull())
        return typeError(globalObject, scope, shouldThrow, error);
    return true;
}

JSValue SparseArrayValueMap::getConcurrently(unsigned i)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    auto iterator = m_set.find<SparseArrayEntryTranslator>(i);
    if (iterator == m_set.end())
        return JSValue();
    return iterator->getConcurrently();
}

void SparseArrayEntry::forceSet(SparseArrayValueMap* map, unsigned attributes)
{
    // FIXME: We can expand this for non x86 environments. Currently, loading ReadOnly | DontDelete property
    // from compiler thread is only supported in X86 architecture because of its TSO nature.
    // https://bugs.webkit.org/show_bug.cgi?id=134641
    if (isX86())
        WTF::storeStoreFence();

    if (attributes & PropertyAttribute::Accessor)
        map->setHasAnyKindOfGetterSetterProperties();
    // TSAN wave 4 (triage §3.10 / §8.10): this locked store pairs with the
    // unlocked relaxed reads in attributes()/get()/getConcurrently; a plain
    // store on the shared word is UB against them. Relaxed atomic store —
    // codegen-identical, flag-off unchanged. The storeStoreFence above still
    // orders the value store before this attribute publication for the
    // getConcurrently consume chain.
    WTF::atomicStore(&m_attributes, attributes, std::memory_order_relaxed);
}

void SparseArrayEntry::forceSet(VM& vm, SparseArrayValueMap* map, JSValue value, unsigned attributes)
{
    m_value.set(vm, map, value);
    forceSet(map, attributes);
}

std::optional<SparseArrayEntry> SparseArrayValueMap::getEntry(unsigned i)
{
    Locker locker { cellLock() };
    tsanAcquireCtorPublication();
    auto it = m_set.find<SparseArrayEntryTranslator>(i);
    if (it == m_set.end())
        return std::nullopt;
    // TSAN wave 5 (triage family 10 jsvalue-slots, r4 key 'data race x
    // SparseArrayValueMap::getEntry'): do NOT memcpy the shared entry into
    // the optional — unlocked relaxed-atomic writers (putIndexedDescriptor's
    // forceSet under defineOwnIndexedProperty) race a plain copy. Snapshot
    // field-wise through the relaxed accessors; the lock still serializes
    // against rehash (entry address stability), the entry words themselves
    // carry the OM §1 racy-value tolerance.
    return it->copySnapshotConcurrent();
}

void SparseArrayEntry::get(JSObject* thisObject, PropertySlot& slot) const
{
    JSValue value = m_value.get();
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
    descriptor.setDescriptor(m_value.get(), attributes());
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

    return attributesDependency.consume(this)->m_value.get();
}

bool SparseArrayEntry::put(JSGlobalObject* globalObject, JSValue thisValue, SparseArrayValueMap* map, JSValue value, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // Flag-off only: flag-on putEntry stores under the map's cell lock in
    // putEntryConcurrent. attributes() loads relaxed and is codegen-identical
    // to a plain load.
    unsigned attributes = this->attributes();
    if (!(attributes & PropertyAttribute::Accessor)) {
        if (attributes & PropertyAttribute::ReadOnly)
            return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);

        m_value.set(vm, map, value);
        return true;
    }

    RELEASE_AND_RETURN(scope, uncheckedDowncast<GetterSetter>(m_value.get())->callSetter(globalObject, thisValue, value, shouldThrow));
}

JSValue SparseArrayEntry::getNonSparseMode() const
{
    // TSAN wave 4 (triage §3.10): attributes() loads relaxed — same predicate,
    // no plain read of the shared attribute word.
    ASSERT(!attributes());
    return m_value.get();
}

JSValue SparseArrayEntry::get() const
{
    return m_value.get();
}

template<typename Visitor>
void SparseArrayValueMap::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    SparseArrayValueMap* thisObject = uncheckedDowncast<SparseArrayValueMap>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(cell, visitor);
    {
        Locker locker { thisObject->cellLock() };
        for (auto& entry : thisObject->m_set)
            visitor.append(entry.asValue());
    }
    visitor.reportExtraMemoryVisited(thisObject->m_reportedCapacity * sizeof(SparseArrayEntry));
}

DEFINE_VISIT_CHILDREN(SparseArrayValueMap);

} // namespace JSC


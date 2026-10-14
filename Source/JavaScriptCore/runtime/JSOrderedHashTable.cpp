/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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
#include "JSOrderedHashTable.h"

#include "AOTTypedCompactTable.h"
#include "ButterflyInlinesLight.h"

namespace JSC {

template<typename Traits>
template<typename Visitor>
void JSOrderedHashTable<Traits>::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSOrderedHashTable<Traits>* thisObject = uncheckedDowncast<JSOrderedHashTable<Traits>>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);

    visitor.append(thisObject->m_storage);
#if ENABLE(AOT)
    visitor.append(thisObject->m_compactTable);
#endif
}

#if ENABLE(AOT)
using AOT::Typed::CompactTable;

template<typename Traits>
void JSOrderedHashTable<Traits>::convertCompactTable()
{
    CompactTable::convert(vm(), this);
}

template<typename Traits>
void JSOrderedHashTable<Traits>::copyCompactTableOf(VM& vm, JSOrderedHashTable* base)
{
    m_compactTable.set(vm, this, uncheckedDowncast<CompactTable>(base->m_compactTable.get())->copy(vm));
}

template<typename Traits>
uint32_t JSOrderedHashTable<Traits>::sizeOfCompactTable()
{
    return uncheckedDowncast<CompactTable>(m_compactTable.get())->size();
}

template<typename Traits>
void JSOrderedHashTable<Traits>::clearCompactTable()
{
    CompactTable::clear(vm(), this);
}

template<typename Traits>
static NEVER_INLINE JSValue findInCompactTableGenerally(JSGlobalObject* globalObject, CompactTable* table, JSValue key)
{
    auto entry = table->find(globalObject, key);
    if (!entry)
        return { };
    return Traits::hasValueData ? table->valueAt(*entry) : key;
}

template<typename Traits>
JSValue JSOrderedHashTable<Traits>::findInCompactTable(JSGlobalObject* globalObject, JSValue key)
{
    auto* table = uncheckedDowncast<CompactTable>(m_compactTable.get());
    uint32_t found;
    switch (table->tryFindQuickly(key, found)) {
    case CompactTable::Quickly::Found:
        return Traits::hasValueData ? table->valueAt(found) : key;
    case CompactTable::Quickly::Absent:
        return { };
    case CompactTable::Quickly::Unknown:
        break;
    }
    return findInCompactTableGenerally<Traits>(globalObject, table, key);
}

template<typename Traits>
bool JSOrderedHashTable<Traits>::removeFromCompactTable(JSGlobalObject* globalObject, JSValue key)
{
    return CompactTable::remove(globalObject, this, key);
}

template<typename Traits>
bool JSOrderedHashTable<Traits>::tryAddToCompactTable(JSGlobalObject* globalObject, JSValue key, JSValue value)
{
    return CompactTable::tryAdd(globalObject, this, key, value);
}
#endif

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(template<typename Traits>, JSOrderedHashTable<Traits>);

template class JSOrderedHashTable<MapTraits>;
template class JSOrderedHashTable<SetTraits>;

} // namespace JSC

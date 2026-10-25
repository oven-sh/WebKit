/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PropertyRun.h"

#include "Identifier.h"
#include "JSCInlines.h"
#include "PropertyTable.h"
#include <wtf/HashFunctions.h>
#include <wtf/MathExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyRun);
WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyRunTable);

unsigned PropertyRun::computeHash(std::span<UniquedStringImpl* const> names)
{
    unsigned hash = names.size();
    for (UniquedStringImpl* name : names)
        hash = WTF::pairIntHash(hash, PtrHash<UniquedStringImpl*>::hash(name));
    return hash;
}

PropertyRun::PropertyRun(std::span<UniquedStringImpl* const> names, unsigned hash)
    : m_names(FixedVector<RefPtr<UniquedStringImpl>>::createWithSizeFromGenerator(names.size(), [&](size_t index) {
        return RefPtr<UniquedStringImpl> { names[index] };
    }))
    , m_transitionKey(SymbolImpl::createNullSymbol())
    , m_hash(hash)
{
}

struct PropertyRunTable::NamesTranslator {
    static unsigned hash(const std::span<UniquedStringImpl* const>& names) { return PropertyRun::computeHash(names); }

    static bool equal(const Ref<PropertyRun>& run, const std::span<UniquedStringImpl* const>& names)
    {
        if (run->size() != names.size())
            return false;
        for (size_t i = 0; i < names.size(); ++i) {
            if (run->names()[i].get() != names[i])
                return false;
        }
        return true;
    }
};

const PropertyRun* PropertyRunTable::find(std::span<UniquedStringImpl* const> names) const
{
    auto iterator = m_runs.find<NamesTranslator>(names);
    return iterator == m_runs.end() ? nullptr : iterator->ptr();
}

std::span<const PropertyRun* const> PropertyRunTable::runsEndingWith(UniquedStringImpl* lastName) const
{
    auto iterator = m_runsByLastName.find(lastName);
    if (iterator == m_runsByLastName.end())
        return { };
    return iterator->value.span();
}

const PropertyRun* PropertyRunTable::intern(std::span<UniquedStringImpl* const> names)
{
    if (names.size() < 2 || names.size() > PropertyRun::maxSize)
        return nullptr;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!names[i] || (!names[i]->isSymbol() && parseIndex(*names[i])))
            return nullptr;
        for (size_t j = 0; j < i; ++j) {
            if (names[j] == names[i])
                return nullptr;
        }
    }

    if (const PropertyRun* existing = find(names))
        return existing;
    Ref<PropertyRun> run = adoptRef(*new PropertyRun(names, PropertyRun::computeHash(names)));
    const PropertyRun* result = run.ptr();
    m_runs.add(WTF::move(run));
    auto& runsWithLastName = m_runsByLastName.add(names.back(), Vector<const PropertyRun*, 1> { }).iterator->value;
    size_t position = runsWithLastName.findIf([&](const PropertyRun* other) {
        return other->size() > result->size();
    });
    runsWithLastName.insert(position == notFound ? runsWithLastName.size() : position, result);
    return result;
}

bool PropertyAdditionList::hasAttributes() const
{
    if (attributes.empty())
        return commonAttributes;
    return std::ranges::any_of(attributes, [](unsigned value) {
        return value;
    });
}

struct PropertyAdditionCache::AdditionsTranslator {
    struct Query {
        Structure* base;
        unsigned hash;
        const PropertyAdditionList& additions;
    };

    static unsigned hash(const Query& query) { return query.hash; }

    static bool equal(const Entry& entry, const Query& query)
    {
        return entry.hash == query.hash && entry.base == query.base && entry.structure && matches(entry.structure, query.base, query.additions);
    }
};

PropertyAdditionCache::~PropertyAdditionCache()
{
    if (m_vm)
        m_vm->heap.unregisterWeakGCHashTable(this);
}

// The order of the names is part of the key, and so every order of one set of names has a place of its own. Addresses, not texts: whoever sends the names cannot choose the places.
unsigned PropertyAdditionCache::computeHash(Structure* base, std::span<UniquedStringImpl* const> names)
{
    unsigned hash = PtrHash<Structure*>::hash(base);
    for (UniquedStringImpl* name : names)
        hash = WTF::pairIntHash(hash, PtrHash<UniquedStringImpl*>::hash(name));
    return hash;
}

unsigned PropertyAdditionCache::computeUnorderedHash(unsigned propertyHash, unsigned numberOfProperties)
{
    return WTF::pairIntHash(propertyHash, numberOfProperties);
}

bool PropertyAdditionCache::matches(Structure* structure, Structure* base, const PropertyAdditionList& additions)
{
    PropertyTable* table = structure->materializedPropertyTable();
    if (!table || table->hasDeletedOffset())
        return false;
    unsigned inlineCapacity = base->inlineCapacity();
    unsigned firstPropertyNumber = numberOfSlotsForMaxOffset(base->maxOffset(), inlineCapacity);
    if (structure->inlineCapacity() != inlineCapacity || table->size() != firstPropertyNumber + additions.names.size())
        return false;
    for (size_t i = 0; i < additions.names.size(); ++i) {
        auto [offset, attributes] = table->get(additions.names[i]);
        if (offset != offsetForPropertyNumber(firstPropertyNumber + i, inlineCapacity) || attributes != additions.attributesAt(i))
            return false;
    }
    return true;
}

Structure* PropertyAdditionCache::find(Structure* base, const PropertyAdditionList& additions) const
{
    auto iterator = m_entries.find<AdditionsTranslator>(AdditionsTranslator::Query { base, computeHash(base, additions.names), additions });
    return iterator == m_entries.end() ? nullptr : iterator->structure;
}

void PropertyAdditionCache::add(VM& vm, Structure* base, std::span<UniquedStringImpl* const> names, unsigned unorderedHash, Structure* structure)
{
    Entry entry { base, structure, computeHash(base, names), unorderedHash };
    ASSERT(!m_entries.contains(entry));
    if (!m_vm) {
        m_vm = &vm;
        vm.heap.registerWeakGCHashTable(this);
    }
    m_entries.add(entry);
    if (m_entries.size() * bitsForEachEntry > m_unorderedHashes.numBits())
        rebuildUnorderedHashes();
    else
        m_unorderedHashes.at(unorderedHash & (m_unorderedHashes.numBits() - 1)) = true;
    markDirty(vm);
}

void PropertyAdditionCache::rebuildUnorderedHashes()
{
    FastBitVector unorderedHashes(m_entries.isEmpty() ? 0 : std::max<size_t>(fewestBits, roundUpToPowerOfTwo(m_entries.size() * bitsForEachEntry * 2)));
    for (const Entry& entry : m_entries)
        unorderedHashes.at(entry.unorderedHash & (unorderedHashes.numBits() - 1)) = true;
    m_unorderedHashes = unorderedHashes;
}

void PropertyAdditionCache::reconcileWeakReferencesAtGCEnd(VM& vm, CollectionScope collectionScope)
{
    auto isDead = [&](const Entry& entry) {
        return !entry.structure || !vm.heap.isMarked(entry.base) || !vm.heap.isMarked(entry.structure);
    };
    if (collectionScope == CollectionScope::Full) {
        if (m_entries.removeIf(isDead))
            rebuildUnorderedHashes();
        return;
    }
    for (const Entry& entry : m_entries) {
        if (isDead(entry))
            entry.structure = nullptr;
    }
}

} // namespace JSC

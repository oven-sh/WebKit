/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <JavaScriptCore/JSExportMacros.h>
#include <JavaScriptCore/WeakGCHashTable.h>
#include <span>
#include <wtf/FastBitVector.h>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class Structure;
class VM;

class PropertyRun final : public RefCounted<PropertyRun> {
    WTF_MAKE_TZONE_ALLOCATED_EXPORT(PropertyRun, JS_EXPORT_PRIVATE);
    WTF_MAKE_NONCOPYABLE(PropertyRun);
public:
    static constexpr unsigned maxSize = 64;

    std::span<const RefPtr<UniquedStringImpl>> names() const { return m_names.span(); }
    unsigned size() const { return m_names.size(); }
    UniquedStringImpl* transitionKey() const { return m_transitionKey.ptr(); }
    unsigned hash() const { return m_hash; }

    static unsigned computeHash(std::span<UniquedStringImpl* const> names);

private:
    friend class PropertyRunTable;

    PropertyRun(std::span<UniquedStringImpl* const> names, unsigned hash);

    const FixedVector<RefPtr<UniquedStringImpl>> m_names;
    const Ref<SymbolImpl> m_transitionKey;
    const unsigned m_hash;
};

struct PropertyAdditionList {
    unsigned attributesAt(size_t index) const { return attributes.empty() ? commonAttributes : attributes[index]; }
    bool hasAttributes() const;

    std::span<UniquedStringImpl* const> names;
    std::span<const unsigned> attributes;
    unsigned commonAttributes { 0 };
};

class PropertyAdditionCache final : public WeakGCHashTable {
    WTF_MAKE_NONCOPYABLE(PropertyAdditionCache);
public:
    PropertyAdditionCache() = default;
    JS_EXPORT_PRIVATE ~PropertyAdditionCache() final;

    static unsigned computeHash(Structure* base, std::span<UniquedStringImpl* const> names);
    static unsigned computeUnorderedHash(unsigned propertyHash, unsigned numberOfProperties);
    static bool matches(Structure*, Structure* base, const PropertyAdditionList&);

    bool isEmpty() const { return m_entries.isEmpty(); }
    unsigned size() const { return m_entries.size(); }
    bool mayContainUnorderedHash(unsigned unorderedHash) const { return m_unorderedHashes.numBits() && m_unorderedHashes.at(unorderedHash & (m_unorderedHashes.numBits() - 1)); }
    JS_EXPORT_PRIVATE Structure* find(Structure* base, const PropertyAdditionList&) const;
    void add(VM&, Structure* base, std::span<UniquedStringImpl* const> names, unsigned unorderedHash, Structure*);

    void reconcileWeakReferencesAtGCEnd(VM&, CollectionScope) final;

private:
    struct Entry {
        Entry() = default;
        Entry(Structure* base, Structure* structure, unsigned hash, unsigned unorderedHash)
            : base(base)
            , structure(structure)
            , hash(hash)
            , unorderedHash(unorderedHash)
        {
        }
        Entry(WTF::HashTableDeletedValueType)
            : base(std::bit_cast<Structure*>(static_cast<uintptr_t>(1)))
        {
        }

        bool isHashTableDeletedValue() const { return base == std::bit_cast<Structure*>(static_cast<uintptr_t>(1)); }

        friend bool operator==(const Entry&, const Entry&) = default;

        Structure* base { nullptr };
        mutable Structure* structure { nullptr };
        unsigned hash { 0 };
        unsigned unorderedHash { 0 };
    };
    struct EntryHash {
        static unsigned hash(const Entry& entry) { return entry.hash; }
        static bool equal(const Entry& a, const Entry& b) { return a == b; }
        static constexpr bool safeToCompareToEmptyOrDeleted = true;
    };
    struct AdditionsTranslator;

    static constexpr size_t bitsForEachEntry = 8;
    static constexpr size_t fewestBits = 1024;

    void rebuildUnorderedHashes();

    HashSet<Entry, EntryHash, SimpleClassHashTraits<Entry>> m_entries;
    // One bit for the names of an entry in whatever order. It may say yes where there is nothing, never no where there is something.
    FastBitVector m_unorderedHashes;
    VM* m_vm { nullptr };
};

class PropertyRunTable {
    WTF_MAKE_TZONE_ALLOCATED(PropertyRunTable);
    WTF_MAKE_NONCOPYABLE(PropertyRunTable);
public:
    PropertyRunTable() = default;

    JS_EXPORT_PRIVATE const PropertyRun* intern(std::span<UniquedStringImpl* const> names);
    const PropertyRun* find(std::span<UniquedStringImpl* const> names) const;
    // Shortest first.
    std::span<const PropertyRun* const> runsEndingWith(UniquedStringImpl* lastName) const;
    bool isEmpty() const { return m_runs.isEmpty(); }

    PropertyAdditionCache& additionCache() LIFETIME_BOUND { return m_additionCache; }

private:
    struct RunHash {
        static unsigned hash(const Ref<PropertyRun>& run) { return run->hash(); }
        static bool equal(const Ref<PropertyRun>& a, const Ref<PropertyRun>& b) { return a.ptr() == b.ptr(); }
        static constexpr bool safeToCompareToEmptyOrDeleted = false;
    };
    struct NamesTranslator;

    HashSet<Ref<PropertyRun>, RunHash> m_runs;
    HashMap<UniquedStringImpl*, Vector<const PropertyRun*, 1>> m_runsByLastName;
    PropertyAdditionCache m_additionCache;
};

} // namespace JSC

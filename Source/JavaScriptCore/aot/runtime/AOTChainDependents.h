/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyWalk.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include "JSObject.h"
#include "RememberedSlot.h"
#include <array>
#include <bit>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

struct State;

using ChainObjects = Vector<JSObject*, 4>;

class ObjectsSeen {
public:
    void add(const JSObject* object) { m_words[hashOf(object) / 64] |= uint64_t { 1 } << hashOf(object) % 64; }
    bool mayContain(const JSObject* object) const { return m_words[hashOf(object) / 64] >> hashOf(object) % 64 & 1; }

private:
    static constexpr unsigned bitsOfHash = 13;
    static uint32_t hashOf(const JSObject* object) { return static_cast<uint32_t>(std::bit_cast<uintptr_t>(object) >> 4) * 0x9e3779b1u >> (32 - bitsOfHash); }

    std::array<uint64_t, (1u << bitsOfHash) / 64> m_words { };
};

enum class ChainWalkMode : uint8_t { MayAllocate, AllocatesNothing };
enum class ChainWalkUse : uint8_t { KeptForReads, KeptForReadsThatAskObjects, KeptForStores, UsedAtOnce };
enum class ChainWalkResult : uint8_t { Unknown, Absent, Found };

struct ChainWalkStart {
    JSGlobalObject* globalObject;
    Structure* structure;
    JSObject* receiverOrNull { nullptr };
    ChainWalkMode mode { ChainWalkMode::MayAllocate };
    ChainWalkUse use { ChainWalkUse::KeptForReads };
};

struct ChainWalk {
    bool dependsOnChain() const { return !changeableObjects.isEmpty(); }

    ChainWalkResult result { ChainWalkResult::Unknown };
    bool didFlatten { false };
    bool passedSlotFilledWithoutReport { false };
    bool passedEmptySlot { false };
    JSObject* firstObjectWithEmptySlot { nullptr };
    JSObject* holder { nullptr };
    JSObject* owner { nullptr };
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
    ChainObjects changeableObjects;
};

ChainWalk walkChain(const ChainWalkStart&, UniquedStringImpl*);

ALWAYS_INLINE JSValue valueAtOffset(const JSObject* holder, PropertyOffset offset)
{
    if (isInlineOffset(offset))
        return holder->inlineStorageUnsafe()[offsetInInlineStorage(offset)].get();
    return holder->getDirect(offset);
}

enum class StoreThroughChain : uint8_t { Unknown, CreatesOwnProperty, CallsSetter, CallsCustomSetter, IsRefused };

StoreThroughChain storeThroughChain(const ChainWalk&);

struct CreatedMembers {
    JSObject* prototype;
    Structure* structureOfDeclaredMemberHolders;
};

class ChainDependentOwner {
public:
    virtual ~ChainDependentOwner() = default;
    virtual void dropChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) = 0;
    virtual bool hasChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) const = 0;
    virtual bool keepChainDependent(VM&, uint32_t, UniquedStringImpl*, const CreatedMembers&) { return false; }
};

enum class ChainDependentOwnerID : uint8_t { NoOwner = UINT8_MAX };

struct ChainDependent {
    friend bool operator==(const ChainDependent&, const ChainDependent&) = default;

    UniquedStringImpl* uidOrNull { nullptr };
    uint32_t subject { 0 };
    ChainDependentOwnerID owner { };
};
static_assert(sizeof(ChainDependent) == 16);

class ChainDependents final : ChainDependentOwner {
    WTF_MAKE_NONCOPYABLE(ChainDependents);
    WTF_MAKE_TZONE_ALLOCATED(ChainDependents);
public:
    explicit ChainDependents(State&);

    ChainDependentOwnerID addOwner(ChainDependentOwner&);

    void add(std::span<JSObject* const> chainObjects, const ChainDependent&);
    void addStateOfStructure(std::span<JSObject* const> chainObjects, Structure*, UniquedStringImpl*);
    void didChange(JSObject* changed, UniquedStringImpl* uidOrNull)
    {
        if (m_objectsWithDependents.mayContain(changed))
            dropDependentsOn(changed, uidOrNull);
    }
    void didCreateMembers(const CreatedMembers&);
    bool hasDependents(JSObject* object) const { return m_dependentsByObject.contains(object); }
    size_t numberOfDependents(JSObject*) const;
    JSGlobalObject* realm() const;

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    struct HashTraits : WTF::GenericHashTraits<ChainDependent> {
        static constexpr bool emptyValueIsZero = false;
        static ChainDependent emptyValue() { return { nullptr, 0, ChainDependentOwnerID::NoOwner }; }
        static void constructDeletedValue(ChainDependent& slot) { slot = { nullptr, 1, ChainDependentOwnerID::NoOwner }; }
        static bool isDeletedValue(const ChainDependent& value) { return value == ChainDependent { nullptr, 1, ChainDependentOwnerID::NoOwner }; }
    };
    struct Hash {
        static unsigned hash(const ChainDependent& dependent) { return WTF::pairIntHash(WTF::PtrHash<UniquedStringImpl*>::hash(dependent.uidOrNull), WTF::intHash(dependent.subject) + static_cast<unsigned>(dependent.owner)); }
        static bool equal(const ChainDependent& a, const ChainDependent& b) { return a == b; }
        static constexpr bool safeToCompareToEmptyOrDeleted = true;
    };
    using DependentSet = UncheckedKeyHashSet<ChainDependent, Hash, HashTraits>;
    struct ObjectDependents {
        bool isEmpty() const { return onAnyName.isEmpty() && byName.isEmpty(); }
        size_t size() const;
        void forEach(const Invocable<void(const ChainDependent&)> auto&) const;
        void removeIf(const Invocable<bool(const ChainDependent&)> auto&);

        DependentSet onAnyName;
        UncheckedKeyHashMap<UniquedStringImpl*, DependentSet> byName;
    };

    void dropDependentsOn(JSObject* changed, UniquedStringImpl* uidOrNull);
    void dropChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl*) final;
    bool hasChainDependent(VM&, uint32_t structureIDBits, UniquedStringImpl*) const final;
    ChainDependentOwner& ownerOf(const ChainDependent& dependent) { return *m_owners[static_cast<unsigned>(dependent.owner)]; }

    State& m_state;
    Vector<ChainDependentOwner*, 8> m_owners;
    UncheckedKeyHashSet<StructureID> m_structuresWithState;
    UncheckedKeyHashMap<JSObject*, ObjectDependents> m_dependentsByObject;
    ObjectsSeen m_objectsWithDependents;
    size_t m_numberOfDependentsAddedSinceOwnersWereAsked { 0 };
    size_t m_numberOfDependentsLeftWhenOwnersWereAsked { 0 };
    RememberedSlotSet m_writtenSinceCollection;
};

void didChangeChainObject(VM&, JSObject* changed, UniquedStringImpl* uidOrNull);
void didReplaceValueOnChainObject(VM&, JSObject* changed, UniquedStringImpl*);
enum class StoredName : bool { MayBeSpelledByProgram, IsNotSpelledByProgram };
bool storeCreatesOwnProperty(JSGlobalObject*, Structure* receiverStructure, JSObject* receiverOrNull, UniquedStringImpl*, StoredName = StoredName::MayBeSpelledByProgram);
bool storeCreatesOwnProperty(JSGlobalObject*, Structure* receiverStructure, JSObject* receiverOrNull, UniquedStringImpl*, ChainDependents& ofOwner, const ChainDependent&);

PropertyWalk::Presence presenceForStructure(VM&, Structure*, UniquedStringImpl*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

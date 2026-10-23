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
#include "PropertyOffset.h"
#include "RememberedSlot.h"
#include <memory>
#include <span>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC {

class JSGlobalObject;
class JSObject;
class Structure;
class VM;

namespace AOT {

struct State;

using ChainObjects = Vector<JSObject*, 4>;

enum class ChainWalkMode : uint8_t { MayAllocate, AllocatesNothing };
// A kept answer holds until an object of the chain reports a change.
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
    JSObject* firstObjectWithEmptySlot { nullptr };
    // The cell whose storage has the property: the object of the chain, or the extra properties of a compiled one.
    JSObject* holder { nullptr };
    JSObject* owner { nullptr };
    // Invalid for a field of a compiled object whose slot holds no boxed value in the cell: the walk says what a store meets there, and a read asks the holder.
    PropertyOffset offset { invalidOffset };
    unsigned attributes { 0 };
    ChainObjects changeableObjects;
};

ChainWalk walkChain(const ChainWalkStart&, UniquedStringImpl*);

// holder->getDirect(offset). A field of a compiled object has the offset of an inline property, and the Structure of such an object speaks of no inline storage.
ALWAYS_INLINE JSValue valueAtOffset(const JSObject* holder, PropertyOffset offset)
{
    if (isInlineOffset(offset))
        return holder->inlineStorageUnsafe()[offsetInInlineStorage(offset)].get();
    return holder->getDirect(offset);
}

enum class StoreThroughChain : uint8_t { Unknown, CreatesOwnProperty, CallsSetter, CallsCustomSetter, IsRefused };

StoreThroughChain storeThroughChain(const ChainWalk&);

class ChainDependentOwner {
public:
    virtual ~ChainDependentOwner() = default;
    virtual void dropChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) = 0;
    virtual bool hasChainDependent(VM&, uint32_t subject, UniquedStringImpl* uidOrNull) const = 0;
};

class ChainDependentOwnerID {
public:
    constexpr ChainDependentOwnerID() = default;
    explicit constexpr ChainDependentOwnerID(uint8_t index)
        : m_index(index)
    {
    }

    constexpr uint8_t index() const { return m_index; }
    friend constexpr bool operator==(ChainDependentOwnerID, ChainDependentOwnerID) = default;

private:
    uint8_t m_index { 0 };
};

struct ChainDependent {
    friend bool operator==(const ChainDependent&, const ChainDependent&) = default;

    UniquedStringImpl* uidOrNull { nullptr };
    uint32_t subject { 0 };
    ChainDependentOwnerID owner;
    bool isHashTableDeletedValue { false };
};
static_assert(sizeof(ChainDependent) == 16);

struct ChainDependentHashTraits : WTF::GenericHashTraits<ChainDependent> {
    static constexpr bool emptyValueIsZero = false;
    static constexpr bool hasIsEmptyValueFunction = true;
    static ChainDependent emptyValue() { return { nullptr, UINT32_MAX, ChainDependentOwnerID(UINT8_MAX), false }; }
    static bool isEmptyValue(const ChainDependent& value) { return value == emptyValue(); }
    static void constructDeletedValue(ChainDependent& slot) { slot = { nullptr, UINT32_MAX, ChainDependentOwnerID(UINT8_MAX), true }; }
    static bool isDeletedValue(const ChainDependent& value) { return value.isHashTableDeletedValue; }
};

struct ChainDependentHash {
    static unsigned hash(const ChainDependent& dependent) { return WTF::pairIntHash(WTF::PtrHash<UniquedStringImpl*>::hash(dependent.uidOrNull), WTF::intHash(dependent.subject) + dependent.owner.index()); }
    static bool equal(const ChainDependent& a, const ChainDependent& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = true;
};

class ChainDependents {
    WTF_MAKE_NONCOPYABLE(ChainDependents);
    WTF_MAKE_TZONE_ALLOCATED(ChainDependents);
public:
    static constexpr unsigned maxNumberOfOwners = UINT8_MAX;

    explicit ChainDependents(State&);
    ~ChainDependents();

    ChainDependentOwnerID addOwner(ChainDependentOwner&);
    ChainDependentOwnerID structureStateOwner() const { return m_structureStateOwner; }

    void add(std::span<JSObject* const> chainObjects, const ChainDependent&);
    void didChange(JSObject* changed, UniquedStringImpl* uidOrNull);
    bool hasDependents(JSObject*) const;
    size_t numberOfDependents(JSObject*) const;

    size_t removeDeadEntries(VM&, CollectionScope);

private:
    using DependentSet = UncheckedKeyHashSet<ChainDependent, ChainDependentHash, ChainDependentHashTraits>;
    class StructureStateOwner;

    ChainDependentOwner& ownerOf(const ChainDependent& dependent) { return *m_owners[dependent.owner.index()]; }

    State& m_state;
    Vector<ChainDependentOwner*, 8> m_owners;
    std::unique_ptr<StructureStateOwner> m_structureState;
    ChainDependentOwnerID m_structureStateOwner;
    UncheckedKeyHashMap<JSObject*, DependentSet> m_dependentsByObject;
    size_t m_numberOfDependentsAddedSinceOwnersWereAsked { 0 };
    size_t m_numberOfDependentsLeftWhenOwnersWereAsked { 0 };
    RememberedSlotSet m_writtenSinceCollection;
};

void didChangeChainObject(VM&, JSObject* changed, UniquedStringImpl* uidOrNull);
void didFlattenStructure(VM&, Structure*);
void didChangeGlobalLexicalBindings(JSGlobalObject*);

bool storeCreatesOwnProperty(JSGlobalObject*, Structure* receiverStructure, JSObject* receiverOrNull, UniquedStringImpl*);
bool storeCreatesOwnProperty(JSGlobalObject*, Structure* receiverStructure, JSObject* receiverOrNull, UniquedStringImpl*, const ChainDependent&);

PropertyWalk::Presence presenceForStructure(VM&, Structure*, UniquedStringImpl*);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

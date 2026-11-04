/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyList.h"
#include "AOTState.h"
#include "AOTVector.h"
#include "RememberedSlot.h"
#include "StructureID.h"
#include <array>
#include <wtf/Noncopyable.h>
#include <wtf/text/Latin1Character.h>

namespace JSC {

class JSArray;
class JSCellButterfly;
class JSObject;
class JSPropertyNameEnumerator;
class StructureChain;
class VM;

namespace AOT {

class Object;
struct State;

// The own properties of the objects with one shape word, one numbered key order and one Structure of the second object, in key order: each an enumerable data property under a string.
struct CachedPropertyList {
    bool isFor(StructureID ofExtraProperties, uint32_t shape, uint32_t order) const { return extraPropertiesStructureID == ofExtraProperties && shapeWord == shape && orderAndCount == order; }

    StructureID extraPropertiesStructureID;
    uint32_t shapeWord { 0 };
    uint32_t orderAndCount { 0 };
    // The program holds the name of a field, the Structure of the second object every other.
    Vector<PropertyListEntry> properties;
    // The names as strings, made when they are first asked for.
    JSCellButterfly* keys { nullptr };
    // What a for-in loop got last over an object whose Structure names the Structure of the second object, and the chain of prototypes that it was made for. The enumerator names the Structure of that object.
    JSPropertyNameEnumerator* enumerator { nullptr };
    StructureChain* prototypeChain { nullptr };
    // Written when JSON.stringify first asks for the list, with holdsOnlyPlainData.
    // ,"name": of each property, one behind the other, with FieldAsProperty::jsonKeyBlockSize bytes behind the last. Both are empty where a name is not written as it is.
    Vector<Latin1Character> jsonKeys;
    Vector<uint32_t> endsOfJSONKeys;
    bool hasToJSON { false };
    bool holdsOnlyPlainData { false };
};

// Every own key of the objects with one Structure and one order word, enumerable or not: the strings, and behind them the symbols where they are listed.
struct KeptOwnKeys {
    bool isFor(StructureID id, uint32_t word, bool symbolsToo) const { return structureID == id && orderWord == word && listsSymbols == symbolsToo; }

    StructureID structureID;
    uint32_t orderWord { 0 };
    bool listsSymbols { false };
    // Null until the keys are asked for a second time.
    JSCellButterfly* keys { nullptr };
};

class PropertyListCache {
    WTF_MAKE_NONCOPYABLE(PropertyListCache);
public:
    explicit PropertyListCache(State&);

    static constexpr unsigned numberOfEntries = 256;
    static constexpr unsigned numberOfOwnKeyLists = 64;

    CachedPropertyList* find(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount)
    {
        if (!m_entries)
            return nullptr;
        CachedPropertyList& list = (*m_entries)[placeOf(extraPropertiesStructureID, shapeWord, orderAndCount)];
        return list.isFor(extraPropertiesStructureID, shapeWord, orderAndCount) ? &list : nullptr;
    }
    CachedPropertyList& add(CachedPropertyList&&);
    // The place of that list, or the place that it would take.
    KeptOwnKeys& ownKeysFor(StructureID, uint32_t orderWord, bool listsSymbols);
    void clear();

private:
    using Entries = std::array<CachedPropertyList, numberOfEntries>;
    using OwnKeyLists = std::array<KeptOwnKeys, numberOfOwnKeyLists>;

    static unsigned placeOf(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount) { return ((extraPropertiesStructureID.bits() >> 4) + shapeWord + orderAndCount * 31) % numberOfEntries; }

    MadeInArena<Entries> m_entries;
    MadeInArena<OwnKeyLists> m_ownKeyLists;
    unsigned m_numberOfOwnKeyListsReplaced { 0 };
    RememberedSlotSet m_writes;
};

// Made if need be, and kept until the next collection or until another list takes its place. Null for an object whose properties are not listed so.
CachedPropertyList* cachedPropertyList(VM&, const Object&);
// What is kept for objects like this one, or null. This object may lack a field that is listed.
CachedPropertyList* findCachedPropertyList(const Object&, JSObject* extraProperties);
void writeJSONKeys(VM&, CachedPropertyList&);
// For an object with extra properties whose Structure names theirs. The list that is kept, or null.
JSCellButterfly* findKeptOwnKeys(const Object&, bool listsSymbols);
// The list was made for the object just now. It is kept if the same was asked for before.
void didListOwnKeys(VM&, const Object&, bool listsSymbols, JSArray* list);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

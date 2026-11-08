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
#include <wtf/BitSet.h>
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

struct CachedPropertyList {
    bool isFor(StructureID ofExtraProperties, uint32_t shape, uint32_t order) const { return extraPropertiesStructureID == ofExtraProperties && shapeWord == shape && orderAndCount == order; }

    StructureID extraPropertiesStructureID;
    uint32_t shapeWord { 0 };
    uint32_t orderAndCount { 0 };
    Vector<PropertyListEntry> properties;
    JSCellButterfly* keys { nullptr };
    JSPropertyNameEnumerator* enumerator { nullptr };
    StructureChain* prototypeChain { nullptr };
    Vector<Latin1Character> jsonKeys;
    Vector<uint32_t> endsOfJSONKeys;
    bool hasToJSON { false };
    bool holdsOnlyPlainData { false };
};

struct KeptOwnKeys {
    bool isFor(StructureID id, uint32_t word, bool symbolsToo) const { return structureID == id && orderWord == word && listsSymbols == symbolsToo; }

    StructureID structureID;
    uint32_t orderWord { 0 };
    bool listsSymbols { false };
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
    bool isSecondWalk(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount);
    KeptOwnKeys& ownKeysFor(StructureID, uint32_t orderWord, bool listsSymbols);
    void clear();

private:
    using Entries = std::array<CachedPropertyList, numberOfEntries>;
    using OwnKeyLists = std::array<KeptOwnKeys, numberOfOwnKeyLists>;

    static unsigned placeOf(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount) { return ((extraPropertiesStructureID.bits() >> 4) + shapeWord + orderAndCount * 31) % numberOfEntries; }

    static constexpr unsigned numberOfWalksRemembered = 8 * numberOfEntries;

    WTF::BitSet<numberOfWalksRemembered> m_walksWithoutList;
    unsigned m_numberOfWalksWithoutList { 0 };
    MadeInArena<Entries> m_entries;
    MadeInArena<OwnKeyLists> m_ownKeyLists;
    unsigned m_numberOfOwnKeyListsReplaced { 0 };
    RememberedSlotSet m_writes;
};

CachedPropertyList* cachedPropertyList(VM&, const Object&);
CachedPropertyList* findCachedPropertyList(const Object&, JSObject* extraProperties);
void writeJSONKeys(VM&, CachedPropertyList&);
JSCellButterfly* findKeptOwnKeys(const Object&, bool listsSymbols);
void didListOwnKeys(VM&, const Object&, bool listsSymbols, JSArray* list);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

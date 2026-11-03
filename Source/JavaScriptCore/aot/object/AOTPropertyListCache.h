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
#include <memory>
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
    StructureID extraPropertiesStructureID;
    uint32_t shapeWord { 0 };
    uint32_t orderAndCount { 0 };
    JSCellButterfly* keys { nullptr };
    Vector<PropertyListEntry> properties;
    JSPropertyNameEnumerator* enumerator { nullptr };
    StructureChain* prototypeChain { nullptr };
    // Written when JSON.stringify first asks for the list, with holdsOnlyPlainData: most lists serve keys, copies and loops alone.
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

    CachedPropertyList& entryFor(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount);
    // The place of that list, or the place that it would take.
    KeptOwnKeys& ownKeysFor(StructureID, uint32_t orderWord, bool listsSymbols);
    void clear();

private:
    using Entries = std::array<CachedPropertyList, numberOfEntries>;
    using OwnKeyLists = std::array<KeptOwnKeys, numberOfOwnKeyLists>;

    State& m_state;
    MadeInArena<Entries> m_entries;
    MadeInArena<OwnKeyLists> m_ownKeyLists;
    unsigned m_numberOfOwnKeyListsReplaced { 0 };
    RememberedSlotSet m_writes;
};

CachedPropertyList* cachedPropertyList(VM&, const Object&);
void writeJSONKeys(VM&, CachedPropertyList&);
CachedPropertyList* findCachedPropertyList(VM&, const Object&, JSObject* extraProperties);
// For an object with extra properties whose Structure names theirs. The list that is kept, or null.
JSCellButterfly* findKeptOwnKeys(const Object&, bool listsSymbols);
// The list was made for the object just now. It is kept if the same was asked for before.
void didListOwnKeys(VM&, const Object&, bool listsSymbols, JSArray* list);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

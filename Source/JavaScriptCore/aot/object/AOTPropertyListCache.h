/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyList.h"
#include "AOTVector.h"
#include "RememberedSlot.h"
#include "StructureID.h"
#include <array>
#include <memory>
#include <wtf/Noncopyable.h>
#include <wtf/text/Latin1Character.h>

namespace JSC {

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
    Vector<Latin1Character> jsonKeys;
    Vector<uint32_t> endsOfJSONKeys;
    bool hasToJSON { false };
    bool holdsOnlyPlainData { false };
};

class PropertyListCache {
    WTF_MAKE_NONCOPYABLE(PropertyListCache);
public:
    explicit PropertyListCache(State&);

    static constexpr unsigned numberOfEntries = 256;

    CachedPropertyList& entryFor(StructureID extraPropertiesStructureID, uint32_t shapeWord, uint32_t orderAndCount);
    void clear();

private:
    using Entries = std::array<CachedPropertyList, numberOfEntries>;

    State& m_state;
    std::unique_ptr<Entries> m_entries;
    RememberedSlotSet m_writes;
};

CachedPropertyList* cachedPropertyList(VM&, const Object&);
CachedPropertyList* findCachedPropertyList(VM&, const Object&, JSObject* extraProperties);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

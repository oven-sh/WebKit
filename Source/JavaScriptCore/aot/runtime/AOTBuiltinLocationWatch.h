/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTWatchpoints.h"
#include "Identifier.h"
#include "JSType.h"
#include "PropertyOffset.h"
#include "PropertySlot.h"
#include "StructureID.h"
#include <array>
#include <span>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT {

struct Instance;
struct State;

// The byte of a member in BuiltinWords::locationStates.
enum class WatchedLocationState : uint8_t { NotArmed, Unchanged, Changed };
static_assert(!static_cast<uint8_t>(WatchedLocationState::NotArmed));

// In BuiltinWords::ownSlotStructureIDs, for an own slot whose Structure was looked for and does not hold the property where compiled code reads it. No cell has this number.
inline constexpr uint32_t structureIDOfNoInstance = StructureID::nukedStructureIDBit;

// The table counts the properties that the constructor defines. Only a plain object has room for properties inside its cell.
constexpr PropertyOffset propertyOffsetOfOwnSlot(JSType typeOfInstances, uint8_t positionAmongOwnSlots)
{
    return typeOfInstances == FinalObjectType ? positionAmongOwnSlots : firstOutOfLineOffset + positionAmongOwnSlots;
}

// Answers at run time for the members of built-in objects that only a write replaces, where the compiler could not rule the write out.
class BuiltinLocationWatch {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BuiltinLocationWatch);
    WTF_MAKE_NONCOPYABLE(BuiltinLocationWatch);
public:
    explicit BuiltinLocationWatch(State&);

    WatchedLocationState stateOf(BuiltinID member) const;
    unsigned numberOfWatches() const { return m_watches.size(); }

    // Whether the holder still has the member that start-up found.
    bool arm(VM&, BuiltinID member);
    void didCreateMember(JSObject* holder, UniquedStringImpl* key, JSObject* member);

    // The native function behind a getter that has no function object. Null without the host that binds it.
    GetValueFunc::Ptr customGetterOf(BuiltinOverloadID);

private:
    struct CustomGetter {
        BuiltinOverloadID overload;
        GetValueFunc::Ptr function { nullptr };
    };

    // NotArmed: the host has not made the member yet.
    WatchedLocationState tryWatch(VM&, BuiltinID member, WatchedLocationState&);

    State& m_state;
    std::span<WatchedLocationState> m_states;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<PropertyValueWatchpoint>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_watches;
    // The holders, the keys and the objects are kept alive for as long as the instance lives: a static table has a few hundred entries at the most.
    using Location = std::pair<JSObject*, UniquedStringImpl*>;
    UncheckedKeyHashMap<Location, JSObject*> m_objectsMadeByHost;
    // A location and the condition of a watchpoint name a key without keeping it alive.
    WTF::Vector<Identifier> m_keys;
    // The bindings are a sorted list. Each place holds the getter that was asked for last among those that share it.
    std::array<CustomGetter, 64> m_customGetters;
};

// Finds the Structure that the constructor leaves an instance with once it has defined the properties up to the own slot, and gives compiled code its number.
void publishStructureOfOwnSlot(Instance&, BuiltinOverloadID);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

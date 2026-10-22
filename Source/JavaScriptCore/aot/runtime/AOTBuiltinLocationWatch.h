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
#include <memory>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/HashFunctions.h>
#include <wtf/HashMap.h>
#include <wtf/HashTraits.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC {

class JSCell;
class JSObject;
class VM;

namespace AOT {

class BuiltinCellKey;
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
    ~BuiltinLocationWatch();

    WatchedLocationState stateOf(BuiltinID member) const;
    unsigned numberOfWatches() const { return m_watches.size(); }

    // Whether the holder still has the member that start-up found.
    bool arm(VM&, BuiltinID member);
    void didCreateMember(JSObject* holder, UniquedStringImpl* key, JSObject* member);

    // The native function behind a getter that has no function object. Null without the host that binds it.
    GetValueFunc::Ptr customGetterOf(BuiltinOverloadID);

private:
    struct Watch {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Watch);

        Watch(const Identifier& name, WatchedLocationState& state)
            : name(name)
            , state(state)
        {
        }

        PropertyValueWatchpoint watchpoint;
        // The condition of the watchpoint names the key without keeping it alive.
        Identifier name;
        WatchedLocationState& state;
    };

    struct CustomGetter {
        BuiltinOverloadID overload;
        GetValueFunc::Ptr function { nullptr };
    };

    static void didFire(VM&, void* watch);

    enum class Attempt : uint8_t { Watches, CannotWatch, IsNotMadeYet };
    Attempt tryWatch(VM&, BuiltinID member, WatchedLocationState&);
    JSObject* findHolder(BuiltinID member);
    JSCell* cellFilledBeforeProgramRan(BuiltinCellKey) const;
    bool isWhatStartUpFound(BuiltinID member, JSValue held) const;

    State& m_state;
    std::span<WatchedLocationState> m_states;
    UncheckedKeyHashMap<uint32_t, std::unique_ptr<Watch>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_watches;
    // The holders, the keys and the objects are kept alive for as long as the instance lives: a static table has a few hundred entries at the most.
    using Location = std::pair<JSObject*, UniquedStringImpl*>;
    UncheckedKeyHashMap<Location, JSObject*> m_objectsMadeByHost;
    Vector<Identifier> m_keysOfObjectsMadeByHost;
    // The bindings are a sorted list. Each place holds the getter that was asked for last among those that share it.
    std::array<CustomGetter, 64> m_customGetters;
};

// Finds the Structure that the constructor leaves an instance with once it has defined the properties up to the own slot, and gives compiled code its number.
void publishStructureOfOwnSlot(Instance&, BuiltinOverloadID);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

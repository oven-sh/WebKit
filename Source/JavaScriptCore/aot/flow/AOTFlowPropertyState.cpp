/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyState.h"

#if ENABLE(AOT)

#include "AOTFlowBuiltinLocations.h"
#include "AOTFlowCallOperations.h"
#include "AOTFlowParts.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static Vector<PropertyKeyID, 2> collectOwnKeysOfLists(const Program& program)
{
    const BuiltinTable& builtins = program.builtins();
    Vector<PropertyKeyID, 2> keys;
    for (unsigned index = 0; index < builtins.classCount(); ++index) {
        BuiltinClassID builtinClass(index);
        BuiltinID prototype = builtins.at(builtinClass).prototype;
        if (!prototype || !indexedElementSlotOf(builtins, builtinClass))
            continue;

        for (BuiltinID member : builtins.membersOf(prototype)) {
            const Builtin& entry = builtins.at(member);
            if (!entry.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
                continue;
            PropertyKeyID key = program.keyOf(entry.name);
            if (!keys.contains(key))
                keys.append(key);
        }
    }
    return keys;
}

// A store under a key nobody can name already reaches every property of its receivers.
static bool doesMoreThanStore(BuiltinPropertyActionKind kind)
{
    switch (kind) {
    case BuiltinPropertyActionKind::ReadsByKey:
    case BuiltinPropertyActionKind::StoresByKey:
    case BuiltinPropertyActionKind::DefinesName:
    case BuiltinPropertyActionKind::ReadsGetter:
    case BuiltinPropertyActionKind::ReadsSetter:
    case BuiltinPropertyActionKind::TestsByKey:
    case BuiltinPropertyActionKind::DescribesOwnProperties:
        return false;
    case BuiltinPropertyActionKind::DeletesByKey:
    case BuiltinPropertyActionKind::AssignsFrom:
    case BuiltinPropertyActionKind::TakesDescriptor:
    case BuiltinPropertyActionKind::TakesDescriptorMap:
    case BuiltinPropertyActionKind::ChangesIntegrity:
    case BuiltinPropertyActionKind::ChangesPrototype:
    case BuiltinPropertyActionKind::BecomesPrototype:
    case BuiltinPropertyActionKind::DefinesGetter:
    case BuiltinPropertyActionKind::DefinesSetter:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool changesReceiver(const BuiltinOverload& overload)
{
    return overload.mode == BuiltinCallMode::Set && std::ranges::any_of(overload.flow.propertyActions, [](const BuiltinPropertyAction& action) {
        return action.object.isReceiver() && doesMoreThanStore(action.kind);
    });
}

static FlowMembersOfBuiltins collectSettersChangingReceivers(const BuiltinTable& builtins)
{
    FlowMembersOfBuiltins setters;
    BitVector visitedPrototypes;
    for (unsigned index = 0; index < builtins.classCount(); ++index) {
        BuiltinID prototype = builtins.at(BuiltinClassID(index)).prototype;
        if (!prototype)
            continue;
        prototype = builtins.canonicalID(prototype);
        if (visitedPrototypes.set(prototype.index()))
            continue;

        for (BuiltinID member : builtins.membersOf(prototype)) {
            if (std::ranges::any_of(builtins.overloadsOf(member), changesReceiver))
                setters.append(FlowMemberOfBuiltin { prototype, member });
        }
    }
    return setters;
}

static Vector<FlowMemberOfBuiltin> collectMembersOutsidePrototypes(const BuiltinTable& builtins)
{
    Vector<FlowMemberOfBuiltin> members;
    for (unsigned index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID holder(index);
        const Builtin& entry = builtins.at(holder);
        if (entry.kind == BuiltinKind::Prototype || entry.scope != BuiltinScope::Realm || entry.sameObjectAs)
            continue;
        // All that Function and its like have of their own is a name, a length and a prototype, and the unknown value answers for those.
        if (entry.kind == BuiltinKind::Constructor && letsInUnseenCode(builtins, holder))
            continue;
        for (BuiltinID member : builtins.membersOf(holder))
            members.append(FlowMemberOfBuiltin { holder, member });
    }
    std::ranges::stable_sort(members, { }, [&](const FlowMemberOfBuiltin& found) {
        return builtins.at(found.member).name.index();
    });
    return members;
}

// What the language defines and the table leaves unlocked is what real packages replace, and a polyfill writes through a helper that is handed the object and the key
// (`defineBuiltIn(O, key, value)` of core-js). Such a write must succeed, so it counts for these members like a write to the holder itself.
static BitVector collectMembersLockedWhileConstant(const BuildContext& build, const BuiltinTable& builtins)
{
    BitVector members(builtins.builtinCount());
    for (unsigned index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID member(index);
        if (builtins.at(member).origin == BuiltinOrigin::Engine)
            continue;
        if (canLockAtStartUp(builtins, member) && build.isEnabled(FlowResultKind::ConstantBuiltinLocations, index))
            members.quickSet(index);
    }
    return members;
}

std::unique_ptr<FlowPropertyState, FlowPropertyStateDeleter> createFlowPropertyState(FlowSolver& solver)
{
    const Program& program = solver.program();

    std::unique_ptr<FlowPropertyState, FlowPropertyStateDeleter> state(new FlowPropertyState);
    state->keys.prototype = program.keyOf(BuiltinNames::prototype);
    state->keys.constructor = program.keyOf(BuiltinNames::constructor);
    state->keys.value = program.keyOf(BuiltinNames::value);
    state->keys.get = program.keyOf(BuiltinNames::get);
    state->keys.set = program.keyOf(BuiltinNames::set);
    state->keys.writable = program.keyOf(BuiltinNames::writable);
    state->keys.enumerable = program.keyOf(BuiltinNames::enumerable);
    state->keys.configurable = program.keyOf(BuiltinNames::configurable);
    state->ownKeysOfLists = collectOwnKeysOfLists(program);
    state->membersOutsidePrototypes = collectMembersOutsidePrototypes(program.builtins());
    state->settersChangingReceivers = collectSettersChangingReceivers(program.builtins());
    state->membersLockedWhileConstant = collectMembersLockedWhileConstant(solver.build(), program.builtins());
    state->objectOrNull = solver.types().unionOf(TypeTable::object, TypeTable::null);
    state->symbols = createFlowSymbolState(program);
    state->constructions = createFlowConstructionState(solver);
    return state;
}

void FlowPropertyStateDeleter::operator()(FlowPropertyState* state) const
{
    delete state;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

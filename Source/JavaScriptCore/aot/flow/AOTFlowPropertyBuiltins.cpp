/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowPropertyLocations.h"

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTFlowCallOperations.h"
#include "AOTFlowPropertyState.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool listsAllMembers(const FlowWalkInputs& inputs, const FlowHolder& holder)
{
    return inputs.builtins().at(holder.builtin).flags.contains(BuiltinFlag::ListsAllMembers);
}

static FlowOwnProperty createOwnProperty(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID key, BuiltinID member)
{
    FlowOwnProperty property;
    property.key = key;
    property.builtin = member;

    const FlowProperty* entry = holder.object ? inputs.graph.findProperty(holder.object, key) : nullptr;
    if (entry) {
        property.value = entry->value;
        property.getter = entry->getter;
        property.setter = entry->setter;
    }

    if (member) {
        property.lookup = FlowOwnLookup::Found;
        property.presence = FlowOwnPresence::ShownByValue;
        return property;
    }

    // A store by the program does not say what the host has under a name the table does not list.
    if (!listsEveryMemberUnder(inputs.program, holder.builtin, key)) {
        property.lookup = FlowOwnLookup::Unknown;
        return property;
    }

    property.lookup = entry ? FlowOwnLookup::Found : FlowOwnLookup::Absent;
    return property;
}

FlowOwnProperty findOwnBuiltinProperty(const FlowWalkInputs& inputs, const FlowHolder& holder, PropertyKeyID key)
{
    RELEASE_ASSERT(holder.builtin);
    return createOwnProperty(inputs, holder, key, findOwnMember(inputs.program, holder.builtin, key));
}

FlowOwnListing forEachOwnBuiltinProperty(const FlowWalkInputs& inputs, const FlowHolder& holder, NOESCAPE const FlowOwnPropertyFunctor& functor)
{
    RELEASE_ASSERT(holder.builtin);
    const BuiltinTable& builtins = inputs.builtins();

    Vector<FlowOwnProperty, 16> properties;
    auto appendMember = [&](BuiltinID member) {
        properties.append(createOwnProperty(inputs, holder, inputs.program.keyOf(builtins.at(member).name), member));
    };

    for (BuiltinID member : builtins.membersOf(holder.builtin))
        appendMember(member);

    // The global object is a member of itself, which membersOf() does not list.
    BuiltinName holderName = builtins.at(holder.builtin).name;
    if (holderName && builtins.findOwnMember(holder.builtin, holderName) == holder.builtin)
        appendMember(holder.builtin);

    if (holder.object) {
        for (const FlowProperty& entry : inputs.graph.propertiesOf(holder.object)) {
            if (!findOwnMember(inputs.program, holder.builtin, entry.key))
                properties.append(createOwnProperty(inputs, holder, entry.key, BuiltinID()));
        }
    }

    for (const FlowOwnProperty& property : properties)
        functor(property);
    return listsAllMembers(inputs, holder) ? FlowOwnListing::Complete : FlowOwnListing::MayHaveOthers;
}

void noteWritesToMembersOf(FlowSolver& solver, FlowObjectID holder, const Witness& witness)
{
    const BuiltinTable& builtins = solver.builtins();
    BuiltinID builtin = holderOf(solver.objects(), holder).builtin;
    if (!builtin)
        return;

    BitVector& writtenBuiltins = solver.propertyState().builtinsWrittenUnderUnnamedKeys;
    size_t index = builtins.canonicalID(builtin).index();
    bool hasNewWrite = !writtenBuiltins.get(index);
    writtenBuiltins.set(index);
    for (BuiltinID member : builtins.membersOf(builtin)) {
        if (builtins.at(member).mutability == BuiltinMutability::AlwaysLocked || solver.records().builtinUse(member).uses.contains(BuiltinUse::Written))
            continue;
        noteBuiltinUse(solver, member, BuiltinUse::Written, witness);
        hasNewWrite = true;
    }
    // A lookup that took one of the members for a constant waits on the mutations of the holder, and the holder may have all of them.
    if (hasNewWrite)
        solver.revisitMutationWaiters(holder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

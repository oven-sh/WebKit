/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinMembers.h"
#include "AOTFlowResults.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

inline bool isLockedLocation(const BuiltinTable& builtins, BuiltinID member)
{
    return builtins.at(member).mutability == BuiltinMutability::AlwaysLocked || builtins.isLocationOfLockedObject(member);
}

// A member that is constant unless written and that start-up finds by its path from the global object.
inline bool canLockAtStartUp(const BuiltinTable& builtins, BuiltinID member)
{
    const Builtin& entry = builtins.at(member);
    if (!entry.holder || !entry.name || entry.mutability != BuiltinMutability::ConstantUnlessWritten || isLockedLocation(builtins, member))
        return false;
    // A realm has no object of a built-in that exists once per module, and start-up finds a holder by its path from the global object.
    return entry.scope == BuiltinScope::Realm && builtins.isUnder(member, Builtins::GlobalThis);
}

inline Vector<BuiltinID> constantBuiltinMembersToLock(const Program& program, const FlowResults& flow)
{
    const BuiltinTable& builtins = program.builtins();
    Vector<BuiltinID> members;
    for (const BuiltinLocationFlow& location : flow.builtinLocations()) {
        if (location.state != BuiltinLocationState::Constant)
            continue;
        BuiltinID member = findOwnMember(program, location.holder, location.key);
        if (member && canLockAtStartUp(builtins, member))
            members.append(member);
    }
    AOT::sort(members, { }, [&](BuiltinID member) {
        return std::pair { builtins.at(member).holder, member };
    });
    return members;
}

// The property that holds the built-in when the realm is created may hold something else later.
inline bool mayBeReplaced(const Program& program, const FlowResults& flow, BuiltinID builtin)
{
    const BuiltinTable& builtins = program.builtins();
    const Builtin& entry = builtins.at(builtin);
    if (!entry.holder || !entry.name || isLockedLocation(builtins, builtin))
        return false;
    // No object stands for the holder of a compiler helper, so no store reaches the helper.
    if (entry.flags.contains(BuiltinFlag::IsCompilerHelper))
        return false;
    return flow.builtinLocation(builtin) != BuiltinLocationState::Constant;
}

inline bool isWritten(const BuiltinLocationFlow& record)
{
    return hasFunction(record.witness);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

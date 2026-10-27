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

inline bool canLockAtStartUp(const BuiltinTable& builtins, BuiltinID member)
{
    const Builtin& entry = builtins.at(member);
    if (!entry.holder || !entry.name || entry.mutability != BuiltinMutability::ConstantUnlessWritten || isLockedLocation(builtins, member))
        return false;
    return entry.scope == BuiltinScope::Realm && builtins.isUnder(member, Builtins::GlobalThis);
}

inline Vector<BuiltinID> constantBuiltinMembersToLock(const Program& program, const FlowResults& flow)
{
    const BuiltinTable& builtins = program.builtins();
    Vector<BuiltinID> members;
    for (size_t index : flow.builtinMembersLockedAtStartUp()) {
        // A default export is no property of its holder: the loader hands it out.
        BuiltinID member(static_cast<uint32_t>(index));
        if (findOwnMember(program, builtins.at(member).holder, program.keyOf(builtins.at(member).name)) == member)
            members.append(member);
    }
    AOT::sort(members, { }, [&](BuiltinID member) {
        return std::pair { builtins.at(member).holder, member };
    });
    return members;
}

inline bool mayBeReplaced(const Program& program, const FlowResults& flow, BuiltinID builtin)
{
    const BuiltinTable& builtins = program.builtins();
    const Builtin& entry = builtins.at(builtin);
    if (!entry.holder || !entry.name || isLockedLocation(builtins, builtin))
        return false;
    if (entry.flags.contains(BuiltinFlag::IsCompilerHelper))
        return false;
    return flow.builtinLocation(builtin) != BuiltinLocationState::Constant;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

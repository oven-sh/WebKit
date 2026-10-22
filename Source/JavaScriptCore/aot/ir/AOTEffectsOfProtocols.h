/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffects.h"
#include "AOTType.h"
#include "AOTTypeTable.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct Type;

struct CalleeList {
    Vector<FunctionID, 2> functions;
    Vector<BuiltinID, 2> builtins;
    bool isComplete { false };

    static CalleeList none()
    {
        CalleeList list;
        list.isComplete = true;
        return list;
    }

    static CalleeList unknown() { return { }; }

    bool runsNothing() const { return isComplete && functions.isEmpty() && builtins.isEmpty(); }
    bool mayRunUserCode() const { return !isComplete || !functions.isEmpty(); }

    void add(FunctionID);
    void add(BuiltinID);
    void merge(const CalleeList&);

    void dump(PrintStream&) const;

    friend bool operator==(const CalleeList&, const CalleeList&) = default;
};

// What a call by value hands to the entrypoints of its callees. Without a list of arguments: any number of any values.
struct EnteringOperands {
    TypeID thisValue { TypeTable::anything };
    std::optional<std::span<const TypeID>> arguments;
    // Of a construction. Null where new.target is the callee itself.
    TypeID newTarget { TypeTable::anything };
};

CalleeList calleesOf(const Program&, const Type& callee);
CalleeList calleesOf(const Program&, CalleeSetID);

// `Named` is a [[Get]] of namedMembers.
enum class NamedMembers : uint8_t { AreOnlyInspected, AreOnlyRead };

// `CallsNamed` calls what it reads under calledMembers, with the operand as `this`. An empty list stands for every member.
CalleeList membersRunBy(const Program&, const Type& operand, Protocols, std::span<const BuiltinName> namedMembers, std::span<const BuiltinName> calledMembers, NamedMembers);

// What the protocols that look a member up read in an object and in the objects it inherits from, whether or not they find code to run.
Effects readsOfMemberLookups(const Type& operand, Protocols);

bool protocolMayRunUserCode(const Program&, TypeID operandType, Protocol);

// False where the type proves that each lookup of the protocol ends without a member: no field, method, accessor or built-in under any of its keys.
bool protocolMayFindMember(const Program&, const Type& operand, Protocol);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

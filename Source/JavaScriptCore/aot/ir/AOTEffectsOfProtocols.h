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
#include "AOTVector.h"
#include <optional>
#include <span>

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

struct EnteringOperands {
    TypeID thisValue { TypeTable::anything };
    std::optional<std::span<const TypeID>> arguments;
    TypeID newTarget { TypeTable::anything };
};

CalleeList calleesOf(const Program&, const Type& callee);
CalleeList calleesOf(const Program&, CalleeSetID);

enum class NamedMembers : uint8_t { AreOnlyInspected, AreOnlyRead };

CalleeList membersRunBy(const Program&, const Type& operand, Protocols, std::span<const BuiltinName> namedMembers, std::span<const BuiltinName> calledMembers, NamedMembers);

Effects readsOfMemberLookups(const Type& operand, Protocols);

bool protocolMayFindMember(const Program&, const Type& operand, Protocol);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

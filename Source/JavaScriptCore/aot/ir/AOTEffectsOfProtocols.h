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

    TypeID argumentAt(size_t index) const
    {
        if (!arguments)
            return TypeTable::anything;
        return index < arguments->size() ? (*arguments)[index] : TypeTable::undefined;
    }
};

// A value that enters a declared position meets two tests, each as far as a test decides its type. The first is for the type the source wrote:
// where it fails the program gets the TypeError it was promised, and the message names that type. The second is for what flow narrowed the
// position to. Flow has seen every value that passes the first, so the second fails only for a caller flow has not seen.
// The passes that emit the tests and the effects of a call by value ask here.
Type testedWrittenTypeOf(const Type& written);
Type testedProvenTypeOf(const Type&);
bool mayFailEntryTests(const Program&, const Type& value, const DeclaredType&);

CalleeList calleesOf(const Program&, const Type& callee);
CalleeList calleesOf(const Program&, CalleeSetID);

// `Named` is a [[Get]] of namedMembers. What it reads is converted where the operand also runs `ToPrimitive`, unless the parameter says
// that its built-in only looks at what the members hold.
enum class NamedMembers : uint8_t { AreOnlyInspected, AreOnlyRead };

// `CallsNamed` calls what it reads under calledMembers, with the operand as `this`. An empty list stands for every member.
CalleeList membersRunBy(const Program&, const Type& operand, Protocols, std::span<const BuiltinName> namedMembers, std::span<const BuiltinName> calledMembers, NamedMembers);

// What the protocols that look a member up read in an object and in the objects it inherits from, whether or not they find code to run.
// A member of a primitive is found on a built-in prototype: it is proven to stay what it is, or the lookup is unknown code.
// What a protocol without a key reads is declared by the entry that runs it.
Effects readsOfMemberLookups(const Type& operand, Protocols);

bool protocolMayRunUserCode(const Program&, TypeID operandType, Protocol);

// False where the type proves that each lookup of the protocol ends without a member: no field, method, accessor or built-in under any of its keys.
bool protocolMayFindMember(const Program&, const Type& operand, Protocol);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

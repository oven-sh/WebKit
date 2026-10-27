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

// What an operation may run. Complete: it runs nothing else. Open: these are known of, beside whatever else it runs.
class CalleeList {
public:
    static CalleeList none()
    {
        CalleeList list;
        list.m_isComplete = true;
        return list;
    }

    static CalleeList unknown() { return { }; }

    // The lists are sorted and name nothing twice.
    static CalleeList complete(const Callees& callees) { return CalleeList(callees, true); }
    static CalleeList open(const Callees& callees) { return CalleeList(callees, false); }

    // All that is run.
    std::optional<ProvenValue<Callees>> all() const LIFETIME_BOUND
    {
        return m_isComplete ? std::optional(Proven::byClosedSet(listed())) : std::nullopt;
    }
    // All that is run, where no built-in is among it.
    std::optional<ProvenValue<std::span<const FunctionID>>> functionsAlone() const LIFETIME_BOUND
    {
        return m_isComplete && m_builtins.isEmpty() ? std::optional(Proven::byClosedSet(m_functions.span())) : std::nullopt;
    }
    // What is likely to run.
    Expected<Callees> seen() const LIFETIME_BOUND { return listed(); }
    // For whoever adds up what may happen: each of these may run, and where the list is open so may anything else.
    Callees listed() const LIFETIME_BOUND { return { m_functions.span(), m_builtins.span() }; }

    bool listsEveryCallee() const { return m_isComplete; }
    bool runsNothing() const { return m_isComplete && m_functions.isEmpty() && m_builtins.isEmpty(); }
    bool mayRunUserCode() const { return !m_isComplete || !m_functions.isEmpty(); }

    void add(FunctionID);
    void add(BuiltinID);
    void addUnknownCallees() { m_isComplete = false; }
    void remove(FunctionID function) { m_functions.removeFirst(function); }
    void removeBuiltins() { m_builtins.clear(); }

    void dump(PrintStream&) const;

    friend bool operator==(const CalleeList&, const CalleeList&) = default;

private:
    CalleeList() = default;
    CalleeList(const Callees& callees, bool isComplete)
        : m_functions(callees.functions)
        , m_builtins(callees.builtins)
        , m_isComplete(isComplete)
    {
    }

    Vector<FunctionID, 2> m_functions;
    Vector<BuiltinID, 2> m_builtins;
    bool m_isComplete { false };
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

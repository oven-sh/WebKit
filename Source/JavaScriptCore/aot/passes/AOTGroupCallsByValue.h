/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTSortedSet.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

using CallGroupID = ID<struct CallGroupIDTag>;

enum class GenericCallers : uint8_t { Unknown, None };

enum class GroupEligibility : uint8_t {
    Unknown,
    TakesArgumentList,
    TakesNewTarget,
    IsConstructor,
    HasSeveralResults,
    CannotBeCalledByValue,
    HasNoBody,
    Eligible,
};

struct CallGroupPosition {
    SortedSet<TypeID, 2> types;
    bool isOmittedAtSomeSite { false };
};

struct CallGroup {
    Vector<FunctionID> members;
    Vector<AccessSiteID> sites;
    CallGroupPosition thisValue;
    Vector<CallGroupPosition> arguments;
    bool someMemberTakesThis { false };
};

class CallGroups {
public:
    CallGroups() = default;

    unsigned numberOfGroups() const { return m_groups.size(); }
    const CallGroup& at(CallGroupID id) const LIFETIME_BOUND { return m_groups[id.index()]; }

    CallGroupID groupOf(FunctionID member) const { return member.index() < m_groupOfFunction.size() ? m_groupOfFunction[member.index()] : CallGroupID(); }
    CallGroupID groupOf(AccessSiteID site) const { return site && site.index() < m_groupOfSite.size() ? m_groupOfSite[site.index()] : CallGroupID(); }
    GenericCallers genericCallersOf(FunctionID function) const { return function.index() < m_genericCallers.size() ? m_genericCallers[function.index()] : GenericCallers::Unknown; }
    // Every grouped site that reaches the member is a tail call that has to reuse its frame. The member leaves the test of its written result to its callers, and an entrypoint
    // that stayed behind to make that test would be a frame per call: whoever receives the result of the chain of tail calls makes the test.
    bool leavesResultTestToReceiver(FunctionID member) const { return m_leavesResultTestToReceiver.get(member.index()); }

    void dump(PrintStream&, const Program&) const;

private:
    friend CallGroups groupCallsByValue(const Program&, const FlowResults&, BuildContext&);

    Vector<CallGroup> m_groups;
    Vector<CallGroupID> m_groupOfFunction;
    Vector<CallGroupID> m_groupOfSite;
    Vector<GenericCallers> m_genericCallers;
    BitVector m_leavesResultTestToReceiver;
};

CallGroups groupCallsByValue(const Program&, const FlowResults&, BuildContext&);

GroupEligibility groupEligibilityOf(const Program&, FunctionID);

// The functions whose closure exists without a NewClosure: a read of the variable that starts with the function creates it.
BitVector findFunctionsReadFromVariables(const Program&, const FlowResults&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

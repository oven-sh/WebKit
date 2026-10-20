/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGroupCallsByValue.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDiagnostics.h"
#include "AOTFlowResults.h"
#include "AOTIRPrinter.h"
#include "AOTResolveCallsCallees.h"
#include "AOTSlotPlan.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ScannedCall {
    FunctionID caller;
    ValueID call;
    AccessSiteID site;
    FunctionList members;
    bool isRequiredTailCall { false };
};

struct GroupNode : WTF::UnionFind<GroupNode> { };

} // anonymous namespace

GroupEligibility groupEligibilityOf(const Program& program, FunctionID id)
{
    if (!id)
        return GroupEligibility::Unknown;

    const Function& function = program.at(id);
    if (!function.hasBody())
        return GroupEligibility::HasNoBody;
    if (!canBeCalledByValueOf(function.role))
        return GroupEligibility::CannotBeCalledByValue;
    // SplitCoroutines gives a derived version of a coroutine the role of a ramp, and no closure names a derived version.
    if (function.role == FunctionRole::CoroutineRamp && function.derivation.source)
        return GroupEligibility::CannotBeCalledByValue;

    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        break;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return GroupEligibility::IsConstructor;
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return GroupEligibility::CannotBeCalledByValue;
    }

    if (function.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return GroupEligibility::TakesArgumentList;
    if (function.has(FunctionFlags::HasNewTarget))
        return GroupEligibility::TakesNewTarget;
    if (function.results.size() != 1)
        return GroupEligibility::HasSeveralResults;
    return GroupEligibility::Eligible;
}

static bool mayBeReadWithItsFunction(const Variable& variable)
{
    switch (variable.flow.reads) {
    case ReadState::Unknown:
        return true;
    case ReadState::OnlyCalled:
        // A call cannot name its callee while another value may be stored, so it reads the variable.
        return variable.flow.stores != StoreCount::Never;
    case ReadState::NeverRead:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BitVector findFunctionsReadFromVariables(const Program& program, const FlowResults& flow)
{
    BitVector described = findVariablesDescribedToRunTime(program, flow);
    BitVector functions(program.functions.size());
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        const Variable& variable = program.variables[index];
        if (variable.function && (described.quickGet(index) || mayBeReadWithItsFunction(variable)))
            functions.quickSet(variable.function.index());
    }
    return functions;
}

static FunctionList findGroupMembers(const Program& program, const FlowResults& flow, const Function& caller, const Value& call, uint64_t maxDispatchCases)
{
    if (call.opcode != Opcode::Call || !call.accessSite() || call.expectedResult())
        return { };

    CalleeSetID stamp = call.callees();
    CalleeCandidates candidates = calleeCandidates(program, flow, caller, call.calleeOperand(), stamp, call.accessSite());
    if (!candidates.isProven() || candidates.hasOtherCallees)
        return { };
    if (candidates.proven.size() <= maxDispatchCases)
        return { };

    bool canAllJoin = std::ranges::all_of(candidates.proven, [&](FunctionID member) {
        return groupEligibilityOf(program, member) == GroupEligibility::Eligible && versionToEnter(program, flow, member, stamp) == member;
    });
    if (!canAllJoin)
        return { };
    return WTF::move(candidates.proven);
}

static bool isRequiredTailCall(const Terminator& terminator, ValueID call)
{
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == call && terminator.requirement() == TailCallRequirement::Required;
}

static Vector<ScannedCall> scanCalls(const Program& program, const FlowResults& flow, const BuildContext& build)
{
    uint64_t maxDispatchCases = build.limit(LimitID::MaxDispatchCases);
    return collectInParallel<ScannedCall>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<ScannedCall>& calls) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            return;

        FunctionID id(index);
        FunctionID asRead = findFunctionAsRead(&program, id);
        bool isSwitchedOn = build.isEnabled(PassID::GroupCallsByValue, asRead) && build.isEnabled(PassID::ChooseCallingConventions, asRead);
        for (const Block& block : function.blocks) {
            for (ValueID valueID : block.values) {
                const Value& value = function.at(valueID);
                if (accessClassOf(value.opcode) != AccessClass::IndirectCall || !Value::opcodesWithCallees.contains(value.opcode))
                    continue;
                FunctionList members = isSwitchedOn ? findGroupMembers(program, flow, function, value, maxDispatchCases) : FunctionList();
                calls.append(ScannedCall { id, valueID, value.accessSite(), WTF::move(members), isRequiredTailCall(block.terminator, valueID) });
            }
        }
    });
}

static unsigned computeNumberOfSites(std::span<const ScannedCall> calls)
{
    unsigned numberOfSites = 0;
    for (const ScannedCall& call : calls) {
        if (call.site)
            numberOfSites = std::max(numberOfSites, call.site.index() + 1);
    }
    return numberOfSites;
}

// The versions of a function share their access sites, and ChooseCallingConventions converts every copy of a grouped site.
static void ungroupMixedSites(Vector<ScannedCall>& calls, unsigned numberOfSites)
{
    BitVector hasGenericCopy(numberOfSites);
    for (const ScannedCall& call : calls) {
        if (call.members.isEmpty() && call.site)
            hasGenericCopy.set(call.site.index());
    }
    for (ScannedCall& call : calls) {
        if (!call.members.isEmpty() && hasGenericCopy.get(call.site.index()))
            call.members.clear();
    }
}

// A member has one group entrypoint, which either tests the result or returns with a tail call. The tail calls that have to reuse their frame keep the entrypoint.
// A site that needs the test calls by value: the generic entrypoint makes it.
static BitVector separateSitesThatNeedResultTests(const Program& program, const FlowResults& flow, Vector<ScannedCall>& calls, unsigned numberOfSites)
{
    BitVector hasOtherCopy(numberOfSites);
    for (const ScannedCall& call : calls) {
        if (!call.members.isEmpty() && !call.isRequiredTailCall)
            hasOtherCopy.set(call.site.index());
    }

    BitVector leavesResultTestToReceiver(program.functions.size());
    for (const ScannedCall& call : calls) {
        if (call.members.isEmpty() || hasOtherCopy.get(call.site.index()))
            continue;
        for (FunctionID member : call.members) {
            if (resultCheckOf(flow, member) == ResultCheck::ByCallers)
                leavesResultTestToReceiver.quickSet(member.index());
        }
    }
    if (leavesResultTestToReceiver.isEmpty())
        return leavesResultTestToReceiver;

    for (ScannedCall& call : calls) {
        if (call.members.isEmpty() || !hasOtherCopy.get(call.site.index()))
            continue;
        bool needsResultTest = std::ranges::any_of(call.members, [&](FunctionID member) {
            return leavesResultTestToReceiver.quickGet(member.index());
        });
        if (needsResultTest)
            call.members.clear();
    }
    return leavesResultTestToReceiver;
}

static BitVector uniteMembers(std::span<const ScannedCall> calls, unsigned numberOfSites, Vector<GroupNode>& nodes)
{
    BitVector isMember(nodes.size());
    Vector<FunctionID> representativeOfSite(numberOfSites);
    for (const ScannedCall& call : calls) {
        if (call.members.isEmpty())
            continue;
        FunctionID& representative = representativeOfSite[call.site.index()];
        if (!representative)
            representative = call.members[0];
        for (FunctionID member : call.members) {
            isMember.quickSet(member.index());
            nodes[member.index()].unify(&nodes[representative.index()]);
        }
    }
    return isMember;
}

static void numberGroups(Vector<GroupNode>& nodes, const BitVector& isMember, Vector<CallGroup>& groups, Vector<CallGroupID>& groupOfFunction)
{
    Vector<CallGroupID> groupOfRoot(nodes.size());
    groupOfFunction.fill(CallGroupID(), nodes.size());
    for (size_t member : isMember) {
        size_t root = nodes[member].find() - nodes.span().data();
        CallGroupID& group = groupOfRoot[root];
        if (!group) {
            group = CallGroupID(groups.size());
            groups.append(CallGroup { });
        }
        groupOfFunction[member] = group;
        groups[group.index()].members.append(FunctionID(member));
    }
}

static void computeGroupShape(const Program& program, CallGroup& group)
{
    unsigned numberOfArguments = 0;
    for (FunctionID member : group.members) {
        const Function& function = program.at(member);
        numberOfArguments = std::max(numberOfArguments, function.numberOfPositionalParameters());
        group.someMemberTakesThis |= function.has(FunctionFlags::HasThis);
    }
    group.arguments.grow(numberOfArguments);
}

static void addSite(const Program& program, const ScannedCall& scanned, CallGroup& group)
{
    const Function& caller = program.at(scanned.caller);
    const Value& call = caller.at(scanned.call);
    group.sites.append(scanned.site);

    if (ValueID thisValue = call.thisValueOperand())
        group.thisValue.types.add(caller.at(thisValue).type);
    else
        group.thisValue.isOmittedAtSomeSite = true;

    std::span<const ValueID> arguments = call.argumentsOperands();
    for (size_t index = 0; index < group.arguments.size(); ++index) {
        if (index < arguments.size())
            group.arguments[index].types.add(caller.at(arguments[index]).type);
        else
            group.arguments[index].isOmittedAtSomeSite = true;
    }
}

static BitVector findFunctionsEnteredGenerically(const Program& program, const FlowResults& flow, std::span<const ScannedCall> calls)
{
    BitVector isEnteredGenerically = functionsCalledByHost(program);
    auto add = [&](FunctionID version) {
        if (version)
            isEnteredGenerically.set(closureFunctionOf(program, flow, version).index());
    };

    BitVector visitedStamps(program.types.numberOfCalleeSets());
    for (const ScannedCall& scanned : calls) {
        if (!scanned.members.isEmpty())
            continue;
        CalleeSetID stamp = program.at(scanned.caller).at(scanned.call).callees();
        if (!stamp || visitedStamps.set(stamp.index()))
            continue;
        for (FunctionID version : program.at(stamp).functions)
            add(version);
    }
    for (const FlowIndirectCall& call : flow.indirectCalls())
        add(call.callee);
    isEnteredGenerically.merge(findFunctionsReadFromVariables(program, flow));
    return isEnteredGenerically;
}

CallGroups groupCallsByValue(const Program& program, const FlowResults& flow, BuildContext& build)
{
    CallGroups result;
    Vector<ScannedCall> calls = scanCalls(program, flow, build);
    unsigned numberOfSites = computeNumberOfSites(calls.span());
    ungroupMixedSites(calls, numberOfSites);
    result.m_leavesResultTestToReceiver = separateSitesThatNeedResultTests(program, flow, calls, numberOfSites);

    Vector<GroupNode> nodes(program.functions.size());
    BitVector isMember = uniteMembers(calls.span(), numberOfSites, nodes);
    if (isMember.isEmpty())
        return result;

    numberGroups(nodes, isMember, result.m_groups, result.m_groupOfFunction);
    for (CallGroup& group : result.m_groups)
        computeGroupShape(program, group);

    result.m_groupOfSite.fill(CallGroupID(), numberOfSites);
    for (const ScannedCall& call : calls) {
        if (call.members.isEmpty())
            continue;
        CallGroupID group = result.m_groupOfFunction[call.members[0].index()];
        CallGroupID& groupOfSite = result.m_groupOfSite[call.site.index()];
        RELEASE_ASSERT(!groupOfSite || groupOfSite == group);
        groupOfSite = group;
        addSite(program, call, result.m_groups[group.index()]);
    }
    for (CallGroup& group : result.m_groups)
        sortAndRemoveDuplicates(build.parallelLoops(), group.sites);

    BitVector isEnteredGenerically = findFunctionsEnteredGenerically(program, flow, calls.span());
    result.m_genericCallers.fill(GenericCallers::Unknown, program.functions.size());
    for (size_t member : isMember) {
        const FunctionFlow& memberFlow = flow.function(FunctionID(member));
        if (!isEnteredGenerically.get(member) && memberFlow.callers == CallerState::AllKnown && memberFlow.closureUse == ClosureUse::CalledOnly)
            result.m_genericCallers[member] = GenericCallers::None;
    }

    build.statistics().countChanges(PassID::GroupCallsByValue, result.m_groups.size());
    return result;
}

static void dumpPosition(IRPrinter& printer, const CallGroupPosition& position)
{
    PrintStream& out = printer.out();
    out.print("["_s);
    CommaPrinter comma;
    for (TypeID type : position.types) {
        out.print(comma);
        printer.print(type);
    }
    if (position.isOmittedAtSomeSite)
        out.print(position.types.isEmpty() ? "omitted"_s : " | omitted"_s);
    out.print("]"_s);
}

void CallGroups::dump(PrintStream& out, const Program& program) const
{
    IRPrinter printer(out, program);
    for (unsigned index = 0; index < m_groups.size(); ++index) {
        const CallGroup& group = m_groups[index];
        out.print("group g"_s, index, " members="_s);
        printer.list(group.members, [&](FunctionID member) {
            printer.print(member);
        });
        out.print(" sites="_s);
        printer.list(group.sites, [&](AccessSiteID site) {
            out.print("a"_s, site.index());
        });
        out.print(" this="_s);
        dumpPosition(printer, group.thisValue);
        out.print(" arguments="_s);
        printer.list(group.arguments, [&](const CallGroupPosition& position) {
            dumpPosition(printer, position);
        });
        out.println();
    }

    CommaPrinter receiverSeparator(" "_s, "result test left to the receiver: "_s);
    for (size_t index : m_leavesResultTestToReceiver) {
        if (!groupOf(FunctionID(index)))
            continue;
        out.print(receiverSeparator);
        printer.print(FunctionID(index));
    }
    if (receiverSeparator.didPrint())
        out.println();

    CommaPrinter separator(" "_s, "generic callers none: "_s);
    for (unsigned index = 0; index < m_genericCallers.size(); ++index) {
        if (m_genericCallers[index] != GenericCallers::None)
            continue;
        out.print(separator);
        printer.print(FunctionID(index));
    }
    if (separator.didPrint())
        out.println();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

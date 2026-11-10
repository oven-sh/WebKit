/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTailCallPlan.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCallingConventions.h"
#include "AOTConvention.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTStackTestPlan.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTStubBodyConvention.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct GraphOfTailCallPlan {
    std::span<const uint32_t> targetsOf(unsigned node) const { return targets.span().subspan(firstTargetOf[node], firstTargetOf[node + 1] - firstTargetOf[node]); }

    Vector<uint32_t> firstTargetOf;
    Vector<uint32_t> targets;
};

} // anonymous namespace

static FunctionID calleeOfTailCallInTailCallPlan(const Program& program, const LinkTables& linked, FunctionID caller, const SummaryCall& call, bool& comesBack)
{
    comesBack = false;
    switch (call.callee) {
    case SummaryCall::Callee::Builtin:
        comesBack = callsOfBuiltinComeBack(program, call.builtin);
        return { };
    case SummaryCall::Callee::Binding:
        return call.function;
    case SummaryCall::Callee::Name:
    case SummaryCall::Callee::Value:
        return linked.calleeAt(caller, call.site);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID valueBehindNamesInTailCallPlan(const BodyIndex& index, ValueID value)
{
    for (;;) {
        ValueID named;
        if (index.isParameter(value))
            named = index.valueNarrowedInto(value);
        else if (BodyValue definition = index.value(value); inputPassThroughOpcodes.contains(definition.opcode()))
            named = definition.operand(0);
        if (!named)
            return value;
        value = named;
    }
}

static std::optional<unsigned> findOwnParameterInTailCallPlan(const Function& function, const BodyIndex& index, ValueID value)
{
    std::optional<uint32_t> entryParameter = index.block(entryBlock).indexOfParameter(value);
    if (!entryParameter || *entryParameter >= 64 || index.numberOfIncomingEdges(entryBlock))
        return std::nullopt;
    unsigned firstParameter = function.firstParameterIndex();
    if (*entryParameter < firstParameter || *entryParameter - firstParameter >= function.numberOfPositionalParameters())
        return std::nullopt;
    return *entryParameter;
}

static ValueID argumentForEntryParameterInTailCallPlan(const Program& program, const BodyValue& call, FunctionID callee, unsigned entryParameter)
{
    if (call.opcode() != Opcode::Call && call.opcode() != Opcode::CallDirect)
        return { };
    std::span<const ValueID> arguments = call.argumentsOperands();
    unsigned position = call.opcode() == Opcode::CallDirect ? entryParameter : entryParameter - program.at(callee).firstParameterIndex();
    return position < arguments.size() ? arguments[position] : ValueID();
}

namespace {

class FollowerOfTailCalls {
    WTF_MAKE_NONCOPYABLE(FollowerOfTailCalls);
public:
    FollowerOfTailCalls(const Program&, const Facts&);

    FunctionsThatEndTheirTailCalls run();

private:
    enum class Rows : uint8_t { NotFollowed, AreBeingFollowed, MayNeverEnd, End };

    struct FunctionBeingFollowed {
        FunctionID function;
        uint32_t nextCall { 0 };
        uint64_t entryParametersThatMustEndTheirs { 0 };
    };

    void beginToFollow(FunctionID);
    void goOnWithLastFunction();
    bool followTailCall(FunctionBeingFollowed&, const SummaryCall&, FunctionID& toFollowFirst) const;
    std::optional<unsigned> findOwnParameterCalledBy(FunctionID caller, const SummaryCall&) const;

    const Program& m_program;
    const Facts& m_facts;
    Vector<Rows> m_rows;
    Vector<uint64_t> m_entryParametersThatMustEndTheirs;
    Vector<FunctionBeingFollowed> m_functionsBeingFollowed;
};

FollowerOfTailCalls::FollowerOfTailCalls(const Program& program, const Facts& facts)
    : m_program(program)
    , m_facts(facts)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    m_rows.fill(Rows::MayNeverEnd, numberOfFunctions);
    m_entryParametersThatMustEndTheirs.fill(0, numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (facts.linked().function(FunctionID(index)).isReached && program.functions[index].hasBody())
            m_rows[index] = Rows::NotFollowed;
    }
}

void FollowerOfTailCalls::beginToFollow(FunctionID function)
{
    m_rows[function.index()] = Rows::AreBeingFollowed;
    m_functionsBeingFollowed.append(FunctionBeingFollowed { function });
}

std::optional<unsigned> FollowerOfTailCalls::findOwnParameterCalledBy(FunctionID caller, const SummaryCall& call) const
{
    if (call.callee != SummaryCall::Callee::Value || call.isConstruction || call.passesList || call.isMadeByProtocol)
        return std::nullopt;
    const BodyIndex* index = m_facts.bodies().find(caller);
    if (!index || index->opcodeOf(call.site) != Opcode::Call)
        return std::nullopt;
    return findOwnParameterInTailCallPlan(m_program.at(caller), *index, valueBehindNamesInTailCallPlan(*index, index->value(call.site).calleeOperand()));
}

bool FollowerOfTailCalls::followTailCall(FunctionBeingFollowed& caller, const SummaryCall& call, FunctionID& toFollowFirst) const
{
    bool comesBack;
    FunctionID callee = calleeOfTailCallInTailCallPlan(m_program, m_facts.linked(), caller.function, call, comesBack);
    if (!callee) {
        if (comesBack)
            return true;
        std::optional<unsigned> called = findOwnParameterCalledBy(caller.function, call);
        if (called)
            caller.entryParametersThatMustEndTheirs |= uint64_t(1) << *called;
        return called.has_value();
    }
    if (m_rows[callee.index()] == Rows::NotFollowed) {
        toFollowFirst = callee;
        return true;
    }
    if (m_rows[callee.index()] != Rows::End)
        return false;
    uint64_t mustEndTheirs = m_entryParametersThatMustEndTheirs[callee.index()];
    if (!mustEndTheirs)
        return true;

    const BodyIndex* index = m_facts.bodies().find(caller.function);
    if (!index)
        return false;
    BodyValue written = index->value(call.site);
    for (; mustEndTheirs; mustEndTheirs &= mustEndTheirs - 1) {
        ValueID argument = argumentForEntryParameterInTailCallPlan(m_program, written, callee, static_cast<unsigned>(std::countr_zero(mustEndTheirs)));
        if (!argument)
            return false;
        argument = valueBehindNamesInTailCallPlan(*index, argument);
        if (std::optional<unsigned> handedOn = findOwnParameterInTailCallPlan(m_program.at(caller.function), *index, argument)) {
            caller.entryParametersThatMustEndTheirs |= uint64_t(1) << *handedOn;
            continue;
        }
        if (index->isParameter(argument) || index->opcodeOf(argument) != Opcode::NewClosure)
            return false;
        FunctionID made = index->value(argument).function();
        if (m_rows[made.index()] == Rows::NotFollowed) {
            toFollowFirst = made;
            return true;
        }
        if (m_rows[made.index()] != Rows::End || m_entryParametersThatMustEndTheirs[made.index()])
            return false;
    }
    return true;
}

void FollowerOfTailCalls::goOnWithLastFunction()
{
    FunctionBeingFollowed& last = m_functionsBeingFollowed.last();
    std::span<const SummaryCall> calls = m_facts.summaries().at(last.function).calls.span();
    bool everyRowEnds = true;
    for (; everyRowEnds && last.nextCall < calls.size(); ++last.nextCall) {
        if (!calls[last.nextCall].isInTailPosition)
            continue;
        FunctionID toFollowFirst;
        everyRowEnds = followTailCall(last, calls[last.nextCall], toFollowFirst);
        if (toFollowFirst) {
            beginToFollow(toFollowFirst);
            return;
        }
    }
    m_rows[last.function.index()] = everyRowEnds ? Rows::End : Rows::MayNeverEnd;
    if (everyRowEnds)
        m_entryParametersThatMustEndTheirs[last.function.index()] = last.entryParametersThatMustEndTheirs;
    m_functionsBeingFollowed.removeLast();
}

FunctionsThatEndTheirTailCalls FollowerOfTailCalls::run()
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(m_rows.size());
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (m_rows[index] != Rows::NotFollowed)
            continue;
        beginToFollow(FunctionID(index));
        while (!m_functionsBeingFollowed.isEmpty())
            goOnWithLastFunction();
    }

    FunctionsThatEndTheirTailCalls functions;
    functions.whateverArrives = BitVector(numberOfFunctions);
    functions.ifArgumentsDo = BitVector(numberOfFunctions);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        if (m_rows[index] != Rows::End)
            continue;
        if (uint64_t mustEndTheirs = m_entryParametersThatMustEndTheirs[index]) {
            functions.ifArgumentsDo.quickSet(index);
            functions.conditions.append({ FunctionID(index), mustEndTheirs });
        } else
            functions.whateverArrives.quickSet(index);
    }
    return functions;
}

} // anonymous namespace

FunctionsThatEndTheirTailCalls findFunctionsThatEndTheirTailCalls(const Program& program, const Facts& facts)
{
    if (facts.linked().functions.size() != program.functions.size())
        return { };
    return FollowerOfTailCalls(program, facts).run();
}

void checkFormsRoundRingsOfTailCalls(const Program& program, const Facts& facts, const Plan& plan, BuildContext& build)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    const LinkTables& linked = facts.linked();
    if (linked.functions.size() != numberOfFunctions)
        return;

    struct TailCall {
        FunctionID caller;
        FunctionID callee;
        ValueID site;
        bool isWrittenDirect;
    };
    Vector<TailCall> tailCalls;
    GraphOfTailCallPlan graph;
    graph.firstTargetOf.reserveInitialCapacity(numberOfFunctions + 1);
    for (uint32_t index = 0; index < numberOfFunctions; ++index) {
        graph.firstTargetOf.append(static_cast<uint32_t>(graph.targets.size()));
        FunctionID caller(index);
        if (!linked.function(caller).isReached || !program.at(caller).hasBody())
            continue;
        for (const SummaryCall& call : facts.summaries().at(caller).calls) {
            bool comesBack;
            FunctionID callee = call.isInTailPosition ? calleeOfTailCallInTailCallPlan(program, linked, caller, call, comesBack) : FunctionID();
            if (!callee)
                continue;
            graph.targets.append(callee.index());
            tailCalls.append(TailCall { caller, callee, call.site, call.callee == SummaryCall::Callee::Binding });
        }
    }
    graph.firstTargetOf.append(static_cast<uint32_t>(graph.targets.size()));
    StronglyConnectedComponents rings = computeStronglyConnectedComponents(numberOfFunctions, [&](unsigned node) {
        return graph.targetsOf(node);
    });

    const CallingConventions& conventions = plan.conventions();
    for (const TailCall& tailCall : tailCalls) {
        CodeUnit leaving { tailCall.caller, CodeRole::Body };
        CodeUnit entered { tailCall.callee, CodeRole::Body };
        if (rings.componentOf[tailCall.caller.index()] != rings.componentOf[tailCall.callee.index()] || !conventions.exists(leaving) || !conventions.exists(entered))
            continue;
        if (conventions.numberOfResults(leaving) != 1 || conventions.numberOfResults(entered) != 1)
            continue;
        Representation form = conventions.resultRepresentation(leaving);
        if (form == conventions.resultRepresentation(entered) && (tailCall.isWrittenDirect || form == Representation::JSValue))
            continue;
        build.diagnostics().add(InternalError {
            .kind = InternalErrorKind::PlacementCheckFailed,
            .function = tailCall.caller,
            .value = tailCall.site,
            .detail = "a call in tail position on a ring of such calls cannot be a jump in every unit: the two bodies return in two forms, or in a raw one though the call is one of a value. Each turn of the ring would leave a frame"_s,
        });
    }
}

static bool hasNoUnitBesideBodyAndEntriesInTailCallPlan(FunctionConvention convention)
{
    for (unsigned role = 0; role < numberOfCodeRolesOfOneFunction; ++role) {
        if (convention.has(static_cast<CodeRole>(role)) && static_cast<CodeRole>(role) != CodeRole::Body && !isGenericEntry(static_cast<CodeRole>(role)))
            return false;
    }
    return true;
}

static bool liesInRegistersOfStubBodyInTailCallPlan(const ArgumentLocations& locations)
{
    if (locations.numberOfStackWords() || locations.results.size() != 1)
        return false;
    size_t numberOfGPRs = std::ranges::count(locations.parameters, ArgumentLocation::Kind::GPR, &ArgumentLocation::kind);
    size_t numberOfFPRs = std::ranges::count(locations.parameters, ArgumentLocation::Kind::FPR, &ArgumentLocation::kind);
    return numberOfGPRs <= StubBodyConvention::maxNumberOfOperandGPRs && numberOfFPRs <= StubBodyConvention::maxNumberOfOperandFPRs;
}

BitVector findFunctionsThatSaveRegistersOfCallers(const Program& program, const Facts& facts, const CallingConventions& conventions)
{
    uint32_t numberOfFunctions = static_cast<uint32_t>(program.functions.size());
    BitVector functions(numberOfFunctions);

    BitVector calleesOfTailCalls(numberOfFunctions);
    for (const LinkTables* tables : { &facts.linked(), &facts.held() }) {
        for (const LinkedCall& call : tables->calls) {
            if (call.isInTailPosition)
                calleesOfTailCalls.set(call.callee.index());
        }
    }

    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        const RuntimeFunctionData& data = program.builtins().at(entry.id);
        FunctionID id = entry.function;
        if (!id || !data.flags.contains(RuntimeFunctionFlag::UsesStubConvention) || data.implements.empty())
            continue;

        FunctionConvention convention = conventions.of(id);
        if (!convention.has(CodeRole::Body) || !hasNoUnitBesideBodyAndEntriesInTailCallPlan(convention))
            continue;
        bool makesTailCall = std::ranges::any_of(facts.summaries().at(id).calls, [](const SummaryCall& call) {
            return call.isInTailPosition;
        });
        if (makesTailCall || calleesOfTailCalls.get(id.index()))
            continue;

        CodeUnit body { id, CodeRole::Body };
        if (convention.numberOfResults != 1 || conventions.stackWordsReservedByCallersOf(program, body))
            continue;
        if (liesInRegistersOfStubBodyInTailCallPlan(conventions.locationsOf(program, body)))
            functions.quickSet(id.index());
    }
    return functions;
}

void planTailCallsAndStackTests(const Program& program, const Facts& facts, Plan& plan, StackTestPlan&& stackTests)
{
    if (!facts.mechanisms().contains(Mechanism::DirectCalls))
        return;

    TailCallPlan tailCalls;
    tailCalls.functionsThatSaveRegistersOfCallers = findFunctionsThatSaveRegistersOfCallers(program, facts, plan.conventions());
    plan.setTailCalls(WTF::move(tailCalls));

    plan.setStackTests(WTF::move(stackTests));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

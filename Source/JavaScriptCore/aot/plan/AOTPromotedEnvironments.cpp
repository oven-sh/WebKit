/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */


#include "config.h"
#include "AOTPromotedEnvironments.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTOpcodeSets.h"
#include "AOTSlotPlan.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTThinSummary.h"
#include <optional>
#include <type_traits>
#include <wtf/HashMap.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FunctionsWhoseClosuresOperationMakes {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role... role)
    {
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            if (id && ((role == FunctionReferenceKind::Closure) || ...))
                functions.append(id);
        }
    }

    Vector<FunctionID, 8>& functions;
};

class PlannerOfPromotedEnvironments {
public:
    PlannerOfPromotedEnvironments(const Program& program, const Facts& facts, const SlotPlanParts& slots)
        : m_program(program)
        , m_facts(facts)
        , m_slots(slots)
        , m_isMade(program.layouts.size())
        , m_isRefused(program.layouts.size())
        , m_isToBeGoneThrough(program.functions.size())
        , m_isNamed(program.functions.size())
        , m_holdsWord(program.functions.size())
    {
    }

    PromotedEnvironments run(std::span<const LayoutID> scopesTakenByClosuresMadeForCallsAlone);

private:
    static constexpr uint32_t noSource = 0;
    static constexpr uint32_t noSourceServes = 1;
    static constexpr uint32_t firstSource = 2;

    struct ScopeMadeOnce {
        LayoutID layout;
        FunctionID maker;
    };

    struct TypesOfCapture {
        TypeID established;
        TypeID proven;
        bool everyWriterIsKnown { false };

        friend bool operator==(const TypesOfCapture&, const TypesOfCapture&) = default;
    };

    struct Source {
        FunctionID function;
        ValueID value;
        FunctionID firstReader;
        uint32_t captureOfFirstReader { 0 };
        TypesOfCapture types;
        bool readersAgree { true };
        bool mayLieInFrameOfCaller { false };
        VariableID variable;
    };

    using Candidate = uint32_t;
    struct FunctionAndCandidate {
        uint32_t function;
        Candidate candidate;
    };

    struct HolderMadeOnEveryWay {
        FunctionID holder;
        FunctionID maker;
        bool isStoredForGood { false };
    };

    bool servesAsMaker(FunctionID, const ThinSummary&, const BodyIndex&) const;
    bool mayLieAmongVariables(LayoutID) const;
    bool capturesMayBeLeftOut(FunctionID) const;
    TypesOfCapture typesOfCapture(FunctionID function, uint32_t capture) const
    {
        const LinkedCell& cell = m_facts.linked().capture(function, capture);
        return { m_program.provenTypeOf(m_program.at(function).captures[capture]), cell.everyWriterIsKnown ? cell.proven : TypeID(), cell.everyWriterIsKnown };
    }

    LayoutID scopeIn(TypeID type) const { return type && !holdsOnlyPrimitives(type) ? m_program.types.typeKeptAt(type).environment : LayoutID(); }

    void goThrough(FunctionID);
    void name(FunctionID);
    void findScopesMadeOnce(FunctionID, const ThinSummary&, const BodyIndex&, bool serves);
    void refuseScopesThatPositionsHold();

    uint32_t nodeOf(FunctionID function, uint32_t capture);
    uint32_t rootOf(uint32_t node);
    void unite(uint32_t, uint32_t);
    void addSource(uint32_t node, uint32_t source);
    static bool agrees(TypesOfCapture ofSource, TypesOfCapture ofReader) { return ofSource.established == ofReader.established && (ofSource.proven == ofReader.proven || (!ofReader.proven && ofReader.everyWriterIsKnown)); }
    static void noteReader(Source&, FunctionID reader, uint32_t capture, TypesOfCapture);
    bool mayLieInFrameOfCaller(FunctionID, const BodyIndex&, ValueID) const;
    void followCapturedValues(FunctionID, const ThinSummary&, const BodyIndex&, bool serves);
    bool goThroughWhatStandsInFunctionsThatRunOnce();

    std::optional<Candidate> candidateOfCapture(FunctionID, uint32_t capture);
    FunctionID makerOf(Candidate candidate) const { return candidate < m_sources.size() ? m_sources[candidate].function : m_scopes[candidate - m_sources.size()].maker; }
    bool hasOneClosureThatNeverDies(FunctionID) const;
    bool isKeptByVariableThatStays(FunctionID maker, FunctionID holder) const;
    bool isLaidOnClassEvaluatedAtMostOnce(const BodyIndex&, ValueID closure, FunctionID) const;
    FunctionID makerThatMakesOnEveryWay(FunctionID holder) const;
    bool isStoredForGood(FunctionID holder) const;
    bool isExportsObjectThatStays(LayoutID) const;
    bool isEnteredUnderNoHandler(FunctionID, unsigned stepsLeft = 8) const;
    bool storesForGood(FunctionID maker, const BodyIndex&, ValueID closure, FunctionID holder, BlockID lastBlockOnEveryWayToReturn) const;
    bool isClassToken(TypeID type) const { return type && m_program.at(type).isOpaque(OpaqueKind::ClassToken); }
    bool listFunctionsWhoseClosuresAreHeld(Candidate, TypesOfCapture, bool& holdsClosures);
    void strike(Candidate);
    void noteThatFunctionHoldsWord(uint32_t function);
    void keepWhatCouldBeLetGo();

    VariableID addVariable(PromotedVariable&&);
    void leaveOutCaptures();
    void listStores();

    const Program& m_program;
    const Facts& m_facts;
    const SlotPlanParts& m_slots;
    BitVector m_isMade;
    BitVector m_isRefused;
    BitVector m_isToBeGoneThrough;
    BitVector m_isNamed;
    BitVector m_functionsWhoseClosuresRunTimeMakes;
    Vector<FunctionID> m_functionsToGoThrough;
    Vector<uint32_t> m_functionsNamed;
    Vector<LayoutID> m_scopesMade;
    Vector<ScopeMadeOnce> m_scopes;
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_nodesOfCaptures;
    Vector<uint32_t> m_parents;
    Vector<uint32_t> m_sourcesOfClasses;
    Vector<uint32_t> m_sourcesOfValues;
    Vector<Source> m_sources;
    Vector<HolderMadeOnEveryWay> m_holdersMadeOnEveryWay;
    BitVector m_isStruck;
    BitVector m_isCapturedByClosureThatNeverDies;
    BitVector m_isCapturedByClosureStoredForGood;
    BitVector m_servesBecauseClassIsStoredForGood;
    BitVector m_isCapturedByClosureReachableForGood;
    BitVector m_closuresReachableForGood;
    BitVector m_holdsWord;
    Vector<uint32_t> m_functionsThatHaveComeToHoldWord;
    Vector<FunctionAndCandidate> m_candidatesByFunctionHeld;
    Vector<FunctionAndCandidate> m_functionsByCandidateCaptured;
    PromotedEnvironments m_result;
};

bool PlannerOfPromotedEnvironments::servesAsMaker(FunctionID id, const ThinSummary& summary, const BodyIndex& index) const
{
    const Function& function = m_program.at(id);
    if (!m_facts.runsAtMostOnce(id) || function.isCoroutine() || summary.has(SummaryTrait::HasDirectEval) || index.hasIrreducibleFlow())
        return false;
    return m_program.at(function.module).kind != ModuleKind::RuntimeSource;
}

bool PlannerOfPromotedEnvironments::mayLieAmongVariables(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    const PlannedLayout& planned = m_slots.layouts[id.index()];
    if (layout.kind != LayoutKind::Environment || planned.storage != LayoutStorage::Environment)
        return false;
    if (planned.numberOfFields() < layout.fields.size())
        return false;
    for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (slot.isOutOfLine() || slot.deferredFunction || slot.representation == SlotRepresentation::DeferredClosure)
            return false;
        if (!m_program.at(m_program.provenTypeOf(layout.fields[field].declared)).isJavaScriptValue())
            return false;
    }
    return true;
}

bool PlannerOfPromotedEnvironments::capturesMayBeLeftOut(FunctionID id) const
{
    const Function& function = m_program.at(id);
    if (!function.hasBody() || function.isCoroutine() || !function.has(FunctionCapabilities::MayChangeSignature) || m_functionsWhoseClosuresRunTimeMakes.get(id.index()))
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PlannerOfPromotedEnvironments::goThrough(FunctionID id)
{
    if (!m_isToBeGoneThrough.quickSet(id.index()))
        m_functionsToGoThrough.append(id);
}

void PlannerOfPromotedEnvironments::name(FunctionID id)
{
    if (m_isNamed.quickSet(id.index()))
        return;
    m_functionsNamed.append(id.index());
    goThrough(id);
}

void PlannerOfPromotedEnvironments::findScopesMadeOnce(FunctionID id, const ThinSummary& summary, const BodyIndex& index, bool serves)
{
    for (ValueID site : summary.environmentsMade) {
        LayoutID layout = index.value(site).as<Opcode::NewObject>().layout();
        m_scopesMade.append(layout);
        if (m_isMade.quickSet(layout.index()) || !serves || index.loopOf(index.blockOf(site)) || !mayLieAmongVariables(layout))
            m_isRefused.quickSet(layout.index());
        else
            m_scopes.append({ layout, id });
    }
}

void PlannerOfPromotedEnvironments::refuseScopesThatPositionsHold()
{
    auto refuse = [&](const DeclaredType& declared) {
        if (LayoutID scope = scopeIn(declared.written()))
            m_isRefused.quickSet(scope.index());
    };

    for (LayoutID made : m_scopesMade) {
        for (const Field& field : m_program.at(made).fields)
            refuse(field.declared);
    }
    for (const Variable& variable : m_program.variables)
        refuse(variable.declared);
    for (uint32_t number : m_functionsNamed) {
        const Function& function = m_program.functions[number];
        refuse(function.thisType);
        for (const DeclaredType& parameter : function.parameters)
            refuse(parameter);
        for (const DeclaredType& result : function.results)
            refuse(result);
        bool keepsCaptures = !function.captures.isEmpty() && !capturesMayBeLeftOut(FunctionID(number));
        for (const DeclaredCapture& capture : function.captures) {
            LayoutID scope = scopeIn(capture.written());
            if (scope && (keepsCaptures || m_program.types.typeKeptAt(capture.written()).primitives))
                m_isRefused.quickSet(scope.index());
        }
    }
    for (const EvalScope& scope : m_program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.layout)
                m_isRefused.quickSet(binding.layout.index());
        }
    }
    for (FieldReference exported : m_program.exportedFields())
        m_isRefused.quickSet(exported.layout.index());
    for (const ModuleInstance& instance : m_program.moduleInstances)
        m_isRefused.quickSet(instance.layout.index());
}

uint32_t PlannerOfPromotedEnvironments::nodeOf(FunctionID function, uint32_t capture)
{
    auto added = m_nodesOfCaptures.add(m_facts.linked().firstCaptureOfFunction[function.index()] + capture, static_cast<uint32_t>(m_parents.size()));
    if (added.isNewEntry) {
        m_parents.append(added.iterator->value);
        m_sourcesOfClasses.append(noSource);
    }
    return added.iterator->value;
}

uint32_t PlannerOfPromotedEnvironments::rootOf(uint32_t node)
{
    while (m_parents[node] != node) {
        m_parents[node] = m_parents[m_parents[node]];
        node = m_parents[node];
    }
    return node;
}

void PlannerOfPromotedEnvironments::addSource(uint32_t node, uint32_t source)
{
    uint32_t& known = m_sourcesOfClasses[rootOf(node)];
    if (known == noSource)
        known = source;
    else if (source != noSource && known != source)
        known = noSourceServes;
}

void PlannerOfPromotedEnvironments::unite(uint32_t first, uint32_t second)
{
    first = rootOf(first);
    second = rootOf(second);
    if (first == second)
        return;
    m_parents[second] = first;
    addSource(first, m_sourcesOfClasses[second]);
}

void PlannerOfPromotedEnvironments::noteReader(Source& source, FunctionID reader, uint32_t capture, TypesOfCapture types)
{
    if (!source.types.proven && source.types.everyWriterIsKnown && types.proven && source.types.established == types.established) {
        source.firstReader = reader;
        source.captureOfFirstReader = capture;
        source.types = types;
        return;
    }
    source.readersAgree &= agrees(source.types, types);
}

bool PlannerOfPromotedEnvironments::mayLieInFrameOfCaller(FunctionID id, const BodyIndex& index, ValueID value) const
{
    while (!index.isParameter(value) && inputPassThroughOpcodes.contains(index.opcodeOf(value)) && index.value(value).operand(0))
        value = index.value(value).operand(0);
    if (!index.isParameter(value))
        return false;
    std::optional<uint32_t> entryParameter = index.block(entryBlock).indexOfParameter(value);
    return !entryParameter || m_facts.linked().onlyCallsWhatArrivesAt(id, *entryParameter);
}

static ValueID valueReturnedOnEveryWayForPromotedEnvironments(const BodyIndex& index)
{
    ValueID returned;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!index.isReachable(block))
            continue;
        BodyTerminator end = index.terminator(block);
        if (end.kind() == TerminatorKind::ReturnCall)
            return { };
        if (end.kind() != TerminatorKind::Return)
            continue;
        ValueID value = end.as<TerminatorKind::Return>().valueOperand();
        while (value && !index.isParameter(value) && inputIdentityOpcodes.contains(index.opcodeOf(value)))
            value = index.value(value).operand(0);
        if (!value || (returned && returned != value))
            return { };
        returned = value;
    }
    return returned;
}

static BlockID lastBlockOnEveryWayToReturnForPromotedEnvironments(const BodyIndex& index)
{
    BlockID last;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID block(number);
        TerminatorKind end = index.isReachable(block) ? index.terminator(block).kind() : TerminatorKind::Unreachable;
        if (end != TerminatorKind::Return && end != TerminatorKind::ReturnCall)
            continue;
        if (!last)
            last = block;
        while (!index.dominates(last, block))
            last = index.immediateDominator(last);
    }
    return last;
}

void PlannerOfPromotedEnvironments::followCapturedValues(FunctionID id, const ThinSummary& summary, const BodyIndex& index, bool serves)
{
    const LinkTables& linked = m_facts.linked();
    if (serves)
        m_sourcesOfValues.fill(noSource, index.numberOfValues());
    BodyBlock entry = index.block(entryBlock);
    uint32_t numberOfOwnCaptures = static_cast<uint32_t>(m_program.at(id).captures.size());
    std::optional<BlockID> lastBlockOnEveryWayToReturn;
    ValueID returnedOnEveryWay = serves ? valueReturnedOnEveryWayForPromotedEnvironments(index) : ValueID();
    for (ValueID site : summary.sitesThatHandOverCaptures) {
        BodyValue value = index.value(site);
        bool makesClosure = value.opcode() == Opcode::NewClosure;
        if (!makesClosure && value.opcode() != Opcode::CallDirect) {
            Vector<FunctionID, 8> functions;
            FunctionsWhoseClosuresOperationMakes made { functions };
            value.forEachReference(made);
            for (FunctionID function : functions) {
                m_functionsWhoseClosuresRunTimeMakes.set(function.index());
                name(function);
            }
            continue;
        }
        FunctionID callee = makesClosure ? value.as<Opcode::NewClosure>().function() : value.as<Opcode::CallDirect>().function();
        name(callee);
        std::span<const ValueID> operands = makesClosure ? value.as<Opcode::NewClosure>().capturesOperands() : value.as<Opcode::CallDirect>().argumentsOperands();
        uint32_t numberOfCapturesOfCallee = static_cast<uint32_t>(m_program.at(callee).captures.size());
        bool isMemberThatNeverDies = makesClosure && serves && isLaidOnClassEvaluatedAtMostOnce(index, site, callee);
        bool isKeptByVariable = makesClosure && site == returnedOnEveryWay && isKeptByVariableThatStays(id, callee);
        if (isMemberThatNeverDies || isKeptByVariable)
            m_closuresReachableForGood.set(callee.index());
        if (makesClosure && serves && (hasOneClosureThatNeverDies(callee) || isMemberThatNeverDies || isKeptByVariable)) {
            if (!lastBlockOnEveryWayToReturn)
                lastBlockOnEveryWayToReturn = lastBlockOnEveryWayToReturnForPromotedEnvironments(index);
            if (*lastBlockOnEveryWayToReturn && index.dominates(index.blockOf(site), *lastBlockOnEveryWayToReturn) && !index.block(index.blockOf(site)).hasHandler())
                m_holdersMadeOnEveryWay.append({ callee, id, storesForGood(id, index, site, callee, *lastBlockOnEveryWayToReturn) });
        }
        for (uint32_t capture = 0; capture < numberOfCapturesOfCallee; ++capture) {
            if (scopeIn(m_program.provenTypeOf(m_program.at(callee).captures[capture])))
                continue;
            uint32_t node = nodeOf(callee, capture);
            ValueID handed = capture < operands.size() ? operands[capture] : ValueID();
            ValueID behindCasts = handed;
            while (behindCasts && !index.isParameter(behindCasts) && castsThatClaimNothing.contains(index.opcodeOf(behindCasts)))
                behindCasts = index.value(behindCasts).operand(0);
            std::optional<uint32_t> entryParameterBehindCasts = behindCasts && index.isParameter(behindCasts) ? entry.indexOfParameter(behindCasts) : std::nullopt;
            if (entryParameterBehindCasts && *entryParameterBehindCasts < numberOfOwnCaptures) {
                unite(node, nodeOf(id, *entryParameterBehindCasts));
                continue;
            }
            std::optional<uint32_t> entryParameter = handed && index.isParameter(handed) ? entry.indexOfParameter(handed) : std::nullopt;
            bool isJoinOfValues = handed && index.isParameter(handed) && !entryParameter;
            bool isLentByCaller = entryParameter && linked.onlyCallsWhatArrivesAt(id, *entryParameter);
            if (!handed || !serves || isJoinOfValues || isLentByCaller || index.loopOf(index.blockOf(handed))) {
                addSource(node, noSourceServes);
                continue;
            }
            TypesOfCapture types = typesOfCapture(callee, capture);
            uint32_t& source = m_sourcesOfValues[handed.index()];
            if (source == noSource) {
                m_sources.append(Source { id, handed, callee, capture, types, true, mayLieInFrameOfCaller(id, index, handed), { } });
                source = firstSource + static_cast<uint32_t>(m_sources.size()) - 1;
            } else
                noteReader(m_sources[source - firstSource], callee, capture, types);
            addSource(node, source);
        }
    }
}

bool PlannerOfPromotedEnvironments::goThroughWhatStandsInFunctionsThatRunOnce()
{
    if (m_facts.linked().firstCaptureOfFunction.size() != m_program.functions.size() + 1)
        return false;
    const ThinSummaries& summaries = m_facts.summaries();
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        if (m_facts.runsAtMostOnce(FunctionID(number)))
            goThrough(FunctionID(number));
    }
    while (!m_functionsToGoThrough.isEmpty()) {
        FunctionID id = m_functionsToGoThrough.takeLast();
        const ThinSummary& summary = summaries.at(id);
        if (summary.sitesThatHandOverCaptures.isEmpty() && summary.environmentsMade.isEmpty())
            continue;
        const BodyIndex* index = m_facts.bodies().find(id);
        if (!index)
            return false;
        bool serves = servesAsMaker(id, summary, *index);
        followCapturedValues(id, summary, *index, serves);
        findScopesMadeOnce(id, summary, *index, serves);
    }
    AOT::sort(m_holdersMadeOnEveryWay, { }, &HolderMadeOnEveryWay::holder);
    std::ranges::sort(m_functionsNamed);
    return true;
}

std::optional<PlannerOfPromotedEnvironments::Candidate> PlannerOfPromotedEnvironments::candidateOfCapture(FunctionID id, uint32_t capture)
{
    if (LayoutID scope = scopeIn(m_program.provenTypeOf(m_program.at(id).captures[capture]))) {
        auto found = std::ranges::lower_bound(m_scopes, scope, { }, &ScopeMadeOnce::layout);
        if (found == m_scopes.end() || found->layout != scope)
            return std::nullopt;
        return static_cast<Candidate>(m_sources.size() + (found - m_scopes.begin()));
    }
    auto node = m_nodesOfCaptures.find(m_facts.linked().firstCaptureOfFunction[id.index()] + capture);
    uint32_t found = node == m_nodesOfCaptures.end() ? noSource : m_sourcesOfClasses[rootOf(node->value)];
    if (found < firstSource || !agrees(m_sources[found - firstSource].types, typesOfCapture(id, capture)))
        return std::nullopt;
    return found - firstSource;
}

bool PlannerOfPromotedEnvironments::hasOneClosureThatNeverDies(FunctionID function) const
{
    LayoutID constructed = classOfConstructorFunction(m_program, function);
    return constructed && m_facts.isEvaluatedAtMostOnce(constructed);
}

bool PlannerOfPromotedEnvironments::isLaidOnClassEvaluatedAtMostOnce(const BodyIndex& index, ValueID closure, FunctionID function) const
{
    std::optional<BodyUser> user = index.onlyUser(closure);
    if (!user || user->kind != BodyUseKind::Operand || index.opcodeOf(user->value) != Opcode::DefineMethod)
        return false;
    auto definition = index.value(user->value).as<Opcode::DefineMethod>();
    if (definition.functionOperand() != closure || !definition.key() || !m_facts.isEvaluatedAtMostOnce(definition.layout()))
        return false;
    bool isListed = false;
    for (const ClassMember& member : m_facts.membersOf(definition.layout())) {
        if (member.side != definition.side())
            continue;
        if (!member.key)
            return false;
        if (member.key != definition.key())
            continue;
        bool isOtherHalfOfAccessor = member.kind != definition.methodKind() && member.kind != MethodKind::Method && definition.methodKind() != MethodKind::Method;
        if (isOtherHalfOfAccessor && !member.isAssignedByProgram)
            continue;
        if (isListed || member.isAssignedByProgram || member.kind != definition.methodKind() || member.function != function)
            return false;
        isListed = true;
    }
    return isListed;
}

bool PlannerOfPromotedEnvironments::isKeptByVariableThatStays(FunctionID maker, FunctionID holder) const
{
    const LinkTables& linked = m_facts.linked();
    if (!linked.function(maker).everyCallerIsKnown || !isEnteredUnderNoHandler(maker))
        return false;
    VariableID kept;
    for (FunctionID referrer : linked.referrersOf(maker)) {
        const BodyIndex* index = linked.isGroup(referrer) ? nullptr : m_facts.bodies().find(referrer);
        if (!index)
            return false;
        for (const LinkedCall& call : linked.callsOf(referrer)) {
            if (call.callee != maker)
                continue;
            if (kept)
                return false;
            std::optional<BodyUser> user = index->onlyUser(call.site);
            while (user && user->kind == BodyUseKind::Operand && inputIdentityOpcodes.contains(index->opcodeOf(user->value)))
                user = index->onlyUser(user->value);
            if (!user || user->kind != BodyUseKind::Operand || index->opcodeOf(user->value) != Opcode::PutModuleVariable)
                return false;
            kept = index->value(user->value).as<Opcode::PutModuleVariable>().variable();
        }
    }
    if (!kept)
        return false;
    const LinkedCell& cell = linked.variable(kept);
    if (!cell.everyWriterIsKnown || !cell.proven)
        return false;
    Type held = m_program.at(cell.proven);
    held.primitives &= ~Primitive::Absent;
    return !held.primitives && singleFunctionOf(TypeReader(m_program), held) == holder;
}

bool PlannerOfPromotedEnvironments::isExportsObjectThatStays(LayoutID layout) const
{
    const LinkTables& linked = m_facts.linked();
    ModuleID file = m_program.at(layout).module;
    if (!file || !m_program.at(file).exportsVariable || !m_program.at(file).body || !m_facts.runsAtMostOnce(m_program.at(file).body))
        return false;
    if (!linked.layout(layout).possibleChanges.isEmpty() || linked.layout(layout).reachesUnseenCode)
        return false;
    const LinkedCell& held = linked.variable(m_program.at(file).exportsVariable);
    return held.everyWriterIsKnown && held.isFilled && held.proven && singleLayoutOf(TypeReader(m_program), m_program.at(held.proven)) == layout && !m_program.at(held.proven).primitives;
}

bool PlannerOfPromotedEnvironments::isEnteredUnderNoHandler(FunctionID id, unsigned stepsLeft) const
{
    const Function& function = m_program.at(id);
    if (function.isModuleBody())
        return true;
    const LinkTables& linked = m_facts.linked();
    if (!stepsLeft || function.isCoroutine() || !linked.function(id).everyCallerIsKnown || !m_facts.runsAtMostOnce(id))
        return false;
    bool isCalled = false;
    for (FunctionID referrer : linked.referrersOf(id)) {
        const BodyIndex* index = linked.isGroup(referrer) ? nullptr : m_facts.bodies().find(referrer);
        if (!index)
            return false;
        for (const LinkedCall& call : linked.callsOf(referrer)) {
            if (call.callee != id)
                continue;
            if (index->block(index->blockOf(call.site)).hasHandler() || !isEnteredUnderNoHandler(referrer, stepsLeft - 1))
                return false;
            isCalled = true;
        }
    }
    return isCalled;
}

bool PlannerOfPromotedEnvironments::storesForGood(FunctionID maker, const BodyIndex& index, ValueID closure, FunctionID holder, BlockID lastBlockOnEveryWayToReturn) const
{
    if (!hasOneClosureThatNeverDies(holder) || !isEnteredUnderNoHandler(maker))
        return false;
    const LinkTables& linked = m_facts.linked();
    BodyBlock entry = index.block(entryBlock);
    auto behindCasts = [&](ValueID value) {
        while (value && !index.isParameter(value) && castsThatClaimNothing.contains(index.opcodeOf(value)))
            value = index.value(value).operand(0);
        return value;
    };
    auto withoutAbsent = [&](TypeID id) {
        Type type = m_program.at(id);
        type.primitives &= ~Primitive::Absent;
        return type;
    };
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        BlockID block(number);
        if (!index.dominates(block, lastBlockOnEveryWayToReturn) || index.block(block).hasHandler())
            continue;
        for (BodyValue operation : index.block(block).values()) {
            if (operation.opcode() != Opcode::PropSet)
                continue;
            auto store = operation.as<Opcode::PropSet>();
            if (store.isFreeName() || behindCasts(store.valueOperand()) != closure)
                continue;
            ValueID object = behindCasts(store.objectOperand());
            std::optional<uint32_t> parameter = object && index.isParameter(object) ? entry.indexOfParameter(object) : std::nullopt;
            TypeID proven = parameter && *parameter < linked.parametersOf(maker).size() ? linked.parametersOf(maker)[*parameter].proven : TypeID();
            LayoutID layout = proven && !m_program.at(proven).primitives ? singleLayoutOf(TypeReader(m_program), m_program.at(proven)) : LayoutID();
            if (!layout || !isExportsObjectThatStays(layout))
                continue;
            uint32_t numberListed = static_cast<uint32_t>(m_program.at(layout).fields.size());
            std::optional<uint32_t> field = m_program.at(layout).findField(store.key());
            if (!field) {
                std::span<const PropertyKeyID> appended = linked.appendedKeysOf(layout);
                if (auto found = std::ranges::find(appended, store.key()); found != appended.end())
                    field = numberListed + static_cast<uint32_t>(found - appended.begin());
            }
            const LinkedCell* cell = field ? linked.findCellOfField({ layout, *field }) : nullptr;
            if (cell && cell->everyWriterIsKnown && cell->proven && !withoutAbsent(cell->proven).primitives && singleFunctionOf(TypeReader(m_program), withoutAbsent(cell->proven)) == holder)
                return true;
        }
    }
    return false;
}

bool PlannerOfPromotedEnvironments::isStoredForGood(FunctionID holder) const
{
    auto found = std::ranges::lower_bound(m_holdersMadeOnEveryWay, holder, { }, &HolderMadeOnEveryWay::holder);
    return found != m_holdersMadeOnEveryWay.end() && found->holder == holder && found->isStoredForGood;
}

FunctionID PlannerOfPromotedEnvironments::makerThatMakesOnEveryWay(FunctionID holder) const
{
    auto found = std::ranges::lower_bound(m_holdersMadeOnEveryWay, holder, { }, &HolderMadeOnEveryWay::holder);
    return found != m_holdersMadeOnEveryWay.end() && found->holder == holder ? found->maker : FunctionID();
}

bool PlannerOfPromotedEnvironments::listFunctionsWhoseClosuresAreHeld(Candidate candidate, TypesOfCapture types, bool& holdsClosures)
{
    if (isClassToken(types.established))
        return m_isCapturedByClosureThatNeverDies.quickGet(candidate);
    for (TypeID id : { types.proven, types.established }) {
        if (!id)
            continue;
        Type type = m_program.at(id);
        type.primitives &= ~Primitive::Absent;
        if (type.hasOnlyPrimitives(Primitive::NumberLike))
            return true;
        if (!type.hasOnlyClosures())
            continue;
        holdsClosures = true;
        bool noneCouldBeLetGo = true;
        forEachFunctionAmongClosuresOf(TypeReader(m_program), type, scopedLambda<void(FunctionID)>([&](FunctionID function) {
            noneCouldBeLetGo &= m_isCapturedByClosureThatNeverDies.quickGet(candidate) || hasOneClosureThatNeverDies(function);
            if (!m_isCapturedByClosureReachableForGood.quickGet(candidate))
                m_candidatesByFunctionHeld.append({ function.index(), candidate });
        }));
        return noneCouldBeLetGo;
    }
    if (!m_isCapturedByClosureStoredForGood.quickGet(candidate))
        return false;
    m_servesBecauseClassIsStoredForGood.quickSet(candidate);
    return true;
}

void PlannerOfPromotedEnvironments::noteThatFunctionHoldsWord(uint32_t function)
{
    if (!m_holdsWord.quickSet(function))
        m_functionsThatHaveComeToHoldWord.append(function);
}

void PlannerOfPromotedEnvironments::strike(Candidate candidate)
{
    if (m_isStruck.quickSet(candidate))
        return;
    for (const FunctionAndCandidate& capturing : std::ranges::equal_range(m_functionsByCandidateCaptured, candidate, { }, &FunctionAndCandidate::candidate))
        noteThatFunctionHoldsWord(capturing.function);
}

void PlannerOfPromotedEnvironments::keepWhatCouldBeLetGo()
{
    Candidate numberOfCandidates = static_cast<Candidate>(m_sources.size() + m_scopes.size());
    m_isStruck = BitVector(numberOfCandidates);
    m_isCapturedByClosureThatNeverDies = BitVector(numberOfCandidates);
    m_isCapturedByClosureStoredForGood = BitVector(numberOfCandidates);
    m_servesBecauseClassIsStoredForGood = BitVector(numberOfCandidates);
    m_isCapturedByClosureReachableForGood = BitVector(numberOfCandidates);
    if (!numberOfCandidates)
        return;

    for (uint32_t number : m_functionsNamed) {
        FunctionID id(number);
        const Function& function = m_program.functions[number];
        if (function.captures.isEmpty())
            continue;
        bool mayBeLeftOut = capturesMayBeLeftOut(id);
        FunctionID madeOnEveryWayBy = makerThatMakesOnEveryWay(id);
        for (uint32_t capture = 0; capture < function.captures.size(); ++capture) {
            std::optional<Candidate> candidate = mayBeLeftOut || madeOnEveryWayBy ? candidateOfCapture(id, capture) : std::nullopt;
            if (candidate && madeOnEveryWayBy == makerOf(*candidate)) {
                m_isCapturedByClosureThatNeverDies.quickSet(*candidate);
                if (isStoredForGood(id))
                    m_isCapturedByClosureStoredForGood.quickSet(*candidate);
                if (m_closuresReachableForGood.get(number))
                    m_isCapturedByClosureReachableForGood.quickSet(*candidate);
            }
            if (candidate && mayBeLeftOut)
                m_functionsByCandidateCaptured.append({ number, *candidate });
            else
                noteThatFunctionHoldsWord(number);
        }
    }

    Vector<Candidate> struckFromStart;
    for (Candidate candidate = 0; candidate < m_sources.size(); ++candidate) {
        const Source& source = m_sources[candidate];
        auto mayBeAbsent = [&](TypeID type) {
            return type && (m_program.at(type).primitives & Primitive::Absent);
        };
        bool holdsClosures = false;
        bool serves = source.readersAgree && (m_program.at(source.types.established).isJavaScriptValue() || isClassToken(source.types.established)) && !mayBeAbsent(source.types.established) && !mayBeAbsent(source.types.proven)
            && listFunctionsWhoseClosuresAreHeld(candidate, source.types, holdsClosures);
        if (!serves || (holdsClosures && source.mayLieInFrameOfCaller))
            struckFromStart.append(candidate);
    }
    for (uint32_t place = 0; place < m_scopes.size(); ++place) {
        Candidate candidate = static_cast<Candidate>(m_sources.size() + place);
        LayoutID scope = m_scopes[place].layout;
        const Layout& layout = m_program.at(scope);
        bool serves = !m_isRefused.quickGet(scope.index());
        for (uint32_t field = 0; serves && field < layout.fields.size(); ++field) {
            bool holdsClosures = false;
            serves = listFunctionsWhoseClosuresAreHeld(candidate, { m_program.provenTypeOf(layout.fields[field].declared), m_facts.linked().provenOfField({ scope, field }) }, holdsClosures) || m_isCapturedByClosureReachableForGood.quickGet(candidate);
        }
        if (!serves)
            struckFromStart.append(candidate);
    }
    AOT::sort(m_candidatesByFunctionHeld, { }, &FunctionAndCandidate::function);
    AOT::sort(m_functionsByCandidateCaptured, { }, &FunctionAndCandidate::candidate);
    for (const FunctionAndCandidate& held : m_candidatesByFunctionHeld) {
        if (!m_isNamed.quickGet(held.function) && !m_program.functions[held.function].captures.isEmpty())
            noteThatFunctionHoldsWord(held.function);
    }

    for (Candidate candidate : struckFromStart)
        strike(candidate);
    while (!m_functionsThatHaveComeToHoldWord.isEmpty()) {
        uint32_t function = m_functionsThatHaveComeToHoldWord.takeLast();
        for (const FunctionAndCandidate& holding : std::ranges::equal_range(m_candidatesByFunctionHeld, function, { }, &FunctionAndCandidate::function))
            strike(holding.candidate);
    }
}

VariableID PlannerOfPromotedEnvironments::addVariable(PromotedVariable&& variable)
{
    m_result.makers.set(variable.maker.index());
    m_result.variables.append(WTF::move(variable));
    return VariableID(m_result.firstVariable.index() + static_cast<uint32_t>(m_result.variables.size()) - 1);
}

void PlannerOfPromotedEnvironments::leaveOutCaptures()
{
    for (uint32_t number : m_functionsNamed) {
        FunctionID id(number);
        const Function& function = m_program.functions[number];
        if (function.captures.isEmpty() || !capturesMayBeLeftOut(id))
            continue;
        uint32_t numberRemoved = 0;
        for (uint32_t capture = 0; capture < function.captures.size(); ++capture) {
            std::optional<Candidate> candidate = candidateOfCapture(id, capture);
            if (!candidate || m_isStruck.quickGet(*candidate))
                continue;
            VariableID variable;
            if (*candidate < m_sources.size()) {
                Source& source = m_sources[*candidate];
                if (!source.variable)
                    source.variable = addVariable(PromotedVariable { m_program.at(source.function).module, m_program.at(source.firstReader).captures[source.captureOfFirstReader].variableName, { }, source.function, { }, 0, source.firstReader, source.captureOfFirstReader });
                variable = source.variable;
            }
            m_result.removedCaptures.append(RemovedCapture { id, capture, variable });
            ++numberRemoved;
        }
        if (!numberRemoved)
            continue;
        if (m_result.numbersOfRemovedCaptures.isEmpty())
            m_result.numbersOfRemovedCaptures.fill(0, m_program.functions.size());
        m_result.numbersOfRemovedCaptures[number] = numberRemoved;
    }
}

void PlannerOfPromotedEnvironments::listStores()
{
    for (const Source& source : m_sources) {
        if (source.variable)
            m_result.stores.append(StoreOfCapturedValue { source.function, source.value, source.variable });
    }
    AOT::sort(m_result.stores, { }, [](const StoreOfCapturedValue& store) {
        return std::pair { store.function, store.value };
    });
}

PromotedEnvironments PlannerOfPromotedEnvironments::run(std::span<const LayoutID> scopesTakenByClosuresMadeForCallsAlone)
{
    if (!goThroughWhatStandsInFunctionsThatRunOnce())
        return { };
    for (LayoutID scope : scopesTakenByClosuresMadeForCallsAlone)
        m_isRefused.quickSet(scope.index());
    if (!m_scopes.isEmpty())
        refuseScopesThatPositionsHold();
    AOT::sort(m_scopes, { }, &ScopeMadeOnce::layout);
    keepWhatCouldBeLetGo();

    m_result.firstVariable = VariableID(static_cast<uint32_t>(m_program.variables.size()));
    for (uint32_t place = 0; place < m_scopes.size(); ++place) {
        const ScopeMadeOnce& scope = m_scopes[place];
        if (m_isStruck.quickGet(m_sources.size() + place))
            continue;
        const Layout& layout = m_program.at(scope.layout);
        m_result.layouts.append(PromotedLayout { scope.layout, VariableID(m_result.firstVariable.index() + static_cast<uint32_t>(m_result.variables.size())) });
        m_result.makers.set(scope.maker.index());
        for (uint32_t field = 0; field < layout.fields.size(); ++field)
            addVariable(PromotedVariable { m_program.at(scope.maker).module, layout.fields[field].variableName, { }, scope.maker, scope.layout, field, { }, 0 });
    }
    leaveOutCaptures();
    listStores();
    for (size_t candidate : m_servesBecauseClassIsStoredForGood)
        m_result.numberOfCapturesOfClassesStoredForGood += !m_isStruck.quickGet(candidate);
    return WTF::move(m_result);
}

} // anonymous namespace

PromotedEnvironments planPromotedEnvironments(const Program& program, const Facts& facts, const SlotPlanParts& slots, std::span<const LayoutID> scopesTakenByClosuresMadeForCallsAlone)
{
    if (!facts.mechanisms().contains(Mechanism::PromotedEnvironments))
        return { };
    return PlannerOfPromotedEnvironments(program, facts, slots).run(scopesTakenByClosuresMadeForCallsAlone);
}

void appendPromotedVariables(const PromotedEnvironments& promoted, SlotPlanParts& slots)
{
    for (const PromotedVariable& variable : promoted.variables) {
        PlannedVariable planned;
        if (variable.layout)
            planned.representation = slots.layouts[variable.layout.index()].slots[variable.field].representation;
        SlotTraits traits = traitsOf(planned.representation);
        if (traits.hasAbsent)
            planned.initialWord = traits.absentBits;
        slots.variables.append(planned);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The scope of a function that runs at most once is never made: its variables lie among the variables of the module, and a closure over it captures nothing.
// Only what nobody could have let go lies there: numbers, booleans, undefined, null, and a closure that the program holds for good anyway.
// A word of the instance lives as long as the program. An object, a list or a text that closures have captured dies with them, so it keeps its scope or its capture.
// A closure holds its captures and whatever hangs on the function object: own properties, the prototype object, a parent. Link can list neither once a built-in is handed the function.
// The one closure of a constructor function never dies in any build: the closure table takes it among its roots when its prototype property is made (ClassTable::didCreatePrototypePropertyOf()).
// So a closure lies in a word where it is such a closure, or where such a closure captures it, by value or in its scope: what a closure that never dies has captured never dies.
// The closure that captures it counts only where the function that fills the word makes it on every way from its entry to a return: that it is written does not say that it is made.
// Its function holds no word itself once this plan is through. The forms stay: an object, a list or a text comes back by neither door.
// Where something throws in front of the closure that never dies, or its prototype property is never made, the word holds what nothing else would have held.
// A constructor function that loses its last capture is in the closure table from its making on, and not only from the first time its prototype property is asked for.
// Suspension: a coroutine neither makes such a scope nor loses a capture. Throw: a store that was reached stays, as it would have stayed in the cell.
// Enforced: the maker stops the program by name at its second entry (EnterBodyThatRunsOnce), is copied into no caller and is never written straight.
// The body of a module has no such stop: that the loader enters it at most once rests on bodiesThatLoaderEntersAtMostOnce().
// Effects: Link counts the words that such a function may fill among its writes (knownEffectsOfBody()), by what the function is and without this plan.
// Withheld (Mechanism::PromotedEnvironments): the tables are empty, and every reader goes by the captures as they are written.

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
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FunctionsWhoseClosuresOperationMakes {
    template<typename IDType, typename... Role>
    void operator()(IDType id, Role... role)
    {
        if constexpr (std::is_same_v<IDType, FunctionID>) {
            if (id && ((role == FunctionReferenceKind::Closure) || ...))
                functions.set(id.index());
        }
    }

    BitVector& functions;
};

class PlannerOfPromotedEnvironments {
public:
    PlannerOfPromotedEnvironments(const Program& program, const Facts& facts, const SlotPlanParts& slots)
        : m_program(program)
        , m_facts(facts)
        , m_slots(slots)
        , m_isMade(program.layouts.size())
        , m_isRefused(program.layouts.size())
        , m_holdsWord(program.functions.size())
    {
    }

    PromotedEnvironments run();

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

    // A value or a scope that may come to lie among the variables. The values come first, in the order of m_sources, then the scopes in the order of m_scopes.
    using Candidate = uint32_t;
    struct FunctionAndCandidate {
        uint32_t function;
        Candidate candidate;
    };

    struct HolderMadeOnEveryWay {
        FunctionID holder;
        FunctionID maker;
    };

    bool servesAsMaker(FunctionID, const ThinSummary&, const BodyIndex&) const;
    bool mayLieAmongVariables(LayoutID) const;
    bool capturesMayBeLeftOut(FunctionID) const;
    TypesOfCapture typesOfCapture(FunctionID function, uint32_t capture) const
    {
        const LinkedCell& cell = m_facts.linked().capture(function, capture);
        return { m_program.provenTypeOf(m_program.at(function).captures[capture]), cell.everyWriterIsKnown ? cell.proven : TypeID() };
    }

    void findScopesMadeOnce();
    void refuseScopesThatPositionsHold();

    uint32_t nodeOf(FunctionID function, uint32_t capture) const { return m_facts.linked().firstCaptureOfFunction[function.index()] + capture; }
    uint32_t rootOf(uint32_t node);
    void unite(uint32_t, uint32_t);
    void addSource(uint32_t node, uint32_t source);
    bool mayLieInFrameOfCaller(FunctionID, const BodyIndex&, ValueID) const;
    bool followCapturedValues();

    std::optional<Candidate> candidateOfCapture(FunctionID, uint32_t capture);
    FunctionID makerOf(Candidate candidate) const { return candidate < m_sources.size() ? m_sources[candidate].function : m_scopes[candidate - m_sources.size()].maker; }
    bool hasOneClosureThatNeverDies(FunctionID) const;
    FunctionID makerThatMakesOnEveryWay(FunctionID holder) const;
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
    BitVector m_functionsWhoseClosuresRunTimeMakes;
    Vector<ScopeMadeOnce> m_scopes;
    Vector<uint32_t> m_parents;
    Vector<uint32_t> m_sourcesOfClasses;
    Vector<uint32_t> m_sourcesOfValues;
    Vector<Source> m_sources;
    Vector<HolderMadeOnEveryWay> m_holdersMadeOnEveryWay;
    BitVector m_isStruck;
    BitVector m_isCapturedByClosureThatNeverDies;
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
    // The run time puts the state of a module of the library back and runs its body again when the body has thrown.
    return m_program.at(function.module).kind != ModuleKind::RuntimeSource;
}

bool PlannerOfPromotedEnvironments::mayLieAmongVariables(LayoutID id) const
{
    const Layout& layout = m_program.at(id);
    const PlannedLayout& planned = m_slots.layouts[id.index()];
    if (layout.kind != LayoutKind::Environment || planned.storage != LayoutStorage::Environment || planned.liesOnlyInFrames)
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
    // The record of a class makes the closures of members over the word that the class object captures.
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PlannerOfPromotedEnvironments::findScopesMadeOnce()
{
    const ThinSummaries& summaries = m_facts.summaries();
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        FunctionID id(number);
        const ThinSummary& summary = summaries.at(id);
        const BodyIndex* index = summary.environmentsMade.isEmpty() ? nullptr : m_facts.bodies().find(id);
        if (!index)
            continue;
        bool serves = servesAsMaker(id, summary, *index);
        for (ValueID site : summary.environmentsMade) {
            LayoutID layout = index->value(site).as<Opcode::NewObject>().layout();
            if (m_isMade.quickSet(layout.index()) || !serves || index->loopOf(index->blockOf(site)) || !mayLieAmongVariables(layout))
                m_isRefused.quickSet(layout.index());
            else
                m_scopes.append({ layout, id });
        }
    }
}

void PlannerOfPromotedEnvironments::refuseScopesThatPositionsHold()
{
    auto scopeIn = [&](const DeclaredType& declared) -> std::pair<LayoutID, bool> {
        TypeID written = declared.written();
        if (!written || holdsOnlyPrimitives(written))
            return { };
        Type type = m_program.at(written);
        return { type.environment, !!type.primitives };
    };
    auto refuse = [&](const DeclaredType& declared) {
        if (LayoutID scope = scopeIn(declared).first)
            m_isRefused.quickSet(scope.index());
    };

    for (const Layout& layout : m_program.layouts) {
        if (!layout.isEnvironment())
            continue;
        for (const Field& field : layout.fields)
            refuse(field.declared);
    }
    for (const Variable& variable : m_program.variables)
        refuse(variable.declared);
    for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
        const Function& function = m_program.functions[number];
        refuse(function.thisType);
        for (const DeclaredType& parameter : function.parameters)
            refuse(parameter);
        for (const DeclaredType& result : function.results)
            refuse(result);
        bool keepsCaptures = !function.captures.isEmpty() && !capturesMayBeLeftOut(FunctionID(number));
        for (const DeclaredCapture& capture : function.captures) {
            auto [scope, mayBeNoScope] = scopeIn(capture);
            if (scope && (keepsCaptures || mayBeNoScope))
                m_isRefused.quickSet(scope.index());
        }
    }
    for (const EvalScope& scope : m_program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (binding.layout)
                m_isRefused.quickSet(binding.layout.index());
        }
    }
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

// A caller makes the closure that it hands to a parameter that is only called in its own frame (findLoanOfClosure()). Link judges that on the body as written, where no store into a word stands.
bool PlannerOfPromotedEnvironments::mayLieInFrameOfCaller(FunctionID id, const BodyIndex& index, ValueID value) const
{
    while (!index.isParameter(value) && inputOperationsThatHandOn.contains(index.opcodeOf(value)) && index.value(value).operand(0))
        value = index.value(value).operand(0);
    if (!index.isParameter(value))
        return false;
    std::optional<uint32_t> entryParameter = index.block(entryBlock).indexOfParameter(value);
    return !entryParameter || m_facts.linked().onlyCallsWhatArrivesAt(id, *entryParameter);
}

// Every way from the entry to a return goes through the blocks that stand in front of all the blocks that return.
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

// Captures that are handed on from one function to the next hold one value. The classes are united in one go over the sites that the scan has listed.
// The source of a class is the operand as it is written. A name behind a test or a check is another value than the one that was tested: it has a type of its own.
bool PlannerOfPromotedEnvironments::followCapturedValues()
{
    const LinkTables& linked = m_facts.linked();
    if (linked.firstCaptureOfFunction.size() != m_program.functions.size() + 1)
        return false;
    uint32_t numberOfCaptures = linked.firstCaptureOfFunction.last();
    m_parents = Vector<uint32_t>(numberOfCaptures, [](size_t node) {
        return static_cast<uint32_t>(node);
    });
    m_sourcesOfClasses.fill(noSource, numberOfCaptures);

    const ThinSummaries& summaries = m_facts.summaries();
    for (uint32_t number = 0; number < summaries.size(); ++number) {
        FunctionID id(number);
        const ThinSummary& summary = summaries.at(id);
        if (summary.sitesThatHandOverCaptures.isEmpty())
            continue;
        const BodyIndex* index = m_facts.bodies().find(id);
        if (!index)
            return false;
        bool serves = servesAsMaker(id, summary, *index);
        if (serves)
            m_sourcesOfValues.fill(noSource, index->numberOfValues());
        BodyBlock entry = index->block(entryBlock);
        uint32_t numberOfOwnCaptures = static_cast<uint32_t>(m_program.at(id).captures.size());
        std::optional<BlockID> lastBlockOnEveryWayToReturn;
        for (ValueID site : summary.sitesThatHandOverCaptures) {
            BodyValue value = index->value(site);
            bool makesClosure = value.opcode() == Opcode::NewClosure;
            if (!makesClosure && value.opcode() != Opcode::CallDirect) {
                FunctionsWhoseClosuresOperationMakes made { m_functionsWhoseClosuresRunTimeMakes };
                value.forEachReference(made);
                continue;
            }
            FunctionID callee = makesClosure ? value.as<Opcode::NewClosure>().function() : value.as<Opcode::CallDirect>().function();
            std::span<const ValueID> operands = makesClosure ? value.as<Opcode::NewClosure>().capturesOperands() : value.as<Opcode::CallDirect>().argumentsOperands();
            uint32_t numberOfCapturesOfCallee = static_cast<uint32_t>(m_program.at(callee).captures.size());
            if (makesClosure && serves && hasOneClosureThatNeverDies(callee)) {
                if (!lastBlockOnEveryWayToReturn)
                    lastBlockOnEveryWayToReturn = lastBlockOnEveryWayToReturnForPromotedEnvironments(*index);
                if (*lastBlockOnEveryWayToReturn && index->dominates(index->blockOf(site), *lastBlockOnEveryWayToReturn) && !index->block(index->blockOf(site)).hasHandler())
                    m_holdersMadeOnEveryWay.append({ callee, id });
            }
            for (uint32_t capture = 0; capture < numberOfCapturesOfCallee; ++capture) {
                TypesOfCapture types = typesOfCapture(callee, capture);
                if (m_program.at(types.established).environment)
                    continue;
                uint32_t node = nodeOf(callee, capture);
                ValueID handed = capture < operands.size() ? operands[capture] : ValueID();
                ValueID behindCasts = handed;
                while (behindCasts && !index->isParameter(behindCasts) && castsThatClaimNothing.contains(index->opcodeOf(behindCasts)))
                    behindCasts = index->value(behindCasts).operand(0);
                std::optional<uint32_t> entryParameterBehindCasts = behindCasts && index->isParameter(behindCasts) ? entry.indexOfParameter(behindCasts) : std::nullopt;
                if (entryParameterBehindCasts && *entryParameterBehindCasts < numberOfOwnCaptures) {
                    unite(node, nodeOf(id, *entryParameterBehindCasts));
                    continue;
                }
                std::optional<uint32_t> entryParameter = handed && index->isParameter(handed) ? entry.indexOfParameter(handed) : std::nullopt;
                bool isJoinOfValues = handed && index->isParameter(handed) && !entryParameter;
                bool isLentByCaller = entryParameter && linked.onlyCallsWhatArrivesAt(id, *entryParameter);
                if (!handed || !serves || isJoinOfValues || isLentByCaller || index->loopOf(index->blockOf(handed))) {
                    addSource(node, noSourceServes);
                    continue;
                }
                uint32_t& source = m_sourcesOfValues[handed.index()];
                if (source == noSource) {
                    m_sources.append(Source { id, handed, callee, capture, types, true, mayLieInFrameOfCaller(id, *index, handed), { } });
                    source = firstSource + static_cast<uint32_t>(m_sources.size()) - 1;
                } else
                    m_sources[source - firstSource].readersAgree &= m_sources[source - firstSource].types == types;
                addSource(node, source);
            }
        }
    }
    AOT::sort(m_holdersMadeOnEveryWay, { }, &HolderMadeOnEveryWay::holder);
    return true;
}

std::optional<PlannerOfPromotedEnvironments::Candidate> PlannerOfPromotedEnvironments::candidateOfCapture(FunctionID id, uint32_t capture)
{
    TypesOfCapture types = typesOfCapture(id, capture);
    if (LayoutID scope = m_program.at(types.established).environment) {
        auto found = std::ranges::lower_bound(m_scopes, scope, { }, &ScopeMadeOnce::layout);
        if (found == m_scopes.end() || found->layout != scope)
            return std::nullopt;
        return static_cast<Candidate>(m_sources.size() + (found - m_scopes.begin()));
    }
    uint32_t found = m_sourcesOfClasses[rootOf(nodeOf(id, capture))];
    if (found < firstSource || m_sources[found - firstSource].types != types)
        return std::nullopt;
    return found - firstSource;
}

bool PlannerOfPromotedEnvironments::hasOneClosureThatNeverDies(FunctionID function) const
{
    LayoutID constructed = classOfConstructorFunction(m_program, function);
    return constructed && m_facts.isEvaluatedAtMostOnce(constructed);
}

FunctionID PlannerOfPromotedEnvironments::makerThatMakesOnEveryWay(FunctionID holder) const
{
    auto found = std::ranges::lower_bound(m_holdersMadeOnEveryWay, holder, { }, &HolderMadeOnEveryWay::holder);
    return found != m_holdersMadeOnEveryWay.end() && found->holder == holder ? found->maker : FunctionID();
}

bool PlannerOfPromotedEnvironments::listFunctionsWhoseClosuresAreHeld(Candidate candidate, TypesOfCapture types, bool& holdsClosures)
{
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
            m_candidatesByFunctionHeld.append({ function.index(), candidate });
        }));
        return noneCouldBeLetGo;
    }
    return false;
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

// The greatest set: everything is taken to lie among the variables, and what holds a closure that keeps a word is struck out, which leaves the functions over it with a word.
void PlannerOfPromotedEnvironments::keepWhatCouldBeLetGo()
{
    Candidate numberOfCandidates = static_cast<Candidate>(m_sources.size() + m_scopes.size());
    m_isStruck = BitVector(numberOfCandidates);
    m_isCapturedByClosureThatNeverDies = BitVector(numberOfCandidates);
    if (!numberOfCandidates)
        return;

    for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
        FunctionID id(number);
        const Function& function = m_program.functions[number];
        if (function.captures.isEmpty())
            continue;
        bool mayBeLeftOut = capturesMayBeLeftOut(id);
        FunctionID madeOnEveryWayBy = makerThatMakesOnEveryWay(id);
        for (uint32_t capture = 0; capture < function.captures.size(); ++capture) {
            std::optional<Candidate> candidate = mayBeLeftOut || madeOnEveryWayBy ? candidateOfCapture(id, capture) : std::nullopt;
            if (candidate && madeOnEveryWayBy == makerOf(*candidate))
                m_isCapturedByClosureThatNeverDies.quickSet(*candidate);
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
        bool serves = source.readersAgree && m_program.at(source.types.established).isJavaScriptValue() && !mayBeAbsent(source.types.established) && !mayBeAbsent(source.types.proven)
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
            serves = listFunctionsWhoseClosuresAreHeld(candidate, { m_program.provenTypeOf(layout.fields[field].declared), m_facts.linked().provenOfField({ scope, field }) }, holdsClosures);
        }
        if (!serves)
            struckFromStart.append(candidate);
    }
    AOT::sort(m_candidatesByFunctionHeld, { }, &FunctionAndCandidate::function);
    AOT::sort(m_functionsByCandidateCaptured, { }, &FunctionAndCandidate::candidate);

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
    for (uint32_t number = 0; number < m_program.functions.size(); ++number) {
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

PromotedEnvironments PlannerOfPromotedEnvironments::run()
{
    if (!followCapturedValues())
        return { };
    findScopesMadeOnce();
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
    return WTF::move(m_result);
}

} // anonymous namespace

PromotedEnvironments planPromotedEnvironments(const Program& program, const Facts& facts, const SlotPlanParts& slots)
{
    if (!facts.mechanisms().contains(Mechanism::PromotedEnvironments))
        return { };
    return PlannerOfPromotedEnvironments(program, facts, slots).run();
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

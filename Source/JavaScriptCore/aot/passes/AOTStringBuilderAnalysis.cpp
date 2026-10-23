/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderAnalysis.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTDecidedBranches.h"
#include "AOTEdgeResults.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLiveness.h"
#include "AOTPass.h"
#include "AOTRuleSupport.h"
#include "AOTSort.h"
#include "AOTType.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/IterationStatus.h>
#include <wtf/PrintStream.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t noWeb = UINT32_MAX;
static_assert(OperandIndex::Add::left == stateOperandIndex);
static_assert(OperandIndex::Strcat::parts == stateOperandIndex);

std::optional<uint32_t> findWeb(const Vector<uint32_t>& webOfValue, ValueID id)
{
    if (id.index() >= webOfValue.size() || webOfValue[id.index()] == noWeb)
        return std::nullopt;
    return webOfValue[id.index()];
}

ASCIILiteral stateUseNameOf(StringStateUse use)
{
    switch (use) {
    case StringStateUse::Unknown:
        return "Unknown"_s;
    case StringStateUse::Length:
        return "Length"_s;
    case StringStateUse::Truthiness:
        return "Truthiness"_s;
    case StringStateUse::Read:
        return "Read"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void dumpUser(PrintStream& out, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        out.print("v", use.user.index());
        return;
    case Use::Kind::TerminatorOperand:
        out.print("b", use.block.index());
        return;
    case Use::Kind::EdgeArgument:
        out.print("b", use.block.index(), ".", use.targetIndex);
        return;
    case Use::Kind::HandlerArgument:
        out.print("b", use.block.index(), ".handler");
        return;
    case Use::Kind::BlockSubject:
        out.print("b", use.block.index(), ".subject");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool hasStringConcatenation(const Program& program, const Function& function)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            if (isStringConcatenation(program, function, function.at(id)))
                return true;
        }
    }
    return false;
}

struct StateClass : WTF::UnionFind<StateClass> { };

struct WebState {
    Vector<uint32_t, 2> cyclesWithAppends;
    uint32_t maxDepth { 0 };
    bool isCarriedAroundCycle { false };
    bool hasTooFewAppends { false };
};

class StringBuilderAnalyzer {
public:
    StringBuilderAnalyzer(FunctionPassContext&, Vector<StringWeb>&, Vector<uint32_t>& webOfValue);

    void run();

private:
    using LocalCalc = Liveness::LocalCalc;

    void findUnreplaceableUses();
    void findValuesBesideCursors();
    void findValuesBesideCursors(const Liveness&, BlockID);
    void findOnlyConcatenatedParameters();
    bool isOnlyConcatenated(ValueID parameter) const;
    void reset();
    void findCandidates();
    void uniteStates();
    bool cutAtStructure();
    void findCyclesWithAppends();
    void decideCost();
    void checkWebLimit();
    bool cutAtInterference();
    IterationStatus walkBlock(BlockID);
    void visitTerminator(BlockID, const LocalCalc&);
    void visitEdge(ValueID liveMember, Edge);
    IterationStatus visitValue(BlockID, uint32_t index, const LocalCalc&);
    IterationStatus visitConcatenation(uint32_t web, BlockID, uint32_t index);
    IterationStatus visitParameters(BlockID, const LocalCalc&);
    void addRead(uint32_t web, const StringStateRead&);
    void collectEntries();
    void collectEntriesOnEdge(Edge);
    bool wouldReadUnspecifiedFirstState(const StringWeb&) const;
    void sortRecords(StringWeb&) const;
    void reportLimitHits();
    void appendCutValues();

    void addCandidate(ValueID);
    bool cut(ValueID, StringBuilderDeclineReason);
    void setDecision(uint32_t web, StringBuilderDeclineReason, ValueID decidedAt);
    void collectLiveValues(const LocalCalc&);
    StringStateUse classifyUse(ValueID user, uint32_t operandIndex) const;

    std::optional<uint32_t> webOf(ValueID id) const { return findWeb(m_webOfValue, id); }
    std::optional<uint32_t> undecidedWebOf(ValueID) const;
    bool isDecided(uint32_t web) const { return m_webs[web].decision != StringBuilderDeclineReason::Unknown; }
    bool isParameter(ValueID id) const { return m_useLists.locationOf(id).placement == ValuePlacement::Parameter; }
    bool isEmptyString(ValueID) const;
    ValueID argumentOn(Edge, uint32_t parameterIndex) const;
    bool hasMember(uint32_t web, std::span<const ValueID>, ValueID excluded = { }) const;
    bool hasStateAmongParts(uint32_t web, ValueID concatenation) const;
    bool continuesOn(uint32_t web, const Target&) const;
    bool hasAppendInCycle(uint32_t web, BlockID) const;
    bool isPartOfOtherWeb(uint32_t web) const;
    bool isLiveOnlyForHandler(ValueID member, BlockID, uint32_t index) const;

    FunctionPassContext& m_context;
    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    const BlockCycles& m_cycles;
    Vector<StringWeb>& m_webs;
    Vector<uint32_t>& m_webOfValue;
    Vector<WebState> m_states;
    uint32_t m_numberOfUndecidedWebs { 0 };
    Vector<ValueID> m_candidates;
    BitVector m_isCandidate;
    BitVector m_isCut;
    BitVector m_hasUnreplaceableUse;
    Vector<ValueID> m_operationBeforeCursors;
    BitVector m_isOnlyConcatenated;
    Vector<std::pair<ValueID, StringBuilderDeclineReason>> m_cuts;
    std::optional<Liveness> m_liveness;
    Vector<ValueID, 16> m_liveValues;
};

StringBuilderAnalyzer::StringBuilderAnalyzer(FunctionPassContext& context, Vector<StringWeb>& webs, Vector<uint32_t>& webOfValue)
    : m_context(context)
    , m_program(context.program)
    , m_function(context.function)
    , m_cfg(context.analyses.cfg())
    , m_useLists(context.analyses.useLists())
    , m_cycles(context.analyses.blockCycles())
    , m_webs(webs)
    , m_webOfValue(webOfValue)
    , m_isCandidate(context.function.values.size())
    , m_isCut(context.function.values.size())
    , m_hasUnreplaceableUse(context.function.values.size())
    , m_isOnlyConcatenated(context.function.values.size())
{
}

void StringBuilderAnalyzer::run()
{
    findUnreplaceableUses();
    findValuesBesideCursors();
    findOnlyConcatenatedParameters();
    for (;;) {
        reset();
        findCandidates();
        uniteStates();
        if (cutAtStructure())
            continue;
        decideCost();
        checkWebLimit();
        if (!cutAtInterference())
            break;
    }
    collectEntries();
    reportLimitHits();
    appendCutValues();
}

// The use lists do not list a narrowed operand, and neither it nor the subject of a block can take a replacement.
void StringBuilderAnalyzer::findUnreplaceableUses()
{
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        const Block& block = m_function.at(blockID);
        if (block.subject)
            m_hasUnreplaceableUse.quickSet(block.subject.index());
        for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex) {
            uint32_t numberOfResults = numberOfEdgeResults(m_function, blockID, targetIndex);
            for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
                EdgeResult result = edgeResultAt(m_function, { blockID, targetIndex, resultIndex });
                switch (result.kind) {
                case EdgeResultKind::NarrowedOperand:
                    m_hasUnreplaceableUse.quickSet(result.subject.index());
                    break;
                case EdgeResultKind::Guard:
                case EdgeResultKind::DeclaredResult:
                    break;
                }
            }
        }
    }
}

// A builder operation between the definition of a cursor and a use of it makes the cursor stale, whichever builder it works on.
void StringBuilderAnalyzer::findValuesBesideCursors()
{
    BitVector cursors(m_function.values.size());
    auto addIfCursor = [&](ValueID id) {
        if (m_program.typeOf(m_function, id).opaque == OpaqueKind::BuilderCursor)
            cursors.quickSet(id.index());
    };
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        std::ranges::for_each(m_function.at(blockID).parameters, addIfCursor);
        std::ranges::for_each(m_function.at(blockID).values, addIfCursor);
    }
    if (cursors.isEmpty())
        return;

    LivenessOptions options;
    options.trackedValues = &cursors;
    Liveness liveness(m_function, m_cfg, m_useLists, options);
    m_operationBeforeCursors.fill(ValueID(), m_function.values.size());
    for (BlockID blockID : m_cfg.reversePostOrder())
        findValuesBesideCursors(liveness, blockID);
}

void StringBuilderAnalyzer::findValuesBesideCursors(const Liveness& liveness, BlockID blockID)
{
    const Block& block = m_function.at(blockID);
    uint32_t numberOfValues = block.values.size();
    LocalCalc live(liveness, blockID);
    auto hasLiveCursor = [&] {
        bool hasAny = false;
        live.forEachLive([&](ValueID) {
            hasAny = true;
        });
        return hasAny;
    };
    auto markAsUnreplaceable = [&](ValueID id) {
        if (id)
            m_hasUnreplaceableUse.quickSet(id.index());
    };

    BitVector hasCursorInFront(numberOfValues);
    if (hasLiveCursor()) {
        block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
            markAsUnreplaceable(operand);
        });
        forEachOutgoingEdge(m_function, blockID, [&](Edge, const Target& target) {
            std::ranges::for_each(target.arguments, markAsUnreplaceable);
        });
    }
    for (uint32_t index = numberOfValues; index--;) {
        live.execute(index);
        if (hasLiveCursor())
            hasCursorInFront.quickSet(index);
    }
    bool entersWithCursor = numberOfValues ? hasCursorInFront.quickGet(0) : hasLiveCursor();
    if (entersWithCursor)
        std::ranges::for_each(block.parameters, markAsUnreplaceable);

    std::optional<uint32_t> indexBeforeCursors;
    for (uint32_t index = 0; index < numberOfValues; ++index) {
        ValueID id = block.values[index];
        if (!hasCursorInFront.quickGet(index)) {
            indexBeforeCursors = index;
            continue;
        }
        markAsUnreplaceable(id);
        m_function.at(id).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (!operand)
                return;
            ValueLocation location = m_useLists.locationOf(operand);
            bool isDefinedTooLate = location.placement == ValuePlacement::InBlock && location.block == blockID && indexBeforeCursors && location.index >= *indexBeforeCursors;
            if (!indexBeforeCursors || isDefinedTooLate || classifyUse(id, operandIndex) != StringStateUse::Read)
                markAsUnreplaceable(operand);
        });
        if (indexBeforeCursors)
            m_operationBeforeCursors[id.index()] = block.values[*indexBeforeCursors];
    }
}

// Whatever happens to such a parameter converts it to a string, so nobody can tell when it was converted. It can be a state although undefined or a number may flow in.
void StringBuilderAnalyzer::findOnlyConcatenatedParameters()
{
    Vector<ValueID, 8> parameters;
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        if (blockID == m_cfg.root())
            continue;
        for (ValueID parameter : m_function.at(blockID).parameters.subspan(0, numberOfSharedExplicitParameters(m_function, m_cfg, blockID))) {
            const Type& type = m_program.typeOf(m_function, parameter);
            if (type.isString() || !isStringBuilderPart(type) || m_hasUnreplaceableUse.quickGet(parameter.index()))
                continue;
            parameters.append(parameter);
            m_isOnlyConcatenated.quickSet(parameter.index());
        }
    }

    for (bool didRemove = true; didRemove;) {
        didRemove = parameters.removeAllMatching([&](ValueID parameter) {
            if (isOnlyConcatenated(parameter))
                return false;
            m_isOnlyConcatenated.quickClear(parameter.index());
            return true;
        });
    }
}

bool StringBuilderAnalyzer::isOnlyConcatenated(ValueID parameter) const
{
    return std::ranges::all_of(m_useLists.usesOf(parameter), [&](const Use& use) {
        if (use.isEdgeUse())
            return m_isOnlyConcatenated.quickGet(m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex].index());
        return use.kind == Use::Kind::Operand && isStringConcatenation(m_program, m_function, m_function.at(use.user));
    });
}

void StringBuilderAnalyzer::reset()
{
    m_webs.clear();
    m_webOfValue.clear();
    m_states.clear();
    m_numberOfUndecidedWebs = 0;
    m_candidates.shrink(0);
    m_isCandidate.clearAll();
    m_liveness.reset();
}

std::optional<uint32_t> StringBuilderAnalyzer::undecidedWebOf(ValueID id) const
{
    std::optional<uint32_t> web = webOf(id);
    if (web && isDecided(*web))
        return std::nullopt;
    return web;
}

bool StringBuilderAnalyzer::isEmptyString(ValueID id) const
{
    return stringConstantOf(m_program, m_function, id) == m_program.strings.emptyString();
}

ValueID StringBuilderAnalyzer::argumentOn(Edge edge, uint32_t parameterIndex) const
{
    IncomingValue incoming = incomingValueOf(m_function, edge, parameterIndex);
    RELEASE_ASSERT(incoming.kind == IncomingValue::Kind::Argument);
    return incoming.argument;
}

bool StringBuilderAnalyzer::hasMember(uint32_t web, std::span<const ValueID> values, ValueID excluded) const
{
    return std::ranges::any_of(values, [&](ValueID id) {
        return id != excluded && webOf(id) == web;
    });
}

bool StringBuilderAnalyzer::hasStateAmongParts(uint32_t web, ValueID concatenation) const
{
    bool hasState = false;
    m_function.at(concatenation).forEachOperand([&](ValueID part, uint32_t operandIndex) {
        hasState |= operandIndex != stateOperandIndex && webOf(part) == web;
    });
    return hasState;
}

bool StringBuilderAnalyzer::continuesOn(uint32_t web, const Target& target) const
{
    return hasMember(web, target.arguments.span()) || hasMember(web, m_liveness->liveAtHead(target.block));
}

bool StringBuilderAnalyzer::hasAppendInCycle(uint32_t web, BlockID block) const
{
    return m_cycles.isInCycle(block) && std::ranges::binary_search(m_states[web].cyclesWithAppends, *m_cycles.outermostCycleOf(block));
}

// The records of such a web say whether it can continue the other web, however short it is.
bool StringBuilderAnalyzer::isPartOfOtherWeb(uint32_t web) const
{
    return std::ranges::any_of(m_webs[web].members, [&](ValueID member) {
        return std::ranges::any_of(m_useLists.usesOf(member), [&](Use use) {
            while (use.kind == Use::Kind::Operand && m_function.at(use.user).opcode == Opcode::Upcast) {
                std::optional<Use> next = m_useLists.singleUse(use.user);
                if (!next)
                    return false;
                use = *next;
            }
            if (use.kind != Use::Kind::Operand || use.operandIndex == stateOperandIndex)
                return false;
            std::optional<uint32_t> other = webOf(use.user);
            return other && other != web;
        });
    });
}

// The append that replaces a concatenation completes or leaves the content as it was, so the builder holds the member wherever the block can throw.
bool StringBuilderAnalyzer::isLiveOnlyForHandler(ValueID member, BlockID blockID, uint32_t index) const
{
    const Block& block = m_function.at(blockID);
    if (!block.handler || !block.handler->arguments.contains(member))
        return false;

    bool hasLaterUse = std::ranges::any_of(m_useLists.usesOf(member), [&](const Use& use) {
        return use.block == blockID && m_useLists.pointOf(use).index > index;
    });
    if (hasLaterUse)
        return false;

    bool isLiveInTarget = std::ranges::any_of(block.terminator.targets, [&](const Target& target) {
        return m_liveness->isLiveAtHead(member, target.block);
    });
    if (isLiveInTarget)
        return false;

    bool mayThrowLater = std::ranges::any_of(block.values.span().subspan(index + 1), [&](ValueID id) {
        return m_context.analyses.mayThrow(id);
    });
    return !mayThrowLater && !m_context.analyses.effectsOfTerminator(blockID).mayThrow();
}

void StringBuilderAnalyzer::setDecision(uint32_t web, StringBuilderDeclineReason decision, ValueID decidedAt)
{
    RELEASE_ASSERT(!isDecided(web) && decision != StringBuilderDeclineReason::Unknown);
    --m_numberOfUndecidedWebs;

    StringWeb& decided = m_webs[web];
    decided.decision = decision;
    decided.decidedAt = decidedAt;
    decided.entries.clear();
    decided.reads.clear();
    decided.discardedEdges.clear();
    decided.discardedAfter.clear();
}

bool StringBuilderAnalyzer::cut(ValueID value, StringBuilderDeclineReason reason)
{
    if (m_cuts.size() >= m_context.limit(LimitID::MaxStringWebsPerFunction)) {
        setDecision(*webOf(value), reason, value);
        return false;
    }
    m_isCut.quickSet(value.index());
    m_cuts.append({ value, reason });
    return true;
}

void StringBuilderAnalyzer::addCandidate(ValueID id)
{
    if (m_isCut.quickGet(id.index()))
        return;
    m_isCandidate.quickSet(id.index());
    m_candidates.append(id);
}

void StringBuilderAnalyzer::findCandidates()
{
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        const Block& block = m_function.at(blockID);
        if (blockID != m_cfg.root()) {
            for (ValueID parameter : block.parameters.subspan(0, numberOfSharedExplicitParameters(m_function, m_cfg, blockID))) {
                if (m_program.typeOf(m_function, parameter).isString() || m_isOnlyConcatenated.quickGet(parameter.index()))
                    addCandidate(parameter);
            }
        }
        for (ValueID id : block.values) {
            if (isStringConcatenation(m_program, m_function, m_function.at(id)))
                addCandidate(id);
        }
    }
}

void StringBuilderAnalyzer::uniteStates()
{
    size_t numberOfValues = m_function.values.size();
    Vector<StateClass> classes(numberOfValues);
    auto rootOf = [&](size_t index) {
        return static_cast<size_t>(classes[index].find() - classes.span().data());
    };

    BitVector concatenations(numberOfValues);
    for (ValueID id : m_candidates) {
        ValueLocation location = m_useLists.locationOf(id);
        if (location.placement != ValuePlacement::Parameter) {
            concatenations.quickSet(id.index());
            ValueID state = m_function.at(id).operand(stateOperandIndex);
            if (m_isCandidate.get(state.index()))
                classes[id.index()].unify(&classes[state.index()]);
            continue;
        }
        for (Edge edge : m_cfg.incomingEdges(location.block)) {
            ValueID argument = argumentOn(edge, location.index);
            if (m_isCandidate.get(argument.index()))
                classes[id.index()].unify(&classes[argument.index()]);
        }
    }
    if (concatenations.isEmpty())
        return;

    BitVector classesWithConcatenation(numberOfValues);
    for (size_t index : concatenations)
        classesWithConcatenation.quickSet(rootOf(index));

    m_webOfValue.fill(noWeb, numberOfValues);
    for (size_t index : m_isCandidate) {
        size_t root = rootOf(index);
        if (!classesWithConcatenation.quickGet(root))
            continue;
        if (m_webOfValue[root] == noWeb) {
            m_webOfValue[root] = static_cast<uint32_t>(m_webs.size());
            m_webs.append(StringWeb { });
        }
        uint32_t web = m_webOfValue[root];
        m_webOfValue[index] = web;
        m_webs[web].members.append(ValueID(index));
    }
    m_states.grow(m_webs.size());
    m_numberOfUndecidedWebs = static_cast<uint32_t>(m_webs.size());
}

bool StringBuilderAnalyzer::cutAtStructure()
{
    bool didCut = false;
    for (uint32_t web = 0; web < m_webs.size(); ++web) {
        for (ValueID id : m_webs[web].members) {
            if (isParameter(id) || !hasStateAmongParts(web, id))
                continue;
            didCut |= cut(id, StringBuilderDeclineReason::StateIsNotFirstOperand);
            break;
        }
    }
    return didCut;
}

void StringBuilderAnalyzer::findCyclesWithAppends()
{
    for (ValueID id : m_candidates) {
        std::optional<uint32_t> web = undecidedWebOf(id);
        ValueLocation location = m_useLists.locationOf(id);
        if (!web || location.placement == ValuePlacement::Parameter || !m_cycles.isInCycle(location.block))
            continue;
        auto& cycles = m_states[*web].cyclesWithAppends;
        uint32_t cycle = *m_cycles.outermostCycleOf(location.block);
        if (cycles.isEmpty() || cycles.last() != cycle)
            cycles.append(cycle);
    }
    for (WebState& state : m_states) {
        auto& cycles = state.cyclesWithAppends;
        AOT::sort(cycles);
        cycles.shrink(cycles.size() - std::ranges::unique(cycles).size());
    }
}

void StringBuilderAnalyzer::decideCost()
{
    if (!m_numberOfUndecidedWebs)
        return;
    findCyclesWithAppends();

    Vector<uint32_t> depths(FillWith { }, m_function.values.size(), 0u);
    for (ValueID id : m_candidates) {
        std::optional<uint32_t> web = undecidedWebOf(id);
        if (!web)
            continue;
        WebState& state = m_states[*web];
        ValueLocation location = m_useLists.locationOf(id);
        uint32_t depth = 0;
        if (location.placement == ValuePlacement::Parameter) {
            for (Edge edge : m_cfg.incomingEdges(location.block)) {
                ValueID argument = argumentOn(edge, location.index);
                if (webOf(argument) != web)
                    continue;
                depth = std::max(depth, depths[argument.index()]);
                bool isRetreatingEdge = m_cfg.reversePostOrderIndex(edge.from) >= m_cfg.reversePostOrderIndex(location.block);
                if (isRetreatingEdge && m_cycles.areInSameCycle(edge.from, location.block) && hasAppendInCycle(*web, location.block))
                    state.isCarriedAroundCycle = true;
            }
        } else {
            ValueID previousState = m_function.at(id).operand(stateOperandIndex);
            depth = webOf(previousState) == web ? depths[previousState.index()] + 1 : 1;
        }
        depths[id.index()] = depth;
        state.maxDepth = std::max(state.maxDepth, depth);
    }

    uint64_t minAppends = m_context.limit(LimitID::MinAppendsForStringBuilder);
    for (uint32_t web = 0; web < m_webs.size(); ++web) {
        WebState& state = m_states[web];
        m_webs[web].numberOfAppends = state.maxDepth;
        if (isDecided(web) || state.isCarriedAroundCycle || state.maxDepth >= minAppends)
            continue;
        state.hasTooFewAppends = true;
        if (!isPartOfOtherWeb(web))
            setDecision(web, StringBuilderDeclineReason::TooFewAppends, m_webs[web].members.first());
    }
}

void StringBuilderAnalyzer::checkWebLimit()
{
    uint64_t maxWebs = m_context.limit(LimitID::MaxStringWebsPerFunction);
    uint64_t numberOfWebs = 0;
    for (uint32_t web = 0; web < m_webs.size(); ++web) {
        if (!isDecided(web) && ++numberOfWebs > maxWebs)
            setDecision(web, StringBuilderDeclineReason::WebLimit, m_webs[web].members.first());
    }
}

bool StringBuilderAnalyzer::cutAtInterference()
{
    if (!m_numberOfUndecidedWebs)
        return false;
    BitVector trackedValues(m_function.values.size());
    BitVector blocksWithMembers(m_cfg.numNodes());
    for (uint32_t web = 0; web < m_webs.size(); ++web) {
        if (isDecided(web))
            continue;
        for (ValueID member : m_webs[web].members) {
            trackedValues.quickSet(member.index());
            blocksWithMembers.quickSet(m_useLists.blockOf(member).index());
        }
    }

    LivenessOptions options;
    options.trackedValues = &trackedValues;
    m_liveness.emplace(m_function, m_cfg, m_useLists, options);
    for (BlockID block : m_cfg.reversePostOrder()) {
        if (!m_numberOfUndecidedWebs)
            return false;
        if (!blocksWithMembers.quickGet(block.index()) && m_liveness->liveAtHead(block).empty())
            continue;
        if (walkBlock(block) == IterationStatus::Done)
            return true;
    }
    return false;
}

IterationStatus StringBuilderAnalyzer::walkBlock(BlockID blockID)
{
    LocalCalc live(*m_liveness, blockID);
    visitTerminator(blockID, live);
    for (uint32_t index = m_function.at(blockID).values.size(); index--;) {
        if (visitValue(blockID, index, live) == IterationStatus::Done)
            return IterationStatus::Done;
        live.execute(index);
    }
    return visitParameters(blockID, live);
}

void StringBuilderAnalyzer::collectLiveValues(const LocalCalc& live)
{
    m_liveValues.shrink(0);
    live.forEachLive([&](ValueID id) {
        m_liveValues.append(id);
    });
}

void StringBuilderAnalyzer::visitTerminator(BlockID blockID, const LocalCalc& live)
{
    collectLiveValues(live);
    Vector<ValueID, 8> liveMembers;
    for (ValueID id : m_liveValues) {
        std::optional<uint32_t> web = undecidedWebOf(id);
        if (web && !hasMember(*web, liveMembers.span()))
            liveMembers.append(id);
    }

    const Terminator& terminator = m_function.at(blockID).terminator;
    for (ValueID liveMember : liveMembers) {
        uint32_t web = *webOf(liveMember);
        bool continues = false;
        forEachOutgoingEdge(m_function, blockID, [&](Edge, const Target& target) {
            continues |= continuesOn(web, target);
        });

        bool isRead = false;
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (webOf(operand) != web)
                return;
            isRead = true;
            addRead(web, { Use { Use::Kind::TerminatorOperand, blockID, { }, operandIndex, 0 }, operand, StringStateUse::Read, !continues });
        });
        if (isRead && !continues)
            continue;

        forEachOutgoingEdge(m_function, blockID, [&](Edge edge, const Target&) {
            visitEdge(liveMember, edge);
        });
    }
}

void StringBuilderAnalyzer::visitEdge(ValueID liveMember, Edge edge)
{
    uint32_t web = *webOf(liveMember);
    const Target& target = targetOf(m_function, edge);
    bool isLiveInTarget = hasMember(web, m_liveness->liveAtHead(target.block));
    auto numberOfPassedMembers = std::ranges::count_if(target.arguments.span(), [&](ValueID argument) {
        return webOf(argument) == web;
    });
    if (!numberOfPassedMembers) {
        if (!isLiveInTarget && !edge.isHandlerEdge())
            m_webs[web].discardedEdges.append(StringStateEdgeDiscard { liveMember, edge });
        return;
    }

    const auto& parameters = m_function.at(target.block).parameters;
    Use::Kind kind = edge.isHandlerEdge() ? Use::Kind::HandlerArgument : Use::Kind::EdgeArgument;
    bool endsWeb = !edge.isHandlerEdge() && !isLiveInTarget && numberOfPassedMembers == 1;
    for (uint32_t index = 0; index < target.arguments.size(); ++index) {
        ValueID argument = target.arguments[index];
        if (webOf(argument) != web || webOf(parameters[index]) == web)
            continue;
        addRead(web, { Use { kind, edge.from, { }, index, edge.targetIndex }, argument, StringStateUse::Read, endsWeb });
    }
}

IterationStatus StringBuilderAnalyzer::visitValue(BlockID blockID, uint32_t index, const LocalCalc& live)
{
    ValueID id = m_function.at(blockID).values[index];
    const Value& value = m_function.at(id);
    bool touchesWeb = undecidedWebOf(id).has_value();
    value.forEachOperand([&](ValueID operand, uint32_t) {
        touchesWeb |= undecidedWebOf(operand).has_value();
    });
    if (!touchesWeb)
        return IterationStatus::Continue;

    collectLiveValues(live);
    if (std::optional<uint32_t> web = undecidedWebOf(id)) {
        if (visitConcatenation(*web, blockID, index) == IterationStatus::Done)
            return IterationStatus::Done;
    }

    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        std::optional<uint32_t> web = undecidedWebOf(operand);
        if (!web || (operandIndex == stateOperandIndex && webOf(id) == web))
            return;
        StringStateUse kind = classifyUse(id, operandIndex);
        bool endsWeb = !hasMember(*web, m_liveValues.span());
        ValueID operationBeforeCursors = m_operationBeforeCursors.isEmpty() ? ValueID() : m_operationBeforeCursors[id.index()];
        addRead(*web, { Use { Use::Kind::Operand, blockID, id, operandIndex, 0 }, operand, kind, endsWeb, operationBeforeCursors });
        if (kind != StringStateUse::Read && endsWeb)
            m_webs[*web].discardedAfter.append(StringStateDiscard { operand, id });
    });
    return IterationStatus::Continue;
}

IterationStatus StringBuilderAnalyzer::visitConcatenation(uint32_t web, BlockID blockID, uint32_t index)
{
    ValueID id = m_function.at(blockID).values[index];
    bool hasOtherLiveState = m_hasUnreplaceableUse.quickGet(id.index()) || std::ranges::any_of(m_liveValues, [&](ValueID other) {
        return other != id && webOf(other) == web && !isLiveOnlyForHandler(other, blockID, index);
    });
    if (hasOtherLiveState)
        return cut(id, StringBuilderDeclineReason::TwoStatesLive) ? IterationStatus::Done : IterationStatus::Continue;

    if (!hasMember(web, m_liveValues.span()))
        m_webs[web].discardedAfter.append(StringStateDiscard { id, id });
    return IterationStatus::Continue;
}

IterationStatus StringBuilderAnalyzer::visitParameters(BlockID blockID, const LocalCalc& live)
{
    const Block& block = m_function.at(blockID);
    for (ValueID parameter : block.parameters) {
        std::optional<uint32_t> web = undecidedWebOf(parameter);
        if (!web)
            continue;
        bool hasOtherLiveState = m_hasUnreplaceableUse.quickGet(parameter.index())
            || hasMember(*web, m_liveness->liveAtHead(blockID))
            || hasMember(*web, block.parameters.span(), parameter);
        if (hasOtherLiveState) {
            if (cut(parameter, StringBuilderDeclineReason::TwoStatesLive))
                return IterationStatus::Done;
            continue;
        }
        if (!live.isLive(parameter))
            m_webs[*web].discardedAfter.append(StringStateDiscard { parameter, parameter });
    }
    return IterationStatus::Continue;
}

StringStateUse StringBuilderAnalyzer::classifyUse(ValueID userID, uint32_t operandIndex) const
{
    const Value& user = m_function.at(userID);
    if (user.opcode == Opcode::ToBoolean)
        return StringStateUse::Truthiness;
    if (user.opcode == Opcode::GetStringLength)
        return StringStateUse::Length;
    if (user.opcode == Opcode::CallBuiltin && builtinOperandAt(user, operandIndex).isReceiver() && overloadOf(m_program, user).code.implementation.inlineBuiltin == InlineBuiltin::StringLength)
        return StringStateUse::Length;
    if (user.opcode == Opcode::StrictEq || user.opcode == Opcode::Eq) {
        ValueID other = user.leftOperand() == user.operand(operandIndex) ? user.rightOperand() : user.leftOperand();
        if (isEmptyString(other) && !isConditionOfBranchWithProofs(m_function, m_useLists, userID))
            return StringStateUse::Length;
    }
    return StringStateUse::Read;
}

void StringBuilderAnalyzer::addRead(uint32_t web, const StringStateRead& read)
{
    m_webs[web].reads.append(read);
}

void StringBuilderAnalyzer::collectEntries()
{
    if (!m_numberOfUndecidedWebs)
        return;
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(blockID).values) {
            std::optional<uint32_t> web = undecidedWebOf(id);
            if (!web)
                continue;
            ValueID state = m_function.at(id).operand(stateOperandIndex);
            if (webOf(state) != web)
                m_webs[*web].entries.append(StringWebEntry { state, id, std::nullopt, { } });
        }
        forEachOutgoingEdge(m_function, blockID, [&](Edge edge, const Target&) {
            collectEntriesOnEdge(edge);
        });
    }

    for (uint32_t web = 0; web < m_webs.size(); ++web) {
        if (isDecided(web))
            continue;
        sortRecords(m_webs[web]);
        if (wouldReadUnspecifiedFirstState(m_webs[web])) {
            m_webs[web].decidedAt = m_webs[web].members.first();
            continue;
        }
        if (!m_states[web].hasTooFewAppends) {
            m_webs[web].decision = StringBuilderDeclineReason::Created;
            continue;
        }
        m_webs[web].decision = StringBuilderDeclineReason::TooFewAppends;
        m_webs[web].decidedAt = m_webs[web].members.first();
    }
}

// The builder is begun on the edge with an append of the first state: a read in front of whatever says whether that state is there.
bool StringBuilderAnalyzer::wouldReadUnspecifiedFirstState(const StringWeb& web) const
{
    return std::ranges::any_of(web.entries, [&](const StringWebEntry& entry) {
        return entry.edge && m_function.at(entry.value).opcode != Opcode::Unspecified && mayBeUnspecified(m_context.analyses, entry.value);
    });
}

void StringBuilderAnalyzer::collectEntriesOnEdge(Edge edge)
{
    const auto& parameters = m_function.at(targetOf(m_function, edge).block).parameters;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        std::optional<uint32_t> web = undecidedWebOf(parameter);
        if (!web)
            continue;
        ValueID argument = argumentOn(edge, index);
        if (webOf(argument) != web)
            m_webs[*web].entries.append(StringWebEntry { argument, { }, edge, parameter });
    }
}

void StringBuilderAnalyzer::sortRecords(StringWeb& web) const
{
    AOT::sort(web.reads, [&](const StringStateRead& a, const StringStateRead& b) {
        return m_useLists.isBefore(a.use, b.use);
    });
    AOT::sort(web.discardedAfter, { }, [&](const StringStateDiscard& discard) {
        ProgramPoint point = m_useLists.pointAfter(discard.after);
        return std::pair { m_cfg.reversePostOrderIndex(point.block), point.index };
    });
}

// An obstacle is the decision of a whole web only where no cut was left for it.
void StringBuilderAnalyzer::reportLimitHits()
{
    for (const StringWeb& web : m_webs) {
        switch (web.decision) {
        case StringBuilderDeclineReason::TwoStatesLive:
        case StringBuilderDeclineReason::StateIsNotFirstOperand:
        case StringBuilderDeclineReason::WebLimit:
            m_context.gaveUp(LimitID::MaxStringWebsPerFunction, web.decidedAt);
            break;
        case StringBuilderDeclineReason::Unknown:
        case StringBuilderDeclineReason::Created:
        case StringBuilderDeclineReason::TooFewAppends:
            break;
        }
    }
}

void StringBuilderAnalyzer::appendCutValues()
{
    for (auto [value, reason] : m_cuts) {
        StringWeb record;
        record.decision = reason;
        record.decidedAt = value;
        record.members.append(value);
        m_webs.append(WTF::move(record));
    }
}

} // anonymous namespace

std::optional<uint32_t> StringBuilderPlan::webOf(ValueID id) const
{
    return findWeb(m_webOfValue, id);
}

void StringBuilderPlan::dump(PrintStream& out) const
{
    for (uint32_t index = 0; index < m_webs.size(); ++index) {
        const StringWeb& web = m_webs[index];
        out.print("web ", index, " ", nameOf(web.decision));
        if (web.decidedAt)
            out.print(" at v", web.decidedAt.index());
        out.print(" members");
        for (ValueID member : web.members)
            out.print(" v", member.index());
        if (!web.entries.isEmpty())
            out.print(" entries");
        for (const StringWebEntry& entry : web.entries)
            out.print(" v", entry.value.index());
        if (!web.reads.isEmpty())
            out.print(" reads");
        for (const StringStateRead& read : web.reads) {
            out.print(" ");
            dumpUser(out, read.use);
            out.print(":", stateUseNameOf(read.kind), read.endsWeb ? "!" : "");
        }
        if (!web.discardedAfter.isEmpty() || !web.discardedEdges.isEmpty())
            out.print(" discards");
        for (const StringStateDiscard& discard : web.discardedAfter)
            out.print(" v", discard.after.index());
        for (const StringStateEdgeDiscard& discard : web.discardedEdges)
            out.print(" b", discard.edge.from.index(), ".", discard.edge.targetIndex);
        out.print("\n");
    }
}

bool isStringConcatenation(const Program& program, const Function&, const Value& value)
{
    if (!concatenationOpcodes.contains(value.opcode))
        return false;
    return value.numberOfOperands() && program.at(value.type).isString();
}

bool isStringBuilderPart(const Type& type)
{
    return type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull | Primitive::BigInt);
}

StringBuilderPlan analyzeStringBuilders(FunctionPassContext& context)
{
    StringBuilderPlan plan;
    if (!hasStringConcatenation(context.program, context.function))
        return plan;
    StringBuilderAnalyzer(context, plan.m_webs, plan.m_webOfValue).run();
    return plan;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

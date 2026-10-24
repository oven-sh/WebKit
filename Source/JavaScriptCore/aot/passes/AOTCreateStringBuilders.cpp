/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderAnalysis.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTDecidedBranches.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTLiveness.h"
#include "AOTPass.h"
#include "AOTRuleSupport.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTSort.h"
#include "AOTStringRanges.h"
#include <algorithm>
#include <ranges>
#include <wtf/BitVector.h>
#include <wtf/UnionFind.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t stateOperandIndex = 0;
static_assert(OperandIndex::Add::left == stateOperandIndex);
static_assert(OperandIndex::Strcat::parts == stateOperandIndex);

static constexpr OpcodeSet keyedAccesses { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };
static constexpr OpcodeSet equalityComparisonsOfStrings { Opcode::StrictEq, Opcode::Eq };
static constexpr OpcodeSet closingOperations { Opcode::StringBuilderFinish, Opcode::StringBuilderDiscard };
static constexpr OpcodeSet redirectableOperations = closingOperations | OpcodeSet {
    Opcode::StringBuilderAppend, Opcode::StringBuilderAppendJSON, Opcode::StringBuilderCheckpoint, Opcode::StringBuilderRollBack, Opcode::CallDirect,
};
static constexpr AbstractHeapSet stringCreationHeaps = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage };

bool isStringConcatenation(const Program& program, const Function&, const Value& value)
{
    return concatenationOpcodes.contains(value.opcode) && value.numberOfOperands() && program.at(value.type).isString();
}

bool isStringBuilderPart(const Type& type)
{
    return type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull | Primitive::BigInt);
}

namespace {

enum class StringStateUse : uint8_t { Length, Truthiness, Read };

struct StringStateRead {
    Use use;
    ValueID member;
    StringStateUse kind;
    bool endsWeb;
    // Set where a builder cursor is live in front of the user: the string is made in front of this operation of the same block, where none is.
    ValueID operationBeforeCursors { };
};

struct StringWebEntry {
    ValueID value;
    ValueID concatenation;
    std::optional<Edge> edge;
    ValueID parameter;
};

struct StringStateEdgeDiscard {
    ValueID member;
    Edge edge;
};

struct StringStateDiscard {
    ValueID member;
    ValueID after;
};

// The states of one growing text: concatenations that take the state before them as their first operand, and the parameters that states are passed to.
struct StringWeb {
    StringBuilderDeclineReason decision { StringBuilderDeclineReason::Unknown };
    ValueID decidedAt;
    uint32_t numberOfAppends { 0 };
    bool isCarriedAroundCycle { false };
    bool hasTooFewAppends { false };
    BitVector cyclesWithAppends;
    Vector<ValueID, 8> members;
    Vector<StringWebEntry, 2> entries;
    Vector<StringStateRead, 4> reads;
    Vector<StringStateEdgeDiscard, 2> discardedEdges;
    Vector<StringStateDiscard, 2> discardedAfter;
};

struct StringBuilderPlan {
    static constexpr uint32_t noWeb = UINT32_MAX;

    Vector<StringWeb> webs;
    Vector<uint32_t> webOfValue;

    std::optional<uint32_t> webOf(ValueID id) const
    {
        if (id.index() >= webOfValue.size() || webOfValue[id.index()] == noWeb)
            return std::nullopt;
        return webOfValue[id.index()];
    }

    auto createdWebs() const
    {
        return std::views::filter(webs.span(), [](const StringWeb& web) { return web.decision == StringBuilderDeclineReason::Created; });
    }
};

// One step of the pass. The builder gets no FunctionAnalyses: the graph and the use lists have to outlive split edges and queued insertions.
struct StringBuilderStep {
    FunctionPassContext& context;
    const Program& program { context.program };
    Function& function { context.function };
    const CFG& cfg { context.analyses.cfg() };
    const UseLists& useLists { context.analyses.useLists() };
    IRBuilder builder { context.analyses.context(), function, context.types };
    BitVector valuesToErase { };
    BitVector blocksWithErasedValues { };

    StringBuilderPlan findWebs();
    PassOutcome mergeNestedWebs(const StringBuilderPlan&);
    Vector<ValueID, 8> rewriteWebs(const StringBuilderPlan&);
    void foldConversionsInto(std::span<const ValueID> appends);
    PassOutcome joinNestedBuilders();

    const Type& typeOf(ValueID id) const { return program.typeOf(function, id); }
    bool isPart(ValueID id) const { return isStringBuilderPart(typeOf(id)); }
    bool isEmptyString(ValueID id) const { return stringConstantOf(program, function, id) == program.strings.emptyString(); }

    void retype(Value& value)
    {
        value.type = resultTypeOf(program, function, value, context.types);
        value.setExpected({ });
    }

    Value& becomeBuilderOperation(ValueID id, Opcode opcode, ValueID handle)
    {
        Value& value = function.edit(id);
        value.become(opcode);
        value.setBuilderOperand(handle);
        return value;
    }

    void erase(ValueID value)
    {
        valuesToErase.set(value.index());
        blocksWithErasedValues.set(useLists.blockOf(value).index());
    }

    void eraseMarkedValues()
    {
        for (size_t blockIndex : blocksWithErasedValues) {
            const BlockValues& values = function.at(BlockID(blockIndex)).values;
            BitVector erasedIndexes;
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (valuesToErase.get(values[index].index()))
                    erasedIndexes.set(index);
            }
            builder.eraseValues(BlockID(blockIndex), erasedIndexes);
        }
    }
};

StringBuilderPlan StringBuilderStep::findWebs()
{
    using LocalCalc = Liveness::LocalCalc;
    StringBuilderPlan plan;
    Vector<StringWeb>& webs = plan.webs;
    const BlockCycles& cycles = context.analyses.blockCycles();
    size_t numberOfValues = function.values.size();
    uint32_t numberOfUndecidedWebs = 0;

    auto isDecided = [&](uint32_t web) { return webs[web].decision != StringBuilderDeclineReason::Unknown; };
    auto undecidedWebOf = [&](ValueID id) {
        std::optional<uint32_t> web = plan.webOf(id);
        return web && isDecided(*web) ? std::nullopt : web;
    };
    auto hasMember = [&](uint32_t web, std::span<const ValueID> values, ValueID excluded = { }) {
        return std::ranges::any_of(values, [&](ValueID id) { return id != excluded && plan.webOf(id) == web; });
    };
    auto argumentOn = [&](Edge edge, uint32_t parameterIndex) {
        IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
        RELEASE_ASSERT(incoming.kind == IncomingValue::Kind::Argument);
        return incoming.argument;
    };
    auto sharedParametersOf = [&](BlockID block) {
        return function.at(block).parameters.subspan(0, block == cfg.root() ? 0 : numberOfSharedExplicitParameters(function, cfg, block));
    };
    auto classifyUse = [&](ValueID userID, uint32_t operandIndex) {
        const Value& user = function.at(userID);
        if (user.opcode == Opcode::ToBoolean)
            return StringStateUse::Truthiness;
        if (user.opcode == Opcode::GetStringLength)
            return StringStateUse::Length;
        if (user.opcode == Opcode::CallBuiltin && builtinOperandAt(user, operandIndex).isReceiver() && overloadOf(program, user).code.implementation.inlineBuiltin == InlineBuiltin::StringLength)
            return StringStateUse::Length;
        if (equalityComparisonsOfStrings.contains(user.opcode)) {
            ValueID other = user.leftOperand() == user.operand(operandIndex) ? user.rightOperand() : user.leftOperand();
            if (isEmptyString(other) && !isConditionOfBranchWithProofs(function, useLists, userID))
                return StringStateUse::Length;
        }
        return StringStateUse::Read;
    };

    // The use lists do not list a narrowed operand, and neither it nor the subject of a block can take a replacement.
    BitVector hasUnreplaceableUse(numberOfValues);
    BitVector cursors(numberOfValues);
    auto markAsUnreplaceable = [&](ValueID id, uint32_t = 0) {
        if (id)
            hasUnreplaceableUse.quickSet(id.index());
    };
    auto addIfCursor = [&](ValueID id) {
        if (typeOf(id).opaque == OpaqueKind::BuilderCursor)
            cursors.quickSet(id.index());
    };
    for (BlockID blockID : cfg.reversePostOrder()) {
        const Block& block = function.at(blockID);
        markAsUnreplaceable(block.subject);
        for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex) {
            uint32_t numberOfResults = numberOfEdgeResults(function, blockID, targetIndex);
            for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex)
                markAsUnreplaceable(operandNarrowedOnEdge(function, { blockID, targetIndex, resultIndex }));
        }
        std::ranges::for_each(block.parameters, addIfCursor);
        std::ranges::for_each(block.values, addIfCursor);
    }

    // A builder operation between the definition of a cursor and a use of it makes the cursor stale, whichever builder it works on.
    Vector<ValueID> operationBeforeCursors;
    auto findValuesBesideCursors = [&](const Liveness& livenessOfCursors, BlockID blockID) {
        const Block& block = function.at(blockID);
        uint32_t numberOfValuesInBlock = block.values.size();
        LocalCalc live(livenessOfCursors, blockID);
        auto hasLiveCursor = [&] {
            bool hasAny = false;
            live.forEachLive([&](ValueID) { hasAny = true; });
            return hasAny;
        };
        if (hasLiveCursor()) {
            block.terminator.forEachOperand(markAsUnreplaceable);
            forEachOutgoingEdge(function, blockID, [&](Edge, const Target& target) {
                std::ranges::for_each(target.arguments, markAsUnreplaceable);
            });
        }
        BitVector hasCursorInFront(numberOfValuesInBlock);
        for (uint32_t index = numberOfValuesInBlock; index--;) {
            live.execute(index);
            if (hasLiveCursor())
                hasCursorInFront.quickSet(index);
        }
        if (numberOfValuesInBlock ? hasCursorInFront.quickGet(0) : hasLiveCursor())
            std::ranges::for_each(block.parameters, markAsUnreplaceable);

        std::optional<uint32_t> indexBeforeCursors;
        for (uint32_t index = 0; index < numberOfValuesInBlock; ++index) {
            ValueID id = block.values[index];
            if (!hasCursorInFront.quickGet(index)) {
                indexBeforeCursors = index;
                continue;
            }
            markAsUnreplaceable(id);
            function.at(id).forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                ValueLocation location = useLists.locationOf(operand);
                bool isDefinedTooLate = location.placement == ValuePlacement::InBlock && location.block == blockID && indexBeforeCursors && location.index >= *indexBeforeCursors;
                if (!indexBeforeCursors || isDefinedTooLate || classifyUse(id, operandIndex) != StringStateUse::Read)
                    markAsUnreplaceable(operand);
            });
            if (indexBeforeCursors)
                operationBeforeCursors[id.index()] = block.values[*indexBeforeCursors];
        }
    };
    if (!cursors.isEmpty()) {
        Liveness livenessOfCursors(function, cfg, useLists, { .trackedValues = &cursors });
        operationBeforeCursors.fill(ValueID(), numberOfValues);
        for (BlockID blockID : cfg.reversePostOrder())
            findValuesBesideCursors(livenessOfCursors, blockID);
    }

    // Whatever happens to such a parameter converts it to a string, so nobody can tell when it was converted. It can be a state although undefined or a number may flow in.
    BitVector isOnlyConcatenated(numberOfValues);
    Vector<ValueID, 8> parametersOnlyConcatenated;
    for (BlockID blockID : cfg.reversePostOrder()) {
        for (ValueID parameter : sharedParametersOf(blockID)) {
            const Type& type = typeOf(parameter);
            if (type.isString() || !isStringBuilderPart(type) || hasUnreplaceableUse.quickGet(parameter.index()))
                continue;
            parametersOnlyConcatenated.append(parameter);
            isOnlyConcatenated.quickSet(parameter.index());
        }
    }
    auto isConcatenated = [&](const Use& use) {
        if (use.isEdgeUse())
            return isOnlyConcatenated.quickGet(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex].index());
        return use.kind == Use::Kind::Operand && isStringConcatenation(program, function, function.at(use.user));
    };
    for (bool didRemove = true; didRemove;) {
        didRemove = parametersOnlyConcatenated.removeAllMatching([&](ValueID parameter) {
            if (std::ranges::all_of(useLists.usesOf(parameter), isConcatenated))
                return false;
            isOnlyConcatenated.quickClear(parameter.index());
            return true;
        });
    }

    Vector<ValueID> candidates;
    BitVector isCandidate(numberOfValues);
    BitVector isCut(numberOfValues);
    Vector<std::pair<ValueID, StringBuilderDeclineReason>> cuts;
    std::optional<Liveness> liveness;
    Vector<ValueID, 16> liveValues;

    auto setDecision = [&](uint32_t web, StringBuilderDeclineReason decision, ValueID decidedAt) {
        RELEASE_ASSERT(!isDecided(web) && decision != StringBuilderDeclineReason::Unknown);
        --numberOfUndecidedWebs;
        StringWeb& decided = webs[web];
        decided.decision = decision;
        decided.decidedAt = decidedAt;
        decided.entries.clear();
        decided.reads.clear();
        decided.discardedEdges.clear();
        decided.discardedAfter.clear();
    };
    auto cut = [&](ValueID value, StringBuilderDeclineReason reason) {
        if (cuts.size() >= context.limit(LimitID::MaxStringWebsPerFunction)) {
            setDecision(*plan.webOf(value), reason, value);
            return false;
        }
        isCut.quickSet(value.index());
        cuts.append({ value, reason });
        return true;
    };
    auto addCandidate = [&](ValueID id) {
        if (isCut.quickGet(id.index()))
            return;
        isCandidate.quickSet(id.index());
        candidates.append(id);
    };
    // The records of such a web say whether it can continue the other web, however short it is.
    auto isPartOfOtherWeb = [&](uint32_t web) {
        return std::ranges::any_of(webs[web].members, [&](ValueID member) {
            return std::ranges::any_of(useLists.usesOf(member), [&](Use use) {
                while (use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::Upcast) {
                    std::optional<Use> next = useLists.singleUse(use.user);
                    if (!next)
                        return false;
                    use = *next;
                }
                if (use.kind != Use::Kind::Operand || use.operandIndex == stateOperandIndex)
                    return false;
                std::optional<uint32_t> other = plan.webOf(use.user);
                return other && other != web;
            });
        });
    };

    auto visitEdge = [&](ValueID liveMember, uint32_t web, Edge edge, const Target& target) {
        bool isLiveInTarget = hasMember(web, liveness->liveAtHead(target.block));
        auto numberOfPassedMembers = std::ranges::count_if(target.arguments.span(), [&](ValueID argument) { return plan.webOf(argument) == web; });
        if (!numberOfPassedMembers) {
            if (!isLiveInTarget && !edge.isHandlerEdge())
                webs[web].discardedEdges.append(StringStateEdgeDiscard { liveMember, edge });
            return;
        }
        const auto& parameters = function.at(target.block).parameters;
        Use::Kind kind = edge.isHandlerEdge() ? Use::Kind::HandlerArgument : Use::Kind::EdgeArgument;
        bool endsWeb = !edge.isHandlerEdge() && !isLiveInTarget && numberOfPassedMembers == 1;
        for (uint32_t index = 0; index < target.arguments.size(); ++index) {
            ValueID argument = target.arguments[index];
            if (plan.webOf(argument) == web && plan.webOf(parameters[index]) != web)
                webs[web].reads.append(StringStateRead { Use { kind, edge.from, { }, index, edge.targetIndex }, argument, StringStateUse::Read, endsWeb });
        }
    };
    auto visitTerminator = [&](BlockID blockID, const LocalCalc& live) {
        Vector<ValueID, 8> liveMembers;
        live.forEachLive([&](ValueID id) {
            std::optional<uint32_t> web = undecidedWebOf(id);
            if (web && !hasMember(*web, liveMembers.span()))
                liveMembers.append(id);
        });
        for (ValueID liveMember : liveMembers) {
            uint32_t web = *plan.webOf(liveMember);
            bool continues = false;
            forEachOutgoingEdge(function, blockID, [&](Edge, const Target& target) {
                continues |= hasMember(web, target.arguments.span()) || hasMember(web, liveness->liveAtHead(target.block));
            });
            bool isRead = false;
            function.at(blockID).terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (plan.webOf(operand) != web)
                    return;
                isRead = true;
                webs[web].reads.append(StringStateRead { Use { Use::Kind::TerminatorOperand, blockID, { }, operandIndex, 0 }, operand, StringStateUse::Read, !continues });
            });
            if (isRead && !continues)
                continue;
            forEachOutgoingEdge(function, blockID, [&](Edge edge, const Target& target) {
                visitEdge(liveMember, web, edge, target);
            });
        }
    };
    // The append that replaces a concatenation completes or leaves the content as it was, so the builder holds the member wherever the block can throw.
    auto isLiveOnlyForHandler = [&](ValueID member, BlockID blockID, uint32_t index) {
        const Block& block = function.at(blockID);
        if (!block.handler || !block.handler->arguments.contains(member))
            return false;
        if (std::ranges::any_of(useLists.usesOf(member), [&](const Use& use) { return use.block == blockID && useLists.pointOf(use).index > index; }))
            return false;
        if (std::ranges::any_of(block.terminator.targets, [&](const Target& target) { return liveness->isLiveAtHead(member, target.block); }))
            return false;
        bool mayThrowLater = std::ranges::any_of(block.values.subspan(index + 1), [&](ValueID id) { return context.analyses.mayThrow(id); });
        return !mayThrowLater && !context.analyses.effectsOfTerminator(blockID).mayThrow();
    };
    auto cutAtValue = [&](BlockID blockID, uint32_t index, const LocalCalc& live) {
        ValueID id = function.at(blockID).values[index];
        const Value& value = function.at(id);
        std::optional<uint32_t> webOfResult = undecidedWebOf(id);
        bool touchesWeb = webOfResult.has_value();
        value.forEachOperand([&](ValueID operand, uint32_t) {
            touchesWeb |= undecidedWebOf(operand).has_value();
        });
        if (!touchesWeb)
            return false;

        liveValues.shrink(0);
        live.forEachLive([&](ValueID liveValue) { liveValues.append(liveValue); });
        if (webOfResult) {
            bool hasOtherLiveState = hasUnreplaceableUse.quickGet(id.index()) || std::ranges::any_of(liveValues, [&](ValueID other) {
                return other != id && plan.webOf(other) == webOfResult && !isLiveOnlyForHandler(other, blockID, index);
            });
            if (hasOtherLiveState && !context.build.isSabotaged(Sabotage::GrowingTextsAreHeldInOneState)) {
                if (cut(id, StringBuilderDeclineReason::TwoStatesLive))
                    return true;
            } else if (!hasMember(*webOfResult, liveValues.span()))
                webs[*webOfResult].discardedAfter.append(StringStateDiscard { id, id });
        }

        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            std::optional<uint32_t> web = undecidedWebOf(operand);
            if (!web || (operandIndex == stateOperandIndex && plan.webOf(id) == web))
                return;
            StringStateUse kind = classifyUse(id, operandIndex);
            bool endsWeb = !hasMember(*web, liveValues.span());
            ValueID beforeCursors = operationBeforeCursors.isEmpty() ? ValueID() : operationBeforeCursors[id.index()];
            webs[*web].reads.append(StringStateRead { Use { Use::Kind::Operand, blockID, id, operandIndex, 0 }, operand, kind, endsWeb, beforeCursors });
            if (kind != StringStateUse::Read && endsWeb)
                webs[*web].discardedAfter.append(StringStateDiscard { operand, id });
        });
        return false;
    };
    auto cutAtParameters = [&](BlockID blockID, const LocalCalc& live) {
        const Block& block = function.at(blockID);
        for (ValueID parameter : block.parameters) {
            std::optional<uint32_t> web = undecidedWebOf(parameter);
            if (!web)
                continue;
            if (hasUnreplaceableUse.quickGet(parameter.index()) || hasMember(*web, liveness->liveAtHead(blockID)) || hasMember(*web, block.parameters.span(), parameter)) {
                if (cut(parameter, StringBuilderDeclineReason::TwoStatesLive))
                    return true;
            } else if (!live.isLive(parameter))
                webs[*web].discardedAfter.append(StringStateDiscard { parameter, parameter });
        }
        return false;
    };
    auto cutAtInterference = [&] {
        BitVector trackedValues(numberOfValues);
        BitVector blocksWithMembers(cfg.numNodes());
        for (uint32_t web = 0; web < webs.size(); ++web) {
            if (isDecided(web))
                continue;
            for (ValueID member : webs[web].members) {
                trackedValues.quickSet(member.index());
                blocksWithMembers.quickSet(useLists.blockOf(member).index());
            }
        }
        liveness.emplace(function, cfg, useLists, LivenessOptions { .trackedValues = &trackedValues });
        for (BlockID block : cfg.reversePostOrder()) {
            if (!numberOfUndecidedWebs)
                return false;
            if (!blocksWithMembers.quickGet(block.index()) && liveness->liveAtHead(block).empty())
                continue;
            LocalCalc live(*liveness, block);
            visitTerminator(block, live);
            for (uint32_t index = function.at(block).values.size(); index--;) {
                if (cutAtValue(block, index, live))
                    return true;
                live.execute(index);
            }
            if (cutAtParameters(block, live))
                return true;
        }
        return false;
    };

    for (;;) {
        plan = { };
        candidates.shrink(0);
        isCandidate.clearAll();
        for (BlockID blockID : cfg.reversePostOrder()) {
            for (ValueID parameter : sharedParametersOf(blockID)) {
                if (typeOf(parameter).isString() || isOnlyConcatenated.quickGet(parameter.index()))
                    addCandidate(parameter);
            }
            for (ValueID id : function.at(blockID).values) {
                if (isStringConcatenation(program, function, function.at(id)))
                    addCandidate(id);
            }
        }

        struct StateClass : WTF::UnionFind<StateClass> { };
        Vector<StateClass> classes(numberOfValues);
        BitVector classesWithConcatenation(numberOfValues);
        auto rootOf = [&](size_t index) { return static_cast<size_t>(classes[index].find() - classes.span().data()); };
        auto uniteWithCandidate = [&](ValueID id, ValueID state) {
            if (isCandidate.get(state.index()))
                classes[id.index()].unify(&classes[state.index()]);
        };
        for (ValueID id : candidates) {
            ValueLocation location = useLists.locationOf(id);
            if (location.placement != ValuePlacement::Parameter) {
                uniteWithCandidate(id, function.at(id).operand(stateOperandIndex));
                continue;
            }
            for (Edge edge : cfg.incomingEdges(location.block))
                uniteWithCandidate(id, argumentOn(edge, location.index));
        }
        for (ValueID id : candidates) {
            if (useLists.locationOf(id).placement != ValuePlacement::Parameter)
                classesWithConcatenation.quickSet(rootOf(id.index()));
        }
        plan.webOfValue.fill(StringBuilderPlan::noWeb, numberOfValues);
        for (size_t index : isCandidate) {
            size_t root = rootOf(index);
            if (!classesWithConcatenation.quickGet(root))
                continue;
            if (plan.webOfValue[root] == StringBuilderPlan::noWeb) {
                plan.webOfValue[root] = static_cast<uint32_t>(webs.size());
                webs.append(StringWeb { });
            }
            plan.webOfValue[index] = plan.webOfValue[root];
            webs[plan.webOfValue[root]].members.append(ValueID(index));
        }
        numberOfUndecidedWebs = static_cast<uint32_t>(webs.size());

        bool didCutAtStructure = false;
        for (uint32_t web = 0; web < webs.size(); ++web) {
            for (ValueID id : webs[web].members) {
                if (useLists.locationOf(id).placement == ValuePlacement::Parameter || !hasMember(web, function.at(id).operands().subspan(stateOperandIndex + 1)))
                    continue;
                didCutAtStructure |= cut(id, StringBuilderDeclineReason::StateIsNotFirstOperand);
                break;
            }
        }
        if (didCutAtStructure)
            continue;
        if (!numberOfUndecidedWebs)
            break;

        for (ValueID id : candidates) {
            std::optional<uint32_t> web = undecidedWebOf(id);
            ValueLocation location = useLists.locationOf(id);
            if (web && location.placement != ValuePlacement::Parameter && cycles.isInCycle(location.block))
                webs[*web].cyclesWithAppends.set(*cycles.outermostCycleOf(location.block));
        }
        Vector<uint32_t> depths(FillWith { }, numberOfValues, 0u);
        for (ValueID id : candidates) {
            std::optional<uint32_t> web = undecidedWebOf(id);
            if (!web)
                continue;
            ValueLocation location = useLists.locationOf(id);
            uint32_t depth = 0;
            if (location.placement == ValuePlacement::Parameter) {
                for (Edge edge : cfg.incomingEdges(location.block)) {
                    ValueID argument = argumentOn(edge, location.index);
                    if (plan.webOf(argument) != web)
                        continue;
                    depth = std::max(depth, depths[argument.index()]);
                    bool isRetreatingEdge = cfg.reversePostOrderIndex(edge.from) >= cfg.reversePostOrderIndex(location.block);
                    if (isRetreatingEdge && cycles.areInSameCycle(edge.from, location.block) && webs[*web].cyclesWithAppends.get(*cycles.outermostCycleOf(location.block)))
                        webs[*web].isCarriedAroundCycle = true;
                }
            } else {
                ValueID previousState = function.at(id).operand(stateOperandIndex);
                depth = plan.webOf(previousState) == web ? depths[previousState.index()] + 1 : 1;
            }
            depths[id.index()] = depth;
            webs[*web].numberOfAppends = std::max(webs[*web].numberOfAppends, depth);
        }

        uint64_t minAppends = context.limit(LimitID::MinAppendsForStringBuilder);
        uint64_t maxWebs = context.limit(LimitID::MaxStringWebsPerFunction);
        uint64_t numberOfWebs = 0;
        for (uint32_t index = 0; index < webs.size(); ++index) {
            StringWeb& web = webs[index];
            if (isDecided(index))
                continue;
            web.hasTooFewAppends = !web.isCarriedAroundCycle && web.numberOfAppends < minAppends;
            if (web.hasTooFewAppends && !isPartOfOtherWeb(index))
                setDecision(index, StringBuilderDeclineReason::TooFewAppends, web.members.first());
            else if (++numberOfWebs > maxWebs)
                setDecision(index, StringBuilderDeclineReason::WebLimit, web.members.first());
        }
        if (!numberOfUndecidedWebs || !cutAtInterference())
            break;
    }

    for (BlockID blockID : numberOfUndecidedWebs ? cfg.reversePostOrder() : std::span<const BlockID> { }) {
        for (ValueID id : function.at(blockID).values) {
            std::optional<uint32_t> web = undecidedWebOf(id);
            if (web && plan.webOf(function.at(id).operand(stateOperandIndex)) != web)
                webs[*web].entries.append(StringWebEntry { function.at(id).operand(stateOperandIndex), id, std::nullopt, { } });
        }
        forEachOutgoingEdge(function, blockID, [&](Edge edge, const Target& target) {
            const auto& parameters = function.at(target.block).parameters;
            for (uint32_t index = 0; index < parameters.size(); ++index) {
                std::optional<uint32_t> web = undecidedWebOf(parameters[index]);
                if (!web)
                    continue;
                ValueID argument = argumentOn(edge, index);
                if (plan.webOf(argument) != web)
                    webs[*web].entries.append(StringWebEntry { argument, { }, edge, parameters[index] });
            }
        });
    }
    for (StringWeb& web : webs) {
        if (web.decision != StringBuilderDeclineReason::Unknown)
            continue;
        AOT::sort(web.reads, [&](const StringStateRead& a, const StringStateRead& b) {
            return useLists.isBefore(a.use, b.use);
        });
        AOT::sort(web.discardedAfter, { }, [&](const StringStateDiscard& discard) {
            ProgramPoint point = useLists.pointAfter(discard.after);
            return std::pair { cfg.reversePostOrderIndex(point.block), point.index };
        });
        // The builder is begun on the edge with an append of the first state: a read in front of whatever says whether that state is there.
        bool wouldReadUnspecifiedFirstState = std::ranges::any_of(web.entries, [&](const StringWebEntry& entry) {
            return entry.edge && function.at(entry.value).opcode != Opcode::Unspecified && mayBeUnspecified(context.analyses, entry.value);
        });
        if (!wouldReadUnspecifiedFirstState)
            web.decision = web.hasTooFewAppends ? StringBuilderDeclineReason::TooFewAppends : StringBuilderDeclineReason::Created;
        if (web.decision != StringBuilderDeclineReason::Created)
            web.decidedAt = web.members.first();
    }

    // An obstacle is the decision of a whole web only where no cut was left for it.
    for (const StringWeb& web : webs) {
        if (web.decision == StringBuilderDeclineReason::TwoStatesLive || web.decision == StringBuilderDeclineReason::StateIsNotFirstOperand || web.decision == StringBuilderDeclineReason::WebLimit)
            context.gaveUp(LimitID::MaxStringWebsPerFunction, web.decidedAt);
    }
    for (auto [value, reason] : cuts) {
        webs.append(StringWeb { .decision = reason, .decidedAt = value });
        webs.last().members.append(value);
    }
    return plan;
}

// a + b + (c ? d + e : f) + g is computed as ((c ? a + b + d + e : a + b + f) + g), so that one web holds every state and nothing is built apart.
PassOutcome StringBuilderStep::mergeNestedWebs(const StringBuilderPlan& plan)
{
    struct NestedWeb {
        const StringWeb* web { nullptr };
        ValueID lastState;
        Use readOfLastState;
        Vector<ValueID, 2> casts;
        Vector<ValueID, 8> front;
    };
    struct MergedConcatenation {
        ValueID concatenation;
        Vector<NestedWeb, 1> nestedWebs;
        Vector<ValueID, 8> remainingOperands;
    };
    const Dominators& dominators = context.analyses.dominators();
    Vector<MergedConcatenation, 4> merges;
    Vector<Edge, 4> edgesToSplit;
    Vector<ValueID, 4> declinedConcatenations;
    BitVector outerWebs;
    BitVector nestedWebs;

    auto operandUse = [&](ValueID user, uint32_t operandIndex) { return Use { Use::Kind::Operand, useLists.blockOf(user), user, operandIndex, 0 }; };
    auto siteOf = [&](const StringWebEntry& entry) { return entry.concatenation ? useLists.blockOf(entry.concatenation) : entry.edge->from; };
    auto isUnobservable = [](const Effects& effects) { return effects.writes.containsOnly(bookkeepingHeaps) && !effects.mayThrow && !effects.mayRunOtherCode(); };

    auto findNestedWeb = [&](ValueID concatenation, uint32_t operandIndex) -> std::optional<NestedWeb> {
        NestedWeb nested;
        nested.lastState = function.at(concatenation).operand(operandIndex);
        nested.readOfLastState = operandUse(concatenation, operandIndex);
        while (function.at(nested.lastState).opcode == Opcode::Upcast && useLists.useCount(nested.lastState) == 1) {
            nested.casts.append(nested.lastState);
            nested.readOfLastState = operandUse(nested.lastState, OperandIndex::Upcast::value);
            nested.lastState = function.at(nested.lastState).valueOperand();
        }
        std::optional<uint32_t> web = plan.webOf(nested.lastState);
        if (!web || web == plan.webOf(concatenation))
            return std::nullopt;

        nested.web = &plan.webs[*web];
        if (nested.web->reads.size() != 1 || nested.web->entries.isEmpty())
            return std::nullopt;
        if (nested.web->reads.first().use != nested.readOfLastState || !nested.web->reads.first().endsWeb)
            return std::nullopt;
        return nested;
    };
    // A concatenation converts its operands before it joins them. The code that the conversion of an object runs would come after the front is written.
    auto hasOnlyPartsFrom = [&](ValueID concatenation, uint32_t firstOperandIndex) {
        return std::ranges::all_of(function.at(concatenation).operands().subspan(firstOperandIndex), [&](ValueID part) { return isPart(part); });
    };
    // A value that is defined before every state of a web is not defined a second time while the web lives, so each entry sees what the concatenation would have seen.
    auto isDefinedBefore = [&](ValueID value, const StringWeb& web) {
        BlockID block = useLists.blockOf(value);
        for (ValueID member : web.members) {
            ValueLocation location = useLists.locationOf(member);
            if (location.placement == ValuePlacement::Parameter) {
                if (location.block == block || !dominators.dominates(block, location.block))
                    return false;
            } else if (!context.analyses.dominates(value, ProgramPoint { location.block, location.index }))
                return false;
        }
        return std::ranges::all_of(web.entries, [&](const StringWebEntry& entry) {
            return !entry.edge || context.analyses.dominates(value, pointBeforeTerminatorOf(function, entry.edge->from));
        });
    };
    auto canMoveToEntries = [&](const StringWeb& web, std::span<const ValueID> front) {
        for (const StringWebEntry& entry : web.entries) {
            if ((entry.edge && entry.edge->isHandlerEdge()) || !isPart(entry.value))
                return false;
            if (entry.concatenation && !hasOnlyPartsFrom(entry.concatenation, stateOperandIndex + 1))
                return false;
        }
        for (ValueID part : front) {
            if (!isPart(part) || function.at(part).opcode == Opcode::Unspecified)
                return false;
            if (!isRematerializable(function.at(part)) && !isDefinedBefore(part, web))
                return false;
        }
        return true;
    };
    auto buildsOnlyStrings = [&](BlockID block, uint32_t firstIndex, uint32_t endIndex) {
        for (ValueID id : function.at(block).values.subspan(firstIndex, endIndex - firstIndex)) {
            if (function.at(id).opcode == Opcode::LoopHint || isUnobservable(context.analyses.effectsOf(id).onAnyEdge()))
                continue;
            // To convert a primitive and to join primitives can only run out of memory, which is the error of a text that grows too long.
            const Value& value = function.at(id);
            bool createsStringOfPrimitives = value.opcode == Opcode::ToString || isStringConcatenation(program, function, value);
            value.forEachOperand([&](ValueID part, uint32_t) {
                createsStringOfPrimitives &= isPart(part);
            });
            if (!createsStringOfPrimitives)
                return false;
        }
        return true;
    };
    // The front is written at the entries, so a text that grows too long throws there and not at the concatenation.
    auto buildsOnlyStringsUntil = [&](ValueID concatenation, const StringWeb& web) {
        if (!web.discardedEdges.isEmpty() || !web.discardedAfter.isEmpty())
            return false;

        ValueLocation end = useLists.locationOf(concatenation);
        const OutOfLineOptional<Target>& handler = function.at(end.block).handler;
        BitVector barrier(function.blocks.size());
        barrier.set(end.block.index());
        if (handler)
            barrier.set(handler->block.index());
        auto buildsOnlyStringsToBlockEnd = [&](BlockID block, uint32_t firstIndex) {
            return function.at(block).handler == handler
                && buildsOnlyStrings(block, firstIndex, function.at(block).values.size())
                && isUnobservable(context.analyses.effectsOfTerminator(block).onAnyEdge());
        };

        BitVector blocksBetween(function.blocks.size());
        bool entersBlockOfConcatenation = false;
        for (const StringWebEntry& entry : web.entries) {
            BlockID site = siteOf(entry);
            uint32_t firstIndex = entry.concatenation ? useLists.locationOf(entry.concatenation).index + 1 : function.at(site).values.size();
            if (site == end.block) {
                if (firstIndex > end.index || !buildsOnlyStrings(site, firstIndex, end.index))
                    return false;
                continue;
            }
            if (!buildsOnlyStringsToBlockEnd(site, firstIndex))
                return false;
            BitVector reachedBlocks = cfg.blocksReachableFrom(site, barrier);
            if (!cfg.isInCycleAvoiding(site, end.block))
                reachedBlocks.clear(site.index());
            blocksBetween.merge(reachedBlocks);
            entersBlockOfConcatenation = true;
        }
        for (size_t index : blocksBetween) {
            if (!buildsOnlyStringsToBlockEnd(BlockID(index), 0))
                return false;
        }
        return !entersBlockOfConcatenation || buildsOnlyStrings(end.block, 0, end.index);
    };
    // Each entry takes the state of the outer web. Whatever reads that state later would need a second copy of the text.
    auto isUsedAfterEntries = [&](ValueID state, const Use& readOfState, const StringWeb& web) {
        BitVector blocksWithUses(function.blocks.size());
        Vector<BlockID, 8> sites;
        for (const StringWebEntry& entry : web.entries) {
            if (blocksWithUses.set(siteOf(entry).index()))
                return true;
            sites.append(siteOf(entry));
        }
        for (const Use& use : useLists.usesOf(state)) {
            if (use == readOfState)
                continue;
            if (sites.contains(use.block))
                return true;
            blocksWithUses.set(use.block.index());
        }

        BlockID definition = useLists.blockOf(state);
        BitVector barrier(function.blocks.size());
        barrier.set(definition.index());
        for (BlockID site : sites) {
            if (cfg.isInCycleAvoiding(site, definition))
                return true;
            BitVector reachedUses = cfg.blocksReachableFrom(site, barrier);
            reachedUses.clear(site.index());
            reachedUses.filter(blocksWithUses);
            if (!reachedUses.isEmpty())
                return true;
        }
        return false;
    };
    // Code for an entry stands at the end of the block the edge leaves. The next plan names the blocks on these edges.
    auto needsBlocksOnEntryEdges = [&](const StringWeb& web) {
        bool needsBlocks = false;
        for (const StringWebEntry& entry : web.entries) {
            if (!entry.edge || function.at(entry.edge->from).terminator.kind == TerminatorKind::Jump)
                continue;
            needsBlocks = true;
            edgesToSplit.appendIfNotContains(*entry.edge);
        }
        return needsBlocks;
    };
    // A web changes in one way in a round: its entries move, or its concatenations lose their fronts. The next round sees the other.
    auto planConcatenation = [&](uint32_t web, ValueID id) {
        if (nestedWebs.get(web))
            return;
        const Value& concatenation = function.at(id);
        MergedConcatenation merge { id, { }, { concatenation.operand(stateOperandIndex) } };
        Use readOfState = operandUse(id, stateOperandIndex);
        for (uint32_t operandIndex = stateOperandIndex + 1; operandIndex < concatenation.numberOfOperands(); ++operandIndex) {
            std::optional<NestedWeb> nested = findNestedWeb(id, operandIndex);
            if (!nested || outerWebs.get(*plan.webOf(nested->lastState))) {
                merge.remainingOperands.append(concatenation.operand(operandIndex));
                continue;
            }
            ValueID state = merge.remainingOperands[stateOperandIndex];
            bool canMerge = hasOnlyPartsFrom(id, operandIndex + 1) && canMoveToEntries(*nested->web, merge.remainingOperands.span()) && buildsOnlyStringsUntil(id, *nested->web);
            if (canMerge && needsBlocksOnEntryEdges(*nested->web))
                return;
            if (!canMerge || (plan.webOf(state) && isUsedAfterEntries(state, readOfState, *nested->web))) {
                if (nested->web->decision != StringBuilderDeclineReason::Created)
                    declinedConcatenations.append(id);
                merge.remainingOperands.append(concatenation.operand(operandIndex));
                continue;
            }
            nestedWebs.set(*plan.webOf(nested->lastState));
            readOfState = nested->readOfLastState;
            nested->front = std::exchange(merge.remainingOperands, { nested->lastState });
            merge.nestedWebs.append(WTF::move(*nested));
        }
        if (merge.nestedWebs.isEmpty())
            return;
        outerWebs.set(web);
        merges.append(WTF::move(merge));
    };

    for (uint32_t web = 0; web < plan.webs.size(); ++web) {
        if (plan.webs[web].decision != StringBuilderDeclineReason::Created)
            continue;
        for (ValueID member : plan.webs[web].members) {
            if (function.at(member).opcode != Opcode::Parameter)
                planConcatenation(web, member);
        }
    }
    if (!edgesToSplit.isEmpty()) {
        for (Edge edge : edgesToSplit)
            builder.splitEdge(edge);
        return PassOutcome::Changed;
    }
    if (merges.isEmpty()) {
        for (ValueID concatenation : declinedConcatenations)
            context.addDecline(Wire::DeclineKind::StringBuildersNotJoined, concatenation);
        return PassOutcome::Unchanged;
    }

    auto prependToEntry = [&](std::span<const ValueID> front, const StringWebEntry& entry) {
        ProgramPoint point = entry.concatenation ? useLists.pointOf(entry.concatenation) : pointBeforeTerminatorOf(function, entry.edge->from);
        builder.setOrigin(entry.concatenation ? function.at(entry.concatenation).origin() : function.at(entry.edge->from).terminator.origin());

        Vector<ValueID, 8> parts;
        for (ValueID part : front)
            parts.append(isRematerializable(function.at(part)) ? builder.insertBefore(point, Value(function.at(part))) : part);
        if (!isEmptyString(entry.value))
            parts.append(entry.value);

        if (entry.concatenation && !context.exceeds(LimitID::MaxFusedConcatenationParts, parts.size() + function.at(entry.concatenation).numberOfOperands() - 1, entry.concatenation)) {
            Value& concatenation = function.edit(entry.concatenation);
            parts.append(concatenation.operands().subspan(stateOperandIndex + 1));
            concatenation.become(Opcode::Strcat);
            concatenation.setVariadicOperands(parts.span());
            return;
        }

        ValueID state = parts.first();
        if (parts.size() > 1 || !typeOf(state).isAnyString()) {
            Value concatenation = IRBuilder::make(Opcode::Strcat);
            concatenation.setVariadicOperands(parts.span());
            state = builder.insertBefore(point, WTF::move(concatenation));
        }
        if (entry.concatenation)
            function.edit(entry.concatenation, ValueEdit::Definition).setOperand(stateOperandIndex, state);
        else
            editTarget(function, *entry.edge).arguments[useLists.locationOf(entry.parameter).index] = state;
    };
    ValueSubstitution substitution(function);
    size_t numberOfNestedWebs = 0;
    for (const MergedConcatenation& merge : merges) {
        for (const NestedWeb& nested : merge.nestedWebs) {
            for (const StringWebEntry& entry : nested.web->entries) {
                // No use of an Unspecified value is reached, and a concatenation at the entry would be one that is.
                if (function.at(entry.value).opcode != Opcode::Unspecified)
                    prependToEntry(nested.front.span(), entry);
            }
            for (ValueID member : nested.web->members) {
                Value& state = function.edit(member);
                state.type = TypeTable::string;
                state.setExpected({ });
            }
            for (ValueID cast : nested.casts)
                erase(cast);
        }
        numberOfNestedWebs += merge.nestedWebs.size();

        if (merge.remainingOperands.size() > 1) {
            function.edit(merge.concatenation, ValueEdit::Definition).setVariadicOperands(merge.remainingOperands.span());
            continue;
        }
        substitution.replace(merge.concatenation, merge.remainingOperands.first());
        erase(merge.concatenation);
    }
    builder.applyInsertions();
    eraseMarkedValues();
    substitution.apply(function, nullptr);
    context.build.statistics().count(StatisticID::StringBuildersJoined, numberOfNestedWebs);
    return PassOutcome::Changed;
}

} // anonymous namespace

static std::optional<BlockID> frontOf(const Function& function, const CFG& cfg, BlockID block)
{
    std::optional<Edge> edge = cfg.singleIncomingEdge(block);
    if (!edge || !function.at(block).parameters.isEmpty())
        return std::nullopt;
    const Block& front = function.at(edge->from);
    return front.terminator.kind == TerminatorKind::Jump && !front.handler ? std::optional<BlockID>(edge->from) : std::nullopt;
}

static bool splitInFrontOfCoveredBlocks(FunctionPassContext& context, const StringBuilderPlan& plan)
{
    Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    BitVector blocksWithoutFront;
    auto requireFront = [&](BlockID block) {
        if (!frontOf(function, cfg, block))
            blocksWithoutFront.set(block.index());
    };
    for (const StringWeb& web : plan.createdWebs()) {
        for (const StringWebEntry& entry : web.entries) {
            if (entry.edge && entry.edge->isHandlerEdge())
                requireFront(entry.edge->from);
        }
        for (const StringStateRead& read : web.reads) {
            if (read.use.kind == Use::Kind::HandlerArgument)
                requireFront(read.use.block);
        }
    }
    if (blocksWithoutFront.isEmpty())
        return false;

    IRBuilder builder(context.analyses.context(), function, context.types);
    for (size_t blockIndex : blocksWithoutFront) {
        BlockID block(blockIndex);
        const Block& contents = function.at(block);
        uint32_t index = indexAfterCaughtValues(function, contents);
        builder.setOrigin(index < contents.values.size() ? function.at(contents.values[index]).origin() : contents.terminator.origin());
        builder.splitBlock(ProgramPoint { block, index });
        builder.removeHandler(block);
    }
    return true;
}

static void reportDecisions(FunctionPassContext& context, const StringBuilderPlan& plan)
{
    for (const StringWeb& web : plan.webs) {
        switch (web.decision) {
        case StringBuilderDeclineReason::Created:
            context.build.statistics().count(StatisticID::StringBuildersCreated);
            break;
        case StringBuilderDeclineReason::TooFewAppends:
            context.addDecline(Wire::DeclineKind::StringBuilderNotCreated, web.decidedAt, web.decision, LimitID::MinAppendsForStringBuilder, web.numberOfAppends);
            break;
        case StringBuilderDeclineReason::WebLimit:
            context.addDecline(Wire::DeclineKind::StringBuilderNotCreated, web.decidedAt, web.decision, LimitID::MaxStringWebsPerFunction);
            break;
        case StringBuilderDeclineReason::Unknown:
        case StringBuilderDeclineReason::TwoStatesLive:
        case StringBuilderDeclineReason::StateIsNotFirstOperand:
            context.addDecline(Wire::DeclineKind::StringBuilderNotCreated, web.decidedAt, web.decision);
            break;
        }
    }
}

// Every state of a created web becomes the handle of its builder and every concatenation an append. Returns the appends.
Vector<ValueID, 8> StringBuilderStep::rewriteWebs(const StringBuilderPlan& plan)
{
    struct InsertionSite {
        ProgramPoint point;
        Origin origin;
    };
    struct Materialization {
        ValueID member;
        ProgramPoint point;
        ValueID string;
    };
    TypeID handleType = context.types.intern(Type::forOpaque(OpaqueKind::StringBuilder));
    Vector<ValueID> handleOf(function.values.size());
    BitVector blocksOnEdges;
    Materialization lastMaterialization;
    Vector<ValueID, 8> appends;

    auto endsInUncoveredJump = [&](BlockID block) { return function.at(block).terminator.kind == TerminatorKind::Jump && !function.at(block).handler; };
    auto blockOnEdge = [&](Edge edge) {
        BlockID target = targetOf(function, edge).block;
        return blocksOnEdges.get(target.index()) ? std::optional<BlockID>(target) : std::nullopt;
    };
    // Code on an edge never runs under a handler that could still get the handle.
    auto splitEdge = [&](Edge edge) {
        if (!endsInUncoveredJump(edge.from) && !blockOnEdge(edge))
            blocksOnEdges.set(builder.splitEdge(edge).index());
    };
    auto targetOnEdge = [&](Edge edge) -> Target& {
        if (std::optional<BlockID> block = blockOnEdge(edge))
            return function.edit(*block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.next();
        return editTarget(function, edge);
    };
    auto findEntryArgument = [&](const StringWebEntry& entry, Edge edge) -> std::optional<uint32_t> {
        std::optional<uint32_t> web = plan.webOf(entry.parameter);
        const Target& target = targetOnEdge(edge);
        const auto& parameters = function.at(target.block).parameters;
        for (uint32_t index = 0; index < target.arguments.size(); ++index) {
            if (target.arguments[index] == entry.value && plan.webOf(parameters[index]) == web)
                return index;
        }
        return std::nullopt;
    };
    auto hasMatchingHandlerEntry = [&](const StringWebEntry& entry) {
        BlockID block = entry.edge->from;
        return function.at(block).handler && findEntryArgument(entry, Edge::toHandlerOf(block));
    };

    auto siteBeforeTerminatorOf = [&](BlockID block) { return InsertionSite { pointBeforeTerminatorOf(function, block), function.at(block).terminator.origin() }; };
    auto siteOfValue = [&](ValueID value) { return InsertionSite { useLists.pointOf(value), function.at(value).origin() }; };
    auto siteOnEdge = [&](Edge edge) {
        std::optional<BlockID> block = blockOnEdge(edge);
        RELEASE_ASSERT(block || endsInUncoveredJump(edge.from));
        return InsertionSite { pointBeforeTerminatorOf(function, block.value_or(edge.from)), function.at(edge.from).terminator.origin() };
    };
    auto siteInFrontOf = [&](BlockID block) {
        std::optional<BlockID> front = frontOf(function, cfg, block);
        RELEASE_ASSERT(front);
        return siteBeforeTerminatorOf(*front);
    };
    auto siteOfUse = [&](const Use& use) {
        switch (use.kind) {
        case Use::Kind::Operand:
            return siteOfValue(use.user);
        case Use::Kind::TerminatorOperand:
            return siteBeforeTerminatorOf(use.block);
        case Use::Kind::EdgeArgument:
            return siteOnEdge(use.edge());
        case Use::Kind::HandlerArgument:
            return siteInFrontOf(use.block);
        case Use::Kind::BlockSubject:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto insert = [&](const InsertionSite& site, Value&& value) {
        builder.setOrigin(site.origin);
        return builder.insertBefore(site.point, WTF::move(value));
    };
    auto beginBuilder = [&](const InsertionSite& site, ValueID firstState) {
        ValueID handle = insert(site, IRBuilder::make(Opcode::StringBuilderBegin));
        // No use of an Unspecified value is reached, and an append on the edge would be one that is.
        if (!isEmptyString(firstState) && function.at(firstState).opcode != Opcode::Unspecified)
            insert(site, IRBuilder::make(Opcode::StringBuilderAppend, { handle, firstState }));
        return handle;
    };
    auto replace = [&](const Use& use, ValueID replacement) {
        if (use.kind == Use::Kind::EdgeArgument)
            targetOnEdge(use.edge()).arguments[use.operandIndex] = replacement;
        else
            replaceUse(function, use, replacement);
    };

    for (const StringWeb& web : plan.createdWebs()) {
        for (const StringWebEntry& entry : web.entries) {
            if (!entry.edge)
                continue;
            if (!entry.edge->isHandlerEdge()) {
                if (!hasMatchingHandlerEntry(entry))
                    splitEdge(*entry.edge);
                continue;
            }
            uint32_t numberOfTargets = function.at(entry.edge->from).terminator.targets.size();
            for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
                if (!findEntryArgument(entry, Edge { entry.edge->from, targetIndex }))
                    splitEdge(Edge { entry.edge->from, targetIndex });
            }
        }
        for (const StringStateRead& read : web.reads) {
            if (read.use.kind == Use::Kind::EdgeArgument)
                splitEdge(read.use.edge());
        }
        for (const StringStateEdgeDiscard& discard : web.discardedEdges)
            splitEdge(discard.edge);
    }

    for (const StringWeb& web : plan.createdWebs()) {
        for (ValueID member : web.members) {
            Value& value = function.edit(member);
            if (value.opcode != Opcode::Parameter)
                continue;
            value.type = handleType;
            value.setExpected({ });
            handleOf[member.index()] = member;
        }
        for (const StringWebEntry& entry : web.entries) {
            if (entry.concatenation)
                handleOf[entry.concatenation.index()] = insert(siteOfValue(entry.concatenation), IRBuilder::make(Opcode::StringBuilderBegin));
        }
        for (ValueID member : web.members) {
            ValueID state = member;
            while (!handleOf[state.index()])
                state = function.at(state).operand(stateOperandIndex);
            ValueID handle = handleOf[state.index()];
            for (ValueID next = member; next != state; next = function.at(next).operand(stateOperandIndex))
                handleOf[next.index()] = handle;
        }
    }

    auto rewriteRead = [&](const StringStateRead& read) {
        const Use& use = read.use;
        ValueID handle = handleOf[read.member.index()];
        InsertionSite site = siteOfUse(use);
        switch (read.kind) {
        case StringStateUse::Length:
            if (equalityComparisonsOfStrings.contains(function.at(use.user).opcode)) {
                ValueID length = insert(site, IRBuilder::make(Opcode::StringBuilderLength, { handle }));
                ValueID lengthOfEmptyString = insert(site, makeInteger(0));
                Value& comparison = function.edit(use.user);
                comparison.setLeftOperand(length);
                comparison.setRightOperand(lengthOfEmptyString);
            } else
                retype(becomeBuilderOperation(use.user, Opcode::StringBuilderLength, handle));
            return;
        case StringStateUse::Truthiness:
            replace(use, insert(site, IRBuilder::make(Opcode::StringBuilderLength, { handle })));
            return;
        case StringStateUse::Read:
            if (read.operationBeforeCursors)
                site.point = useLists.pointOf(read.operationBeforeCursors);
            if (lastMaterialization.member != read.member || lastMaterialization.point != site.point)
                lastMaterialization = { read.member, site.point, insert(site, IRBuilder::make(read.endsWeb ? Opcode::StringBuilderFinish : Opcode::StringBuilderToString, { handle })) };
            replace(use, lastMaterialization.string);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    for (const StringWeb& web : plan.createdWebs())
        std::ranges::for_each(web.reads, rewriteRead);

    auto insertConversion = [&](const InsertionSite& site, Opcode concatenation, ValueID part) {
        if (concatenation == Opcode::Strcat)
            return insert(site, IRBuilder::make(Opcode::ToString, { part }));
        RELEASE_ASSERT(concatenation == Opcode::Add);
        Value emptyString = IRBuilder::make(Opcode::String);
        emptyString.setString(program.strings.emptyString());
        ValueID left = insert(site, WTF::move(emptyString));
        return insert(site, IRBuilder::make(Opcode::Add, { left, part }));
    };
    auto continuesWeb = [&](ValueID member, const Use& use) {
        std::optional<uint32_t> web = plan.webOf(member);
        if (use.isEdgeUse())
            return plan.webOf(function.at(targetOnEdge(use.edge()).block).parameters[use.operandIndex]) == web;
        return use.kind == Use::Kind::Operand && use.operandIndex == stateOperandIndex && plan.webOf(use.user) == web;
    };
    for (const StringWeb& web : plan.createdWebs()) {
        for (ValueID id : web.members) {
            Opcode opcode = function.at(id).opcode;
            if (opcode == Opcode::Parameter)
                continue;
            bool continuesBuilder = plan.webOf(function.at(id).operand(stateOperandIndex)) == plan.webOf(id);
            Vector<ValueID, 8> parts(function.at(id).operands().subspan(continuesBuilder ? stateOperandIndex + 1 : 0));
            for (ValueID& part : parts) {
                if (!isPart(part))
                    part = insertConversion(siteOfValue(id), opcode, part);
            }
            Value& append = becomeBuilderOperation(id, Opcode::StringBuilderAppend, handleOf[id.index()]);
            append.setPartsOperands(parts.span());
            retype(append);
            appends.append(id);
        }
        for (const StringWebEntry& entry : web.entries) {
            if (entry.edge && !entry.edge->isHandlerEdge() && !hasMatchingHandlerEntry(entry)) {
                ValueID handle = beginBuilder(siteOnEdge(*entry.edge), entry.value);
                targetOnEdge(*entry.edge).arguments[useLists.locationOf(entry.parameter).index] = handle;
            }
        }
        for (const StringWebEntry& entry : web.entries) {
            if (!entry.edge || !entry.edge->isHandlerEdge())
                continue;
            BlockID block = entry.edge->from;
            ValueID handle = beginBuilder(siteInFrontOf(block), entry.value);
            uint32_t numberOfTargets = function.at(block).terminator.targets.size();
            for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
                Edge edge { block, targetIndex };
                if (std::optional<uint32_t> argumentIndex = findEntryArgument(entry, edge))
                    targetOnEdge(edge).arguments[*argumentIndex] = handle;
                else
                    insert(siteOnEdge(edge), IRBuilder::make(Opcode::StringBuilderDiscard, { handle }));
            }
            targetOnEdge(*entry.edge).arguments[useLists.locationOf(entry.parameter).index] = handle;
        }

        size_t numberOfReads = 0;
        for (ValueID member : web.members) {
            for (const Use& use : useLists.usesOf(member)) {
                if (!continuesWeb(member, use))
                    ++numberOfReads;
                else if (use.isEdgeUse())
                    replace(use, handleOf[member.index()]);
            }
        }
        RELEASE_ASSERT(numberOfReads == web.reads.size());
    }

    for (const StringWeb& web : plan.createdWebs()) {
        for (const StringStateDiscard& discard : web.discardedAfter)
            insert({ useLists.pointAfter(discard.after), function.at(discard.after).origin() }, IRBuilder::make(Opcode::StringBuilderDiscard, { handleOf[discard.member.index()] }));
        for (const StringStateEdgeDiscard& discard : web.discardedEdges)
            insert(siteOnEdge(discard.edge), IRBuilder::make(Opcode::StringBuilderDiscard, { handleOf[discard.member.index()] }));
    }
    builder.applyInsertions();
    context.analyses.didChangeControlFlow();
    return appends;
}

// Every fold is found before the first value is added: FunctionAnalyses answers for the values it has seen.
void StringBuilderStep::foldConversionsInto(std::span<const ValueID> appends)
{
    const BuiltinTable& builtins = program.builtins();
    auto convertedOperandOf = [&](const Value& call, const BuiltinForms& forms) {
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (forms.appendForm == InlineBuiltin::CallStringConstructorAppend)
            return arguments.size() == 1 && isPart(arguments[0]) ? arguments[0] : ValueID();
        if (forms.appendForm != InlineBuiltin::NumberToStringAppend)
            return ValueID();
        ValueID receiver = call.receiverOperand();
        bool isDecimal = arguments.empty() || (arguments.size() == 1 && integerConstantThroughCasts(program, function, arguments[0]) == 10);
        return receiver && typeOf(receiver).isNumber() && isDecimal ? receiver : ValueID();
    };
    // The append makes the same conversion, so it runs out of memory where the call would have.
    auto onlyCreatesString = [](const Effects& effects) { return !effects.mayRunOtherCode() && !effects.mayBeTerminated && effects.writes.containsOnly(stringCreationHeaps); };

    for (ValueID append : appends) {
        if (useLists.locationOf(append).placement != ValuePlacement::InBlock)
            continue;
        uint32_t numberOfOperands = function.at(append).numberOfOperands();
        for (uint32_t operandIndex = OperandIndex::StringBuilderAppend::parts; operandIndex < numberOfOperands; ++operandIndex) {
            ValueID part = function.at(append).operand(operandIndex);
            const Value& call = function.at(part);
            if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || !typeOf(part).isString())
                continue;
            std::optional<Use> use = useLists.singleUse(part);
            if (!use || use->user != append || useLists.blockOf(part) != use->block)
                continue;
            const BuiltinForms* forms = builtins.formsOf(builtins.at(call.overload()).builtin);
            ValueID converted = forms ? convertedOperandOf(call, *forms) : ValueID();
            if (!converted || !onlyCreatesString(context.analyses.effectsOf(part).onAnyEdge()))
                continue;
            function.edit(append, ValueEdit::Definition).setOperand(operandIndex, converted);
            erase(part);
        }
    }
    if (blocksWithErasedValues.isEmpty())
        return;
    eraseMarkedValues();
    context.analyses.didChangeValues();
}

// A text that is built only to become a part of another is written into the builder of the other.
PassOutcome StringBuilderStep::joinNestedBuilders()
{
    struct NestedBuilder {
        ValueID begin;
        ValueID concatenation;
        uint32_t operandIndex { 0 };
    };
    FunctionAnalyses& analyses = context.analyses;
    Vector<NestedBuilder, 4> nestedBuilders;
    BitVector nestedBegins;
    BitVector joinedConcatenations;

    auto outerBuilderOf = [&](const NestedBuilder& nested) {
        const Value& concatenation = function.at(nested.concatenation);
        return concatenation.opcode == Opcode::StringBuilderAppend ? concatenation.builderOperand() : ValueID();
    };
    auto findConcatenationUse = [&](ValueID finish) -> std::optional<Use> {
        bool didFollowEdge = false;
        ValueID string = finish;
        while (std::optional<Use> use = useLists.singleUse(string)) {
            if (use->kind == Use::Kind::EdgeArgument) {
                if (didFollowEdge || use->block != useLists.blockOf(finish) || function.at(use->block).terminator.kind != TerminatorKind::Jump)
                    return std::nullopt;
                didFollowEdge = true;
                string = function.at(targetOf(function, use->edge()).block).parameters[use->operandIndex];
                continue;
            }
            if (use->kind != Use::Kind::Operand)
                return std::nullopt;
            const Value& user = function.at(use->user);
            if (user.opcode == Opcode::Upcast) {
                string = use->user;
                continue;
            }
            if (user.opcode == Opcode::StringBuilderAppend ? use->operandIndex >= OperandIndex::StringBuilderAppend::parts : isStringConcatenation(program, function, user))
                return use;
            return std::nullopt;
        }
        return std::nullopt;
    };
    auto hasOnlyRedirectableUses = [&](ValueID begin) {
        unsigned numberOfFinishes = 0;
        for (const Use& use : useLists.usesOf(begin)) {
            if (use.kind != Use::Kind::Operand)
                return false;
            Opcode opcode = function.at(use.user).opcode;
            if (familyOf(opcode) != OperationFamily::JSONWrite && !redirectableOperations.contains(opcode))
                return false;
            numberOfFinishes += opcode == Opcode::StringBuilderFinish;
        }
        return numberOfFinishes == 1;
    };
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (function.at(id).opcode != Opcode::StringBuilderFinish)
                continue;
            ValueID begin = function.at(id).builderOperand();
            std::optional<Use> use = findConcatenationUse(id);
            if (!use)
                continue;
            if (function.at(begin).opcode != Opcode::StringBuilderBegin || !hasOnlyRedirectableUses(begin)) {
                context.addDecline(Wire::DeclineKind::StringBuildersNotJoined, use->user);
                continue;
            }
            nestedBuilders.append(NestedBuilder { begin, use->user, use->operandIndex });
            nestedBegins.set(begin.index());
        }
    }
    if (nestedBuilders.isEmpty())
        return PassOutcome::Unchanged;

    BitVector trackedValues(function.values.size());
    for (const NestedBuilder& nested : nestedBuilders) {
        trackedValues.quickSet(nested.begin.index());
        if (ValueID outer = outerBuilderOf(nested))
            trackedValues.quickSet(outer.index());
    }
    Liveness liveness(function, cfg, useLists, { .trackedValues = &trackedValues });

    auto isUsedIn = [&](ValueID used, BlockID id, uint32_t firstIndex, std::optional<uint32_t> endIndex) {
        const Block& block = function.at(id);
        bool isUsed = false;
        auto visitOperand = [&](ValueID operand, uint32_t) { isUsed |= operand == used; };
        for (ValueID value : block.values.subspan(firstIndex, endIndex ? *endIndex - firstIndex : std::dynamic_extent))
            function.at(value).forEachOperand(visitOperand);
        if (!endIndex)
            block.terminator.forEachOperand(visitOperand);
        return isUsed;
    };
    // Between the begin and the end nothing else may see the outer builder, and nothing may reach the end with half a text.
    auto hasOnlySafeExits = [&](ValueID begin, ValueID outer, ProgramPoint end) {
        const Dominators& dominators = analyses.dominators();
        ProgramPoint start = useLists.pointAfter(begin);
        BitVector closingBlocks;
        for (const Use& use : useLists.usesOf(begin)) {
            if (closingOperations.contains(function.at(use.user).opcode))
                closingBlocks.set(use.block.index());
        }

        BitVector visitedBlocks;
        visitedBlocks.set(start.block.index());
        Vector<BlockID, 8> worklist { start.block };
        bool isSafe = true;
        while (isSafe && !worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            bool isFirst = block == start.block;
            bool isLast = block == end.block;
            if (outer && isUsedIn(outer, block, isFirst ? start.index : 0, isLast ? std::optional<uint32_t>(end.index) : std::nullopt))
                return false;
            bool isClosed = closingBlocks.get(block.index()) || (!isFirst && !liveness.isLiveAtHead(begin, block));
            forEachOutgoingEdge(function, block, [&](Edge edge, const Target& target) {
                if (isLast && !edge.isHandlerEdge())
                    return;
                bool hasWholeText = isClosed && !edge.isHandlerEdge();
                bool staysInside = target.block != start.block && dominators.dominates(start.block, target.block);
                bool isFollowed = liveness.isLiveAtHead(begin, target.block);
                if (outer && target.arguments.contains(outer))
                    isSafe = false;
                else if (!isFollowed && !outer)
                    isSafe &= hasWholeText || !staysInside;
                else if (!isFollowed && liveness.isLiveAtHead(outer, target.block)) {
                    isSafe &= hasWholeText && staysInside;
                    isFollowed = true;
                }
                if (isFollowed && !visitedBlocks.set(target.block.index()))
                    worklist.append(target.block);
            });
        }
        return isSafe;
    };
    auto canJoin = [&](const NestedBuilder& nested) {
        ValueID outer = outerBuilderOf(nested);
        if (joinedConcatenations.get(nested.concatenation.index()) || (outer && nestedBegins.get(outer.index())))
            return false;

        ProgramPoint start = useLists.pointOf(nested.begin);
        ProgramPoint end = useLists.pointOf(nested.concatenation);
        if (!analyses.dominates(nested.begin, end) || (outer && !analyses.dominates(outer, start)))
            return false;
        if (start.block != end.block && analyses.isInCycle(end.block) && cfg.isInCycleAvoiding(end.block, start.block))
            return false;
        bool canAppendParts = true;
        function.at(nested.concatenation).forEachOperand([&](ValueID part, uint32_t operandIndex) {
            if (part == outer)
                return;
            canAppendParts &= isPart(part);
            if (operandIndex < nested.operandIndex)
                canAppendParts &= isRematerializable(function.at(part)) || analyses.dominates(part, start);
        });
        if (!canAppendParts || !hasOnlySafeExits(nested.begin, outer, end))
            return false;
        joinedConcatenations.set(nested.concatenation.index());
        return true;
    };
    nestedBuilders.removeAllMatching([&](const NestedBuilder& nested) {
        if (canJoin(nested))
            return false;
        context.addDecline(Wire::DeclineKind::StringBuildersNotJoined, nested.concatenation);
        return true;
    });
    if (nestedBuilders.isEmpty())
        return PassOutcome::Unchanged;

    auto makeAppend = [](ValueID handle, std::span<const ValueID> parts) {
        Value append = IRBuilder::make(Opcode::StringBuilderAppend, { handle });
        append.setPartsOperands(parts);
        return append;
    };
    for (const NestedBuilder& nested : nestedBuilders) {
        ValueID begin = nested.begin;
        ValueID concatenation = nested.concatenation;
        ValueID outer = outerBuilderOf(nested);
        Vector<ValueID, 8> earlierParts;
        Vector<ValueID, 8> laterParts;
        function.at(concatenation).forEachOperand([&](ValueID part, uint32_t operandIndex) {
            if (part != outer)
                (operandIndex < nested.operandIndex ? earlierParts : laterParts).append(part);
        });

        ProgramPoint start = useLists.pointOf(begin);
        builder.setOrigin(function.at(begin).origin());
        if (!outer)
            outer = builder.insertBefore(start, IRBuilder::make(Opcode::StringBuilderBegin));
        for (ValueID& part : earlierParts) {
            if (isRematerializable(function.at(part)))
                part = builder.insertBefore(start, Value(function.at(part)));
        }
        if (!earlierParts.isEmpty())
            builder.insertBefore(start, makeAppend(outer, earlierParts.span()));

        for (const Use& use : useLists.usesOf(begin)) {
            Value& user = function.edit(use.user);
            if (!closingOperations.contains(user.opcode)) {
                user.setOperand(use.operandIndex, outer);
                continue;
            }
            if (user.opcode == Opcode::StringBuilderFinish) {
                user.become(Opcode::String);
                user.setString(program.strings.emptyString());
            } else {
                user.become(Opcode::StringBuilderRollBack);
                user.setBuilderOperand(outer);
                user.setCheckpointOperand(begin);
            }
            retype(user);
        }
        retype(becomeBuilderOperation(begin, Opcode::StringBuilderCheckpoint, outer));

        if (function.at(concatenation).opcode == Opcode::StringBuilderAppend) {
            function.edit(concatenation, ValueEdit::Definition).setPartsOperands(laterParts.span());
            continue;
        }
        bool isLikelyKey = function.at(concatenation).opcode == Opcode::Strcat && function.at(concatenation).isLikelyKey();
        builder.setOrigin(function.at(concatenation).origin());
        builder.insertBefore(useLists.pointOf(concatenation), makeAppend(outer, laterParts.span()));
        Value& finish = becomeBuilderOperation(concatenation, Opcode::StringBuilderFinish, outer);
        finish.setIsLikelyKey(isLikelyKey);
        retype(finish);
    }
    context.build.statistics().count(StatisticID::StringBuildersJoined, nestedBuilders.size());
    builder.applyInsertions();
    analyses.didChangeValues();
    return PassOutcome::Changed;
}

// Add converts an object with another hint than Strcat does.
bool canBeMarkedAsLikelyKey(const Program& program, const Function& function, const Value& value)
{
    if (Value::opcodesWithIsLikelyKey.contains(value.opcode))
        return !value.isLikelyKey();
    if (value.opcode != Opcode::Add || !isStringConcatenation(program, function, value))
        return false;
    return isStringBuilderPart(program.typeOf(function, value.leftOperand())) && isStringBuilderPart(program.typeOf(function, value.rightOperand()));
}

void markAsLikelyKey(Function& function, ValueID id)
{
    Value& string = function.edit(id);
    if (string.opcode == Opcode::Add) {
        Vector<ValueID, 2> parts { string.leftOperand(), string.rightOperand() };
        string.become(Opcode::Strcat);
        string.setVariadicOperands(parts.span());
    }
    string.setIsLikelyKey(true);
}

bool isKeyOperand(const Program& program, const Function& function, const Use& use)
{
    if (use.kind != Use::Kind::Operand)
        return false;
    const Value& user = function.at(use.user);
    if (keyedAccesses.contains(user.opcode))
        return use.operandIndex == OperandPosition::property(user.opcode);
    if (user.opcode != Opcode::CallBuiltin || builtinOperandAt(user, use.operandIndex) != BuiltinOperand::argument(0))
        return false;
    std::optional<InlineBuiltin> builtin = program.builtins().at(user.overload()).code.implementation.inlineBuiltin;
    return builtin && familyOf(*builtin) == InlineBuiltinFamily::HashTableBuiltin;
}

static bool markLikelyKeys(FunctionPassContext& context)
{
    Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    BitVector visitedValues;
    Vector<ValueID, 8> worklist;
    auto isOnlyUsedAsKey = [&](ValueID string) {
        visitedValues.clearAll();
        visitedValues.set(string.index());
        worklist.shrink(0);
        worklist.append(string);
        bool isUsedAsKey = false;
        while (!worklist.isEmpty()) {
            std::span<const Use> uses = useLists.usesOf(worklist.takeLast());
            if (uses.empty())
                return false;
            for (const Use& use : uses) {
                if (isKeyOperand(context.program, function, use)) {
                    isUsedAsKey = true;
                    continue;
                }
                // As the call by name that stands in where a Map is not what its type says.
                if (function.at(use.block).frequency != BlockFrequency::Normal)
                    continue;

                ValueID sameString;
                if (use.kind == Use::Kind::EdgeArgument)
                    sameString = function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
                else if (use.kind == Use::Kind::Operand && isCast(function.at(use.user).opcode))
                    sameString = use.user;
                else
                    return false;
                if (!visitedValues.set(sameString.index()))
                    worklist.append(sameString);
            }
        }
        return isUsedAsKey;
    };

    bool didChange = false;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (!canBeMarkedAsLikelyKey(context.program, function, function.at(id)) || !isOnlyUsedAsKey(id))
                continue;
            markAsLikelyKey(function, id);
            didChange = true;
        }
    }
    if (didChange)
        context.analyses.didChangeValues();
    return didChange;
}

static StringBuilderPlan analyzeStringBuilders(FunctionPassContext& context)
{
    bool hasConcatenation = std::ranges::any_of(context.function.blocks.span(), [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) { return isStringConcatenation(context.program, context.function, context.function.at(id)); });
    });
    return hasConcatenation ? StringBuilderStep { context }.findWebs() : StringBuilderPlan { };
}

static PassOutcome createAndJoinBuilders(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    StringBuilderPlan plan = analyzeStringBuilders(context);
    while (StringBuilderStep { context }.mergeNestedWebs(plan) == PassOutcome::Changed || splitInFrontOfCoveredBlocks(context, plan)) {
        context.analyses.didChangeControlFlow();
        plan = analyzeStringBuilders(context);
        outcome = PassOutcome::Changed;
    }
    if (plan.webs.isEmpty())
        return outcome;

    reportDecisions(context, plan);
    if (!plan.createdWebs().empty()) {
        Vector<ValueID, 8> appends = StringBuilderStep { context }.rewriteWebs(plan);
        StringBuilderStep { context }.foldConversionsInto(appends.span());
        outcome = PassOutcome::Changed;
    }
    outcome |= StringBuilderStep { context }.joinNestedBuilders();
    if (markLikelyKeys(context))
        outcome = PassOutcome::Changed;
    return outcome | expandAppendedStringRanges(context);
}

PassOutcome createStringBuilders(FunctionPassContext& context)
{
    PassOutcome outcome = createAndJoinBuilders(context);
    // Each step that edits tells the analyses when it is done, for the step behind it asks them. So whatever they hold was made behind the last edit.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

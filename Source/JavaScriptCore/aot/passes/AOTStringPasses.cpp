/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringBuilderAnalysis.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTable.h"
#include "AOTDecidedBranches.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEscapeAnalysis.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include "AOTIRBuilder.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTKnownValue.h"
#include "AOTLiveness.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPass.h"
#include "AOTPropertyAccessElements.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTResultRules.h"
#include "AOTRewrite.h"
#include "AOTRuleSupport.h"
#include "AOTSchemaInlineBuiltins.h"
#include "AOTSchemaWalkers.h"
#include "AOTSlotPlan.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTSpecifiedValues.h"
#include "AOTSplitStringWalk.h"
#include "AOTStringArena.h"
#include "AOTStringRanges.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "YarrFlags.h"
#include <algorithm>
#include <array>
#include <ranges>
#include <wtf/BitVector.h>
#include <wtf/FastBitVector.h>
#include <wtf/PrintStream.h>
#include <wtf/UnionFind.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t stateOperandIndex = 0;
static_assert(OperandIndex::Add::left == stateOperandIndex);
static_assert(OperandIndex::Strcat::parts == stateOperandIndex);

static constexpr OpcodeSet equalityComparisonsOfStrings { Opcode::StrictEq, Opcode::Eq };
static constexpr OpcodeSet closingOperations { Opcode::StringBuilderFinish, Opcode::StringBuilderDiscard };
static constexpr OpcodeSet redirectableOperations = closingOperations | OpcodeSet {
    Opcode::StringBuilderAppend, Opcode::StringBuilderAppendJSON, Opcode::StringBuilderAppendJSONMembers, Opcode::StringBuilderCheckpoint, Opcode::StringBuilderRollBack, Opcode::CallDirect,
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
    // The parameters that are typed as plain strings while the pass runs and that some way leaves unfilled.
    const BitVector& statesReadAsStrings;
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
    // Nobody reads what is Unspecified, so nothing is appended for it. It is asked what it stands in for.
    bool isPart(ValueID id) const { return isStringBuilderPart(function.at(id).opcode == Opcode::Unspecified ? typeOf(id).withoutPrimitives(Primitive::Unspecified) : typeOf(id)); }
    bool isEmptyString(ValueID id) const { return stringConstantOf(program, function, id) == program.strings.emptyString(); }
    // As a state of a web such a word is never read where it is unfilled. As the text that a web begins with it would be.
    bool mayBeUnfilled(ValueID id) const
    {
        return function.at(id).opcode != Opcode::Unspecified && (typeOf(id).mayBeUnspecified() || statesReadAsStrings.get(sameObjectThroughCasts(function, id).valueInFrontOfFacts().index()));
    }

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
    if (!cursors.isEmpty()) {
        Liveness livenessOfCursors(function, cfg, useLists, { .trackedValues = &cursors });
        operationBeforeCursors.fill(ValueID(), numberOfValues);
        for (BlockID blockID : cfg.reversePostOrder()) {
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
        }
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
                while (use.kind == Use::Kind::Operand && isCastThatProvesNothing(function.at(use.user).opcode)) {
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

    // The append that replaces a concatenation completes or leaves the content as it was, so the builder holds the member wherever the block can throw.
    auto isLiveOnlyForHandler = [&](ValueID member, BlockID blockID, uint32_t index) {
        const Block& block = function.at(blockID);
        if (!block.handler || (!block.handler->arguments.contains(member) && !liveness->isLiveAtHead(member, block.handler->block)))
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
        for (BlockID blockID : cfg.reversePostOrder()) {
            if (!numberOfUndecidedWebs)
                return false;
            if (!blocksWithMembers.quickGet(blockID.index()) && liveness->liveAtHead(blockID).empty())
                continue;
            LocalCalc live(*liveness, blockID);
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
                });
            }
            for (uint32_t index = function.at(blockID).values.size(); index--;) {
                if (cutAtValue(blockID, index, live))
                    return true;
                live.execute(index);
            }
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
        }
        return false;
    };

    // A text goes from the operands of a concatenation to its result, and from arguments to a parameter.
    Vector<ValueID> argumentsOfParameters;
    Vector<std::pair<uint32_t, uint32_t>> argumentRanges;
    std::optional<StronglyConnectedComponents> textCycles;
    auto sourcesOfText = [&](unsigned index) -> std::span<const ValueID> {
        const Value& value = function.at(ValueID(index));
        if (value.opcode == Opcode::Parameter)
            return argumentsOfParameters.subspan(argumentRanges[index].first, argumentRanges[index].second);
        if (isCastThatProvesNothing(value.opcode) || isStringConcatenation(program, function, value))
            return value.operands();
        return { };
    };
    auto findTextCycles = [&] {
        argumentRanges.fill({ 0, 0 }, numberOfValues);
        for (BlockID blockID : cfg.reversePostOrder()) {
            std::span<const ValueID> parameters = sharedParametersOf(blockID);
            for (uint32_t index = 0; index < parameters.size(); ++index) {
                uint32_t start = argumentsOfParameters.size();
                for (Edge edge : cfg.incomingEdges(blockID))
                    argumentsOfParameters.append(argumentOn(edge, index));
                argumentRanges[parameters[index].index()] = { start, argumentsOfParameters.size() - start };
            }
        }
        textCycles = computeStronglyConnectedComponents(numberOfValues, sourcesOfText);
    };
    // A text that grows around a loop stays in one builder all the way around. Where any step of the way takes it as a string, a builder would be begun with a copy of the whole text at every turn.
    auto cutTextsCopiedAroundCycles = [&] {
        if (!cfg.hasCycle() || !numberOfUndecidedWebs)
            return false;
        if (!textCycles)
            findTextCycles();
        BitVector cyclesWithCopies;
        for (ValueID member : candidates) {
            std::optional<uint32_t> web = undecidedWebOf(member);
            if (!web || !textCycles->isInCycle.get(member.index()))
                continue;
            uint32_t cycle = textCycles->componentOf[member.index()];
            bool isParameter = function.at(member).opcode == Opcode::Parameter;
            std::span<const ValueID> sources = sourcesOfText(member.index());
            for (uint32_t index = 0; index < sources.size(); ++index) {
                bool staysInBuilder = plan.webOf(sources[index]) == web && (isParameter || index == stateOperandIndex);
                if (!staysInBuilder && textCycles->componentOf[sources[index].index()] == cycle)
                    cyclesWithCopies.set(cycle);
            }
        }
        if (cyclesWithCopies.isEmpty())
            return false;
        for (ValueID id : candidates) {
            if (cyclesWithCopies.get(textCycles->componentOf[id.index()]))
                isCut.quickSet(id.index());
        }
        return true;
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
        if (!numberOfUndecidedWebs || (!cutAtInterference() && !cutTextsCopiedAroundCycles()))
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
            return entry.edge && mayBeUnfilled(entry.value);
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
        while (isCastThatProvesNothing(function.at(nested.lastState).opcode) && useLists.useCount(nested.lastState) == 1) {
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
        // The front is joined to each first state where the web is entered: a read in front of whatever says whether that state is there.
        bool wouldReadEmptyFirstState = std::ranges::any_of(nested.web->entries, [&](const StringWebEntry& entry) {
            return mayBeUnfilled(entry.value);
        });
        if (wouldReadEmptyFirstState)
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
            if (isCastThatProvesNothing(user.opcode)) {
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
    if (std::optional<unsigned> property = OperandPosition::property(user.opcode))
        return use.operandIndex == *property;
    if (user.opcode != Opcode::CallBuiltin || builtinOperandAt(user, use.operandIndex) != BuiltinOperand::argument(0))
        return false;
    std::optional<InlineBuiltin> builtin = program.builtins().at(user.overload()).code.implementation.inlineBuiltin;
    return builtin && familyOf(*builtin) == InlineBuiltinFamily::HashTableBuiltin;
}

bool isOnlyKeyOfProperties(const Function& function, const UseLists& useLists, ValueID string)
{
    BitVector visitedValues;
    visitedValues.set(string.index());
    Vector<ValueID, 8> worklist { string };
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            ValueID sameString;
            if (use.kind == Use::Kind::EdgeArgument)
                sameString = function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex];
            else if (use.kind == Use::Kind::Operand && OperandPosition::property(function.at(use.user).opcode) == use.operandIndex)
                continue;
            else if (use.kind == Use::Kind::Operand && isCast(function.at(use.user).opcode))
                sameString = use.user;
            else if (function.at(use.block).frequency.predicted() == BlockFrequency::Normal)
                return false;
            if (sameString && !visitedValues.set(sameString.index()))
                worklist.append(sameString);
        }
    }
    return true;
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
                if (function.at(use.block).frequency.predicted() != BlockFrequency::Normal)
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

static StringBuilderPlan analyzeStringBuilders(FunctionPassContext& context, const BitVector& statesReadAsStrings)
{
    bool hasConcatenation = std::ranges::any_of(context.function.blocks.span(), [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) { return isStringConcatenation(context.program, context.function, context.function.at(id)); });
    });
    return hasConcatenation ? StringBuilderStep { context, statesReadAsStrings }.findWebs() : StringBuilderPlan { };
}

// The parts of an append are written one after the other, and each of them may find that the text gets too long. A handler that goes on with the builder wants the text as it was in front of the append.
static PassOutcome rollBackAppendsUnderHandlers(FunctionPassContext& context)
{
    Function& function = context.function;
    const CFG& cfg = context.analyses.cfg();
    auto isRolledBackIn = [&](BlockID handler, ValueID builder) {
        return std::ranges::any_of(function.at(handler).values, [&](ValueID id) {
            return function.at(id).opcode == Opcode::StringBuilderRollBack && function.at(id).builderOperand() == builder;
        });
    };
    auto forEachAppendOfSeveralParts = [&](BlockID blockID, const Invocable<void(uint32_t index, ValueID builder)> auto& functor) {
        const Block& block = function.at(blockID);
        if (!block.handler)
            return;
        for (uint32_t index = block.values.size(); index--;) {
            const Value& value = function.at(block.values[index]);
            if (value.opcode == Opcode::StringBuilderAppend && value.partsOperands().size() > 1 && !isRolledBackIn(block.handler->block, value.builderOperand()))
                functor(index, value.builderOperand());
        }
    };

    BitVector builders(function.values.size());
    for (BlockID blockID : cfg.reversePostOrder()) {
        forEachAppendOfSeveralParts(blockID, [&](uint32_t, ValueID builder) {
            builders.quickSet(builder.index());
        });
    }
    if (builders.isEmpty())
        return PassOutcome::Unchanged;

    Liveness liveness(function, cfg, context.analyses.useLists(), LivenessOptions { .trackedValues = &builders });
    Vector<ProgramPoint, 4> appends;
    for (BlockID blockID : cfg.reversePostOrder()) {
        forEachAppendOfSeveralParts(blockID, [&](uint32_t index, ValueID builder) {
            const Target& handler = *function.at(blockID).handler;
            if (handler.arguments.contains(builder) || liveness.isLiveAtHead(builder, handler.block))
                appends.append(ProgramPoint { blockID, index });
        });
    }
    if (appends.isEmpty())
        return PassOutcome::Unchanged;

    // The appends of a block come last to first, so the block in front of a split keeps its indexes.
    IRBuilder builder(context.analyses.context(), function, context.types);
    for (ProgramPoint point : appends) {
        ValueID append = function.at(point.block).values[point.index];
        ValueID handle = function.at(append).builderOperand();
        builder.setOrigin(function.at(append).origin());
        builder.setHandler(std::optional<Target> { *function.at(point.block).handler });
        std::array<uint32_t, 2> indexes { point.index, point.index + 1 };
        BlockID covered = builder.splitBlock(point.block, indexes).first();
        ValueID checkpoint = builder.insertBefore(pointBeforeTerminatorOf(function, point.block), IRBuilder::make(Opcode::StringBuilderCheckpoint, { handle }));
        builder.applyInsertions();

        BlockID failed = builder.addHandlerBlock(BlockRole::CleanupHandler, ValueID());
        function.edit(covered, { BlockEdit::Edges, BlockEdit::Arguments }).handler = Target { failed, { }, { } };
        builder.moveToEndOf(failed);
        ValueID exception = builder.append(Opcode::CatchException, { });
        builder.append(Opcode::StringBuilderRollBack, { handle, checkpoint });
        builder.emitRethrow(exception);
    }
    context.analyses.didChangeControlFlow();
    return PassOutcome::Changed;
}

static PassOutcome createAndJoinBuilders(FunctionPassContext& context, const BitVector& statesReadAsStrings)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    StringBuilderPlan plan = analyzeStringBuilders(context, statesReadAsStrings);
    while (StringBuilderStep { context, statesReadAsStrings }.mergeNestedWebs(plan) == PassOutcome::Changed || splitInFrontOfCoveredBlocks(context, plan)) {
        context.analyses.didChangeControlFlow();
        plan = analyzeStringBuilders(context, statesReadAsStrings);
        outcome = PassOutcome::Changed;
    }
    if (plan.webs.isEmpty())
        return outcome;

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
    if (!plan.createdWebs().empty()) {
        Vector<ValueID, 8> appends = StringBuilderStep { context, statesReadAsStrings }.rewriteWebs(plan);
        StringBuilderStep { context, statesReadAsStrings }.foldConversionsInto(appends.span());
        outcome = PassOutcome::Changed;
    }
    outcome |= StringBuilderStep { context, statesReadAsStrings }.joinNestedBuilders();
    if (markLikelyKeys(context))
        outcome = PassOutcome::Changed;
    outcome |= expandAppendedStringRanges(context);
    return outcome | rollBackAppendsUnderHandlers(context);
}

// A text that grows may begin as Unspecified on a way that never reads it: a builder begins empty there, and then nothing is left unfilled.
// While the pass looks for such texts their states are plain strings. What stays a string gets its facts again behind the pass, where its uses stand. Where the pass makes nothing, all is put back as it was.
struct StatesReadAsStrings {
    struct State {
        ValueID parameter;
        TypeID type;
    };
    struct ReplacedUse {
        Use use;
        ValueID cast;
    };
    Vector<State, 8> states;
    BitVector parameters;
    Vector<std::pair<ValueID, Value>, 8> casts;
    Vector<ReplacedUse, 8> uses;

    bool isEmpty() const { return states.isEmpty(); }
};

static StatesReadAsStrings readStatesThatBeginUnspecifiedAsStrings(FunctionPassContext& context)
{
    Function& function = context.function;
    if (!context.analyses.opcodesOfValues().contains(Opcode::Unspecified))
        return { };
    const CFG& cfg = context.analyses.cfg();
    const UseLists& useLists = context.analyses.useLists();
    auto typeOf = [&](ValueID id) -> const Type& { return context.types.at(function.at(id).type); };

    BitVector isState(function.values.size());
    Vector<std::pair<BlockID, uint32_t>, 8> states;
    for (BlockID block : cfg.reversePostOrder()) {
        const CompactList<ValueID>& parameters = function.at(block).parameters;
        for (uint32_t index = 0; block != cfg.root() && index < parameters.size(); ++index) {
            const Type& type = typeOf(parameters[index]);
            if (!type.mayBeUnspecified() || !type.withoutPrimitives(Primitive::Unspecified).isString())
                continue;
            states.append({ block, index });
            isState.quickSet(parameters[index].index());
        }
    }
    auto isCastBehindFact = [&](const Use& use) {
        return use.kind == Use::Kind::Operand && function.at(use.user).opcode == Opcode::AssumeType && use.operandIndex == OperandIndex::AssumeType::value && function.at(use.user).factOperand();
    };
    for (bool didRemove = true; didRemove;) {
        didRemove = states.removeAllMatching([&](auto state) {
            ValueID parameter = function.at(state.first).parameters[state.second];
            bool beginsUnspecified = std::ranges::all_of(cfg.incomingEdges(state.first), [&](Edge edge) {
                ValueID passed = valuePassedOn(function, edge, state.second);
                return passed && (!typeOf(passed).mayBeUnspecified() || function.at(passed).opcode == Opcode::Unspecified || isState.quickGet(passed.index()));
            });
            bool isOnlyCastOrPassedOn = std::ranges::all_of(useLists.usesOf(parameter), [&](const Use& use) {
                if (use.isEdgeUse())
                    return isState.quickGet(function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex].index());
                return isCastBehindFact(use);
            });
            if (beginsUnspecified && isOnlyCastOrPassedOn)
                return false;
            isState.quickClear(parameter.index());
            return true;
        });
    }
    if (states.isEmpty())
        return { };

    StatesReadAsStrings result;
    for (auto [block, index] : states) {
        ValueID parameter = function.at(block).parameters[index];
        result.states.append({ parameter, function.at(parameter).type });
        result.parameters.set(parameter.index());
        for (const Use& use : useLists.usesOf(parameter)) {
            if (!isCastBehindFact(use))
                continue;
            result.casts.append({ use.user, function.at(use.user) });
            for (const Use& useOfCast : useLists.usesOf(use.user))
                result.uses.append({ useOfCast, use.user });
        }
    }
    for (const auto& replaced : result.uses)
        replaceUse(function, replaced.use, function.at(replaced.cast).valueOperand());
    // A cast that nobody names any more would count as a read of the state.
    for (const auto& [cast, value] : result.casts)
        becomeUndefined(function, cast);
    for (const auto& state : result.states)
        function.edit(state.parameter, ValueEdit::Type).type = context.types.intern(typeOf(state.parameter).withoutPrimitives(Primitive::Unspecified));
    context.analyses.didChangeValues();
    return result;
}

static void putBack(FunctionPassContext& context, const StatesReadAsStrings& read)
{
    Function& function = context.function;
    for (const auto& state : read.states)
        function.edit(state.parameter, ValueEdit::Type).type = state.type;
    for (const auto& [cast, value] : read.casts)
        function.edit(cast) = value;
    for (const auto& replaced : read.uses)
        replaceUse(function, replaced.use, replaced.cast);
    context.analyses.didChangeValues();
}

PassOutcome createStringBuilders(FunctionPassContext& context)
{
    StatesReadAsStrings statesReadAsStrings = readStatesThatBeginUnspecifiedAsStrings(context);
    PassOutcome outcome = createAndJoinBuilders(context, statesReadAsStrings.parameters);
    if (!statesReadAsStrings.isEmpty()) {
        if (outcome == PassOutcome::Unchanged)
            putBack(context, statesReadAsStrings);
        else {
            IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
            nameFactsWhereUnspecifiedValuesAreUsed(builder, context.analyses);
        }
    }
    // Each step that edits tells the analyses when it is done, for the step behind it asks them. So whatever they hold was made behind the last edit.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

static constexpr uint32_t startResult = 0;
static constexpr uint32_t endResult = 1;

static constexpr InlineBuiltin inlineBuiltinOf(StringRangeMethod method)
{
    switch (method) {
    case StringRangeMethod::Slice:
        return InlineBuiltin::StringSlice;
    case StringRangeMethod::Substring:
        return InlineBuiltin::StringSubstring;
    case StringRangeMethod::Substr:
        return InlineBuiltin::StringSubstr;
    case StringRangeMethod::SingleIndex:
        return InlineBuiltin::StringCharAt;
    case StringRangeMethod::SingleRelativeIndex:
        return InlineBuiltin::StringAt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

std::optional<StringRangeSource> stringRangeSourceOf(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode == Opcode::Substring || value.opcode == Opcode::SubstringOrUndefined)
        return StringRangeSource { .string = value.stringOperand(), .bounds = StringRangeBounds { value.startOperand(), value.endOperand() }, .mayBeUndefined = value.opcode == Opcode::SubstringOrUndefined };
    if (value.opcode == Opcode::CallBuiltin) {
        if (value.builtinCallForm() != BuiltinCallForm::Whole || overloadOf(program, value).result.substringOf != BuiltinOperand::receiver())
            return std::nullopt;
        std::optional<InlineBuiltin> builtin = inlineImplementationOf(program, value);
        auto method = std::ranges::find_if(allMembersOf<StringRangeMethod>, [&](StringRangeMethod method) { return inlineBuiltinOf(method) == builtin; });
        ValueID receiver = value.receiverOperand();
        if (method == allMembersOf<StringRangeMethod>.end() || !receiver || !program.typeOf(function, receiver).isAnyString())
            return std::nullopt;

        StringRangeSource source { .string = receiver, .method = *method, .mayBeUndefined = *method == StringRangeMethod::SingleRelativeIndex };
        std::span<const ValueID> arguments = value.argumentsOperands();
        if (arguments.size() > (source.isSingleUnit() ? 1u : 2u))
            return std::nullopt;
        std::array<ValueID*, 2> positions { &source.first, &source.second };
        for (size_t index = 0; index < arguments.size(); ++index) {
            const Type& type = program.typeOf(function, arguments[index]);
            if (type.isNumber())
                *positions[index] = arguments[index];
            else if (type.isNever() || !type.hasOnlyPrimitives(Primitive::Undefined))
                return std::nullopt;
        }
        // A position that is never negative counts from the start.
        if (source.method == StringRangeMethod::SingleRelativeIndex && source.first) {
            const Type& position = program.typeOf(function, source.first);
            if (position.isInteger() && position.integers.minimum >= 0)
                source.method = StringRangeMethod::SingleIndex;
        }
        return source;
    }
    if (value.opcode != Opcode::ElemGet)
        return std::nullopt;
    std::optional<ElementAccessPlan> plan = planElementAccessOf(program, function, value);
    // SingleIndex truncates the position like charAt, but s[1.5] and s[NaN] are undefined.
    if (!plan || plan->form != ElementAccessForm::String || !program.typeOf(function, value.propertyOperand()).isInteger())
        return std::nullopt;
    return StringRangeSource { .string = value.objectOperand(), .first = value.propertyOperand(), .method = StringRangeMethod::SingleIndex, .mayBeUndefined = true };
}

StringRangeBounds insertStringRange(IRBuilder& builder, ProgramPoint point, const StringRangeSource& source, std::optional<StringRangeBounds> enclosing)
{
    auto insert = [&](Value&& value) { return builder.insertBefore(point, WTF::move(value)); };
    // A range lies within its string, so the sums and differences of bounds are positions in the string, also where the types of the bounds are too wide to say so.
    auto insertPosition = [&](Opcode opcode, ValueID left, ValueID right) {
        ValueID position = insert(IRBuilder::make(opcode, { left, right }));
        return builder.context().program.typeOf(builder.function(), position).isInteger() ? position : insert(IRBuilder::make(Opcode::ToInt32, { position }));
    };
    StringRangeBounds bounds;
    if (source.bounds)
        bounds = *source.bounds;
    else {
        ValueID length = enclosing ? insertPosition(Opcode::Sub, enclosing->end, enclosing->start) : insert(IRBuilder::make(Opcode::GetStringLength, { source.string }));
        ValueID first = source.first ? source.first : insert(makeInteger(0));
        Value range = IRBuilder::make(Opcode::StringRange, { length, first, source.second });
        range.setMethod(source.method);
        ValueID results = insert(WTF::move(range));
        bounds.start = insert(makeResultAt(results, startResult));
        bounds.end = insert(makeResultAt(results, endResult));
    }
    if (!enclosing)
        return bounds;
    ValueID start = insertPosition(Opcode::Add, enclosing->start, bounds.start);
    return { start, insertPosition(Opcode::Add, enclosing->start, bounds.end) };
}

Value makeStringRangeRead(Opcode read, ValueID string, std::optional<StringRangeBounds> bounds, ValueID position)
{
    RELEASE_ASSERT(familyOf(read) == OperationFamily::StringRangeRead);
    Value value = IRBuilder::make(read);
    value.setStringOperand(string);
    if (bounds) {
        value.setStartOperand(bounds->start);
        value.setEndOperand(bounds->end);
    }
    if (position)
        value.setPositionOperand(position);
    return value;
}

BuiltinCallSites collectBuiltinCallSites(FunctionPassContext& context)
{
    return BuiltinCallSites(context.sitesIn(SiteList::BuiltinCalls));
}

using RangeCalls = Vector<std::pair<ValueSite, StringRangeSource>>;

static PassOutcome expandRangeCalls(FunctionPassContext& context, const RangeCalls& calls)
{
    if (calls.isEmpty())
        return PassOutcome::Unchanged;

    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const auto& [site, source] : calls) {
        ReplacedEffects replaced;
        replaced.add({ context.program, context.inputs.effects }, function, function.at(site.value));

        builder.setOrigin(function.at(site.value).origin());
        StringRangeBounds bounds = insertStringRange(builder, site.point(), source, std::nullopt);

        Value& value = function.edit(site.value);
        value.become(Opcode::Substring);
        value.setStringOperand(source.string);
        value.setStartOperand(bounds.start);
        value.setEndOperand(bounds.end);
        checkRewrite(context, site, replaced);
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

PassOutcome expandStringRanges(FunctionPassContext& context, const BuiltinCallSites& sites)
{
    RangeCalls calls;
    for (const ValueSite& site : sites) {
        std::optional<StringRangeSource> source = stringRangeSourceOf(context.program, context.function, context.function.at(site.value));
        // A Substring does not say that it holds at most one unit, and selection reads the code unit of charAt and at only from the call.
        if (source && !source->isSingleUnit())
            calls.append({ site, *source });
    }
    return expandRangeCalls(context, calls);
}

PassOutcome expandAppendedStringRanges(FunctionPassContext& context)
{
    const Function& function = context.function;
    RangeCalls calls;
    for (const ValueSite& site : collectBuiltinCallSites(context)) {
        const Value& value = function.at(site.value);
        const BuiltinForms* forms = context.program.builtins().formsOf(overloadOf(context.program, value).builtin);
        if (!forms || (forms->appendForm != InlineBuiltin::StringSliceAppend && forms->appendForm != InlineBuiltin::StringSubstringAppend))
            continue;
        std::optional<Use> use = context.analyses.useLists().singleUse(site.value);
        if (!use || use->kind != Use::Kind::Operand || use->block != site.block || function.at(use->user).opcode != Opcode::StringBuilderAppend)
            continue;
        if (std::optional<StringRangeSource> source = stringRangeSourceOf(context.program, function, value))
            calls.append({ site, *source });
    }
    return expandRangeCalls(context, calls);
}

std::optional<SplitString> splitStringOf(FunctionPassContext& pass, const Value& call)
{
    const Program& program = pass.program;
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || !call.receiverOperand() || call.argumentsOperands().size() != 1)
        return std::nullopt;
    const BuiltinImplementation& implementation = program.builtins().at(call.overload()).code.implementation;
    if (implementation.inlineBuiltin != InlineBuiltin::StringSplit || static_cast<SearchValueKind>(implementation.inlineParameters[0]) != SearchValueKind::String)
        return std::nullopt;

    ValueID string = call.receiverOperand();
    ValueID separator = call.argumentsOperands()[0];
    const Type& separatorType = program.typeOf(pass.function, separator);
    if (!program.typeOf(pass.function, string).isAnyString() || separatorType.isNever() || !separatorType.hasOnlyPrimitives(Primitive::LiteralString))
        return std::nullopt;
    if (std::ranges::any_of(separatorType.stringLiterals, [&](StringID text) { return program.strings[text].empty(); }))
        return std::nullopt;

    TypingContext typing { program, pass.function, pass.types };
    BuiltinOperandTypes operands { pass.function.at(string).type, { pass.function.at(separator).type, pass.types.intern(Type::integer(Range::length())) } };
    OverloadMatch match = matchOverload({ typing, Builtins::StringPrototypeIndexOf, BuiltinCallMode::Call, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty() || !match.overload)
        return std::nullopt;
    return SplitString { string, separator, match.overload };
}

Range positionRangeOfSplitString()
{
    // The end of a separator is the sum of an index and a length, and their types only say that each is at most the greatest length.
    return { 0, 2 * Range::length().maximum };
}

SplitStringStep appendSplitStringStep(IRBuilder& builder, const SplitString& splitString, ValueID position)
{
    TypeArena& types = builder.types();
    ValueID length = builder.append(Opcode::GetStringLength, { splitString.string });
    ValueID hasSubstring = builder.append(Opcode::LessEq, { position, length });
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(hasSubstring, 0, BlockFrequency::Normal, BlockFrequency::Normal);
    ValueID start = branch.provenOnTaken;

    ValueID found = builder.append(makeBuiltinCall(splitString.separatorSearch, BuiltinCallForm::Whole, { splitString.string, { splitString.separator, start } }, { }));
    ValueID isLast = builder.append(Opcode::Less, { found, builder.appendInteger(0) });

    BlockID merge = builder.addBlock(builder.function().at(builder.currentBlock()).frequency);
    ValueID end = builder.addParameter(merge, types.intern(Type::integer(Range::length())));
    ValueID next = builder.addParameter(merge, types.intern(Type::integer(positionRangeOfSplitString())));
    IRBuilder::ProvenBranch search = builder.emitBranchWithProof(isLast, 0, BlockFrequency::Normal, BlockFrequency::Normal);
    builder.emitJump(merge, { length, builder.append(Opcode::Add, { length, builder.appendInteger(1) }) });

    builder.moveToEndOf(search.notTaken);
    ValueID separatorLength = builder.append(Opcode::GetStringLength, { splitString.separator });
    builder.emitJump(merge, { search.provenOnNotTaken, builder.append(Opcode::Add, { search.provenOnNotTaken, separatorLength }) });

    builder.moveToEndOf(merge);
    return { branch.notTaken, start, end, next };
}

PassOutcome fuseConcatenations(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    Vector<std::pair<BlockID, BitVector>, 2> absorbedValues;
    bool didMoveStrings = false;
    bool didDropParts = false;

    auto typeOf = [&](ValueID id) -> const Type& { return program.typeOf(function, id); };
    auto isText = [&](ValueID id) { return function.at(id).opcode == Opcode::String; };
    auto hasObjectOperand = [&](const Value& value) { return std::ranges::any_of(value.operands(), [&](ValueID operand) { return typeOf(operand).hasObjects(); }); };
    auto hasSymbolOperand = [&](const Value& value) { return std::ranges::any_of(value.operands(), [&](ValueID operand) { return !!(typeOf(operand).primitives & Primitive::AnySymbol); }); };
    auto isConcatenation = [&](const Value& value) {
        if (!concatenationOpcodes.contains(value.opcode) || !program.at(value.type).isString() || hasObjectOperand(value) || hasSymbolOperand(value))
            return false;
        return value.opcode == Opcode::Strcat || typeOf(value.leftOperand()).isAnyString() || typeOf(value.rightOperand()).isAnyString();
    };
    // A concatenation converts each of its parts as ToString does.
    auto isConversionOfPrimitive = [&](const Value& value) {
        return value.opcode == Opcode::ToString && (!hasObjectOperand(value) || context.build.isSabotaged(Sabotage::FusedConversionsRunCode)) && !hasSymbolOperand(value);
    };
    // No string is too long to be made of such parts, so the concatenation throws nothing and may run at any later time.
    auto hasBoundedLength = [&](const Value& value) {
        JoinedTexts texts;
        for (ValueID operand : value.operands())
            texts.add(program, typeOf(operand));
        return texts.fitInString();
    };
    // A key is looked up by its parts, and no string is made of a number among them.
    auto mayBecomeKey = [&](ValueID string) {
        const Value& value = function.at(string);
        if (Value::opcodesWithIsLikelyKey.contains(value.opcode) && value.isLikelyKey())
            return true;
        std::span<const Use> uses = analyses.useLists().usesOf(string);
        return !uses.empty() && std::ranges::all_of(uses, [&](const Use& use) {
            if (use.kind == Use::Kind::TerminatorOperand)
                return function.at(use.block).terminator.kind == TerminatorKind::Return;
            return isKeyOperand(program, function, use);
        });
    };
    // A concatenation fails for lack of memory or for its length, with one error. Which of two such operations fails first, nobody can tell: `a[i] + a[j] + a[k]` reads a character, which may have to be made, between two concatenations.
    // The analyses hear of a concatenation that has absorbed another at the end of the walk, so they are asked about the other values alone.
    auto hasObservableEffects = [&](ValueID id) { return !isConcatenation(function.at(id)) && !analyses.isRemovableWhenUnused(id); };
    auto hasObservableEffectsBetween = [&](BlockID id, uint32_t begin, uint32_t end) { return std::ranges::any_of(function.at(id).values.subspan(begin, end - begin), hasObservableEffects); };

    auto becomeConcatenationOf = [&](ValueID id, std::span<const ValueID> parts) {
        bool isLikelyKey = Value::opcodesWithIsLikelyKey.contains(function.at(id).opcode) && function.at(id).isLikelyKey();
        Value& concatenation = function.edit(id, ValueEdit::Definition);
        concatenation.become(Opcode::Strcat);
        concatenation.setVariadicOperands(parts);
        concatenation.setIsLikelyKey(isLikelyKey);
    };
    auto markAbsorbed = [&](ValueLocation location) {
        size_t index = absorbedValues.findIf([&](const auto& absorbed) { return absorbed.first == location.block; });
        if (index == notFound) {
            index = absorbedValues.size();
            absorbedValues.append({ location.block, BitVector() });
        }
        absorbedValues[index].second.set(location.index);
    };
    auto eraseAbsorbedValues = [&] {
        size_t numberOfAbsorbedValues = 0;
        for (const auto& [block, indexes] : std::exchange(absorbedValues, { })) {
            builder.eraseValues(block, indexes);
            numberOfAbsorbedValues += indexes.bitCount();
        }
        // The builder knows what it has erased, not the operands that went to whoever absorbed it, nor the copies that its other readers were handed.
        if (numberOfAbsorbedValues)
            analyses.didChangeValues();
        return numberOfAbsorbedValues;
    };

    // An access that fails its test reads the property by name and comes back, so the start of a key, `a.id + ","`, reaches the rest of the expression as a parameter of the block where the ways meet.
    auto makeStringsBehindMerges = [&] {
        struct MergedString {
            BlockID block;
            uint32_t parameterIndex { 0 };
            ValueID parameter;
            ValueID pattern;
            bool isValid { true };
            bool isWanted { false };
            Vector<std::pair<Edge, ValueID>, 4> incoming { };
            Vector<ValueID, 4> partParameters { };
        };
        const CFG& cfg = analyses.cfg();
        const UseLists& uses = analyses.useLists();
        Vector<MergedString, 4> merged;
        auto find = [&](ValueID parameter) -> MergedString* {
            size_t index = merged.findIf([&](const MergedString& known) { return known.parameter == parameter; });
            return index == notFound ? nullptr : &merged[index];
        };
        auto parameterAt = [&](const Use& use) { return function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex]; };
        // Each part is a number or a string of the source text, so the concatenation gives the same string wherever it runs, and throws nothing. Nobody reads it where it is made.
        auto isMadeOnlyForEdges = [&](ValueID id) {
            const Value& value = function.at(id);
            if (!isConcatenation(value) || (value.opcode == Opcode::Strcat && value.isLikelyKey()))
                return false;
            if (!std::ranges::all_of(value.operands(), [&](ValueID part) { return isText(part) || typeOf(part).isNumber(); }) || uses.locationOf(id).placement != ValuePlacement::InBlock || !uses.useCount(id))
                return false;
            return std::ranges::all_of(uses.usesOf(id), [](const Use& use) { return use.kind == Use::Kind::EdgeArgument; });
        };
        auto haveSameForm = [&](ValueID first, ValueID second) {
            return std::ranges::equal(function.at(first).operands(), function.at(second).operands(), [&](ValueID a, ValueID b) {
                return isText(a) == isText(b) && (!isText(a) || function.at(a).string() == function.at(b).string());
            });
        };
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                if (!isMadeOnlyForEdges(id))
                    continue;
                for (const Use& use : uses.usesOf(id)) {
                    if (!find(parameterAt(use)))
                        merged.append(MergedString { targetOf(function, use.edge()).block, use.operandIndex, parameterAt(use), id });
                }
            }
        }
        if (merged.isEmpty())
            return;

        // A block that nothing reaches any more stays until the blocks are compacted. What it passes to a merge has no form, and its edge takes arguments like any other.
        BitVector isEnteredFromDeadBlock(function.blocks.size());
        for (uint32_t index = 0; index < function.blocks.size(); ++index) {
            if (!cfg.isReachable(BlockID(index)))
                function.at(BlockID(index)).forEachTarget([&](const Target& target) { isEnteredFromDeadBlock.quickSet(target.block.index()); });
        }
        auto comesAsOneForm = [&](const MergedString& string) {
            const Block& block = function.at(string.block);
            if (block.role != BlockRole::Ordinary || isEnteredFromDeadBlock.quickGet(string.block.index()))
                return false;
            auto isReadByOwnHandler = [&](const Use& use) {
                if (use.block == string.block)
                    return use.kind == Use::Kind::HandlerArgument;
                return !analyses.dominators().terminatorDominates(string.block, use.block);
            };
            if (block.handler && std::ranges::any_of(uses.usesOf(string.parameter), isReadByOwnHandler))
                return false;
            for (Edge edge : cfg.incomingEdges(string.block)) {
                const Target& target = targetOf(function, edge);
                if (edge.isHandlerEdge() || string.parameterIndex >= target.arguments.size())
                    return false;
                ValueID argument = target.arguments[string.parameterIndex];
                if (const MergedString* other = find(argument)) {
                    if (!other->isValid || !haveSameForm(other->pattern, string.pattern))
                        return false;
                    continue;
                }
                if (!isMadeOnlyForEdges(argument) || !haveSameForm(argument, string.pattern))
                    return false;
                // It is made behind every merge that it reaches, or it stays where it is.
                for (const Use& use : uses.usesOf(argument)) {
                    const MergedString* reached = find(parameterAt(use));
                    if (!reached || !reached->isValid)
                        return false;
                }
            }
            return true;
        };
        // Behind the merge another concatenation takes the string in, there or behind a further merge. Otherwise nothing is won.
        auto isTakenIn = [&](const MergedString& string) {
            return std::ranges::any_of(uses.usesOf(string.parameter), [&](const Use& use) {
                if (use.kind == Use::Kind::Operand)
                    return isConcatenation(function.at(use.user));
                if (use.kind != Use::Kind::EdgeArgument)
                    return false;
                const MergedString* next = find(parameterAt(use));
                return next && next->isValid && next->isWanted;
            });
        };
        for (bool didChange = true; didChange;) {
            didChange = false;
            for (MergedString& string : merged)
                string.isWanted = false;
            for (bool didGrow = true; didGrow;) {
                didGrow = false;
                for (MergedString& string : merged) {
                    if (string.isValid && !string.isWanted && isTakenIn(string)) {
                        string.isWanted = true;
                        didGrow = true;
                    }
                }
            }
            for (MergedString& string : merged) {
                if (string.isValid && (!string.isWanted || !comesAsOneForm(string))) {
                    string.isValid = false;
                    didChange = true;
                }
            }
        }
        merged.removeAllMatching([](const MergedString& string) { return !string.isValid; });
        if (merged.isEmpty())
            return;

        Vector<ValueID, 8> perhapsUnused;
        for (MergedString& string : merged) {
            for (Edge edge : cfg.incomingEdges(string.block)) {
                ValueID argument = targetOf(function, edge).arguments[string.parameterIndex];
                string.incoming.append({ edge, argument });
                if (function.at(argument).opcode != Opcode::Parameter)
                    perhapsUnused.appendIfNotContains(argument);
            }
        }
        for (MergedString& string : merged) {
            Value pattern(function.at(string.pattern));
            builder.setOrigin(pattern.origin());
            for (ValueID part : pattern.operands())
                string.partParameters.append(isText(part) ? ValueID() : builder.addParameter(string.block, TypeTable::number));
        }
        for (const MergedString& string : merged) {
            for (auto [edge, argument] : string.incoming) {
                const MergedString* other = find(argument);
                for (uint32_t partIndex = 0; partIndex < string.partParameters.size(); ++partIndex) {
                    if (string.partParameters[partIndex])
                        builder.appendArgument(edge, other ? other->partParameters[partIndex] : function.at(argument).operand(partIndex));
                }
            }
        }

        ValueSubstitution substitution(function);
        for (const MergedString& string : merged) {
            Value concatenation(function.at(string.pattern));
            builder.setOrigin(concatenation.origin());
            Vector<ValueID, 4> parts;
            for (uint32_t partIndex = 0; partIndex < string.partParameters.size(); ++partIndex) {
                if (ValueID parameter = string.partParameters[partIndex])
                    parts.append(parameter);
                else
                    parts.append(builder.insertBefore(ProgramPoint::atHeadOf(string.block), Value(function.at(concatenation.operand(partIndex)))));
            }
            concatenation.become(Opcode::Strcat);
            concatenation.setVariadicOperands(parts.span());
            perhapsUnused.append(builder.insertBefore(ProgramPoint::atHeadOf(string.block), WTF::move(concatenation)));
            substitution.replace(string.parameter, perhapsUnused.last());
        }
        builder.applyInsertions();
        substitution.apply(function, &analyses);
        for (size_t index = 0; index < merged.size(); ++index) {
            BlockID block = merged[index].block;
            if (merged.findIf([&](const MergedString& string) { return string.block == block; }) != index)
                continue;
            BitVector parameterIndexes;
            for (const MergedString& string : merged) {
                if (string.block == block)
                    parameterIndexes.set(string.parameterIndex);
            }
            builder.removeParameters(block, parameterIndexes);
        }

        // First the concatenations that nothing is passed any more, then the texts that only they held.
        for (unsigned round = 0; round < 2; ++round) {
            const UseLists& usesNow = analyses.useLists();
            Vector<ValueID, 8> parts;
            for (ValueID id : perhapsUnused) {
                if (usesNow.useCount(id))
                    continue;
                for (ValueID part : function.at(id).operands()) {
                    if (isText(part))
                        parts.appendIfNotContains(part);
                }
                markAbsorbed(usesNow.locationOf(id));
            }
            eraseAbsorbedValues();
            perhapsUnused = WTF::move(parts);
        }
        didMoveStrings = true;
    };
    makeStringsBehindMerges();

    // `${a.id},${b.id}`: an access that fails its test reads the property by name and comes back, so each number reaches the template converted, as a parameter of the block where the ways meet.
    struct Passed {
        Edge edge;
        ValueLocation parameter;
        ValueID conversion;
    };
    Vector<Passed, 8> passed;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            // An Add of what may be two numbers would add them.
            if (function.at(id).opcode != Opcode::Strcat)
                continue;
            for (ValueID operand : function.at(id).operands()) {
                if (function.at(operand).opcode != Opcode::Parameter)
                    continue;
                const UseLists& uses = analyses.useLists();
                const CFG& cfg = analyses.cfg();
                ValueLocation location = uses.locationOf(operand);
                if (location.placement != ValuePlacement::Parameter || uses.useCount(operand) != 1 || function.at(location.block).role != BlockRole::Ordinary || !mayBecomeKey(id))
                    continue;
                size_t numberBefore = passed.size();
                for (Edge edge : cfg.incomingEdges(location.block)) {
                    if (edge.isHandlerEdge() || incomingValueOf(function, edge, location.index).kind != IncomingValue::Kind::Argument) {
                        passed.shrink(numberBefore);
                        break;
                    }
                    ValueID argument = targetOf(function, edge).arguments[location.index];
                    if (isConversionOfPrimitive(function.at(argument)) && uses.useCount(argument) == 1)
                        passed.append({ edge, location, argument });
                }
            }
        }
    }
    if (!passed.isEmpty()) {
        const UseLists& uses = analyses.useLists();
        for (const Passed& change : passed) {
            ValueID primitive = function.at(change.conversion).operand(0);
            editTarget(function, change.edge, BlockEdit::Arguments).arguments[change.parameter.index] = primitive;
            Value& parameter = function.edit(function.at(change.parameter.block).parameters[change.parameter.index], ValueEdit::Type);
            parameter.type = context.types.unionOf(parameter.type, function.at(primitive).type);
            markAbsorbed(uses.locationOf(change.conversion));
        }
        eraseAbsorbedValues();
        didMoveStrings = true;
    }

    auto findAbsorbableOperand = [&](ProgramPoint outerPoint, ValueID operand, uint32_t limit, const BitVector& absorbed, size_t numberOfOtherParts) -> std::optional<uint32_t> {
        const Block& block = function.at(outerPoint.block);
        const Value& inner = function.at(operand);
        // Nobody can tell when a primitive is converted, so what runs between the conversion and the concatenation does not matter.
        bool isConversion = isConversionOfPrimitive(inner) && mayBecomeKey(block.values[outerPoint.index]);
        if (!isConcatenation(inner) && !isConversion)
            return std::nullopt;
        bool mayRunAtAnyLaterTime = isConversion || hasBoundedLength(inner);

        const UseLists& uses = analyses.useLists();
        ValueLocation location = uses.locationOf(operand);
        if (uses.useCount(operand) != 1 || location.block != outerPoint.block || location.index >= (mayRunAtAnyLaterTime ? outerPoint.index : limit))
            return std::nullopt;
        for (uint32_t index = limit; !mayRunAtAnyLaterTime && --index > location.index;) {
            if (!absorbed.get(index) && hasObservableEffects(block.values[index]))
                return std::nullopt;
        }
        if (context.exceeds(LimitID::MaxFusedConcatenationParts, numberOfOtherParts + inner.numberOfOperands(), block.values[outerPoint.index]))
            return std::nullopt;
        return location.index;
    };
    for (size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        BitVector absorbed;
        for (uint32_t outerIndex = 0; outerIndex < values.size(); ++outerIndex) {
            const Value& outer = function.at(values[outerIndex]);
            if (!isConcatenation(outer))
                continue;
            Vector<ValueID, 16> parts;
            uint32_t limit = outerIndex;
            bool didAbsorb = false;
            for (uint32_t operandIndex = outer.numberOfOperands(); operandIndex--;) {
                ValueID operand = outer.operand(operandIndex);
                std::optional<uint32_t> found = findAbsorbableOperand({ BlockID(blockIndex), outerIndex }, operand, limit, absorbed, parts.size() + operandIndex);
                if (!found) {
                    parts.append(operand);
                    continue;
                }
                const Value& inner = function.at(operand);
                for (uint32_t partIndex = inner.numberOfOperands(); partIndex--;)
                    parts.append(inner.operand(partIndex));
                absorbed.set(*found);
                didAbsorb = true;
                if (inner.opcode != Opcode::ToString && !hasBoundedLength(inner))
                    limit = *found;
            }
            parts.reverse();
            if (didAbsorb)
                becomeConcatenationOf(values[outerIndex], parts.span());

            // A concatenation converts each of its parts by itself, so the constant without characters adds nothing to it: "" + x is x as a string.
            size_t numberOfEmptyParts = parts.removeAllMatching([&](ValueID part) {
                KnownValue known = knownValueOf(program, function, part);
                return known.kind() == KnownValue::Kind::String && known.asString() == program.strings.emptyString();
            });
            if (numberOfEmptyParts) {
                becomeConcatenationOf(values[outerIndex], parts.span());
                didDropParts = true;
            }
        }
        if (!absorbed.isEmpty())
            absorbedValues.append({ BlockID(blockIndex), WTF::move(absorbed) });
    }
    size_t numberOfAbsorbedValues = eraseAbsorbedValues();
    if (didDropParts)
        analyses.didChangeValues();

    Vector<std::pair<Edge, ValueID>, 4> copiesOnEdges;
    Vector<std::pair<Use, ValueID>, 4> copiesForReaders;
    BitVector absorbedFromEarlierBlocks;
    // Tests of the parts stand between the concatenations of one expression, each with an edge to code for the part it did not expect.
    auto canAbsorbFromEarlierBlock = [&](ProgramPoint outerPoint, ValueID operand, size_t numberOfOtherParts) {
        const Value& inner = function.at(operand);
        if (!isConcatenation(inner))
            return false;
        const UseLists& uses = analyses.useLists();
        ValueLocation location = uses.locationOf(operand);
        if (location.placement != ValuePlacement::InBlock || location.block == outerPoint.block)
            return false;
        ValueID outer = function.at(outerPoint.block).values[outerPoint.index];
        if (context.exceeds(LimitID::MaxFusedConcatenationParts, numberOfOtherParts + inner.numberOfOperands(), outer))
            return false;
        if (std::ranges::any_of(inner.operands(), [&](ValueID part) { return absorbedFromEarlierBlocks.get(part.index()); }))
            return false;

        const CFG& cfg = analyses.cfg();
        bool mayThrow = !hasBoundedLength(inner);
        Vector<Edge, 8> way;
        for (BlockID block = outerPoint.block; block != location.block; block = way.last().from) {
            std::span<const Edge> incoming = cfg.incomingEdges(block);
            if (incoming.size() != 1 || incoming[0].isHandlerEdge() || function.at(block).handler || way.size() >= context.limit(LimitID::MaxBlocksBetweenFusedConcatenations))
                return false;
            if (mayThrow && hasObservableEffectsBetween(block, 0, block == outerPoint.block ? outerPoint.index : function.at(block).values.size()))
                return false;
            way.append(incoming[0]);
        }
        if (function.at(location.block).handler || (mayThrow && hasObservableEffectsBetween(location.block, location.index + 1, function.at(location.block).values.size())))
            return false;
        constexpr TerminatorKindSet terminatorsOnTheWay { TerminatorKind::Jump, TerminatorKind::Branch, TerminatorKind::TestAndBranch };
        if (!std::ranges::all_of(way, [&](Edge edge) { return terminatorsOnTheWay.contains(function.at(edge.from).terminator.kind); }))
            return false;

        size_t numberOfCopiesOnEdges = copiesOnEdges.size();
        size_t numberOfCopiesForReaders = copiesForReaders.size();
        bool isUsedByOuter = false;
        bool canCopyForOthers = std::ranges::all_of(uses.usesOf(operand), [&](const Use& use) {
            if (use.kind == Use::Kind::Operand && absorbedFromEarlierBlocks.get(use.user.index()))
                return false;
            if (use.kind == Use::Kind::Operand && use.user == outer)
                return !std::exchange(isUsedByOuter, true);
            bool isOnTheWay = use.block == outerPoint.block || way.containsIf([&](Edge edge) { return edge.from == use.block; });
            if (!isOnTheWay) {
                const Block& block = function.at(use.block);
                if (mayThrow || block.handler || block.role != BlockRole::Ordinary || use.kind == Use::Kind::HandlerArgument || use.kind == Use::Kind::BlockSubject)
                    return false;
                copiesForReaders.append({ use, operand });
                return true;
            }
            if (use.kind != Use::Kind::EdgeArgument || use.block == outerPoint.block || way.contains(use.edge()))
                return false;
            copiesOnEdges.append({ use.edge(), operand });
            return true;
        });
        if (canCopyForOthers && isUsedByOuter)
            return true;
        copiesOnEdges.shrink(numberOfCopiesOnEdges);
        copiesForReaders.shrink(numberOfCopiesForReaders);
        return false;
    };
    // An inner concatenation has absorbed what it can before another absorbs it.
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& outer = function.at(values[index]);
            if (!isConcatenation(outer))
                continue;
            Vector<ValueID, 16> parts;
            bool didAbsorb = false;
            uint32_t numberOfOperands = outer.numberOfOperands();
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                ValueID operand = outer.operand(operandIndex);
                if (!canAbsorbFromEarlierBlock({ block, index }, operand, parts.size() + numberOfOperands - operandIndex - 1)) {
                    parts.append(operand);
                    continue;
                }
                parts.append(function.at(operand).operands());
                markAbsorbed(analyses.useLists().locationOf(operand));
                absorbedFromEarlierBlocks.set(operand.index());
                didAbsorb = true;
            }
            if (didAbsorb)
                becomeConcatenationOf(values[index], parts.span());
        }
    }

    if (!copiesForReaders.isEmpty()) {
        // A reader that has absorbed a concatenation since holds the original at another place.
        Vector<std::pair<Use, ValueID>, 4> readers;
        for (auto [reader, original] : copiesForReaders) {
            if (reader.kind != Use::Kind::Operand) {
                readers.append({ reader, original });
                continue;
            }
            std::span<const ValueID> operands = function.at(reader.user).operands();
            for (reader.operandIndex = 0; reader.operandIndex < operands.size(); ++reader.operandIndex) {
                if (operands[reader.operandIndex] == original)
                    readers.append({ reader, original });
            }
        }
        BitVector isDone;
        for (size_t first = 0; first < readers.size(); ++first) {
            if (isDone.get(first))
                continue;
            auto [reader, original] = readers[first];
            ValueID copy = builder.insertBefore(ProgramPoint::atHeadOf(reader.block), Value(function.at(original)));
            for (size_t index = first; index < readers.size(); ++index) {
                if (isDone.get(index) || readers[index].second != original || readers[index].first.block != reader.block)
                    continue;
                isDone.set(index);
                replaceUse(function, readers[index].first, copy);
            }
        }
        builder.applyInsertions();
    }

    Vector<Value, 4> originals = copiesOnEdges.map<Vector<Value, 4>>([&](const auto& copy) { return Value(function.at(copy.second)); });
    BitVector isDone;
    for (size_t first = 0; first < copiesOnEdges.size(); ++first) {
        if (isDone.get(first))
            continue;
        Edge edge = copiesOnEdges[first].first;
        BlockID added = builder.splitEdge(edge);
        for (size_t index = first; index < copiesOnEdges.size(); ++index) {
            ValueID original = copiesOnEdges[index].second;
            if (isDone.get(index) || copiesOnEdges[index].first != edge)
                continue;
            isDone.set(index);
            if (!function.at(added).terminator.targets[0].arguments.contains(original))
                continue;
            Arguments& arguments = function.edit(added, BlockEdit::Arguments).terminator.targets[0].arguments;
            std::ranges::replace(arguments, original, builder.insertBefore(ProgramPoint::atHeadOf(added), Value(originals[index])));
        }
        builder.applyInsertions();
    }

    // A copy for a reader stands at the head of the reader's block, and moves all that lies in that block.
    for (auto& [block, indexes] : absorbedValues) {
        indexes.clearAll();
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (absorbedFromEarlierBlocks.get(values[index].index()))
                indexes.set(index);
        }
    }
    numberOfAbsorbedValues += eraseAbsorbedValues();
    if (numberOfAbsorbedValues)
        context.build.statistics().count(StatisticID::ConcatenationsFused, numberOfAbsorbedValues);
    else if (!didMoveStrings && !didDropParts)
        return PassOutcome::Unchanged;
    // The analyses were told of every edit, so whatever they hold was made behind the last one.
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

static_assert(numberOfEnumerators<RegExpFlags> == Yarr::numberOfFlags);

static const RegExpSummary& regExpSummaryOf(FunctionPassContext& context, StringID pattern, OptionSet<RegExpFlags> flags)
{
    Vector<char16_t> buffer;
    return context.build.regExpSummaries().summaryOf(pattern, toYarrFlags(flags), context.program.strings.viewOf(pattern, buffer));
}

PassOutcome foldConstantRegExps(FunctionPassContext& context)
{
    Rewrite rewrite(context);
    const Program& program = context.program;
    const Function& function = context.function;

    auto known = [&](ValueID id) { return knownValueOf(program, function, id); };
    auto mayRunJavaScript = [&](const Value& call) {
        EffectDescription description = describeEffects(program, function, call);
        return description.callsUnknownCode || !description.functions.isEmpty();
    };
    auto replace = [&](ValueID id, Value&& replacement) {
        replacement.type = resultTypeOf(program, function, replacement, context.types);
        if (!context.types.at(replacement.type).isSubsetOf(rewrite.typeOf(id), program))
            return;
        replacement.setOrigin(function.at(id).origin());
        rewrite.edit(id) = WTF::move(replacement);
        rewrite.retype(id);
    };
    auto parseFlags = [&](KnownValue flags) -> std::optional<OptionSet<RegExpFlags>> {
        if (flags.kind() != KnownValue::Kind::String)
            return std::nullopt;
        Vector<char16_t> buffer;
        std::optional<OptionSet<Yarr::Flags>> parsedFlags = Yarr::parseFlags(program.strings.viewOf(flags.asString(), buffer));
        if (!parsedFlags)
            return std::nullopt;
        OptionSet<RegExpFlags> result;
        for (RegExpFlags flag : allMembersOf<RegExpFlags>) {
            if (parsedFlags->contains(toYarrFlag(flag)))
                result.add(flag);
        }
        return result;
    };
    auto patternTextOf = [&](ValueID pattern) {
        if (!pattern || known(pattern).kind() == KnownValue::Kind::Undefined)
            return program.strings.emptyString();
        KnownValue text = evaluateStringOperation(program, context.strings, function, IRBuilder::make(Opcode::ToString, { pattern }), known);
        return text.kind() == KnownValue::Kind::String ? text.asString() : StringID();
    };
    auto createdLiteralOf = [&](ValueID pattern, KnownValue flags) -> std::optional<RegExpLiteralInType> {
        bool passesFlags = flags.kind() != KnownValue::Kind::Undefined;
        std::optional<OptionSet<RegExpFlags>> passedFlags = passesFlags ? parseFlags(flags) : OptionSet<RegExpFlags> { };
        if (!passedFlags)
            return std::nullopt;
        if (StringID text = patternTextOf(pattern))
            return RegExpLiteralInType { text, passedFlags->toRaw() };
        std::optional<RegExpLiteralInType> copied = singleRegExpLiteralAsCreatedOf(rewrite.typeOf(pattern));
        if (!copied || !passesFlags)
            return std::nullopt;
        return RegExpLiteralInType { copied->pattern, passedFlags->toRaw() };
    };
    auto foldCreation = [&](ValueID id) {
        const Value& call = function.at(id);
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (arguments.size() > 2)
            return;
        KnownValue flags = arguments.size() == 2 ? known(arguments[1]) : KnownValue::undefined();
        std::optional<RegExpLiteralInType> literal = createdLiteralOf(arguments.empty() ? ValueID() : arguments[0], flags);
        if (!literal || mayRunJavaScript(call) || !regExpSummaryOf(context, literal->pattern, literal->flags()).canBeCreated())
            return;
        Value creation(Opcode::NewRegExp);
        creation.setPattern(literal->pattern);
        creation.setRegExpFlags(literal->flags());
        creation.setAllocationSite(call.allocationSite());
        replace(id, WTF::move(creation));
    };
    auto foldGetter = [&](ValueID id, BuiltinID getter, const BuiltinImplementation& implementation) {
        const Value& call = function.at(id);
        ValueID receiver = call.receiverOperand();
        std::optional<RegExpLiteralInType> literal = receiver ? singleRegExpLiteralAsCreatedOf(rewrite.typeOf(receiver)) : std::nullopt;
        if (!literal || mayRunJavaScript(call))
            return;
        if (implementation.inlineBuiltin == InlineBuiltin::RegExpFlag) {
            Value constant(Opcode::Boolean);
            constant.setBoolean(literal->flags().contains(static_cast<RegExpFlags>(implementation.inlineParameters[0])));
            replace(id, WTF::move(constant));
            return;
        }
        StringID string;
        if (getter == Builtins::RegExpPrototypeFlags) {
            Yarr::FlagsString flags = Yarr::flagsString(toYarrFlags(literal->flags()));
            string = context.strings.intern(StringView { flags.span() });
        } else if (getter == Builtins::RegExpPrototypeSource) {
            const RegExpSummary& summary = regExpSummaryOf(context, literal->pattern, literal->flags());
            if (summary.isValid())
                string = summary.sourceIsPattern ? literal->pattern : context.strings.intern(StringView(summary.source));
        }
        if (!string)
            return;
        Value constant(Opcode::String);
        constant.setString(string);
        replace(id, WTF::move(constant));
    };

    // A fold is made at once: the getter behind a creation reads the literal out of the type of the new value.
    const BuiltinTable& table = program.builtins();
    for (BlockID blockID : rewrite.blocks()) {
        const Block& block = function.at(blockID);
        ValueID tailCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
        for (ValueID id : block.values) {
            const Value& call = function.at(id);
            if (id == tailCall || call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
                continue;
            const BuiltinOverload& overload = table.at(call.overload());
            BuiltinID builtin = table.canonicalID(overload.builtin);
            switch (overload.mode) {
            case BuiltinCallMode::Call:
            case BuiltinCallMode::Construct:
                if (builtin == Builtins::RegExp)
                    foldCreation(id);
                continue;
            case BuiltinCallMode::Get:
                foldGetter(id, builtin, overload.code.implementation);
                continue;
            case BuiltinCallMode::Set:
                continue;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    return rewrite.commit();
}

static constexpr OperandUses contentsReadOrWritten { OperandUse::ReadsContents, OperandUse::WritesContents };

static constexpr AnalysisSet analysesUnaffectedBySharing {
    FunctionAnalysis::UseLists,
    FunctionAnalysis::Identities,
    FunctionAnalysis::Liveness,
    FunctionAnalysis::Frequencies,
    FunctionAnalysis::FailedTests,
};

// While every use of the object is one that this pass accepts, lastIndex holds a number, and converting it runs no user code.
static constexpr OperandUses usesOfMatching {
    OperandUse::Inspects,
    OperandUse::ReadsContents,
    OperandUse::WritesContents,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
    OperandUse::PassesToUserCode,
};

PassOutcome shareRegExpLiterals(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    const BuildContext& build = context.build;
    Vector<ValueID, 4> literals;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (function.at(id).opcode == Opcode::NewRegExp)
                literals.append(id);
        }
    }
    if (literals.isEmpty())
        return PassOutcome::Unchanged;

    const EscapeAnalysis& escape = context.analyses.escape();
    const ValueIdentities& identities = context.analyses.identities();
    auto isOnlyInspected = [](const ObjectUse& objectUse) { return !objectUse.isEscape && objectUse.uses.containsOnly({ OperandUse::Inspects }); };
    // The copies of a site that inlining and cloning make share the object of the site. A built-in that matches takes one of them where they meet in one operand.
    auto takesObjectOfAnotherCopy = [&](const Value& user, const ObjectUse& objectUse, ValueID id) {
        const Value& literal = function.at(id);
        ValueID operandThatMatches = effectRuleOf(user.opcode) == EffectRule::Builtin && objectUse.uses.containsAny(contentsReadOrWritten) ? user.operand(objectUse.use.operandIndex) : ValueID();
        bool isNamedOnce = operandThatMatches && std::ranges::count(user.operands(), operandThatMatches) == 1;
        bool result = false;
        user.forEachChild([&](ValueID operand) {
            if (isNamedOnce && operand == operandThatMatches)
                return;
            for (ObjectID object : escape.pointsTo(operand).objects) {
                const Value& other = function.at(escape.at(object).definition);
                if (escape.at(object).definition != id && other.opcode == Opcode::NewRegExp && other.position == literal.position && other.pattern() == literal.pattern() && other.regExpFlags() == literal.regExpFlags())
                    result = true;
            }
        });
        return result;
    };
    auto isHarmlessInBuiltin = [&](const Value& call, const ObjectUse& objectUse, OptionSet<RegExpFlags> flags, bool isOfOneEvaluation) {
        if (!objectUse.uses.containsAny(contentsReadOrWritten))
            return isOfOneEvaluation && isOnlyInspected(objectUse);
        std::optional<InlineBuiltin> builtin;
        if (objectUse.uses.containsOnly(usesOfMatching) && declaredBehaviorOf(program.builtins().at(call.overload()), builtinOperandAt(call, objectUse.use.operandIndex)).protocols.containsOnly({ Protocol::ToPrimitive }))
            builtin = inlineImplementationOf(program, call);
        if (!builtin)
            return build.isSabotaged(Sabotage::SharedPatternsAreUsedInUnknownWays);

        // A match stores lastIndex only under g or y. With g, these String methods store 0 first and stop at the failed match that stores 0 again.
        switch (*builtin) {
        case InlineBuiltin::RegExpExec:
        case InlineBuiltin::RegExpExecPrefix:
        case InlineBuiltin::RegExpTest:
            return !flags.containsAny({ RegExpFlags::Global, RegExpFlags::Sticky }) || build.isSabotaged(Sabotage::SharedPatternsAdvance);
        case InlineBuiltin::StringMatch:
        case InlineBuiltin::StringMatchPrefix:
        case InlineBuiltin::StringReplace:
        case InlineBuiltin::StringReplaceAll:
            return !flags.contains(RegExpFlags::Sticky) || build.isSabotaged(Sabotage::SharedPatternsAreSticky);
        default:
            return build.isSabotaged(Sabotage::SharedPatternsAreUsedInUnknownWays);
        }
    };
    // A value that stands for the objects of several evaluations may be handed on and matched with. Whoever inspects it could tell them apart.
    auto isHarmless = [&](const ObjectUse& objectUse, ValueID literal, bool isOfOneEvaluation) {
        if (objectUse.use.kind != Use::Kind::Operand)
            return isOnlyInspected(objectUse) && (isOfOneEvaluation || objectUse.use.isEdgeUse());
        const Value& user = function.at(objectUse.use.user);
        if (takesObjectOfAnotherCopy(user, objectUse, literal))
            return false;
        switch (effectRuleOf(user.opcode)) {
        case EffectRule::Declared:
        case EffectRule::Operator:
        case EffectRule::Access:
            return isOnlyInspected(objectUse) && (isOfOneEvaluation || isCastWithoutTest(user.opcode));
        case EffectRule::Call:
            // A callee may evaluate the site of the literal again, and would find the two objects identical.
            return build.isSabotaged(Sabotage::SharedPatternsReachCalls);
        case EffectRule::Builtin:
            return isHarmlessInBuiltin(user, objectUse, function.at(literal).regExpFlags(), isOfOneEvaluation);
        case EffectRule::Inline:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto canShare = [&](ValueID literal) {
        std::optional<ObjectID> object = escape.objectDefinedBy(literal);
        if (!object || escape.at(*object).reasons.contains(EscapeReason::LimitReached))
            return false;
        bool isOfOneEvaluation = std::ranges::all_of(escape.aliasesOf(*object), [&](ValueID alias) { return identities.identityOf(alias) == literal; });
        if (!std::ranges::all_of(escape.usesOf(*object), [&](const ObjectUse& objectUse) { return isHarmless(objectUse, literal, isOfOneEvaluation); }))
            return false;
        // The operation that creates a shared literal cannot throw, and a pattern may fail to compile unless the build has its metadata.
        const RegExpSummary& summary = regExpSummaryOf(context, function.at(literal).pattern(), function.at(literal).regExpFlags());
        return summary.isValid() && summary.metadata == RegExpMetadata::Known;
    };
    literals.removeAllMatching([&](ValueID literal) { return !canShare(literal); });
    if (literals.isEmpty())
        return PassOutcome::Unchanged;

    for (ValueID literal : literals) {
        Value& value = function.edit(literal);
        StringID pattern = value.pattern();
        OptionSet<RegExpFlags> flags = value.regExpFlags();
        value.become(Opcode::SharedRegExpLiteral);
        value.setPattern(pattern);
        value.setRegExpFlags(flags);
    }
    context.analyses.invalidate(valueAnalyses - analysesUnaffectedBySharing);
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

static_assert(defaultLimitValue(LimitID::MaxLiteralStrings, Architecture::ARM64) == maxLiteralStrings);
static_assert(defaultLimitValue(LimitID::MaxLiteralStrings, Architecture::X86_64) == maxLiteralStrings);
static_assert(literalStringWord(0) == emptyLiteralStringWord);

// Their value is one that another value, a field or a variable of the program held before.
static constexpr OpcodeSet operationsHandingOnValues = castsThatProveNothing | OpcodeSet { Opcode::Parameter, Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::CallDirect };

std::optional<uint32_t> LiteralStringTable::indexOf(StringID string) const
{
    auto found = m_indexes.find(string);
    return found == m_indexes.end() ? std::nullopt : std::optional<uint32_t>(found->value);
}

uint32_t LiteralStringTable::numberOf(StringID string) const
{
    std::optional<uint32_t> index = indexOf(string);
    RELEASE_ASSERT(index);
    return *index;
}

void LiteralStringTable::dump(PrintStream& out, const Program& program) const
{
    for (size_t index = 0; index < m_strings.size(); ++index)
        out.print(index, " \"", byteCast<char8_t>(program.string(m_strings[index])), "\"", isWithoutText(m_strings[index]) ? " without text" : "", "\n");
}

LiteralStringTable numberLiteralStrings(const Program& program, const FlowResults& flow, BuildContext& build)
{
    enum class Source : uint8_t { Program, Anywhere, Parser };
    // A validating build and the report of a mismatch hold every value against its type, also a text that the engine made and no code reads: Object.keys(). They read the texts of the members.
    bool everyWordHasText = build.validates() || build.recordedOptions().policy() == MismatchPolicy::Report;
    size_t numberOfStrings = program.strings.size();
    FastBitVector strings(numberOfStrings);
    // Each gets a word, and its text only if the program spells it.
    FastBitVector membersFromOutside(numberOfStrings);
    // Each gets a word only if the program spells it.
    FastBitVector membersHandedOn(numberOfStrings);
    FastBitVector constantsOfCode(numberOfStrings);
    FastBitVector variablesNamedByCode(program.variables.size());

    constexpr unsigned typesInShare = 4096;
    static_assert(!(typesInShare % 32));
    unsigned numberOfTypes = program.types.size();
    FastBitVector typesToVisit(numberOfTypes);
    forEachIndexInParallel(build.parallelLoops(), (numberOfTypes + typesInShare - 1) / typesInShare, [&](unsigned share) {
        for (unsigned index = share * typesInShare; index < std::min(numberOfTypes, (share + 1) * typesInShare); ++index)
            typesToVisit[index] = program.at(TypeID(index)).hasLiteralStrings();
    });
    FastBitVector typesFromOutsideToVisit = typesToVisit;
    FastBitVector typesHandedOnToVisit = typesToVisit;

    auto add = [&](StringID string) { strings.atomicSetAndCheck(string.index(), true); };
    // A string from outside the program gets its word where it comes in, so every member has one there. Where the program hands its own values on, a member that it spells nowhere does not occur.
    // A parse by type compares what it reads with the texts of the members.
    auto collectLiteralsOf = [&](TypeID id, Source source) {
        bool needsTexts = source == Source::Parser || everyWordHasText;
        FastBitVector& toVisit = needsTexts ? typesToVisit : source == Source::Anywhere ? typesFromOutsideToVisit : typesHandedOnToVisit;
        if (!toVisit.at(id.index()) || !toVisit.atomicSetAndCheck(id.index(), false))
            return;
        FastBitVector& members = needsTexts ? strings : source == Source::Anywhere ? membersFromOutside : membersHandedOn;
        for (StringID string : program.at(id).stringLiterals)
            members.atomicSetAndCheck(string.index(), true);
    };
    auto collectLiteralsOfAll = [&](std::span<const DeclaredType> positions, Source source) {
        for (const DeclaredType& declared : positions)
            collectLiteralsOf(declared.written(), source);
    };
    auto collectStringsOfFunction = [&](FunctionID id, Source sourceOfArguments) {
        const Function& function = program.at(id);
        collectLiteralsOfAll(function.captures.span(), Source::Program);
        collectLiteralsOf(function.thisType.written(), sourceOfArguments);
        collectLiteralsOfAll(function.parameters.span(), sourceOfArguments);
        collectLiteralsOfAll(function.results.span(), Source::Program);

        auto collectFromValue = [&](const Value& value) {
            if (value.opcode != Opcode::String)
                collectLiteralsOf(value.type, operationsHandingOnValues.contains(value.opcode) ? Source::Program : Source::Anywhere);
            else
                constantsOfCode.atomicSetAndCheck(value.string().index(), true);
            forEachType(value, [&](TypeID type) {
                collectLiteralsOf(type, Source::Anywhere);
            });
            for (unsigned side = 0; equalityComparisonsOfStrings.contains(value.opcode) && side < 2; ++side) {
                if (!program.typeOf(function, definitionThroughUpcasts(function, value.operand(1 - side))).hasLiteralStrings())
                    continue;
                std::optional<ValueID> constant = constantThroughCasts(program, function, value.operand(side));
                if (constant && function.at(*constant).opcode == Opcode::String)
                    add(function.at(*constant).string());
            }
            if (VariableID variable = Value::opcodesWithVariable.contains(value.opcode) ? value.variable() : VariableID())
                variablesNamedByCode.atomicSetAndCheck(variable.index(), true);
        };
        auto collectFromTerminator = [&](const Terminator& terminator) {
            if (terminator.kind == TerminatorKind::SwitchString && program.typeOf(function, terminator.subjectOperand()).hasLiteralStrings())
                std::ranges::for_each(terminator.strings(), add);
        };

        BodyIndex* index = program.bodyIndex();
        if (std::optional<RowOfBody> row = index ? index->rowOf(program, id) : std::nullopt; row && row->listsAllValues) {
            for (const Value& value : function.values)
                collectFromValue(value);
            if (!row->terminators.contains(TerminatorKind::SwitchString))
                return;
            for (const Block& block : function.blocks)
                collectFromTerminator(block.terminator);
            return;
        }
        for (const Block& block : function.blocks) {
            for (ValueID parameter : block.parameters)
                collectFromValue(function.at(parameter));
            for (ValueID value : block.values)
                collectFromValue(function.at(value));
            collectFromTerminator(block.terminator);
        }
    };

    BitVector isEnteredOtherwise = functionsCalledByHost(program, build.parallelLoops());
    for (const FlowIndirectCall& call : flow.indirectCalls())
        isEnteredOtherwise.set(call.callee.index());
    forEachIndexInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        FunctionID id(index);
        if (!hasBody(program, id))
            return;
        bool everyCallIsSeen = !isEnteredOtherwise.get(index) && (program.at(id).hasOnlyDirectCallers() || callerStateOf(flow, id) == CallerState::AllKnown);
        collectStringsOfFunction(id, everyCallIsSeen ? Source::Program : Source::Anywhere);
    });

    for (const Layout& layout : program.layouts) {
        bool isParsedByType = layout.flow.observedUses.contains(ObservedUse::IsParsedByType);
        bool isFilledByNativeCode = layout.flow.observedUses.contains(ObservedUse::ReachesTypedParse);
        for (const Field& field : layout.fields)
            collectLiteralsOf(field.declared.written(), isParsedByType ? Source::Parser : !isFilledByNativeCode && field.flow.location.writers == WriterState::AllKnown ? Source::Program : Source::Anywhere);
    }
    BitVector described = findVariablesDescribedToRunTime(program, flow);
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        TypeID type = program.variables[index].declared.written();
        if (described.quickGet(index) || variablesNamedByCode.at(index)) {
            collectLiteralsOf(type, !described.quickGet(index) && program.variables[index].flow.writers == WriterState::AllKnown ? Source::Program : Source::Anywhere);
            continue;
        }
        // The text of a constant that was folded into its readers is in the image with them.
        for (StringID literal : program.at(type).stringLiterals) {
            if (constantsOfCode.at(literal.index()))
                add(literal);
        }
    }
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            if (const DataNode::String* string = node.tryAs<DataNode::String>())
                constantsOfCode[string->value.index()] = true;
        });
    }

    LiteralStringTable table;
    table.m_withoutText = BitVector(numberOfStrings);
    table.m_leftOut = BitVector(numberOfStrings);
    membersFromOutside.forEachSetBit([&](size_t index) {
        if (!strings.at(index) && !constantsOfCode.at(index))
            table.m_withoutText.quickSet(index);
        strings[index] = true;
    });
    membersHandedOn.forEachSetBit([&](size_t index) {
        if (strings.at(index))
            return;
        if (constantsOfCode.at(index))
            strings[index] = true;
        else
            table.m_leftOut.quickSet(index);
    });

    StringID emptyString = program.strings.emptyString();
    table.m_withoutText.quickClear(emptyString.index());
    table.m_strings.reserveInitialCapacity(strings.bitCount() + 1);
    table.m_strings.append(emptyString);
    strings.forEachSetBit([&](size_t index) {
        if (index != emptyString.index())
            table.m_strings.append(StringID(index));
    });

    // Compiled code and the run time write emptyLiteralStringWord for an empty string without asking the table, so number 0 always exists.
    if (build.exceeds(LimitID::MaxLiteralStrings, table.m_strings.size())) {
        size_t numberOfWords = std::max<uint64_t>(build.limit(LimitID::MaxLiteralStrings), 1);
        table.m_stringsWithoutWord = Vector<StringID>(table.m_strings.subspan(numberOfWords));
        for (StringID string : table.m_stringsWithoutWord)
            table.m_withoutText.quickClear(string.index());
        table.m_strings.shrink(numberOfWords);
    }

    table.m_indexes.reserveInitialCapacity(table.m_strings.size());
    for (uint32_t number = 0; number < table.m_strings.size(); ++number)
        table.m_indexes.add(table.m_strings[number], number);
    build.statistics().count(StatisticID::LiteralStringsNumbered, table.m_strings.size());
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

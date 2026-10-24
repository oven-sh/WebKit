/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRewrite.h"

#if ENABLE(AOT)

#include "AOTDecidedBranches.h"
#include "AOTHoistTestsIntoPredecessors.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// HoistTestsIntoPredecessors.

using TypePredicate = bool (Type::*)() const;

static bool canHoistIntoPredecessors(Rewrite& rewrite, BlockID mergePoint, size_t parameterIndex, TypePredicate isCheap)
{
    if (rewrite.at(mergePoint).isHandlerBlock())
        return false;
    bool hasCheapArgument = false;
    for (Edge edge : rewrite.cfg().incomingEdges(mergePoint)) {
        if (edge.isHandlerEdge() || edge.from == mergePoint || rewrite.at(edge.from).terminator.kind != TerminatorKind::Jump)
            return false;
        hasCheapArgument |= (rewrite.typeOf(targetOf(rewrite.function, edge).arguments[parameterIndex]).*isCheap)();
    }
    return hasCheapArgument && !rewrite.analyses.isEnteredFromUnreachableBlock(mergePoint);
}

static void hoistTruthinessTest(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (id == entryBlock || block.values.size() != 1 || block.terminator.conditionOperand() != block.values[0])
        return;
    ValueID test = block.values[0];
    ValueID parameter = rewrite.at(test).opcode == Opcode::ToBoolean ? rewrite.at(test).valueOperand() : ValueID();
    size_t parameterIndex = parameter ? block.parameters.find(parameter) : notFound;
    if (parameterIndex == notFound || !canHoistIntoPredecessors(rewrite, id, parameterIndex, &Type::isBoolean))
        return;
    if (rewrite.uses().useCount(parameter) != 1 || rewrite.uses().useCount(test) != 1 || isConditionOfBranchWithProofs(rewrite.function, rewrite.uses(), test) || !rewrite.analyses.isRemovableWhenUnused(test))
        return;
    rewrite.claim(id);
    rewrite.later([&rewrite, id, test, parameter, parameterIndex, edges = Vector<Edge, 4>(rewrite.cfg().incomingEdges(id))] {
        for (Edge edge : edges) {
            ValueID argument = targetOf(rewrite.function, edge).arguments[parameterIndex];
            if (!rewrite.typeOf(argument).isBoolean())
                editTarget(rewrite.function, edge).arguments[parameterIndex] = rewrite.insertBefore(pointBeforeTerminatorOf(rewrite.function, edge.from), rewrite.at(test).origin(), IRBuilder::make(Opcode::ToBoolean, { argument }));
        }
        rewrite.edit(id, BlockEdit::Terminator).terminator.setConditionOperand(parameter);
        rewrite.erase(id, test);
        rewrite.retype(parameter);
    });
}

// The first operator of a block, behind its constants, on a parameter that is cheap to work on in some predecessor.
static void hoistOperatorOnParameter(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (id == entryBlock)
        return;
    size_t valueIndex = block.values.findIf([&](ValueID value) { return !isRematerializable(rewrite.at(value)); });
    if (valueIndex == notFound || !operatorsHoistedIntoPredecessors.contains(rewrite.at(block.values[valueIndex]).opcode))
        return;
    ValueID operation = block.values[valueIndex];
    const Value& value = rewrite.at(operation);
    size_t leftIndex = block.parameters.find(value.operand(0));
    size_t rightIndex = block.parameters.find(value.operand(1));
    if ((leftIndex == notFound) == (rightIndex == notFound))
        return;
    uint32_t operandIndex = leftIndex == notFound ? 1 : 0;
    uint32_t parameterIndex = operandIndex ? rightIndex : leftIndex;
    const Type& parameter = rewrite.typeOf(value.operand(operandIndex));
    TypePredicate isCheap = nullptr;
    if (relationalOperators.contains(value.opcode) && !parameter.isNumber())
        isCheap = &Type::isNumber;
    else if (operatorsOnInt32.contains(value.opcode) && parameter.isNumber() && !parameter.isInteger())
        isCheap = &Type::isInteger;
    if (!isCheap || !canHoistIntoPredecessors(rewrite, id, parameterIndex, isCheap))
        return;
    std::span<const Edge> incomingEdges = rewrite.cfg().incomingEdges(id);
    if (!std::ranges::all_of(incomingEdges, [&](Edge edge) { return rewrite.at(edge.from).handler == block.handler; }))
        return;
    if (rewrite.uses().useCount(value.operand(operandIndex)) != 1 || isConditionOfBranchWithProofs(rewrite.function, rewrite.uses(), operation))
        return;
    bool otherOperandIsConstantOfBlock = std::ranges::contains(block.values.span().first(valueIndex), value.operand(1 - operandIndex));
    rewrite.later([&rewrite, id, operation, operandIndex, parameterIndex, otherOperandIsConstantOfBlock, edges = Vector<Edge, 4>(incomingEdges)] {
        Opcode opcode = rewrite.at(operation).opcode;
        Origin origin = rewrite.at(operation).origin();
        std::array<ValueID, 2> operands { rewrite.at(operation).operand(0), rewrite.at(operation).operand(1) };
        ValueID otherOperand = operands[1 - operandIndex];
        rewrite.builder().setOrigin(origin);
        ValueID result = rewrite.builder().addParameter(id, rewrite.at(operation).type);
        for (Edge edge : edges) {
            ProgramPoint point = pointBeforeTerminatorOf(rewrite.function, edge.from);
            operands[operandIndex] = targetOf(rewrite.function, edge).arguments[parameterIndex];
            if (otherOperandIsConstantOfBlock)
                operands[1 - operandIndex] = rewrite.insertBefore(point, origin, Value(rewrite.at(otherOperand)));
            rewrite.builder().appendArgument(edge, rewrite.insertBefore(point, origin, IRBuilder::make(opcode, { operands[0], operands[1] })));
        }
        rewrite.replaceUsesOf(operation, result);
        rewrite.erase(id, operation);
        rewrite.removeParameter(id, parameterIndex);
    });
}

static constexpr Rule hoistedTestRules[] {
    { TerminatorKindSet { TerminatorKind::Branch }, hoistTruthinessTest, BlocksShown::WithParameters },
    { allTerminators, hoistOperatorOnParameter, BlocksShown::WithParameters },
};

PassOutcome hoistTestsIntoPredecessors(FunctionPassContext& context)
{
    return runRules<hoistedTestRules, Rounds::UntilUnchanged>(context);
}

// RemoveRedundantHandlers.

static void removeUnusedHandler(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (std::ranges::any_of(block.values, [&](ValueID value) { return rewrite.analyses.mayThrow(value); }) || rewrite.analyses.effectsOfTerminator(id).mayThrow())
        return;
    rewrite.later([&rewrite, id] { rewrite.builder().removeHandler(id); });
}

static bool onlyRethrows(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (!block.isHandlerBlock() || !block.parameters.isEmpty() || block.values.isEmpty())
        return false;
    ValueID caught = block.values[0];
    const Terminator& terminator = block.terminator;
    bool rethrowsCaughtValue = (rewrite.at(caught).opcode == Opcode::CatchValue && terminator.kind == TerminatorKind::Throw && terminator.valueOperand() == caught)
        || (rewrite.at(caught).opcode == Opcode::CatchException && terminator.kind == TerminatorKind::Rethrow && terminator.exceptionOperand() == caught);
    if (!rethrowsCaughtValue || !std::ranges::all_of(block.values.span().subspan(1), [&](ValueID value) { return rewrite.analyses.isRemovableWhenUnused(value); }))
        return false;
    return std::ranges::none_of(block.values, [&](ValueID value) {
        return (block.handler && block.handler->arguments.contains(value)) || std::ranges::any_of(rewrite.uses().usesOf(value), [&](const Use& use) { return use.block != id; });
    });
}

// Whoever is covered by a handler that only throws on is covered by the handler of that handler.
static void skipRethrowingHandler(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (!onlyRethrows(rewrite, id, block) || (block.handler && block.handler->block == id))
        return;
    for (Edge edge : rewrite.cfg().incomingEdges(id)) {
        if (edge.from == id)
            continue;
        rewrite.later([&rewrite, from = edge.from, outer = std::optional<Target>(block.handler)] {
            if (!rewrite.at(from).handler)
                return;
            if (outer)
                rewrite.edit(from, { BlockEdit::Edges, BlockEdit::Arguments }).handler = *outer;
            else
                rewrite.builder().removeHandler(from);
        });
    }
}

static constexpr Rule redundantHandlerRules[] {
    { allTerminators, removeUnusedHandler, BlocksShown::WithHandler },
    { TerminatorKindSet { TerminatorKind::Throw, TerminatorKind::Rethrow }, skipRethrowingHandler },
};

PassOutcome removeRedundantHandlers(FunctionPassContext& context)
{
    if (!context.analyses.cfg().hasHandlers() || runRules<redundantHandlerRules, Rounds::UntilUnchanged>(context) == PassOutcome::Unchanged)
        return PassOutcome::Unchanged;
    Rewrite(context).builder().emptyUnreachableBlocks();
    return PassOutcome::Changed;
}

// TestNarrowerTypesFirst.

static const Value* findTypeTestThatEnds(Rewrite& rewrite, const Block& block)
{
    if (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.terminator.conditionOperand() != block.values.last())
        return nullptr;
    const Value& test = rewrite.at(block.values.last());
    return test.opcode == Opcode::TypeTest && rewrite.uses().useCount(block.values.last()) == 1 ? &test : nullptr;
}

static void testNarrowerTypeFirst(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (block.frequency == BlockFrequency::Rare || block.handler)
        return;
    const Value* wider = findTypeTestThatEnds(rewrite, block);
    const Target& proven = block.terminator.targets[0];
    if (!wider || !proven.arguments.isEmpty() || proven.proofs.size() != 1 || proven.proofs[0])
        return;

    BlockID next = proven.block;
    const Block& nextBlock = rewrite.at(next);
    if (next == id || nextBlock.handler || nextBlock.isHandlerBlock() || nextBlock.values.size() != 1 || nextBlock.parameters.size() != 1 || !rewrite.cfg().singleIncomingEdge(next))
        return;
    const Value* narrower = findTypeTestThatEnds(rewrite, nextBlock);
    if (!narrower || narrower->valueOperand() != nextBlock.parameters[0] || rewrite.uses().useCount(nextBlock.parameters[0]) != 1)
        return;
    const Target& provenNarrower = nextBlock.terminator.targets[0];
    if (provenNarrower.block == next || provenNarrower.block == id)
        return;
    const Type& widerType = rewrite.context.types.at(wider->testedType());
    const Type& narrowerType = rewrite.context.types.at(narrower->testedType());
    if (wider->testedType() == narrower->testedType() || !narrowerType.isSubsetOf(widerType, rewrite.program))
        return;
    // `links[id] ??= ..`, `session().project`: the second test asks whether there is nothing. Mostly there is something, which would fail it, then pass the first test on the failing side and be asked again.
    if (narrowerType.isUndefinedOrNull() || rewrite.at(provenNarrower.block).frequency > rewrite.at(nextBlock.terminator.targets[1].block).frequency)
        return;

    rewrite.claim(id);
    rewrite.claim(next);
    // Where the first test admits nothing beside the second but undefined and null, flow expects `undefined|number` and the use needs a number: the rest is unlikely.
    BlockFrequency frequencyOfRest = widerType.withoutNullish().isSubsetOf(narrowerType, rewrite.program) ? BlockFrequency::Unlikely : block.frequency;
    rewrite.later([&rewrite, id, test = block.values.last(), narrowerType = narrower->testedType(), provenNarrower = Target(provenNarrower), frequencyOfRest]() mutable {
        IRBuilder& builder = rewrite.builder();
        Target taken = rewrite.at(id).terminator.targets[0];
        Target notTaken = rewrite.at(id).terminator.targets[1];
        builder.setOrigin(rewrite.at(test).origin());
        BlockID failed = builder.addBlock(frequencyOfRest);
        builder.moveToEndOf(failed);
        ValueID widerTest = builder.appendTypeTest(rewrite.at(test).valueOperand(), rewrite.at(test).testedType());
        builder.emitBranch(widerTest, WTF::move(taken), WTF::move(notTaken));
        rewrite.edit(test, ValueEdit::Definition).setTestedType(narrowerType);
        builder.redirectEdge(Edge { id, 0 }, WTF::move(provenNarrower));
        builder.redirectEdge(Edge { id, 1 }, Target { failed, { }, { } });
    });
}

static constexpr Rule narrowerTestRules[] { { TerminatorKindSet { TerminatorKind::Branch }, testNarrowerTypeFirst } };

PassOutcome testNarrowerTypesFirst(FunctionPassContext& context)
{
    return runRules<narrowerTestRules>(context);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

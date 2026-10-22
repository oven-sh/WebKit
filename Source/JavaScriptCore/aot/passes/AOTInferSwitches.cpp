/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTComparisonChains.h"
#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ShapeTestLink {
    ValueID test;
    BlockID block;
    LayoutSetID layouts;
};

using ShapeTestLinks = Vector<ShapeTestLink, 4>;

} // anonymous namespace

static void appendEdgeResultParameters(const Function& function, BlockID from, Vector<ValueID, 8>& parameters)
{
    const Terminator& terminator = function.at(from).terminator;
    for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
        uint32_t first = firstEdgeResultParameter(function, from, targetIndex);
        uint32_t count = numberOfEdgeResults(function, from, targetIndex);
        parameters.append(function.at(terminator.targets[targetIndex].block).parameters.subspan(first, count));
    }
}

static unsigned inferIntegerSwitches(FunctionPassContext& context, IRBuilder& builder)
{
    ChainLimits limits { context.limit(LimitID::MinSwitchCases), context.limit(LimitID::MaxSwitchCases), LimitID::MaxSwitchCases };
    Vector<ComparisonChain> chains = findComparisonChains(context.analyses, ChainKind::Integers, limits, [](ValueID) {
        return true;
    });
    if (chains.isEmpty())
        return 0;

    for (size_t index = chains.size(); index--;)
        convertChainToSwitch(builder, context.analyses, ChainKind::Integers, chains[index]);
    builder.setHandler(std::nullopt);

    Vector<ValueID, 8> narrowedSubjects;
    for (const ComparisonChain& chain : chains)
        appendEdgeResultParameters(context.function, chain.head, narrowedSubjects);
    if (!narrowedSubjects.isEmpty())
        narrowValueTypes(TypePropagationInputs { context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo }, narrowedSubjects.span());
    return static_cast<unsigned>(chains.size());
}

static std::optional<ValueID> findShapeTestAtEndOf(const Function& function, BlockID blockID)
{
    const Block& block = function.at(blockID);
    if (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty())
        return std::nullopt;

    ValueID test = block.terminator.conditionOperand();
    if (block.values.last() != test || function.at(test).opcode != Opcode::ShapeTest)
        return std::nullopt;
    return test;
}

static std::optional<ShapeTestLink> findLinkAtEndOf(FunctionPassContext& context, BlockID block, ValueID subject)
{
    const Function& function = context.function;
    std::optional<ValueID> test = findShapeTestAtEndOf(function, block);
    if (!test)
        return std::nullopt;

    const Value& value = function.at(*test);
    if (context.analyses.identityOf(value.objectOperand()) != subject || context.analyses.useLists().useCount(*test) != 1)
        return std::nullopt;

    const Terminator& branch = function.at(block).terminator;
    if (branch.taken().block == branch.notTaken().block)
        return std::nullopt;

    const LayoutSet& layouts = context.program.at(value.layouts());
    if (!layouts.isComplete || layouts.members.isEmpty())
        return std::nullopt;
    return ShapeTestLink { *test, block, value.layouts() };
}

static std::optional<ShapeTestLink> findFollower(FunctionPassContext& context, const ShapeTestLink& previous, ValueID subject)
{
    const Function& function = context.function;
    const Target& edge = function.at(previous.block).terminator.notTaken();
    const Block& block = function.at(edge.block);
    if (!edge.arguments.isEmpty() || !block.parameters.isEmpty() || block.values.size() != 1 || block.isHandlerBlock())
        return std::nullopt;
    if (!context.analyses.cfg().singleIncomingEdge(edge.block))
        return std::nullopt;
    return findLinkAtEndOf(context, edge.block, subject);
}

static ShapeTestLinks findRun(FunctionPassContext& context, const ShapeTestLink& head, ValueID subject)
{
    const Program& program = context.program;
    ShapeTestLinks links { head };
    while (std::optional<ShapeTestLink> follower = findFollower(context, links.last(), subject)) {
        bool overlapsRun = std::ranges::any_of(links, [&](const ShapeTestLink& link) {
            return overlaps(program.at(link.layouts), program.at(follower->layouts));
        });
        if (overlapsRun)
            break;
        links.append(*follower);
    }
    return links;
}

static void rewriteRun(IRBuilder& builder, const ShapeTestLinks& links)
{
    Function& function = builder.function();
    BlockID head = links.first().block;
    Origin origin = function.at(head).terminator.origin();
    ValueID subject = function.at(links.first().test).objectOperand();

    Vector<std::pair<LayoutSetID, Target>> cases;
    cases.reserveInitialCapacity(links.size());
    for (const ShapeTestLink& link : links)
        cases.append({ link.layouts, function.at(link.block).terminator.taken() });
    Target otherwise = function.at(links.last().block).terminator.notTaken();

    for (const ShapeTestLink& follower : links.span().subspan(1)) {
        Block& block = function.edit(follower.block);
        block.values.clear();
        block.terminator = Terminator { };
        block.handler = std::nullopt;
    }

    BitVector testIndex;
    testIndex.set(function.at(head).values.size() - 1);
    builder.eraseValues(head, testIndex);
    function.edit(head, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator = Terminator { };
    builder.setOrigin(origin);
    builder.moveToEndOf(head);
    builder.emitSwitchOnShape(subject, WTF::move(cases), WTF::move(otherwise));
}

static unsigned inferShapeSwitches(FunctionPassContext& context, IRBuilder& builder)
{
    const Function& function = context.function;
    BitVector isFollower(function.blocks.size());
    Vector<ShapeTestLinks> runs;
    for (BlockID block : context.analyses.cfg().reversePostOrder()) {
        if (isFollower.quickGet(block.index()))
            continue;
        std::optional<ValueID> test = findShapeTestAtEndOf(function, block);
        if (!test)
            continue;
        ValueID object = function.at(*test).objectOperand();
        if (!context.program.typeOf(function, object).isObject())
            continue;

        ValueID subject = context.analyses.identityOf(object);
        std::optional<ShapeTestLink> head = findLinkAtEndOf(context, block, subject);
        if (!head)
            continue;
        ShapeTestLinks links = findRun(context, *head, subject);
        if (links.size() < 2)
            continue;

        for (const ShapeTestLink& follower : links.span().subspan(1))
            isFollower.quickSet(follower.block.index());
        runs.append(WTF::move(links));
    }

    for (const ShapeTestLinks& links : runs)
        rewriteRun(builder, links);
    // The builder reports the switch, not the blocks that have been emptied into it.
    if (!runs.isEmpty())
        context.analyses.didChangeControlFlow();
    return static_cast<unsigned>(runs.size());
}

PassOutcome inferSwitches(FunctionPassContext& context)
{
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    unsigned numberOfSwitches = inferIntegerSwitches(context, builder);
    numberOfSwitches += inferShapeSwitches(context, builder);
    if (!numberOfSwitches)
        return PassOutcome::Unchanged;
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

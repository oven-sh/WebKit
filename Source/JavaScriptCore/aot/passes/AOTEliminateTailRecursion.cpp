/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTSummarizeForInlining.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TailRecursionSite {
    BlockID block;
    ValueID call;
};

} // anonymous namespace

static bool isTailRecursion(const FunctionPassContext& context, const Block& block)
{
    if (block.terminator.kind != TerminatorKind::ReturnCall || block.handler)
        return false;

    const Value& call = context.function.at(block.terminator.callOperand());
    return call.opcode == Opcode::CallDirect
        && call.function() == context.id
        && !call.inlineFrame
        && call.numberOfOperands() == context.function.numberOfEntryParameters();
}

static Vector<TailRecursionSite, 2> findTailRecursionSites(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<TailRecursionSite, 2> sites;
    for (BlockID id : context.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        if (isTailRecursion(context, block))
            sites.append(TailRecursionSite { id, block.terminator.callOperand() });
    }
    return sites;
}

static bool hasOperationBoundToFrame(FunctionPassContext& context)
{
    const Function& function = context.function;
    return std::ranges::any_of(context.analyses.cfg().reversePostOrder(), [&](BlockID id) {
        const Block& block = function.at(id);
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            if (isBoundToFrame(function, block, index))
                return true;
        }
        return false;
    });
}

static BlockID createLoopHeader(FunctionPassContext& context, IRBuilder& builder)
{
    Function& function = context.function;
    builder.setOrigin({ function.position, 0 });
    BlockID header = builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));

    // The entry block is empty now, and the arguments of its handler are about to name parameters of the header.
    if (function.at(entryBlock).handler)
        builder.removeHandler(entryBlock);

    const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
    ValueSubstitution substitution(function);
    for (ValueID entryParameter : entryParameters) {
        ValueID headerParameter = builder.addParameter(header, function.at(entryParameter).type);
        function.edit(headerParameter, ValueEdit::Type).setExpected(function.at(entryParameter).expected());
        substitution.replace(entryParameter, headerParameter);
    }
    substitution.apply(function, &context.analyses);
    for (ValueID entryParameter : entryParameters)
        builder.appendArgument({ entryBlock, 0 }, entryParameter);

    builder.insertBefore(ProgramPoint::atHeadOf(header), IRBuilder::make(Opcode::LoopHint));
    builder.applyInsertions();
    return header;
}

static void replaceCallWithJump(IRBuilder& builder, TailRecursionSite site, BlockID header)
{
    Function& function = builder.function();
    const Value& call = function.at(site.call);
    Arguments arguments(call.variadicOperands());

    Block& block = function.edit(site.block);
    size_t indexOfCall = block.values.reverseFind(site.call);
    RELEASE_ASSERT(indexOfCall != notFound);
    block.terminator = Terminator { };
    BitVector erasedIndexes;
    erasedIndexes.set(indexOfCall);
    builder.eraseValues(site.block, erasedIndexes);
    builder.moveToEndOf(site.block);
    builder.setOrigin(call.origin());
    builder.emitJump(header, WTF::move(arguments));
}

PassOutcome eliminateTailRecursion(FunctionPassContext& context)
{
    Function& function = context.function;
    if (!function.hasOrdinaryFrame())
        return PassOutcome::Unchanged;

    Vector<TailRecursionSite, 2> sites = findTailRecursionSites(context);
    if (sites.isEmpty() || hasOperationBoundToFrame(context))
        return PassOutcome::Unchanged;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    BlockID header = createLoopHeader(context, builder);
    for (TailRecursionSite site : sites) {
        if (site.block == entryBlock)
            site.block = header;
        replaceCallWithJump(builder, site, header);
    }

    context.build.statistics().count(StatisticID::TailRecursionsEliminated, sites.size());
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

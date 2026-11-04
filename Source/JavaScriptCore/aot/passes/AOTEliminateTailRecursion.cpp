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
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

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

    Value hint = IRBuilder::make(Opcode::LoopHint);
    hint.setReplacesTailCalls(true);
    builder.insertBefore(ProgramPoint::atHeadOf(header), WTF::move(hint));
    builder.applyInsertions();
    return header;
}

PassOutcome eliminateTailRecursion(FunctionPassContext& context)
{
    Function& function = context.function;
    if (!function.hasOrdinaryFrame())
        return PassOutcome::Unchanged;

    std::span<const BlockID> blocks = context.analyses.cfg().reversePostOrder();
    Vector<BlockID, 2> sites;
    for (BlockID id : blocks) {
        const Block& block = function.at(id);
        if (block.terminator.kind != TerminatorKind::ReturnCall || block.handler)
            continue;
        const Value& call = function.at(block.terminator.callOperand());
        if (call.opcode == Opcode::CallDirect && call.function() == context.id && call.numberOfOperands() == function.numberOfEntryParameters())
            sites.append(id);
    }
    auto hasOperationBoundToFrame = [&](BlockID id) {
        const Block& block = function.at(id);
        for (uint32_t index = 0; index < block.values.size(); ++index) {
            if (isBoundToFrame(function, block, index))
                return true;
        }
        return false;
    };
    if (sites.isEmpty() || std::ranges::any_of(blocks, hasOperationBoundToFrame))
        return PassOutcome::Unchanged;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    BlockID header = createLoopHeader(context, builder);
    for (BlockID site : sites) {
        if (site == entryBlock)
            site = header;
        ValueID callID = function.at(site).terminator.callOperand();
        const Value& call = function.at(callID);
        Arguments arguments(call.variadicOperands());
        if (context.build.isSabotaged(Sabotage::TailCallsSwapTheirLastArguments) && arguments.size() > 1 && function.at(arguments[arguments.size() - 1]).type == function.at(arguments[arguments.size() - 2]).type)
            std::swap(arguments[arguments.size() - 1], arguments[arguments.size() - 2]);

        Block& block = function.edit(site);
        size_t indexOfCall = block.values.reverseFind(callID);
        RELEASE_ASSERT(indexOfCall != notFound);
        block.terminator = Terminator { };
        BitVector erasedIndexes;
        erasedIndexes.set(indexOfCall);
        builder.eraseValues(site, erasedIndexes);
        builder.moveToEndOf(site);
        builder.setOrigin(call.origin());
        builder.emitJump(header, WTF::move(arguments));
    }

    context.build.statistics().count(StatisticID::TailRecursionsEliminated, sites.size());
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

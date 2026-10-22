/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTValueSubstitution.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

// The cleanup block's own handler, as a covered block has to name it: where the cleanup block passes on one of its parameters, the covered block passes what it passed for that parameter.
static std::optional<Target> findHandlerAround(const Function& function, const Target& toCleanupBlock)
{
    const Block& cleanupBlock = function.at(toCleanupBlock.block);
    RELEASE_ASSERT(toCleanupBlock.arguments.size() == cleanupBlock.parameters.size());

    std::optional<Target> outerHandler = cleanupBlock.handler;
    if (!outerHandler)
        return std::nullopt;
    for (ValueID& argument : outerHandler->arguments) {
        size_t parameterIndex = cleanupBlock.parameters.find(argument);
        if (parameterIndex != notFound)
            argument = toCleanupBlock.arguments[parameterIndex];
    }
    return outerHandler;
}

// Nothing but the cleanup block leads to the handler around it, so what stands there may read a parameter of the cleanup block by its name.
// A covered block that goes past a dropped cleanup block brings no such value. The handler takes it as a parameter of its own, which findHandlerAround fills.
void passParametersOfCleanupBlocksOn(FunctionPassContext& pass, IRBuilder& builder)
{
    Function& function = pass.function;
    bool didChange = false;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockID cleanupBlock(index);
        if (function.at(cleanupBlock).role != BlockRole::IterationCleanup || !function.at(cleanupBlock).handler)
            continue;
        BlockID handlerAround = function.at(cleanupBlock).handler->block;
        if (!pass.analyses.cfg().isReachable(cleanupBlock) || !pass.analyses.cfg().singleIncomingEdge(handlerAround))
            continue;

        for (uint32_t parameterIndex = 0; parameterIndex < function.at(cleanupBlock).parameters.size(); ++parameterIndex) {
            ValueID parameter = function.at(cleanupBlock).parameters[parameterIndex];
            Vector<Use, 4> usesBehind;
            for (const Use& use : pass.analyses.useLists().usesOf(parameter)) {
                if (use.block != cleanupBlock)
                    usesBehind.append(use);
            }
            if (usesBehind.isEmpty())
                continue;

            ValueID passedOn = builder.addParameter(handlerAround, function.at(parameter).type);
            for (const Use& use : usesBehind)
                replaceUse(function, use, passedOn);
            builder.appendArgument(Edge::toHandlerOf(cleanupBlock), parameter);
            didChange = true;
        }
    }
    if (didChange)
        pass.analyses.didChangeValues();
}

// Every block names its handler, so one walk over the blocks finds the covered ones. Asking the CFG for the edges into a cleanup block would compute it again after every redirected edge.
void dropCleanupRegions(FunctionPassContext& pass, IRBuilder& builder, std::span<const BlockID> cleanupBlocks)
{
    if (cleanupBlocks.empty())
        return;

    const Function& function = pass.function;
    BitVector isDropped(function.blocks.size());
    for (BlockID cleanupBlock : cleanupBlocks)
        isDropped.quickSet(cleanupBlock.index());
    auto leadsToDroppedBlock = [&](const std::optional<Target>& handler) {
        return handler && isDropped.quickGet(handler->block.index());
    };

    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        BlockID coveredBlock(index);
        if (!leadsToDroppedBlock(function.at(coveredBlock).handler))
            continue;

        std::optional<Target> outerHandler = findHandlerAround(function, *function.at(coveredBlock).handler);
        while (leadsToDroppedBlock(outerHandler))
            outerHandler = findHandlerAround(function, *outerHandler);

        if (outerHandler)
            builder.redirectEdge(Edge::toHandlerOf(coveredBlock), WTF::move(*outerHandler));
        else
            builder.removeHandler(coveredBlock);
    }
    builder.emptyUnreachableBlocks();
}

void releaseCleanupBlocks(FunctionPassContext& pass, std::span<const BlockID> cleanupBlocks)
{
    bool didRelease = false;
    for (BlockID cleanupBlock : cleanupBlocks) {
        Block& block = pass.function.edit(cleanupBlock);
        if (block.role != BlockRole::IterationCleanup)
            continue;
        block.role = BlockRole::CleanupHandler;
        block.subject = ValueID();
        didRelease = true;
    }
    if (didRelease)
        pass.analyses.didChangeValues();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

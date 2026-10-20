/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFunctionAnalyses.h"

namespace JSC { namespace AOT { namespace Typed {

static Vector<BlockID, 8> collectCoveredBlocks(const CFG& cfg, BlockID cleanupBlock)
{
    Vector<BlockID, 8> coveredBlocks;
    for (Edge edge : cfg.incomingEdges(cleanupBlock)) {
        RELEASE_ASSERT(edge.isHandlerEdge());
        coveredBlocks.append(edge.from);
    }
    return coveredBlocks;
}

static Target createOuterHandlerFor(const Function& function, BlockID coveredBlock, BlockID cleanupBlock, const Target& outerHandler)
{
    const CompactList<ValueID>& parameters = function.at(cleanupBlock).parameters;
    const Arguments& passedToCleanupBlock = function.at(coveredBlock).handler->arguments;
    RELEASE_ASSERT(passedToCleanupBlock.size() == parameters.size());

    Target handler = outerHandler;
    for (ValueID& argument : handler.arguments) {
        size_t parameterIndex = parameters.find(argument);
        if (parameterIndex != notFound)
            argument = passedToCleanupBlock[parameterIndex];
    }
    return handler;
}

static void redirectCoveredBlocks(FunctionPassContext& pass, IRBuilder& builder, BlockID cleanupBlock)
{
    const Function& function = pass.function;
    Vector<BlockID, 8> coveredBlocks = collectCoveredBlocks(pass.analyses.cfg(), cleanupBlock);
    std::optional<Target> outerHandler = function.at(cleanupBlock).handler;
    for (BlockID coveredBlock : coveredBlocks) {
        if (outerHandler)
            builder.redirectEdge(Edge::toHandlerOf(coveredBlock), createOuterHandlerFor(function, coveredBlock, cleanupBlock, *outerHandler));
        else
            builder.removeHandler(coveredBlock);
    }
}

void dropCleanupRegions(FunctionPassContext& pass, IRBuilder& builder, std::span<const BlockID> cleanupBlocks)
{
    if (cleanupBlocks.empty())
        return;

    for (BlockID cleanupBlock : cleanupBlocks)
        redirectCoveredBlocks(pass, builder, cleanupBlock);
    builder.emptyUnreachableBlocks();
}

void releaseCleanupBlocks(FunctionPassContext& pass, std::span<const BlockID> cleanupBlocks)
{
    bool didRelease = false;
    for (BlockID cleanupBlock : cleanupBlocks) {
        Block& block = pass.function.at(cleanupBlock);
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

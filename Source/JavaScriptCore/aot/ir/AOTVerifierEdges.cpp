/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include <span>

namespace JSC { namespace AOT { namespace Typed {

void FunctionVerifier::verifyEdges()
{
    for (uint32_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const Block& contents = m_function.at(block);
        std::span<const Target> targets = contents.terminator.targets.span();
        for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            verifyEdge(block, targetIndex, targets[targetIndex]);
            if (hasFailed())
                return;
        }
        verifyHandler(block, contents);
        if (hasFailed())
            return;
        verifyBlockRole(block, contents);
        if (hasFailed())
            return;
    }
    verifyHandlerBlocksAreEnteredByHandlerEdges();
}

void FunctionVerifier::verifyEdge(BlockID from, uint32_t targetIndex, const Target& target)
{
    const Terminator& terminator = m_function.at(from).terminator;
    uint32_t numberOfTargets = terminator.targets.size();
    if (!target.proofs.isEmpty() && targetResultsOf(terminator.kind, targetIndex, numberOfTargets) != TargetResults::Proofs) {
        fail(InternalErrorKind::InvalidProof, from);
        return;
    }

    size_t numberOfIncomingValues = target.arguments.size() + numberOfEdgeResults(m_function, from, targetIndex);
    if (numberOfIncomingValues != m_function.at(target.block).parameters.size()) {
        fail(InternalErrorKind::EdgeArgumentCountMismatch, from, { }, target.block.index());
        return;
    }

    verifyProofs(from, targetIndex, target);
}

void FunctionVerifier::verifyProofs(BlockID from, uint32_t targetIndex, const Target& target)
{
    for (uint32_t resultIndex = 0; resultIndex < target.proofs.size(); ++resultIndex) {
        if (tryGetEdgeResult(m_function, EdgeResultPosition { from, targetIndex, resultIndex }))
            continue;
        fail(InternalErrorKind::InvalidProof, from, { }, resultIndex);
        return;
    }
}

void FunctionVerifier::verifyHandler(BlockID block, const Block& contents)
{
    if (!contents.handler)
        return;

    const Target& handler = *contents.handler;
    if (!handler.proofs.isEmpty()) {
        fail(InternalErrorKind::InvalidProof, block);
        return;
    }

    const Block& handlerBlock = m_function.at(handler.block);
    if (handler.arguments.size() != handlerBlock.parameters.size()) {
        fail(InternalErrorKind::EdgeArgumentCountMismatch, block, { }, handler.block.index());
        return;
    }
    if (!handlerBlock.isHandlerBlock())
        fail(InternalErrorKind::InvalidHandler, block, { }, handler.block.index());
}

void FunctionVerifier::verifyBlockRole(BlockID block, const Block& contents)
{
    bool hasFittingSubject = false;
    switch (contents.role) {
    case BlockRole::Ordinary:
    case BlockRole::CatchHandler:
    case BlockRole::CleanupHandler:
        hasFittingSubject = !contents.subject;
        break;
    case BlockRole::IterationCleanup:
        hasFittingSubject = contents.subject && m_function.at(contents.subject).opcode == Opcode::IteratorOpen;
        break;
    case BlockRole::DisposalCleanup:
        hasFittingSubject = !!contents.subject;
        break;
    }
    if (!hasFittingSubject || (block == entryBlock && contents.isHandlerBlock()))
        fail(InternalErrorKind::InvalidBlockRole, block, contents.subject);
}

void FunctionVerifier::verifyHandlerBlocksAreEnteredByHandlerEdges()
{
    for (uint32_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (const Target& target : m_function.at(block).terminator.targets) {
            if (!m_function.at(target.block).isHandlerBlock())
                continue;
            fail(InternalErrorKind::InvalidHandler, block, { }, target.block.index());
            return;
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

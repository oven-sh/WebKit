/*
 * Copyright (C) 2014-2023 Apple Inc. All rights reserved.
 * Copyright (C) 2014 Saam Barati. <saambarati1@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "ControlFlowProfiler.h"

#include "BytecodeStructs.h"
#include "CodeCache.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "VM.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ControlFlowProfiler);

ControlFlowProfiler::ControlFlowProfiler() = default;

ControlFlowProfiler::~ControlFlowProfiler()
{
    for (const BlockLocationCache& cache : m_sourceIDBuckets.values()) {
        for (BasicBlockLocation* block : cache.values())
            delete block;
    }
}

BasicBlockLocation* ControlFlowProfiler::getBasicBlockLocation(SourceID sourceID, int startOffset, int endOffset)
{
    auto addResult = m_sourceIDBuckets.add(sourceID, BlockLocationCache());
    BlockLocationCache& blockLocationCache = addResult.iterator->value;
    BasicBlockKey key(startOffset, endOffset);
    auto addResultForBasicBlock = blockLocationCache.add(key, nullptr);
    if (addResultForBasicBlock.isNewEntry)
        addResultForBasicBlock.iterator->value = new BasicBlockLocation(startOffset, endOffset);
    return addResultForBasicBlock.iterator->value;
}

BasicBlockLocation* ControlFlowProfiler::getBranchLocation(SourceID sourceID, const OpProfileControlFlow& bytecode)
{
    ASSERT(bytecode.m_isBranch);
    int decisionStartOffset = bytecode.m_textOffset;
    int decisionEndOffset = bytecode.m_textEndOffset - 1;
    DecisionCache& cache = m_decisions.add(sourceID, DecisionCache()).iterator->value;
    auto& branches = cache.add(BasicBlockKey(decisionStartOffset, decisionEndOffset), Vector<std::unique_ptr<BasicBlockLocation>, 2>()).iterator->value;
    while (branches.size() <= bytecode.m_branchIndex)
        branches.append(makeUnique<BasicBlockLocation>(decisionStartOffset, decisionEndOffset));
    BasicBlockLocation* branch = branches[bytecode.m_branchIndex].get();
    branch->setCountsOtherBranches(bytecode.m_countsOtherBranches);
    return branch;
}

void ControlFlowProfiler::didLinkCodeBlock(VM& vm, UnlinkedCodeBlock* codeBlock, const SourceCode& source)
{
    // A function of a module can be called before the module is evaluated.
    if (codeBlock->codeType() != FunctionCode && m_sourcesWithEagerBytecode.remove(source.providerID()))
        generateBytecodeForFunctions(vm, codeBlock, source);
}

void ControlFlowProfiler::generateBytecodeForFunctions(VM& vm, UnlinkedCodeBlock* codeBlock, const SourceCode& source)
{
    ParserError error;
    generateUnlinkedCodeBlockForFunctions(vm, codeBlock, source, codeBlock->codeGenerationMode(), error, std::numeric_limits<unsigned>::max(), OptimizeBytecode::No, [&](UnlinkedFunctionCodeBlock* functionCodeBlock, const SourceCode& functionSource) {
        if (!functionCodeBlock->hasOpProfileControlFlowBytecodeOffsets())
            return;
        for (JSInstructionStream::Offset offset : functionCodeBlock->opProfileControlFlowBytecodeOffsets()) {
            auto bytecode = functionCodeBlock->instructions().at(offset)->as<OpProfileControlFlow>();
            if (bytecode.m_isBranch)
                getBranchLocation(functionSource.providerID(), bytecode);
        }
    });
}

Vector<BranchRange> ControlFlowProfiler::getBranchesForSourceID(SourceID sourceID) const
{
    Vector<BranchRange> result;
    auto decisions = m_decisions.find(sourceID);
    if (decisions == m_decisions.end())
        return result;

    for (const auto& [decision, branches] : decisions->value) {
        size_t executionCountOfOtherBranches = 0;
        for (const auto& branch : branches) {
            if (!branch->countsOtherBranches())
                executionCountOfOtherBranches += branch->executionCount();
        }
        for (unsigned index = 0; index < branches.size(); index++) {
            size_t executionCount = branches[index]->executionCount();
            if (branches[index]->countsOtherBranches())
                executionCount -= executionCountOfOtherBranches;
            result.append(BranchRange { decision.m_startOffset, decision.m_endOffset, index, executionCount });
        }
    }
    std::ranges::sort(result, [](const BranchRange& a, const BranchRange& b) {
        return std::tie(a.m_decisionStartOffset, a.m_decisionEndOffset, a.m_index) < std::tie(b.m_decisionStartOffset, b.m_decisionEndOffset, b.m_index);
    });
    return result;
}

void ControlFlowProfiler::dumpData() const
{
    auto iter = m_sourceIDBuckets.begin();
    auto end = m_sourceIDBuckets.end();
    for (; iter != end; ++iter) {
        dataLog("SourceID: ", iter->key, "\n");
        for (const BasicBlockLocation* block : iter->value.values())
            block->dumpData();
    }
}

Vector<BasicBlockRange> ControlFlowProfiler::getBasicBlocksForSourceIDWithoutFunctionRange(SourceID sourceID, VM& vm, Gaps gaps) const
{
    Vector<BasicBlockRange> result(0);
    auto bucketFindResult = m_sourceIDBuckets.find(sourceID);
    if (bucketFindResult == m_sourceIDBuckets.end())
        return result;

    const BlockLocationCache& cache = bucketFindResult->value;
    for (const BasicBlockLocation* block : cache.values()) {
        bool hasExecuted = block->hasExecuted();
        size_t executionCount = block->executionCount();

        const Vector<BasicBlockLocation::Gap>& blockRanges = gaps == Gaps::With ? Vector { BasicBlockLocation::Gap { block->startOffset(), block->endOffset() } } : block->getExecutedRanges();
        result.reserveCapacity(result.size() + blockRanges.size());
        for (BasicBlockLocation::Gap gap : blockRanges) {
            BasicBlockRange range;
            range.m_hasExecuted = hasExecuted;
            range.m_isInLoopHeader = block->isInLoopHeader();
            range.m_executionCount = executionCount;
            range.m_startOffset = gap.first;
            range.m_endOffset = gap.second;
            result.append(range);
        }
    }

    // The body of a generator or of an async function and a static block are a part of what the source has as a function
    // or a class, and run as functions of their own: a gap in the basic blocks around them, with none of their own until they run.
    for (auto [hasExecuted, start, end, parseMode] : vm.functionHasExecutedCache()->getFunctionRanges(sourceID)) {
        if (hasExecuted || (!isGeneratorOrAsyncFunctionBodyParseMode(parseMode) && parseMode != SourceParseMode::ClassStaticBlockMode))
            continue;
        BasicBlockRange range;
        range.m_hasExecuted = false;
        range.m_executionCount = 0;
        range.m_startOffset = static_cast<int>(start);
        range.m_endOffset = static_cast<int>(end);
        result.append(range);
    }

    return result;
}

Vector<BasicBlockRange> ControlFlowProfiler::getBasicBlocksForSourceID(SourceID sourceID, VM& vm) const
{
    Vector<BasicBlockRange> result(0);
    auto bucketFindResult = m_sourceIDBuckets.find(sourceID);
    if (bucketFindResult == m_sourceIDBuckets.end())
        return result;

    const BlockLocationCache& cache = bucketFindResult->value;
    for (const BasicBlockLocation* block : cache.values()) {
        bool hasExecuted = block->hasExecuted();
        size_t executionCount = block->executionCount();
        const Vector<BasicBlockLocation::Gap>& blockRanges = block->getExecutedRanges();
        for (BasicBlockLocation::Gap gap : blockRanges) {
            BasicBlockRange range;
            range.m_hasExecuted = hasExecuted;
            range.m_executionCount = executionCount;
            range.m_startOffset = gap.first;
            range.m_endOffset = gap.second;
            result.append(range);
        }
    }

    auto functionRanges = vm.functionHasExecutedCache()->getFunctionRanges(sourceID);
    for (const auto& functionRange : functionRanges) {
        BasicBlockRange range;
        range.m_hasExecuted = std::get<0>(functionRange);
        range.m_startOffset = static_cast<int>(std::get<1>(functionRange));
        range.m_endOffset = static_cast<int>(std::get<2>(functionRange));
        range.m_executionCount = range.m_hasExecuted ? 1 : 0; // This is a hack. We don't actually count this.
        result.append(range);
    }

    return result;
}

static BasicBlockRange LEGACY_NODELETE findBasicBlockAtTextOffset(int offset, const Vector<BasicBlockRange>& blocks)
{
    int bestDistance = INT_MAX;
    BasicBlockRange bestRange;
    bestRange.m_startOffset = bestRange.m_endOffset = -1;
    bestRange.m_hasExecuted = false; // Suppress MSVC warning.
    // Because some ranges may overlap because of function boundaries, make sure to find the smallest range enclosing the offset.
    for (BasicBlockRange range : blocks) {
        if (range.m_startOffset <= offset && offset <= range.m_endOffset && (range.m_endOffset - range.m_startOffset) < bestDistance) {
            RELEASE_ASSERT(range.m_endOffset - range.m_startOffset >= 0);
            bestDistance = range.m_endOffset - range.m_startOffset;
            bestRange = range;
        }
    }

    RELEASE_ASSERT(bestRange.m_startOffset != -1 && bestRange.m_endOffset != -1);
    return bestRange;
}

bool ControlFlowProfiler::hasBasicBlockAtTextOffsetBeenExecuted(int offset, SourceID sourceID, VM& vm)
{
    const Vector<BasicBlockRange>& blocks = getBasicBlocksForSourceID(sourceID, vm);
    BasicBlockRange range = findBasicBlockAtTextOffset(offset, blocks);
    return range.m_hasExecuted;
}

size_t ControlFlowProfiler::basicBlockExecutionCountAtTextOffset(int offset, SourceID sourceID, VM& vm)
{
    const Vector<BasicBlockRange>& blocks = getBasicBlocksForSourceID(sourceID, vm);
    BasicBlockRange range = findBasicBlockAtTextOffset(offset, blocks);
    return range.m_executionCount;
}

} // namespace JSC

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeLayout.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallSiteTable.h"
#include "AOTCodeTemplates.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static bool endsInJumpToNextBody(const LinkedFunctions& functions, const ResolvedCallees& callees, const OutlinedCode& outlinedCode, uint32_t position)
{
    const CodeBody& body = functions[position].body;
    if (body.relocations.isEmpty())
        return false;
    const Relocation& jump = body.relocations.last();
    if (jump.kind != RelocationKind::Jump || jump.space != SymbolSpace::Function || jump.entry != primaryEntry || jump.offset + sizeOfJump != body.bytes.size())
        return false;
    ResolvedCallee callee = callees.at(position, jump.symbol);
    return callee.body == OptionalIndex { position + 1 } && !callee.alias
        && !outlinedCode.ownBodiesSize(position) && !functions[position + 1].body.primaryEntryOffset();
}

static std::optional<size_t> computeNopSize(const CompiledCode& function)
{
    // The layout aligns the primary entry, so the first byte of the body lies this far past a multiple of the alignment.
    size_t startMisalignment = (imageFunctionAlignment - function.body.primaryEntryOffset() % imageFunctionAlignment) % imageFunctionAlignment;
    auto roundUpToAlignedEnd = [&](size_t size) {
        return WTF::roundUpToMultipleOf<imageFunctionAlignment>(size + startMisalignment) - startMisalignment;
    };
    // Image::locate would attribute a return address at the very end of a body to the next body.
    size_t offset = function.body.relocations.last().offset;
    std::optional<uint32_t> lastReturnSite = endOfLastReturnSite(function);
    size_t newSize = std::max(roundUpToAlignedEnd(offset), lastReturnSite ? roundUpToAlignedEnd(static_cast<size_t>(*lastReturnSite) + 1) : 0);
    if (!newSize || newSize - offset >= sizeOfJump)
        return std::nullopt;
    return newSize - offset;
}

Vector<FinalJump> findFinalJumps(const LinkedFunctions& functions, const ResolvedCallees& callees, const OutlinedCode& outlinedCode, BuildContext& context)
{
    Vector<FinalJump> finalJumps(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        const CompiledCode& function = functions[position];
        finalJumps[position].sizeWithoutJump = safeCast<uint32_t>(function.body.bytes.size());
        if (position + 1 == functions.size() || !endsInJumpToNextBody(functions, callees, outlinedCode, position))
            return;
        if (std::optional<size_t> nopSize = computeNopSize(function))
            finalJumps[position] = { safeCast<uint32_t>(function.body.relocations.last().offset + *nopSize), true };
    });
    return finalJumps;
}

static void removeFinalJump(CodeBody& body, const FinalJump& finalJump)
{
    RELEASE_ASSERT(finalJump.jumpsToNextBody);
    size_t offset = body.relocations.takeLast().offset;
    body.bytes.shrink(offset);
    body.bytes.grow(finalJump.sizeWithoutJump);
    fillWithNops(body.bytes.mutableSpan().subspan(offset));
    body.paddingStart = finalJump.sizeWithoutJump;
}

void removeFinalJumps(LinkedFunctions& functions, const BitVector& fallsIntoNext, std::span<const FinalJump> finalJumps, BuildContext& context)
{
    RELEASE_ASSERT(finalJumps.size() == functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        CompiledCode& function = functions[position];
        if (fallsIntoNext.get(position))
            removeFinalJump(function.body, finalJumps[position]);
        std::optional<uint32_t> lastReturnSite = endOfLastReturnSite(function);
        RELEASE_ASSERT_WITH_MESSAGE(!lastReturnSite || *lastReturnSite < function.body.bytes.size(), "AOT: a function ends in a call: its return address would be taken for the next function's");
    });
    context.statistics().count(StatisticID::JumpsToNextFunctionRemoved, fallsIntoNext.bitCount());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

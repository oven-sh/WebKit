/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallSiteTable.h"
#include "AOTCodeIndex.h"
#include "AOTCodeTemplates.h"
#include "AOTThreads.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

enum class ChainState : uint8_t {
    NotSeen,
    OnPath,
    Done,
};

struct MovedBody {
    uint32_t position;
    uint32_t target;
    uint32_t alias;
};

struct HostBody {
    uint32_t position;
    uint32_t firstAppendedBody;
};

struct RemovedBodies {
    BitVector isRemoved;
    Vector<MovedBody> prependedHeads;
    Vector<MovedBody> appendedBodies;
    Vector<HostBody> hosts;
};

} // anonymous namespace

static constexpr size_t maxPinnedUnitLength = UINT16_MAX / codeOffsetUnit * codeOffsetUnit;

static void appendPinnedRange(Vector<CodeUnit>& units, size_t begin, size_t end)
{
    while (begin < end) {
        size_t length = std::min(end - begin, maxPinnedUnitLength);
        units.append(CodeUnit { safeCast<uint32_t>(begin), static_cast<uint16_t>(length), CodeUnit::Kind::Pinned });
        begin += length;
    }
}

static void appendPinnedUnit(CodeBody& body, size_t start)
{
    if (!body.units.isEmpty())
        appendPinnedRange(body.units, start, body.bytes.size());
}

static void trimPadding(CompiledCode& code, const CodeTemplates& templates)
{
    CodeBody& body = code.body;
    RELEASE_ASSERT(body.paddingStart <= body.bytes.size());
    // Image::locate would attribute a return address at the very end of a body to the next body.
    std::optional<uint32_t> lastReturnSite = endOfLastReturnSite(code);
    size_t size = std::max<size_t>(body.paddingStart, lastReturnSite ? *lastReturnSite + codeOffsetUnit : 0);
    if (size < body.bytes.size()) {
        body.bytes.shrink(size);
        while (!body.units.isEmpty() && body.units.last().offset >= size)
            body.units.removeLast();
        if (!body.units.isEmpty() && body.units.last().offset + body.units.last().length > size) {
            CodeUnit& last = body.units.last();
            RELEASE_ASSERT(last.kind == CodeUnit::Kind::Pinned);
            last.length = safeCast<uint16_t>(size - last.offset);
        }
    } else if (size > body.bytes.size()) {
        size_t start = body.bytes.size();
        RELEASE_ASSERT(!((size - start) % templates.breakpoint.size()));
        while (body.bytes.size() < size)
            body.bytes.appendVector(templates.breakpoint);
        appendPinnedUnit(body, start);
    }
    body.paddingStart = safeCast<uint32_t>(size);
}

static const Relocation* findOnlyJump(const CompiledCode& code)
{
    const CodeBody& body = code.body;
    const SiteDescription& sites = code.sites;
    if (code.frame.hasFrame() || !code.frame.prologueChecks.isEmpty())
        return nullptr;
    if (body.relocations.size() != 1 || body.entries.size() != 1)
        return nullptr;
    if (!body.localSites.isEmpty() || !body.returnSites.isEmpty() || !body.firstReadSites.isEmpty() || !body.constants.isEmpty() || !body.jumpTables.isEmpty())
        return nullptr;
    if (sites.positions.size() != 1 || sites.numberOfSitesWithoutCode
        || !sites.inlineFrames.isEmpty() || !sites.spreadSites.isEmpty() || !sites.flaggedSites.isEmpty() || !sites.handlers.isEmpty()
        || !sites.faultingAccesses.isEmpty() || !sites.pollSites.isEmpty() || !sites.frameMapWords.isEmpty())
        return nullptr;
    const Relocation& jump = body.relocations[0];
    if (jump.kind != RelocationKind::Jump || jump.space != SymbolSpace::Function || jump.entry != primaryEntry || jump.offset + sizeOfJump != body.bytes.size())
        return nullptr;
    RELEASE_ASSERT(!code.entry.hasValueEntry);
    RELEASE_ASSERT(body.primaryEntryOffset() <= jump.offset);
    return &jump;
}

static OptionalIndex findJumpTarget(const CompiledCode& code, uint32_t position, const CodeIndex& index)
{
    const Relocation* jump = findOnlyJump(code);
    if (!jump)
        return { };
    std::optional<uint32_t> callee = index.indexOf(code.body.callees[jump->symbol]);
    if (!callee || *callee == position)
        return { };
    return { *callee };
}

static bool isOneJump(const LinkedFunctions& functions, const Vector<OptionalIndex>& jumpsTo, uint32_t position)
{
    return jumpsTo[position] && !functions[position].body.relocations[0].offset;
}

static Vector<OptionalIndex> findChainEnds(const LinkedFunctions& functions, const Vector<OptionalIndex>& jumpsTo)
{
    uint32_t count = safeCast<uint32_t>(functions.size());
    Vector<OptionalIndex> chainEnd(FillWith { }, count, OptionalIndex { });
    Vector<ChainState> states(FillWith { }, count, ChainState::NotSeen);
    Vector<uint32_t, 16> path;
    for (uint32_t start = 0; start < count; ++start) {
        uint32_t position = start;
        while (states[position] == ChainState::NotSeen && isOneJump(functions, jumpsTo, position)) {
            states[position] = ChainState::OnPath;
            path.append(position);
            position = *jumpsTo[position];
        }
        OptionalIndex end;
        switch (states[position]) {
        case ChainState::NotSeen:
            end = { position };
            path.append(position);
            break;
        case ChainState::OnPath:
            break;
        case ChainState::Done:
            end = chainEnd[position];
            break;
        }
        for (uint32_t member : path) {
            chainEnd[member] = end;
            states[member] = ChainState::Done;
        }
        path.shrink(0);
    }
    return chainEnd;
}

static Vector<uint32_t> computeUseCounts(const LinkedFunctions& functions, const CodeIndex& index, const Vector<OptionalIndex>& chainEnd, const ParallelLoops& loops)
{
    Vector<uint32_t> uses(FillWith { }, functions.size(), 0u);
    forEachIndexInParallel(loops, safeCast<unsigned>(functions.size()), [&](unsigned position) {
        const CodeBody& body = functions[position].body;
        for (const Relocation& relocation : body.relocations) {
            if (relocation.space != SymbolSpace::Function)
                continue;
            std::optional<uint32_t> callee = index.indexOf(body.callees[relocation.symbol]);
            if (!callee)
                continue;
            uint32_t target = chainEnd[*callee] ? *chainEnd[*callee] : *callee;
            WTF::atomicExchangeAdd(&uses[target], 1u, std::memory_order_relaxed);
        }
    });
    return uses;
}

static void propagateStackProof(const FrameDescription& removed, FrameDescription& target)
{
    if (removed.isProvenToFitOnStack && target.isEnteredWithoutStackCheck)
        target.isProvenToFitOnStack = true;
}

static bool canPrependHead(const CodeBody& target)
{
    return target.entries.size() == 1 && !target.primaryEntryOffset();
}

static RemovedBodies createAliases(LinkedFunctions& functions, const Vector<OptionalIndex>& jumpsTo, const Vector<OptionalIndex>& chainEnd, const Vector<uint32_t>& uses, Vector<EntryAlias>& aliases)
{
    RemovedBodies result { BitVector(functions.size()), { }, { }, { } };
    BitVector hasPrependedHead(functions.size());
    OptionalIndex lastRetained;
    auto choosePlacement = [&](uint32_t position, uint32_t end) -> std::optional<AliasPlacement> {
        if (isOneJump(functions, jumpsTo, position))
            return AliasPlacement::AtPrimaryEntry;
        if (*jumpsTo[position] == end && !hasPrependedHead.get(end) && canPrependHead(functions[end].body))
            return AliasPlacement::BeforeBody;
        if (lastRetained)
            return AliasPlacement::AfterBody;
        return std::nullopt;
    };
    for (uint32_t position = 0; position < functions.size(); ++position) {
        OptionalIndex end = jumpsTo[position] && !uses[position] ? chainEnd[*jumpsTo[position]] : OptionalIndex { };
        std::optional<AliasPlacement> placement = end ? choosePlacement(position, *end) : std::nullopt;
        if (!placement) {
            lastRetained = { position };
            continue;
        }
        const CompiledCode& removed = functions[position];
        CompiledCode& target = functions[*end];
        result.isRemoved.set(position);
        propagateStackProof(removed.frame, target.frame);
        uint32_t alias = safeCast<uint32_t>(aliases.size());
        FunctionID body = target.function;
        switch (*placement) {
        case AliasPlacement::AtPrimaryEntry:
            break;
        case AliasPlacement::BeforeBody:
            hasPrependedHead.set(*end);
            result.prependedHeads.append({ position, *end, alias });
            break;
        case AliasPlacement::AfterBody:
            if (result.hosts.isEmpty() || result.hosts.last().position != *lastRetained)
                result.hosts.append({ *lastRetained, safeCast<uint32_t>(result.appendedBodies.size()) });
            result.appendedBodies.append({ position, *end, alias });
            body = functions[*lastRetained].function;
            break;
        }
        aliases.append({ .function = removed.function, .body = body, .entry = removed.entry, .placement = *placement });
    }
    return result;
}

static void prependHead(CompiledCode& target, const CompiledCode& removed, const Relocation& jump, EntryAlias& alias)
{
    constexpr size_t alignment = imageFunctionAlignment;
    CodeBody& body = target.body;
    std::span<const uint8_t> head = removed.body.bytes.span().first(jump.offset);
    uint32_t prefixSize = removed.body.primaryEntryOffset();
    size_t pad = (alignment - (head.size() - prefixSize) % alignment) % alignment;
    uint32_t shift = safeCast<uint32_t>(head.size() + pad);
    forEachCodeOffset(target, [&](uint32_t& offset, CodeOffsetRole, unsigned) {
        offset += shift;
    });
    if (!body.units.isEmpty()) {
        for (CodeUnit& unit : body.units)
            unit.offset += shift;
        Vector<CodeUnit> pinnedUnits;
        appendPinnedRange(pinnedUnits, 0, shift);
        body.units.insertVector(0, pinnedUnits);
    }
    body.bytes.insertFill(0, 0, shift);
    memcpySpan(body.bytes.mutableSpan(), head);
    fillWithNops(body.bytes.mutableSubspan(head.size(), pad));
    alias.start = 0;
    alias.prefixSize = prefixSize;
}

static void appendBody(CodeBody& host, const CompiledCode& guest, FunctionID target, EntryAlias& alias)
{
    constexpr size_t alignment = imageFunctionAlignment;
    static_assert(alignment <= 16);
    size_t hostEnd = host.bytes.size();
    uint32_t guestEntry = guest.body.primaryEntryOffset();
    // The layout aligns the primary entry of the host, so the first byte of the host lies this far past a multiple of the alignment.
    size_t startMisalignment = (alignment - host.primaryEntryOffset() % alignment) % alignment;
    size_t start = WTF::roundUpToMultipleOf<alignment>(startMisalignment + hostEnd + guestEntry) - startMisalignment - guestEntry;
    while (host.bytes.size() < start)
        host.bytes.append(0);
    alias.start = safeCast<uint32_t>(start);
    alias.prefixSize = guestEntry;
    host.bytes.appendVector(guest.body.bytes);
    appendPinnedUnit(host, hostEnd);
    Relocation jump = guest.body.relocations[0];
    jump.offset += alias.start;
    size_t callee = host.callees.find(target);
    if (callee == notFound) {
        callee = host.callees.size();
        host.callees.append(target);
    }
    jump.symbol = safeCast<uint32_t>(callee);
    host.relocations.append(jump);
    host.paddingStart = safeCast<uint32_t>(host.bytes.size());
}

void removeJumpOnlyFunctions(LinkedFunctions& functions, Vector<EntryAlias>& aliases, const CodeTemplates& templates, BuildContext& context)
{
    RELEASE_ASSERT(aliases.isEmpty());
    const ParallelLoops& loops = context.parallelLoops();
    unsigned count = safeCast<unsigned>(functions.size());
    CodeIndex index(functions.span());
    Vector<OptionalIndex> jumpsTo(FillWith { }, count, OptionalIndex { });
    forEachIndexInParallel(loops, count, [&](unsigned position) {
        trimPadding(functions[position], templates);
        jumpsTo[position] = findJumpTarget(functions[position], position, index);
    });
    Vector<OptionalIndex> chainEnd = findChainEnds(functions, jumpsTo);
    Vector<uint32_t> uses = computeUseCounts(functions, index, chainEnd, loops);
    RemovedBodies removed = createAliases(functions, jumpsTo, chainEnd, uses, aliases);
    forEachIndexInParallel(loops, removed.prependedHeads.size(), [&](unsigned number) {
        MovedBody head = removed.prependedHeads[number];
        const CompiledCode& source = functions[head.position];
        prependHead(functions[head.target], source, source.body.relocations[0], aliases[head.alias]);
    });
    forEachIndexInParallel(loops, removed.hosts.size(), [&](unsigned number) {
        HostBody host = removed.hosts[number];
        size_t appendedBodiesEnd = number + 1 < removed.hosts.size() ? removed.hosts[number + 1].firstAppendedBody : removed.appendedBodies.size();
        for (const MovedBody& guest : removed.appendedBodies.subspan(host.firstAppendedBody, appendedBodiesEnd - host.firstAppendedBody))
            appendBody(functions[host.position].body, functions[guest.position], functions[guest.target].function, aliases[guest.alias]);
    });
    if (!aliases.isEmpty()) {
        Vector<uint32_t> retainedPositions = findIndicesInParallel(loops, count, [&](unsigned position) {
            return !removed.isRemoved.get(position);
        });
        functions.reorder(retainedPositions.span(), context);
    }
    context.statistics().count(StatisticID::JumpOnlyFunctionsRemoved, aliases.size());
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOutliner.h"

#if ENABLE(AOT)

#include "AOTAscendingSearch.h"
#include "AOTBuildContext.h"
#include "AOTCodeIndex.h"
#include "AOTCodeTemplates.h"
#include "AOTCodeUnits.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "CPU.h"
#include "MacroAssembler.h"
#include <algorithm>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

AOT_THRESHOLD(size_t, maxComparedBytes, 16);
static_assert(sizeOfNearCall == sizeOfJump);

namespace {

struct Shift {
    uint32_t from;
    uint32_t shift;
};

struct OriginalCode {
    Vector<uint8_t> bytes;
    Vector<CodeUnit> units;
    Vector<LinkedSite> localSites;
    Vector<uint32_t> callOffsets; // By site, in the rewritten bytes.
};

struct RewrittenCode {
    Vector<uint8_t> bytes;
    Vector<Shift> shifts;
    Vector<uint32_t> callOffsets;
    Vector<std::pair<uint32_t, uint32_t>, 0> paddingBeforeAliases;
};

struct RetainedRewrite {
    OriginalCode original;
    Vector<Shift> shifts;
};

// Where the last search ended in each list of one function: upperBoundFrom().
struct SearchPositions {
    size_t shift { 0 };
    size_t site { 0 };
    size_t unit { 0 };
    size_t localSite { 0 };
};

// Whether every offset that has moved is compared with the code that it named before. No byte of the output depends on it: CompileOptions::verifiesProgram.
enum class RewriteCheck : bool { Skip, Run };

} // anonymous namespace

static uint32_t endOf(const OutlinedSite& site)
{
    return site.offset + site.length;
}

static uint32_t mapped(const Vector<Shift>& shifts, uint32_t offset, size_t& position)
{
    size_t found = upperBoundFrom(position, shifts.span(), offset, [](uint32_t offset, const Shift& entry) {
        return offset < entry.from;
    });
    return offset - (found ? shifts[found - 1].shift : 0);
}

static uint32_t mapped(const Vector<Shift>& shifts, uint32_t offset)
{
    size_t position = 0;
    return mapped(shifts, offset, position);
}

static const OutlinedSite* findSiteOverlapping(const Vector<OutlinedSite>& sites, uint32_t firstByte, uint32_t lastByte, size_t& position)
{
    size_t found = upperBoundFrom(position, sites.span(), firstByte, [](uint32_t byte, const OutlinedSite& site) {
        return byte < site.offset + site.length;
    });
    return found != sites.size() && sites[found].offset <= lastByte ? &sites[found] : nullptr;
}

static bool canBeMapped(const Vector<OutlinedSite>& sites, uint32_t offset, size_t& position)
{
    auto* site = findSiteOverlapping(sites, offset, offset, position);
    return !site || site->offset == offset;
}

static const CodeUnit* findUnitContaining(const Vector<CodeUnit>& units, uint32_t offset, size_t& position)
{
    size_t next = upperBoundFrom(position, units.span(), offset, [](uint32_t offset, const CodeUnit& unit) {
        return offset < unit.offset;
    });
    return next && offset - units[next - 1].offset < units[next - 1].length ? &units[next - 1] : nullptr;
}

static const LinkedSite* findNextLocalSite(const Vector<LinkedSite>& localSites, uint32_t offset, size_t& position)
{
    size_t found = upperBoundFrom(position, localSites.span(), offset, [](uint32_t offset, const LinkedSite& site) {
        return offset < site.offset + MacroAssembler::relocationSize(site.kind);
    });
    return found != localSites.size() ? &localSites[found] : nullptr;
}

static Vector<uint8_t, 8> copyWithClearedField(std::span<const uint8_t> bytes, uint32_t offset, RelocationKind kind)
{
    Vector<uint8_t, 8> instructions(bytes.subspan(offset, MacroAssembler::relocationSize(kind)));
    MacroAssembler::clearRelocation(kind, instructions.mutableSpan());
    return instructions;
}

static bool isUnmoved(const OriginalCode& original, std::span<const uint8_t> newBytes, const Vector<OutlinedSite>& sites, uint32_t oldOffset, uint32_t newOffset, std::optional<RelocationKind> kind, SearchPositions& positions)
{
    if (auto* site = findSiteOverlapping(sites, oldOffset, oldOffset, positions.site))
        return site->offset == oldOffset && newOffset == original.callOffsets[site - sites.begin()];
    std::span<const uint8_t> oldBytes = original.bytes.span();
    if (kind) {
        size_t size = MacroAssembler::relocationSize(*kind);
        if (oldOffset + size > oldBytes.size() || newOffset + size > newBytes.size())
            return false;
        return copyWithClearedField(oldBytes, oldOffset, *kind) == copyWithClearedField(newBytes, newOffset, *kind);
    }
    auto* unit = findUnitContaining(original.units, oldOffset, positions.unit);
    if (!unit)
        return false;
    size_t size = std::min<size_t>(maxComparedBytes, unit->offset + unit->length - oldOffset);
    // A Pinned unit may contain local sites, whose distances relinkLocalSites has changed.
    if (auto* local = findNextLocalSite(original.localSites, oldOffset, positions.localSite)) {
        if (local->offset < oldOffset)
            return false;
        size = std::min<size_t>(size, local->offset - oldOffset);
    }
    return newOffset + size <= newBytes.size() && equalSpans(oldBytes.subspan(oldOffset, size), newBytes.subspan(newOffset, size));
}

static Vector<Vector<uint32_t, 0>> collectAliasOffsets(size_t numberOfFunctions, const Vector<EntryAlias>& aliases, const CodeIndex& index)
{
    Vector<Vector<uint32_t, 0>> aliasOffsetsByBody(numberOfFunctions);
    for (auto& alias : aliases) {
        if (alias.placement == AliasPlacement::AfterBody)
            aliasOffsetsByBody[*index.indexOf(alias.body)].append(alias.start);
    }
    for (auto& offsets : aliasOffsetsByBody) {
        AOT::sort(offsets);
        removeRepeatedElements(offsets);
    }
    return aliasOffsetsByBody;
}

static const Relocation* findFinalCall(const CodeBody& from, const OutlinedBody& body)
{
    uint32_t callOffset = body.offset + body.length - static_cast<uint32_t>(sizeOfNearCall);
    auto* call = std::ranges::lower_bound(from.relocations, callOffset, { }, &Relocation::offset);
    RELEASE_ASSERT(call != from.relocations.end() && call->offset == callOffset && call->kind == RelocationKind::Call);
    return call;
}

static Relocation createFinalJump(OutlinedCode& result, const CodeBody& from, const ResolvedCallees& callees, const OutlinedBody& body, const Relocation* call, uint32_t bodyStart)
{
    uint32_t sizeBeforeCall = body.length - static_cast<uint32_t>(sizeOfNearCall);
    auto createJumpTo = [&](size_t symbol) {
        return Relocation { bodyStart + sizeBeforeCall, RelocationKind::Jump, call->space, call->entry, safeCast<uint32_t>(symbol) };
    };
    switch (call->space) {
    case SymbolSpace::Function:
        RELEASE_ASSERT(!isARM64());
        result.callees.append(callees.at(body.function, call->symbol));
        return createJumpTo(result.callees.size() - 1);
    case SymbolSpace::Stub:
        result.stubKeys.append(from.stubKeys[call->symbol]);
        return createJumpTo(result.stubKeys.size() - 1);
    case SymbolSpace::ArgumentThunk:
        result.argumentMoves.append(from.argumentMoves[call->symbol]);
        return createJumpTo(result.argumentMoves.size() - 1);
    case SymbolSpace::OutlinedBody:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::SiteCache:
    case SymbolSpace::RegExp:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void appendOutlinedBodies(OutlinedCode& result, const LinkedFunctions& functions, const OutliningRewriteInputs& inputs, const ParallelLoops& loops)
{
    const CodeTemplates& templates = inputs.templates;
    struct Copy {
        const OutlinedBody* body;
        uint32_t at;
        const Relocation* finalCall;
    };
    std::span<const OutlinedBody> bodies = inputs.outlining.bodies.span();
    Vector<Copy> copies;
    copies.reserveInitialCapacity(bodies.size());
    result.indexWithinKind.reserveInitialCapacity(bodies.size());
    uint64_t sizeOfOwnBodies = 0;
    uint64_t sizeOfSharedBodies = 0;
    for (auto& body : bodies) {
        auto& destination = body.singleCaller ? result.ownBodies : result.sharedBodies;
        uint64_t& size = body.singleCaller ? sizeOfOwnBodies : sizeOfSharedBodies;
        result.indexWithinKind.append(destination.offsets.size());
        if (body.singleCaller)
            result.ownBodiesByCaller[*body.singleCaller].append(destination.offsets.size());
        uint32_t at = safeCast<uint32_t>(size);
        destination.offsets.append(at);
        copies.append({ &body, at, nullptr });
        size += body.length + (body.endsInCall ? 0 : templates.returnToCaller.size());
    }
    result.ownBodies.bytes.grow(safeCast<uint32_t>(sizeOfOwnBodies));
    result.sharedBodies.bytes.grow(safeCast<uint32_t>(sizeOfSharedBodies));

    constexpr size_t bodiesInChunk = 256;
    forEachIndexInParallel(loops, static_cast<unsigned>((copies.size() + bodiesInChunk - 1) / bodiesInChunk), [&](unsigned chunk) {
        for (Copy& copy : copies.mutableSpan().subspan(chunk * bodiesInChunk, std::min(bodiesInChunk, copies.size() - chunk * bodiesInChunk))) {
            const OutlinedBody& body = *copy.body;
            auto& from = functions[body.function].body;
            RELEASE_ASSERT(body.length >= sizeOfNearCall && body.offset + body.length <= from.bytes.size());
            std::span<uint8_t> bytes = (body.singleCaller ? result.ownBodies : result.sharedBodies).bytes.mutableSpan().subspan(copy.at);
            memcpySpan(bytes, from.bytes.span().subspan(body.offset, body.length));
            if (body.endsInCall) {
                memcpySpan(bytes.subspan(body.length - sizeOfJump), templates.nearJump.span());
                copy.finalCall = findFinalCall(from, body);
            } else
                memcpySpan(bytes.subspan(body.length), templates.returnToCaller.span());
        }
    });
    for (const Copy& copy : copies) {
        if (!copy.finalCall)
            continue;
        auto& destination = copy.body->singleCaller ? result.ownBodies : result.sharedBodies;
        destination.relocations.append(createFinalJump(result, functions[copy.body->function].body, inputs.callees, *copy.body, copy.finalCall, copy.at));
    }

    parallelSort(loops, copies.mutableSpan(), [](const Copy& a, const Copy& b) {
        return std::pair { a.body->function, a.body->offset } < std::pair { b.body->function, b.body->offset };
    });
    Vector<uint32_t> firstCopyOfFunction;
    for (uint32_t index = 0; index < copies.size(); ++index) {
        if (!index || copies[index].body->function != copies[index - 1].body->function)
            firstCopyOfFunction.append(index);
    }
    forEachIndexInParallel(loops, firstCopyOfFunction.size(), [&](unsigned group) {
        uint32_t end = group + 1 < firstCopyOfFunction.size() ? firstCopyOfFunction[group + 1] : static_cast<uint32_t>(copies.size());
        const PackedCodeUnits& units = functions[copies[firstCopyOfFunction[group]].body->function].body.units;
        auto position = units.begin();
        for (uint32_t index = firstCopyOfFunction[group]; index < end; ++index) {
            const Copy& copy = copies[index];
            auto& destination = copy.body->singleCaller ? result.ownBodies : result.sharedBodies;
            position = adjustForNearCall(destination.bytes.mutableSpan().subspan(copy.at, copy.body->length), position, units.end(), copy.body->offset);
        }
    });
}

static RewrittenCode copyAroundSites(const CodeBody& oldBody, uint32_t position, const Outlining& outlining, const Vector<uint32_t, 0>& aliasOffsets, const CodeTemplates& templates)
{
    const Vector<uint8_t>& oldBytes = oldBody.bytes;
    uint32_t primaryEntryOffset = oldBody.primaryEntryOffset();
    RewrittenCode result;
    auto& bytes = result.bytes;
    bytes.reserveInitialCapacity(oldBytes.size());
    uint32_t copied = 0;
    uint32_t shift = 0;
    size_t nextAlias = 0;
    auto copyUpTo = [&](uint32_t end) {
        for (; nextAlias < aliasOffsets.size() && aliasOffsets[nextAlias] <= end; ++nextAlias) {
            uint32_t alias = aliasOffsets[nextAlias];
            RELEASE_ASSERT(alias >= copied && alias > primaryEntryOffset);
            bytes.append(oldBytes.span().subspan(copied, alias - copied));
            copied = alias;
            RELEASE_ASSERT(bytes.size() + shift == alias);
            // The layout aligns the primary entry, so an alias stays aligned when it moves as far as that entry, modulo the alignment.
            uint32_t primaryEntryShift = primaryEntryOffset - mapped(result.shifts, primaryEntryOffset);
            RELEASE_ASSERT(shift >= primaryEntryShift);
            uint32_t padding = (shift - primaryEntryShift) % static_cast<uint32_t>(imageFunctionAlignment);
            result.paddingBeforeAliases.append({ alias, padding });
            if (!padding)
                continue;
            RELEASE_ASSERT(!(padding % templates.breakpoint.size()));
            for (size_t paddingEnd = bytes.size() + padding; bytes.size() < paddingEnd;)
                bytes.appendVector(templates.breakpoint);
            shift -= padding;
            result.shifts.append({ alias, shift });
        }
        bytes.append(oldBytes.span().subspan(copied, end - copied));
        copied = end;
    };
    for (auto& site : outlining.sites[position]) {
        auto& body = outlining.bodies[site.body];
        RELEASE_ASSERT(site.offset >= copied && site.length == body.length && endOf(site) <= oldBytes.size());
        RELEASE_ASSERT(!body.singleCaller || *body.singleCaller == position);
        copyUpTo(site.offset);
        result.callOffsets.append(safeCast<uint32_t>(bytes.size()));
        bytes.appendVector(templates.nearCall);
        copied = endOf(site);
        shift += site.length - static_cast<uint32_t>(sizeOfNearCall);
        result.shifts.append({ copied, shift });
    }
    copyUpTo(safeCast<uint32_t>(oldBytes.size()));
    return result;
}

static void checkUnitsInSites(const OriginalCode& original, const Vector<OutlinedSite>& sites, const Vector<OutlinedBody>& bodies)
{
    using Kind = CodeUnit::Kind;
    size_t numberOfHeads = 0;
    size_t nextSite = 0;
    uint32_t previousUnitEnd = 0;
    for (const CodeUnit& unit : original.units) {
        uint32_t unitEnd = unit.offset + unit.length;
        RELEASE_ASSERT(unit.offset == previousUnitEnd && unit.length && unitEnd <= original.bytes.size());
        previousUnitEnd = unitEnd;
        while (nextSite < sites.size() && endOf(sites[nextSite]) <= unit.offset)
            ++nextSite;
        if (auto* site = nextSite < sites.size() && sites[nextSite].offset < unitEnd ? &sites[nextSite] : nullptr) {
            bool endsInCall = unitEnd == endOf(*site) && bodies[site->body].endsInCall;
            RELEASE_ASSERT(unit.offset >= site->offset && unitEnd <= endOf(*site));
            RELEASE_ASSERT(endsInCall ? unit.kind == Kind::Call : unit.isPlain());
            numberOfHeads += unit.offset == site->offset;
        }
    }
    RELEASE_ASSERT(previousUnitEnd == original.bytes.size() && numberOfHeads == sites.size());
}

static void relinkLocalSites(const OriginalCode& original, const Vector<OutlinedSite>& sites, const Vector<OutlinedBody>& bodies, RewriteCheck check, RewrittenCode& rewritten)
{
    if (check == RewriteCheck::Run)
        checkUnitsInSites(original, sites, bodies);
    // The sites ascend. Their targets do so by and large.
    SearchPositions atSites;
    SearchPositions atTargets;
    for (const LinkedSite& local : original.localSites) {
        unsigned size = MacroAssembler::relocationSize(local.kind);
        // setRelocationDistance adds to a Distance32. A LinkBuffer links none.
        RELEASE_ASSERT(size && local.kind != RelocationKind::Distance32);
        RELEASE_ASSERT(check == RewriteCheck::Skip || (!findSiteOverlapping(sites, local.offset, local.offset + size - 1, atSites.site) && canBeMapped(sites, local.target, atTargets.site)));
        uint32_t newOffset = mapped(rewritten.shifts, local.offset, atSites.shift);
        uint32_t newTarget = mapped(rewritten.shifts, local.target, atTargets.shift);
        int64_t distance = static_cast<int64_t>(newTarget) - static_cast<int64_t>(newOffset);
        if (distance == static_cast<int64_t>(local.target) - static_cast<int64_t>(local.offset))
            continue;
        RELEASE_ASSERT(MacroAssembler::setRelocationDistance(local.kind, rewritten.bytes.mutableSubspan(newOffset, size), distance));
        RELEASE_ASSERT(check == RewriteCheck::Skip || isUnmoved(original, rewritten.bytes.span(), sites, local.offset, newOffset, local.kind, atSites));
    }
}

static void removeFinalCalls(Vector<Relocation>& relocations, const Vector<OutlinedSite>& sites, const Vector<OutlinedBody>& bodies)
{
    size_t numberOfFinalCalls = 0;
    for (auto& site : sites)
        numberOfFinalCalls += bodies[site.body].endsInCall;
    auto* site = sites.begin();
    size_t numberRemoved = relocations.removeAllMatching([&](const Relocation& relocation) {
        while (site != sites.end() && endOf(*site) <= relocation.offset)
            ++site;
        if (site == sites.end() || relocation.offset < site->offset)
            return false;
        RELEASE_ASSERT(bodies[site->body].endsInCall && relocation.kind == RelocationKind::Call && relocation.offset + sizeOfNearCall == endOf(*site));
        return true;
    });
    RELEASE_ASSERT(numberRemoved == numberOfFinalCalls);
}

template<typename T>
static void mergeByOffset(Vector<T>& ascending, const Vector<T>& added)
{
    size_t middle = ascending.size();
    ascending.appendVector(added);
    std::ranges::inplace_merge(ascending, ascending.begin() + middle, { }, &T::offset);
}

static void moveOffsets(CompiledCode& code, const Vector<OutlinedSite>& sites, const Vector<OutlinedBody>& bodies, const OriginalCode& original, RewriteCheck check, const RewrittenCode& rewritten)
{
    auto& body = code.body;
    removeFinalCalls(body.relocations, sites, bodies);

    std::span<const uint8_t> newBytes = rewritten.bytes.span();
    SearchPositions positions;
    forEachCodeOffset(code, [&](uint32_t& offset, CodeOffsetRole role, unsigned size) {
        uint32_t oldOffset = offset;
        offset = mapped(rewritten.shifts, oldOffset, positions.shift);
        if (check == RewriteCheck::Skip)
            return;
        switch (role) {
        case CodeOffsetRole::Call:
            RELEASE_ASSERT(!findSiteOverlapping(sites, oldOffset, oldOffset + size - 1, positions.site));
            RELEASE_ASSERT(isUnmoved(original, newBytes, sites, oldOffset, offset, RelocationKind::Call, positions));
            return;
        case CodeOffsetRole::Site:
            RELEASE_ASSERT(!findSiteOverlapping(sites, oldOffset, oldOffset + std::max(size, 1u) - 1, positions.site));
            RELEASE_ASSERT(isUnmoved(original, newBytes, sites, oldOffset, offset, std::nullopt, positions));
            return;
        case CodeOffsetRole::Target:
            RELEASE_ASSERT(canBeMapped(sites, oldOffset, positions.site));
            RELEASE_ASSERT(isUnmoved(original, newBytes, sites, oldOffset, offset, std::nullopt, positions));
            return;
        case CodeOffsetRole::ReturnAddress:
        case CodeOffsetRole::Boundary:
            RELEASE_ASSERT(canBeMapped(sites, oldOffset, positions.site));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });

    Vector<Relocation> calls;
    Vector<ReturnSite> returnSites;
    calls.reserveInitialCapacity(sites.size());
    for (size_t i = 0; i < sites.size(); ++i) {
        uint32_t callOffset = rewritten.callOffsets[i];
        calls.append(Relocation { callOffset, RelocationKind::Call, SymbolSpace::OutlinedBody, primaryEntry, sites[i].body });
        if (!bodies[sites[i].body].endsInCall)
            returnSites.append(ReturnSite { callOffset + static_cast<uint32_t>(sizeOfNearCall), { } });
    }
    mergeByOffset(body.relocations, calls);
    mergeByOffset(body.returnSites, returnSites);
}

static bool areInFrameRanges(const Vector<OutlinedSite>& sites, const FrameDescription& frame)
{
    auto* range = frame.frameRanges.begin();
    for (auto& site : sites) {
        while (range != frame.frameRanges.end() && range->end <= site.offset)
            ++range;
        if (range == frame.frameRanges.end() || site.offset < range->begin || endOf(site) > range->end)
            return false;
    }
    return true;
}

static void rewriteFunction(CompiledCode& code, uint32_t position, const OutliningRewriteInputs& inputs, const Vector<uint32_t, 0>& aliasOffsets, RewriteCheck check, OutlinedCode& result, RetainedRewrite& retained)
{
    auto& body = code.body;
    auto& outlining = inputs.outlining;
    auto& sites = outlining.sites[position];
    RELEASE_ASSERT(!isARM64() || (areInFrameRanges(sites, code.frame) && (aliasOffsets.isEmpty() || endOf(sites.last()) <= aliasOffsets.first())));
    RewrittenCode rewritten = copyAroundSites(body, position, outlining, aliasOffsets, inputs.templates);
    PackedCodeUnits units = std::exchange(body.units, { });
    OriginalCode original { WTF::move(body.bytes), check == RewriteCheck::Run ? units.unpack() : Vector<CodeUnit>(), WTF::move(body.localSites), rewritten.callOffsets };
    relinkLocalSites(original, sites, outlining.bodies, check, rewritten);
    moveOffsets(code, sites, outlining.bodies, original, check, rewritten);
    body.bytes = WTF::move(rewritten.bytes);
    result.paddingBeforeAliases[position] = WTF::move(rewritten.paddingBeforeAliases);
    if (!aliasOffsets.isEmpty())
        retained = { WTF::move(original), WTF::move(rewritten.shifts) };
}

static void moveAliases(Vector<EntryAlias>& aliases, const CodeIndex& index, const LinkedFunctions& functions, const Outlining& outlining, const Vector<RetainedRewrite>& retainedRewrites, RewriteCheck check)
{
    for (auto& alias : aliases) {
        if (alias.placement != AliasPlacement::AfterBody)
            continue;
        uint32_t position = *index.indexOf(alias.body);
        auto& [original, shifts] = retainedRewrites[position];
        if (shifts.isEmpty())
            continue;
        auto& sites = outlining.sites[position];
        auto& body = functions[position].body;
        SearchPositions positions;
        RELEASE_ASSERT(check == RewriteCheck::Skip || canBeMapped(sites, alias.start, positions.site));
        uint32_t oldStart = alias.start;
        alias.start = mapped(shifts, oldStart);
        RELEASE_ASSERT(check == RewriteCheck::Skip || isUnmoved(original, body.bytes.span(), sites, oldStart, alias.start, std::nullopt, positions));
        RELEASE_ASSERT(!((*alias.innerEntryOffset() - body.primaryEntryOffset()) % imageFunctionAlignment));
    }
}

static void releaseCodeUnits(LinkedFunctions& functions, BuildContext& context)
{
    Vector<std::pair<PackedCodeUnits, Vector<LinkedSite>>> released(functions.size());
    forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
        auto& body = functions[position].body;
        released[position] = { std::exchange(body.units, { }), std::exchange(body.localSites, { }) };
    });
    context.destroyLater(WTF::move(released));
}

OutlinedCode applyOutlining(LinkedFunctions& functions, Vector<EntryAlias>& aliases, const OutliningRewriteInputs& inputs, BuildContext& context)
{
    auto& outlining = inputs.outlining;
    OutlinedCode result;
    result.ownBodiesByCaller.grow(functions.size());
    result.paddingBeforeAliases.grow(functions.size());
    if (!outlining.bodies.isEmpty()) {
        RELEASE_ASSERT(outlining.sites.size() == functions.size());
        auto aliasOffsetsByBody = collectAliasOffsets(functions.size(), aliases, inputs.index);
        appendOutlinedBodies(result, functions, inputs, context.parallelLoops());
        Vector<RetainedRewrite> retainedRewrites(functions.size());
        RewriteCheck check = context.options().verifiesProgram ? RewriteCheck::Run : RewriteCheck::Skip;
        forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
            if (!outlining.sites[position].isEmpty())
                rewriteFunction(functions[position], position, inputs, aliasOffsetsByBody[position], check, result, retainedRewrites[position]);
        });
        moveAliases(aliases, inputs.index, functions, outlining, retainedRewrites, check);
        context.destroyLater(std::tuple { WTF::move(aliasOffsetsByBody), WTF::move(retainedRewrites) });
    }
    releaseCodeUnits(functions, context);
    return result;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

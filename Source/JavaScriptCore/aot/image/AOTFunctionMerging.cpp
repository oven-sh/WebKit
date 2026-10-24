/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCallSiteTable.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"

namespace JSC { namespace AOT {

using Typed::FunctionRole;

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

static void appendPinnedRange(auto& units, size_t begin, size_t end)
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
        body.units.shrinkCodeSize(safeCast<uint32_t>(size));
    } else if (size > body.bytes.size()) {
        size_t start = body.bytes.size();
        RELEASE_ASSERT(!((size - start) % templates.breakpoint.size()));
        while (body.bytes.size() < size)
            body.bytes.appendVector(templates.breakpoint);
        appendPinnedUnit(body, start);
    }
    body.paddingStart = safeCast<uint32_t>(size);
}

// Code without a frame that calls nothing, faults nowhere and is described by nothing but its bytes and the jumps in them.
static bool leavesOnlyByJumps(const CompiledCode& code)
{
    const CodeBody& body = code.body;
    const SiteDescription& sites = code.sites;
    if (code.frame.hasFrame() || !code.frame.prologueChecks.isEmpty())
        return false;
    if (body.relocations.isEmpty() || body.entries.size() != 1)
        return false;
    if (!body.returnSites.isEmpty() || !body.firstReadSites.isEmpty() || !body.constants.isEmpty() || !body.jumpTables.isEmpty())
        return false;
    if (sites.positions.size() != 1 || sites.numberOfSitesWithoutCode
        || !sites.inlineFrames.isEmpty() || !sites.flaggedSites.isEmpty() || !sites.handlers.isEmpty()
        || !sites.faultingAccesses.isEmpty() || !sites.pollSites.isEmpty() || !sites.frameMapWords.isEmpty())
        return false;
    return std::ranges::all_of(body.relocations, [](const Relocation& jump) {
        return jump.kind == RelocationKind::Jump && jump.space == SymbolSpace::Function && jump.entry == primaryEntry;
    });
}

static OptionalIndex findJumpTarget(const CompiledCode& code, uint32_t position, const CodeIndex& index)
{
    const CodeBody& body = code.body;
    if (!leavesOnlyByJumps(code) || body.relocations.size() != 1 || !body.localSites.isEmpty())
        return { };
    const Relocation& jump = body.relocations[0];
    if (jump.offset + sizeOfJump != body.bytes.size())
        return { };
    RELEASE_ASSERT(body.primaryEntryOffset() <= jump.offset);
    std::optional<uint32_t> callee = index.indexOf(body.callees[jump.symbol]);
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

static RemovedBodies createAliases(LinkedFunctions& functions, const Vector<OptionalIndex>& jumpsTo, const Vector<OptionalIndex>& chainEnd, const Vector<uint32_t>& uses, Vector<EntryAlias>& aliases)
{
    RemovedBodies result { BitVector(functions.size()), { }, { }, { } };
    BitVector hasPrependedHead(functions.size());
    OptionalIndex lastRetained;
    auto choosePlacement = [&](uint32_t position, uint32_t end) -> std::optional<AliasPlacement> {
        if (isOneJump(functions, jumpsTo, position))
            return AliasPlacement::AtPrimaryEntry;
        if (!hasPrependedHead.get(end) && functions[end].body.entries.size() == 1 && !functions[end].body.primaryEntryOffset())
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
        if (removed.frame.isProvenToFitOnStack && target.frame.isEnteredWithoutStackCheck)
            target.frame.isProvenToFitOnStack = true;
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
        Vector<CodeUnit> pinnedUnits;
        appendPinnedRange(pinnedUnits, 0, shift);
        body.units.prepend(pinnedUnits.span());
    }
    body.bytes.insertFill(0, 0, shift);
    memcpySpan(body.bytes.mutableSpan(), head);
    fillWithNops(body.bytes.mutableSubspan(head.size(), pad));
    alias.start = 0;
    alias.prefixSize = prefixSize;
}

static void appendBody(CodeBody& host, const CompiledCode& guest, FunctionID target, EntryAlias& alias, const CodeTemplates& templates)
{
    constexpr size_t alignment = imageFunctionAlignment;
    static_assert(alignment <= 16);
    size_t hostEnd = host.bytes.size();
    uint32_t guestEntry = guest.body.primaryEntryOffset();
    // The layout aligns the primary entry of the host, so the first byte of the host lies this far past a multiple of the alignment.
    size_t startMisalignment = (alignment - host.primaryEntryOffset() % alignment) % alignment;
    size_t start = WTF::roundUpToMultipleOf<alignment>(startMisalignment + hostEnd + guestEntry) - startMisalignment - guestEntry;
    RELEASE_ASSERT(!((start - hostEnd) % templates.breakpoint.size()));
    while (host.bytes.size() < start)
        host.bytes.appendVector(templates.breakpoint);
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

#if CPU(X86_64)
// The override for the data segment, which in 64-bit code changes no address.
static constexpr uint8_t prefixWithoutMeaning = 0x3e;
#else
static constexpr uint8_t prefixWithoutMeaning = 0;
#endif

// Tests in front of a body: where all of them pass, a jump to the body, which lies next.
static uint32_t endInJumpToNextBody(CompiledCode& code, FunctionID next)
{
    CodeBody& body = code.body;
    if (!leavesOnlyByJumps(code) || body.relocations.size() < 2 || body.paddingStart != body.bytes.size())
        return 0;
    const Relocation jump = body.relocations[0];
    if (body.callees[jump.symbol] != next)
        return 0;
    uint32_t endOfJump = safeCast<uint32_t>(jump.offset + sizeOfJump);
    uint32_t sizeBehindJump = safeCast<uint32_t>(body.bytes.size()) - endOfJump;
    // The entry of the entrance and the entry of the body are both aligned.
    uint32_t sizeOfPrefix = 0;
#if CPU(X86_64)
    static_assert(imageFunctionAlignment == 2);
    if (!body.primaryEntryOffset() && jump.offset % imageFunctionAlignment)
        sizeOfPrefix = 1;
#endif
    auto moved = [&](uint32_t offset) {
        return offset < endOfJump ? offset + sizeBehindJump + sizeOfPrefix : offset - endOfJump;
    };

    Vector<uint8_t> bytes;
    bytes.reserveInitialCapacity(body.bytes.size() + sizeOfPrefix);
    bytes.append(body.bytes.span().subspan(endOfJump));
    if (sizeOfPrefix)
        bytes.append(prefixWithoutMeaning);
    bytes.append(body.bytes.span().first(endOfJump));
    Vector<LinkedSite> localSites = body.localSites;
    for (LinkedSite& site : localSites) {
        unsigned size = MacroAssembler::relocationSize(site.kind);
        if (!size || site.kind == RelocationKind::Distance32 || site.target >= body.bytes.size())
            return 0;
        site.offset = moved(site.offset);
        site.target = moved(site.target);
        if (!MacroAssembler::setRelocationDistance(site.kind, bytes.mutableSpan().subspan(site.offset, size), static_cast<int64_t>(site.target) - static_cast<int64_t>(site.offset)))
            return 0;
    }
    AOT::sort(localSites, { }, &LinkedSite::offset);

    body.bytes = WTF::move(bytes);
    body.paddingStart = safeCast<uint32_t>(body.bytes.size());
    body.localSites = WTF::move(localSites);
    body.relocations.removeAt(0);
    for (Relocation& relocation : body.relocations)
        relocation.offset = moved(relocation.offset);
    body.relocations.append(jump);
    body.relocations.last().offset = moved(jump.offset);
    body.entries[0].offset += sizeBehindJump;
    body.units = { };
    return sizeBehindJump;
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
            appendBody(functions[host.position].body, functions[guest.position], functions[guest.target].function, aliases[guest.alias], templates);
    });
    if (!aliases.isEmpty()) {
        Vector<uint32_t> retainedPositions = findIndicesInParallel(loops, count, [&](unsigned position) {
            return !removed.isRemoved.get(position);
        });
        functions.reorder(retainedPositions.span(), context);
    }
    CodeIndex indexOfRetained(functions.span());
    BitVector isHost(functions.size());
    for (const EntryAlias& alias : aliases) {
        if (alias.placement == AliasPlacement::AfterBody)
            isHost.set(*indexOfRetained.indexOf(alias.body));
    }
    Vector<uint32_t> distanceMoved(FillWith { }, functions.size(), 0u);
    forEachIndexInParallel(loops, safeCast<unsigned>(functions.size()), [&](unsigned position) {
        if (position + 1 < functions.size() && !isHost.get(position))
            distanceMoved[position] = endInJumpToNextBody(functions[position], functions[position + 1].function);
    });
    for (EntryAlias& alias : aliases) {
        if (alias.placement == AliasPlacement::BeforeBody)
            alias.start += distanceMoved[*indexOfRetained.indexOf(alias.body)];
    }
    context.statistics().count(StatisticID::JumpOnlyFunctionsRemoved, aliases.size());
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace {

using ComparisonKey = Vector<uint64_t, 8>;
using CalleeList = Vector<uint32_t, 2>;
using SiteCacheOrdinals = UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

struct AppendedEntry {
    friend auto operator<=>(const AppendedEntry&, const AppendedEntry&) = default;

    uint32_t start;
    uint32_t prefixSize;
    AliasPlacement placement;
};
using AppendedEntryList = Vector<AppendedEntry, 0>;

struct FoldingInputs {
    const LinkedFunctions& functions;
    const Vector<EntryAlias>& aliases;
    const Typed::Program& program;
    const CodeIndex& index;
    const AliasIndex& aliasIndex;
    const Vector<AppendedEntryList>& appendedEntriesOf;
};

struct FoldingCandidates {
    explicit FoldingCandidates(size_t numberOfBodies)
        : comparisonKeys(numberOfBodies)
        , codeHashes(FillWith { }, numberOfBodies, static_cast<uint64_t>(0))
        , isCandidate(FillWith { }, numberOfBodies, static_cast<uint8_t>(0))
        , candidateCallees(numberOfBodies)
    {
    }

    Vector<uint32_t> candidates;
    Vector<ComparisonKey> comparisonKeys;
    Vector<uint64_t> codeHashes;
    Vector<uint8_t> isCandidate;
    Vector<CalleeList> candidateCallees;
};

enum class CalleeWalkState : uint8_t {
    NotSeen,
    InProgress,
    ReachesNoCycle,
    ReachesCycle,
};

} // anonymous namespace

static void appendWords(ComparisonKey& key, std::initializer_list<uint64_t> words)
{
    key.appendList(words);
}

static constexpr uint64_t packWords(uint32_t high, uint32_t low)
{
    return static_cast<uint64_t>(high) << 32 | low;
}

static Vector<AppendedEntryList> collectAppendedEntries(const Vector<EntryAlias>& aliases, const CodeIndex& index, size_t numberOfBodies)
{
    Vector<AppendedEntryList> appendedEntriesOf(numberOfBodies);
    for (auto& alias : aliases) {
        if (alias.placement == AliasPlacement::AtPrimaryEntry)
            continue;
        auto body = index.indexOf(alias.body);
        RELEASE_ASSERT(body);
        appendedEntriesOf[*body].append({ alias.start, alias.prefixSize, alias.placement });
    }
    for (auto& appendedEntries : appendedEntriesOf)
        AOT::sort(appendedEntries);
    return appendedEntriesOf;
}

static bool hasVisibleSites(const CompiledCode& code)
{
    if (!code.sites.faultingAccesses.isEmpty() || !code.sites.pollSites.isEmpty() || !code.body.firstReadSites.isEmpty())
        return true;
    std::span<const Relocation> relocations = code.body.relocations.span();
    size_t next = 0;
    for (const ReturnSite& returnSite : code.body.returnSites) {
        if (!returnSite.site)
            continue;
        if (!code.entry.isGenericEntrypoint)
            return true;
        while (next < relocations.size() && relocations[next].offset + sizeOfNearCall < returnSite.offset)
            ++next;
        if (next == relocations.size() || relocations[next].offset + sizeOfNearCall != returnSite.offset)
            return true;
        if (relocations[next].kind != RelocationKind::Call || relocations[next].space != SymbolSpace::Function)
            return true;
    }
    return false;
}

static std::optional<uint32_t> resolveToBodyStart(FunctionID function, const FoldingInputs& inputs)
{
    if (auto body = inputs.index.indexOf(function))
        return body;
    OptionalIndex found = inputs.aliasIndex.aliasOf(function);
    if (!found || inputs.aliases[*found].placement != AliasPlacement::AtPrimaryEntry)
        return std::nullopt;
    return inputs.index.indexOf(inputs.aliases[*found].body);
}

static void appendSymbolWords(ComparisonKey& key, const CodeBody& body, const Relocation& relocation, SiteCacheOrdinals& siteCacheOrdinals)
{
    switch (relocation.space) {
    case SymbolSpace::Function:
        return;
    case SymbolSpace::Stub:
        key.append(body.stubKeys[relocation.symbol].bits());
        return;
    case SymbolSpace::ArgumentThunk:
        key.append(body.argumentMoves[relocation.symbol].size());
        key.appendVector(body.argumentMoves[relocation.symbol]);
        return;
    case SymbolSpace::Constant: {
        const ConstantData& constant = body.constants[relocation.symbol];
        std::span<const uint8_t> bytes = constant.bytes.span();
        appendWords(key, { constant.alignment, bytes.size() });
        while (!bytes.empty()) {
            size_t size = std::min(bytes.size(), sizeof(uint64_t));
            uint64_t word = 0;
            memcpy(&word, bytes.data(), size);
            key.append(word);
            bytes = bytes.subspan(size);
        }
        return;
    }
    case SymbolSpace::JumpTable:
        key.append(body.jumpTables[relocation.symbol].targets.size());
        key.appendVector(body.jumpTables[relocation.symbol].targets);
        return;
    case SymbolSpace::SiteCache:
        key.append(siteCacheOrdinals.add(relocation.symbol, siteCacheOrdinals.size()).iterator->value);
        return;
    case SymbolSpace::RegExp:
        key.append(relocation.symbol);
        return;
    case SymbolSpace::OutlinedBody:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The key holds all that two bodies of the same bytes have to agree in, but for the functions they call.
static bool tryDescribeCandidate(uint32_t position, const FoldingInputs& inputs, ComparisonKey& key, CalleeList& callees)
{
    const CompiledCode& code = inputs.functions[position];
    const CodeBody& body = code.body;
    const FrameDescription& frame = code.frame;
    const SiteDescription& sites = code.sites;
    if (!sites.handlers.isEmpty() || !sites.inlineFrames.isEmpty() || !sites.frameMapWords.isEmpty())
        return false;
    FunctionRole role = inputs.program.at(code.function).role;
    FunctionTraceRecord trace = Typed::functionTraceOf(inputs.program, code.function);
    bool isSharedBody = role == FunctionRole::OutlinedBody && trace.visibility() == TraceVisibility::Hidden;
    if (!isSharedBody && ((frame.hasFrame() && !code.entry.isGenericEntrypoint) || hasVisibleSites(code)))
        return false;
    for (const Relocation& relocation : body.relocations) {
        if (relocation.space != SymbolSpace::Function)
            continue;
        auto callee = resolveToBodyStart(body.callees[relocation.symbol], inputs);
        if (!callee) {
            callees.clear();
            return false;
        }
        callees.append(*callee);
    }

    // An alias gives two entrypoints one callee, and isEntrypointOf() compares the source function of an entrypoint with that of the frame it called.
    bool calleeCanAppearInTrace = code.entry.isGenericEntrypoint && std::ranges::any_of(callees, [&](uint32_t callee) {
        return inputs.functions[callee].frame.hasFrame() || hasVisibleSites(inputs.functions[callee]);
    });
    const AppendedEntryList& appendedEntries = inputs.appendedEntriesOf[position];
    key.append(calleeCanAppearInTrace ? static_cast<uint64_t>(trace.sourceFunctionIndex) + 1 : 0);
    appendWords(key, { !frame.hasFrame(), frame.spillsArgumentRegisters, isSharedBody, trace.rejectsOnThrow, frame.stackUsedAtCalls, frame.isEnteredWithoutStackCheck, frame.prologueChecks.size(), appendedEntries.size() });
    for (const PrologueCheck& check : frame.prologueChecks)
        key.append(packWords(check.callOffset, check.resumeOffset));
    for (auto& appendedEntry : appendedEntries)
        appendWords(key, { appendedEntry.start, appendedEntry.prefixSize, static_cast<uint64_t>(appendedEntry.placement) });
    if (frame.hasFrame() || isSharedBody) {
        appendWords(key, { sites.numberOfSitesWithoutCode, code.entry.numberOfParameters, static_cast<uint64_t>(role) });
        appendWords(key, { trace.implementedBuiltinPlusOne, trace.visibilityBits, trace.codeTypeBits, trace.isStrict, trace.isConstructor, trace.isAsync, trace.isNamedByOwner });
    }
    if (frame.hasFrame()) {
        appendWords(key, { frame.frameSizeInBytes, frame.hasFrameRecord, frame.calleeSaveRegisters.registerCount(), frame.frameRanges.size() });
        for (auto& entry : frame.calleeSaveRegisters)
            appendWords(key, { entry.reg().index(), static_cast<uint64_t>(entry.offset()) });
        for (const CodeRange& range : frame.frameRanges)
            key.append(packWords(range.begin, range.end));
    }
    if (isSharedBody) {
        key.append(sites.faultingAccesses.size());
        for (auto& access : sites.faultingAccesses) {
            appendWords(key, { access.codeOffset, access.callSite.bits, access.throwSite.size() });
            key.appendVector(access.throwSite);
        }
        key.append(sites.pollSites.size());
        for (auto& poll : sites.pollSites)
            appendWords(key, { poll.offset, poll.callerIsFrameless, poll.callSite.bits });
    }
    key.append(sites.flaggedSites.size());
    for (auto& flaggedSite : sites.flaggedSites)
        appendWords(key, { flaggedSite.site, flaggedSite.flags.toRaw() });

    SiteCacheOrdinals siteCacheOrdinals;
    appendWords(key, { body.relocations.size(), body.entries.size(), body.returnSites.size(), body.firstReadSites.size() });
    for (const Relocation& relocation : body.relocations) {
        key.append(packWords(relocation.offset, static_cast<uint32_t>(relocation.kind) << 24 | static_cast<uint32_t>(relocation.space) << 16 | relocation.entry));
        appendSymbolWords(key, body, relocation, siteCacheOrdinals);
    }
    for (const CodeEntry& entry : body.entries)
        key.append(packWords(entry.offset, entry.id));
    for (const ReturnSite& returnSite : body.returnSites)
        key.append(packWords(returnSite.offset, returnSite.site.bits));
    for (const FirstReadSite& firstReadSite : body.firstReadSites)
        key.append(packWords(firstReadSite.returnOffset, firstReadSite.site));
    return true;
}

static FoldingCandidates findCandidates(const FoldingInputs& inputs, const ParallelLoops& loops)
{
    const LinkedFunctions& functions = inputs.functions;
    FoldingCandidates result(functions.size());
    result.candidates = findIndicesInParallel(loops, safeCast<unsigned>(functions.size()), [&](unsigned position) {
        if (!tryDescribeCandidate(position, inputs, result.comparisonKeys[position], result.candidateCallees[position]))
            return false;
        result.isCandidate[position] = 1;
        std::span<const uint8_t> bytes = functions[position].body.bytes.span();
        result.codeHashes[position] = packWords(safeCast<uint32_t>(bytes.size()), StringHasher::computeHashAndMaskTop8Bits(bytes));
        return true;
    });
    return result;
}

// The members of the group that compare equal get the class of the first of them. Whether a class has changed.
static bool assignClasses(Vector<uint32_t>& equivalenceClasses, std::span<uint32_t> group, const ParallelLoops& loops, const auto& compare)
{
    parallelSort(loops, group, [&](uint32_t first, uint32_t second) {
        int order = compare(first, second);
        return order ? order < 0 : first < second;
    });
    Vector<uint8_t> isDuplicateOfPrevious(group.size());
    forEachIndexInParallel(loops, group.size(), [&](unsigned i) {
        isDuplicateOfPrevious[i] = i && !compare(group[i - 1], group[i]);
    });
    bool changed = false;
    uint32_t equivalenceClass = 0;
    for (size_t i = 0; i < group.size(); ++i) {
        if (!isDuplicateOfPrevious[i])
            equivalenceClass = group[i];
        changed |= std::exchange(equivalenceClasses[group[i]], equivalenceClass) != equivalenceClass;
    }
    return changed;
}

static Vector<uint32_t> computeInitialClasses(const LinkedFunctions& functions, FoldingCandidates& found, const ParallelLoops& loops)
{
    const Vector<ComparisonKey>& comparisonKeys = found.comparisonKeys;
    const Vector<uint64_t>& codeHashes = found.codeHashes;
    Vector<uint32_t> equivalenceClasses(functions.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    assignClasses(equivalenceClasses, found.candidates.mutableSpan(), loops, [&](uint32_t first, uint32_t second) {
        if (codeHashes[first] != codeHashes[second])
            return codeHashes[first] < codeHashes[second] ? -1 : 1;
        auto& a = functions[first].body;
        auto& b = functions[second].body;
        ASSERT(a.bytes.size() == b.bytes.size());
        if (int order = memcmp(a.bytes.span().data(), b.bytes.span().data(), a.bytes.size()))
            return order;
        auto order = std::lexicographical_compare_three_way(comparisonKeys[first].begin(), comparisonKeys[first].end(), comparisonKeys[second].begin(), comparisonKeys[second].end());
        return order < 0 ? -1 : order > 0;
    });
    return equivalenceClasses;
}

static std::pair<Vector<CalleeWalkState>, Vector<uint32_t>> computeCalleeHeights(const FoldingCandidates& found, size_t numberOfBodies)
{
    Vector<CalleeWalkState> states(FillWith { }, numberOfBodies, CalleeWalkState::NotSeen);
    Vector<uint32_t> heights(FillWith { }, numberOfBodies, 0u);
    Vector<std::pair<uint32_t, uint32_t>> inProgress;
    for (uint32_t root : found.candidates) {
        if (states[root] != CalleeWalkState::NotSeen)
            continue;
        states[root] = CalleeWalkState::InProgress;
        inProgress.append({ root, 0 });
        while (!inProgress.isEmpty()) {
            auto [position, nextCallee] = inProgress.last();
            auto& callees = found.candidateCallees[position];
            if (nextCallee < callees.size()) {
                ++inProgress.last().second;
                uint32_t callee = callees[nextCallee];
                if (found.isCandidate[callee] && states[callee] == CalleeWalkState::NotSeen) {
                    states[callee] = CalleeWalkState::InProgress;
                    inProgress.append({ callee, 0 });
                }
                continue;
            }
            bool reachesCycle = false;
            for (uint32_t callee : callees) {
                if (!found.isCandidate[callee])
                    continue;
                reachesCycle |= states[callee] != CalleeWalkState::ReachesNoCycle;
                heights[position] = std::max(heights[position], heights[callee] + 1);
            }
            states[position] = reachesCycle ? CalleeWalkState::ReachesCycle : CalleeWalkState::ReachesNoCycle;
            inProgress.removeLast();
        }
    }
    return { WTF::move(states), WTF::move(heights) };
}

static void refineClassesByCallees(Vector<uint32_t>& equivalenceClasses, FoldingCandidates& found, const ParallelLoops& loops)
{
    Vector<uint32_t>& candidates = found.candidates;
    const Vector<CalleeList>& candidateCallees = found.candidateCallees;
    auto compare = [&](uint32_t first, uint32_t second) {
        if (equivalenceClasses[first] != equivalenceClasses[second])
            return equivalenceClasses[first] < equivalenceClasses[second] ? -1 : 1;
        for (size_t i = 0; i < candidateCallees[first].size(); ++i) {
            uint32_t a = equivalenceClasses[candidateCallees[first][i]];
            uint32_t b = equivalenceClasses[candidateCallees[second][i]];
            if (a != b)
                return a < b ? -1 : 1;
        }
        return 0;
    };
    auto refine = [&](std::span<uint32_t> group) {
        return assignClasses(equivalenceClasses, group, loops, compare);
    };
    auto [states, heights] = computeCalleeHeights(found, equivalenceClasses.size());
    auto reachesCycle = [&](uint32_t position) {
        return states[position] == CalleeWalkState::ReachesCycle;
    };
    parallelSort(loops, candidates.mutableSpan(), [&](uint32_t first, uint32_t second) {
        if (reachesCycle(first) != reachesCycle(second))
            return reachesCycle(second);
        return heights[first] != heights[second] ? heights[first] < heights[second] : first < second;
    });
    size_t acyclicCandidateCount = std::ranges::partition_point(candidates, std::not_fn(reachesCycle)) - candidates.begin();
    for (size_t begin = 0; begin < acyclicCandidateCount;) {
        size_t end = begin + 1;
        while (end < acyclicCandidateCount && heights[candidates[end]] == heights[candidates[begin]])
            ++end;
        refine(candidates.mutableSpan().subspan(begin, end - begin));
        begin = end;
    }
    for (bool changed = acyclicCandidateCount < candidates.size(); changed;)
        changed = refine(candidates.mutableSpan().subspan(acyclicCandidateCount));
}

static Vector<FoldedFunction> removeFoldedBodies(LinkedFunctions& functions, Vector<EntryAlias>& aliases, const CodeIndex& index, const Vector<uint32_t>& equivalenceClasses, BuildContext& context)
{
    for (auto& alias : aliases) {
        auto body = index.indexOf(alias.body);
        RELEASE_ASSERT(body);
        alias.body = functions[equivalenceClasses[*body]].function;
    }
    size_t bytes = 0;
    Vector<FoldedFunction> foldedFunctions;
    Vector<uint32_t> retainedPositions;
    retainedPositions.reserveInitialCapacity(functions.size());
    for (uint32_t position = 0; position < functions.size(); ++position) {
        if (equivalenceClasses[position] == position) {
            retainedPositions.append(position);
            continue;
        }
        auto& folded = functions[position];
        auto& canonical = functions[equivalenceClasses[position]];
        canonical.frame.isProvenToFitOnStack |= folded.frame.isProvenToFitOnStack;
        bytes += folded.body.bytes.size();
        aliases.append({ .function = folded.function, .body = canonical.function, .entry = folded.entry });
        foldedFunctions.append({ folded.function, canonical.function, static_cast<uint32_t>(folded.body.bytes.size()) });
    }
    if (retainedPositions.size() < functions.size())
        functions.reorder(retainedPositions.span(), context);
    context.statistics().count(StatisticID::FunctionsFolded, foldedFunctions.size());
    context.statistics().count(StatisticID::BytesFolded, bytes);
    return foldedFunctions;
}

Vector<FoldedFunction> foldIdenticalFunctions(LinkedFunctions& functions, Vector<EntryAlias>& aliases, const Typed::Program& program, BuildContext& context)
{
    const ParallelLoops& loops = context.parallelLoops();
    CodeIndex index(functions.span());
    AliasIndex aliasIndex(aliases.span());
    Vector<AppendedEntryList> appendedEntriesOf = collectAppendedEntries(aliases, index, functions.size());
    FoldingCandidates found = findCandidates({ functions, aliases, program, index, aliasIndex, appendedEntriesOf }, loops);
    Vector<uint32_t> equivalenceClasses = computeInitialClasses(functions, found, loops);
    refineClassesByCallees(equivalenceClasses, found, loops);
    Vector<FoldedFunction> foldedFunctions = removeFoldedBodies(functions, aliases, index, equivalenceClasses, context);
    context.destroyLater(std::tuple { WTF::move(found), WTF::move(aliasIndex), WTF::move(appendedEntriesOf) });
    return foldedFunctions;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImageBuilder.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCodeIndex.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTThreads.h"
#include <algorithm>
#include <compare>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashMap.h>
#include <wtf/text/StringHasherInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

using Typed::FunctionRole;

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

struct TraceKind {
    bool isSharedBody() const { return role == FunctionRole::OutlinedBody && trace.visibility() == TraceVisibility::Hidden; }

    FunctionRole role;
    FunctionTraceRecord trace;
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

struct CalleeHeights {
    bool reachesCycle(uint32_t position) const { return states[position] == CalleeWalkState::ReachesCycle; }

    Vector<CalleeWalkState> states;
    Vector<uint32_t> heights;
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
        std::ranges::sort(appendedEntries);
    return appendedEntriesOf;
}

static TraceKind traceKindOf(const Typed::Program& program, FunctionID function)
{
    return { program.at(function).role, Typed::functionTraceOf(program, function) };
}

static bool hasUnfoldableRecords(const CompiledCode& code)
{
    return !code.sites.handlers.isEmpty() || !code.sites.inlineFrames.isEmpty() || !code.sites.spreadSites.isEmpty() || !code.sites.frameMapWords.isEmpty();
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

static bool canAppearInTrace(const CompiledCode& code)
{
    return code.frame.hasFrame() || hasVisibleSites(code);
}

static std::optional<uint32_t> resolveToBodyStart(FunctionID function, const FoldingInputs& inputs)
{
    if (auto body = inputs.index.indexOf(function))
        return body;
    OptionalIndex found = inputs.aliasIndex.aliasOf(function);
    if (!found)
        return std::nullopt;
    const EntryAlias& alias = inputs.aliases[*found];
    if (alias.placement != AliasPlacement::AtPrimaryEntry)
        return std::nullopt;
    return inputs.index.indexOf(alias.body);
}

static bool collectCallees(CalleeList& callees, const CompiledCode& code, const FoldingInputs& inputs)
{
    for (const Relocation& relocation : code.body.relocations) {
        if (relocation.space != SymbolSpace::Function)
            continue;
        auto callee = resolveToBodyStart(code.body.callees[relocation.symbol], inputs);
        if (!callee) {
            callees.clear();
            return false;
        }
        callees.append(*callee);
    }
    return true;
}

// An alias gives two entrypoints one callee, and isEntrypointOf() compares the source function of an entrypoint with that of the frame it called.
static uint64_t sourceFunctionWord(const CompiledCode& code, const TraceKind& kind, const CalleeList& callees, const LinkedFunctions& functions)
{
    if (!code.entry.isGenericEntrypoint)
        return 0;
    bool calleeCanAppearInTrace = std::ranges::any_of(callees, [&](uint32_t callee) {
        return canAppearInTrace(functions[callee]);
    });
    return calleeCanAppearInTrace ? static_cast<uint64_t>(kind.trace.sourceFunctionIndex) + 1 : 0;
}

static void appendTraceWords(ComparisonKey& key, const CompiledCode& code, const TraceKind& kind)
{
    auto& trace = kind.trace;
    appendWords(key, { code.sites.numberOfSitesWithoutCode, code.entry.numberOfParameters, static_cast<uint64_t>(kind.role) });
    appendWords(key, { trace.implementedBuiltinPlusOne, trace.visibilityBits, trace.codeTypeBits, trace.isStrict, trace.isConstructor, trace.isAsync, trace.isNamedByOwner });
}

static void appendFrameWords(ComparisonKey& key, const FrameDescription& frame)
{
    appendWords(key, { frame.frameSizeInBytes, frame.calleeSaveRegisters.registerCount(), frame.frameRanges.size() });
    for (auto& entry : frame.calleeSaveRegisters)
        appendWords(key, { entry.reg().index(), static_cast<uint64_t>(entry.offset()) });
    for (const CodeRange& range : frame.frameRanges)
        key.append(packWords(range.begin, range.end));
}

static void appendSharedBodyWords(ComparisonKey& key, const CompiledCode& code)
{
    key.append(code.sites.faultingAccesses.size());
    for (auto& access : code.sites.faultingAccesses) {
        appendWords(key, { access.codeOffset, access.callSite.bits, access.throwSite.size() });
        for (uint32_t word : access.throwSite)
            key.append(word);
    }
    key.append(code.sites.pollSites.size());
    for (auto& poll : code.sites.pollSites)
        appendWords(key, { poll.offset, poll.callerIsFrameless, poll.callSite.bits });
}

static void appendSiteWords(ComparisonKey& key, const SiteDescription& sites)
{
    key.append(sites.flaggedSites.size());
    for (auto& flaggedSite : sites.flaggedSites)
        appendWords(key, { flaggedSite.site, flaggedSite.flags.toRaw() });
}

static void appendConstantWords(ComparisonKey& key, const ConstantData& constant)
{
    std::span<const uint8_t> bytes = constant.bytes.span();
    appendWords(key, { constant.alignment, bytes.size() });
    while (!bytes.empty()) {
        size_t size = std::min(bytes.size(), sizeof(uint64_t));
        uint64_t word = 0;
        memcpy(&word, bytes.data(), size);
        key.append(word);
        bytes = bytes.subspan(size);
    }
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
    case SymbolSpace::Constant:
        appendConstantWords(key, body.constants[relocation.symbol]);
        return;
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

static void appendLinkWords(ComparisonKey& key, const CodeBody& body)
{
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
}

static void appendComparisonKey(ComparisonKey& key, const CompiledCode& code, const TraceKind& kind, const AppendedEntryList& appendedEntries)
{
    auto& frame = code.frame;
    appendWords(key, { !frame.hasFrame(), kind.isSharedBody(), kind.trace.rejectsOnThrow, frame.stackUsedAtCalls, frame.isEnteredWithoutStackCheck, code.entry.hasValueEntry, frame.prologueChecks.size(), appendedEntries.size() });
    for (const PrologueCheck& check : frame.prologueChecks)
        key.append(packWords(check.callOffset, check.resumeOffset));
    for (auto& appendedEntry : appendedEntries)
        appendWords(key, { appendedEntry.start, appendedEntry.prefixSize, static_cast<uint64_t>(appendedEntry.placement) });
    if (frame.hasFrame() || kind.isSharedBody())
        appendTraceWords(key, code, kind);
    if (frame.hasFrame())
        appendFrameWords(key, frame);
    if (kind.isSharedBody())
        appendSharedBodyWords(key, code);
    appendSiteWords(key, code.sites);
    appendLinkWords(key, code.body);
}

static bool tryDescribeCandidate(uint32_t position, const FoldingInputs& inputs, ComparisonKey& key, CalleeList& callees)
{
    const CompiledCode& code = inputs.functions[position];
    if (hasUnfoldableRecords(code))
        return false;
    TraceKind kind = traceKindOf(inputs.program, code.function);
    if (!kind.isSharedBody() && ((code.frame.hasFrame() && !code.entry.isGenericEntrypoint) || hasVisibleSites(code)))
        return false;
    if (!collectCallees(callees, code, inputs))
        return false;
    key.append(sourceFunctionWord(code, kind, callees, inputs.functions));
    appendComparisonKey(key, code, kind, inputs.appendedEntriesOf[position]);
    return true;
}

static uint64_t computeCodeHash(const CodeBody& body)
{
    return packWords(safeCast<uint32_t>(body.bytes.size()), StringHasher::computeHashAndMaskTop8Bits(body.bytes.span()));
}

static FoldingCandidates findCandidates(const FoldingInputs& inputs, const ParallelLoops& loops)
{
    const LinkedFunctions& functions = inputs.functions;
    FoldingCandidates result(functions.size());
    result.candidates = findIndicesInParallel(loops, safeCast<unsigned>(functions.size()), [&](unsigned position) {
        if (!tryDescribeCandidate(position, inputs, result.comparisonKeys[position], result.candidateCallees[position]))
            return false;
        result.isCandidate[position] = 1;
        result.codeHashes[position] = computeCodeHash(functions[position].body);
        return true;
    });
    return result;
}

static Vector<uint32_t> computeInitialClasses(const LinkedFunctions& functions, FoldingCandidates& found, const ParallelLoops& loops)
{
    Vector<uint32_t>& candidates = found.candidates;
    const Vector<ComparisonKey>& comparisonKeys = found.comparisonKeys;
    const Vector<uint64_t>& codeHashes = found.codeHashes;
    auto compareWithoutCallees = [&](uint32_t first, uint32_t second) {
        if (codeHashes[first] != codeHashes[second])
            return codeHashes[first] < codeHashes[second] ? -1 : 1;
        auto& a = functions[first].body;
        auto& b = functions[second].body;
        ASSERT(a.bytes.size() == b.bytes.size());
        if (int order = memcmp(a.bytes.span().data(), b.bytes.span().data(), a.bytes.size()))
            return order;
        auto order = std::lexicographical_compare_three_way(comparisonKeys[first].begin(), comparisonKeys[first].end(), comparisonKeys[second].begin(), comparisonKeys[second].end());
        return order < 0 ? -1 : order > 0;
    };
    Vector<uint32_t> equivalenceClasses(functions.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    parallelSort(loops, candidates.mutableSpan(), [&](uint32_t first, uint32_t second) {
        int order = compareWithoutCallees(first, second);
        return order ? order < 0 : first < second;
    });
    Vector<uint8_t> isDuplicateOfPrevious(candidates.size());
    forEachIndexInParallel(loops, candidates.size(), [&](unsigned i) {
        isDuplicateOfPrevious[i] = i && !compareWithoutCallees(candidates[i - 1], candidates[i]);
    });
    for (size_t i = 1; i < candidates.size(); ++i) {
        if (isDuplicateOfPrevious[i])
            equivalenceClasses[candidates[i]] = equivalenceClasses[candidates[i - 1]];
    }
    return equivalenceClasses;
}

static CalleeHeights computeCalleeHeights(const FoldingCandidates& found, size_t numberOfBodies)
{
    CalleeHeights result {
        Vector<CalleeWalkState>(FillWith { }, numberOfBodies, CalleeWalkState::NotSeen),
        Vector<uint32_t>(FillWith { }, numberOfBodies, 0u),
    };
    Vector<CalleeWalkState>& states = result.states;
    Vector<uint32_t>& heights = result.heights;
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
    return result;
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
    Vector<uint8_t> isDuplicateOfPrevious(candidates.size());
    Vector<uint32_t> nextClasses;
    auto refine = [&](std::span<uint32_t> group) {
        parallelSort(loops, group, [&](uint32_t first, uint32_t second) {
            int order = compare(first, second);
            return order ? order < 0 : first < second;
        });
        nextClasses.resize(group.size());
        forEachIndexInParallel(loops, group.size(), [&](unsigned i) {
            isDuplicateOfPrevious[i] = i && !compare(group[i - 1], group[i]);
        });
        bool changed = false;
        for (size_t i = 0; i < group.size(); ++i) {
            nextClasses[i] = isDuplicateOfPrevious[i] ? nextClasses[i - 1] : group[i];
            changed |= nextClasses[i] != equivalenceClasses[group[i]];
        }
        for (size_t i = 0; i < group.size(); ++i)
            equivalenceClasses[group[i]] = nextClasses[i];
        return changed;
    };
    CalleeHeights calleeHeights = computeCalleeHeights(found, equivalenceClasses.size());
    const Vector<uint32_t>& heights = calleeHeights.heights;
    parallelSort(loops, candidates.mutableSpan(), [&](uint32_t first, uint32_t second) {
        if (calleeHeights.reachesCycle(first) != calleeHeights.reachesCycle(second))
            return calleeHeights.reachesCycle(second);
        return heights[first] != heights[second] ? heights[first] < heights[second] : first < second;
    });
    size_t acyclicCandidateCount = std::ranges::partition_point(candidates, [&](uint32_t candidate) {
        return !calleeHeights.reachesCycle(candidate);
    }) - candidates.begin();
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)

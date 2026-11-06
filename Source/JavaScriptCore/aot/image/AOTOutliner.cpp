/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOutliner.h"

#if ENABLE(AOT)

#include "AOTAscendingSearch.h"
#include "AOTBuildContext.h"
#include "AOTPhaseTimes.h"

namespace JSC { namespace AOT {

namespace {

AOT_THRESHOLD(unsigned, shortestWindow, 2);
constexpr unsigned longestWindow = 24;
AOT_THRESHOLD(int64_t, minSavings, 64);
// A site pays a call and a return each time it runs. Where it may run, that has to buy at least this many bytes,
AOT_THRESHOLD(uint32_t, minBytesSavedAtSiteThatMayRun, 3);
// and its piece has to stay in the caches because many sites call it, or save this many bytes a site.
AOT_THRESHOLD(size_t, minSitesOfSmallPieceThatMayRun, 512);
AOT_THRESHOLD(uint32_t, minBytesSavedByPieceWithFewSites, 16);
constexpr unsigned numberOfOutlinerShards = 256;
constexpr unsigned unitsInChunk = 1 << 16;
constexpr unsigned unitsInBlock = 64;
constexpr uint64_t hashBase = 0x9e3779b97f4a7c15ULL;

constexpr auto hashBasePowers = [] {
    std::array<uint64_t, longestWindow + 1> powers { 1 };
    for (unsigned length = 1; length <= longestWindow; ++length)
        powers[length] = powers[length - 1] * hashBase;
    return powers;
}();

using ShardStarts = std::array<uint32_t, numberOfOutlinerShards + 1>;

enum : uint8_t {
    MayBeInWindow = 1,
    IsTarget = 2,
    IsCall = 4,
    IsTaken = 8,
    MayRun = 16,
};

ALWAYS_INLINE uint64_t mix(uint64_t value)
{
    value ^= value >> 32;
    value *= 0xd6e8feb86659fd93ULL;
    value ^= value >> 32;
    value *= 0xd6e8feb86659fd93ULL;
    value ^= value >> 32;
    return value;
}

uint64_t hashForOutlining(std::span<const uint8_t> bytes)
{
    uint64_t result = bytes.size();
    for (uint8_t byte : bytes)
        result = (result ^ byte) * 0x100000001b3ULL;
    return mix(result);
}

uint64_t hashForOutlining(const CodeBody& body, const Relocation& call, uint32_t identity)
{
    uint64_t result = mix(static_cast<uint64_t>(call.space) << 16 | call.entry);
    result = mix(result ^ identity);
    switch (call.space) {
    case SymbolSpace::Function:
        return result;
    case SymbolSpace::Stub:
        return mix(result ^ body.stubKeys[call.symbol].bits());
    case SymbolSpace::ArgumentThunk:
        for (uint64_t word : body.argumentMoves[call.symbol])
            result = mix(result ^ word);
        return result;
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

template<typename T>
const T* advanceTo(std::span<const T>& ascending, uint32_t T::* offset, uint32_t bound)
{
    while (!ascending.empty() && ascending.front().*offset < bound)
        ascending = ascending.subspan(1);
    return ascending.empty() ? nullptr : &ascending.front();
}

struct Window {
    uint64_t hash;
    uint32_t unit;
    uint32_t function;
};
static_assert(sizeof(Window) == 16);

struct ChosenWindow {
    uint32_t unit;
    uint32_t function;
    uint32_t window;
};

constexpr uint32_t isChosenWindow = 1u << 31;
constexpr uint32_t windowMayRun = 1u << 30;
constexpr uint32_t functionOfWindow = windowMayRun - 1;

struct ChosenFamily {
    uint32_t begin;
    uint32_t end;
    uint32_t body;
    uint32_t sizeInBytes;
    uint32_t firstLocation;
    uint32_t firstUnit;
};

struct Family {
    int64_t saving;
    uint32_t begin;
    uint32_t end;
    uint32_t firstUnit;
    uint32_t sizeInBytes;
    bool endsInCall;
    bool allAreIdentical;
};

void sortByHashAndUnit(std::span<Window> windows, std::span<Window> scratch)
{
    constexpr unsigned numberOfParts = 256;
    auto isLess = [](const Window& a, const Window& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.unit < b.unit;
    };
    auto partOf = [](const Window& window) {
        return static_cast<unsigned>(window.hash >> 48 & (numberOfParts - 1));
    };
    if (windows.size() < 4 * numberOfParts) {
        AOT::sortInLine(windows, isLess);
        return;
    }
    std::array<uint32_t, numberOfParts + 1> starts { };
    for (auto& window : windows)
        ++starts[partOf(window) + 1];
    for (unsigned part = 0; part < numberOfParts; ++part)
        starts[part + 1] += starts[part];
    auto next = starts;
    for (auto& window : windows)
        scratch[next[partOf(window)]++] = window;
    for (unsigned part = 0; part < numberOfParts; ++part)
        AOT::sortInLine(scratch.subspan(starts[part], starts[part + 1] - starts[part]), isLess);
    memcpySpan(windows, scratch);
}

template<typename Functor>
void forEachChunkInParallel(const ParallelLoops& loops, size_t count, size_t sizeOfChunk, const Functor& functor)
{
    forEachIndexInParallel(loops, static_cast<unsigned>((count + sizeOfChunk - 1) / sizeOfChunk), [&](unsigned chunk) {
        size_t begin = static_cast<size_t>(chunk) * sizeOfChunk;
        functor(begin, std::min(count, begin + sizeOfChunk), chunk);
    });
}

void zeroOnPool(const ParallelLoops& loops, std::span<uint8_t> bytes)
{
    forEachChunkInParallel(loops, bytes.size(), MB, [&](size_t begin, size_t end, unsigned) {
        zeroSpan(bytes.subspan(begin, end - begin));
    });
}

template<typename T>
void copyOnPool(const ParallelLoops& loops, std::span<T> target, std::span<const T> source)
{
    static_assert(std::is_trivially_copyable_v<T>);
    constexpr size_t elementsInChunk = MB / sizeof(T);
    forEachIndexInParallel(loops, static_cast<unsigned>((source.size() + elementsInChunk - 1) / elementsInChunk), [&](unsigned chunk) {
        size_t start = static_cast<size_t>(chunk) * elementsInChunk;
        size_t size = std::min(elementsInChunk, source.size() - start);
        memcpySpan(target.subspan(start, size), source.subspan(start, size));
    });
}

template<typename T>
void fillWithZeroesOnPool(const ParallelLoops& loops, Vector<T>& vector, size_t size)
{
    static_assert(std::is_integral_v<T>);
#if ASAN_ENABLED
    UNUSED_PARAM(loops);
    vector.fill(0, size);
#else
    RELEASE_ASSERT(vector.isEmpty());
    vector.reserveCapacity(size);
    zeroOnPool(loops, unsafeMakeSpan(reinterpret_cast<uint8_t*>(vector.begin()), size * sizeof(T)));
    vector.grow(size);
#endif
}

class Chooser {
public:
    Chooser(const OutlinerInputs& inputs, BuildContext& context)
        : m_inputs(inputs)
        , m_functions(inputs.functions.span())
        , m_index(m_functions)
        , m_context(context)
    {
    }

    Outlining run()
    {
        PhaseTimes times(m_context, PassID::GenerateCode);
        gatherUnits();
        times.didFinish("outliner: gather units"_s);
        m_result.sites.resize(m_functions.size());
        if (m_numberOfUnits) {
            findLongestRepeatCandidates(findRuns());
            times.didFinish("outliner: find longest repeats"_s);
            for (unsigned length = longestWindow; length >= shortestWindow; --length)
                selectWindowsOf(length);
            times.didFinish("outliner: select windows"_s);
        }
        listSites();
        countStatistics();
        times.didFinish("outliner: list sites"_s);
        m_context.destroyLater(std::tuple { WTF::move(m_firstUnit), WTF::move(m_prefixHashes), WTF::move(m_offsets), WTF::move(m_lengths), WTF::move(m_flags), WTF::move(m_runs), WTF::move(m_longestRepeatCandidate), WTF::move(m_chosenLocations), WTF::move(m_active), WTF::move(m_stillActive), WTF::move(m_found), WTF::move(m_windows), WTF::move(m_chosenFamilies), WTF::move(m_takenUnits), WTF::move(m_functionOfBlock), WTF::move(m_code) });
        return WTF::move(m_result);
    }

private:
    // No two sites of a function begin at one offset, so the order in which the threads come by does not show.
    void listSites()
    {
        const ParallelLoops& loops = m_context.parallelLoops();
        auto forEachLocation = [&](const auto& functor) {
            forEachChunkInParallel(loops, m_chosenLocations.size(), 1 << 12, [&](size_t begin, size_t end, unsigned) {
                for (const ChosenLocation& location : m_chosenLocations.span().subspan(begin, end - begin))
                    functor(location);
            });
        };
        Vector<uint32_t> numberOfSites;
        fillWithZeroesOnPool(loops, numberOfSites, m_functions.size());
        forEachLocation([&](const ChosenLocation& location) {
            AtomicRef(numberOfSites[location.function]).fetch_add(1, std::memory_order_relaxed);
        });
        forEachIndexInParallel(loops, m_functions.size(), [&](unsigned function) {
            if (numberOfSites[function])
                m_result.sites[function] = Vector<OutlinedSite>(std::exchange(numberOfSites[function], 0));
        });
        forEachLocation([&](const ChosenLocation& location) {
            uint32_t position = AtomicRef(numberOfSites[location.function]).fetch_add(1, std::memory_order_relaxed);
            m_result.sites[location.function][position] = { m_offsets[location.firstUnit], location.sizeInBytes, location.body };
        });
        forEachIndexInParallel(loops, m_functions.size(), [&](unsigned function) {
            if (numberOfSites[function] > 1)
                AOT::sortInLine(m_result.sites[function], { }, &OutlinedSite::offset);
        });
    }

    void gatherUnits()
    {
        unsigned count = m_functions.size();
        RELEASE_ASSERT(count <= functionOfWindow);
        const ParallelLoops& loops = m_context.parallelLoops();
        size_t total = 0;
        m_firstUnit.grow(count + 1);
        forEachIndexInParallel(loops, count, [&](unsigned function) {
            m_firstUnit[function] = safeCast<uint32_t>(m_functions[function].body.units.size());
        });
        for (unsigned function = 0; function < count; ++function)
            total += std::exchange(m_firstUnit[function], safeCast<uint32_t>(total));
        m_firstUnit[count] = safeCast<uint32_t>(total);
        m_numberOfUnits = safeCast<uint32_t>(total);
        fillWithZeroesOnPool(loops, m_prefixHashes, total + 1);
        fillWithZeroesOnPool(loops, m_offsets, total);
        fillWithZeroesOnPool(loops, m_lengths, total);
        fillWithZeroesOnPool(loops, m_flags, total);
        fillWithZeroesOnPool(loops, m_runs, total);
        fillWithZeroesOnPool(loops, m_longestRepeatCandidate, total);
        fillWithZeroesOnPool(loops, m_takenUnits, total / 64 + 1);
        m_functionOfBlock.grow(total / unitsInBlock + 1);
        m_code.grow(count);
        Vector<Vector<uint32_t, 0>> aliasesByBody = groupAliasesByBody();
        forEachIndexInParallel(loops, count, [&](unsigned function) {
            m_code[function] = m_functions[function].body.bytes.span().data();
            for (size_t block = (static_cast<size_t>(m_firstUnit[function]) + unitsInBlock - 1) / unitsInBlock; block * unitsInBlock < m_firstUnit[function + 1]; ++block)
                m_functionOfBlock[block] = function;
            if (!m_functions[function].body.units.isEmpty())
                gatherUnitsOf(function, aliasesByBody[function].span());
        });
        computePrefixHashes();
    }

    Vector<Vector<uint32_t, 0>> groupAliasesByBody() const
    {
        Vector<Vector<uint32_t, 0>> aliasesByBody(m_functions.size());
        for (uint32_t alias = 0; alias < m_inputs.aliases.size(); ++alias) {
            if (m_inputs.aliases[alias].placement != AliasPlacement::AtPrimaryEntry)
                aliasesByBody[*m_index.indexOf(m_inputs.aliases[alias].body)].append(alias);
        }
        return aliasesByBody;
    }

    void computePrefixHashes()
    {
        size_t total = m_numberOfUnits;
        unsigned numberOfChunks = (total + unitsInChunk - 1) / unitsInChunk;
        forEachChunkInParallel(m_context.parallelLoops(), total, unitsInChunk, [&](size_t begin, size_t end, unsigned) {
            uint64_t sum = 0;
            for (size_t unit = begin; unit < end; ++unit) {
                sum = m_prefixHashes[unit + 1] + sum * hashBase;
                m_prefixHashes[unit + 1] = sum;
            }
        });
        uint64_t chunkPower = 1;
        for (unsigned unit = 0; unit < unitsInChunk; ++unit)
            chunkPower *= hashBase;
        Vector<uint64_t> chunkPrefixSums(numberOfChunks);
        uint64_t prefixSum = 0;
        for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk) {
            chunkPrefixSums[chunk] = prefixSum;
            if (chunk + 1 < numberOfChunks)
                prefixSum = m_prefixHashes[static_cast<size_t>(chunk + 1) * unitsInChunk] + prefixSum * chunkPower;
        }
        forEachChunkInParallel(m_context.parallelLoops(), total, unitsInChunk, [&](size_t begin, size_t end, unsigned chunk) {
            uint64_t carried = chunkPrefixSums[chunk];
            for (size_t unit = begin; unit < end; ++unit) {
                carried *= hashBase;
                m_prefixHashes[unit + 1] += carried;
            }
        });
    }

    std::optional<uint32_t> identityOf(unsigned function, const Relocation& call) const
    {
        if (call.space != SymbolSpace::Function)
            return 0;
        ResolvedCallee resolved = m_inputs.callees.at(function, call.symbol);
        if (resolved.alias) {
            const EntryAlias& alias = m_inputs.aliases[*resolved.alias];
            RELEASE_ASSERT(m_index.indexOf(alias.body) == *resolved.body);
            if (alias.placement != AliasPlacement::AtPrimaryEntry)
                return std::nullopt;
        }
        return *resolved.body + 1;
    }

    bool callsSameTarget(unsigned firstFunction, const Relocation& first, unsigned secondFunction, const Relocation& second) const
    {
        if (first.space != second.space || first.entry != second.entry || identityOf(firstFunction, first) != identityOf(secondFunction, second))
            return false;
        switch (first.space) {
        case SymbolSpace::Function:
            return true;
        case SymbolSpace::Stub:
            return m_functions[firstFunction].body.stubKeys[first.symbol] == m_functions[secondFunction].body.stubKeys[second.symbol];
        case SymbolSpace::ArgumentThunk:
            return m_functions[firstFunction].body.argumentMoves[first.symbol] == m_functions[secondFunction].body.argumentMoves[second.symbol];
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

    static bool canEndWindow(const CompiledCode&, const Relocation& call, bool hasFirstReadSite)
    {
        switch (call.space) {
        case SymbolSpace::Stub:
        case SymbolSpace::ArgumentThunk:
            return !isARM64() || !hasFirstReadSite;
        case SymbolSpace::Function:
            return !isARM64();
        case SymbolSpace::OutlinedBody:
        case SymbolSpace::Constant:
        case SymbolSpace::JumpTable:
        case SymbolSpace::SiteCache:
        case SymbolSpace::RegExp:
        case SymbolSpace::WebAssembly:
        case SymbolSpace::FFI:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void gatherUnitsOf(unsigned function, std::span<const uint32_t> aliasesInBody)
    {
        collectUnits(function);
        // Among the units of the function: upperBoundFrom(). The sites of a list ascend. A local site comes right in front of its target, which mostly lies near it.
        size_t positionOfLastSite = 0;
        forEachCodeOffset(m_functions[function], [&](uint32_t offset, CodeOffsetRole role, unsigned size) {
            switch (role) {
            case CodeOffsetRole::Site:
                pinUnitsOverlapping(function, offset, size, positionOfLastSite);
                break;
            case CodeOffsetRole::Target:
            case CodeOffsetRole::Boundary:
                markTarget(function, offset, positionOfLastSite);
                break;
            case CodeOffsetRole::Call:
            case CodeOffsetRole::ReturnAddress:
                break;
            }
        });
        for (uint32_t index : aliasesInBody) {
            const EntryAlias& alias = m_inputs.aliases[index];
            markTarget(function, alias.start, positionOfLastSite);
            markTarget(function, *alias.innerEntryOffset(), positionOfLastSite);
        }
    }

    void collectUnits(unsigned function)
    {
        auto& code = m_functions[function];
        auto& body = code.body;
        std::span<const Relocation> relocations = body.relocations.span();
        std::span<const FirstReadSite> firstReadSites = body.firstReadSites.span();
        std::span<const CodeRange> frameRanges = code.frame.frameRanges.span();
        auto canCallFrom = [&](uint32_t offset) {
            if (!isARM64())
                return true;
            auto* range = advanceTo(frameRanges, &CodeRange::end, offset + 1);
            return range && range->begin <= offset;
        };
        uint32_t unit = m_firstUnit[function];
        uint32_t end = 0;
        for (const CodeUnit& codeUnit : body.units) {
            RELEASE_ASSERT(codeUnit.offset == end && codeUnit.length);
            end += codeUnit.length;
            m_offsets[unit] = codeUnit.offset;
            m_lengths[unit] = std::min<uint32_t>(codeUnit.length, std::numeric_limits<uint8_t>::max());
            switch (codeUnit.kind) {
            case CodeUnit::Kind::Plain:
            case CodeUnit::Kind::PlainOffStackPointer:
                if (codeUnit.length > std::numeric_limits<uint8_t>::max() || !canCallFrom(codeUnit.offset))
                    break;
                m_flags[unit] = MayBeInWindow | (codeUnit.mayRun ? MayRun : 0);
                m_prefixHashes[unit + 1] = hashForOutlining(body.bytes.span().subspan(codeUnit.offset, codeUnit.length));
                break;
            case CodeUnit::Kind::Call: {
                auto* call = advanceTo(relocations, &Relocation::offset, codeUnit.offset);
                RELEASE_ASSERT(call && call->offset == codeUnit.offset && call->kind == RelocationKind::Call && codeUnit.length == sizeOfNearCall);
                auto* firstReadSite = advanceTo(firstReadSites, &FirstReadSite::returnOffset, end);
                if (!canCallFrom(codeUnit.offset) || !canEndWindow(code, *call, firstReadSite && firstReadSite->returnOffset == end))
                    break;
                if (auto identity = identityOf(function, *call)) {
                    m_flags[unit] = MayBeInWindow | IsCall | (codeUnit.mayRun ? MayRun : 0);
                    m_prefixHashes[unit + 1] = hashForOutlining(body, *call, *identity);
                }
                break;
            }
            case CodeUnit::Kind::Pinned:
            case CodeUnit::Kind::End:
                break;
            }
            ++unit;
        }
        RELEASE_ASSERT(unit == m_firstUnit[function + 1] && end == body.bytes.size());
    }

    std::optional<uint32_t> unitContaining(unsigned function, uint32_t byteOffset, size_t& position) const
    {
        if (byteOffset >= m_functions[function].body.bytes.size())
            return std::nullopt;
        auto offsets = m_offsets.span().subspan(m_firstUnit[function], m_firstUnit[function + 1] - m_firstUnit[function]);
        return m_firstUnit[function] + static_cast<uint32_t>(upperBoundFrom(position, offsets, byteOffset, std::less<uint32_t> { })) - 1;
    }

    void markTarget(unsigned function, uint32_t target, size_t positionToSearchFrom)
    {
        auto unit = unitContaining(function, target, positionToSearchFrom);
        if (!unit)
            return;
        if (m_offsets[*unit] == target)
            m_flags[*unit] |= IsTarget;
        else
            m_flags[*unit] &= ~MayBeInWindow;
    }

    void pinUnitsOverlapping(unsigned function, uint32_t offset, unsigned size, size_t& position)
    {
        auto first = unitContaining(function, offset, position);
        if (!first)
            return;
        uint32_t end = offset + std::max(size, 1u);
        for (uint32_t unit = *first; unit < m_firstUnit[function + 1] && m_offsets[unit] < end; ++unit)
            m_flags[unit] &= ~MayBeInWindow;
    }

    size_t findRuns()
    {
        std::atomic<size_t> windowStartCandidateCount { 0 };
        forEachIndexInParallel(m_context.parallelLoops(), m_functions.size(), [&](unsigned function) {
            uint32_t end = m_firstUnit[function + 1];
            size_t numberInFunction = 0;
            for (uint32_t unit = end; unit-- > m_firstUnit[function];) {
                uint8_t flags = m_flags[unit];
                if (!(flags & MayBeInWindow)) {
                    m_runs[unit] = 0;
                    continue;
                }
                bool isLast = unit + 1 == end || (flags & IsCall) || (m_flags[unit + 1] & IsTarget);
                m_runs[unit] = isLast ? 1 : std::min<unsigned>(longestWindow, 1 + m_runs[unit + 1]);
                numberInFunction += m_runs[unit] >= shortestWindow;
            }
            windowStartCandidateCount += numberInFunction;
        });
        return windowStartCandidateCount;
    }

    void findLongestRepeatCandidates(size_t numberOfCandidates)
    {
        const ParallelLoops& loops = m_context.parallelLoops();
        unsigned numberOfChunks = (m_numberOfUnits + unitsInChunk - 1) / unitsInChunk;
        Vector<Vector<uint32_t>> candidates(numberOfChunks);
        size_t largestSlotMask = roundUpToPowerOfTwo(std::max<size_t>(4 * numberOfCandidates, 1 << 16)) - 1;
        FastBitVector seen;
        FastBitVector seenTwice;
        seen.resize(largestSlotMask + 1);
        seenTwice.resize(largestSlotMask + 1);
        for (unsigned length = shortestWindow; length <= longestWindow; ++length) {
            size_t slotMask = std::min(largestSlotMask, roundUpToPowerOfTwo(std::max<size_t>(4 * numberOfCandidates, 1 << 16)) - 1);
            auto forEachCandidateIn = [&](unsigned chunk, const auto& functor) {
                if (length > shortestWindow) {
                    for (uint32_t unit : candidates[chunk])
                        functor(unit);
                    return;
                }
                uint32_t end = std::min<uint64_t>(m_numberOfUnits, static_cast<uint64_t>(chunk + 1) * unitsInChunk);
                for (uint32_t unit = chunk * unitsInChunk; unit < end; ++unit) {
                    if (m_runs[unit] >= length)
                        functor(unit);
                }
            };
            zeroOnPool(loops, asMutableByteSpan(seen.unsafeWords().words()).first((slotMask + 1) / 8));
            zeroOnPool(loops, asMutableByteSpan(seenTwice.unsafeWords().words()).first((slotMask + 1) / 8));
            forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
                forEachCandidateIn(chunk, [&](uint32_t unit) {
                    size_t slot = hashOf(unit, length) & slotMask;
                    if (!seen.atomicSetAndCheck(slot, true))
                        seenTwice.atomicSetAndCheck(slot, true);
                });
            });
            std::atomic<size_t> remainingCandidateCount { 0 };
            forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
                Vector<uint32_t> remaining;
                forEachCandidateIn(chunk, [&](uint32_t unit) {
                    if (!seenTwice.at(hashOf(unit, length) & slotMask))
                        return;
                    m_longestRepeatCandidate[unit] = length;
                    if (m_runs[unit] > length)
                        remaining.append(unit);
                });
                remainingCandidateCount += remaining.size();
                candidates[chunk] = WTF::move(remaining);
            });
            if (!remainingCandidateCount)
                break;
            numberOfCandidates = remainingCandidateCount;
        }
        Vector<std::array<Vector<uint32_t>, longestWindow + 1>> chunkUnits(numberOfChunks);
        forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
            uint32_t end = std::min<uint64_t>(m_numberOfUnits, static_cast<uint64_t>(chunk + 1) * unitsInChunk);
            for (uint32_t unit = chunk * unitsInChunk; unit < end; ++unit) {
                if (uint8_t length = m_longestRepeatCandidate[unit])
                    chunkUnits[chunk][length].append(unit);
            }
        });
        Vector<std::array<uint32_t, longestWindow + 1>> chunkStarts(numberOfChunks);
        std::array<uint32_t, longestWindow + 1> countByLength { };
        for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk) {
            chunkStarts[chunk] = countByLength;
            for (unsigned length = 0; length <= longestWindow; ++length)
                countByLength[length] += chunkUnits[chunk][length].size();
        }
        size_t repeatCandidateCount = 0;
        for (unsigned length = 0; length <= longestWindow; ++length) {
            m_unitsByLongest[length].grow(countByLength[length]);
            repeatCandidateCount += countByLength[length];
        }
        forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
            for (unsigned length = 0; length <= longestWindow; ++length) {
                auto units = chunkUnits[chunk][length].span();
                memcpySpan(m_unitsByLongest[length].mutableSpan().subspan(chunkStarts[chunk][length], units.size()), units);
            }
        });
        m_active.reserveCapacity(repeatCandidateCount);
        m_stillActive.reserveCapacity(repeatCandidateCount);
        m_found.reserveCapacity(repeatCandidateCount);
        m_windows.reserveCapacity(repeatCandidateCount);
        m_chosenLocations.reserveCapacity(repeatCandidateCount);
    }

    ALWAYS_INLINE uint64_t hashOf(uint32_t unit, unsigned length) const { return mix(m_prefixHashes[unit + length] - m_prefixHashes[unit] * hashBasePowers[length]); }
    ALWAYS_INLINE uint32_t sizeInBytesOf(uint32_t unit, unsigned length) const { return m_offsets[unit + length - 1] + m_lengths[unit + length - 1] - m_offsets[unit]; }
    // A taken window is at least this long, so one that overlaps this window covers its first or last unit.
    ALWAYS_INLINE bool isFree(uint32_t unit, unsigned length) const { return !((m_flags[unit] | m_flags[unit + length - 1]) & IsTaken); }
    // The flags get IsTaken behind the families of a length. Until then only these bits have it.
    ALWAYS_INLINE bool isTakenUnit(uint32_t unit) const { return m_takenUnits[unit / 64] >> (unit % 64) & 1; }
    ALWAYS_INLINE bool isStillFree(uint32_t unit, unsigned length) const { return !isTakenUnit(unit) && !isTakenUnit(unit + length - 1); }
    ALWAYS_INLINE void takeUnits(uint32_t unit, unsigned length)
    {
        static_assert(longestWindow < 64);
        uint64_t bits = (1ULL << length) - 1;
        m_takenUnits[unit / 64] |= bits << (unit % 64);
        if (unit % 64 + length > 64)
            m_takenUnits[unit / 64 + 1] |= bits >> (64 - unit % 64);
    }
    ALWAYS_INLINE bool savesTooLittleWhereItMayRun(uint32_t unit, unsigned length) const
    {
        return ((m_flags[unit] | m_flags[unit + length - 1]) & MayRun) && sizeInBytesOf(unit, length) < sizeOfNearCall + minBytesSavedAtSiteThatMayRun;
    }
    bool mayRun(uint32_t unit, unsigned length) const
    {
        return std::ranges::any_of(m_flags.span().subspan(unit, length), [](uint8_t flags) {
            return flags & MayRun;
        });
    }
    unsigned functionOf(uint32_t unit) const
    {
        unsigned function = m_functionOfBlock[unit / unitsInBlock];
        while (m_firstUnit[function + 1] <= unit)
            ++function;
        return function;
    }

    bool hasSingleCaller(std::span<const Window> sortedByUnit) const { return sortedByUnit.back().unit < m_firstUnit[functionOf(sortedByUnit.front().unit) + 1]; }
    uint64_t numberOfCopiesOf(OptionalIndex singleCaller) const { return singleCaller ? 1 : m_inputs.numberOfSharedCodeCopies; }

    int64_t savingsOf(uint64_t numberOfSites, uint64_t sizeInBytes, bool endsInCall, uint64_t numberOfCopies) const
    {
        if (numberOfSites < 2)
            return 0;
        int64_t returnSize = m_inputs.templates.returnToCaller.size();
        int64_t bodySize = static_cast<int64_t>(sizeInBytes) + (endsInCall ? 0 : returnSize);
        return static_cast<int64_t>(numberOfSites) * (static_cast<int64_t>(sizeInBytes) - static_cast<int64_t>(sizeOfNearCall)) - static_cast<int64_t>(numberOfCopies) * bodySize;
    }

    bool isIdentical(uint32_t first, unsigned firstFunction, uint32_t second, unsigned secondFunction, unsigned length) const
    {
        uint32_t sizeInBytes = sizeInBytesOf(first, length);
        if (sizeInBytesOf(second, length) != sizeInBytes)
            return false;
        bool endsInCall = m_flags[first + length - 1] & IsCall;
        if (endsInCall != !!(m_flags[second + length - 1] & IsCall))
            return false;
        if (memcmp(m_code[firstFunction] + m_offsets[first], m_code[secondFunction] + m_offsets[second], sizeInBytes))
            return false;
        if (!endsInCall)
            return true;
        auto& a = m_functions[firstFunction];
        auto& b = m_functions[secondFunction];
        auto callAt = [&](const CompiledCode& code, uint32_t offset) -> const Relocation& {
            auto found = std::ranges::lower_bound(code.body.relocations, offset, { }, &Relocation::offset);
            RELEASE_ASSERT(found != code.body.relocations.end() && found->offset == offset);
            return *found;
        };
        return callsSameTarget(firstFunction, callAt(a, m_offsets[first + length - 1]), secondFunction, callAt(b, m_offsets[second + length - 1]));
    }

    void selectWindowsOf(unsigned length)
    {
        size_t numberOfActiveBefore = m_active.size();
        m_active.grow(numberOfActiveBefore + m_unitsByLongest[length].size());
        copyOnPool(m_context.parallelLoops(), m_active.mutableSpan().subspan(numberOfActiveBefore), m_unitsByLongest[length].span());
        m_unitsByLongest[length].clear();
        if (m_active.isEmpty())
            return;
        ShardStarts shardStart = collectWindowsByShard(length);
        chooseFamilies(length, findFamilies(length, shardStart));
        removeTakenUnits();
    }

    unsigned numberOfActiveChunks() const { return (m_active.size() + unitsInChunk - 1) / unitsInChunk; }

    template<typename Functor>
    void forEachLocationIn(unsigned chunk, const Functor& functor) const
    {
        size_t end = std::min<size_t>(m_active.size(), static_cast<size_t>(chunk + 1) * unitsInChunk);
        for (size_t location = static_cast<size_t>(chunk) * unitsInChunk; location < end; ++location)
            functor(location);
    }

    ShardStarts collectWindowsByShard(unsigned length)
    {
        unsigned numberOfChunks = numberOfActiveChunks();
        m_found.resize(m_active.size());
        Vector<uint32_t> starts;
        starts.fill(0, static_cast<size_t>(numberOfChunks) * numberOfOutlinerShards);
        forEachIndexInParallel(m_context.parallelLoops(), numberOfChunks, [&](unsigned chunk) {
            forEachLocationIn(chunk, [&](size_t location) {
                uint32_t unit = m_active[location];
                if (!isFree(unit, length) || savesTooLittleWhereItMayRun(unit, length)) {
                    m_found[location] = { 0, std::numeric_limits<uint32_t>::max(), 0 };
                    return;
                }
                uint64_t hash = hashOf(unit, length);
                m_found[location] = { hash, unit, 0 };
                ++starts[static_cast<size_t>(chunk) * numberOfOutlinerShards + (hash >> 56)];
            });
        });
        ShardStarts shardStart;
        uint32_t total = 0;
        for (unsigned shard = 0; shard < numberOfOutlinerShards; ++shard) {
            shardStart[shard] = total;
            for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk) {
                uint32_t& start = starts[static_cast<size_t>(chunk) * numberOfOutlinerShards + shard];
                uint32_t number = start;
                start = total;
                total += number;
            }
        }
        shardStart[numberOfOutlinerShards] = total;
        m_windows.resize(total);
        forEachIndexInParallel(m_context.parallelLoops(), numberOfChunks, [&](unsigned chunk) {
            forEachLocationIn(chunk, [&](size_t location) {
                auto& window = m_found[location];
                if (window.unit != std::numeric_limits<uint32_t>::max())
                    m_windows[starts[static_cast<size_t>(chunk) * numberOfOutlinerShards + (window.hash >> 56)]++] = window;
            });
        });
        return shardStart;
    }

    ParallelArray<Family> findFamilies(unsigned length, const ShardStarts& shardStart)
    {
        Vector<Vector<Family>> shardFamilies(numberOfOutlinerShards);
        forEachIndexInParallel(m_context.parallelLoops(), numberOfOutlinerShards, [&](unsigned shard) {
            auto windows = m_windows.mutableSpan().subspan(shardStart[shard], shardStart[shard + 1] - shardStart[shard]);
            sortByHashAndUnit(windows, m_found.mutableSpan().subspan(shardStart[shard], windows.size()));
            for (size_t begin = 0; begin < windows.size();) {
                size_t end = begin + 1;
                while (end < windows.size() && windows[end].hash == windows[begin].hash)
                    ++end;
                uint64_t numberOfSites = 0;
                uint32_t afterLast = 0;
                for (size_t i = begin; i < end; ++i) {
                    if (numberOfSites && windows[i].unit < afterLast)
                        continue;
                    ++numberOfSites;
                    afterLast = windows[i].unit + length;
                }
                uint32_t first = windows[begin].unit;
                uint32_t sizeInBytes = sizeInBytesOf(first, length);
                bool endsInCall = m_flags[first + length - 1] & IsCall;
                int64_t saving = savingsOf(numberOfSites, sizeInBytes, endsInCall, 1);
                if (saving >= minSavings && !hasSingleCaller(windows.subspan(begin, end - begin)))
                    saving = savingsOf(numberOfSites, sizeInBytes, endsInCall, m_inputs.numberOfSharedCodeCopies);
                if (saving >= minSavings) {
                    bool allAreIdentical = true;
                    unsigned firstFunction = functionOf(first);
                    constexpr size_t unitPrefetchDistance = 16;
                    constexpr size_t codePrefetchDistance = 8;
                    auto prefetchUnit = [&](Window& window) {
                        window.function = functionOf(window.unit);
                        __builtin_prefetch(&m_code[window.function]);
                        __builtin_prefetch(&m_offsets[window.unit]);
                        __builtin_prefetch(&m_lengths[window.unit + length - 1]);
                        __builtin_prefetch(&m_flags[window.unit]);
                    };
                    auto prefetchCode = [&](const Window& window) {
                        __builtin_prefetch(m_code[window.function] + m_offsets[window.unit]);
                    };
                    for (size_t i = begin; i < std::min(end, begin + unitPrefetchDistance); ++i)
                        prefetchUnit(windows[i]);
                    for (size_t i = begin; i < std::min(end, begin + codePrefetchDistance); ++i)
                        prefetchCode(windows[i]);
                    for (size_t i = begin; i < end; ++i) {
                        if (end - i > unitPrefetchDistance)
                            prefetchUnit(windows[i + unitPrefetchDistance]);
                        if (end - i > codePrefetchDistance)
                            prefetchCode(windows[i + codePrefetchDistance]);
                        unsigned function = windows[i].function;
                        allAreIdentical = allAreIdentical && isIdentical(first, firstFunction, windows[i].unit, function, length);
                        windows[i].function = function | (mayRun(windows[i].unit, length) ? windowMayRun : 0);
                    }
                    shardFamilies[shard].append({ saving, static_cast<uint32_t>(shardStart[shard] + begin), static_cast<uint32_t>(shardStart[shard] + end), first, sizeInBytes, endsInCall, allAreIdentical });
                }
                begin = end;
            }
        });
        auto families = ParallelArray<Family>::concatenationOf(m_context.parallelLoops(), WTF::move(shardFamilies));
        parallelSort(m_context.parallelLoops(), families.mutableSpan(), [](const Family& a, const Family& b) {
            return a.saving != b.saving ? a.saving > b.saving : a.firstUnit < b.firstUnit;
        });
        return families;
    }

    void chooseFamilies(unsigned length, const ParallelArray<Family>& families)
    {
        Vector<ChosenWindow> chosen;
        m_chosenFamilies.shrink(0);
        uint32_t numberOfLocations = 0;
        for (size_t number = 0; number < families.size(); ++number) {
            constexpr uint32_t windowPrefetchDistance = 16;
            constexpr size_t familyWindowPrefetchDistance = 8;
            constexpr size_t familyFlagPrefetchDistance = 4;
            constexpr uint32_t windowsInCacheLine = 4;
            auto& family = families[number];
            if (number + familyWindowPrefetchDistance < families.size())
                __builtin_prefetch(&m_windows[families[number + familyWindowPrefetchDistance].begin]);
            if (number + familyFlagPrefetchDistance < families.size()) {
                auto& later = families[number + familyFlagPrefetchDistance];
                for (uint32_t i = later.begin; i < std::min(later.end, later.begin + windowsInCacheLine); ++i)
                    __builtin_prefetch(&m_takenUnits[m_windows[i].unit / 64]);
            }
            chosen.shrink(0);
            uint32_t afterLast = 0;
            for (uint32_t i = family.begin; i < family.end; ++i) {
                if (family.end - i > windowPrefetchDistance)
                    __builtin_prefetch(&m_takenUnits[m_windows[i + windowPrefetchDistance].unit / 64]);
                uint32_t unit = m_windows[i].unit;
                if ((!chosen.isEmpty() && unit < afterLast) || !isStillFree(unit, length))
                    continue;
                unsigned function = m_windows[i].function & functionOfWindow;
                if (!chosen.isEmpty() && !family.allAreIdentical && !isIdentical(chosen[0].unit, chosen[0].function, unit, function, length))
                    continue;
                chosen.append({ unit, function, i });
                afterLast = unit + length;
            }
            if (chosen.isEmpty())
                continue;
            bool knowsSize = family.allAreIdentical;
            if (chosen.size() < minSitesOfSmallPieceThatMayRun && (knowsSize ? family.sizeInBytes : sizeInBytesOf(chosen[0].unit, length)) < sizeOfNearCall + minBytesSavedByPieceWithFewSites) {
                chosen.removeAllMatching([&](const ChosenWindow& window) {
                    return !!(m_windows[window.window].function & windowMayRun);
                });
                if (chosen.isEmpty())
                    continue;
            }
            uint32_t first = chosen[0].unit;
            uint32_t sizeInBytes = knowsSize ? family.sizeInBytes : sizeInBytesOf(first, length);
            bool endsInCall = knowsSize ? family.endsInCall : m_flags[first + length - 1] & IsCall;
            OptionalIndex singleCaller;
            if (chosen.last().function == chosen[0].function)
                singleCaller = { chosen[0].function };
            int64_t saving = savingsOf(chosen.size(), sizeInBytes, endsInCall, numberOfCopiesOf(singleCaller));
            if (saving < minSavings)
                continue;
            uint32_t body = m_result.bodies.size();
            for (auto& [unit, function, window] : chosen) {
                takeUnits(unit, length);
                m_windows[window].function |= isChosenWindow;
            }
            m_chosenFamilies.append({ family.begin, family.end, body, sizeInBytes, std::exchange(numberOfLocations, numberOfLocations + static_cast<uint32_t>(chosen.size())), first });
            m_result.bodies.append({ chosen[0].function, 0, sizeInBytes, static_cast<uint32_t>(chosen.size()), singleCaller, endsInCall });
        }
        size_t numberOfLocationsBefore = m_chosenLocations.size();
        m_chosenLocations.grow(numberOfLocationsBefore + numberOfLocations);
        std::span<ChosenLocation> locations = m_chosenLocations.mutableSpan().subspan(numberOfLocationsBefore);
        forEachIndexInParallel(m_context.parallelLoops(), m_chosenFamilies.size(), [&](unsigned number) {
            const ChosenFamily& family = m_chosenFamilies[number];
            m_result.bodies[family.body].offset = m_offsets[family.firstUnit];
            uint32_t location = family.firstLocation;
            for (uint32_t i = family.begin; i < family.end; ++i) {
                const Window& window = m_windows[i];
                if (!(window.function & isChosenWindow))
                    continue;
                locations[location++] = { window.function & functionOfWindow, window.unit, family.sizeInBytes, family.body };
                for (unsigned unit = 0; unit < length; ++unit)
                    m_flags[window.unit + unit] |= IsTaken;
            }
        });
    }

    void removeTakenUnits()
    {
        unsigned numberOfChunks = numberOfActiveChunks();
        Vector<uint32_t> remainingStarts(numberOfChunks);
        forEachIndexInParallel(m_context.parallelLoops(), numberOfChunks, [&](unsigned chunk) {
            size_t start = static_cast<size_t>(chunk) * unitsInChunk;
            size_t end = start;
            forEachLocationIn(chunk, [&](size_t location) {
                uint32_t unit = m_active[location];
                if (!(m_flags[unit] & IsTaken))
                    m_active[end++] = unit;
            });
            remainingStarts[chunk] = end - start;
        });
        uint32_t remainingCount = 0;
        for (uint32_t& start : remainingStarts) {
            uint32_t number = start;
            start = remainingCount;
            remainingCount += number;
        }
        m_stillActive.resize(remainingCount);
        forEachIndexInParallel(m_context.parallelLoops(), numberOfChunks, [&](unsigned chunk) {
            uint32_t number = (chunk + 1 < numberOfChunks ? remainingStarts[chunk + 1] : remainingCount) - remainingStarts[chunk];
            memcpySpan(m_stillActive.mutableSpan().subspan(remainingStarts[chunk], number), m_active.span().subspan(static_cast<size_t>(chunk) * unitsInChunk, number));
        });
        m_active.swap(m_stillActive);
    }

    void countStatistics()
    {
        int64_t bytesOutlined = 0;
        for (auto& body : m_result.bodies)
            bytesOutlined += savingsOf(body.numberOfSites, body.length, body.endsInCall, numberOfCopiesOf(body.singleCaller));
        m_context.statistics().count(StatisticID::BodiesOutlined, m_result.bodies.size());
        m_context.statistics().count(StatisticID::BytesOutlined, bytesOutlined);
    }

    const OutlinerInputs& m_inputs;
    std::span<const CompiledCode> m_functions;
    CodeIndex m_index;
    BuildContext& m_context;
    Outlining m_result;
    Vector<uint32_t> m_firstUnit;
    Vector<uint64_t> m_prefixHashes;
    Vector<uint32_t> m_offsets;
    Vector<uint8_t> m_lengths;
    Vector<uint8_t> m_flags;
    Vector<uint8_t> m_runs;
    Vector<uint8_t> m_longestRepeatCandidate;
    Vector<uint64_t> m_takenUnits;
    Vector<uint32_t> m_functionOfBlock;
    Vector<const uint8_t*> m_code;
    std::array<Vector<uint32_t>, longestWindow + 1> m_unitsByLongest;
    struct ChosenLocation {
        uint32_t function;
        uint32_t firstUnit;
        uint32_t sizeInBytes;
        uint32_t body;
    };
    Vector<ChosenLocation> m_chosenLocations;
    Vector<ChosenFamily> m_chosenFamilies;
    Vector<uint32_t> m_active;
    Vector<uint32_t> m_stillActive;
    Vector<Window> m_found;
    Vector<Window> m_windows;
    uint32_t m_numberOfUnits { 0 };
};

} // anonymous namespace

Outlining chooseOutlinedSequences(const OutlinerInputs& inputs, BuildContext& context)
{
    return Chooser(inputs, context).run();
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

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

static Vector<Vector<uint32_t, 0>> collectAliasesBehindBodies(size_t numberOfFunctions, const Vector<EntryAlias>& aliases, const CodeIndex& index)
{
    Vector<Vector<uint32_t, 0>> aliasesByBody(numberOfFunctions);
    for (uint32_t alias = 0; alias < aliases.size(); ++alias) {
        if (aliases[alias].placement == AliasPlacement::AfterBody)
            aliasesByBody[*index.indexOf(aliases[alias].body)].append(alias);
    }
    return aliasesByBody;
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

    forEachChunkInParallel(loops, copies.size(), 256, [&](size_t begin, size_t end, unsigned) {
        for (Copy& copy : copies.mutableSpan().subspan(begin, end - begin)) {
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

static void rewriteFunction(CompiledCode& code, uint32_t position, const OutliningRewriteInputs& inputs, Vector<EntryAlias>& aliases, const Vector<uint32_t, 0>& aliasesBehindBody, RewriteCheck check, OutlinedCode& result)
{
    auto& body = code.body;
    auto& outlining = inputs.outlining;
    auto& sites = outlining.sites[position];
    Vector<uint32_t, 0> aliasOffsets = aliasesBehindBody.map<Vector<uint32_t, 0>>([&](uint32_t alias) {
        return aliases[alias].start;
    });
    AOT::sort(aliasOffsets);
    removeRepeatedElements(aliasOffsets);
    RELEASE_ASSERT(!isARM64() || (areInFrameRanges(sites, code.frame) && (aliasOffsets.isEmpty() || endOf(sites.last()) <= aliasOffsets.first())));
    RewrittenCode rewritten = copyAroundSites(body, position, outlining, aliasOffsets, inputs.templates);
    PackedCodeUnits units = std::exchange(body.units, { });
    OriginalCode original { WTF::move(body.bytes), check == RewriteCheck::Run ? units.unpack() : Vector<CodeUnit>(), WTF::move(body.localSites), rewritten.callOffsets };
    relinkLocalSites(original, sites, outlining.bodies, check, rewritten);
    moveOffsets(code, sites, outlining.bodies, original, check, rewritten);
    body.bytes = WTF::move(rewritten.bytes);
    result.paddingBeforeAliases[position] = WTF::move(rewritten.paddingBeforeAliases);
    for (uint32_t index : aliasesBehindBody) {
        EntryAlias& alias = aliases[index];
        SearchPositions positions;
        RELEASE_ASSERT(check == RewriteCheck::Skip || canBeMapped(sites, alias.start, positions.site));
        uint32_t oldStart = alias.start;
        alias.start = mapped(rewritten.shifts, oldStart);
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
        auto aliasesBehindBodies = collectAliasesBehindBodies(functions.size(), aliases, inputs.index);
        appendOutlinedBodies(result, functions, inputs, context.parallelLoops());
        RewriteCheck check = context.options().verifiesProgram ? RewriteCheck::Run : RewriteCheck::Skip;
        forEachIndexInParallel(context.parallelLoops(), functions.size(), [&](unsigned position) {
            if (!outlining.sites[position].isEmpty())
                rewriteFunction(functions[position], position, inputs, aliases, aliasesBehindBodies[position], check, result);
        });
        context.destroyLater(WTF::move(aliasesBehindBodies));
    }
    releaseCodeUnits(functions, context);
    return result;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

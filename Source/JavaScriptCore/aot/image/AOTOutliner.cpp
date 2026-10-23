/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOutliner.h"

#if ENABLE(AOT)

#include "AOTAscendingSearch.h"
#include "AOTBuildContext.h"
#include "AOTCodeUnits.h"
#include "AOTPhaseTimes.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/Atomics.h>
#include <wtf/FastBitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

namespace {

constexpr unsigned shortestWindow = 2;
constexpr unsigned longestWindow = 24;
constexpr int64_t minSavings = 64;
// A site pays a call and a return each time it runs. Where it may run, that has to buy at least this many bytes,
constexpr uint32_t minBytesSavedAtSiteThatMayRun = 3;
// and its piece has to stay in the caches because many sites call it, or save this many bytes a site.
constexpr size_t minSitesOfSmallPieceThatMayRun = 512;
constexpr uint32_t minBytesSavedByPieceWithFewSites = 16;
constexpr unsigned numberOfShards = 256;
constexpr unsigned unitsInChunk = 1 << 16;
constexpr unsigned unitsInBlock = 64;
constexpr uint64_t hashBase = 0x9e3779b97f4a7c15ULL;

constexpr auto hashBasePowers = [] {
    std::array<uint64_t, longestWindow + 1> powers { 1 };
    for (unsigned length = 1; length <= longestWindow; ++length)
        powers[length] = powers[length - 1] * hashBase;
    return powers;
}();

using ShardStarts = std::array<uint32_t, numberOfShards + 1>;

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

uint64_t computeHash(std::span<const uint8_t> bytes)
{
    uint64_t result = bytes.size();
    for (uint8_t byte : bytes)
        result = (result ^ byte) * 0x100000001b3ULL;
    return mix(result);
}

uint64_t computeHash(const CodeBody& body, const Relocation& call, uint32_t identity)
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
        AOT::sort(windows, isLess);
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
        AOT::sort(scratch.subspan(starts[part], starts[part + 1] - starts[part]), isLess);
    memcpySpan(windows, scratch);
}

void zeroOnPool(const ParallelLoops& loops, std::span<uint8_t> bytes)
{
    constexpr size_t bytesInChunk = MB;
    forEachIndexInParallel(loops, static_cast<unsigned>((bytes.size() + bytesInChunk - 1) / bytesInChunk), [&](unsigned chunk) {
        size_t start = static_cast<size_t>(chunk) * bytesInChunk;
        zeroSpan(bytes.subspan(start, std::min(bytesInChunk, bytes.size() - start)));
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
        constexpr unsigned locationsInChunk = 1 << 12;
        unsigned numberOfChunks = (m_chosenLocations.size() + locationsInChunk - 1) / locationsInChunk;
        auto forEachLocation = [&](const auto& functor) {
            forEachIndexInParallel(loops, numberOfChunks, [&](unsigned chunk) {
                size_t start = static_cast<size_t>(chunk) * locationsInChunk;
                for (const ChosenLocation& location : m_chosenLocations.span().subspan(start, std::min<size_t>(locationsInChunk, m_chosenLocations.size() - start)))
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
                AOT::sort(m_result.sites[function], { }, &OutlinedSite::offset);
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
        auto forEachUnitIn = [&](unsigned chunk, const auto& functor) {
            size_t end = std::min(total, static_cast<size_t>(chunk + 1) * unitsInChunk);
            for (size_t unit = static_cast<size_t>(chunk) * unitsInChunk; unit < end; ++unit)
                functor(unit);
        };
        forEachIndexInParallel(m_context.parallelLoops(), numberOfChunks, [&](unsigned chunk) {
            uint64_t sum = 0;
            forEachUnitIn(chunk, [&](size_t unit) {
                sum = m_prefixHashes[unit + 1] + sum * hashBase;
                m_prefixHashes[unit + 1] = sum;
            });
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
        forEachIndexInParallel(m_context.parallelLoops(), numberOfChunks, [&](unsigned chunk) {
            uint64_t carried = chunkPrefixSums[chunk];
            forEachUnitIn(chunk, [&](size_t unit) {
                carried *= hashBase;
                m_prefixHashes[unit + 1] += carried;
            });
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
                m_prefixHashes[unit + 1] = computeHash(body.bytes.span().subspan(codeUnit.offset, codeUnit.length));
                break;
            case CodeUnit::Kind::Call: {
                auto* call = advanceTo(relocations, &Relocation::offset, codeUnit.offset);
                RELEASE_ASSERT(call && call->offset == codeUnit.offset && call->kind == RelocationKind::Call && codeUnit.length == sizeOfNearCall);
                auto* firstReadSite = advanceTo(firstReadSites, &FirstReadSite::returnOffset, end);
                if (!canCallFrom(codeUnit.offset) || !canEndWindow(code, *call, firstReadSite && firstReadSite->returnOffset == end))
                    break;
                if (auto identity = identityOf(function, *call)) {
                    m_flags[unit] = MayBeInWindow | IsCall | (codeUnit.mayRun ? MayRun : 0);
                    m_prefixHashes[unit + 1] = computeHash(body, *call, *identity);
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
        starts.fill(0, static_cast<size_t>(numberOfChunks) * numberOfShards);
        forEachIndexInParallel(m_context.parallelLoops(), numberOfChunks, [&](unsigned chunk) {
            forEachLocationIn(chunk, [&](size_t location) {
                uint32_t unit = m_active[location];
                if (!isFree(unit, length) || savesTooLittleWhereItMayRun(unit, length)) {
                    m_found[location] = { 0, std::numeric_limits<uint32_t>::max(), 0 };
                    return;
                }
                uint64_t hash = hashOf(unit, length);
                m_found[location] = { hash, unit, 0 };
                ++starts[static_cast<size_t>(chunk) * numberOfShards + (hash >> 56)];
            });
        });
        ShardStarts shardStart;
        uint32_t total = 0;
        for (unsigned shard = 0; shard < numberOfShards; ++shard) {
            shardStart[shard] = total;
            for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk) {
                uint32_t& start = starts[static_cast<size_t>(chunk) * numberOfShards + shard];
                uint32_t number = start;
                start = total;
                total += number;
            }
        }
        shardStart[numberOfShards] = total;
        m_windows.resize(total);
        forEachIndexInParallel(m_context.parallelLoops(), numberOfChunks, [&](unsigned chunk) {
            forEachLocationIn(chunk, [&](size_t location) {
                auto& window = m_found[location];
                if (window.unit != std::numeric_limits<uint32_t>::max())
                    m_windows[starts[static_cast<size_t>(chunk) * numberOfShards + (window.hash >> 56)]++] = window;
            });
        });
        return shardStart;
    }

    Vector<Family> findFamilies(unsigned length, const ShardStarts& shardStart)
    {
        std::array<Vector<Family>, numberOfShards> shardFamilies;
        forEachIndexInParallel(m_context.parallelLoops(), numberOfShards, [&](unsigned shard) {
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
        Vector<Family> families;
        ShardStarts firstFamily;
        uint32_t numberOfFamilies = 0;
        for (unsigned shard = 0; shard < numberOfShards; ++shard)
            firstFamily[shard] = std::exchange(numberOfFamilies, static_cast<uint32_t>(numberOfFamilies + shardFamilies[shard].size()));
        families.grow(numberOfFamilies);
        forEachIndexInParallel(m_context.parallelLoops(), numberOfShards, [&](unsigned shard) {
            memcpySpan(families.mutableSpan().subspan(firstFamily[shard], shardFamilies[shard].size()), shardFamilies[shard].span());
        });
        parallelSort(m_context.parallelLoops(), families.mutableSpan(), [](const Family& a, const Family& b) {
            return a.saving != b.saving ? a.saving > b.saving : a.firstUnit < b.firstUnit;
        });
        return families;
    }

    void chooseFamilies(unsigned length, const Vector<Family>& families)
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

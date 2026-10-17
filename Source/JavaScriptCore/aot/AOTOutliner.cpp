/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOutliner.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTInstructionsInCode.h"
#include "AOTTypedThreads.h"
#include <tuple>
#include <wtf/DataLog.h>
#include <wtf/FastBitVector.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT {

namespace {

constexpr unsigned shortestWindow = 2;
constexpr unsigned longestWindow = 24;
constexpr int64_t minSavings = 64;
constexpr unsigned numberOfShards = 256;
constexpr unsigned unitsInChunk = 1 << 16;
constexpr uint64_t sumsBase = 0x9e3779b97f4a7c15ULL;
#if CPU(X86_64)
constexpr int64_t returnSize = 1;
#else
constexpr int64_t returnSize = sizeof(uint32_t);
#endif

enum : uint8_t {
    MayBeInWindow = 1,
    IsTarget = 2,
    IsCall = 4,
    IsTaken = 8,
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

uint64_t numberOf(std::span<const uint8_t> bytes)
{
    uint64_t result = bytes.size();
    for (uint8_t byte : bytes)
        result = (result ^ byte) * 0x100000001b3ULL;
    return mix(result);
}

uint64_t numberOf(const CompiledFunctionInfo& info, const StubCall& call)
{
    uint64_t result = mix(static_cast<uint64_t>(call.stub) << 32 | static_cast<uint64_t>(call.thunk) << 8 | 0xe8);
    result = mix(result ^ (static_cast<uint64_t>(call.firstOmittedGPR) << 8 | call.endOfOmittedGPRs));
    if (call.function != StubCall::noFunction) {
        auto& key = info.knownCallees[call.function];
        result = mix(result ^ (static_cast<uint64_t>(key.module) << 32 | key.start));
        result = mix(result ^ key.kind);
    }
    if (call.argumentMoves != StubCall::noArgumentMoves) {
        for (uint64_t word : info.argumentMoves[call.argumentMoves])
            result = mix(result ^ word);
    }
    return result;
}

bool callsSameTarget(const CompiledFunctionInfo& a, const StubCall& first, const CompiledFunctionInfo& b, const StubCall& second)
{
    if (first.stub != second.stub || first.thunk != second.thunk || first.isTailCall != second.isTailCall || first.firstOmittedGPR != second.firstOmittedGPR || first.endOfOmittedGPRs != second.endOfOmittedGPRs)
        return false;
    if ((first.function == StubCall::noFunction) != (second.function == StubCall::noFunction) || (first.argumentMoves == StubCall::noArgumentMoves) != (second.argumentMoves == StubCall::noArgumentMoves))
        return false;
    if (first.function != StubCall::noFunction && !a.knownCallees[first.function].sameFunction(b.knownCallees[second.function]))
        return false;
    return first.argumentMoves == StubCall::noArgumentMoves || a.argumentMoves[first.argumentMoves] == b.argumentMoves[second.argumentMoves];
}

struct Window {
    uint64_t hash;
    uint32_t unit;
    uint32_t function;
};
static_assert(sizeof(Window) == 16);

struct Family {
    int64_t saving;
    uint32_t begin;
    uint32_t end;
    uint32_t firstUnit;
    bool allAreIdentical;
};

void sortByHashAndUnit(std::span<Window> windows, std::span<Window> room)
{
    constexpr unsigned numberOfParts = 256;
    auto isAhead = [](const Window& a, const Window& b) {
        return a.hash != b.hash ? a.hash < b.hash : a.unit < b.unit;
    };
    auto partOf = [](const Window& window) {
        return static_cast<unsigned>(window.hash >> 48 & (numberOfParts - 1));
    };
    if (windows.size() < 4 * numberOfParts) {
        std::ranges::sort(windows, isAhead);
        return;
    }
    std::array<uint32_t, numberOfParts + 1> starts { };
    for (auto& window : windows)
        ++starts[partOf(window) + 1];
    for (unsigned part = 0; part < numberOfParts; ++part)
        starts[part + 1] += starts[part];
    auto next = starts;
    for (auto& window : windows)
        room[next[partOf(window)]++] = window;
    for (unsigned part = 0; part < numberOfParts; ++part)
        std::ranges::sort(room.subspan(starts[part], starts[part + 1] - starts[part]), isAhead);
    memcpySpan(windows, room);
}

void zeroOnPool(std::span<uint8_t> bytes)
{
    constexpr size_t bytesInChunk = MB;
    Typed::forEachIndexInParallel(static_cast<unsigned>((bytes.size() + bytesInChunk - 1) / bytesInChunk), [&](unsigned chunk) {
        size_t start = static_cast<size_t>(chunk) * bytesInChunk;
        zeroSpan(bytes.subspan(start, std::min(bytesInChunk, bytes.size() - start)));
    });
}

template<typename T>
void fillWithZeroesOnPool(Vector<T>& vector, size_t size)
{
    static_assert(std::is_integral_v<T>);
#if ASAN_ENABLED
    vector.fill(0, size);
#else
    RELEASE_ASSERT(vector.isEmpty());
    vector.reserveCapacity(size);
    zeroOnPool(unsafeMakeSpan(reinterpret_cast<uint8_t*>(vector.begin()), size * sizeof(T)));
    vector.grow(size);
#endif
}

template<typename T, typename IsAhead>
void sortOnPool(Vector<T>& list, const IsAhead& isAhead)
{
    constexpr size_t numberOfRuns = 16;
    constexpr size_t fewestForPool = 1 << 13;
    size_t count = list.size();
    if (count < fewestForPool) {
        std::ranges::sort(list, isAhead);
        return;
    }
    Vector<T> merged(count);
    auto runOf = [&](Vector<T>& from, size_t run, size_t runs) {
        size_t start = count * run / runs;
        return from.mutableSpan().subspan(start, count * (run + 1) / runs - start);
    };
    Typed::forEachIndexInParallel(numberOfRuns, [&](unsigned run) {
        std::ranges::sort(runOf(list, run, numberOfRuns), isAhead);
    });
    for (size_t runs = numberOfRuns; runs > 1; runs /= 2) {
        Typed::forEachIndexInParallel(runs / 2, [&](unsigned pair) {
            std::ranges::merge(runOf(list, 2 * pair, runs), runOf(list, 2 * pair + 1, runs), runOf(merged, pair, runs / 2).begin(), isAhead);
        });
        list.swap(merged);
    }
}

class Chooser {
public:
    Chooser(const Vector<const CompiledCode*>& functions, Vector<std::pair<uint32_t, uint32_t>>&& entries)
        : m_functions(functions)
        , m_entries(WTF::move(entries))
    {
    }

    Outlining run()
    {
        gatherUnits();
        m_result.places.resize(m_functions.size());
        if (m_numberOfUnits) {
            findRuns();
            findLongestRepeatCandidates();
            for (unsigned length = longestWindow; length >= shortestWindow; --length)
                selectWindowsOf(length);
        }
        sortOnPool(m_chosenLocations, [](const ChosenLocation& a, const ChosenLocation& b) {
            return a.function != b.function ? a.function < b.function : a.place.offset < b.place.offset;
        });
        Vector<uint32_t> functionFirstLocations;
        for (uint32_t i = 0; i < m_chosenLocations.size(); ++i) {
            if (!i || m_chosenLocations[i].function != m_chosenLocations[i - 1].function)
                functionFirstLocations.append(i);
        }
        Typed::forEachIndexInParallel(functionFirstLocations.size(), [&](unsigned number) {
            uint32_t first = functionFirstLocations[number];
            uint32_t end = number + 1 < functionFirstLocations.size() ? functionFirstLocations[number + 1] : m_chosenLocations.size();
            m_result.places[m_chosenLocations[first].function] = Vector<OutlinedSite>(end - first, [&](size_t i) {
                OutlinedSite place = m_chosenLocations[first + i].place;
                place.offset = m_offsets[place.offset];
                return place;
            });
        });
        report();
        Typed::destroyOnHelperThread(std::tuple { WTF::move(m_firstUnit), WTF::move(m_decoded), WTF::move(m_sums), WTF::move(m_offsets), WTF::move(m_lengths), WTF::move(m_flags), WTF::move(m_runs), WTF::move(m_longestRepeatCandidate), WTF::move(m_active), WTF::move(m_stillActive), WTF::move(m_found), WTF::move(m_windows) });
        return WTF::move(m_result);
    }

private:
    bool isInRegion(const CompiledCode& code) const
    {
        if (!code.rareCodeMayBeOutlined || code.bytes.isEmpty() || !code.continuations.isEmpty())
            return false;
        return !isARM64() || !code.info.hasNoFrame;
    }

    void gatherUnits()
    {
        unsigned count = m_functions.size();
        std::ranges::sort(m_entries);
        Vector<uint32_t> numberOfUnits;
        numberOfUnits.fill(0, count);
        Vector<WalkFailure> causes;
        causes.fill(WalkFailure::None, count);
        Vector<uint32_t> codeSizes(count);
        m_decoded.grow(count);
        Typed::forEachIndexInParallel(count, [&](unsigned function) {
            auto& code = *m_functions[function];
            codeSizes[function] = safeCast<uint32_t>(code.bytes.size());
            if (!isInRegion(code))
                return;
            uint32_t number = 0;
            uint32_t firstGatheredOffset = firstGatheredOffsetOf(code);
            Walked& walked = m_decoded[function];
            walked.kindsAndLengths.reserveInitialCapacity(isX86() ? code.instructions.size() : code.bytes.size() / sizeof(uint32_t));
            WalkFailure cause = InstructionsInCodeInternal::tryDecode(code, [&](const InstructionInCode& instruction) {
                number += instruction.offset >= firstGatheredOffset;
                RELEASE_ASSERT(instruction.length < 16);
                walked.kindsAndLengths.append(static_cast<uint8_t>(static_cast<unsigned>(instruction.kind) << 4 | instruction.length));
                if (hasTarget(instruction.kind))
                    walked.targets.append(instruction.target);
            });
            if (cause != WalkFailure::None || !code.hasCodeAnnotations || code.isRewritten) {
                walked = { };
                numberOfUnits[function] = std::numeric_limits<uint32_t>::max();
                causes[function] = cause;
                return;
            }
            if (!number)
                walked = { };
            numberOfUnits[function] = number;
        });
        m_firstUnit.reserveInitialCapacity(count + 1);
        size_t total = 0;
        size_t codeSize = 0;
        for (unsigned function = 0; function < count; ++function) {
            m_firstUnit.append(safeCast<uint32_t>(total));
            if (numberOfUnits[function] == std::numeric_limits<uint32_t>::max()) {
                ++m_undecodedFunctionCount;
                m_undecodedBytes += codeSizes[function];
                auto& [functions, bytes] = m_undecodedByCause[static_cast<unsigned>(causes[function])];
                ++functions;
                bytes += codeSizes[function];
                numberOfUnits[function] = 0;
            }
            total += numberOfUnits[function];
            m_numberOfFunctions += !!numberOfUnits[function];
            codeSize += codeSizes[function];
        }
        m_firstUnit.append(safeCast<uint32_t>(total));
        m_numberOfUnits = safeCast<uint32_t>(total);
        m_numberOfCopies = codeSize / stubCallReach + 1;
        fillWithZeroesOnPool(m_sums, total + 1);
        fillWithZeroesOnPool(m_offsets, total);
        fillWithZeroesOnPool(m_lengths, total);
        fillWithZeroesOnPool(m_flags, total);
        fillWithZeroesOnPool(m_runs, total);
        fillWithZeroesOnPool(m_longestRepeatCandidate, total);
        Typed::forEachIndexInParallel(count, [&](unsigned function) {
            if (numberOfUnits[function])
                gatherUnitsOf(function);
        });
        unsigned numberOfChunks = (total + unitsInChunk - 1) / unitsInChunk;
        auto forEachUnitIn = [&](unsigned chunk, const auto& functor) {
            size_t end = std::min(total, static_cast<size_t>(chunk + 1) * unitsInChunk);
            for (size_t unit = static_cast<size_t>(chunk) * unitsInChunk; unit < end; ++unit)
                functor(unit);
        };
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            uint64_t sum = 0;
            forEachUnitIn(chunk, [&](size_t unit) {
                sum = m_sums[unit + 1] + sum * sumsBase;
                m_sums[unit + 1] = sum;
            });
        });
        uint64_t chunkPower = 1;
        for (unsigned unit = 0; unit < unitsInChunk; ++unit)
            chunkPower *= sumsBase;
        Vector<uint64_t> chunkPrefixSums(numberOfChunks);
        uint64_t prefixSum = 0;
        for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk) {
            chunkPrefixSums[chunk] = prefixSum;
            if (chunk + 1 < numberOfChunks)
                prefixSum = m_sums[static_cast<size_t>(chunk + 1) * unitsInChunk] + prefixSum * chunkPower;
        }
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            uint64_t carried = chunkPrefixSums[chunk];
            forEachUnitIn(chunk, [&](size_t unit) {
                carried *= sumsBase;
                m_sums[unit + 1] += carried;
            });
        });
        m_powers[0] = 1;
        for (unsigned length = 1; length <= longestWindow; ++length)
            m_powers[length] = m_powers[length - 1] * sumsBase;
    }

    struct Walked {
        Vector<uint8_t> kindsAndLengths;
        Vector<uint32_t> targets;
    };

    static uint32_t firstGatheredOffsetOf(const CompiledCode& code) { return code.allCodeMayBeOutlined ? 0 : code.rareCodeStart; }

    static bool hasTarget(InstructionInCode::Kind kind)
    {
        switch (kind) {
        case InstructionInCode::Kind::StubCall:
        case InstructionInCode::Kind::Jump:
        case InstructionInCode::Kind::ConditionalJump:
        case InstructionInCode::Kind::TableAddress:
        case InstructionInCode::Kind::TableEntry:
            return true;
        case InstructionInCode::Kind::Plain:
        case InstructionInCode::Kind::Pinned:
        case InstructionInCode::Kind::End:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    template<typename Functor>
    void forEachDecodedInstruction(unsigned function, const Functor& functor)
    {
        Walked walked = std::exchange(m_decoded[function], { });
        uint32_t offset = 0;
        size_t nextTarget = 0;
        for (uint8_t packed : walked.kindsAndLengths) {
            InstructionInCode instruction { offset, 0, static_cast<uint8_t>(packed & 15), static_cast<InstructionInCode::Kind>(packed >> 4) };
            if (hasTarget(instruction.kind))
                instruction.target = walked.targets[nextTarget++];
            functor(static_cast<const InstructionInCode&>(instruction));
            offset += instruction.length;
        }
    }

    void gatherUnitsOf(unsigned function)
    {
        auto& code = *m_functions[function];
        auto& info = code.info;
        uint32_t unit = m_firstUnit[function];
        uint32_t end = 0;
        Vector<uint32_t, 64> targets;
        constexpr unsigned numberOfFirstBranches = 8;
        Vector<std::pair<uint32_t, uint32_t>, numberOfFirstBranches> firstBranches;
        uint32_t pollAtPrologue = std::numeric_limits<uint32_t>::max();
        uint32_t firstGatheredOffset = firstGatheredOffsetOf(code);
        forEachDecodedInstruction(function, [&](const InstructionInCode& instruction) {
            RELEASE_ASSERT(instruction.offset == end && instruction.length);
            end += instruction.length;
            bool isGathered = instruction.offset >= firstGatheredOffset;
            if (isGathered) {
                m_offsets[unit] = instruction.offset;
                m_lengths[unit] = std::min<uint32_t>(instruction.length, std::numeric_limits<uint8_t>::max());
            }
            switch (instruction.kind) {
            case InstructionInCode::Kind::Plain:
                if (!isGathered)
                    break;
                m_flags[unit] = MayBeInWindow;
                m_sums[unit + 1] = numberOf(code.bytes.span().subspan(instruction.offset, instruction.length));
                break;
            case InstructionInCode::Kind::StubCall: {
                auto& call = info.stubCalls[instruction.target];
                RELEASE_ASSERT(call.offset == instruction.offset && instruction.length == sizeOfNearCall);
                if (call.stub == Stub::PollAtPrologue)
                    pollAtPrologue = instruction.offset;
                if (!isGathered || call.isTailCall || (isARM64() && call.function != StubCall::noFunction))
                    break;
                m_flags[unit] = MayBeInWindow | IsCall;
                m_sums[unit + 1] = numberOf(info, call);
                break;
            }
            case InstructionInCode::Kind::Jump:
            case InstructionInCode::Kind::ConditionalJump:
                if (firstBranches.size() < numberOfFirstBranches)
                    firstBranches.append({ instruction.offset, instruction.target });
                [[fallthrough]];
            case InstructionInCode::Kind::TableEntry:
                targets.append(instruction.target);
                break;
            case InstructionInCode::Kind::Pinned:
            case InstructionInCode::Kind::TableAddress:
                break;
            case InstructionInCode::Kind::End:
#if CPU(X86_64) && OS(LINUX)
                if (code.bytes[instruction.offset] == trapInstruction && !std::ranges::binary_search(info.faultingAccesses, instruction.offset, { }, &CompiledFunctionInfo::FaultingAccess::codeOffset))
                    pollAtPrologue = instruction.offset;
#endif
                break;
            }
            unit += isGathered;
        });
        RELEASE_ASSERT(unit == m_firstUnit[function + 1] && end == code.bytes.size());
        for (auto& entrypoint : info.typedCatchEntrypointsForCallSites)
            targets.append(entrypoint.second);
        uint32_t appendedEntriesStart = std::numeric_limits<uint32_t>::max();
        for (auto entry = std::ranges::lower_bound(m_entries, std::pair<uint32_t, uint32_t> { function, 0 }); entry != m_entries.end() && entry->first == function; ++entry) {
            targets.append(entry->second);
            if (entry->second)
                appendedEntriesStart = std::min(appendedEntriesStart, entry->second);
        }
        std::ranges::sort(targets);
        unit = m_firstUnit[function];
        for (uint32_t target : targets) {
            if (target < firstGatheredOffset)
                continue;
            while (unit < m_firstUnit[function + 1] && m_offsets[unit] < target)
                ++unit;
            RELEASE_ASSERT(unit < m_firstUnit[function + 1] && m_offsets[unit] == target);
            m_flags[unit] |= IsTarget;
        }
        for (auto& [offset, target] : firstBranches) {
            if (target != pollAtPrologue)
                continue;
            for (unit = m_firstUnit[function]; unit < m_firstUnit[function + 1] && m_offsets[unit] < offset; ++unit)
                m_flags[unit] &= ~MayBeInWindow;
        }
        for (unit = m_firstUnit[function + 1]; unit-- > m_firstUnit[function] && m_offsets[unit] >= appendedEntriesStart;)
            m_flags[unit] &= ~MayBeInWindow;
        if (!code.allCodeMayBeOutlined) {
            for (unit = m_firstUnit[function]; unit < m_firstUnit[function + 1] && m_offsets[unit] < code.rareCodeStart; ++unit)
                m_flags[unit] &= ~MayBeInWindow;
        }
    }

    void findRuns()
    {
        std::atomic<size_t> windowStartCandidateCount { 0 };
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned function) {
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
        m_windowStartCandidateCount = windowStartCandidateCount;
    }

    void findLongestRepeatCandidates()
    {
        unsigned numberOfChunks = (m_numberOfUnits + unitsInChunk - 1) / unitsInChunk;
        Vector<Vector<uint32_t>> candidates(numberOfChunks);
        size_t numberOfCandidates = m_windowStartCandidateCount;
        m_slotMask = roundUpToPowerOfTwo(std::max<size_t>(4 * numberOfCandidates, 1 << 16)) - 1;
        m_seen.resize(m_slotMask + 1);
        m_seenTwice.resize(m_slotMask + 1);
        for (unsigned length = shortestWindow; length <= longestWindow; ++length) {
            size_t slotMask = std::min(m_slotMask, roundUpToPowerOfTwo(std::max<size_t>(4 * numberOfCandidates, 1 << 16)) - 1);
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
            zeroOnPool(asMutableByteSpan(m_seen.unsafeWords().words()).first((slotMask + 1) / 8));
            zeroOnPool(asMutableByteSpan(m_seenTwice.unsafeWords().words()).first((slotMask + 1) / 8));
            Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
                forEachCandidateIn(chunk, [&](uint32_t unit) {
                    size_t slot = hashOf(unit, length) & slotMask;
                    if (!m_seen.atomicSetAndCheck(slot, true))
                        m_seenTwice.atomicSetAndCheck(slot, true);
                });
            });
            std::atomic<size_t> unchangedCount { 0 };
            Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
                Vector<uint32_t> left;
                forEachCandidateIn(chunk, [&](uint32_t unit) {
                    if (!m_seenTwice.at(hashOf(unit, length) & slotMask))
                        return;
                    m_longestRepeatCandidate[unit] = length;
                    if (m_runs[unit] > length)
                        left.append(unit);
                });
                unchangedCount += left.size();
                candidates[chunk] = WTF::move(left);
            });
            size_t held = 0;
            for (auto& some : candidates)
                held += some.capacity() * sizeof(uint32_t);
            m_peak = std::max(m_peak, unitsSize() + held);
            if (!unchangedCount)
                break;
            numberOfCandidates = unchangedCount;
        }
        Vector<std::array<Vector<uint32_t>, longestWindow + 1>> chunkUnits(numberOfChunks);
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            uint32_t end = std::min<uint64_t>(m_numberOfUnits, static_cast<uint64_t>(chunk + 1) * unitsInChunk);
            for (uint32_t unit = chunk * unitsInChunk; unit < end; ++unit) {
                if (uint8_t length = m_longestRepeatCandidate[unit])
                    chunkUnits[chunk][length].append(unit);
            }
        });
        Vector<std::array<uint32_t, longestWindow + 1>> chunkStarts(numberOfChunks);
        std::array<uint32_t, longestWindow + 1> numbers { };
        for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk) {
            chunkStarts[chunk] = numbers;
            for (unsigned length = 0; length <= longestWindow; ++length)
                numbers[length] += chunkUnits[chunk][length].size();
        }
        size_t repeatCandidateCount = 0;
        for (unsigned length = 0; length <= longestWindow; ++length) {
            m_unitsByLongest[length].grow(numbers[length]);
            repeatCandidateCount += numbers[length];
        }
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            for (unsigned length = 0; length <= longestWindow; ++length) {
                auto units = chunkUnits[chunk][length].span();
                memcpySpan(m_unitsByLongest[length].mutableSpan().subspan(chunkStarts[chunk][length], units.size()), units);
            }
        });
        m_active.reserveCapacity(repeatCandidateCount);
        m_stillActive.reserveCapacity(repeatCandidateCount);
        m_found.reserveCapacity(repeatCandidateCount);
        m_windows.reserveCapacity(repeatCandidateCount);
    }

    size_t unitsSize() const
    {
        return m_sums.capacity() * sizeof(uint64_t) + (m_offsets.capacity() + m_firstUnit.capacity()) * sizeof(uint32_t) + m_lengths.capacity() + m_flags.capacity() + m_runs.capacity() + m_longestRepeatCandidate.capacity() + (m_slotMask + 1) / 4;
    }

    ALWAYS_INLINE uint64_t hashOf(uint32_t unit, unsigned length) const { return mix(m_sums[unit + length] - m_sums[unit] * m_powers[length]); }
    ALWAYS_INLINE uint32_t bytesOf(uint32_t unit, unsigned length) const { return m_offsets[unit + length - 1] + m_lengths[unit + length - 1] - m_offsets[unit]; }
    ALWAYS_INLINE bool isFree(uint32_t unit, unsigned length) const { return !((m_flags[unit] | m_flags[unit + length - 1]) & IsTaken); }
    unsigned functionOf(uint32_t unit) const { return std::ranges::upper_bound(m_firstUnit, unit) - m_firstUnit.begin() - 1; }

    int64_t savingsOf(uint64_t places, uint64_t bytes, bool endsInCall) const
    {
        if (places < 2)
            return 0;
        int64_t bodySize = static_cast<int64_t>(bytes) + (endsInCall ? 0 : returnSize);
        return static_cast<int64_t>(places) * (static_cast<int64_t>(bytes) - static_cast<int64_t>(sizeOfNearCall)) - static_cast<int64_t>(m_numberOfCopies) * bodySize;
    }

    bool isIdentical(uint32_t first, unsigned firstFunction, uint32_t second, unsigned secondFunction, unsigned length) const
    {
        uint32_t bytes = bytesOf(first, length);
        if (bytesOf(second, length) != bytes)
            return false;
        auto& a = *m_functions[firstFunction];
        auto& b = *m_functions[secondFunction];
        bool endsInCall = m_flags[first + length - 1] & IsCall;
        if (endsInCall != !!(m_flags[second + length - 1] & IsCall))
            return false;
        if (memcmp(a.bytes.span().data() + m_offsets[first], b.bytes.span().data() + m_offsets[second], bytes))
            return false;
        if (!endsInCall)
            return true;
        auto callAt = [&](const CompiledCode& code, uint32_t offset) -> const StubCall& {
            auto found = std::ranges::lower_bound(code.info.stubCalls, offset, { }, &StubCall::offset);
            RELEASE_ASSERT(found != code.info.stubCalls.end() && found->offset == offset);
            return *found;
        };
        return callsSameTarget(a.info, callAt(a, m_offsets[first + length - 1]), b.info, callAt(b, m_offsets[second + length - 1]));
    }

    void selectWindowsOf(unsigned length)
    {
        m_active.appendVector(m_unitsByLongest[length]);
        m_unitsByLongest[length].clear();
        if (m_active.isEmpty())
            return;
        unsigned numberOfChunks = (m_active.size() + unitsInChunk - 1) / unitsInChunk;
        auto forEachLocationIn = [&](unsigned chunk, const auto& functor) {
            size_t end = std::min<size_t>(m_active.size(), static_cast<size_t>(chunk + 1) * unitsInChunk);
            for (size_t place = static_cast<size_t>(chunk) * unitsInChunk; place < end; ++place)
                functor(place);
        };
        m_found.resize(m_active.size());
        Vector<uint32_t> starts;
        starts.fill(0, static_cast<size_t>(numberOfChunks) * numberOfShards);
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            forEachLocationIn(chunk, [&](size_t place) {
                uint32_t unit = m_active[place];
                if (!isFree(unit, length)) {
                    m_found[place] = { 0, std::numeric_limits<uint32_t>::max(), 0 };
                    return;
                }
                uint64_t hash = hashOf(unit, length);
                m_found[place] = { hash, unit, 0 };
                ++starts[static_cast<size_t>(chunk) * numberOfShards + (hash >> 56)];
            });
        });
        std::array<uint32_t, numberOfShards + 1> shardStart;
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
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            forEachLocationIn(chunk, [&](size_t place) {
                auto& window = m_found[place];
                if (window.unit != std::numeric_limits<uint32_t>::max())
                    m_windows[starts[static_cast<size_t>(chunk) * numberOfShards + (window.hash >> 56)]++] = window;
            });
        });
        std::array<Vector<Family>, numberOfShards> shardFamilies;
        Typed::forEachIndexInParallel(numberOfShards, [&](unsigned shard) {
            auto windows = m_windows.mutableSpan().subspan(shardStart[shard], shardStart[shard + 1] - shardStart[shard]);
            sortByHashAndUnit(windows, m_found.mutableSpan().subspan(shardStart[shard], windows.size()));
            for (size_t begin = 0; begin < windows.size();) {
                size_t end = begin + 1;
                while (end < windows.size() && windows[end].hash == windows[begin].hash)
                    ++end;
                uint64_t places = 0;
                uint32_t afterLast = 0;
                for (size_t i = begin; i < end; ++i) {
                    if (places && windows[i].unit < afterLast)
                        continue;
                    ++places;
                    afterLast = windows[i].unit + length;
                }
                uint32_t first = windows[begin].unit;
                if (int64_t saving = savingsOf(places, bytesOf(first, length), m_flags[first + length - 1] & IsCall); saving >= minSavings) {
                    bool allAreIdentical = true;
                    for (size_t i = begin; i < end; ++i) {
                        windows[i].function = functionOf(windows[i].unit);
                        allAreIdentical = allAreIdentical && isIdentical(first, windows[begin].function, windows[i].unit, windows[i].function, length);
                    }
                    shardFamilies[shard].append({ saving, static_cast<uint32_t>(shardStart[shard] + begin), static_cast<uint32_t>(shardStart[shard] + end), first, allAreIdentical });
                }
                begin = end;
            }
        });
        Vector<Family> families;
        for (auto& some : shardFamilies)
            families.appendVector(some);
        sortOnPool(families, [](const Family& a, const Family& b) {
            return a.saving != b.saving ? a.saving > b.saving : a.firstUnit < b.firstUnit;
        });
        size_t waiting = 0;
        for (auto& some : m_unitsByLongest)
            waiting += some.capacity() * sizeof(uint32_t);
        m_peak = std::max(m_peak, unitsSize() + waiting + (m_active.capacity() + m_stillActive.capacity() + starts.capacity()) * sizeof(uint32_t) + (m_found.capacity() + m_windows.capacity()) * sizeof(Window) + 2 * families.capacity() * sizeof(Family));
        Vector<std::pair<uint32_t, unsigned>> chosen;
        for (size_t number = 0; number < families.size(); ++number) {
            constexpr uint32_t windowsBefore = 16;
            constexpr size_t windowFamiliesBefore = 8;
            constexpr size_t flagFamiliesBefore = 4;
            constexpr uint32_t windowsInCacheLine = 4;
            auto& family = families[number];
            if (number + windowFamiliesBefore < families.size())
                __builtin_prefetch(&m_windows[families[number + windowFamiliesBefore].begin]);
            if (number + flagFamiliesBefore < families.size()) {
                auto& later = families[number + flagFamiliesBefore];
                for (uint32_t i = later.begin; i < std::min(later.end, later.begin + windowsInCacheLine); ++i)
                    __builtin_prefetch(&m_flags[m_windows[i].unit]);
            }
            chosen.shrink(0);
            uint32_t afterLast = 0;
            for (uint32_t i = family.begin; i < family.end; ++i) {
                if (family.end - i > windowsBefore)
                    __builtin_prefetch(&m_flags[m_windows[i + windowsBefore].unit]);
                uint32_t unit = m_windows[i].unit;
                if ((!chosen.isEmpty() && unit < afterLast) || !isFree(unit, length))
                    continue;
                unsigned function = m_windows[i].function;
                if (!chosen.isEmpty() && !family.allAreIdentical && !isIdentical(chosen[0].first, chosen[0].second, unit, function, length))
                    continue;
                chosen.append({ unit, function });
                afterLast = unit + length;
            }
            if (chosen.isEmpty())
                continue;
            uint32_t first = chosen[0].first;
            uint32_t bytes = bytesOf(first, length);
            bool endsInCall = m_flags[first + length - 1] & IsCall;
            int64_t saving = savingsOf(chosen.size(), bytes, endsInCall);
            if (saving < minSavings)
                continue;
            uint32_t body = m_result.bodies.size();
            uint32_t singleCaller = chosen[0].second;
            for (auto& [unit, function] : chosen) {
                for (unsigned i = 0; i < length; ++i)
                    m_flags[unit + i] |= IsTaken;
                m_chosenLocations.append({ function, { unit, bytes, body } });
                if (function != chosen[0].second)
                    singleCaller = Outlining::none;
            }
            m_result.bodies.append({ chosen[0].second, m_offsets[first], bytes, static_cast<uint32_t>(chosen.size()), singleCaller, endsInCall });
            m_saved[length] += saving;
            m_places[length] += chosen.size();
            ++m_bodies[length];
        }
        Vector<uint32_t> keptStarts(numberOfChunks);
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            size_t start = static_cast<size_t>(chunk) * unitsInChunk;
            size_t end = start;
            forEachLocationIn(chunk, [&](size_t place) {
                uint32_t unit = m_active[place];
                if (!(m_flags[unit] & IsTaken))
                    m_active[end++] = unit;
            });
            keptStarts[chunk] = end - start;
        });
        uint32_t keptCount = 0;
        for (uint32_t& start : keptStarts) {
            uint32_t number = start;
            start = keptCount;
            keptCount += number;
        }
        m_stillActive.resize(keptCount);
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            uint32_t number = (chunk + 1 < numberOfChunks ? keptStarts[chunk + 1] : keptCount) - keptStarts[chunk];
            memcpySpan(m_stillActive.mutableSpan().subspan(keptStarts[chunk], number), m_active.span().subspan(static_cast<size_t>(chunk) * unitsInChunk, number));
        });
        m_active.swap(m_stillActive);
    }

    void report() const
    {
        int64_t saved = 0;
        uint64_t places = 0;
        uint64_t bodiesSize = 0;
        uint64_t withOneCaller = 0;
        if (!Options::verboseAOTCompilation())
            return;
        for (unsigned length = 0; length <= longestWindow; ++length) {
            saved += m_saved[length];
            places += m_places[length];
        }
        for (auto& body : m_result.bodies) {
            bodiesSize += body.length + (body.endsInCall ? 0 : returnSize);
            withOneCaller += body.singleCaller != Outlining::none;
        }
        dataLogLn("AOT: outlining: ", m_numberOfUnits, " instructions in ", m_numberOfFunctions, " of ", m_functions.size(), " functions: ", m_result.bodies.size(), " bodies of ", bodiesSize, " bytes, ", withOneCaller, " of them called by one function, ", places, " places, ", saved, " bytes saved");
        dataLogLn("AOT: outlining: ", m_undecodedFunctionCount, " functions of the region, ", m_undecodedBytes, " bytes, could not be walked and are left alone; the search held at most ", m_peak, " bytes");
        uint64_t eligibleCount = 0;
        uint64_t eligibleBytes = 0;
        for (uint32_t unit = 0; unit < m_numberOfUnits; ++unit) {
            if (m_flags[unit] & MayBeInWindow) {
                ++eligibleCount;
                eligibleBytes += m_lengths[unit];
            }
        }
        dataLogLn("AOT: outlining: ", eligibleCount, " instructions, ", eligibleBytes, " bytes, are eligible for a window");
        for (unsigned cause = 0; cause < numberOfWalkFailures; ++cause) {
            if (auto [functions, bytes] = m_undecodedByCause[cause]; functions)
                dataLogLn("AOT: outlining: not decoded: ", functions, " functions, ", bytes, " bytes: ", cause ? walkFailureMessages[cause] : "nothing to decode: the function is marked as rewritten, or its annotations were not kept"_s);
        }
        for (unsigned length = longestWindow; length >= shortestWindow; --length) {
            if (m_bodies[length])
                dataLogLn("AOT: outlining: windows of ", length, ": ", m_bodies[length], " bodies, ", m_places[length], " places, ", m_saved[length], " bytes saved");
        }
    }

    const Vector<const CompiledCode*>& m_functions;
    Vector<std::pair<uint32_t, uint32_t>> m_entries;
    Outlining m_result;
    Vector<uint32_t> m_firstUnit;
    Vector<Walked> m_decoded;
    Vector<uint64_t> m_sums;
    Vector<uint32_t> m_offsets;
    Vector<uint8_t> m_lengths;
    Vector<uint8_t> m_flags;
    Vector<uint8_t> m_runs;
    Vector<uint8_t> m_longestRepeatCandidate;
    std::array<Vector<uint32_t>, longestWindow + 1> m_unitsByLongest;
    struct ChosenLocation {
        uint32_t function;
        OutlinedSite place;
    };
    Vector<ChosenLocation> m_chosenLocations;
    Vector<uint32_t> m_active;
    Vector<uint32_t> m_stillActive;
    Vector<Window> m_found;
    Vector<Window> m_windows;
    FastBitVector m_seen;
    FastBitVector m_seenTwice;
    std::array<uint64_t, longestWindow + 1> m_powers { };
    std::array<int64_t, longestWindow + 1> m_saved { };
    std::array<uint64_t, longestWindow + 1> m_places { };
    std::array<uint64_t, longestWindow + 1> m_bodies { };
    size_t m_slotMask { 0 };
    size_t m_numberOfCopies { 1 };
    uint32_t m_numberOfUnits { 0 };
    size_t m_windowStartCandidateCount { 0 };
    uint32_t m_numberOfFunctions { 0 };
    uint32_t m_undecodedFunctionCount { 0 };
    size_t m_undecodedBytes { 0 };
    std::array<std::pair<uint32_t, size_t>, numberOfWalkFailures> m_undecodedByCause { };
    size_t m_peak { 0 };
};

} // anonymous namespace

Outlining chooseWindows(const Vector<const CompiledCode*>& functions, Vector<std::pair<uint32_t, uint32_t>>&& entriesInsideFunctions)
{
    return Chooser(functions, WTF::move(entriesInsideFunctions)).run();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

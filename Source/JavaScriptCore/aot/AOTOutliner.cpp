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
#include <wtf/DataLog.h>
#include <wtf/FastBitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

const std::optional<OutliningOptions>& OutliningOptions::asked()
{
    static const NeverDestroyed<std::optional<OutliningOptions>> result = []() -> std::optional<OutliningOptions> {
        const char* text = getenv("AOT_SWITCH_OUTLINE");
        if (!text)
            return std::nullopt;
        OutliningOptions options;
        char region[3] = { };
        char form = 0;
        int read = 0;
        int fields = sscanf(text, "%2[atgl],%u,%c%n", region, &options.leastLength, &form, &read);
        const char* rest = text + read;
        bool isRegion = !strcmp(region, "a") || !strcmp(region, "t") || !strcmp(region, "g") || !strcmp(region, "l") || !strcmp(region, "tl") || !strcmp(region, "gl");
        bool isWellFormed = fields == 3 && isRegion && strchr("sk", form) && options.leastLength >= 2 && options.leastLength <= 24 && (!*rest || !strcmp(rest, ",count"));
        if (!isWellFormed) {
            dataLogLn("AOT: AOT_SWITCH_OUTLINE is not <a|t|g|l|tl|gl>,<least length, 2 to 24>,<s|k>[,count]: ", text);
            CRASH();
        }
        options.region |= region[0] == 't' ? isNotHotByText : region[0] == 'g' ? isColdByGraph : 0;
        options.takesFunctionsWhole = region[0] != 'l';
        options.takesRareCode = strchr(region, 'l');
        options.mayEndInCall = form == 'k';
        options.onlyCounts = *rest;
        return options;
    }();
    return result.get();
}

namespace {

constexpr unsigned longestWindow = 24;
constexpr int64_t leastSaving = 64;
constexpr unsigned numberOfShards = 256;
constexpr unsigned unitsInChunk = 1 << 16;
constexpr uint64_t baseOfSums = 0x9e3779b97f4a7c15ULL;
#if CPU(X86_64)
constexpr int64_t sizeOfReturn = 1;
#else
constexpr int64_t sizeOfReturn = sizeof(uint32_t);
#endif

enum : uint8_t {
    MayStandInWindow = 1,
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

bool callsTheSame(const CompiledFunctionInfo& a, const StubCall& first, const CompiledFunctionInfo& b, const StubCall& second)
{
    if (first.stub != second.stub || first.thunk != second.thunk || first.isTailCall != second.isTailCall)
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
};

struct Family {
    int64_t saving;
    uint32_t begin;
    uint32_t end;
    uint32_t firstUnit;
};

class Chooser {
public:
    Chooser(const Vector<const CompiledCode*>& functions, Vector<std::pair<uint32_t, uint32_t>>&& entries, const OutliningOptions& options)
        : m_functions(functions)
        , m_entries(WTF::move(entries))
        , m_options(options)
    {
    }

    Outlining run()
    {
        gatherUnits();
        m_result.places.resize(m_functions.size());
        if (m_numberOfUnits) {
            findRuns();
            findLongestThatMayRepeat();
            for (unsigned length = longestWindow; length >= m_options.leastLength; --length)
                takeWindowsOf(length);
        }
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned function) {
            std::ranges::sort(m_result.places[function], { }, &OutlinedPlace::offset);
        });
        report();
        return WTF::move(m_result);
    }

private:
    bool isInRegion(const CompiledCode& code) const
    {
        if (!m_options.takes(code.regionsForOutlining) || code.bytes.isEmpty() || !code.continuations.isEmpty())
            return false;
        return !isARM64() || !code.info.hasNoFrame;
    }

    void gatherUnits()
    {
        unsigned count = m_functions.size();
        std::ranges::sort(m_entries);
        Vector<uint32_t> numberOfUnits;
        numberOfUnits.fill(0, count);
        Vector<CauseOfNoWalk> causes;
        causes.fill(CauseOfNoWalk::None, count);
        Typed::forEachIndexInParallel(count, [&](unsigned function) {
            auto& code = *m_functions[function];
            if (!isInRegion(code))
                return;
            if (!canBeWalked(code)) {
                numberOfUnits[function] = std::numeric_limits<uint32_t>::max();
                causes[function] = causeOfNoWalk(code);
                return;
            }
            uint32_t number = 0;
            forEachInstructionInCode(code, [&](const InstructionInCode&) {
                ++number;
            });
            numberOfUnits[function] = number;
        });
        m_firstUnit.reserveInitialCapacity(count + 1);
        size_t total = 0;
        size_t sizeOfCode = 0;
        for (unsigned function = 0; function < count; ++function) {
            m_firstUnit.append(safeCast<uint32_t>(total));
            if (numberOfUnits[function] == std::numeric_limits<uint32_t>::max()) {
                ++m_numberOfFunctionsNotWalked;
                m_bytesNotWalked += m_functions[function]->bytes.size();
                auto& [functions, bytes] = m_notWalkedByCause[static_cast<unsigned>(causes[function])];
                ++functions;
                bytes += m_functions[function]->bytes.size();
                numberOfUnits[function] = 0;
            }
            total += numberOfUnits[function];
            m_numberOfFunctions += !!numberOfUnits[function];
            sizeOfCode += m_functions[function]->bytes.size();
        }
        m_firstUnit.append(safeCast<uint32_t>(total));
        m_numberOfUnits = safeCast<uint32_t>(total);
        m_numberOfCopies = sizeOfCode / stubCallReach + 1;
        m_sums.fill(0, total + 1);
        m_offsets.fill(0, total);
        m_lengths.fill(0, total);
        m_flags.fill(0, total);
        m_runs.fill(0, total);
        m_longestThatMayRepeat.fill(0, total);
        Typed::forEachIndexInParallel(count, [&](unsigned function) {
            if (numberOfUnits[function])
                gatherUnitsOf(function);
        });
        for (size_t unit = 0; unit < total; ++unit)
            m_sums[unit + 1] += m_sums[unit] * baseOfSums;
        m_powers[0] = 1;
        for (unsigned length = 1; length <= longestWindow; ++length)
            m_powers[length] = m_powers[length - 1] * baseOfSums;
        m_slotMask = roundUpToPowerOfTwo(std::max<size_t>(4 * total, 1 << 16)) - 1;
        m_seen.resize(m_slotMask + 1);
        m_seenTwice.resize(m_slotMask + 1);
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
        forEachInstructionInCode(code, [&](const InstructionInCode& instruction) {
            RELEASE_ASSERT(instruction.offset == end && instruction.length);
            end += instruction.length;
            m_offsets[unit] = instruction.offset;
            m_lengths[unit] = std::min<uint32_t>(instruction.length, std::numeric_limits<uint8_t>::max());
            switch (instruction.kind) {
            case InstructionInCode::Kind::Plain:
                m_flags[unit] = MayStandInWindow;
                m_sums[unit + 1] = numberOf(code.bytes.span().subspan(instruction.offset, instruction.length));
                break;
            case InstructionInCode::Kind::StubCall: {
                auto& call = info.stubCalls[instruction.target];
                RELEASE_ASSERT(call.offset == instruction.offset && instruction.length == sizeOfNearCall);
                if (call.stub == Stub::PollAtPrologue)
                    pollAtPrologue = instruction.offset;
                if (!m_options.mayEndInCall || call.isTailCall || (isARM64() && call.function != StubCall::noFunction))
                    break;
                m_flags[unit] = MayStandInWindow | IsCall;
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
            case InstructionInCode::Kind::AddressOfTable:
            case InstructionInCode::Kind::End:
                break;
            }
            ++unit;
        });
        RELEASE_ASSERT(unit == m_firstUnit[function + 1] && end == code.bytes.size());
        for (auto& entrypoint : info.typedCatchEntrypointsForCallSites)
            targets.append(entrypoint.second);
        uint32_t startOfGuests = std::numeric_limits<uint32_t>::max();
        for (auto entry = std::ranges::lower_bound(m_entries, std::pair<uint32_t, uint32_t> { function, 0 }); entry != m_entries.end() && entry->first == function; ++entry) {
            targets.append(entry->second);
            if (entry->second)
                startOfGuests = std::min(startOfGuests, entry->second);
        }
        std::ranges::sort(targets);
        unit = m_firstUnit[function];
        for (uint32_t target : targets) {
            while (unit < m_firstUnit[function + 1] && m_offsets[unit] < target)
                ++unit;
            RELEASE_ASSERT(unit < m_firstUnit[function + 1] && m_offsets[unit] == target);
            m_flags[unit] |= IsTarget;
        }
        for (auto& [offset, target] : firstBranches) {
            if (target != pollAtPrologue)
                continue;
            for (unit = m_firstUnit[function]; m_offsets[unit] < offset; ++unit)
                m_flags[unit] &= ~MayStandInWindow;
        }
        if (isARM64() || !m_options.takesEverything()) {
            for (unit = m_firstUnit[function + 1]; unit-- > m_firstUnit[function] && m_offsets[unit] >= startOfGuests;)
                m_flags[unit] &= ~MayStandInWindow;
        }
        if (!m_options.takesWhole(code.regionsForOutlining)) {
            for (unit = m_firstUnit[function]; unit < m_firstUnit[function + 1] && m_offsets[unit] < code.startOfRareCode; ++unit)
                m_flags[unit] &= ~MayStandInWindow;
        }
    }

    void findRuns()
    {
        Typed::forEachIndexInParallel(m_functions.size(), [&](unsigned function) {
            uint32_t end = m_firstUnit[function + 1];
            for (uint32_t unit = end; unit-- > m_firstUnit[function];) {
                uint8_t flags = m_flags[unit];
                if (!(flags & MayStandInWindow)) {
                    m_runs[unit] = 0;
                    continue;
                }
                bool isLast = unit + 1 == end || (flags & IsCall) || (m_flags[unit + 1] & IsTarget);
                m_runs[unit] = isLast ? 1 : std::min<unsigned>(longestWindow, 1 + m_runs[unit + 1]);
            }
        });
    }

    void findLongestThatMayRepeat()
    {
        unsigned numberOfChunks = (m_numberOfUnits + unitsInChunk - 1) / unitsInChunk;
        Vector<Vector<uint32_t>> candidates(numberOfChunks);
        for (unsigned length = m_options.leastLength; length <= longestWindow; ++length) {
            auto forEachCandidateIn = [&](unsigned chunk, const auto& functor) {
                if (length > m_options.leastLength) {
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
            m_seen.clearAll();
            m_seenTwice.clearAll();
            Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
                forEachCandidateIn(chunk, [&](uint32_t unit) {
                    size_t slot = hashOf(unit, length) & m_slotMask;
                    if (!m_seen.atomicSetAndCheck(slot, true))
                        m_seenTwice.atomicSetAndCheck(slot, true);
                });
            });
            std::atomic<size_t> numberLeft { 0 };
            Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
                Vector<uint32_t> left;
                forEachCandidateIn(chunk, [&](uint32_t unit) {
                    if (!m_seenTwice.at(hashOf(unit, length) & m_slotMask))
                        return;
                    m_longestThatMayRepeat[unit] = length;
                    if (m_runs[unit] > length)
                        left.append(unit);
                });
                numberLeft += left.size();
                candidates[chunk] = WTF::move(left);
            });
            size_t held = 0;
            for (auto& some : candidates)
                held += some.capacity() * sizeof(uint32_t);
            m_peak = std::max(m_peak, sizeOfUnits() + held);
            if (!numberLeft)
                break;
        }
        for (uint32_t unit = 0; unit < m_numberOfUnits; ++unit) {
            if (uint8_t length = m_longestThatMayRepeat[unit])
                m_unitsByLongest[length].append(unit);
        }
    }

    size_t sizeOfUnits() const
    {
        return m_sums.capacity() * sizeof(uint64_t) + (m_offsets.capacity() + m_firstUnit.capacity()) * sizeof(uint32_t) + m_lengths.capacity() + m_flags.capacity() + m_runs.capacity() + m_longestThatMayRepeat.capacity() + (m_slotMask + 1) / 4;
    }

    ALWAYS_INLINE uint64_t hashOf(uint32_t unit, unsigned length) const { return mix(m_sums[unit + length] - m_sums[unit] * m_powers[length]); }
    ALWAYS_INLINE uint32_t bytesOf(uint32_t unit, unsigned length) const { return m_offsets[unit + length - 1] + m_lengths[unit + length - 1] - m_offsets[unit]; }
    ALWAYS_INLINE bool isFree(uint32_t unit, unsigned length) const { return !((m_flags[unit] | m_flags[unit + length - 1]) & IsTaken); }
    unsigned functionOf(uint32_t unit) const { return std::ranges::upper_bound(m_firstUnit, unit) - m_firstUnit.begin() - 1; }

    int64_t savingOf(uint64_t places, uint64_t bytes, bool endsInCall) const
    {
        if (places < 2)
            return 0;
        int64_t sizeOfBody = static_cast<int64_t>(bytes) + (endsInCall ? 0 : sizeOfReturn);
        return static_cast<int64_t>(places) * (static_cast<int64_t>(bytes) - static_cast<int64_t>(sizeOfNearCall)) - static_cast<int64_t>(m_numberOfCopies) * sizeOfBody;
    }

    bool isTheSame(uint32_t first, unsigned functionOfFirst, uint32_t second, unsigned functionOfSecond, unsigned length) const
    {
        uint32_t bytes = bytesOf(first, length);
        if (bytesOf(second, length) != bytes)
            return false;
        auto& a = *m_functions[functionOfFirst];
        auto& b = *m_functions[functionOfSecond];
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
        return callsTheSame(a.info, callAt(a, m_offsets[first + length - 1]), b.info, callAt(b, m_offsets[second + length - 1]));
    }

    void takeWindowsOf(unsigned length)
    {
        m_active.appendVector(m_unitsByLongest[length]);
        m_unitsByLongest[length].clear();
        if (m_active.isEmpty())
            return;
        unsigned numberOfChunks = (m_active.size() + unitsInChunk - 1) / unitsInChunk;
        auto forEachPlaceIn = [&](unsigned chunk, const auto& functor) {
            size_t end = std::min<size_t>(m_active.size(), static_cast<size_t>(chunk + 1) * unitsInChunk);
            for (size_t place = static_cast<size_t>(chunk) * unitsInChunk; place < end; ++place)
                functor(place);
        };
        m_found.resize(m_active.size());
        Vector<uint32_t> starts;
        starts.fill(0, static_cast<size_t>(numberOfChunks) * numberOfShards);
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            forEachPlaceIn(chunk, [&](size_t place) {
                uint32_t unit = m_active[place];
                if (!isFree(unit, length)) {
                    m_found[place] = { 0, std::numeric_limits<uint32_t>::max() };
                    return;
                }
                uint64_t hash = hashOf(unit, length);
                m_found[place] = { hash, unit };
                ++starts[static_cast<size_t>(chunk) * numberOfShards + (hash >> 56)];
            });
        });
        std::array<uint32_t, numberOfShards + 1> startOfShard;
        uint32_t total = 0;
        for (unsigned shard = 0; shard < numberOfShards; ++shard) {
            startOfShard[shard] = total;
            for (unsigned chunk = 0; chunk < numberOfChunks; ++chunk) {
                uint32_t& start = starts[static_cast<size_t>(chunk) * numberOfShards + shard];
                uint32_t number = start;
                start = total;
                total += number;
            }
        }
        startOfShard[numberOfShards] = total;
        m_windows.resize(total);
        Typed::forEachIndexInParallel(numberOfChunks, [&](unsigned chunk) {
            forEachPlaceIn(chunk, [&](size_t place) {
                auto& window = m_found[place];
                if (window.unit != std::numeric_limits<uint32_t>::max())
                    m_windows[starts[static_cast<size_t>(chunk) * numberOfShards + (window.hash >> 56)]++] = window;
            });
        });
        std::array<Vector<Family>, numberOfShards> familiesOfShard;
        Typed::forEachIndexInParallel(numberOfShards, [&](unsigned shard) {
            auto windows = m_windows.mutableSpan().subspan(startOfShard[shard], startOfShard[shard + 1] - startOfShard[shard]);
            std::ranges::sort(windows, [](const Window& a, const Window& b) {
                return a.hash != b.hash ? a.hash < b.hash : a.unit < b.unit;
            });
            for (size_t begin = 0; begin < windows.size();) {
                size_t end = begin + 1;
                while (end < windows.size() && windows[end].hash == windows[begin].hash)
                    ++end;
                uint64_t places = 0;
                uint32_t behindLast = 0;
                for (size_t i = begin; i < end; ++i) {
                    if (places && windows[i].unit < behindLast)
                        continue;
                    ++places;
                    behindLast = windows[i].unit + length;
                }
                uint32_t first = windows[begin].unit;
                if (int64_t saving = savingOf(places, bytesOf(first, length), m_flags[first + length - 1] & IsCall); saving >= leastSaving)
                    familiesOfShard[shard].append({ saving, static_cast<uint32_t>(startOfShard[shard] + begin), static_cast<uint32_t>(startOfShard[shard] + end), first });
                begin = end;
            }
        });
        Vector<Family> families;
        for (auto& some : familiesOfShard)
            families.appendVector(some);
        std::ranges::sort(families, [](const Family& a, const Family& b) {
            return a.saving != b.saving ? a.saving > b.saving : a.firstUnit < b.firstUnit;
        });
        size_t waiting = 0;
        for (auto& some : m_unitsByLongest)
            waiting += some.capacity() * sizeof(uint32_t);
        m_peak = std::max(m_peak, sizeOfUnits() + waiting + (m_active.capacity() + starts.capacity()) * sizeof(uint32_t) + (m_found.capacity() + m_windows.capacity()) * sizeof(Window) + 2 * families.capacity() * sizeof(Family));
        Vector<std::pair<uint32_t, unsigned>> chosen;
        for (auto& family : families) {
            chosen.shrink(0);
            uint32_t behindLast = 0;
            for (uint32_t i = family.begin; i < family.end; ++i) {
                uint32_t unit = m_windows[i].unit;
                if ((!chosen.isEmpty() && unit < behindLast) || !isFree(unit, length))
                    continue;
                unsigned function = functionOf(unit);
                if (!chosen.isEmpty() && !isTheSame(chosen[0].first, chosen[0].second, unit, function, length))
                    continue;
                chosen.append({ unit, function });
                behindLast = unit + length;
            }
            if (chosen.isEmpty())
                continue;
            uint32_t first = chosen[0].first;
            uint32_t bytes = bytesOf(first, length);
            bool endsInCall = m_flags[first + length - 1] & IsCall;
            int64_t saving = savingOf(chosen.size(), bytes, endsInCall);
            if (saving < leastSaving)
                continue;
            uint32_t body = m_result.bodies.size();
            uint32_t onlyCaller = chosen[0].second;
            for (auto& [unit, function] : chosen) {
                for (unsigned i = 0; i < length; ++i)
                    m_flags[unit + i] |= IsTaken;
                m_result.places[function].append({ m_offsets[unit], bytes, body });
                if (function != chosen[0].second)
                    onlyCaller = Outlining::none;
            }
            m_result.bodies.append({ chosen[0].second, m_offsets[first], bytes, static_cast<uint32_t>(chosen.size()), onlyCaller, endsInCall });
            m_saved[length] += saving;
            m_places[length] += chosen.size();
            ++m_bodies[length];
        }
        m_active.removeAllMatching([&](uint32_t unit) {
            return m_flags[unit] & IsTaken;
        });
    }

    void report() const
    {
        int64_t saved = 0;
        uint64_t places = 0;
        uint64_t sizeOfBodies = 0;
        uint64_t withOneCaller = 0;
        for (unsigned length = 0; length <= longestWindow; ++length) {
            saved += m_saved[length];
            places += m_places[length];
        }
        for (auto& body : m_result.bodies) {
            sizeOfBodies += body.length + (body.endsInCall ? 0 : sizeOfReturn);
            withOneCaller += body.onlyCaller != Outlining::none;
        }
        dataLogLn("AOT: outlining: ", m_numberOfUnits, " instructions in ", m_numberOfFunctions, " of ", m_functions.size(), " functions: ", m_result.bodies.size(), " bodies of ", sizeOfBodies, " bytes, ", withOneCaller, " of them called by one function, ", places, " places, ", saved, " bytes saved", m_options.onlyCounts ? ": counted only" : "");
        dataLogLn("AOT: outlining: ", m_numberOfFunctionsNotWalked, " functions of the region, ", m_bytesNotWalked, " bytes, could not be walked and are left alone; the search held at most ", m_peak, " bytes");
        uint64_t numberThatMayStand = 0;
        uint64_t bytesThatMayStand = 0;
        for (uint32_t unit = 0; unit < m_numberOfUnits; ++unit) {
            if (m_flags[unit] & MayStandInWindow) {
                ++numberThatMayStand;
                bytesThatMayStand += m_lengths[unit];
            }
        }
        dataLogLn("AOT: outlining: ", numberThatMayStand, " instructions, ", bytesThatMayStand, " bytes, may stand in a window");
        for (unsigned cause = 0; cause < numberOfCausesOfNoWalk; ++cause) {
            if (auto [functions, bytes] = m_notWalkedByCause[cause]; functions)
                dataLogLn("AOT: outlining: not walked: ", functions, " functions, ", bytes, " bytes: ", cause ? wordsOfCausesOfNoWalk[cause] : "the walk meets nothing: the function is marked as rewritten, or as one whose notes were not kept"_s);
        }
        for (unsigned length = longestWindow; length >= m_options.leastLength; --length) {
            if (m_bodies[length])
                dataLogLn("AOT: outlining: windows of ", length, ": ", m_bodies[length], " bodies, ", m_places[length], " places, ", m_saved[length], " bytes saved");
        }
    }

    const Vector<const CompiledCode*>& m_functions;
    Vector<std::pair<uint32_t, uint32_t>> m_entries;
    const OutliningOptions& m_options;
    Outlining m_result;
    Vector<uint32_t> m_firstUnit;
    Vector<uint64_t> m_sums;
    Vector<uint32_t> m_offsets;
    Vector<uint8_t> m_lengths;
    Vector<uint8_t> m_flags;
    Vector<uint8_t> m_runs;
    Vector<uint8_t> m_longestThatMayRepeat;
    std::array<Vector<uint32_t>, longestWindow + 1> m_unitsByLongest;
    Vector<uint32_t> m_active;
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
    uint32_t m_numberOfFunctions { 0 };
    uint32_t m_numberOfFunctionsNotWalked { 0 };
    size_t m_bytesNotWalked { 0 };
    std::array<std::pair<uint32_t, size_t>, numberOfCausesOfNoWalk> m_notWalkedByCause { };
    size_t m_peak { 0 };
};

} // anonymous namespace

Outlining chooseWindows(const Vector<const CompiledCode*>& functions, Vector<std::pair<uint32_t, uint32_t>>&& entriesInsideFunctions, const OutliningOptions& options)
{
    Outlining result = Chooser(functions, WTF::move(entriesInsideFunctions), options).run();
    if (options.onlyCounts)
        return { };
    return result;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

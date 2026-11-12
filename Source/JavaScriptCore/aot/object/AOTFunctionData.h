/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVarint.h"
#include "CodeType.h"
#include "JSExportMacros.h"
#include <bit>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT {

enum class TraceVisibility : uint8_t;
enum class StackTraceBuiltinKind : uint8_t;

enum class CallableKind : uint8_t { Plain, Arrow, Method };

static constexpr unsigned classObjectClosureStructure = 4;
static constexpr unsigned numberOfClosureStructures = classObjectClosureStructure + 1;
static constexpr unsigned numberOfSlimClosureKinds = 2;

struct FunctionKindData {
    uint16_t closuresGetOnlyListedNames : 1;
    uint16_t prototypePropertyStaysAsMade : 1;
    uint16_t unusedKindBits : 1 { 0 };
    uint16_t isSlim : 1;
    uint16_t isAsync : 1;
    uint16_t isGenerator : 1;
    uint16_t takesNewTarget : 1;
    uint16_t isCommonJSModule : 1;
    uint16_t isInRuntimeModule : 1;
    uint16_t accessorKind : 2;
    uint16_t isConstructor : 1;
    uint16_t callableKind : 2;
    uint16_t isDerivedConstructor : 1;
    uint16_t hasInferredName : 1;

    constexpr unsigned closureStructureOfKind() const { return isConstructor ? classObjectClosureStructure : isAsync | isGenerator << 1; }
    constexpr unsigned slimClosureKind() const { return isAsync; }
    constexpr CallableKind callable() const { return static_cast<CallableKind>(callableKind); }
    constexpr MethodKind accessor() const { return static_cast<MethodKind>(accessorKind); }
    constexpr bool isConstructiblePlainFunction() const { return !isConstructor && callable() == CallableKind::Plain && !isAsync && !isGenerator; }
    constexpr bool hasInitialNamePrefix() const { return accessor() != MethodKind::Method; }
};
static_assert(sizeof(FunctionKindData) == 2 && isImageRecord<FunctionKindData>);

constexpr unsigned closureStructureOfGeneratorFunctions(bool isAsync) { return FunctionKindData { .isAsync = isAsync, .isGenerator = true }.closureStructureOfKind(); }

struct FunctionTraceData {
    uint16_t visibilityBits : 2;
    uint16_t builtinKindBits : 2;
    uint16_t codeTypeBits : 2;
    uint16_t rejectsOnThrow : 1;
    uint16_t isStrict : 1;
    uint16_t isConstructor : 1;
    uint16_t isAsync : 1;
    uint16_t isNamedByOwner : 1;
    uint16_t isNakedConstructor : 1;
    uint16_t implementsBuiltin : 1;
    uint16_t isNamedByComputedKey : 1;
    uint16_t makesPromiseOfCall : 1;
    uint16_t closuresAreSaidNeverToReachUnseenCode : 1 { 0 };

    TraceVisibility visibility() const { return static_cast<TraceVisibility>(visibilityBits); }
    static constexpr uint16_t kindBitsInsideHelperOfCompiler = 2;
    bool isInsideHelperOfCompiler() const { return !implementsBuiltin && builtinKindBits == kindBitsInsideHelperOfCompiler; }
    bool isHelperOfCompiler() const { return implementsBuiltin ? !builtinKindBits : isInsideHelperOfCompiler(); }
    StackTraceBuiltinKind builtinKind() const { return static_cast<StackTraceBuiltinKind>(isInsideHelperOfCompiler() ? 0 : builtinKindBits); }
    CodeType codeType() const { return static_cast<CodeType>(codeTypeBits); }
};
static_assert(sizeof(FunctionTraceData) == 2 && isImageRecord<FunctionTraceData>);

struct FunctionRecordData {
    FunctionKindData kind;
    FunctionTraceData trace;
};
static_assert(sizeof(FunctionRecordData) == 4 && isImageRecord<FunctionRecordData>);

static constexpr uint8_t unsharedFunctionRecord = 0xff;

struct UnsharedFunctionRecordData {
    uint32_t function;
    FunctionRecordData record;
};
static_assert(sizeof(UnsharedFunctionRecordData) == 8 && isImageRecord<UnsharedFunctionRecordData>);

struct FunctionTraceRecord : FunctionTraceData {
    uint32_t sourceFunctionIndex;
    uint32_t implementedBuiltinPlusOne;

    FunctionID sourceFunction() const { return FunctionID(sourceFunctionIndex); }
    std::optional<BuiltinID> implements() const
    {
        if (!implementedBuiltinPlusOne)
            return std::nullopt;
        return BuiltinID(implementedBuiltinPlusOne - 1);
    }
};

struct ImplementedBuiltinData {
    uint32_t function;
    uint32_t builtin;
};
static_assert(sizeof(ImplementedBuiltinData) == 8 && isImageRecord<ImplementedBuiltinData>);

struct ConstructorData {
    uint32_t function;
    uint32_t layoutIndex;
};
static_assert(sizeof(ConstructorData) == 8 && isImageRecord<ConstructorData>);

struct ConstructEntrypointData {
    static constexpr uint32_t allocatesThis = 1u << 31;
    static constexpr uint32_t onlyReturns = UINT32_MAX;

    uint32_t function;
    uint32_t constructEntrypointAndFlag;

    constexpr uint32_t constructEntrypoint() const { return constructEntrypointAndFlag & ~allocatesThis; }
};
static_assert(sizeof(ConstructEntrypointData) == 8 && isImageRecord<ConstructEntrypointData>);

struct ClosureFunctionRangeData {
    uint32_t first;
    uint32_t end;
    uint32_t omittedWordCount;
};
static_assert(sizeof(ClosureFunctionRangeData) == 12 && isImageRecord<ClosureFunctionRangeData>);

struct NumberGroup {
    static constexpr unsigned size = 32;
    static constexpr unsigned numberOfWords = 2;
    static constexpr uint32_t isListedFlag = 1u << 31;
    static constexpr size_t numberOfGroupWords(size_t numberOfEntries) { return (numberOfEntries + size - 1) / size * numberOfWords; }
};

class SparseNumberTable {
public:
    explicit SparseNumberTable(std::span<const uint32_t> words)
        : m_words(words)
    {
    }

    uint32_t at(uint32_t entry) const
    {
        std::span group = m_words.subspan(entry / NumberGroup::size * NumberGroup::numberOfWords, NumberGroup::numberOfWords);
        uint32_t bit = 1u << entry % NumberGroup::size;
        if (!(group[0] & bit))
            return 0;
        uint32_t rank = std::popcount(group[0] & (bit - 1));
        return group[1] & NumberGroup::isListedFlag ? m_words[(group[1] & ~NumberGroup::isListedFlag) + rank] : group[1] + rank;
    }

    JS_EXPORT_PRIVATE static Vector<uint32_t> create(std::span<const uint32_t> numbers);

private:
    std::span<const uint32_t> m_words;
};

class AscendingNumberTable {
public:
    static constexpr uint32_t maxSpan = 512;

    explicit AscendingNumberTable(std::span<const uint32_t> words)
        : m_words(words)
    {
    }

    uint32_t at(uint32_t index) const
    {
        std::span group = m_words.subspan(index / NumberGroup::size * NumberGroup::numberOfWords, NumberGroup::numberOfWords);
        uint32_t entry = index % NumberGroup::size;
        uint32_t place = group[1];
        if (place & NumberGroup::isListedFlag)
            return m_words[(place & ~NumberGroup::isListedFlag) + entry];
        if (!entry)
            return group[0];
        uint32_t word = place / 32;
        uint32_t bits = m_words[word] & ~0u << place % 32;
        uint32_t onesLeft = entry;
        while (static_cast<uint32_t>(std::popcount(bits)) < onesLeft) {
            onesLeft -= std::popcount(bits);
            bits = m_words[++word];
        }
        while (--onesLeft)
            bits &= bits - 1;
        uint32_t placeOfOne = word * 32 + std::countr_zero(bits);
        return group[0] + (placeOfOne - place) - (entry - 1);
    }

    JS_EXPORT_PRIVATE static Vector<uint32_t> create(std::span<const uint32_t> numbers);

private:
    std::span<const uint32_t> m_words;
};

struct FunctionSources {
    uint32_t numberOfEntries;
    uint32_t entriesOffset;
    uint32_t sourceOffset;
    uint32_t numberOfBlocks;
    uint32_t blockEndsOffset;
    uint32_t sourceSize;
};
static_assert(sizeof(FunctionSources) == 24 && isImageRecord<FunctionSources>);
static constexpr uint32_t sourceBlockSize = 64 * 1024;

struct FunctionSourceEntry {
    Number24 function;
    Number32 start;
    Number32 end;
};
static_assert(sizeof(FunctionSourceEntry) == 11 && isImageRecord<FunctionSourceEntry>);

struct QuotedSourceGroupData {
    uint32_t module;
    uint32_t line;
    uint32_t column;
    uint32_t offsetOfRows;
};
static_assert(sizeof(QuotedSourceGroupData) == 16 && isImageRecord<QuotedSourceGroupData>);
static constexpr unsigned quotedSourceGroupSize = 32;

static constexpr uint8_t countsAreListed = UINT8_MAX;

struct CaptureCounts {
    uint8_t references;
    uint8_t captures;

    static constexpr CaptureCounts decode(uint8_t byte) { return { static_cast<uint8_t>(byte & 15), static_cast<uint8_t>(byte >> 4) }; }
    constexpr uint8_t encoded() const { return references < 15 && captures < 15 ? static_cast<uint8_t>(references | captures << 4) : countsAreListed; }
};
static_assert(CaptureCounts { 14, 14 }.encoded() != countsAreListed && CaptureCounts::decode(CaptureCounts { 3, 14 }.encoded()).captures == 14 && CaptureCounts { 0, 15 }.encoded() == countsAreListed);

struct ClosureFacts {
    FunctionKindData kind;
    uint8_t length;
    uint8_t captureCounts;

    constexpr bool hasListedCounts() const { return length == countsAreListed || captureCounts == countsAreListed; }
};
static_assert(sizeof(ClosureFacts) == 4 && isImageRecord<ClosureFacts>);

struct FunctionWithLargeCountsData {
    Number24 function;
    CaptureCounts captureCounts;
    uint8_t bytesOfLength[2];

    constexpr uint32_t length() const { return bytesOfLength[0] | bytesOfLength[1] << 8; }
};
static_assert(sizeof(FunctionWithLargeCountsData) == 7 && isImageRecord<FunctionWithLargeCountsData>);

struct NamedCaptureData {
    Number24 closureNumber;
    uint8_t slot;
    uint32_t name : 31;
    uint32_t isVariableOfModule : 1;
};
static_assert(sizeof(NamedCaptureData) == 8 && isImageRecord<NamedCaptureData>);

static constexpr uint32_t namedCaptureIsVariableOfModule = UINT8_MAX + 1;
using NamedCaptureList = Vector<std::pair<uint32_t, uint32_t>, 8>;

class RecentNamedCaptureLists {
public:
    size_t size() const { return m_placesFromLatest.size(); }
    size_t find(const NamedCaptureList& list) const
    {
        uint32_t hash = hashOf(list);
        return m_placesFromLatest.findIf([&](uint8_t place) {
            return m_hashes[place] == hash && m_lists[place] == list;
        });
    }
    const NamedCaptureList& moveToFront(size_t index)
    {
        uint8_t place = m_placesFromLatest[index];
        m_placesFromLatest.removeAt(index);
        m_placesFromLatest.insert(0, place);
        return m_lists[place];
    }
    void add(const NamedCaptureList& list)
    {
        uint8_t place = static_cast<uint8_t>(m_lists.size());
        if (m_placesFromLatest.size() == mostLists) {
            place = m_placesFromLatest.takeLast();
            m_lists[place] = list;
            m_hashes[place] = hashOf(list);
        } else {
            m_lists.append(list);
            m_hashes.append(hashOf(list));
        }
        m_placesFromLatest.insert(0, place);
    }

private:
    static constexpr size_t mostLists = 64;

    static uint32_t hashOf(const NamedCaptureList& list)
    {
        uint32_t hash = 0;
        for (auto [slot, name] : list)
            hash = (hash ^ slot ^ name) * 0x9e3779b1u;
        return hash;
    }

    Vector<NamedCaptureList> m_lists;
    Vector<uint32_t, mostLists> m_hashes;
    Vector<uint8_t, mostLists> m_placesFromLatest;
};

class NamedCapturesWriter {
public:
    void append(uint32_t closureNumber, const NamedCaptureList& list)
    {
        RELEASE_ASSERT(!list.isEmpty() && closureNumber >= m_lastClosureNumber);
        appendVarint(m_bytes, closureNumber - m_lastClosureNumber);
        m_lastClosureNumber = closureNumber;
        if (size_t index = m_recent.find(list); index != notFound) {
            appendVarint(m_bytes, index << 1 | 1);
            m_recent.moveToFront(index);
            return;
        }
        m_recent.add(list);
        if (std::ranges::all_of(list, [](const auto& capture) { return capture.first < 62; })) {
            uint64_t slots = 0;
            for (auto [slot, name] : list)
                slots |= 1ull << slot;
            appendVarint(m_bytes, slots << 1);
            for (auto [slot, name] : list)
                appendVarint(m_bytes, name);
            return;
        }
        appendVarint(m_bytes, 0);
        appendVarint(m_bytes, list.size());
        for (auto [slot, name] : list) {
            appendVarint(m_bytes, slot);
            appendVarint(m_bytes, name);
        }
    }
    const Vector<uint8_t>& bytes() const { return m_bytes; }

private:
    Vector<uint8_t> m_bytes;
    RecentNamedCaptureLists m_recent;
    uint32_t m_lastClosureNumber { 0 };
};

inline Vector<uint8_t> encodeNamedCaptures(std::span<const NamedCaptureData> rows)
{
    NamedCapturesWriter writer;
    for (size_t first = 0; first < rows.size();) {
        NamedCaptureList list;
        size_t end = first;
        for (; end < rows.size() && rows[end].closureNumber == rows[first].closureNumber; ++end)
            list.append({ rows[end].slot | (rows[end].isVariableOfModule ? namedCaptureIsVariableOfModule : 0), rows[end].name });
        writer.append(rows[first].closureNumber, list);
        first = end;
    }
    return writer.bytes();
}

bool forEachNamedCapture(std::span<const uint8_t> bytes, const auto& visit)
{
    const uint8_t* cursor = bytes.data();
    const uint8_t* end = cursor + bytes.size();
    RecentNamedCaptureLists recent;
    uint64_t closureNumber = 0;
    auto visitAll = [&](const NamedCaptureList& list) {
        for (auto [slot, name] : list)
            visit(NamedCaptureData { Number24(static_cast<uint32_t>(closureNumber)), static_cast<uint8_t>(slot), name, !!(slot & namedCaptureIsVariableOfModule) });
    };
    while (cursor < end) {
        closureNumber += readVarint(cursor);
        if (cursor >= end || closureNumber > Number24::max)
            return false;
        uint64_t word = readVarint(cursor);
        if (word & 1) {
            if (word >> 1 >= recent.size())
                return false;
            visitAll(recent.moveToFront(word >> 1));
            continue;
        }
        NamedCaptureList list;
        if (uint64_t slots = word >> 1) {
            for (uint32_t slot = 0; slots; ++slot, slots >>= 1) {
                if (!(slots & 1))
                    continue;
                if (cursor >= end)
                    return false;
                list.append({ slot, static_cast<uint32_t>(readVarint(cursor)) });
            }
        } else {
            if (cursor >= end)
                return false;
            uint64_t numberOfCaptures = readVarint(cursor);
            for (uint64_t index = 0; index < numberOfCaptures; ++index) {
                if (cursor >= end)
                    return false;
                uint64_t slot = readVarint(cursor);
                if (cursor >= end || slot > (UINT8_MAX | namedCaptureIsVariableOfModule))
                    return false;
                list.append({ static_cast<uint32_t>(slot), static_cast<uint32_t>(readVarint(cursor)) });
            }
            if (list.isEmpty())
                return false;
        }
        visitAll(list);
        recent.add(list);
    }
    return cursor == end;
}

#define FOR_EACH_AOT_FUNCTION_PROGRAM_TABLE(v) \
    v(functionNames, Number24) \
    v(functionNameGroups, uint32_t) \
    v(functionRecordNumbers, uint8_t) \
    v(functionRecords, FunctionRecordData) \
    v(unsharedFunctionRecords, UnsharedFunctionRecordData) \
    v(sourceFunctions, uint32_t) \
    v(implementedBuiltins, ImplementedBuiltinData) \
    v(closureFactNumbers, uint16_t) \
    v(closureFacts, ClosureFacts) \
    v(functionsWithLargeCounts, FunctionWithLargeCountsData) \
    v(closureCells, uint32_t) \
    v(namedCaptures, uint8_t) \
    v(functionSources, FunctionSources) \
    v(quotedSourceGroups, QuotedSourceGroupData) \
    v(quotedSourceRows, uint8_t) \
    v(quotedSourceText, uint8_t) \
    v(quotedSourceBlocks, uint32_t) \
    v(constructors, ConstructorData) \
    v(constructEntrypoints, ConstructEntrypointData)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

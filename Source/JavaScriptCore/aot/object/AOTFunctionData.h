/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVarint.h"
#include "CodeType.h"
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT {

enum class TraceVisibility : uint8_t;
enum class StackTraceBuiltinKind : uint8_t;

enum class CallableKind : uint8_t { Plain, Arrow, Method, Bound };

static constexpr unsigned classObjectClosureStructure = 4;
static constexpr unsigned numberOfClosureStructures = classObjectClosureStructure + 1;
static constexpr unsigned numberOfSlimClosureKinds = 2;

struct FunctionKindData {
    uint16_t closureStructure : 3;
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
    constexpr bool isBound() const { return callable() == CallableKind::Bound; }
    constexpr bool isConstructiblePlainFunction() const { return !isConstructor && callable() == CallableKind::Plain && !isAsync && !isGenerator; }
    constexpr bool hasInitialNamePrefix() const { return isBound() || accessor() != MethodKind::Method; }
};
static_assert(sizeof(FunctionKindData) == 2 && isImageRecord<FunctionKindData>);
static_assert(classObjectClosureStructure < 1 << 3);

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
    // `{ [Symbol.dispose]() { } }`: the written name is what the property `name` holds. The engine gives such a function that property when the literal is evaluated.
    uint16_t isNamedByComputedKey : 1;
    // The unit of an async function that its callers enter. It makes the promise of the call.
    uint16_t makesPromiseOfCall : 1;
    // Zero: the image finds equal records by comparing all of their bits.
    uint16_t unused : 1 { 0 };

    TraceVisibility visibility() const { return static_cast<TraceVisibility>(visibilityBits); }
    // A function that is written inside a helper of the compiler implements no built-in. No other function that implements none has the kind of a constructor.
    static constexpr uint16_t kindBitsInsideHelperOfCompiler = 2;
    bool isInsideHelperOfCompiler() const { return !implementsBuiltin && builtinKindBits == kindBitsInsideHelperOfCompiler; }
    // Plain Bun keeps it in a module of its own, and a stack trace lists it like any other function.
    bool isHelperOfCompiler() const { return implementsBuiltin ? !builtinKindBits : isInsideHelperOfCompiler(); }
    StackTraceBuiltinKind builtinKind() const { return static_cast<StackTraceBuiltinKind>(isInsideHelperOfCompiler() ? 0 : builtinKindBits); }
    CodeType codeType() const { return static_cast<CodeType>(codeTypeBits); }
};
static_assert(sizeof(FunctionTraceData) == 2 && isImageRecord<FunctionTraceData>);

// A program has few different pairs of the two, so the image holds each pair once and, for every function, the number of its pair in one byte.
struct FunctionRecordData {
    FunctionKindData kind;
    FunctionTraceData trace;
};
static_assert(sizeof(FunctionRecordData) == 4 && isImageRecord<FunctionRecordData>);

// The number of a function whose pair is not among the most common ones. Its pair is listed under the function itself.
static constexpr uint8_t functionRecordIsListedByFunction = 0xff;

struct FunctionRecordByFunctionData {
    uint32_t function;
    FunctionRecordData record;
};
static_assert(sizeof(FunctionRecordByFunctionData) == 8 && isImageRecord<FunctionRecordByFunctionData>);

// What the compiler says of one function. The image has the bits of every function, the source of the functions from the first derived one on, and the few functions that implement a built-in.
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

// The functions from firstFunction up to the next run that are their own source have the name.
struct FunctionNameRunData {
    uint32_t firstFunction;
    uint32_t name;
};
static_assert(sizeof(FunctionNameRunData) == 8 && isImageRecord<FunctionNameRunData>);

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

struct BoundFunctionData {
    uint32_t function;
    uint32_t target;
    uint32_t numberOfBoundArguments : 16;
    uint32_t targetClosureSlotPlusOne : 16;
    // The target constructs, so the bound function does: with the bound arguments that its closure holds, as values, one behind the other from this slot on.
    uint32_t constructs : 1;
    uint32_t firstArgumentSlot : 15;
    uint32_t numberOfCapturedArguments : 16;

    std::optional<unsigned> targetClosureSlot() const
    {
        if (!targetClosureSlotPlusOne)
            return std::nullopt;
        return targetClosureSlotPlusOne - 1;
    }
};
static_assert(sizeof(BoundFunctionData) == 16 && isImageRecord<BoundFunctionData>);

// Both functions by closure number.
struct ConstructEntrypointData {
    // The entrypoint allocates the object itself, and is entered only where the function is its own new.target. Under any other new.target [[Construct]] enters at the entry of [[Call]] with a new object.
    static constexpr uint32_t allocatesThis = 1u << 31;
    // In place of an entrypoint: a plain function that only returns undefined, so the new object is all that its [[Construct]] makes. No stub looks for such a function here.
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

struct FunctionTexts {
    uint32_t numberOfEntries;
    uint32_t entriesOffset;
    uint32_t textOffset;
};
static_assert(sizeof(FunctionTexts) == 12 && isImageRecord<FunctionTexts>);

// In the order of the sources of the functions, so that both a function and its source lead to the text.
struct FunctionTextEntry {
    Number24 function;
    Number32 start;
    Number32 end;
};
static_assert(sizeof(FunctionTextEntry) == 11 && isImageRecord<FunctionTextEntry>);

// What fails at a place of the program quotes a part of textOfQuotes in its message: quotedTextRows holds a QuotedTextRow for each such place, in ascending order of module, line and column.
// A group is rowsInGroupOfQuotedTexts of them, fewer at the end of a module. Its first row counts from the place of the group and from the start of the text.
struct QuotedTextGroupData {
    uint32_t module;
    uint32_t line;
    uint32_t column;
    uint32_t offsetOfRows;
};
static_assert(sizeof(QuotedTextGroupData) == 16 && isImageRecord<QuotedTextGroupData>);
static constexpr unsigned rowsInGroupOfQuotedTexts = 32;

// In functionLengths and in closureCaptureCounts: the function stands in functionsWithLargeCounts.
static constexpr uint8_t countsAreListed = UINT8_MAX;

struct CaptureCounts {
    uint8_t references;
    uint8_t captures;

    static constexpr CaptureCounts decode(uint8_t byte) { return { static_cast<uint8_t>(byte & 15), static_cast<uint8_t>(byte >> 4) }; }
    constexpr uint8_t encoded() const { return references < 15 && captures < 15 ? static_cast<uint8_t>(references | captures << 4) : countsAreListed; }
};
static_assert(CaptureCounts { 14, 14 }.encoded() != countsAreListed && CaptureCounts::decode(CaptureCounts { 3, 14 }.encoded()).captures == 14 && CaptureCounts { 0, 15 }.encoded() == countsAreListed);

// In the order of the functions.
struct FunctionWithLargeCountsData {
    Number24 function;
    CaptureCounts captureCounts;
    uint8_t bytesOfLength[2];

    constexpr uint32_t length() const { return bytesOfLength[0] | bytesOfLength[1] << 8; }
};
static_assert(sizeof(FunctionWithLargeCountsData) == 7 && isImageRecord<FunctionWithLargeCountsData>);

// Under what a heap snapshot shows a reference that every closure of a function holds. In ascending order of closure number and slot. Only a heap snapshot reads them.
// The image holds them as bytes: NamedCapturesWriter, forEachNamedCapture().
struct NamedCaptureData {
    Number24 closureNumber;
    uint8_t slot;
    // The number of the string.
    uint32_t name;
};
static_assert(sizeof(NamedCaptureData) == 8 && isImageRecord<NamedCaptureData>);

// The slots and names of one closure number, the slots rising.
using NamedCaptureList = Vector<std::pair<uint32_t, uint32_t>, 8>;

// The lists that were written or read last, each once, the latest first. Closures made side by side hold the same variables.
class RecentNamedCaptureLists {
public:
    size_t size() const { return m_lists.size(); }
    size_t find(const NamedCaptureList& list) const { return m_lists.find(list); }
    const NamedCaptureList& moveToFront(size_t index)
    {
        NamedCaptureList list = WTF::move(m_lists[index]);
        m_lists.removeAt(index);
        m_lists.insert(0, WTF::move(list));
        return m_lists[0];
    }
    void add(const NamedCaptureList& list)
    {
        if (m_lists.size() == 64)
            m_lists.removeLast();
        m_lists.insert(0, list);
    }

private:
    Vector<NamedCaptureList> m_lists;
};

// The table namedCaptures, for each closure number that has such references, in ascending order: the number less the one before, then a word.
// Odd: above the bit, which of the recent lists is the list of this number too. Even: above the bit, the slots as bits, then a name for each.
// Zero: how many, then the slot and the name of each.
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
        if (list.last().first < 62) {
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
            list.append({ rows[end].slot, rows[end].name });
        writer.append(rows[first].closureNumber, list);
        first = end;
    }
    return writer.bytes();
}

// False if the bytes do not end where a closure number ends, name a list that is not there, or hold a number that no row holds.
bool forEachNamedCapture(std::span<const uint8_t> bytes, const auto& visit)
{
    const uint8_t* cursor = bytes.data();
    const uint8_t* end = cursor + bytes.size();
    RecentNamedCaptureLists recent;
    uint64_t closureNumber = 0;
    auto visitAll = [&](const NamedCaptureList& list) {
        for (auto [slot, name] : list)
            visit(NamedCaptureData { Number24(static_cast<uint32_t>(closureNumber)), static_cast<uint8_t>(slot), name });
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
                if (cursor >= end || slot > UINT8_MAX)
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
    v(functionNameRuns, FunctionNameRunData) \
    v(functionRecordNumbers, uint8_t) \
    v(functionRecords, FunctionRecordData) \
    v(functionRecordsByFunction, FunctionRecordByFunctionData) \
    v(functionSources, Number24) \
    v(implementedBuiltins, ImplementedBuiltinData) \
    v(closureFunctionKinds, FunctionKindData) \
    v(functionLengths, uint8_t) \
    v(closureCaptureCounts, uint8_t) \
    v(functionsWithLargeCounts, FunctionWithLargeCountsData) \
    v(closureCellNumbers, Number24) \
    v(namedCaptures, uint8_t) \
    v(functionTexts, FunctionTexts) \
    v(quotedTextGroups, QuotedTextGroupData) \
    v(quotedTextRows, uint8_t) \
    v(textOfQuotes, uint8_t) \
    v(constructors, ConstructorData) \
    v(boundFunctions, BoundFunctionData) \
    v(constructEntrypoints, ConstructEntrypointData)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

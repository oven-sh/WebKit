/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVarint.h"
#include "CodeType.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/IterationStatus.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

enum class TraceVisibility : uint8_t;
enum class StackTraceBuiltinKind : uint8_t;

enum class CallableKind : uint8_t { Plain, Arrow, Method, Bound };

static constexpr unsigned classObjectClosureStructure = 4;
static constexpr unsigned numberOfClosureStructures = classObjectClosureStructure + 1;
static constexpr unsigned numberOfSlimClosureKinds = 2;
// In FunctionKindData::accessorKind, beside the members of MethodKind: no accessor, and a stack frame shows another name once something has looked for the property `name` of the function object.
static constexpr unsigned methodThatNotesLooksForName = 3;

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
    constexpr MethodKind accessor() const { return accessorKind == methodThatNotesLooksForName ? MethodKind::Method : static_cast<MethodKind>(accessorKind); }
    constexpr bool notesLooksForName() const { return accessorKind != static_cast<unsigned>(MethodKind::Method); }
    constexpr bool isBound() const { return callable() == CallableKind::Bound; }
    // The function object stays as it is: the name that the look brings into its frames is the one that the tables have.
    constexpr bool notesLooksForNameInCell() const { return accessorKind == methodThatNotesLooksForName && !isBound(); }
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
    // `{ [Symbol.dispose]() { } }`: the written name is what the property `name` holds. The engine gives such a function that property when the literal is evaluated, and the console goes on showing it without a name.
    uint16_t isNamedByComputedKey : 1;
    // Its frames are named by the function object that runs in them.
    uint16_t recordsCallee : 1;
    // They are from the first time on that the run time sees a function object of the function get a displayName.
    uint16_t recordsCalleeOnceRenamed : 1;

    TraceVisibility visibility() const { return static_cast<TraceVisibility>(visibilityBits); }
    StackTraceBuiltinKind builtinKind() const { return static_cast<StackTraceBuiltinKind>(builtinKindBits); }
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

    uint32_t function;
    uint32_t constructEntrypointAndFlag;

    constexpr uint32_t constructEntrypoint() const { return constructEntrypointAndFlag & ~allocatesThis; }
};
static_assert(sizeof(ConstructEntrypointData) == 8 && isImageRecord<ConstructEntrypointData>);

// Where a plain function makes a call in tail position. A function that is constructed is listed in stack traces until that call has returned, though its frame is gone.
// ProgramTables::placesOfTailCalls has them one behind the other, ascending by function. Each says how far it is from the one in front.
struct TailCallPlace {
    static constexpr uint32_t noCallee = UINT32_MAX;

    void append(Vector<uint8_t>& bytes, const TailCallPlace& inFront) const
    {
        bool namesCallee = callee != noCallee;
        bool isInOtherSource = sourcePlusOne != inFront.sourcePlusOne;
        appendVarint(bytes, static_cast<uint64_t>(function - inFront.function) << 2 | isInOtherSource << 1 | namesCallee);
        if (isInOtherSource)
            appendVarint(bytes, sourcePlusOne);
        appendVarint(bytes, zigZagEncode(static_cast<int64_t>(line) - inFront.line));
        appendVarint(bytes, column);
        if (namesCallee)
            appendVarint(bytes, callee);
    }

    static TailCallPlace read(const uint8_t*& cursor, const TailCallPlace& inFront)
    {
        TailCallPlace place;
        uint64_t head = readVarint(cursor);
        place.function = inFront.function + static_cast<uint32_t>(head >> 2);
        place.sourcePlusOne = head & 2 ? static_cast<uint32_t>(readVarint(cursor)) : inFront.sourcePlusOne;
        place.line = static_cast<uint32_t>(inFront.line + zigZagDecode(readVarint(cursor)));
        place.column = static_cast<uint32_t>(readVarint(cursor));
        if (head & 1)
            place.callee = static_cast<uint32_t>(readVarint(cursor));
        return place;
    }

    // Both as they are written. The callee is named where the function has several places and the call says whom it enters.
    uint32_t function { 0 };
    uint32_t callee { noCallee };
    // All three are zero in an image without positions.
    uint32_t sourcePlusOne { 0 };
    uint32_t line { 0 };
    uint32_t column { 0 };
};

inline void forEachTailCallPlace(std::span<const uint8_t> bytes, NOESCAPE const Invocable<IterationStatus(const TailCallPlace&)> auto& functor)
{
    TailCallPlace place;
    for (const uint8_t* cursor = bytes.data(); cursor < std::to_address(bytes.end());) {
        place = TailCallPlace::read(cursor, place);
        if (functor(place) == IterationStatus::Done)
            return;
    }
}

struct ClosureFunctionRangeData {
    uint32_t first;
    uint32_t end;
    uint32_t omittedWordCount;
    // The closure number of `first`. Not zero in an image that was built above others for the same realms: a closure says by its number which of them it is of.
    uint32_t numbersBelow;
};
static_assert(sizeof(ClosureFunctionRangeData) == 16 && isImageRecord<ClosureFunctionRangeData>);

struct FunctionTexts {
    uint32_t numberOfEntries;
    uint32_t entriesOffset;
    uint32_t textOffset;
};
static_assert(sizeof(FunctionTexts) == 12 && isImageRecord<FunctionTexts>);

// In the order of the sources of the functions, so that both a function and its source lead to the text.
struct FunctionTextEntry {
    uint32_t function;
    uint32_t start;
    uint32_t end;
};
static_assert(sizeof(FunctionTextEntry) == 12 && isImageRecord<FunctionTextEntry>);

// The text of a constructor is that of its class, and the constructor begins at its parameters. This is how many lines of the text stand above them, where there are any.
// Ascending by the function, which is its own source.
struct ConstructorLineData {
    uint32_t function;
    uint32_t linesOfTextAbove;
};
static_assert(sizeof(ConstructorLineData) == 8 && isImageRecord<ConstructorLineData>);

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

#define FOR_EACH_AOT_FUNCTION_PROGRAM_TABLE(v) \
    v(functionNames, Number24) \
    v(functionNameGroups, uint32_t) \
    v(functionNameRuns, FunctionNameRunData) \
    v(functionRecordNumbers, uint8_t) \
    v(functionRecords, FunctionRecordData) \
    v(functionRecordsByFunction, FunctionRecordByFunctionData) \
    v(functionSources, Number24) \
    v(closureFormsBySource, Number24) \
    v(implementedBuiltins, ImplementedBuiltinData) \
    v(closureFunctionKinds, FunctionKindData) \
    v(functionLengths, uint8_t) \
    v(closureCaptureCounts, uint8_t) \
    v(functionsWithLargeCounts, FunctionWithLargeCountsData) \
    v(closureCellNumbers, Number24) \
    v(functionTexts, FunctionTexts) \
    v(constructorLines, ConstructorLineData) \
    v(constructors, ConstructorData) \
    v(boundFunctions, BoundFunctionData) \
    v(constructEntrypoints, ConstructEntrypointData) \
    v(placesOfTailCalls, uint8_t)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "CodeType.h"
#include <optional>

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

    constexpr unsigned slimClosureKind() const { return isAsync; }
    constexpr CallableKind callable() const { return static_cast<CallableKind>(callableKind); }
    constexpr MethodKind accessor() const { return static_cast<MethodKind>(accessorKind); }
    constexpr bool hasInitialNamePrefix() const { return callable() == CallableKind::Bound || accessor() != MethodKind::Method; }
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
    uint16_t unused : 3;

    TraceVisibility visibility() const { return static_cast<TraceVisibility>(visibilityBits); }
    StackTraceBuiltinKind builtinKind() const { return static_cast<StackTraceBuiltinKind>(builtinKindBits); }
    CodeType codeType() const { return static_cast<CodeType>(codeTypeBits); }
};
static_assert(sizeof(FunctionTraceData) == 2 && isImageRecord<FunctionTraceData>);

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

    std::optional<unsigned> targetClosureSlot() const
    {
        if (!targetClosureSlotPlusOne)
            return std::nullopt;
        return targetClosureSlotPlusOne - 1;
    }
};
static_assert(sizeof(BoundFunctionData) == 12 && isImageRecord<BoundFunctionData>);

struct ConstructEntrypointData {
    uint32_t function;
    uint32_t constructEntrypoint;
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

struct FunctionTextEntry {
    uint32_t function;
    uint32_t start;
    uint32_t end;
};
static_assert(sizeof(FunctionTextEntry) == 12 && isImageRecord<FunctionTextEntry>);

struct CaptureCounts {
    uint8_t references;
    uint8_t captures;

    static constexpr CaptureCounts decode(uint16_t word) { return { static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8) }; }
    constexpr uint16_t encoded() const { return static_cast<uint16_t>(references | captures << 8); }
};

#define FOR_EACH_AOT_FUNCTION_PROGRAM_TABLE(v) \
    v(functionNames, uint32_t) \
    v(functionKinds, FunctionKindData) \
    v(functionTraces, FunctionTraceData) \
    v(functionSources, uint32_t) \
    v(implementedBuiltins, ImplementedBuiltinData) \
    v(functionLengths, uint16_t) \
    v(closureCaptureCounts, uint16_t) \
    v(functionTexts, FunctionTexts) \
    v(constructors, ConstructorData) \
    v(boundFunctions, BoundFunctionData) \
    v(constructEntrypoints, ConstructEntrypointData)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

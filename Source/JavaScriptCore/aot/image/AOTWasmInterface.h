/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(WEBASSEMBLY)

#include "WasmFormat.h"
#include <array>
#include <optional>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC {

namespace AOT {
class RelocationRecorder;
}

namespace B3 {
class CompilationScratch;
struct Policy;
enum class RuntimeFunction : uint32_t;
}

namespace Wasm {

class RTT;
struct ModuleInformation;

#define FOR_EACH_WASM_IMAGE_CODE_OPERATION(macro) \
    macro(operationThrowExceptionFromOMG) \
    macro(operationGrowMemory) \
    macro(operationWasmMemorySizeInPages) \
    macro(operationWasmMemoryFill) \
    macro(operationWasmMemoryCopy) \
    macro(operationWasmMemoryInit) \
    macro(operationWasmDataDrop) \
    macro(operationMemoryMove) \
    macro(operationMemoryFill) \
    macro(operationMemoryAtomicWait32) \
    macro(operationMemoryAtomicWait64) \
    macro(operationMemoryAtomicNotify) \
    macro(operationGetWasmTableElement) \
    macro(operationSetWasmTableElement) \
    macro(operationWasmTableInit) \
    macro(operationWasmTableGrow) \
    macro(operationWasmTableFill) \
    macro(operationWasmTableCopy) \
    macro(operationWasmElemDrop) \
    macro(operationWasmRefFunc) \
    macro(operationWasmWriteBarrierSlowPath) \
    macro(operationPopcount32) \
    macro(operationPopcount64)

enum class ImageCodeOperation : uint32_t {
#define DECLARE_OPERATION(operation) operation,
    FOR_EACH_WASM_IMAGE_CODE_OPERATION(DECLARE_OPERATION)
#undef DECLARE_OPERATION
};

enum class ImageDeclineReason : uint8_t {
    None,
    UnsupportedCPU,
    InvalidModule,
    NoFunctions,
    HasGCTypes,
    CodeTooLarge,
    UsesExceptions,
    UsesTailCalls,
    TestsReferenceTypes,
    CallsUnlistedOperation,
    NamesTypeOfOtherModule,
    SavesVectorRegister,
    SavedRegisterOrder,
    ProcessAddressInCode,
    CodeGeneratorFailed,
};

enum class InterpreterFallbackReason : uint8_t {
    MemoryChecksBounds,
    VectorsAreDisabled,
};

static constexpr unsigned numberOfInterpreterFallbackReasons = 2;
static_assert(static_cast<unsigned>(InterpreterFallbackReason::VectorsAreDisabled) + 1 == numberOfInterpreterFallbackReasons);

// In the records of the directory an array field is an offset from the start of the directory, and a code offset counts from the start of the image's WebAssembly code.
// The ModuleInImage records follow the header, and the DeclinedModuleInImage records follow those.
struct DirectoryInImage {
    uint32_t numberOfModules;
    uint32_t numberOfDeclinedModules;
};

struct FunctionInImage {
    uint32_t entry;
    uint16_t savedRegisters;
    uint8_t hasCode;
    uint8_t usesVectors;
};
static_assert(sizeof(FunctionInImage) == 8);

struct ModuleInImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize;
    uint32_t numberOfFunctions;
    uint32_t numberOfImportedFunctions;
    uint32_t functions;
    uint32_t functionsEnd;
    uint32_t importStubs;
    uint32_t savedRegisters;
    uint32_t isForSignalingMemory;
    uint32_t numberOfJSEntrypoints;
    uint32_t jsEntrypoints;
    uint32_t usesVectors;
};
static_assert(sizeof(ModuleInImage) == 64);

// A module of the program that has no code in the image at all.
struct DeclinedModuleInImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize;
};
static_assert(sizeof(DeclinedModuleInImage) == 24);

struct JSEntrypointInImage {
    uint32_t type;
    uint32_t code;
};

struct SavedRegistersInImage {
    uint64_t registers;
    int32_t offsetOfFirst;
    uint32_t unused;

    friend bool operator==(const SavedRegistersInImage&, const SavedRegistersInImage&) = default;
};

struct ImagePointerTable {
#define COUNT_OPERATION(operation) + 1
    static constexpr uint32_t numberOfOperations = 0 FOR_EACH_WASM_IMAGE_CODE_OPERATION(COUNT_OPERATION);
#undef COUNT_OPERATION
    static constexpr uint32_t genericJSEntrypoint = numberOfOperations;
    static constexpr uint32_t interpreterEntrypoint = numberOfOperations + 1;
    static constexpr uint32_t numberOfSharedPointers = numberOfOperations + 2;

    static constexpr uint32_t slotOf(ImageCodeOperation operation) { return static_cast<uint32_t>(operation); }
    JS_EXPORT_PRIVATE static std::optional<uint32_t> slotOfOperation(const void* taggedOperation);
    JS_EXPORT_PRIVATE static ImagePointerTable forModule(const ModuleInformation&, uint32_t numberOfJSEntrypoints);

    uint32_t callee(FunctionCodeIndex function) const { return numberOfSharedPointers + static_cast<uint32_t>(function.rawIndex()); }
    uint32_t rtt(TypeSignatureIndex type) const { return numberOfSharedPointers + numberOfFunctions + type.rawIndex(); }
    uint32_t jsEntrypointCallee(uint32_t entry) const { return numberOfSharedPointers + numberOfFunctions + numberOfTypes + entry; }
    uint32_t size() const { return numberOfSharedPointers + numberOfFunctions + numberOfTypes + numberOfJSEntrypoints; }

    uint32_t numberOfFunctions { 0 };
    uint32_t numberOfTypes { 0 };
    uint32_t numberOfJSEntrypoints { 0 };
};

JS_EXPORT_PRIVATE std::optional<uint32_t> runtimeFunctionSlot(B3::RuntimeFunction);

// Code offsets count from the start of code.
struct ModuleForImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize { 0 };
    bool isForSignalingMemory { false };
    bool usesVectors { false };
    Vector<uint8_t> code;
    Vector<FunctionInImage> functions;
    uint32_t functionsEnd { 0 };
    Vector<uint32_t> importStubs;
    Vector<JSEntrypointInImage> jsEntrypoints;
    Vector<SavedRegistersInImage> savedRegisters;
    Vector<std::pair<uint32_t, ImageDeclineReason>> interpretedFunctions;
};

struct ImageCallTarget {
    enum class Kind : uint8_t {
        ThrowStub,
        Function,
    };

    Kind kind;
    FunctionCodeIndex function;
};

struct CodeForImage {
    const ImagePointerTable& pointers;
    const UncheckedKeyHashMap<const RTT*, uint32_t>& typeIndices;
    AOT::RelocationRecorder& recorder;
    const B3::Policy& policy;
    B3::CompilationScratch& scratch;
    uint32_t pointerSpace;
    ImageDeclineReason declineReason { ImageDeclineReason::None };
    Vector<ImageCallTarget> callTargets { };
};

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY)

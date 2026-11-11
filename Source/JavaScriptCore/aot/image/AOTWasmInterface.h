/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Platform.h>

#if ENABLE(WEBASSEMBLY)

#include "AOTVector.h"
#include "WasmFormat.h"
#include <array>
#include <bmalloc/Gigacage.h>
#include <optional>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

namespace AOT {
class RelocationRecorder;
}

namespace B3 {
class CompilationScratch;
struct Policy;
}

namespace Wasm {

class RTT;
struct ModuleInformation;

#define FOR_EACH_WASM_IMAGE_CODE_OPERATION(macro) \
    macro(operationWasmToJSException) \
    macro(operationWasmUnwind) \
    macro(operationWasmThrow) \
    macro(operationWasmRethrow) \
    macro(operationWasmRetrieveAndClearExceptionIfCatchable) \
    macro(operationGrowMemory) \
    macro(operationWasmMemorySizeInPages) \
    macro(operationWasmMemoryFill) \
    macro(operationWasmMemoryCopy) \
    macro(operationWasmMemoryInit) \
    macro(operationWasmDataDrop) \
    macro(operationMemoryMove) \
    macro(operationGCSafeMemoryMove) \
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
    macro(operationWasmRefTest) \
    macro(operationWasmAnyConvertExtern) \
    macro(operationWasmStructNewEmpty) \
    macro(operationWasmArrayNewEmpty) \
    macro(operationWasmArrayNewData) \
    macro(operationWasmArrayNewElem) \
    macro(operationWasmArrayInitData) \
    macro(operationWasmArrayInitElem) \
    macro(operationWasmArrayFill2B) \
    macro(operationWasmArrayFill4B) \
    macro(operationWasmArrayFill8B) \
    macro(operationWasmArrayFill16B) \
    macro(operationWasmArrayFillRefs) \
    macro(operationWasmWriteBarrierSlowPath) \
    macro(operationPopcount32) \
    macro(operationPopcount64) \
    macro(operationConvertToI32) \
    macro(operationConvertToI64) \
    macro(operationConvertToF32) \
    macro(operationConvertToF64) \
    macro(operationConvertToBigInt) \
    macro(operationConvertToFuncref) \
    macro(operationConvertToAnyref) \
    macro(operationIterateResults)

enum class ImageCodeOperation : uint32_t {
#define DECLARE_OPERATION(operation) operation,
    FOR_EACH_WASM_IMAGE_CODE_OPERATION(DECLARE_OPERATION)
#undef DECLARE_OPERATION
};

#define FOR_EACH_WASM_IMAGE_DECLINE_REASON(macro) \
    macro(UnsupportedCPU) \
    macro(InvalidModule) \
    macro(CodeTooLarge) \
    macro(FunctionNotCompiled)

enum class ImageDeclineReason : uint8_t {
    None,
#define DECLARE_REASON(reason) reason,
    FOR_EACH_WASM_IMAGE_DECLINE_REASON(DECLARE_REASON)
#undef DECLARE_REASON
};

enum class ImageCodeFailure : uint8_t {
    None,
    NamesTypeOfOtherModule,
    SavesVectorRegister,
    SavedRegisterOrder,
    TooManySavedRegisterSets,
    ProcessAddressInCode,
    OutOfPCRelativeReach,
    OutOfMemory,
};

enum class ImageCodeFlavor : uint8_t {
    Checking,
    Trapping,
};
static constexpr unsigned numberOfImageCodeFlavors = 2;

static constexpr ptrdiff_t lowestAccessibleAddressOfEveryTarget = 4 * KB;

enum class ImageStub : uint32_t {
    ThrowException,
    ThrowStackOverflow,
    CatchRunway,
    FaultWithoutFrame,
};
static constexpr uint32_t numberOfImageStubs = 4;
static_assert(static_cast<uint32_t>(ImageStub::FaultWithoutFrame) + 1 == numberOfImageStubs);

enum class ImageEntry : uint16_t {
    Primary,
    Direct,
    DirectWithZero,
};

constexpr uint32_t imageSymbolOf(ImageStub stub) { return static_cast<uint32_t>(stub); }
inline uint32_t imageSymbolOf(FunctionCodeIndex function) { return numberOfImageStubs + static_cast<uint32_t>(function.rawIndex()); }

struct DirectoryInImage {
    uint32_t numberOfModules;
    uint32_t numberOfDeclinedModules;
    uint32_t catchRunway;
    uint32_t faultWithoutFrame;
};
static_assert(sizeof(DirectoryInImage) == 16);

struct FramelessRangeInImage {
    friend constexpr auto operator<=>(const FramelessRangeInImage&, const FramelessRangeInImage&) = default;

    uint32_t begin;
    uint32_t end;
};
static_assert(sizeof(FramelessRangeInImage) == 8);

struct FunctionInImage {
    uint32_t entry;
    uint32_t size;
    uint32_t firstHandler;
    uint32_t numberOfHandlers;
    uint16_t savedRegisters;
    uint8_t usesVectors;
    uint8_t unused;
};
static_assert(sizeof(FunctionInImage) == 20);

struct HandlerInImage {
    uint32_t type;
    uint32_t start;
    uint32_t end;
    uint32_t tryDepth;
    uint32_t exceptionIndexOrDelegateTarget;
    uint32_t code;
};
static_assert(sizeof(HandlerInImage) == 24);

struct ModuleInImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize;
    uint32_t numberOfFunctions;
    uint32_t numberOfImportedFunctions;
    std::array<uint32_t, numberOfImageCodeFlavors> functions;
    uint32_t handlers;
    uint32_t importStubs;
    uint32_t javaScriptImportStubs;
    uint32_t savedRegisters;
    uint32_t numberOfJSEntrypoints;
    uint32_t jsEntrypoints;
    uint32_t usesVectors;
    uint32_t framelessRanges;
    uint32_t numberOfFramelessRanges;
    uint32_t unused;
};
static_assert(sizeof(ModuleInImage) == 80);

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
    static constexpr uint32_t defaultJSCall = numberOfOperations + 1;
    static constexpr uint32_t wasmToJSCallee = numberOfOperations + 2;
    static constexpr uint32_t restoreFrameCallee = numberOfOperations + 3;
    static constexpr uint32_t restoreFrameReturnAddress = numberOfOperations + 4;
    static constexpr uint32_t structureIDBase = numberOfOperations + 5;
    static constexpr uint32_t throwFromFaultHandler = numberOfOperations + 6;
#if GIGACAGE_ENABLED
    static constexpr uint32_t primitiveGigacageDisableRequest = numberOfOperations + 7;
    static constexpr uint32_t primitiveGigacageBase = numberOfOperations + 8;
    static constexpr uint32_t numberOfSharedPointers = numberOfOperations + 9;
#else
    static constexpr uint32_t numberOfSharedPointers = numberOfOperations + 7;
#endif

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

struct ModuleForImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize { 0 };
    bool usesVectors { false };
    Vector<uint8_t> code;
    std::array<Vector<FunctionInImage>, numberOfImageCodeFlavors> functions;
    Vector<HandlerInImage> handlers;
    Vector<uint32_t> importStubs;
    Vector<uint32_t> javaScriptImportStubs;
    Vector<JSEntrypointInImage> jsEntrypoints;
    WTF::Vector<SavedRegistersInImage> savedRegisters;
    WTF::Vector<FramelessRangeInImage> framelessRanges;
    uint32_t catchRunway { 0 };
    uint32_t faultWithoutFrame { 0 };
};

struct StructLayoutInImage {
    Vector<uint32_t> offsetsOfFieldsInPayload;
    uint32_t payloadSize { 0 };
};

struct CodeForImage {
    const ImagePointerTable& pointers;
    const UncheckedKeyHashMap<const RTT*, uint32_t>& typeIndices;
    const Vector<StructLayoutInImage>& structLayouts;
    AOT::RelocationRecorder& recorder;
    ImageCodeFlavor flavor;

    bool dependsOnFlavor { false };
    ImageCodeFailure failure { ImageCodeFailure::None };
    const B3::Policy* policy { nullptr };
    B3::CompilationScratch* scratch { nullptr };
    uint32_t pointerSpace { 0 };
    unsigned threadedJumps { 0 };
    unsigned firstStateTests { 0 };
    static constexpr GPRReg passedGlobalGPR = GPRInfo::wasmScratchGPR0;
    std::optional<uint32_t> passedGlobal { };
};

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY)

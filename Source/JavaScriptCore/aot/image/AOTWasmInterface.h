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

namespace Wasm {

class RTT;
struct ModuleInformation;

// Every function of the run time that code in an image calls: the functions of a module, and the stubs beside them.
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

// Why a module of the program has no code in the image. Each has a row in diagnostics.def.
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

// Why the code that the generator made cannot stand in an image. Each is an error of the compiler, never of the program.
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

enum class InterpreterFallbackReason : uint8_t {
    VectorsAreDisabled,
};

static constexpr unsigned numberOfInterpreterFallbackReasons = 1;
static_assert(static_cast<unsigned>(InterpreterFallbackReason::VectorsAreDisabled) + 1 == numberOfInterpreterFallbackReasons);

// A function is compiled for each of the two where its code tells them apart, and once where it does not.
enum class ImageCodeFlavor : uint8_t {
    // Tests the bounds of every access to memory and every reference for null. Runs with any memory, and without the fault handler.
    Checking,
    // Leaves both to the fault handler. Memory 0, if there is one, is a signaling memory.
    Trapping,
};
static constexpr unsigned numberOfImageCodeFlavors = 2;

// The first page of every process that runs an image is unmapped, whatever lowestAccessibleAddress() answers in the process that builds it.
static constexpr ptrdiff_t lowestAccessibleAddressOfEveryTarget = 4 * KB;

// Code that a module has once, beside its functions.
enum class ImageStub : uint32_t {
    // Takes the ExceptionType in argumentGPR1. Jumped to.
    ThrowException,
    // Jumped to from a prologue.
    ThrowStackOverflow,
    // Where the unwinder sends a frame of the image that catches. It goes on at VM::targetMachinePCAfterCatch.
    CatchRunway,
};
static constexpr uint32_t numberOfImageStubs = 3;
static_assert(static_cast<uint32_t>(ImageStub::CatchRunway) + 1 == numberOfImageStubs);

// What the index of a symbol of AOT::SymbolSpace::WebAssembly counts, in the code of a module.
constexpr uint32_t imageSymbolOf(ImageStub stub) { return static_cast<uint32_t>(stub); }
inline uint32_t imageSymbolOf(FunctionCodeIndex function) { return numberOfImageStubs + static_cast<uint32_t>(function.rawIndex()); }

// In the records of the directory an array field is an offset from the start of the directory, and a code offset counts from the start of the image's WebAssembly code.
struct DirectoryInImage {
    uint32_t numberOfModules;
    uint32_t numberOfDeclinedModules;
    // ImageStub::CatchRunway of some module: it serves every frame of the image.
    uint32_t catchRunway;
    uint32_t unused;
};
static_assert(sizeof(DirectoryInImage) == 16);

struct FunctionInImage {
    uint32_t entry;
    uint32_t size;
    // In ModuleInImage::handlers.
    uint32_t firstHandler;
    uint32_t numberOfHandlers;
    uint16_t savedRegisters;
    uint8_t usesVectors;
    uint8_t unused;
};
static_assert(sizeof(FunctionInImage) == 20);

// Wasm::UnlinkedHandlerInfo and the place of its code.
struct HandlerInImage {
    uint32_t type;
    uint32_t start;
    uint32_t end;
    uint32_t tryDepth;
    uint32_t exceptionIndexOrDelegateTarget;
    // Nothing for HandlerType::Delegate.
    uint32_t code;
};
static_assert(sizeof(HandlerInImage) == 24);

struct ModuleInImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize;
    uint32_t numberOfFunctions;
    uint32_t numberOfImportedFunctions;
    // [ImageCodeFlavor]. The same array twice where no function tells the flavors apart.
    std::array<uint32_t, numberOfImageCodeFlavors> functions;
    uint32_t handlers;
    // For an import that is a function of another instance.
    uint32_t importStubs;
    // For an import that is a function of JavaScript.
    uint32_t javaScriptImportStubs;
    uint32_t savedRegisters;
    uint32_t numberOfJSEntrypoints;
    uint32_t jsEntrypoints;
    uint32_t usesVectors;
    uint32_t unused;
};
static_assert(sizeof(ModuleInImage) == 72);

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

// What has an address only in the process that runs the code. JSWebAssemblyInstance::imageCodePointers() is the table, and code reads a slot of it through the instance register.
struct ImagePointerTable {
#define COUNT_OPERATION(operation) + 1
    static constexpr uint32_t numberOfOperations = 0 FOR_EACH_WASM_IMAGE_CODE_OPERATION(COUNT_OPERATION);
#undef COUNT_OPERATION
    static constexpr uint32_t genericJSEntrypoint = numberOfOperations;
    // LLInt::defaultCall(): where a call of a function of JavaScript goes that its CallLinkInfo does not know.
    static constexpr uint32_t defaultJSCall = numberOfOperations + 1;
    // Boxed.
    static constexpr uint32_t wasmToJSCallee = numberOfOperations + 2;
    static constexpr uint32_t restoreFrameCallee = numberOfOperations + 3;
    static constexpr uint32_t restoreFrameReturnAddress = numberOfOperations + 4;
    // The number itself, not a pointer to it.
    static constexpr uint32_t structureIDBase = numberOfOperations + 5;
#if GIGACAGE_ENABLED
    static constexpr uint32_t primitiveGigacageDisableRequest = numberOfOperations + 6;
    static constexpr uint32_t primitiveGigacageBase = numberOfOperations + 7;
    static constexpr uint32_t numberOfSharedPointers = numberOfOperations + 8;
#else
    static constexpr uint32_t numberOfSharedPointers = numberOfOperations + 6;
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

// Code offsets count from the start of code.
struct ModuleForImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize { 0 };
    bool usesVectors { false };
    Vector<uint8_t> code;
    // [ImageCodeFlavor]. The second is empty where no function tells the flavors apart.
    std::array<Vector<FunctionInImage>, numberOfImageCodeFlavors> functions;
    Vector<HandlerInImage> handlers;
    Vector<uint32_t> importStubs;
    Vector<uint32_t> javaScriptImportStubs;
    Vector<JSEntrypointInImage> jsEntrypoints;
    WTF::Vector<SavedRegistersInImage> savedRegisters;
    uint32_t catchRunway { 0 };
};

// A process that runs an image keeps the word behind the header of every cell free of data, and the process that builds the image doesn't.
// An RTT of the building process says where fields lie there: code for an image asks this.
struct StructLayoutInImage {
    Vector<uint32_t> offsetsOfFieldsInPayload;
    uint32_t payloadSize { 0 };
};

// What the generator of a function's code is handed, through CompilationContext, where the code is for an image.
struct CodeForImage {
    const ImagePointerTable& pointers;
    const UncheckedKeyHashMap<const RTT*, uint32_t>& typeIndices;
    const Vector<StructLayoutInImage>& structLayouts;
    AOT::RelocationRecorder& recorder;
    ImageCodeFlavor flavor;

    // What the generator answers.
    bool dependsOnFlavor { false };
    ImageCodeFailure failure { ImageCodeFailure::None };
};

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY)

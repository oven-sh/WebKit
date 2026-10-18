/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(WEBASSEMBLY_OMGJIT)

#include "WasmCallee.h"
#include "WasmFormat.h"
#include <array>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace Wasm {

struct ModuleInformation;

#define FOR_EACH_WASM_IMAGE_CODE_OPERATION(macro) \
    macro(operationThrowExceptionFromOMG) \
    macro(operationGrowMemory) \
    macro(operationWasmMemorySizeInPages) \
    macro(operationWasmMemoryFill) \
    macro(operationWasmMemoryCopy) \
    macro(operationWasmMemoryInit) \
    macro(operationWasmDataDrop) \
    macro(operationMemoryCopy) \
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

struct DirectoryInImage {
    uint32_t numberOfModules;
    uint32_t unused;
};

struct ModuleInImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize;
    uint32_t numberOfFunctions;
    uint32_t numberOfImportedFunctions;
    uint32_t entries;
    uint32_t importStubs;
    uint32_t functionSavedRegisters;
    uint32_t savedRegisters;
    uint32_t isForSignalingMemory;
    uint32_t numberOfJSEntrypoints;
    uint32_t jsEntrypoints;
    uint32_t usesVectors;
};
static_assert(sizeof(ModuleInImage) == 64);

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

struct ModuleForImage {
    std::array<uint8_t, 20> digest;
    uint32_t sourceSize { 0 };
    bool isForSignalingMemory { false };
    bool usesVectors { false };
    Vector<uint8_t> code;
    Vector<uint32_t> entries;
    Vector<uint32_t> importStubs;
    Vector<JSEntrypointInImage> jsEntrypoints;
    Vector<uint16_t> functionSavedRegisters;
    Vector<SavedRegistersInImage> savedRegisters;
};

class CodeInImage final : public ThreadSafeRefCounted<CodeInImage> {
    WTF_MAKE_TZONE_ALLOCATED(CodeInImage);
public:
#define COUNT_OPERATION(operation) + 1
    static constexpr unsigned numberOfOperations = 0 FOR_EACH_WASM_IMAGE_CODE_OPERATION(COUNT_OPERATION);
#undef COUNT_OPERATION
    static constexpr unsigned indexOfThrowOperation = 0;
    static constexpr unsigned indexOfGenericJSEntrypoint = numberOfOperations;
    static constexpr unsigned numberOfSharedPointers = numberOfOperations + 1;

    static std::optional<unsigned> indexOfOperation(void*);
    static unsigned indexOfCallee(size_t functionCodeIndex) { return numberOfSharedPointers + functionCodeIndex; }
    static unsigned indexOfRTT(const ModuleInformation&, TypeSignatureIndex);
    static unsigned indexOfJSEntrypointCallee(const ModuleInformation&, unsigned entry);

#if ENABLE(AOT)
    JS_EXPORT_PRIVATE static std::array<uint8_t, 20> digestOf(std::span<const uint8_t> source);
    JS_EXPORT_PRIVATE static std::optional<ModuleForImage> compile(VM&, std::span<const uint8_t> source, String& declineReason);
    JS_EXPORT_PRIVATE static std::pair<Vector<uint8_t>, Vector<uint8_t>> layOut(Vector<ModuleForImage>&&);
#endif
    static constexpr uint16_t functionUsesVectors = 1 << 15;
    static bool isInProcess();
    static bool contains(const void* pc) { return std::bit_cast<uintptr_t>(pc) - std::bit_cast<uintptr_t>(s_start) < s_size; }
    static void setRange(const uint8_t* start, size_t size)
    {
        s_start = start;
        s_size = size;
    }
    static const uint16_t* functionSavedRegistersIfFound(const ModuleInformation&);
    static RefPtr<CodeInImage> tryCreate(const ModuleInformation&, const IPIntCallees&, MemoryMode);
    bool isSafeToRun(MemoryMode mode) const { return !m_isForSignalingMemory || mode == MemoryMode::Signaling; }
    ~CodeInImage();

    void* const* pointers() const { return m_pointers.span().data(); }
    CodePtr<WasmEntryPtrTag> entrypoint(size_t functionCodeIndex) const { return m_callees[functionCodeIndex]->entrypoint(); }
    bool holds(const Callee* callee, size_t functionCodeIndex) const { return m_callees[functionCodeIndex].ptr() == callee; }
    CodePtr<WasmEntryPtrTag> importStub(size_t index) const { return m_importStubs[index]; }
    CodePtr<JSEntryPtrTag> jsEntrypoint(const RTT&) const;

private:
    CodeInImage(const ModuleInformation&, const IPIntCallees&, const ModuleInImage&, const uint8_t* directory, const uint8_t* code);

    FixedVector<void*> m_pointers;
    FixedVector<Ref<OMGCallee>> m_callees;
    FixedVector<CodePtr<WasmEntryPtrTag>> m_importStubs;
    Vector<std::pair<const RTT*, Ref<JSToWasmICCallee>>> m_jsEntrypoints;
    bool m_isForSignalingMemory { false };
    static const uint8_t* s_start;
    static size_t s_size;
};

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

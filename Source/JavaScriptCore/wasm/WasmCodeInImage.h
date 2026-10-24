/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(WEBASSEMBLY_OMGJIT)

#include "AOTWasmInterface.h"
#include "WasmCallee.h"
#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace JSC { namespace Wasm {

class CodeInImage final : public ThreadSafeRefCounted<CodeInImage> {
    WTF_MAKE_TZONE_ALLOCATED(CodeInImage);
public:
    JS_EXPORT_PRIVATE static std::array<uint8_t, 20> digestOf(std::span<const uint8_t> source);
    static RegisterAtOffsetList savedRegistersFrom(const SavedRegistersInImage&);

    JS_EXPORT_PRIVATE static void setImage(const uint8_t* directory, std::span<const uint8_t> code);
    JS_EXPORT_PRIVATE static bool isInProcess();
    static bool needsSourceDigests();
    static bool contains(const void* pc) { return std::bit_cast<uintptr_t>(pc) - std::bit_cast<uintptr_t>(s_code.data()) < s_code.size(); }

    static const ModuleInImage* find(const ModuleInformation&);
    static bool isModuleOfProgram(const ModuleInformation&);
    static std::span<const FunctionInImage> functionsOf(const ModuleInImage&);
    static std::optional<InterpreterFallbackReason> fallbackReason(const ModuleInformation&, MemoryMode);
    static RefPtr<CodeInImage> tryCreate(const ModuleInformation&, const IPIntCallees&, MemoryMode, CodePtr<WasmEntryPtrTag> interpreterEntrypoint);

    static void didFallBackToInterpreter(InterpreterFallbackReason);
    JS_EXPORT_PRIVATE static unsigned interpreterFallbackCount(InterpreterFallbackReason);

    bool isSafeToRun(MemoryMode mode) const { return !m_isForSignalingMemory || mode == MemoryMode::Signaling; }
    ~CodeInImage();

    void* const* pointers() const { return m_pointers.span().data(); }
    CodePtr<WasmEntryPtrTag> entrypoint(size_t functionCodeIndex) const { return m_callees[functionCodeIndex] ? m_callees[functionCodeIndex]->entrypoint() : nullptr; }
    bool holds(const Callee* callee, size_t functionCodeIndex) const { return m_callees[functionCodeIndex].get() == callee; }
    CodePtr<WasmEntryPtrTag> importStub(size_t index) const { return m_importStubs[index]; }
    CodePtr<JSEntryPtrTag> jsEntrypoint(const RTT&) const;

private:
    CodeInImage(const ModuleInformation&, const IPIntCallees&, const ModuleInImage&, CodePtr<WasmEntryPtrTag> interpreterEntrypoint);

    FixedVector<void*> m_pointers;
    FixedVector<RefPtr<OMGCallee>> m_callees;
    FixedVector<CodePtr<WasmEntryPtrTag>> m_importStubs;
    Vector<std::pair<const RTT*, Ref<JSToWasmICCallee>>> m_jsEntrypoints;
    bool m_isForSignalingMemory { false };

    static const uint8_t* s_directory;
    static std::span<const uint8_t> s_code;
    static std::array<std::atomic<unsigned>, numberOfInterpreterFallbackReasons> s_interpreterFallbackCounts;
};

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

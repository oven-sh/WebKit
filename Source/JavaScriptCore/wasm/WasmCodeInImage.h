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

    // A process may hold several images: each entry point has its own. They stay until the process ends.
    JS_EXPORT_PRIVATE static void addImage(std::span<const uint8_t> directory, std::span<const uint8_t> code);
    JS_EXPORT_PRIVATE static bool isInProcess();
    static bool needsSourceDigests();
    // Safe to call from a signal handler.
    static bool contains(const void* pc);

    // Where the unwinder sends a frame whose handler lies at this place in an image. Nothing for a place outside every image.
    static CodePtr<ExceptionHandlerPtrTag> catchRunwayFor(const void* pc);

    // Stores the digest of the bytes, so that the parser does not hash them again.
    static const ModuleInImage* findBeforeParsing(std::span<const uint8_t> source, ModuleInformation&);
    static const ModuleInImage* find(const ModuleInformation&);
    static bool isModuleOfProgram(const ModuleInformation&);
    static std::span<const FunctionInImage> functionsOf(const ModuleInImage&, ImageCodeFlavor);
    JS_EXPORT_PRIVATE static ImageCodeFlavor flavorFor(const ModuleInformation&, MemoryMode);
    static RefPtr<CodeInImage> tryCreate(const ModuleInformation&, const IPIntCallees&, MemoryMode);

    bool serves(const ModuleInformation& info, MemoryMode mode) const { return m_servesEveryFlavor || m_flavor == flavorFor(info, mode); }
    ~CodeInImage();

    void* const* pointers() const { return m_pointers.span().data(); }
    CodePtr<WasmEntryPtrTag> entrypoint(size_t functionCodeIndex) const { return m_callees[functionCodeIndex]->entrypoint(); }
    bool holds(const Callee* callee, size_t functionCodeIndex) const { return m_callees[functionCodeIndex].ptr() == callee; }
    CodePtr<WasmEntryPtrTag> importStub(size_t index) const { return m_importStubs[index]; }
    CodePtr<WasmEntryPtrTag> javaScriptImportStub(size_t index) const { return m_javaScriptImportStubs[index]; }
    CodePtr<JSEntryPtrTag> jsEntrypoint(const RTT&) const;

private:
    CodeInImage(const ModuleInformation&, const IPIntCallees&, const ModuleInImage&, ImageCodeFlavor);

    FixedVector<void*> m_pointers;
    // The code never leaves for another tier, which is all that the class of the callees says.
    Vector<Ref<OMGCallee>> m_callees;
    FixedVector<CodePtr<WasmEntryPtrTag>> m_importStubs;
    FixedVector<CodePtr<WasmEntryPtrTag>> m_javaScriptImportStubs;
    Vector<std::pair<const RTT*, Ref<JSToWasmICCallee>>> m_jsEntrypoints;
    ImageCodeFlavor m_flavor;
    bool m_servesEveryFlavor;

    struct PublishedImage;
    static const PublishedImage& imageHolding(const ModuleInImage&);
    static const ModuleInImage* findRecord(const ModuleInformation&);

    static std::atomic<const PublishedImage*> s_lastPublished;
};

} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

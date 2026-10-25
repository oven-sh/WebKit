/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeTemplates.h"
#include "AOTCompiledCode.h"
#include "AOTDiagnostics.h"
#include "AOTImageBuilder.h"
#include "AOTRelocation.h"
#include "AOTWasmInterface.h"
#include <expected>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

class VM;

namespace AOT {

class BuildContext;

namespace Typed {
class Program;
}

// The modules that the program imports, and the ones whose bytes stand in a string constant as base64 text, alone or behind the head of a data: URL.
JS_EXPORT_PRIVATE WebAssemblyImageInputs compileWebAssemblyModules(VM&, const Typed::Program&, const CodeTemplates&, BuildContext&, const BitVector* stringsInImage = nullptr);
// False where compileWebAssemblyModules() finds no module, whichever strings the image holds.
JS_EXPORT_PRIVATE bool mayHaveWebAssemblyModules(const Typed::Program&);

#if ENABLE(WEBASSEMBLY)
JS_EXPORT_PRIVATE UserDiagnosticKind diagnosticKindOf(Wasm::ImageDeclineReason);
#endif

#if ENABLE(WEBASSEMBLY_OMGJIT)

struct WasmFunctionCode {
    bool hasCode() const { return declineReason == Wasm::ImageDeclineReason::None; }

    void decline(Wasm::ImageDeclineReason reason)
    {
        bool vectors = usesVectors;
        *this = { };
        usesVectors = vectors;
        declineReason = reason;
    }

    RelocatableCode code;
    Vector<Wasm::ImageCallTarget> callTargets;
    Vector<ConstantData> constants;
    Vector<JumpTable> jumpTables;
    Wasm::SavedRegistersInImage savedRegisters { 0, 0, 0 };
    uint16_t savedRegistersIndex { 0 };
    bool usesVectors { false };
    Wasm::ImageDeclineReason declineReason { Wasm::ImageDeclineReason::None };
};

struct WasmJSEntrypointCode {
    uint32_t type;
    Vector<uint8_t> bytes;
};

struct WasmModuleCode {
    std::array<uint8_t, 20> digest { };
    uint32_t sourceSize { 0 };
    bool isForSignalingMemory { false };
    Vector<uint8_t> throwStub;
    Vector<Vector<uint8_t>> importStubs;
    Vector<WasmJSEntrypointCode> jsEntrypoints;
    Vector<WasmFunctionCode> functions;
    WTF::Vector<Wasm::SavedRegistersInImage> savedRegisters;
};

using WasmModuleOrDecline = std::expected<Wasm::ModuleForImage, Wasm::ImageDeclineReason>;

JS_EXPORT_PRIVATE WasmModuleOrDecline compileWasmModule(VM&, std::span<const uint8_t> source, const CodeTemplates&, BuildContext&);
JS_EXPORT_PRIVATE WasmModuleOrDecline linkWasmModule(WasmModuleCode&&, const CodeTemplates&, BuildContext&);

class WasmSectionBuilder {
    WTF_MAKE_NONCOPYABLE(WasmSectionBuilder);
public:
    // AddressFar is page relative on ARM64, and a module is linked as if it were loaded at offset 0.
    static constexpr size_t codeAlignment = 4 * KB;

    WasmSectionBuilder() = default;

    [[nodiscard]] JS_EXPORT_PRIVATE bool append(Wasm::ModuleForImage&&);
    JS_EXPORT_PRIVATE void appendDeclined(const Wasm::DeclinedModuleInImage&);
    JS_EXPORT_PRIVATE WebAssemblyImageInputs finish();

private:
    struct PlacedModule {
        Wasm::ModuleForImage module;
        uint32_t base;
    };

    Vector<PlacedModule> m_modules;
    Vector<Wasm::DeclinedModuleInImage> m_declinedModules;
    uint64_t m_codeSize { 0 };
    uint64_t m_directorySize { sizeof(Wasm::DirectoryInImage) };
};

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

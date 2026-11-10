/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeTemplates.h"
#include "AOTDiagnostics.h"
#include "AOTImageBuilder.h"
#include "AOTRelocation.h"
#include "AOTWasmInterface.h"
#include <array>
#include <expected>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class VM;

namespace AOT {

namespace Typed {
class Program;
}

JS_EXPORT_PRIVATE bool mayHaveWebAssemblyModules(const Typed::Program&);

#if ENABLE(WEBASSEMBLY)
JS_EXPORT_PRIVATE UserDiagnosticKind diagnosticKindOf(Wasm::ImageDeclineReason);
#endif

#if ENABLE(WEBASSEMBLY_OMGJIT)

struct WasmFunctionCode {
    RelocatableCode code;
    Vector<Wasm::HandlerInImage> handlers;
    BitVector directCallees;
    Wasm::SavedRegistersInImage savedRegisters { 0, 0, 0 };
    uint16_t savedRegistersIndex { 0 };
    bool usesVectors { false };
    bool dependsOnFlavor { false };
    bool isOptimized { false };
    unsigned threadedJumps { 0 };
    unsigned firstStateTests { 0 };
};

struct WasmJSEntrypointCode {
    uint32_t type;
    Vector<uint8_t> bytes;
};

struct WasmModuleCode {
    std::array<uint8_t, 20> digest { };
    uint32_t sourceSize { 0 };
    std::array<RelocatableCode, Wasm::numberOfImageStubs> stubs;
    Vector<Vector<uint8_t>> importStubs;
    Vector<Vector<uint8_t>> javaScriptImportStubs;
    Vector<WasmJSEntrypointCode> jsEntrypoints;
    Vector<WasmFunctionCode> functions;
    Vector<WasmFunctionCode> functionsThatCheck;
    Vector<Wasm::SavedRegistersInImage> savedRegisters;
};

using WasmModuleOrDecline = std::expected<Wasm::ModuleForImage, Wasm::ImageDeclineReason>;

JS_EXPORT_PRIVATE WasmModuleOrDecline compileWasmModule(VM&, std::span<const uint8_t> source, const CodeTemplates&, BuildContext&, SourceLocation = noSourceLocation);
JS_EXPORT_PRIVATE WasmModuleOrDecline linkWasmModule(WasmModuleCode&&, const CodeTemplates&, BuildContext&);

class WasmSectionBuilder {
    WTF_MAKE_NONCOPYABLE(WasmSectionBuilder);
public:
    static constexpr size_t codeAlignment = 4 * KB;

    WasmSectionBuilder() = default;

    [[nodiscard]] JS_EXPORT_PRIVATE bool append(Wasm::ModuleForImage&&);
    JS_EXPORT_PRIVATE void appendDeclined(const Wasm::DeclinedModuleInImage&);
    JS_EXPORT_PRIVATE WebAssemblyImageInputs finish(BuildContext&);

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

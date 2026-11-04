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

// The modules of a program: those that it imports, and those whose bytes stand in a string constant as base64 text, alone or behind the head of a data: URL.
// False where compileWebAssemblyModules() of AOTImageBuilder.h finds no module, whichever strings the image holds.
JS_EXPORT_PRIVATE bool mayHaveWebAssemblyModules(const Typed::Program&);

#if ENABLE(WEBASSEMBLY)
JS_EXPORT_PRIVATE UserDiagnosticKind diagnosticKindOf(Wasm::ImageDeclineReason);
#endif

#if ENABLE(WEBASSEMBLY_OMGJIT)

// The code of a function for one flavor, by WebAssembly's baseline compiler: MacroAssembler alone, no B3 and no Air.
// A relocation names a symbol of SymbolSpace::WebAssembly (Wasm::imageSymbolOf()).
struct WasmFunctionCode {
    RelocatableCode code;
    // HandlerInImage::code counts from the start of the function.
    Vector<Wasm::HandlerInImage> handlers;
    // By FunctionCodeIndex: the functions of the module that a call or a tail call in the code names.
    BitVector directCallees;
    Wasm::SavedRegistersInImage savedRegisters { 0, 0, 0 };
    uint16_t savedRegistersIndex { 0 };
    bool usesVectors { false };
    // The generator asked for the flavor, so it may answer with other code for the other one.
    bool dependsOnFlavor { false };
};

struct WasmJSEntrypointCode {
    uint32_t type;
    Vector<uint8_t> bytes;
};

struct WasmModuleCode {
    std::array<uint8_t, 20> digest { };
    uint32_t sourceSize { 0 };
    // [Wasm::ImageStub]
    std::array<RelocatableCode, Wasm::numberOfImageStubs> stubs;
    Vector<Vector<uint8_t>> importStubs;
    Vector<Vector<uint8_t>> javaScriptImportStubs;
    Vector<WasmJSEntrypointCode> jsEntrypoints;
    // Every function, for ImageCodeFlavor::Trapping.
    Vector<WasmFunctionCode> functions;
    // By FunctionCodeIndex, for ImageCodeFlavor::Checking: code only where WasmFunctionCode::dependsOnFlavor of the function above is set.
    Vector<WasmFunctionCode> functionsThatCheck;
    Vector<Wasm::SavedRegistersInImage> savedRegisters;
};

// ImageDeclineReason::None: the module has no function of its own, so there is nothing to compile and nothing to say.
using WasmModuleOrDecline = std::expected<Wasm::ModuleForImage, Wasm::ImageDeclineReason>;

// `location` is where a diagnostic about a single function points.
JS_EXPORT_PRIVATE WasmModuleOrDecline compileWasmModule(VM&, std::span<const uint8_t> source, const CodeTemplates&, BuildContext&, SourceLocation = noSourceLocation);
JS_EXPORT_PRIVATE WasmModuleOrDecline linkWasmModule(WasmModuleCode&&, const CodeTemplates&, BuildContext&);

class WasmSectionBuilder {
    WTF_MAKE_NONCOPYABLE(WasmSectionBuilder);
public:
    // AddressFar is page relative on ARM64, and a module is linked as if it were loaded at offset 0.
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

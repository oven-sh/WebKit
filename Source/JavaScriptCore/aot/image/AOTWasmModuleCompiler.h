/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)

#include "AOTCodeTemplates.h"
#include "AOTVector.h"
#include "AOTWasmInterface.h"
#include "AOTWasmSection.h"
#include "AssemblerPolicy.h"
#include "MemoryMode.h"
#include <array>
#include <expected>
#include <memory>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class VM;

namespace B3 {
class CompilationScratch;
}

namespace Wasm {
class CalleeGroup;
class Module;
}

namespace AOT {

class BuildContext;

class WasmModuleCompiler {
    WTF_MAKE_TZONE_ALLOCATED(WasmModuleCompiler);
    WTF_MAKE_NONCOPYABLE(WasmModuleCompiler);
public:
    JS_EXPORT_PRIVATE static std::expected<std::unique_ptr<WasmModuleCompiler>, Wasm::ImageDeclineReason> create(VM&, std::span<const uint8_t> source);
    JS_EXPORT_PRIVATE ~WasmModuleCompiler();

    JS_EXPORT_PRIVATE std::expected<Wasm::ModuleForImage, Wasm::ImageDeclineReason> compile(const CodeTemplates&, BuildContext&);

private:
    WasmModuleCompiler(Ref<Wasm::Module>&&, Ref<Wasm::CalleeGroup>&&, MemoryMode, std::span<const uint8_t> source);

    AssemblerPolicy assemblerPolicyFor(Wasm::FunctionCodeIndex, const BuildContext&) const;
    void compileFunction(Wasm::FunctionCodeIndex, B3::CompilationScratch&, BuildContext&, WasmFunctionCode&) const;
    void internSavedRegisters(WasmModuleCode&) const;
    std::expected<void, LinkFailure> generateStubs(WasmModuleCode&, BuildContext&) const;

    Ref<Wasm::Module> m_module;
    Ref<Wasm::CalleeGroup> m_group;
    MemoryMode m_mode;
    std::array<uint8_t, 20> m_digest;
    uint32_t m_sourceSize;
    Vector<uint32_t> m_jsEntrypointTypes;
    Wasm::ImagePointerTable m_pointers;
    UncheckedKeyHashMap<const Wasm::RTT*, uint32_t> m_typeIndices;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(WEBASSEMBLY_OMGJIT)

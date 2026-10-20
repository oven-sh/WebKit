/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWasmModules.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTIRProgram.h"
#include "AOTWasmModuleCompiler.h"
#include "AOTWasmSection.h"
#include "WasmCodeInImage.h"
#include <algorithm>
#include <array>
#include <expected>

namespace JSC { namespace AOT {

static void reportDecline(BuildContext& context, Typed::ModuleID module, UserDiagnosticKind kind, LimitID limit, uint32_t count)
{
    SourceLocation location { module.index(), 0 };
    if (limit != LimitID::None)
        context.gaveUp(limit, Subject { .location = location });
    context.diagnostics().add(UserDiagnostic { .kind = kind, .limit = limit, .count = count, .location = location });
}

#if ENABLE(WEBASSEMBLY)

UserDiagnosticKind diagnosticKindOf(Wasm::ImageDeclineReason reason)
{
    switch (reason) {
    case Wasm::ImageDeclineReason::None:
        return UserDiagnosticKind::Unknown;
    case Wasm::ImageDeclineReason::UnsupportedCPU:
        return UserDiagnosticKind::WasmUnsupportedCPU;
    case Wasm::ImageDeclineReason::InvalidModule:
        return UserDiagnosticKind::WasmInvalidModule;
    case Wasm::ImageDeclineReason::NoFunctions:
        return UserDiagnosticKind::WasmNoFunctions;
    case Wasm::ImageDeclineReason::HasGCTypes:
        return UserDiagnosticKind::WasmHasGCTypes;
    case Wasm::ImageDeclineReason::CodeTooLarge:
        return UserDiagnosticKind::WasmCodeTooLarge;
    case Wasm::ImageDeclineReason::UsesExceptions:
        return UserDiagnosticKind::WasmUsesExceptions;
    case Wasm::ImageDeclineReason::UsesTailCalls:
        return UserDiagnosticKind::WasmUsesTailCalls;
    case Wasm::ImageDeclineReason::TestsReferenceTypes:
        return UserDiagnosticKind::WasmTestsReferenceTypes;
    case Wasm::ImageDeclineReason::CallsUnlistedOperation:
        return UserDiagnosticKind::WasmCallsUnlistedOperation;
    case Wasm::ImageDeclineReason::NamesTypeOfOtherModule:
        return UserDiagnosticKind::WasmNamesTypeOfOtherModule;
    case Wasm::ImageDeclineReason::SavesVectorRegister:
        return UserDiagnosticKind::WasmSavesVectorRegister;
    case Wasm::ImageDeclineReason::SavedRegisterOrder:
        return UserDiagnosticKind::WasmSavedRegisterOrder;
    case Wasm::ImageDeclineReason::ProcessAddressInCode:
        return UserDiagnosticKind::WasmProcessAddressInCode;
    case Wasm::ImageDeclineReason::CodeGeneratorFailed:
        return UserDiagnosticKind::WasmCodeGeneratorFailed;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#endif // ENABLE(WEBASSEMBLY)

#if ENABLE(WEBASSEMBLY_OMGJIT)

using CompiledModule = std::expected<Wasm::ModuleForImage, Wasm::ImageDeclineReason>;
using DeclineCount = std::pair<Wasm::ImageDeclineReason, uint32_t>;

struct KnownModule {
    std::array<uint8_t, 20> digest;
    size_t size;
    Vector<DeclineCount, 2> declines;
};

static CompiledModule compileModule(VM& vm, std::span<const uint8_t> bytes, const CodeTemplates& templates, BuildContext& context)
{
    auto compiler = WasmModuleCompiler::create(vm, bytes);
    if (!compiler)
        return makeUnexpected(compiler.error());
    return (*compiler)->compile(templates, context);
}

static Vector<DeclineCount, 2> collectDeclines(const CompiledModule& module)
{
    if (!module)
        return { { module.error(), 1 } };
    Vector<DeclineCount, 2> declines;
    for (auto [function, reason] : module->interpretedFunctions) {
        auto decline = std::ranges::find(declines, reason, &DeclineCount::first);
        if (decline != declines.end())
            ++decline->second;
        else
            declines.append({ reason, 1 });
    }
    std::ranges::sort(declines, { }, &DeclineCount::first);
    return declines;
}

WebAssemblyImageInputs compileWebAssemblyModules(VM& vm, const Typed::Program& program, const CodeTemplates& templates, BuildContext& context)
{
    WasmSectionBuilder builder;
    Vector<KnownModule> knownModules;
    for (auto& entry : program.webAssemblyModules) {
        auto digest = Wasm::CodeInImage::digestOf(entry.bytes);
        size_t index = knownModules.findIf([&](auto& known) {
            return known.digest == digest && known.size == entry.bytes.size();
        });
        if (index == notFound) {
            index = knownModules.size();
            auto module = compileModule(vm, entry.bytes, templates, context);
            Vector<DeclineCount, 2> declines = collectDeclines(module);
            bool isInImage = module && builder.append(WTF::move(*module));
            if (module && !isInImage)
                declines = { { Wasm::ImageDeclineReason::CodeTooLarge, 1 } };
            if (!isInImage)
                builder.appendDeclined({ digest, static_cast<uint32_t>(entry.bytes.size()) });
            knownModules.append(KnownModule { digest, entry.bytes.size(), WTF::move(declines) });
        }
        for (auto [reason, count] : knownModules[index].declines) {
            auto limit = reason == Wasm::ImageDeclineReason::CodeTooLarge ? LimitID::WasmImageCodeSize : LimitID::None;
            reportDecline(context, entry.module, diagnosticKindOf(reason), limit, count);
        }
    }
    return builder.finish();
}

#else

WebAssemblyImageInputs compileWebAssemblyModules(VM&, const Typed::Program& program, const CodeTemplates&, BuildContext& context)
{
    for (auto& entry : program.webAssemblyModules)
        reportDecline(context, entry.module, UserDiagnosticKind::WasmUnsupportedCPU, LimitID::None, 1);
    return { };
}

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

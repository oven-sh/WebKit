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
#include <optional>
#include <utility>
#include <wtf/text/Base64.h>

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

namespace {

struct KnownModule {
    std::array<uint8_t, 20> digest;
    size_t size;
    bool isInImage;
    bool isInText { false };
    Vector<DeclineCount, 2> declines;
};

class ModuleSet {
public:
    ModuleSet(VM& vm, const CodeTemplates& templates, BuildContext& context)
        : m_vm(vm)
        , m_templates(templates)
        , m_context(context)
    {
    }

    KnownModule& add(std::span<const uint8_t> bytes);
    WebAssemblyImageInputs finish() { return m_builder.finish(); }

private:
    VM& m_vm;
    const CodeTemplates& m_templates;
    BuildContext& m_context;
    WasmSectionBuilder m_builder;
    Vector<KnownModule> m_modules;
};

} // anonymous namespace

static LimitID limitOf(Wasm::ImageDeclineReason reason)
{
    return reason == Wasm::ImageDeclineReason::CodeTooLarge ? LimitID::WasmImageCodeSize : LimitID::None;
}

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

KnownModule& ModuleSet::add(std::span<const uint8_t> bytes)
{
    auto digest = Wasm::CodeInImage::digestOf(bytes);
    size_t index = m_modules.findIf([&](auto& known) {
        return known.digest == digest && known.size == bytes.size();
    });
    if (index != notFound)
        return m_modules[index];

    auto module = compileModule(m_vm, bytes, m_templates, m_context);
    Vector<DeclineCount, 2> declines = collectDeclines(module);
    bool isInImage = module && m_builder.append(WTF::move(*module));
    if (module && !isInImage)
        declines = { { Wasm::ImageDeclineReason::CodeTooLarge, 1 } };
    if (!isInImage)
        m_builder.appendDeclined({ digest, static_cast<uint32_t>(bytes.size()) });
    m_modules.append(KnownModule { .digest = digest, .size = bytes.size(), .isInImage = isInImage, .declines = WTF::move(declines) });
    return m_modules.last();
}

// The run time finds code by the digest of the bytes a module is made from, so text that only looks like a module costs bytes and never an answer.
static std::span<const uint8_t> base64OfEmbeddedModule(std::span<const uint8_t> text)
{
    auto modulePrefix = "AGFzbQEAAA"_span;
    auto base64Marker = "base64,"_span;
    constexpr size_t minLength = 64;
    constexpr size_t maxPrefixLength = 64;
    static_assert(maxPrefixLength <= minLength);
    if (text.size() < minLength)
        return { };
    if (spanHasPrefix(text, modulePrefix))
        return text;
    if (text[0] != 'd')
        return { };
    size_t at = WTF::find(text.first(maxPrefixLength), base64Marker);
    if (at == notFound)
        return { };
    text = text.subspan(at + base64Marker.size());
    if (!spanHasPrefix(text, modulePrefix))
        return { };
    return text;
}

static std::optional<Vector<uint8_t>> decodeEmbeddedModule(std::span<const uint8_t> text)
{
    std::span<const uint8_t> base64 = base64OfEmbeddedModule(text);
    if (base64.empty())
        return std::nullopt;
    return base64Decode(base64);
}

static void addModulesInText(const Typed::Program& program, const BitVector* stringsInImage, ModuleSet& modules, BuildContext& context)
{
    uint64_t numberFound = 0;
    uint64_t numberDeclined = 0;
    for (size_t index = 0; index < program.strings.size(); ++index) {
        if (stringsInImage && !stringsInImage->get(index))
            continue;
        std::optional<Vector<uint8_t>> bytes = decodeEmbeddedModule(program.strings[Typed::StringID(index)]);
        if (!bytes)
            continue;
        KnownModule& module = modules.add(bytes->span());
        if (std::exchange(module.isInText, true))
            continue;
        ++numberFound;
        if (!module.isInImage)
            ++numberDeclined;
        for (auto [reason, count] : module.declines) {
            if (LimitID limit = limitOf(reason); limit != LimitID::None)
                context.gaveUp(limit);
        }
    }
    context.statistics().count(StatisticID::WebAssemblyModulesFoundInText, numberFound);
    context.statistics().count(StatisticID::WebAssemblyModulesInTextDeclined, numberDeclined);
}

WebAssemblyImageInputs compileWebAssemblyModules(VM& vm, const Typed::Program& program, const CodeTemplates& templates, BuildContext& context, const BitVector* stringsInImage)
{
    ModuleSet modules(vm, templates, context);
    for (auto& entry : program.webAssemblyModules) {
        for (auto [reason, count] : modules.add(entry.bytes).declines)
            reportDecline(context, entry.module, diagnosticKindOf(reason), limitOf(reason), count);
    }
    addModulesInText(program, stringsInImage, modules, context);
    return modules.finish();
}

bool mayHaveWebAssemblyModules(const Typed::Program& program)
{
    if (!program.webAssemblyModules.isEmpty())
        return true;
    for (size_t index = 0; index < program.strings.size(); ++index) {
        if (!base64OfEmbeddedModule(program.strings[Typed::StringID(index)]).empty())
            return true;
    }
    return false;
}

#else

WebAssemblyImageInputs compileWebAssemblyModules(VM&, const Typed::Program& program, const CodeTemplates&, BuildContext& context, const BitVector*)
{
    for (auto& entry : program.webAssemblyModules)
        reportDecline(context, entry.module, UserDiagnosticKind::WasmUnsupportedCPU, LimitID::None, 1);
    return { };
}

bool mayHaveWebAssemblyModules(const Typed::Program& program)
{
    return !program.webAssemblyModules.isEmpty();
}

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

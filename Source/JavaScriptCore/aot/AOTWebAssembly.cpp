/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWebAssembly.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTTypedIR.h"
#include "Options.h"
#include "WasmCodeInImage.h"
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/text/Base64.h>

namespace JSC { namespace AOT {

#if ENABLE(WEBASSEMBLY_OMGJIT)

static std::optional<Vector<uint8_t>> decodeEmbeddedModule(std::span<const uint8_t> text)
{
    auto modulePrefix = "AGFzbQEAAA"_span;
    auto base64Marker = "base64,"_span;
    constexpr size_t maxPrefixLength = 64;
    if (text.size() < modulePrefix.size())
        return std::nullopt;
    if (!spanHasPrefix(text, modulePrefix)) {
        if (text[0] != 'd')
            return std::nullopt;
        size_t at = WTF::find(text.first(std::min(text.size(), maxPrefixLength)), base64Marker);
        if (at == notFound)
            return std::nullopt;
        text = text.subspan(at + base64Marker.size());
        if (!spanHasPrefix(text, modulePrefix))
            return std::nullopt;
    }
    return base64Decode(text);
}

void addWebAssemblyModules(VM& vm, const Typed::Program& program, ImageBuilder& builder)
{
    Vector<Wasm::ModuleForImage> modules;
    UncheckedKeyHashSet<String> seen;
    auto add = [&](std::span<const uint8_t> source) {
        auto digest = Wasm::CodeInImage::digestOf(source);
        if (!seen.add(String(byteCast<Latin1Character>(std::span<const uint8_t> { digest }))).isNewEntry)
            return;
        String whyDeclined;
        auto module = Wasm::CodeInImage::compile(vm, source, whyDeclined);
        if (Options::verboseAOTCompilation()) [[unlikely]] {
            if (module)
                dataLogLn("AOT: a WebAssembly module of ", source.size(), " bytes is compiled: ", module->entries.size() - 1, " functions, ", module->code.size(), " bytes of code");
            else
                dataLogLn("AOT: a WebAssembly module of ", source.size(), " bytes is left to the interpreter: ", whyDeclined);
        }
        if (module)
            modules.append(WTF::move(*module));
    };
    for (auto text : program.strings) {
        if (auto source = decodeEmbeddedModule(text))
            add(source->span());
    }
    for (auto source : program.webAssemblyModules)
        add(source);
    if (modules.isEmpty())
        return;
    auto [directory, code] = Wasm::CodeInImage::layOut(WTF::move(modules));
    builder.setWebAssembly(WTF::move(directory), WTF::move(code));
}

#else

void addWebAssemblyModules(VM&, const Typed::Program&, ImageBuilder&) { }

#endif // ENABLE(WEBASSEMBLY_OMGJIT)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

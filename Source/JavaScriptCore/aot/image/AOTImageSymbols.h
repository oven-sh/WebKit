/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

class RegExpRequests;
struct BuiltImage;

struct ImageSymbol {
    enum class Kind : uint8_t {
        Function,
        FoldedFunction,
        SharedCodeCopy,
        SharedCode,
        OwnOutlinedBody,
        RegExpBody,
        RegExpRoutine,
        RegExpSection,
        WebAssemblyCode,
        Constants,
        FFICode,
    };

    Kind kind;
    uint32_t offset;
    uint32_t size;
    uint32_t index { 0 };
    FunctionID function { };
    String name { };
    String description { };
};

struct ImageSymbolInputs {
    const BuiltImage& built;
    const Typed::Program& program;
    const Typed::Heat& heat;
    const RegExpRequests& regExpRequests;
};

Vector<ImageSymbol> collectSymbols(const ImageSymbolInputs&);
Vector<uint8_t> encodeSymbols(std::span<const ImageSymbol>, std::span<const uint8_t> image);
void writeMapFile(const UTF8CString& path, std::span<const ImageSymbol>, const ImageSymbolInputs&, std::span<const uint8_t> image, const BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

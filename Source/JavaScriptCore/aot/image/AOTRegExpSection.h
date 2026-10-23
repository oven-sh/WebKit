/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTCodeTemplates.h"
#include "AOTRegExpCompiler.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <span>

namespace JSC::AOT {

struct RegExpSectionItem {
    enum class Kind : uint8_t {
        Body,
        DuplicateBody,
        Routine,
        Table,
    };

    Kind kind;
    uint32_t offset;
    uint32_t size;
    uint32_t regExp;
};

struct RegExpSection {
    static constexpr size_t alignment = 4 * KB;

    Vector<uint8_t> bytes;
    Vector<std::array<uint32_t, 2>> entries;
    Vector<RegExpSectionItem> items;
};

JS_EXPORT_PRIVATE std::optional<RegExpSection> layOutRegExpSection(std::span<const CompiledRegExp>, const AssemblerPolicy&, const CodeTemplates&, BuildContext*);

} // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)

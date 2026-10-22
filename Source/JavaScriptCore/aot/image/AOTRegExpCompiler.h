/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTRegExpCode.h"
#include "AOTRegExpRecord.h"
#include "AOTRegExpRequests.h"
#include "AOTRegExpSummary.h"
#include <array>
#include <memory>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

namespace JSC {

namespace AOT {

struct CompiledRegExp {
    size_t codeSize() const;

    RegExpRecordForm form { RegExpRecordForm::NoMetadata };
    std::optional<RegExpDeclineReason> reason;
    // For 8-bit and for 16-bit subjects. Without the second, a 16-bit subject enters the failure routine, and the pattern is compiled at run time.
    std::array<std::unique_ptr<RegExpCode>, 2> code;
};

enum class RegExpCodeFor16BitSubjects : uint8_t {
    Always,
    WhereWideCharactersCanBeInMatchedSubject,
};

struct RegExpCompilerInputs {
    StringView pattern;
    OptionSet<Yarr::Flags> flags;
    const RegExpSummary& summary;
    const AssemblerPolicy& assemblerPolicy;
    BuildContext* context;
    RegExpCodeFor16BitSubjects codeFor16BitSubjects { RegExpCodeFor16BitSubjects::Always };
};

JS_EXPORT_PRIVATE CompiledRegExp compileRegExp(const RegExpCompilerInputs&);

struct CompiledRegExps {
    uint32_t numberWithForm(RegExpRecordForm) const;

    Vector<CompiledRegExp> results;
    uint32_t numberWithUnicodePropertyLookup { 0 };
};

JS_EXPORT_PRIVATE CompiledRegExps compileRegExps(const Typed::Program&, const RegExpRequests&, BuildContext&);

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)

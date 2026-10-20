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
    std::array<std::unique_ptr<RegExpCode>, 2> code;
};

struct RegExpCompilerInputs {
    StringView pattern;
    OptionSet<Yarr::Flags> flags;
    const RegExpSummary& summary;
    const AssemblerPolicy& assemblerPolicy;
    BuildContext* context;
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

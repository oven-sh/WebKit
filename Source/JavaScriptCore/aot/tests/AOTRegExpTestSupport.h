/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTNativeTest.h"
#include "AOTRegExpCompiler.h"
#include "AOTRegExpSummary.h"
#include <wtf/StdLibExtras.h>
#include <wtf/text/WTFString.h>

namespace JSC::AOT::RegExpTests {

using Form = RegExpRecordForm;

struct Source {
    String pattern;
    OptionSet<Yarr::Flags> flags;
};

inline Wire::CompileOptions makeOptions(StringView word)
{
    Wire::CompileOptions options = makeDefaultCompileOptions();
    AOT_TEST_CHECK(!applyOptionWord(options, word));
    return options;
}

inline uint64_t limitHits(BuildContext& context, LimitID limit)
{
    return context.statistics().values()[static_cast<unsigned>(hitsOf(limit))];
}

inline bool isSameCode(const RegExpCode& a, const RegExpCode& b)
{
    return a.body.bytes == b.body.bytes && a.body.relocations == b.body.relocations && a.symbols == b.symbols && a.reach == b.reach;
}

inline AssemblerPolicy policyFor(const BuildContext* context)
{
    return context ? context->assemblerPolicy() : AssemblerPolicy::forImage(CPUFeatureSet::buildTarget());
}

inline CompiledRegExp compile(const Source& source, BuildContext* context = nullptr)
{
    AssemblerPolicy policy = policyFor(context);
    uint64_t depthLimit = context ? context->limit(LimitID::RegExpNestingDepthWithCode) : Limits::regExpParenthesesDepth;
    RegExpSummary summary = summarizeRegExp(source.pattern, source.flags, safeCast<unsigned>(depthLimit));
    return compileRegExp({ source.pattern, source.flags, summary, policy, context });
}

inline CompiledRegExp compileToCode(const Source& source, BuildContext* context = nullptr)
{
    CompiledRegExp regExp = compile(source, context);
    AOT_TEST_CHECK(regExp.form == Form::Code);
    return regExp;
}

} // namespace JSC::AOT::RegExpTests

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)

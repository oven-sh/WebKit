/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "YarrFlags.h"
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isValidLiteral(FunctionPassContext& context, const Value& literal, Vector<char16_t>& patternBuffer)
{
    OptionSet<Yarr::Flags> flags = toYarrFlags(literal.regExpFlags());
    Yarr::FlagsString flagCharacters = Yarr::flagsString(flags);
    if (!Yarr::parseFlags(StringView { flagCharacters.span() }))
        return false;
    StringView pattern = context.program.strings.viewOf(literal.pattern(), patternBuffer);
    return context.build.regExpSummaries().summaryOf(literal.pattern(), flags, pattern).isValid();
}

PassOutcome checkRegExpLiterals(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<char16_t> patternBuffer;
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID id : function.at(block).values) {
            const Value& value = function.at(id);
            if (value.opcode != Opcode::NewRegExp || isValidLiteral(context, value, patternBuffer))
                continue;
            context.build.diagnostics().add(UserDiagnostic {
                .kind = UserDiagnosticKind::InvalidRegExpLiteral,
                .function = context.id,
                .block = block,
                .value = id,
                .location = function.locationOf(id),
            });
        }
    }
    return PassOutcome::Unchanged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

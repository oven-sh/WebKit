/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTBuiltinTypes.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTSplitUnions.h"
#include <algorithm>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct MatchRewrite {
    ValueID original;
    Value rewritten;
};

} // anonymous namespace

static bool isNeverGlobal(const Type& regExp)
{
    if (!pristineAspectsProvenBy(regExp, BuiltinClasses::RegExp).contains(PristineAspects::AsCreated) || regExp.regExpLiterals().empty())
        return false;
    return std::ranges::none_of(regExp.regExpLiterals(), [](const RegExpLiteralInType& literal) {
        return literal.flags().contains(RegExpFlags::Global);
    });
}

// s.match(r) is r.exec(s) where r is not global.
static std::optional<Value> makeExecOfMatch(FunctionPassContext& context, const Value& call)
{
    const Program& program = context.program;
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || inlineImplementationOf(program, call) != InlineBuiltin::StringMatch)
        return std::nullopt;

    ValueID string = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!string || arguments.size() != 1 || !program.typeOf(context.function, string).isAnyString() || !isNeverGlobal(program.typeOf(context.function, arguments[0])))
        return std::nullopt;

    TypingContext typing { program, context.function, context.types };
    BuiltinOperandTypes operands = operandTypesOf(typing, arguments[0], { &string, 1 });
    OverloadMatch match = matchOverload({ typing, Builtins::RegExpPrototypeExec, BuiltinCallMode::Call, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty())
        return std::nullopt;

    Value exec = makeBuiltinCall(match.overload, BuiltinCallForm::Whole, { arguments[0], { string } }, { call.expectedResult(), call.accessSite(), call.allocationSite() });
    if (!overloadTakesOperands(typing, exec))
        return std::nullopt;
    return exec;
}

unsigned simplifyMatches(FunctionPassContext& context)
{
    Vector<MatchRewrite> rewrites;
    forEachValueSite(context.analyses, context.function, [&](const ValueSite& site) {
        std::optional<Value> rewritten = makeExecOfMatch(context, context.function.at(site.value));
        if (rewritten && keepsTypeOf(context, site.value, *rewritten))
            rewrites.append(MatchRewrite { site.value, WTF::move(*rewritten) });
    });

    for (MatchRewrite& rewrite : rewrites)
        replaceKeepingType(context, rewrite.original, WTF::move(rewrite.rewritten));
    if (!rewrites.isEmpty())
        context.analyses.didChangeValues();
    return static_cast<unsigned>(rewrites.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

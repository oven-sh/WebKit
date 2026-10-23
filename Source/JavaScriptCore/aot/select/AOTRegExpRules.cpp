/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineBuiltinForms.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinFormRules.h"
#include "AOTBuiltinTypes.h"
#include "AOTFlowResults.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTVector.h"
#include "YarrFlags.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct LiteralMatch {
    SelectionGroup group;
    ValueID string;
    ValueID regExp;
    bool usesObject { true };
    RegExpLiteralInType literal;
    RegExpMatchForm form { RegExpMatchForm::Test };
};

} // anonymous namespace

static std::optional<RegExpMatchForm> matchFormOf(InlineBuiltin builtin)
{
    switch (builtin) {
    case InlineBuiltin::RegExpTest:
        return RegExpMatchForm::Test;
    case InlineBuiltin::RegExpExec:
        return RegExpMatchForm::Exec;
    case InlineBuiltin::StringSearch:
        return RegExpMatchForm::Search;
    case InlineBuiltin::StringMatch:
        return RegExpMatchForm::Match;
    default:
        return std::nullopt;
    }
}

static bool takesStringAsReceiver(RegExpMatchForm form)
{
    switch (form) {
    case RegExpMatchForm::Test:
    case RegExpMatchForm::Exec:
        return false;
    case RegExpMatchForm::Search:
    case RegExpMatchForm::Match:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isProvenPristine(const Type& regExp)
{
    return pristineAspectsProvenBy(regExp, BuiltinClasses::RegExp).containsAll({ PristineAspects::Storage, PristineAspects::Names });
}

static std::optional<LiteralMatch> findLiteralMatch(SelectionContext& context, ValueID id)
{
    const Program& program = context.program();
    const Value& call = context.function().at(id);
    if (call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(program, call);
    if (!builtin)
        return std::nullopt;
    std::optional<RegExpMatchForm> form = matchFormOf(*builtin);
    if (!form)
        return std::nullopt;

    ValueID receiver = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!receiver || arguments.size() != 1)
        return std::nullopt;
    ValueID string = takesStringAsReceiver(*form) ? receiver : arguments[0];
    ValueID regExp = takesStringAsReceiver(*form) ? arguments[0] : receiver;
    if (!context.typeOf(string).isAnyString())
        return std::nullopt;

    const Type& regExpType = context.typeOf(regExp);
    std::optional<RegExpLiteralInType> literal = singleRegExpLiteralAsCreatedOf(regExpType);
    if (!literal || !isProvenPristine(regExpType))
        return std::nullopt;

    Vector<char16_t> patternBuffer;
    StringView pattern = program.strings.viewOf(literal->pattern, patternBuffer);
    const RegExpSummary& summary = context.build().regExpSummaries().summaryOf(literal->pattern, toYarrFlags(literal->flags()), pattern);
    if (summary.metadata != RegExpMetadata::Known || !summary.isValid() || !summary.atom.isNull())
        return std::nullopt;

    // What the last match left in RegExp names the RegExp of the object.
    bool usesObject = literal->flags().containsAny(flagsOfMatchesUsingObject) || context.flow().mayReadLegacyRegExpStatics();
    return LiteralMatch { groupOfOne(id), string, regExp, usesObject, *literal, *form };
}

void selectRegExpLiteralMatch(SelectionContext& context)
{
    Vector<LiteralMatch> matches;
    for (ValueID id : context.candidates(Opcode::CallBuiltin)) {
        std::optional<LiteralMatch> match = findLiteralMatch(context, id);
        if (match && tryClaimWithoutOperand(context, SelectionRuleID::RegExpLiteralMatch, match->group, match->usesObject ? ValueID() : match->regExp) == ClaimResult::Claimed)
            matches.append(WTF::move(*match));
    }

    context.rewriteGroups(SelectionRuleID::RegExpLiteralMatch, matches, [](GroupRewriter& rewriter, const LiteralMatch& match) {
        BuiltinOverloadID overload = rewriter.original(match.group.root).overload();
        Value& root = rewriter.replaceRoot(Opcode::RegExpMatchLiteral);
        root.setOverload(overload);
        root.setStringOperand(match.string);
        if (match.usesObject)
            root.setRegExpOperand(match.regExp);
        root.setPattern(match.literal.pattern);
        root.setRegExpFlags(match.literal.flags());
        root.setMatchForm(match.form);
    });
}

static bool hasValidConstantFlags(const InlineBuiltinSite& site, ValueID flags)
{
    const Program& program = site.context.program();
    std::optional<StringID> text = stringConstantOf(program, site.context.function(), flags);
    if (!text)
        return false;
    Vector<char16_t> buffer;
    return Yarr::parseFlags(program.strings.viewOf(*text, buffer)).has_value();
}

static InlineForm chooseFormOfNewRegExp(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    if (arguments.size() < 2 || hasValidConstantFlags(site, arguments[1]))
        return InlineForm::Whole;
    return InlineForm::Call;
}

static bool mayHaveNegativeLimit(const InlineBuiltinSite& site)
{
    std::span<const ValueID> arguments = site.arguments();
    return arguments.size() > 1 && site.typeOf(arguments[1]).integers.minimum < 0;
}

#define AOT_COUNT_INLINE_BUILTIN(name) + 1
static_assert(0 FOR_EACH_AOT_INLINE_BUILTIN_IN_REG_EXP_BUILTIN(AOT_COUNT_INLINE_BUILTIN) == 15, "chooseFormOfRegExpBuiltin needs a case for every member of the family");
#undef AOT_COUNT_INLINE_BUILTIN

InlineForm chooseFormOfRegExpBuiltin(const InlineBuiltinSite& site)
{
    switch (site.builtin) {
    case InlineBuiltin::RegExpExec:
    case InlineBuiltin::RegExpTest:
    case InlineBuiltin::RegExpFlags:
    case InlineBuiltin::RegExpSource:
    case InlineBuiltin::RegExpFlag:
    case InlineBuiltin::RegExpGetLastIndex:
    case InlineBuiltin::RegExpSetLastIndex:
    case InlineBuiltin::RegExpExecPrefix:
    case InlineBuiltin::StringMatchPrefix:
    case InlineBuiltin::StringReplace:
    case InlineBuiltin::StringReplaceAll:
        return InlineForm::Whole;
    case InlineBuiltin::NewRegExp:
        return chooseFormOfNewRegExp(site);
    case InlineBuiltin::StringMatch:
    case InlineBuiltin::StringSearch:
        return wholeOrTwoWay(site, true);
    case InlineBuiltin::StringSplit:
        return wholeOrTwoWay(site, !mayHaveNegativeLimit(site));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

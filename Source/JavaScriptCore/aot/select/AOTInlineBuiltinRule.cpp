/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTInlineBuiltinForms.h"

#if ENABLE(AOT)

#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct InlineMatch {
    SelectionGroup group;
    InlineBuiltin builtin;
    InlineForm form;
};

} // anonymous namespace

ValueID InlineBuiltinSite::receiver() const
{
    return value().receiverOperand();
}

std::span<const ValueID> InlineBuiltinSite::arguments() const
{
    return value().argumentsOperands();
}

std::optional<InlineBuiltin> inlineImplementationOf(const Program& program, const Value& call)
{
    RELEASE_ASSERT(call.opcode == Opcode::CallBuiltin);
    const BuiltinCode& code = overloadOf(program, call).code;
    switch (call.builtinCallForm()) {
    case BuiltinCallForm::Whole:
        if (code.implementation.kind != BuiltinImplementationKind::Inline)
            return std::nullopt;
        return code.implementation.inlineBuiltin;
    case BuiltinCallForm::Fields:
        return code.fieldsForm;
    case BuiltinCallForm::Prefix:
        return code.prefixForm;
    case BuiltinCallForm::List:
        return code.listForm;
    case BuiltinCallForm::AnyReceiver:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

InlineForm chooseInlineForm(const InlineBuiltinSite& site)
{
    InlineForm form = InlineForm::Call;
    switch (familyOf(site.builtin)) {
#define AOT_CHOOSE_INLINE_FORM(family) \
    case InlineBuiltinFamily::family: \
        form = chooseFormOf##family(site); \
        break;
    FOR_EACH_AOT_INLINE_BUILTIN_FAMILY(AOT_CHOOSE_INLINE_FORM)
#undef AOT_CHOOSE_INLINE_FORM
    }
    RELEASE_ASSERT(site.hasSlowPath || form == InlineForm::Whole);
    return form;
}

static bool canBecomeInlineCall(BuiltinCallForm form)
{
    switch (form) {
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::List:
        return true;
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
    case BuiltinCallForm::AnyReceiver:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasSlowPath(const BuiltinImplementation& implementation, BuiltinCallForm form)
{
    switch (form) {
    case BuiltinCallForm::Whole:
        return implementation.otherwise.has_value();
    case BuiltinCallForm::List:
        return true;
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
    case BuiltinCallForm::AnyReceiver:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Entry>
static void fillInlineCall(Entry& entry, const Program& program, const Value& original, InlineBuiltin builtin)
{
    const BuiltinImplementation& implementation = overloadOf(program, original).code.implementation;
    uint32_t numberOfParameters = numberOfParametersOf(builtin);
    RELEASE_ASSERT(!numberOfParameters || implementation.inlineBuiltin == builtin);
    Vector<uint32_t, 4> parameters;
    for (uint8_t parameter : std::span(implementation.inlineParameters).first(numberOfParameters))
        parameters.append(parameter);

    entry.setInlineBuiltin(builtin);
    entry.setInlineParameters(parameters.span());
    entry.setOverload(original.overload());
    if (ValueID receiver = original.receiverOperand())
        entry.setReceiverOperand(receiver);
    entry.setArgumentsOperands(original.argumentsOperands());
    entry.setAccessSite(original.accessSite());
    entry.setAllocationSite(original.allocationSite());
    if constexpr (std::same_as<Entry, Value>) {
        entry.setIsDoneInPlace(original.isDoneInPlace());
        if (builtin == InlineBuiltin::FunctionApply)
            entry.setCalleeName(original.calleeName());
    }
}

std::optional<InlineBuiltinSite> findInlineBuiltinSite(SelectionContext& context, ValueID id)
{
    const Program& program = context.program();
    const Value& call = context.function().at(id);
    if (!canBecomeInlineCall(call.builtinCallForm()))
        return std::nullopt;
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(program, call);
    if (!builtin)
        return std::nullopt;
    ValueLocation location = context.analyses().useLists().locationOf(id);
    if (location.placement != ValuePlacement::InBlock)
        return std::nullopt;

    const BuiltinOverload& overload = overloadOf(program, call);
    const BuiltinImplementation& implementation = overload.code.implementation;
    bool callHasSlowPath = hasSlowPath(implementation, call.builtinCallForm());
    bool isWhole = call.builtinCallForm() == BuiltinCallForm::Whole;
    if (isWhole && implementation.maximumArgumentCount && call.argumentsOperands().size() > *implementation.maximumArgumentCount) {
        RELEASE_ASSERT(callHasSlowPath);
        return std::nullopt;
    }
    return InlineBuiltinSite { context, id, overload, *builtin, callHasSlowPath, context.shapeOf(location.block) };
}

static std::optional<InlineMatch> findInlineMatch(SelectionContext& context, ValueID id)
{
    const Function& function = context.function();
    std::optional<InlineBuiltinSite> site = findInlineBuiltinSite(context, id);
    if (!site)
        return std::nullopt;
    if (site->hasSlowPath && (context.isOnSlowPath(id) || isOwnedByReturnCall(context, id)))
        return std::nullopt;

    InlineForm form = chooseInlineForm(*site);
    if (form == InlineForm::TwoWay && numberOfResultsOf(context.program(), function, function.at(id)))
        form = InlineForm::Call;
    if (form == InlineForm::Call)
        return std::nullopt;

    InlineMatch match { groupOfOne(id), site->builtin, form };
    match.group.hasSlowPath = form == InlineForm::TwoWay;
    return match;
}

static void rewriteInlineCall(const Program& program, GroupRewriter& rewriter, const InlineMatch& match)
{
    ValueID root = match.group.root;
    const Value& original = rewriter.original(root);
    switch (match.form) {
    case InlineForm::Call:
        break;
    case InlineForm::TwoWay: {
        Terminator fast(TerminatorKind::TryCallInline);
        fillInlineCall(fast, program, original, match.builtin);
        rewriter.emitTwoWay(WTF::move(fast));
        return;
    }
    case InlineForm::Whole:
        const Terminator& terminator = rewriter.function().at(rewriter.rootBlock()).terminator;
        if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == root)
            rewriter.builder().demoteReturnCall(rewriter.rootBlock());
        Value& inlineCall = rewriter.replaceRoot(Opcode::CallInline);
        fillInlineCall(inlineCall, program, original, match.builtin);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void selectInlineBuiltin(SelectionContext& context)
{
    Vector<InlineMatch> matches;
    for (ValueID id : context.candidates(Opcode::CallBuiltin)) {
        std::optional<InlineMatch> match = findInlineMatch(context, id);
        if (match && context.tryClaim(SelectionRuleID::InlineBuiltin, match->group) == ClaimResult::Claimed)
            matches.append(WTF::move(*match));
    }

    const Program& program = context.program();
    context.rewriteGroups(SelectionRuleID::InlineBuiltin, matches, [&](GroupRewriter& rewriter, const InlineMatch& match) {
        rewriteInlineCall(program, rewriter, match);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

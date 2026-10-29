/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTInlineBuiltinForms.h"
#include "AOTKnownValue.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ThrownError {
    SelectionGroup group;
    ErrorKind errorKind;
    StringID message;
    std::optional<uint32_t> throwPosition;
};

} // anonymous namespace

static_assert(numberOfParametersOf(InlineBuiltin::NewError) == 1);

static ErrorKind errorKindOf(const BuiltinOverload& overload)
{
    unsigned errorKind = overload.code.implementation.inlineParameters[0];
    RELEASE_ASSERT(errorKind < numberOfEnumerators<ErrorKind>);
    return static_cast<ErrorKind>(errorKind);
}

static const Terminator* findThrowAtCreation(SelectionContext& context, ValueID call)
{
    const UseLists& useLists = context.analyses().useLists();
    ValueLocation location = useLists.locationOf(call);
    if (location.placement != ValuePlacement::InBlock)
        return nullptr;

    const Block& block = context.function().at(location.block);
    // A Throw reaches a catch handler of its own function by a jump, and ThrowNewError would reach it by unwinding.
    if (block.handler)
        return nullptr;

    const Terminator& terminator = block.terminator;
    if (terminator.kind != TerminatorKind::Throw || terminator.valueOperand() != call || location.index + 1 != block.values.size() || useLists.useCount(call) != 1)
        return nullptr;
    // One call site has one list of frames.
    if (terminator.inlineFrame != context.function().at(call).inlineFrame)
        return nullptr;
    return &terminator;
}

static bool canBeTopFrameOfStackTrace(const Program& program, const Function& function, const Value& call)
{
    return !program.sourceFunctionOf(function, call).isHiddenInStackTraces();
}

static std::optional<ThrownError> findThrownError(SelectionContext& context, ValueID id)
{
    const Program& program = context.program();
    const Function& function = context.function();
    const Value& call = function.at(id);
    if (call.builtinCallForm() != BuiltinCallForm::Whole || inlineImplementationOf(program, call) != InlineBuiltin::NewError)
        return std::nullopt;

    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() != 1)
        return std::nullopt;
    KnownValue message = knownValueOf(program, function, arguments[0]);
    if (message.kind() != KnownValue::Kind::String)
        return std::nullopt;

    const Terminator* thrower = findThrowAtCreation(context, id);
    if (!thrower || !canBeTopFrameOfStackTrace(program, function, call))
        return std::nullopt;

    std::optional<uint32_t> throwPosition;
    if (thrower->hasPosition() && thrower->position != call.position)
        throwPosition = thrower->position;
    return ThrownError { groupOfOne(id), errorKindOf(overloadOf(program, call)), message.asString(), throwPosition };
}

void selectThrowNewError(SelectionContext& context)
{
    Vector<ThrownError> matches;
    for (ValueID id : context.candidates(Opcode::CallBuiltin)) {
        std::optional<ThrownError> match = findThrownError(context, id);
        if (match && context.tryClaim(SelectionRuleID::ThrowNewError, match->group) == ClaimResult::Claimed)
            matches.append(WTF::move(*match));
    }

    context.rewriteGroups(SelectionRuleID::ThrowNewError, matches, [](GroupRewriter& rewriter, const ThrownError& match) {
        Value& root = rewriter.replaceRoot(Opcode::ThrowNewError);
        root.setErrorKind(match.errorKind);
        root.setMessage(match.message);
        root.setThrowPosition(match.throwPosition);
        rewriter.endBlockAsUnreachable();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

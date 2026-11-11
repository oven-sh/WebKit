/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStringFolds.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ErrorClassWithKindInStringErrors {
    NamedBuiltinClass builtinClass;
    ErrorKind kind;
};

} // anonymous namespace

static constexpr ErrorClassWithKindInStringErrors errorClassesThatTakeMessageFirstInStringErrors[] = {
    { BuiltinClasses::Error, ErrorKind::Error },
    { BuiltinClasses::EvalError, ErrorKind::EvalError },
    { BuiltinClasses::RangeError, ErrorKind::RangeError },
    { BuiltinClasses::ReferenceError, ErrorKind::ReferenceError },
    { BuiltinClasses::SyntaxError, ErrorKind::SyntaxError },
    { BuiltinClasses::TypeError, ErrorKind::TypeError },
    { BuiltinClasses::URIError, ErrorKind::URIError },
};

static std::optional<ErrorKind> errorKindMadeByInStringErrors(const BuiltinTable& table, BuiltinID callee)
{
    BuiltinClassID made = table.at(callee).instanceClass;
    if (!made || !table.at(made).constructor || table.canonicalID(table.at(made).constructor) != callee)
        return std::nullopt;
    for (const ErrorClassWithKindInStringErrors& errorClass : errorClassesThatTakeMessageFirstInStringErrors) {
        if (made == errorClass.builtinClass)
            return errorClass.kind;
    }
    return std::nullopt;
}

static ValueID findErrorBehindItsCastsInStringErrors(const BodyIndex& index, const BodyBlock& block, ValueID made)
{
    ValueID handedOn = made;
    for (uint32_t next = made.index() + 1; next < block.endOfValues().index(); ++next) {
        BodyValue cast = index.value(ValueID(next));
        if (!opcodesThatStateTypes.contains(cast.opcode()) || cast.operand(0) != handedOn)
            return { };
        handedOn = cast.id();
    }
    return handedOn;
}

bool buildThrowOfNewError(Walker& walker, const CallByOneRow& call)
{
    if ((call.mode != BuiltinCallMode::Call && call.mode != BuiltinCallMode::Construct) || call.receiver || call.arguments.size() != 1)
        return false;
    std::optional<ErrorKind> errorKind = errorKindMadeByInStringErrors(walker.program().builtins(), call.builtin);
    if (!errorKind)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    ValueID made = call.call.id();
    BodyBlock block = index.block(index.blockOf(made));
    BodyTerminator thrower = block.terminator();
    if (thrower.kind() != TerminatorKind::Throw || thrower.operand(0) != findErrorBehindItsCastsInStringErrors(index, block, made))
        return false;
    if (block.hasHandler() || !walker.walksOwnBodyOfUnit() || walker.code().at(walker.currentBlock()).handler)
        return false;
    if (walker.program().at(walker.currentFunction()).isHiddenInStackTraces())
        return false;
    StringID message = textKnownForStrings(walker, call.arguments[0]);
    if (!message || liesInThread(message))
        return false;

    std::optional<uint32_t> throwPosition;
    uint32_t positionOfThrow = thrower.origin().position;
    if (positionOfThrow != DraftWords::noPosition && positionOfThrow != call.origin.position)
        throwPosition = positionOfThrow;

    walker.endLifetimesAtThrow({ });
    if (walker.isUnreachable())
        return true;
    Draft error(Opcode::ThrowNewError, call.origin);
    walker.chooseConservativeFields(error);
    error.setErrorKind(*errorKind);
    error.setMessage(message);
    error.setThrowPosition(throwPosition);
    walker.build(WTF::move(error));
    if (!walker.isUnreachable())
        walker.endAsUnreachable(call.origin);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

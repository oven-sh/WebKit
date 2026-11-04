/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIntlBuiltinEmitter.h"

#if ENABLE(AOT)

#include "AOTDateBuiltinEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

IntlBuiltinEmitter::IntlBuiltinEmitter(CodeGenerator& generator)
    : BuiltinFamilyEmitter(generator)
{
}

// The operation hands back null, and has done nothing that the program can see, for a receiver that is no formatter as the realm makes them and for a value that the formatter refuses.
void IntlBuiltinEmitter::finishWithTextOrMiss(const InlineCall& call, CellPointer textOrNull)
{
    missUnless(call, isNotZero(Integer(textOrNull.value())));
    finish(call, textOrNull);
}

void IntlBuiltinEmitter::generateFormatNumber(const InlineCall& call)
{
    RELEASE_ASSERT(call.numberOfArguments());
    Boxed formatter = boxedOperand(call.context(), call.receiver());
    Number number = numberOperand(call.argument(0));
    finishWithTextOrMiss(call, callOperation<CellPointer>(call.context(), RuntimeOperation::FormatNumber, { formatter, number }));
}

void IntlBuiltinEmitter::generateFormatDateTime(const InlineCall& call)
{
    RELEASE_ASSERT(call.numberOfArguments());
    Boxed formatter = boxedOperand(call.context(), call.receiver());
    ValueID formatted = call.argument(0);
    Number timeValue = typeOf(formatted).isNumber() ? numberOperand(formatted) : emitter<DateBuiltinEmitter>().timeValueOf(cellOperand(formatted));
    finishWithTextOrMiss(call, callOperation<CellPointer>(call.context(), RuntimeOperation::FormatDateTime, { formatter, timeValue }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

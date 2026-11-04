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

void IntlBuiltinEmitter::finishWithTextOrMiss(const InlineCall& call, CellPointer textOrNull)
{
    missUnless(call, Condition(m_out.notNull(textOrNull.value())));
    finish(call, textOrNull);
}

void IntlBuiltinEmitter::generateFormatNumber(const InlineCall& call)
{
    Boxed formatter = boxedOperand(call.context(), call.receiver());
    Number number = numberOperand(call.argument(0));
    finishWithTextOrMiss(call, callOperation<CellPointer>(call.context(), RuntimeOperation::FormatNumber, { formatter, number }));
}

void IntlBuiltinEmitter::generateFormatDateTime(const InlineCall& call)
{
    RELEASE_ASSERT(call.numberOfArguments());
    Boxed formatter = boxedOperand(call.context(), call.receiver());
    Number timeValue = typeOf(call.argument(0)).isNumber() ? numberOperand(call.argument(0)) : emitter<DateBuiltinEmitter>().timeValueOf(cellOperand(call.argument(0)));
    finishWithTextOrMiss(call, callOperation<CellPointer>(call.context(), RuntimeOperation::FormatDateTime, { formatter, timeValue }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolvedCallEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTRegExpFlags.h"
#include "AOTStubsRegExp.h"

namespace JSC { namespace AOT { namespace Typed {

void ResolvedCallEmitter::generateRegExpMatchLiteral(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    ValueID regExp = value->regExpOperand();
    bool recordsMatch = m_facts.mayReadLegacyRegExpStatics();
    // Without the object the stub takes the string alone and leaves no record of the match.
    if (value->guardOperand() || (!regExp && (value->regExpFlags().containsAny(flagsOfMatchesUsingObject) || recordsMatch))) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    CellPointer string = in<CellPointer>(value->stringOperand());
    NumberedImmediate record { m_mentions.regExp(value->pattern(), toYarrFlags(value->regExpFlags())) };
    if (!regExp) {
        define(value, Boxed(callStub(value, regExpMatchLiteralWithoutObject(value->matchForm()), { string.value() }, { .numberedImmediate = record }).first));
        return;
    }

    StubKey stub = regExpMatchLiteral(value->matchForm(), value->regExpFlags().contains(RegExpFlags::Global), recordsMatch);
    define(value, Boxed(callStub(value, stub, { in<CellPointer>(regExp).value(), string.value() }, { .numberedImmediate = record }).first));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

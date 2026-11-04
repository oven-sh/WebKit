/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolvedCallEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTStubCatalog.h"
#include "AOTStubsRegExp.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

void ResolvedCallEmitter::generateRegExpMatchLiteral(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    RegExpRecordID record = value->regExpRecord();
    if (!record) {
        fail(value, InternalErrorKind::PlacedPayloadMissing);
        define(value, emitter<ConstantEmitter>().zeroOf(value.representation()));
        return;
    }

    CellPointer string = in<CellPointer>(value->stringOperand());
    std::optional<CellPointer> regExpObject = inOptional<CellPointer>(value->regExpOperand());
    if (!regExpObject) {
        define(value, Boxed(callStub(value, regExpMatchLiteralWithoutObject(value->matchForm()), { string.value() }, { .immediate = record.index() }).first));
        return;
    }

    StubKey thunk = regExpMatchLiteral(value->matchForm(), value->regExpFlags().contains(RegExpFlags::Global));
    define(value, Boxed(callStub(value, thunk, { regExpObject->value(), string.value() }, { .immediate = record.index() }).first));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

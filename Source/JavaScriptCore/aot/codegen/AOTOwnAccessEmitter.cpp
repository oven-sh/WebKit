/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOwnAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTIdentifierNumbering.h"
#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

OwnAccessEmitter::OwnAccessEmitter(CodeGenerator& generator)
    : OperationEmitter(generator)
{
}

void OwnAccessEmitter::generatePropHasOwn(const ValueContext& value)
{
    Boxed object = in<Boxed>(value->objectOperand());
    if (value.shape() == CodeShape::Compact) {
        Boxed key = boxCell(emitter<ConstantEmitter>().keyConstant(value, value->key()));
        defineTest(value, callOperation<Condition>(value, RuntimeOperation::HasOwnProperty, { object, key }));
        return;
    }

    std::optional<uint32_t> identifier = build().identifierNumbering().numberOf(value->key());
    if (!identifier) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        defineTest(value, boolean(false));
        return;
    }
    defineTest(value, Condition(m_out.castToInt32(callStub(value, inById(PresenceQuestion::HasOwn), { object.value() }, { .immediate = *identifier }).first)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

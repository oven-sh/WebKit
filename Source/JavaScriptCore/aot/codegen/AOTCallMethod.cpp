/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFusedAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTCallEmitter.h"
#include "AOTConvention.h"
#include "AOTIdentifierNumbering.h"
#include "AOTStubsCalls.h"
#include "AOTStubsPropertyKeys.h"

namespace JSC { namespace AOT { namespace Typed {

void FusedAccessEmitter::generateCallMethod(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    std::span<const ValueID> arguments = value->argumentsOperands();
    RELEASE_ASSERT(arguments.size() <= numberOfArgumentGPRs);
    unsigned count = static_cast<unsigned>(arguments.size());

    Boxed receiver = in<Boxed>(value->receiverOperand());
    Vector<LValue, 8> values;
    for (ValueID argument : arguments)
        values.append(in<Boxed>(argument).value());
    std::optional<uint32_t> number = build().identifierNumbering().numberOf(value->key());
    RELEASE_ASSERT(number);

    CallEmitter& calls = emitter<CallEmitter>();
    ConventionCall call {
        .stub = callMethodById(count),
        .thisValue = receiver.value(),
        .arguments = values.span(),
    };
    SiteCacheID siteCache = value->siteCache();
    if (!siteCache) {
        // One site stands for the read and the call. It has the position of the call, which every stack trace taken in the callee shows.
        define(value, calls.callByConvention(value, call, { .immediate = *number, .isTailCall = value->isDemotedTailCall() }));
        return;
    }

    call.callee = callStub(value, findMethodByIdAtSite(), { receiver.value(), m_out.constInt64(*number), siteCacheAddress(siteCache).value() }, { .origin = value->readOrigin() }).first;
    call.stub = callValue(count);
    define(value, calls.callByConvention(value, call, { .isTailCall = value->isDemotedTailCall() }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

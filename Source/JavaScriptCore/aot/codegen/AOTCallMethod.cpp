/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFusedAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTAccessEmitter.h"
#include "AOTCallEmitter.h"
#include "AOTConvention.h"
#include "AOTIdentifierNumbering.h"
#include "AOTStubsCalls.h"
#include "AOTStubsPropertyKeys.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

void FusedAccessEmitter::generateCallMethod(const ValueContext& value)
{
    RELEASE_ASSERT(!value.returnCall());
    std::span<const ValueID> arguments = value->argumentsOperands();
    RELEASE_ASSERT(arguments.size() <= numberOfArgumentGPRs);
    unsigned count = static_cast<unsigned>(arguments.size());

    Boxed receiver = in<Boxed>(value->receiverOperand());
    emitter<AccessEmitter>().recordArrival(value, receiver);
    Vector<LValue, 8> values;
    for (ValueID argument : arguments)
        values.append(in<Boxed>(argument).value());
    std::optional<uint32_t> number = build().identifierNumbering().numberOf(value->key());
    RELEASE_ASSERT(number);

    // The stubs know the key. The site knows what the program calls the method: `a.b.key`.
    std::optional<ThrowSite> siteThatNamesCallee = CallEmitter::siteThatNamesCallee(value);
    CallEmitter& calls = emitter<CallEmitter>();
    ConventionCall call {
        .stub = callMethodById(count),
        .thisValue = receiver.value(),
        .arguments = values.span(),
    };
    // A cache of its own pays for a site in fast code. Elsewhere the tables find the method, and the site is one call in place of two.
    SiteCacheID siteCache = value->siteCache();
    if (!siteCache || value.shape() != CodeShape::Fast) {
        // One return address stands for the read and the call. The stub says which of the two it is at.
        define(value, calls.callByConvention(value, call, { .immediate = *number, .readBeforeCall = value->readOrigin(), .throwSite = siteThatNamesCallee }));
        return;
    }

    StubKey stub = emitter<AccessEmitter>().stubThatReadsAtSite(*number, value->receiverOperand());
    call.callee = callStub(value, stub, { receiver.value(), siteCacheAddress(siteCache).value() }, { .immediate = *number, .origin = value->readOrigin() }).first;
    call.stub = callValue(count, CallEmitter::positionOf(value));
    define(value, calls.callByConvention(value, call, { .throwSite = siteThatNamesCallee }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

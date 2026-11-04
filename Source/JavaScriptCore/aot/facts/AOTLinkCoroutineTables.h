/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTTypeIDs.h"

namespace JSC { namespace AOT { namespace Typed {

// A coroutine that is reached. PROOF in Facts::linked(), EXPECTATION in Facts::held(). Every TypeID lies in the program's table. Null: nothing is known. `never`: nothing ever arrives.
struct LinkedCoroutine {
    FunctionID function;
    // Of an async function that is no generator: what `await` of the promise of a call goes on with: what the returns hand over, settled.
    TypeID fulfilledWith;
    // Of every coroutine: what `return` hands over in the text, as it is. undefined is in it where the body may run off its end.
    TypeID returned;
    // Of a generator, synchronous or not: what a step hands out while the body yields (of an async generator: awaited), and what `yield` gives.
    TypeID yielded;
    TypeID sent;
    // No object that a call answers with has reached unseen code or met the objects of another site, and none has got a property, a definition, a deletion or another prototype.
    // Of a generator function the same holds of the object under its `prototype`. In held(): nothing in sight does any of it.
    bool objectsStayAsMade { false };
    // In linked() alone. LinkedFunction::everyCallHasOneCallee, SummaryCall::resultIsOnlyIterated of each of those calls, and nothing reads or stores a name off a closure of the function.
    bool objectsAreOnlyIterated { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

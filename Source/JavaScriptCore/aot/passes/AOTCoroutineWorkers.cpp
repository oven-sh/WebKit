/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCoroutinePlan.h"

#if ENABLE(AOT)

#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// The result declared for an async function is its promise. The worker returns what the promise is resolved with, of which only the returned values say anything proven.
static TypeID typeOfReturnedValues(const Function& function, TypeArena& types)
{
    TypeID returned = TypeTable::never;
    for (const Block& block : function.blocks) {
        if (block.terminator.kind != TerminatorKind::Return)
            continue;
        ValueID value = block.terminator.valueOperand();
        returned = types.unionOf(returned, value ? function.at(value).type : TypeTable::undefined);
    }
    return returned;
}

// Nothing in the function suspends, so its code stays as it is: a return is a return, and what it throws reaches the caller, which is the ramp or a function that awaits the call.
Function buildAsyncWorker(const AnalysisContext&, TypeArena& types, Function&& unsplit, const CoroutinePlan& plan)
{
    const CreatorRecord::Coroutine& record = plan.record;
    RELEASE_ASSERT(plan.neverSuspends && record.kind == CoroutineKind::AsyncFunction && record.worker && record.worker == record.body);

    Function worker = WTF::move(unsplit);
    if (worker.runsOnlyBehindFailedTest())
        worker.flags.add(FunctionFlags::IsSplitOffGenericVersion);
    worker.role = FunctionRole::AsyncWorker;
    worker.creator = record;
    worker.flags.remove(FunctionFlags::IsAsync);
    worker.text = { };
    worker.results = Vector<DeclaredType> { DeclaredType { .type = typeOfReturnedValues(worker, types) } };
    worker.derivation = Derivation {
        .source = record.ramp,
        .parameterMap = Vector<ParameterOrigin>(worker.numberOfEntryParameters(), [](size_t index) {
            return ParameterOrigin { static_cast<uint32_t>(index) };
        }),
    };
    worker.capabilities = initialCapabilitiesOf(worker);
    return worker;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

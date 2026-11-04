/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "DeferGCInlines.h"
#include "VM.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/SetForScope.h>
#include <wtf/Threading.h>

namespace JSC { namespace AOT {

// An operation takes its caller for compiled code, which has an entry frame above it.
class OperationCallScope {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(OperationCallScope);
public:
    explicit OperationCallScope(VM& vm)
        : m_deferGC(vm)
        , m_topCallFrame(vm.topCallFrame, vm.topCallFrame)
        , m_topEntryFrame(vm.topEntryFrame, static_cast<EntryFrame*>(Thread::currentSingleton().stack().origin()))
    {
    }

private:
    DeferGC m_deferGC;
    SetForScope<CallFrame*> m_topCallFrame;
    SetForScope<EntryFrame*> m_topEntryFrame;
};

template<typename Operation, typename... Arguments>
auto callOperationFromTest(VM& vm, Operation operation, Arguments... arguments)
{
    OperationCallScope scope(vm);
    return operation(arguments...);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

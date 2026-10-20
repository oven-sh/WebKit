/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWatchpoints.h"

#if ENABLE(AOT)

#include <utility>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(PropertyValueWatchpoint);

void PropertyValueWatchpoint::watch(VM& vm, const ObjectPropertyCondition& condition, Callback callback, void* context)
{
    RELEASE_ASSERT(!m_callback && callback);
    m_callback = callback;
    m_context = context;
    initialize(condition);
    install(vm);
}

void PropertyValueWatchpoint::handleFire(VM& vm, const FireDetail&)
{
    Callback callback = std::exchange(m_callback, nullptr);
    void* context = std::exchange(m_context, nullptr);
    callback(vm, context);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

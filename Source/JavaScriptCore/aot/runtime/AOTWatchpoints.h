/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AdaptiveInferredPropertyValueWatchpointBase.h"
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

class PropertyValueWatchpoint final : public AdaptiveInferredPropertyValueWatchpointBase {
    WTF_MAKE_TZONE_ALLOCATED(PropertyValueWatchpoint);
public:
    using Callback = void (*)(VM&, void* context);

    void watch(VM&, const ObjectPropertyCondition&, Callback, void* context);

private:
    void handleFire(VM&, const FireDetail&) final;

    Callback m_callback { nullptr };
    void* m_context { nullptr };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

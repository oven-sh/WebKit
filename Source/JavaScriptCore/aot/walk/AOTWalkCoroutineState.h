/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <memory>

namespace JSC { namespace AOT { namespace Typed {

struct KeptForUnitOfCoroutines {
    virtual ~KeptForUnitOfCoroutines() = default;
};

struct CoroutineStateOfUnit {
    std::unique_ptr<KeptForUnitOfCoroutines> keptByPromises;
    std::unique_ptr<KeptForUnitOfCoroutines> keptByCalls;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

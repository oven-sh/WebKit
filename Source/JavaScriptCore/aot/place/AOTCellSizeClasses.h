/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class CellSizeClasses {
public:
    JS_EXPORT_PRIVATE explicit CellSizeClasses(double progression);

    JS_EXPORT_PRIVATE size_t cellSizeFor(size_t bytes) const;
    JS_EXPORT_PRIVATE unsigned roundInlineCapacity(unsigned inlineCapacity) const;

private:
    Vector<size_t> m_sizes;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

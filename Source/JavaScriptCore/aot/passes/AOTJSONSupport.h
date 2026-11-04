/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"

namespace JSC { namespace AOT { namespace Typed {

inline bool isJSONSerializedKey(const Program& program, PropertyKeyID key)
{
    return key && program.at(key).kind == PropertyKeyKind::Text;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

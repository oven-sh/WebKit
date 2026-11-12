/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

bool sortByKeysOfComparator(Walker&, const BodyValue& call);
bool copyKeyOfComparator(Walker&, const BodyValue& call, FunctionID comparator);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

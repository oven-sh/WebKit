/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

// For a program as read, before its values are typed. What each copy of a function makes has a layout of its own.
void giveLayoutsToCreatedObjects(Program&, const BitVector& copiedFunctions, BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

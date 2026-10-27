/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"

namespace JSC { namespace AOT {

class BuiltinTable;

namespace Typed {

BuiltinSlotID indexedElementSlotOf(const BuiltinTable&, BuiltinClassID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

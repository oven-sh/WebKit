/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

bool isLockedLocation(const BuiltinTable&, BuiltinID member);
// A member that is constant unless written and that start-up finds by its path from the global object.
bool canLockAtStartUp(const BuiltinTable&, BuiltinID member);
Vector<BuiltinID> constantBuiltinMembersToLock(const Program&, const FlowResults&);
bool canCloseAbsentNames(const BuiltinTable&, BuiltinID holder);
bool canFixPrototypeLinkAtStartUp(const BuiltinTable&, BuiltinID holder);
// The property that holds the built-in when the realm is created may hold something else later.
bool mayBeReplaced(const Program&, const FlowResults&, BuiltinID);

bool isWritten(const BuiltinLocationFlow&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

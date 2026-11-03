/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyDict.h"

namespace JSC {
namespace Python {

// types.SimpleNamespace(**attributes). The dict is copied; null is an empty one.
JSValue makeSimpleNamespace(JSGlobalObject*, PyDict* attributes);

// sentinel(name), as made in the module of that name. A null module is None, and a null repr is the name.
JSValue makeSentinel(JSGlobalObject*, const String& name, const String& module, const String& repr = { });

} // namespace Python
} // namespace JSC

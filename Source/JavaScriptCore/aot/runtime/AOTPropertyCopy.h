/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC {

class JSObject;
class VM;

namespace AOT {

struct Instance;

enum class TargetPrototypes : uint8_t { Ignored, MayIntercept };

bool tryCopyDataProperties(Instance*, VM&, JSObject* target, JSObject* source, TargetPrototypes = TargetPrototypes::Ignored);
bool tryCopyRecordDataProperties(Instance*, VM&, JSObject* target, JSObject* record);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <cstdint>

namespace JSC {

class JSObject;
class Structure;
class VM;

namespace AOT {

struct Instance;

enum class TargetPrototypes : uint8_t { Ignored, MayIntercept };

// An ordinary object with such a Structure gets a plain property by a store and, for a new name, the next Structure. Nobody has to be told.
bool takesPlainPropertiesDirectly(Structure* target);
bool tryCopyDataProperties(Instance*, VM&, JSObject* target, JSObject* source, TargetPrototypes = TargetPrototypes::Ignored);
bool tryCopyRecordDataProperties(Instance*, VM&, JSObject* target, JSObject* record);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

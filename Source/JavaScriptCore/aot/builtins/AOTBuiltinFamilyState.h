/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "PropertyOffset.h"
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class JSObject;
class Structure;

namespace AOT {

class BuiltinLocationWatch;
class JSONRuntime;
struct Instance;
struct State;

// What an unmapped arguments object looks like once callee and @@iterator are in place, as long as the realm starts such objects from the same Structure.
struct UnmappedArgumentsShape {
    Structure* initialStructure { nullptr };
    Structure* structure { nullptr };
    PropertyOffset offsetOfCallee { invalidOffset };
    PropertyOffset offsetOfIterator { invalidOffset };
};

struct BuiltinFamilyState {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(BuiltinFamilyState);
    WTF_MAKE_NONCOPYABLE(BuiltinFamilyState);

    explicit BuiltinFamilyState(State&);
    ~BuiltinFamilyState();

    void cacheObjectCreate(JSObject* prototype, Structure* structureOfNewObject);

    Instance& instance;
    std::unique_ptr<JSONRuntime> json;
    std::unique_ptr<BuiltinLocationWatch> locationWatch;
    UnmappedArgumentsShape unmappedArguments;
};

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)

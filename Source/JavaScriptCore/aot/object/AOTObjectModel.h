/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosureTable.h"
#include "AOTKeyOrderTable.h"
#include "AOTRuntimeLayoutTable.h"
#include "AOTStructureTable.h"
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

struct ObjectModelCaches;
struct State;

struct ObjectModel {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ObjectModel);
    WTF_MAKE_NONCOPYABLE(ObjectModel);

    explicit ObjectModel(State&);
    ~ObjectModel();

    RuntimeLayoutTable layouts;
    StructureTable structures;
    KeyOrderTable keyOrders;
    ClosureTable closures;
    ClassTable classes;
    std::unique_ptr<ObjectModelCaches> caches;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

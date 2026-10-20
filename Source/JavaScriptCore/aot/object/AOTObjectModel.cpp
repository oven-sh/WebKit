/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObjectModel.h"

#if ENABLE(AOT)

#include "AOTObjectModelCaches.h"
#include "AOTState.h"
#include "JSCJSValueInlines.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

ObjectModel::ObjectModel(State& state)
    : layouts(state)
    , structures(state)
    , keyOrders(state)
    , closures(state)
    , classes(state)
    , caches(makeUnique<ObjectModelCaches>(state))
{
}

ObjectModel::~ObjectModel() = default;

ObjectModelCaches::ObjectModelCaches(State& state)
    : movePlans(state)
    , assignmentPlans(state)
    , propertyLists(state)
    , classStructures(state)
    , classHierarchy(state)
    , memberGuardWatchpoints(state)
{
}

ObjectModelCaches::~ObjectModelCaches() = default;

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

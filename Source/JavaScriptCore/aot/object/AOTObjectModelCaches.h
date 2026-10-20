/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTClassHierarchy.h"
#include "AOTClassStructureCache.h"
#include "AOTFieldMovePlanCache.h"
#include "AOTMemberGuards.h"
#include "AOTPropertyListCache.h"
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC {

class CustomGetterSetter;
class Structure;

namespace AOT {

struct State;

struct DirectEvalStructures {
    Structure* bindings { nullptr };
    Structure* scope { nullptr };
    Structure* variableScope { nullptr };
};

struct ObjectModelCaches {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ObjectModelCaches);
    WTF_MAKE_NONCOPYABLE(ObjectModelCaches);

    explicit ObjectModelCaches(State&);
    ~ObjectModelCaches();

    FieldMovePlanCache movePlans;
    AssignmentPlanCache assignmentPlans;
    PropertyListCache propertyLists;
    ClassStructureCache classStructures;
    ClassHierarchy classHierarchy;
    MemberGuardWatchpoints memberGuardWatchpoints;
    CustomGetterSetter* staticFieldAccessor { nullptr };
    DirectEvalStructures directEvalStructures;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

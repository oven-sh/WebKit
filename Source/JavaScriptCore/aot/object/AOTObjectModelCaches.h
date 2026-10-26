/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTClassStructureCache.h"
#include "AOTFieldMoves.h"
#include "AOTMemberGuards.h"
#include "AOTPropertyListCache.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "CollectionScope.h"
#include <array>
#include <memory>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/UniqueArray.h>

namespace JSC {

class CustomGetterSetter;
class Structure;
class VM;

namespace AOT {

class DirectEvalCaches;
struct State;

namespace Interpreter {
class ObjectLiteralPlans;
}

struct DirectEvalStructures {
    Structure* bindings { nullptr };
    Structure* scope { nullptr };
    Structure* variableScope { nullptr };
};

// How the getters of an object literal came to lie in the slots of one new object.
struct GetterDefinitionPlan {
    struct Entry {
        UniquedStringImpl* name;
        const FieldData* field;
        FunctionID getter;
    };

    const LayoutData* layout { nullptr };
    uint32_t structureBits { 0 };
    uint32_t orderWordBefore { 0 };
    uint32_t orderWordAfter { 0 };
    uint32_t enumerable { 0 };
    Vector<Entry> entries;
};

struct ObjectModelCaches {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ObjectModelCaches);
    WTF_MAKE_NONCOPYABLE(ObjectModelCaches);

    explicit ObjectModelCaches(State&);
    ~ObjectModelCaches();

    static constexpr unsigned numberOfGetterDefinitionPlans = 64;

    FieldMovePlanCache movePlans;
    std::array<GetterDefinitionPlan, numberOfGetterDefinitionPlans> getterDefinitionPlans;
    AssignmentPlanCache assignmentPlans;
    PropertyListCache propertyLists;
    ClassStructureCache classStructures;
    MemberGuardWatchpoints memberGuardWatchpoints;
    CustomGetterSetter* staticFieldAccessor { nullptr };
    DirectEvalStructures directEvalStructures;
    DirectEvalCaches* directEvalCaches { nullptr };
    std::unique_ptr<Interpreter::ObjectLiteralPlans> objectLiteralPlans;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

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
    Structure* derivedConstructorStandIn { nullptr };
};

// How the getters and setters of an object literal came to lie in the slots of one new object.
struct AccessorDefinitionPlan {
    static constexpr uint8_t noDefinition = UINT8_MAX;

    struct Definition {
        UniquedStringImpl* name;
        FunctionID function;
    };
    // `getter` and `setter` index the definitions. The half that none of them gives is that of the accessor in the slot.
    struct FilledSlot {
        const FieldData* field;
        uint8_t getter;
        uint8_t setter;
        bool isFilledBefore;
    };

    const LayoutData* layout { nullptr };
    uint32_t structureBits { 0 };
    uint32_t orderWordBefore { 0 };
    uint32_t orderWordAfter { 0 };
    uint32_t getters { 0 };
    uint32_t enumerable { 0 };
    Vector<Definition> definitions;
    Vector<FilledSlot> slots;
    // Definition i is the getter of slot i, which is empty before.
    bool definesGettersOfEmptySlots { false };
};

// The fields of a layout that ToPropertyDescriptor reads: enumerable, configurable, value, writable, get, set. Null where the layout has no such field.
struct DescriptorFields {
    const LayoutData* layout { nullptr };
    std::array<const FieldData*, 6> fields { };
};

struct ObjectModelCaches {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ObjectModelCaches);
    WTF_MAKE_NONCOPYABLE(ObjectModelCaches);

    explicit ObjectModelCaches(State&);
    ~ObjectModelCaches();

    static constexpr unsigned numberOfAccessorDefinitionPlans = 64;
    static constexpr unsigned numberOfDescriptorFields = 16;

    FieldMovePlanCache movePlans;
    std::array<AccessorDefinitionPlan, numberOfAccessorDefinitionPlans> accessorDefinitionPlans;
    std::array<DescriptorFields, numberOfDescriptorFields> descriptorFields;
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

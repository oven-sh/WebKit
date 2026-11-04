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

// Where the own enumerable properties lie in the objects with one Structure, one order word and one Structure of the second object, some of them getters: in the order in which a copy reads them.
struct GetterSourceList {
    struct Property {
        // The program holds the name of a field, the Structure of the second object every other.
        UniquedStringImpl* name;
        const FieldData* field;
        PropertyOffset offsetInExtraProperties;
        // In the cell of the copy. None for a property that is not enumerable: it is listed because a getter may change that.
        PropertyOffset offsetInCopy;
        bool isGetter;
    };

    bool isFor(StructureID ofObject, uint32_t word, StructureID ofExtraProperties) const { return structure == ofObject && orderWord == word && extraPropertiesStructure == ofExtraProperties; }

    StructureID structure;
    uint32_t orderWord { 0 };
    StructureID extraPropertiesStructure;
    // Of an ordinary object that was given the enumerable names in this order, all in its cell. Null for objects that are not copied by the list.
    Structure* structureOfCopy { nullptr };
    Vector<Property, 8> properties;
};

class GetterSourceLists {
    WTF_MAKE_NONCOPYABLE(GetterSourceLists);
public:
    explicit GetterSourceLists(State&);

    static constexpr unsigned numberOfLists = 16;
    GetterSourceList& placeFor(StructureID structure, uint32_t orderWord) { return m_lists[(structure.bits() >> 4 ^ orderWord) % numberOfLists]; }
    // The collector may run in a thread of its own, so nothing is released here.
    void clear()
    {
        for (GetterSourceList& list : m_lists) {
            list.structure = StructureID();
            list.structureOfCopy = nullptr;
        }
    }

private:
    std::array<GetterSourceList, numberOfLists> m_lists;
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
    GetterSourceLists getterSources;
    ClassStructureCache classStructures;
    MemberGuardWatchpoints memberGuardWatchpoints;
    CustomGetterSetter* staticFieldAccessor { nullptr };
    DirectEvalStructures directEvalStructures;
    DirectEvalCaches* directEvalCaches { nullptr };
    std::unique_ptr<Interpreter::ObjectLiteralPlans> objectLiteralPlans;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

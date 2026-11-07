/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAssignmentPlans.h"
#include "AOTClassStructureCache.h"
#include "AOTFieldMoves.h"
#include "AOTPropertyListCache.h"
#include "AOTVector.h"
#include "StructureID.h"
#include <array>
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC {

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

struct AccessorDefinitionPlan {
    static constexpr uint8_t noDefinition = UINT8_MAX;

    struct FilledSlot {
        const FieldData* field;
        uint8_t getter;
        uint8_t setter;
        bool isFilledBefore;
    };

    Vector<FilledSlot> slots;
};

struct DescriptorFields {
    const LayoutData* layout { nullptr };
    std::array<const FieldData*, 6> fields { };
};

struct GetterSourceList {
    struct Property {
        UniquedStringImpl* name;
        const FieldData* field;
        PropertyOffset offsetInExtraProperties;
        PropertyOffset offsetInCopy;
        bool isGetter;
    };

    bool isFor(StructureID ofObject, uint32_t word, StructureID ofExtraProperties) const { return structure == ofObject && orderWord == word && extraPropertiesStructure == ofExtraProperties; }

    StructureID structure;
    uint32_t orderWord { 0 };
    StructureID extraPropertiesStructure;
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

    static constexpr unsigned numberOfDescriptorFields = 16;

    FieldMovePlanCache movePlans;
    std::array<DescriptorFields, numberOfDescriptorFields> descriptorFields;
    AssignmentPlanCache assignmentPlans;
    PropertyListCache propertyLists;
    GetterSourceLists getterSources;
    ClassStructureCache classStructures;
    DirectEvalStructures directEvalStructures;
    DirectEvalCaches* directEvalCaches { nullptr };
    std::unique_ptr<Interpreter::ObjectLiteralPlans> objectLiteralPlans;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

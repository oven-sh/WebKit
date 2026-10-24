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

// Where the members that a validator asks for lie in the plain data objects of one Structure, and the order of their keys as an object of the layout records it.
// A row of 32-bit words, which compiled code reads. A place is the distance in bytes from the cell where it is positive and from the butterfly where it is negative.
struct SourcePlan {
    static constexpr unsigned isUsableWord = 0;
    static constexpr unsigned orderWord = 1;
    static constexpr unsigned firstPlaceWord = 2;
    static constexpr int32_t absent = 0;

    static constexpr size_t numberOfWords(uint32_t numberOfMembers) { return firstPlaceWord + numberOfMembers; }
};

class SourcePlans {
    WTF_MAKE_NONCOPYABLE(SourcePlans);
public:
    using Words = UniqueArray<uint32_t>;

    explicit SourcePlans(State&);
    ~SourcePlans();

    static Words createUnusable(uint32_t numberOfMembers);

    // A plan stays where it is, unchanged, for as long as its Structure lives.
    const uint32_t* find(Structure* structure, uint32_t request)
    {
        if (m_lastStructure == structure && m_lastRequest == request)
            return m_lastPlan;
        return findInTable(structure, request);
    }
    const uint32_t* add(Structure*, uint32_t request, uint32_t numberOfMembers, Words&&);
    const uint32_t* planThatNeverApplies(uint32_t numberOfMembers);
    size_t removeDeadEntries(VM&, CollectionScope);

private:
    AOT_CLASS_THRESHOLD(unsigned, maxNumberOfPlans, 4096);

    const uint32_t* findInTable(Structure*, uint32_t request);

    UncheckedKeyHashMap<std::pair<Structure*, uint32_t>, Words> m_plans;
    Vector<Words, 1> m_plansThatNeverApply;
    uint32_t m_membersOfPlanThatNeverApplies { 0 };
    Structure* m_lastStructure { nullptr };
    uint32_t m_lastRequest { 0 };
    const uint32_t* m_lastPlan { nullptr };
};

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
    SourcePlans sourcePlans;
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

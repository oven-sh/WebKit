/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowPropertyLocations.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class LayoutPlacement : uint8_t { FollowsType, FollowsValidationPlan };

class TypedResultBuilder {
    WTF_MAKE_NONCOPYABLE(TypedResultBuilder);
public:
    TypedResultBuilder(FlowSolver&, FlowStatementSite, ValueID origin, std::span<const LayoutID> allocatedLayouts, FlowNodeID uncopiedValues = { });

    void allocateLayoutsAmongPlainData(TypeID root, LayoutPlacement);
    FlowValue valueOf(TypeID);
    void drain();

private:
    struct Pending {
        FlowObjectID object;
        LayoutID layout;
        TypeID type;
        BuiltinClassID builtinClass;
    };

    HashSet<LayoutID> layoutsNamedBy(TypeID root) const;
    FlowValue plainDataValue();
    FlowObjectID objectFor(LayoutID);
    FlowObjectID instanceFor(TypeID, BuiltinClassID);

    void fill(const Pending&);
    void fillPosition(FlowNodeID, TypeID);
    void fillPlainDataWithUncopiedValues();
    void fillLayoutObject(FlowObjectID, const Layout&);
    void fillInheritedFields(FlowObjectID, const Layout&);
    void fillUndeclaredMembers(FlowObjectID);
    void fillField(FlowNodeID, const Field&);
    void fillInstance(FlowObjectID, const ClassPart&);
    bool mayBeMissing(const Field&) const;

    FlowSolver& m_solver;
    FlowStatementSite m_site;
    ValueID m_origin;
    Witness m_witness;
    std::span<const LayoutID> m_allocatedLayouts;
    FlowNodeID m_uncopiedValues;
    HashMap<LayoutID, uint32_t> m_ordinalOfLayout;
    Vector<Pending, 16> m_pending;
    std::optional<FlowPlainDataObjects> m_plainData;
    FlowObjectSet m_objectsAmongPlainData;
};

TypedResultBuilder::TypedResultBuilder(FlowSolver& solver, FlowStatementSite site, ValueID origin, std::span<const LayoutID> allocatedLayouts, FlowNodeID uncopiedValues)
    : m_solver(solver)
    , m_site(site)
    , m_origin(origin)
    , m_witness(solver.witnessAt(site, origin))
    , m_allocatedLayouts(allocatedLayouts)
    , m_uncopiedValues(uncopiedValues)
{
    for (size_t ordinal = 0; ordinal < allocatedLayouts.size(); ++ordinal)
        m_ordinalOfLayout.add(allocatedLayouts[ordinal], static_cast<uint32_t>(ordinal));
}

// Follows the types that valueOf() and fill() follow.
HashSet<LayoutID> TypedResultBuilder::layoutsNamedBy(TypeID root) const
{
    const Program& program = m_solver.program();
    const BuiltinTable& builtins = m_solver.builtins();
    HashSet<LayoutID> namedLayouts;
    HashSet<TypeID> visitedTypes;
    Vector<TypeID, 16> worklist;
    auto visit = [&](TypeID type) {
        if (type && visitedTypes.add(type).isNewEntry)
            worklist.append(type);
    };
    auto visitFieldsOf = [&](const Layout& layout) {
        for (const Field& field : layout.fields)
            visit(expectationOf(field.declared));

        HashSet<LayoutID> visitedBases;
        for (LayoutID base = predictedHeritageOf(layout); base && visitedBases.add(base).isNewEntry; base = predictedHeritageOf(program.at(base))) {
            for (const Field& field : program.at(base).fields) {
                if (field.key && !layout.findField(field.key))
                    visit(expectationOf(field.declared));
            }
        }
    };

    visit(root);
    while (!worklist.isEmpty()) {
        const Type& type = program.at(worklist.takeLast());
        for (LayoutID layout : membersOf(program, type)) {
            if (m_ordinalOfLayout.contains(layout) && namedLayouts.add(layout).isNewEntry)
                visitFieldsOf(program.at(layout));
        }
        for (const ClassPart& part : type.classes) {
            for (unsigned index = 0, count = builtins.numberOfSlotsInType(part.builtinClass); index < count; ++index)
                visit(part.contentsAt(index));
        }
    }
    return namedLayouts;
}

// The code that creates the result allocates every listed layout, also where the type is wider than what was validated. Such an object may stand wherever plain data may.
// A validator allocates a layout wherever a node of its plan names it: as the value of a dictionary, an undeclared member or a member without a field too.
void TypedResultBuilder::allocateLayoutsAmongPlainData(TypeID root, LayoutPlacement placement)
{
    HashSet<LayoutID> layoutsOnlyWhereTypeNamesThem;
    if (placement == LayoutPlacement::FollowsType)
        layoutsOnlyWhereTypeNamesThem = layoutsNamedBy(root);
    for (LayoutID layout : m_allocatedLayouts) {
        if (!layoutsOnlyWhereTypeNamesThem.contains(layout))
            m_objectsAmongPlainData.add(objectFor(layout));
    }
    if (m_objectsAmongPlainData.isEmpty())
        return;

    plainDataValue();
    if (m_plainData->object == unknownFlowObject) {
        m_solver.joinObjects(m_solver.graph().nativeCode(), m_objectsAmongPlainData, m_witness);
        return;
    }
    for (FlowOpenPart part : { FlowOpenPart::IndexedProperties, FlowOpenPart::StringProperties })
        m_solver.joinObjects(m_solver.ensureOpenPart(m_plainData->object, part), m_objectsAmongPlainData, m_witness);
    m_solver.joinObjects(contentsNode(m_solver.graph(), m_plainData->array, ContentsSlot::Element), m_objectsAmongPlainData, m_witness);
}

FlowValue TypedResultBuilder::valueOf(TypeID typeID)
{
    FlowValueTable& values = m_solver.values();
    if (!typeID) {
        FlowValue value = values.unknownValue().withoutObjects();
        value.objects = plainDataValue().objects;
        return value;
    }

    const Program& program = m_solver.program();
    const Type& type = program.at(typeID);
    FlowValue value = values.valueOfType(type).withoutObjects();
    for (LayoutID layout : membersOf(program, type)) {
        if (FlowObjectID object = objectFor(layout))
            value.objects.add(object);
    }
    for (const ClassPart& part : type.classes)
        value.objects.add(instanceFor(typeID, part.builtinClass));
    if (type.hasObjects())
        value.objects.unite(values.filter(plainDataValue(), type.withoutProofs()).objects);
    return value;
}

FlowValue TypedResultBuilder::plainDataValue()
{
    if (!m_plainData) {
        m_plainData = allocatePlainData(m_solver, m_site, m_origin);
        fillPlainDataWithUncopiedValues();
    }

    FlowValue value;
    value.objects.add(m_plainData->object);
    value.objects.add(m_plainData->array);
    value.objects.unite(m_objectsAmongPlainData);
    return value;
}

// A copy that does not fit its layout is an ordinary object with the members of its source.
void TypedResultBuilder::fillPlainDataWithUncopiedValues()
{
    if (!m_uncopiedValues || m_plainData->object == unknownFlowObject)
        return;
    for (FlowOpenPart part : { FlowOpenPart::IndexedProperties, FlowOpenPart::StringProperties })
        m_solver.connect(m_uncopiedValues, m_solver.ensureOpenPart(m_plainData->object, part), { }, m_witness);
    m_solver.connect(m_uncopiedValues, contentsNode(m_solver.graph(), m_plainData->array, ContentsSlot::Element), { }, m_witness);
}

// A validator copies the objects and arrays that its plan describes. Where the plan takes any object or any function, the copy holds the very value its source held.
void TypedResultBuilder::fillPosition(FlowNodeID position, TypeID typeID)
{
    m_solver.join(position, valueOf(typeID), m_witness);
    if (!typeID || m_solver.program().at(typeID).anyObject || m_solver.program().at(typeID).anyFunction)
        m_solver.connect(m_uncopiedValues, position, typeID, m_witness);
}

FlowObjectID TypedResultBuilder::objectFor(LayoutID layout)
{
    auto ordinal = m_ordinalOfLayout.find(layout);
    if (ordinal == m_ordinalOfLayout.end())
        return { };

    FlowSiteDescription description;
    description.kind = FlowSiteKind::Layout;
    description.origin = FlowSiteOrigin::TypedResult;
    description.value = m_origin;
    description.layout = layout;
    description.ordinal = ordinal->value;

    FlowSolveAllocation allocation = ensureSolveAllocation(m_solver, m_site.instance, description);
    if (allocation.isNew)
        m_pending.append(Pending { allocation.object, layout, { }, { } });
    return allocation.object;
}

FlowObjectID TypedResultBuilder::instanceFor(TypeID typeID, BuiltinClassID builtinClass)
{
    FlowSiteDescription description;
    description.kind = FlowSiteKind::Instance;
    description.origin = FlowSiteOrigin::TypedResult;
    description.value = m_origin;
    description.builtinClass = builtinClass;
    description.ordinal = typeID.index();

    FlowSolveAllocation allocation = ensureSolveAllocation(m_solver, m_site.instance, description);
    if (allocation.isNew)
        m_pending.append(Pending { allocation.object, { }, typeID, builtinClass });
    return allocation.object;
}

void TypedResultBuilder::drain()
{
    while (!m_pending.isEmpty())
        fill(m_pending.takeLast());
}

void TypedResultBuilder::fill(const Pending& pending)
{
    const Program& program = m_solver.program();
    if (pending.layout) {
        fillLayoutObject(pending.object, program.at(pending.layout));
        return;
    }

    const ClassPart* part = program.at(pending.type).findClassPart(pending.builtinClass);
    RELEASE_ASSERT(part);
    fillInstance(pending.object, *part);
}

void TypedResultBuilder::fillLayoutObject(FlowObjectID object, const Layout& layout)
{
    m_solver.charge(m_site, layout.fields.size() + 1);

    uint32_t numberOfFields = m_solver.objects().at(object).numberOfDeclaredParts;
    Vector<uint32_t, 16> allFields;
    for (uint32_t index = 0; index < numberOfFields; ++index) {
        fillField(m_solver.graph().field(object, index), layout.fields[index]);
        allFields.append(index);
    }
    fillInheritedFields(object, layout);
    fillUndeclaredMembers(object);

    noteAllocation(m_solver, FlowAllocatedObject { m_site, m_origin, object, false, false }, allFields.span());
    m_solver.noteUse(object, FlowObjectUse::ReachesTypedParse);
}

void TypedResultBuilder::fillInheritedFields(FlowObjectID object, const Layout& layout)
{
    const Program& program = m_solver.program();
    HashSet<LayoutID> visited;
    for (LayoutID baseID = predictedHeritageOf(layout); baseID && visited.add(baseID).isNewEntry; baseID = predictedHeritageOf(program.at(baseID))) {
        const Layout& base = program.at(baseID);
        m_solver.charge(m_site, base.fields.size() + 1);
        for (const Field& field : base.fields) {
            if (field.key && !layout.findField(field.key))
                fillField(m_solver.ensurePropertyValue(object, field.key), field);
        }
    }
}

void TypedResultBuilder::fillUndeclaredMembers(FlowObjectID object)
{
    for (FlowOpenPart part : { FlowOpenPart::IndexedProperties, FlowOpenPart::StringProperties })
        fillPosition(m_solver.ensureOpenPart(object, part), TypeID());
    m_solver.noteEvent(object, FlowObjectEvent::StoredByUnknownKey, m_witness);
    m_solver.noteMutation(object, ObjectMutation::AddProperty, m_witness);
}

void TypedResultBuilder::fillField(FlowNodeID node, const Field& field)
{
    fillPosition(node, expectationOf(field.declared));
    m_solver.noteLocationEvent(node, { FlowLocationEvent::InitializedAtAllocation, FlowLocationEvent::StoredOtherValue });
    if (!mayBeMissing(field))
        return;
    m_solver.joinPrimitives(node, Primitive::Undefined, m_witness);
    m_solver.noteLocationEvent(node, FlowLocationEvent::MissingAtAllocation);
}

bool TypedResultBuilder::mayBeMissing(const Field& field) const
{
    TypeID type = field.declared.type;
    return !type || m_solver.program().at(type).primitives & Primitive::Undefined;
}

void TypedResultBuilder::fillInstance(FlowObjectID object, const ClassPart& part)
{
    const BuiltinTable& builtins = m_solver.builtins();
    for (unsigned index = 0, count = builtins.numberOfSlotsInType(part.builtinClass); index < count; ++index) {
        FlowNodeID node = m_solver.graph().slot(object, builtins.slotInType(part.builtinClass, index));
        if (!node)
            continue;
        fillPosition(node, part.contentsAt(index));
        m_solver.noteLocationEvent(node, overwrittenLocationEvents);
    }
    m_solver.noteEvent(object, FlowObjectEvent::Allocated, m_witness);
}

} // namespace

void allocateTypedResult(FlowSolver& solver, FlowStatementSite site, ValueID origin, TypeID type, std::span<const LayoutID> layouts, FlowNodeID result)
{
    TypedResultBuilder builder(solver, site, origin, layouts);
    builder.allocateLayoutsAmongPlainData(type, LayoutPlacement::FollowsType);
    FlowValue root = builder.valueOf(type);
    builder.drain();
    solver.join(result, root, solver.witnessAt(site, origin));
}

void allocateTypedResult(FlowSolver& solver, FlowStatementSite site, ValueID origin, ExpectedResultID expectedResult, FlowNodeID uncopiedValues, FlowNodeID result)
{
    const ExpectedResult& expected = solver.program().at(expectedResult);
    TypedResultBuilder builder(solver, site, origin, expected.layouts.span(), uncopiedValues);
    builder.allocateLayoutsAmongPlainData(expected.type, expected.validation.isEmpty() ? LayoutPlacement::FollowsType : LayoutPlacement::FollowsValidationPlan);
    FlowValue root = builder.valueOf(expected.type);
    builder.drain();
    solver.join(result, root, solver.witnessAt(site, origin));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

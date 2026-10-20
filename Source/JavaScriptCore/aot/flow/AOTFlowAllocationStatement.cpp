/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowObjectsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowCallOperations.h"
#include "AOTFlowPropertyOperations.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using FieldIndexes = Vector<uint32_t, 8>;

static const Value& allocatingOperation(const FlowSolver& solver LIFETIME_BOUND, FlowStatementSite site, const FlowAllocation& allocation)
{
    RELEASE_ASSERT(allocation.origin && !solver.graph().isHost(site.instance));
    return solver.operationAt(site, allocation.origin);
}

static void markFieldMissing(FlowSolver& solver, LayoutKind kind, FlowNodeID field)
{
    solver.noteLocationEvent(field, FlowLocationEvent::MissingAtAllocation);
    if (!isEnvironmentKind(kind))
        solver.joinPrimitives(field, Primitive::Undefined);
}

static FieldIndexes fieldsOfDataNode(FlowSolver& solver, DataID data, const FlowSiteDescription& description)
{
    std::span<const DataNode* const> nodes = dataNodesInPreorder(solver.objectState(), solver.program(), data);
    if (description.ordinal >= nodes.size())
        return { };
    const DataNode::Object* object = nodes[description.ordinal]->tryAs<DataNode::Object>();
    if (!object)
        return { };

    const Layout& layout = solver.program().at(description.layout);
    FieldIndexes fields;
    for (const DataMember& member : object->members) {
        if (std::optional<uint32_t> field = layout.findField(member.key)) {
            fields.append(*field);
            continue;
        }
        // Own keys other than array indexes are listed in creation order, so the members behind such a key are defined after the allocation.
        if (classOf(solver.program(), member.key) != FlowKeyClass::Index)
            break;
    }
    return fields;
}

static FieldIndexes initialFieldsOf(FlowSolver& solver, FlowStatementSite site, const FlowAllocation& allocation, const FlowSiteDescription& description)
{
    if (description.kind != FlowSiteKind::Layout)
        return { };

    switch (description.origin) {
    case FlowSiteOrigin::Operation: {
        const Value& operation = allocatingOperation(solver, site, allocation);
        if (Value::opcodesWithFields.contains(operation.opcode))
            return FieldIndexes(operation.fields());
        const Layout& layout = solver.program().at(description.layout);
        return layout.builtinLayout ? requiredFieldsOf(layout) : FieldIndexes();
    }
    case FlowSiteOrigin::DataNode:
        return fieldsOfDataNode(solver, allocatingOperation(solver, site, allocation).data(), description);
    case FlowSiteOrigin::CoroutineResult:
    case FlowSiteOrigin::RestParameter:
    case FlowSiteOrigin::BuiltinResult:
    case FlowSiteOrigin::BuiltinElement:
    case FlowSiteOrigin::BuiltinConstructor:
    case FlowSiteOrigin::TypedResult:
    case FlowSiteOrigin::RuntimeError:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void recordInitialValues(FlowSolver& solver, FlowStatementSite site, const FlowAllocation& allocation, FlowObjectID object, std::span<const uint32_t> fields)
{
    if (fields.empty())
        return;

    const Value& operation = allocatingOperation(solver, site, allocation);
    if (operation.opcode != Opcode::NewObject) {
        for (uint32_t field : fields)
            solver.noteLocationEvent(solver.graph().field(object, field), FlowLocationEvent::StoredOtherValue);
        return;
    }

    const FlowSummary& summary = solver.summaryOf(site);
    std::span<const ValueID> values = operation.variadicOperands();
    RELEASE_ASSERT(values.size() == fields.size());
    for (size_t index = 0; index < fields.size(); ++index) {
        FlowLocalID local = summary.localOfValue[values[index].index()];
        FlowLocationRecord& record = solver.records().ensureLocationRecord(solver.graph().field(object, fields[index]));
        recordStoredValue(record, local ? summary.closureCreationAt(local) : nullptr);
    }
}

static void noteFieldsAtAllocation(FlowSolver& solver, const FlowAllocatedObject& allocated, const FlowObject& record, std::span<const uint32_t> initialFields)
{
    // No finished object has the keys of such an allocation alone. The results list what its pieces lead to.
    if (!allocated.isCompletedByPieces)
        solver.records().addInitialFieldList(allocated.object, initialFields);

    for (uint32_t field : initialFields)
        solver.noteLocationEvent(solver.graph().field(allocated.object, field), FlowLocationEvent::InitializedAtAllocation);
    if (allocated.isByConstructor)
        return;

    BitVector listed(record.numberOfDeclaredParts);
    for (uint32_t field : initialFields)
        listed.set(field);

    LayoutKind kind = solver.program().at(record.layout).kind;
    for (uint32_t field = 0; field < record.numberOfDeclaredParts; ++field) {
        if (!listed.get(field))
            markFieldMissing(solver, kind, solver.graph().field(allocated.object, field));
    }
}

void noteAllocation(FlowSolver& solver, const FlowAllocatedObject& allocated, std::span<const uint32_t> initialFields)
{
    if (allocated.object == unknownFlowObject)
        return;

    Witness witness = solver.witnessAt(allocated.site, allocated.origin);
    FlowObject record = solver.objects().at(allocated.object);
    solver.noteEvent(allocated.object, FlowObjectEvent::Allocated, witness);
    if (allocated.isInCycle)
        solver.noteEvent(allocated.object, FlowObjectEvent::AllocatedRepeatedly, witness);

    switch (record.kind) {
    case FlowObjectKind::Closure:
        for (uint32_t index = 0; index < record.numberOfDeclaredParts; ++index)
            solver.noteLocationEvent(solver.graph().capture(allocated.object, index), FlowLocationEvent::InitializedAtAllocation);
        return;
    case FlowObjectKind::Instance:
    case FlowObjectKind::Opaque:
        for (uint32_t index = 0; index < record.numberOfPositions; ++index)
            solver.noteLocationEvent(solver.graph().position(allocated.object, index), FlowLocationEvent::InitializedAtAllocation);
        noteInstanceOf(solver, record.builtinClass, witness);
        return;
    case FlowObjectKind::Allocation:
        if (allocated.isByConstructor)
            solver.noteEvent(allocated.object, FlowObjectEvent::AllocatedByConstructor, witness);
        noteFieldsAtAllocation(solver, allocated, record, initialFields);
        return;
    case FlowObjectKind::Unknown:
    case FlowObjectKind::Prototype:
    case FlowObjectKind::ModuleNamespace:
    case FlowObjectKind::Builtin:
    case FlowObjectKind::ModuleBuiltin:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LayoutID selectAllocatedLayout(const Program& program, const FlowObject& prototype, const FlowSiteDescription& description)
{
    if (prototype.kind != FlowObjectKind::Prototype || !prototype.layout)
        return description.layout;
    // Only the definition of a class ties an object to a layout. The prototype property of a constructor function can be stored to, so the run time knows none of the objects it holds.
    if (program.at(prototype.layout).has(LayoutFlags::IsConstructorFunction))
        return description.layout;
    // An object of a built-in class has the layout of a class only where that class may extend the built-in class.
    if (description.kind == FlowSiteKind::Instance && !program.at(prototype.layout).has(LayoutFlags::HasHeritage))
        return description.layout;
    return prototype.layout;
}

static bool isPrototypeObjectOf(const FlowObject& object, LayoutID layout)
{
    return object.kind == FlowObjectKind::Prototype && object.layout == layout;
}

static FlowObjectID allocateDerivedObject(FlowSolver& solver, const FlowAllocatedObject& own, const FlowSiteDescription& description, LayoutID derivedLayout, std::span<const uint32_t> initialFields)
{
    FlowSiteDescription derived = description;
    // An operation that makes an object of a built-in class with the prototype of a compiled class does the work of the constructor of the built-in class.
    if (description.kind == FlowSiteKind::Instance)
        derived.origin = FlowSiteOrigin::BuiltinConstructor;
    derived.kind = FlowSiteKind::Layout;
    derived.numberOfPositions = 0;
    derived.layout = derivedLayout;
    FlowObjectID object = ensureSolveAllocation(solver, own.site.instance, derived).object;
    if (object == unknownFlowObject)
        return object;
    // The object that stands for every site of a layout does not say who made it.
    if (derived.origin == FlowSiteOrigin::BuiltinConstructor && solver.objects().at(object).standsForLayout)
        return unknownFlowObject;

    noteAllocation(solver, FlowAllocatedObject { own.site, own.origin, object, own.isInCycle, true }, { });
    if (initialFields.empty())
        return object;

    const Layout& constructorLayout = solver.program().at(description.layout);
    for (uint32_t field : initialFields) {
        PropertyKeyID key = constructorLayout.fields[field].key;
        if (!key)
            continue;
        FlowNodeID inherited = solver.ensurePropertyValue(object, key);
        solver.connect(solver.graph().field(own.object, field), inherited);
        solver.noteLocationEvent(inherited, { FlowLocationEvent::InitializedAtAllocation, FlowLocationEvent::StoredOtherValue });
    }
    return object;
}

static void allocateForPrototypes(FlowSolver& solver, const FlowAllocatedObject& own, const FlowAllocation& allocation, const FlowSiteDescription& description, std::span<const uint32_t> initialFields)
{
    FlowNodeID prototypes = solver.node(own.site, allocation.prototype);
    FlowNodeID result = solver.graph().nodeOf(own.site.instance, allocation.origin);
    Witness witness = solver.witnessAt(own.site, own.origin);

    FlowNewObjects fresh = solver.takeNewObjects(objectTemporaryKey(own.site, FlowObjectTemporaryRole::AllocationPrototypes), prototypes);
    solver.forEachMember(fresh, [&](FlowObjectID prototype) {
        LayoutID selected = selectAllocatedLayout(solver.program(), solver.objects().at(prototype), description);
        // Beside the unknown object a prototype that has escaped is a prediction, and the unknown object among the results stands for what is made with it.
        if (selected != description.layout) {
            if (!solver.mayRelyOn(FlowAbsence::UnknownOperand, own.site))
                return;
            if (solver.hasReachedNativeCode(prototype) && solver.valueOf(prototypes).hasUnknownObject())
                return;
        }

        FlowObjectID target = own.object;
        if (selected == description.layout) {
            noteAllocation(solver, own, initialFields);
            recordInitialValues(solver, own.site, allocation, own.object, initialFields);
            if (own.isByConstructor && !isPrototypeObjectOf(solver.objects().at(prototype), description.layout))
                solver.noteMutation(own.object, ObjectMutation::ChangePrototype, witness);
        } else {
            target = allocateDerivedObject(solver, own, description, selected, initialFields);
            if (target == unknownFlowObject) {
                // The unknown object stands only for what has reached native code, and the new object inherits from the prototype.
                solver.joinObject(solver.graph().nativeCode(), prototype, witness);
                solver.noteMutation(prototype, ObjectMutation::BecomePrototype, witness);
                solver.joinUnknownObject(result, witness);
                return;
            }
        }

        FlowNodeID link = solver.temporaryFor(objectTemporaryKey(own.site, FlowObjectTemporaryRole::PrototypeOfAllocatedObject, selected.index()));
        solver.joinObject(link, prototype, witness);
        connectCreationPrototype(solver, target, link);
        solver.joinObject(result, target, witness);
        if (prototype == unknownFlowObject)
            solver.joinUnknownObject(result, witness);
    });
}

void runAllocation(FlowSolver& solver, FlowStatementSite site, const FlowAllocation& allocation)
{
    FlowObjectID object = solver.graph().objectOf(site.instance, allocation.site);
    if (object == unknownFlowObject)
        return;

    FlowSiteDescription description = solver.summaryOf(site).sites[allocation.site];
    FieldIndexes initialFields = initialFieldsOf(solver, site, allocation, description);
    bool isLayout = description.kind == FlowSiteKind::Layout;
    bool isByConstructor = isLayout && solver.program().at(description.layout).constructor;
    bool selectsObjectByPrototype = isLayout || description.kind == FlowSiteKind::Instance;
    FlowAllocatedObject allocated { site, allocation.origin, object, allocation.isInCycle, isByConstructor, description.isCompletedByPieces };

    if (selectsObjectByPrototype && allocation.prototype) {
        allocateForPrototypes(solver, allocated, allocation, description, initialFields.span());
        return;
    }

    noteAllocation(solver, allocated, initialFields.span());
    if (isLayout)
        recordInitialValues(solver, site, allocation, object, initialFields.span());
    if (allocation.prototype)
        connectCreationPrototype(solver, object, solver.node(site, allocation.prototype));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

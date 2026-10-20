/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowRecords.h"
#include "AOTThreads.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// No rule marks these: which prototype an object is created with and what becomes a prototype by a construction shows in the links alone, and nobody compares a stored value with the type of its field.
static constexpr OptionSet<ObjectMutation> mutationsWithoutMark {
    ObjectMutation::ChangePrototype,
    ObjectMutation::BecomePrototype,
    ObjectMutation::StoreOfOtherType,
};

// An object loses its shape to an accessor under an index, which is a redefinition, and to what code does that the build has not seen.
static constexpr OptionSet<ObjectMutation> mutationsThatMayTakeTheShape {
    ObjectMutation::RedefineProperty,
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::LoseShape,
};

// The others have their layout without an operation that says so.
static bool isAllocatedByOperations(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
    case LayoutKind::Environment:
        return true;
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct ObjectsOfLayouts {
    BitVector isAllocated;
    Vector<OptionSet<ObjectMutation>> possibleMutations;
};

} // anonymous namespace

// A construction hands the instance to the constructor and to whoever asked for it, so an instance that is ever made is in the value of some node.
static BitVector findLayoutsWithInstances(const ValueFlowResultInputs& inputs)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowGraph& graph = inputs.solver().graph();
    uint32_t numberOfLayouts = inputs.program.layouts.size();
    ValueSetMaker maker(inputs.bodies().sets());
    Vector<ValueFlowObjectID> everyInstance(numberOfLayouts, [&](size_t layout) {
        return universe.instanceOf(LayoutID(static_cast<uint32_t>(layout)));
    });
    ValueSetID instances = maker.of(everyInstance.span());

    ValueSetID found = ValueSets::nothing;
    HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> seenSets;
    for (uint32_t index = 0; index < graph.numberOfNodes(); ++index) {
        ValueSetID set = graph.valueOf(ValueFlowNodeID(index));
        if (seenSets.add(set.index()).isNewEntry && inputs.sets().meet(set, instances))
            found = maker.unite(found, maker.intersect(set, instances));
    }

    BitVector layouts(numberOfLayouts);
    inputs.sets().forEachMember(found, [&](ValueFlowObjectID instance) {
        layouts.quickSet(universe.layoutOf(instance).index());
    });
    return layouts;
}

OptionSet<ObjectMutation> markedMutationsOf(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    OptionSet<ObjectMutation> marked = inputs.solver().marksOf(object) & allObjectMutations;
    if (!inputs.hasEscaped(object))
        return marked;
    if (unseenCodeCanRun(inputs.solution))
        return allObjectMutations;
    return marked | (inputs.solver().marksOf(ValueFlowUniverse::unknownObject()) & allObjectMutations) | ObjectMutation::ReachNativeCode;
}

// The prototype that a constructor of a class hands to its allocation selects the layout of the class that was constructed, and the code of a built-in makes an ordinary object.
static bool hasLayoutOfSite(const Program& program, FunctionID function, const ValueFlowSite& site)
{
    if (site.origin != ValueFlowSiteOrigin::Operation || site.ordinal)
        return false;
    const Value& allocation = program.at(function).at(site.value);
    return allocation.opcode == Opcode::NewObject && (!allocation.prototypeOperand() || !program.prototypeMaySelectOtherClass(site.layout));
}

// An environment has no member of the universe, and what becomes of an object that unsolved code makes nobody has followed.
static void noteAllocatingOperations(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, const BitVector& layoutsWithInstances, ObjectsOfLayouts& objects)
{
    const Program& program = inputs.program;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        bool isUnsolved = isUnsolvedCode(inputs, tables, FunctionID(index));
        if (!isUnsolved && !inputs.isReached(FunctionID(index)))
            continue;
        for (const Value& value : program.functions[index].values) {
            if (value.opcode != Opcode::NewObject)
                continue;
            objects.isAllocated.quickSet(value.layout().index());
            if (isUnsolved)
                objects.possibleMutations[value.layout().index()] = allObjectMutations;
        }
    }
    // A pass turns a construction into an allocation and a call wherever the type of the callee names the constructor, also in code that the solve finds dead.
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        FunctionID constructor = program.layouts[index].constructor;
        if (!constructor)
            continue;
        bool isUnsolved = isUnsolvedCode(inputs, tables, constructor);
        if (!isUnsolved && !inputs.isReached(constructor))
            continue;
        objects.isAllocated.quickSet(index);
        // Whoever constructs a plain function without the solve seeing it, native code or an unknown callee, gets an instance that the solve knows as the unknown object.
        bool isConstructedUnseen = program.layouts[index].has(LayoutFlags::IsConstructorFunction) && (!layoutsWithInstances.quickGet(index) || inputs.hasEscaped(inputs.universe().closureOf(constructor)));
        if (isUnsolved || isConstructedUnseen)
            objects.possibleMutations[index] = allObjectMutations;
    }
}

static ObjectsOfLayouts collectObjectsOfLayouts(const ValueFlowResultInputs& inputs, const FlowResultTables& tables)
{
    const ValueFlowUniverse& universe = inputs.universe();
    BitVector layoutsWithInstances = findLayoutsWithInstances(inputs);

    ObjectsOfLayouts objects { BitVector(inputs.program.layouts.size()), Vector<OptionSet<ObjectMutation>>(inputs.program.layouts.size()) };
    noteAllocatingOperations(inputs, tables, layoutsWithInstances, objects);
    for (uint32_t index = 0; index < universe.numberOfObjects(); ++index) {
        ValueFlowObjectID object(index);
        ValueFlowObjectKind kind = universe.kindOf(object);
        if (kind != ValueFlowObjectKind::Site && kind != ValueFlowObjectKind::Instance)
            continue;
        LayoutID layout = universe.layoutOf(object);
        if (!layout)
            continue;
        bool isAllocated = kind == ValueFlowObjectKind::Site ? inputs.isReached(universe.functionOf(object)) : layoutsWithInstances.quickGet(layout.index());
        if (!isAllocated)
            continue;
        objects.isAllocated.quickSet(layout.index());
        OptionSet<ObjectMutation> possible = markedMutationsOf(inputs, object) | mutationsWithoutMark;
        if (possible.containsAny(mutationsThatMayTakeTheShape) || (kind == ValueFlowObjectKind::Site && !hasLayoutOfSite(inputs.program, universe.functionOf(object), *universe.siteOf(object))))
            possible.add(ObjectMutation::LoseShape);
        objects.possibleMutations[layout.index()].add(possible);
    }
    return objects;
}

// One class for each layout: what may happen to one object of the layout may happen to every one.
static Vector<AllocationClassID> createAllocationClasses(const ValueFlowResultInputs& inputs, const ObjectsOfLayouts& objects, FlowResultTables& tables, BitVector& layoutsThatKeepTheirShape)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    Vector<AllocationClassID> classOfLayout(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutFlow& flow = application.layouts[index].flow;
        LayoutKind kind = program.layouts[index].kind;
        if (!objects.isAllocated.quickGet(index)) {
            if (isAllocatedByOperations(kind) && inputs.build.isEnabled(FlowResultKind::ProvenLayoutSets, index))
                flow.allocation = AllocationState::NeverAllocated;
            continue;
        }
        if (!hasObjectsThatCodeAlters(kind) || !inputs.build.isEnabled(FlowResultKind::HolderMutations, index))
            continue;

        AllocationClass allocationClass;
        allocationClass.layout = LayoutID(index);
        allocationClass.impossibleMutations = allObjectMutations - objects.possibleMutations[index];
        // What a base leaves behind in an instance nobody has looked at.
        if (program.layouts[index].flags.contains(LayoutFlags::HasHeritage) || kind == LayoutKind::Container || !inputs.build.isEnabled(FlowResultKind::ProvenLayoutSets, index))
            allocationClass.impossibleMutations.remove(ObjectMutation::LoseShape);
        if (allocationClass.impossibleMutations.contains(ObjectMutation::LoseShape))
            layoutsThatKeepTheirShape.quickSet(index);
        classOfLayout[index] = AllocationClassID(tables.sites.allocationClasses.size());
        flow.allocationClasses = { classOfLayout[index] };
        flow.impossibleMutations = allocationClass.impossibleMutations;
        tables.sites.allocationClasses.append(WTF::move(allocationClass));
    }
    return classOfLayout;
}

// A closure is born with the prototype of its kind of function, so only a mark says that the link changes. A class defines static members on its constructor without a mark.
static void createClassesOfClosures(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    constexpr OptionSet<ObjectMutation> mutationsAddingKeys { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty, ObjectMutation::StoreAfterCreation };
    const Program& program = inputs.program;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!inputs.isReached(FunctionID(index)) || calls.functionsNamedByUnsolvedCode.get(index) || !inputs.build.isEnabled(FlowResultKind::HolderMutations, index))
            continue;
        OptionSet<ObjectMutation> marked = markedMutationsOf(inputs, inputs.universe().closureOf(FunctionID(index)));
        AllocationClass closures;
        closures.closureFunction = FunctionID(index);
        closures.impossibleMutations = allObjectMutations - marked - OptionSet<ObjectMutation> { ObjectMutation::BecomePrototype, ObjectMutation::StoreOfOtherType, ObjectMutation::LoseShape };
        closures.addedKeysAreComplete = !marked.containsAny(mutationsAddingKeys) && !program.functions[index].constructedClass && inputs.build.isEnabled(FlowResultKind::OwnNames, index);
        tables.sites.allocationClasses.append(WTF::move(closures));
    }
}

static void stampAllocations(const ValueFlowResultInputs& inputs, std::span<const AllocationClassID> classOfLayout, const BitVector& layoutsThatKeepTheirShape, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, tables, FunctionID(index)))
            continue;
        Vector<FlowAllocationStamp>& stamps = application.bodies[index].allocations;
        for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
            const Value& value = function.values[valueIndex];
            AllocationClassID allocationClass = value.opcode == Opcode::NewObject ? classOfLayout[value.layout().index()] : AllocationClassID();
            if (!allocationClass)
                continue;
            AllocationSiteID site(tables.sites.allocationSites.size());
            tables.sites.allocationSites.append(AllocationSite { FunctionID(index), value.layout(), { }, allocationClass, function.locationOf(ValueID(valueIndex)) });
            stamps.append(FlowAllocationStamp { .value = ValueID(valueIndex), .site = site, .allocationClass = allocationClass, .keepsShape = layoutsThatKeepTheirShape.quickGet(value.layout().index()) });
        }
    }
}

BitVector computeObjectResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    ObjectsOfLayouts objects = collectObjectsOfLayouts(inputs, tables);
    BitVector layoutsThatKeepTheirShape(inputs.program.layouts.size());
    Vector<AllocationClassID> classOfLayout = createAllocationClasses(inputs, objects, tables, layoutsThatKeepTheirShape);
    stampAllocations(inputs, classOfLayout.span(), layoutsThatKeepTheirShape, tables);
    createClassesOfClosures(inputs, calls, tables);
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size());
    inputs.build.statistics().count(StatisticID::FlowAllocationClasses, tables.sites.allocationClasses.size());
    return layoutsThatKeepTheirShape;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

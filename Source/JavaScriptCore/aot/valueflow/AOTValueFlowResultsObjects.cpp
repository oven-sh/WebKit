/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTFlowRecords.h"
#include "AOTPhaseTimes.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<ObjectMutation> mutationsWithoutMark { ObjectMutation::StoreOfOtherType };

namespace {

using WrittenNode = ValueFlowHeapIndex::WrittenNode;
using KeysAddedToClosures = Vector<std::pair<uint32_t, PropertyKeyID>>;

struct ObjectsOfLayouts {
    void add(LayoutID layout, OptionSet<ObjectMutation> mutations)
    {
        isAllocated.quickSet(layout.index());
        possibleMutations[layout.index()].add(mutations);
    }

    BitVector isAllocated;
    Vector<OptionSet<ObjectMutation>> possibleMutations;
};

struct Allocation {
    FunctionID function;
    ValueID value;
};

struct MutationCause {
    ObjectMutation mutation;
    UnprovenCause cause;
};

struct ObjectCreation {
    bool mayHaveOtherPrototype { false };
    ValueSetID layoutSelectors { ValueSets::nothing };
};

struct PrototypeLinks {
    PrototypeLinks(const ValueFlowResultInputs&, const ValueFlowHeapIndex&);

    std::array<ValueFlowObjectID, 3> standInsOf(ValueFlowObjectID object) const
    {
        return { object, inputs.hasEscaped(object) ? ValueFlowUniverse::unknownObject() : ValueFlowObjectID(), inputs.isLost(object) ? ValueFlowUniverse::untrackedObject() : ValueFlowObjectID() };
    }

    bool covers(ValueSetID set, ValueFlowObjectID object) const
    {
        return std::ranges::any_of(standInsOf(object), [&](ValueFlowObjectID standIn) {
            return standIn && inputs.sets().contains(set, standIn);
        });
    }

    ValueSetID linkedPrototypes(ValueSetMaker& makerOfThread, ValueFlowObjectID object) const
    {
        ValueSetID links = linksOfAllObjects;
        for (ValueFlowObjectID standIn : standInsOf(object)) {
            if (auto found = standIn ? linksByObject.find(standIn.index()) : linksByObject.end(); found != linksByObject.end())
                links = makerOfThread.unite(links, found->value);
        }
        return links;
    }

    const ValueFlowResultInputs& inputs;
    BorrowedValueSetMaker borrowedMaker;
    ValueSetMaker& maker;
    ValueSetID classPrototypes;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> linksByObject;
    ValueSetID linksOfAllObjects { ValueSets::nothing };
    ValueSetID prototypes { ValueSets::nothing };
    ValueSetID functionsWithNonObjectPrototype { ValueSets::nothing };
    HashMap<uint32_t, ValueFlowNodeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> markNodes;
};

} // anonymous namespace

static constexpr MutationCause mutationCauses[] {
    { ObjectMutation::ReachUnknownJavaScript, UnprovenCause::ReachesUnknownJavaScript },
    { ObjectMutation::DeleteField, UnprovenCause::FieldDeleted },
    { ObjectMutation::RedefineProperty, UnprovenCause::PropertyRedefined },
    { ObjectMutation::AddProperty, UnprovenCause::PropertyAdded },
    { ObjectMutation::ChangePrototype, UnprovenCause::PrototypeChanged },
    { ObjectMutation::ChangeIntegrity, UnprovenCause::IntegrityChanged },
};

PrototypeLinks::PrototypeLinks(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex)
    : inputs(inputs)
    , borrowedMaker(inputs.makerPool())
    , maker(borrowedMaker.get())
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    auto unite = [&](ValueSetID& into, ValueSetID value) {
        into = maker.unite(into, value);
    };

    Vector<ValueFlowObjectID> prototypesOfClasses;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (isClassLayout(program.layouts[index]))
            prototypesOfClasses.append(universe.prototypeOf(LayoutID(index)));
    }
    classPrototypes = maker.of(prototypesOfClasses.span());
    for (ValueFlowNodeID node : heapIndex.nodesOfMarks())
        markNodes.add(inputs.solver().keyOfMadeNode(node)->payload, node);

    Vector<WrittenNode> storesToPrototypePropertyOfOtherObjects;
    Vector<WrittenNode> copies;
    ValueFlowName prototypeProperty = ValueFlowName::property(program.keyOf(BuiltinNames::prototype));
    for (const ValueFlowHeapIndex::Stretch& stretch : heapIndex.stretchesOfNodesWrittenUnder([&](ValueFlowName name) {
        return name.kind == ValueFlowNameKind::PrototypeLink || name.kind == ValueFlowNameKind::UnknownKey || name.kind == ValueFlowNameKind::CopiedFrom || name == prototypeProperty;
    })) {
        for (const WrittenNode& node : stretch.nodes) {
            if (stretch.name.kind == ValueFlowNameKind::PrototypeLink) {
                unite(prototypes, node.value);
                if (ValueSets::isComplement(node.objects)) {
                    unite(linksOfAllObjects, node.value);
                    continue;
                }
                sets.forEachMember(node.objects, [&](ValueFlowObjectID object) {
                    unite(linksByObject.add(object.index(), ValueSets::nothing).iterator->value, node.value);
                });
                continue;
            }
            bool mayWriteIntoFunction = sets.meet(node.objects, universe.callables());
            if (stretch.name.kind == ValueFlowNameKind::CopiedFrom) {
                copies.append({ node.objects, maker.intersect(node.value, universe.objects()) });
                if (mayWriteIntoFunction)
                    unite(functionsWithNonObjectPrototype, node.objects);
            } else if (mayWriteIntoFunction) {
                unite(prototypes, node.value);
                if (!sets.isSubset(node.value, universe.objects()))
                    unite(functionsWithNonObjectPrototype, node.objects);
            } else
                storesToPrototypePropertyOfOtherObjects.append(node);
        }
    }

    auto standsForMemberOf = [&](ValueSetID holdingPlaceholder, ValueSetID other) {
        return (sets.contains(holdingPlaceholder, ValueFlowUniverse::unknownObject()) && sets.meet(other, inputs.solver().possibleValuesOf(ValueFlowProgramNodes::escaped())))
            || (sets.contains(holdingPlaceholder, ValueFlowUniverse::untrackedObject()) && sets.meet(other, inputs.solver().possibleValuesOf(ValueFlowProgramNodes::lost())));
    };
    auto mayShareObject = [&](ValueSetID first, ValueSetID second) {
        return sets.meet(first, second) || standsForMemberOf(first, second) || standsForMemberOf(second, first);
    };
    ValueSetID sources = ValueSets::nothing;
    for (bool hasGrown = true; hasGrown;) {
        hasGrown = false;
        for (const WrittenNode& copy : copies) {
            if (sets.isSubset(copy.value, sources) || (!sets.meet(copy.objects, universe.callables()) && !mayShareObject(copy.objects, sources)))
                continue;
            unite(sources, copy.value);
            hasGrown = true;
        }
    }
    for (const WrittenNode& store : storesToPrototypePropertyOfOtherObjects) {
        if (mayShareObject(store.objects, sources))
            unite(prototypes, store.value);
    }
}

static BitVector findLayoutsWithInstances(const ValueFlowResultInputs& inputs)
{
    struct FoundByWorker {
        ValueSetsMet seenSets;
        Vector<ValueSetID> instancesInSets;
    };
    constexpr uint32_t nodesPerTask = 4096;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowGraph& graph = inputs.solver().graph();
    uint32_t numberOfNodes = graph.numberOfNodes();
    ValueSetMakers makers(inputs);
    Vector<ValueFlowObjectID> everyInstance(inputs.program.layouts.size(), [&](size_t layout) {
        return universe.instanceOf(LayoutID(static_cast<uint32_t>(layout)));
    });
    ValueSetID instances = makers.of(0).of(everyInstance.span());

    Vector<FoundByWorker> foundByWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (numberOfNodes + nodesPerTask - 1) / nodesPerTask, [&](unsigned task, unsigned worker) {
        FoundByWorker& found = foundByWorker[worker];
        for (uint32_t index = task * nodesPerTask; index < std::min(numberOfNodes, (task + 1) * nodesPerTask); ++index) {
            ValueSetID set = graph.valueOf(ValueFlowNodeID(index));
            if (found.seenSets.add(set) && inputs.sets().meet(set, instances))
                found.instancesInSets.append(makers.of(worker).intersect(set, instances));
        }
    });
    Vector<ValueSetID> instancesInSets;
    for (FoundByWorker& found : foundByWorker)
        instancesInSets.appendVector(WTF::move(found.instancesInSets));
    sortAndRemoveDuplicates(inputs.loops(), instancesInSets);

    BitVector layouts(inputs.program.layouts.size());
    inputs.sets().forEachMember(makers.of(0).uniteAll(instancesInSets.span()), [&](ValueFlowObjectID instance) {
        layouts.quickSet(universe.layoutOf(instance).index());
    });
    return layouts;
}

static OptionSet<ObjectMutation> escapedObjectMutations(const ValueFlowResultInputs& inputs)
{
    if (unseenCodeCanRun(inputs.solution))
        return allObjectMutations;
    return (inputs.solver().marksOf(ValueFlowUniverse::unknownObject()) & allObjectMutations) | ObjectMutation::ReachNativeCode;
}

OptionSet<ObjectMutation> markedMutationsOf(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    OptionSet<ObjectMutation> marked = inputs.solver().marksOf(object) & allObjectMutations;
    return inputs.hasEscaped(object) ? marked | escapedObjectMutations(inputs) : marked;
}

bool neverChangesOnceMade(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    return !markedMutationsOf(inputs, object).containsAny({ ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty,
        ObjectMutation::AddProperty, ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation });
}

static bool mayFallBackToObjectPrototype(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, FunctionID id, const Value& read)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    std::optional<unsigned> newTargetIndex = function.newTargetParameterIndex();
    if (!newTargetIndex || sameObjectThroughCasts(function, read.newTargetOperand()) != function.at(entryBlock).parameters[*newTargetIndex])
        return true;
    ValueSetID newTargets = inputs.solver().possibleValuesOf(inputs.bodies().nodes().entryParameter(id, *newTargetIndex));
    return ValueSets::isComplement(newTargets) || inputs.sets().anyMember(newTargets, [&](ValueFlowObjectID newTarget) {
        if (!inputs.universe().isObject(newTarget))
            return false;
        FunctionID constructor = inputs.universe().closureFunction(newTarget);
        if (!constructor)
            return true;
        LayoutID constructed = program.at(constructor).constructedClass;
        return (!constructed || !isClassLayout(program.at(constructed))) && links.covers(links.functionsWithNonObjectPrototype, newTarget);
    });
}

static ObjectCreation describeCreation(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, ValueSetMaker& maker, ValueFlowObjectID object, LayoutID layout)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    ValueFlowObjectID objectPrototype = universe.objectOf(Builtins::ObjectPrototype);

    ValueSetID prototypes = links.linkedPrototypes(maker, object);
    if (prototypes == ValueSets::nothing)
        return { };
    LayoutKind kind = program.at(layout).kind;
    if (kind != LayoutKind::Instance && kind != LayoutKind::Record && kind != LayoutKind::Container)
        return { .mayHaveOtherPrototype = true };

    ObjectCreation creation;
    ValueSetID allowed = maker.of(kind == LayoutKind::Instance ? universe.prototypeOf(declaredLayoutOf(program, layout)) : objectPrototype);
    const ValueFlowSite* site = universe.siteOf(object);
    bool isMadeByOperation = site && !site->ordinal && (site->origin == ValueFlowSiteOrigin::Operation || site->origin == ValueFlowSiteOrigin::BuiltinResult);
    const Function* function = isMadeByOperation ? &program.at(universe.functionOf(object)) : nullptr;
    const Value* operation = isMadeByOperation ? &function->at(site->value) : nullptr;
    if (universe.kindOf(object) == ValueFlowObjectKind::Instance) {
        FunctionID constructor = program.at(layout).constructor;
        if (constructor && !links.covers(links.functionsWithNonObjectPrototype, universe.closureOf(constructor)))
            allowed = maker.unite(allowed, universe.undefinedValue());
    } else if (operation && site->origin == ValueFlowSiteOrigin::Operation && operation->opcode == Opcode::NewObject && operation->prototypeOperand()) {
        if (mayAllocateOtherLayout(program, *operation)) {
            creation.layoutSelectors = prototypes;
            allowed = maker.unite(allowed, links.classPrototypes);
        }
        const Value& prototype = function->at(sameObjectThroughCasts(*function, operation->prototypeOperand()));
        if (prototype.opcode == Opcode::GetConstructPrototype && !mayFallBackToObjectPrototype(inputs, links, universe.functionOf(object), prototype))
            allowed = maker.unite(allowed, maker.of(objectPrototype));
    } else if (operation && site->origin == ValueFlowSiteOrigin::BuiltinResult && program.prototypeMaySelectOtherClass(layout) && OperandPosition::parent(operation->opcode)) {
        creation.layoutSelectors = prototypes;
        allowed = maker.unite(allowed, links.classPrototypes);
    }
    creation.mayHaveOtherPrototype = !inputs.sets().isSubset(prototypes, allowed);
    return creation;
}

static OptionSet<ObjectMutation> possibleMutationsOf(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, ValueFlowObjectID object)
{
    OptionSet<ObjectMutation> possible = markedMutationsOf(inputs, object) | mutationsWithoutMark;
    return links.covers(links.prototypes, object) ? possible | ObjectMutation::BecomePrototype : possible;
}

static bool isAllocatedObject(const ValueFlowResultInputs& inputs, const BitVector& layoutsWithInstances, ValueFlowObjectID object)
{
    const ValueFlowUniverse& universe = inputs.universe();
    ValueFlowObjectKind kind = universe.kindOf(object);
    if (kind != ValueFlowObjectKind::Site && kind != ValueFlowObjectKind::Instance)
        return false;
    LayoutID layout = universe.layoutOf(object);
    if (!layout || isSiteOfTypedResultThatNothingAllocates(inputs, object))
        return false;
    if (kind == ValueFlowObjectKind::Instance)
        return inputs.program.at(layout).constructor && layoutsWithInstances.quickGet(layout.index());
    return universe.siteOf(object)->origin == ValueFlowSiteOrigin::TypedResult || inputs.isReached(universe.functionOf(object));
}

static Vector<Allocation> findAllocations(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, Opcode other, ValueFlowNotedOperation otherAsNoted)
{
    return collectInParallel<Allocation>(inputs.loops(), inputs.program.functions.size(), [&](unsigned index, Vector<Allocation>& found) {
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::NewObject, otherAsNoted })) || (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, tables, FunctionID(index))))
            return;
        const Function& function = inputs.program.functions[index];
        for (uint32_t value = 0; value < function.values.size(); ++value) {
            if (function.values[value].opcode == Opcode::NewObject || function.values[value].opcode == other)
                found.append({ FunctionID(index), ValueID(value) });
        }
    });
}

static ObjectsOfLayouts collectObjectsOfLayouts(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, const PrototypeLinks& links, const BitVector& layoutsWithInstances)
{
    struct Found {
        LayoutID layout;
        uint32_t placeholder;
        OptionSet<ObjectMutation> possibleMutations;
    };
    constexpr ValueFlowObjectID placeholders[] { ValueFlowUniverse::unknownObject(), ValueFlowUniverse::untrackedObject() };

    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    ObjectsOfLayouts objects { BitVector(program.layouts.size()), Vector<OptionSet<ObjectMutation>>(program.layouts.size()) };

    for (Allocation allocation : findAllocations(inputs, tables, Opcode::CreateIteratorResult, ValueFlowNotedOperation::CreateIteratorResult)) {
        const Value& value = program.at(allocation.function).at(allocation.value);
        LayoutID layout = value.opcode == Opcode::NewObject ? value.layout() : program.layoutOf(BuiltinLayouts::IteratorResult);
        objects.add(layout, isUnsolvedCode(inputs, tables, allocation.function) ? allObjectMutations : OptionSet<ObjectMutation> { });
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        FunctionID constructor = program.layouts[index].constructor;
        bool isUnsolved = constructor && isUnsolvedCode(inputs, tables, constructor);
        if (!constructor || (!isUnsolved && !inputs.isReached(constructor)))
            continue;
        objects.isAllocated.quickSet(index);
        if (isUnsolved)
            objects.possibleMutations[index] = allObjectMutations;
        else if (program.layouts[index].has(LayoutFlags::IsConstructorFunction) && (!layoutsWithInstances.quickGet(index) || inputs.hasEscaped(universe.closureOf(constructor))))
            objects.possibleMutations[index] = escapedObjectMutations(inputs) | mutationsWithoutMark;
    }

    ValueSetMakers makers(inputs);
    Vector<Found> allFound = collectWithMakers<Found>(inputs, makers, universe.numberOfObjects(), [&](unsigned index, ValueSetMaker& maker, Vector<Found>& found) {
        ValueFlowObjectID object(index);
        if (!isAllocatedObject(inputs, layoutsWithInstances, object))
            return;
        LayoutID layout = universe.layoutOf(object);
        OptionSet<ObjectMutation> possible = possibleMutationsOf(inputs, links, object);
        ObjectCreation creation = describeCreation(inputs, links, maker, object, layout);
        found.append({ layout, 0, creation.mayHaveOtherPrototype ? possible | ObjectMutation::ChangePrototype : possible });
        if (creation.layoutSelectors == ValueSets::nothing)
            return;
        LayoutID ownClass = declaredLayoutOf(program, layout);
        sets.forEachMember(maker.intersect(creation.layoutSelectors, links.classPrototypes), [&](ValueFlowObjectID prototype) {
            if (universe.classLayoutOf(prototype) != ownClass)
                found.append({ universe.classLayoutOf(prototype), 0, possible });
        });
        for (uint32_t placeholder = 0; placeholder < std::size(placeholders); ++placeholder) {
            if (sets.contains(creation.layoutSelectors, placeholders[placeholder]))
                found.append({ { }, placeholder, possible });
        }
    });
    OptionSet<ObjectMutation> possibleUnderPlaceholder[std::size(placeholders)];
    for (const Found& found : allFound) {
        if (found.layout)
            objects.add(found.layout, found.possibleMutations);
        else
            possibleUnderPlaceholder[found.placeholder].add(found.possibleMutations);
    }
    for (uint32_t placeholder = 0; placeholder < std::size(placeholders); ++placeholder) {
        ValueSetID covered = inputs.solver().possibleValuesOf(placeholder ? ValueFlowProgramNodes::lost() : ValueFlowProgramNodes::escaped());
        for (uint32_t index = 0; index < program.layouts.size() && !possibleUnderPlaceholder[placeholder].isEmpty(); ++index) {
            const Layout& layout = program.layouts[index];
            if (isClassLayout(layout) && (sets.contains(covered, universe.prototypeOf(LayoutID(index))) || sets.contains(covered, universe.closureOf(layout.constructor))))
                objects.add(LayoutID(index), possibleUnderPlaceholder[placeholder]);
        }
    }
    return objects;
}

static Vector<AllocationClassID> createAllocationClasses(const ValueFlowResultInputs& inputs, const ObjectsOfLayouts& objects, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    Vector<AllocationClassID> classOfLayout(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        LayoutFlow& flow = tables.application->layouts[index].flow;
        LayoutKind kind = program.layouts[index].kind;
        if (!objects.isAllocated.quickGet(index)) {
            bool isAllocatedByOperations = kind != LayoutKind::Closure && kind != LayoutKind::CoroutineFrame && kind != LayoutKind::ModuleNamespace;
            if (isAllocatedByOperations && inputs.build.isEnabled(FlowResultKind::ProvenLayoutSets, index))
                flow.allocation = AllocationState::NeverAllocated;
            continue;
        }
        if (!hasObjectsThatCodeAlters(kind))
            continue;
        if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, index)) {
            flow.cause = UnprovenCause::ResultDisabled;
            continue;
        }

        AllocationClass allocationClass;
        allocationClass.layout = LayoutID(index);
        allocationClass.impossibleMutations = allObjectMutations - objects.possibleMutations[index];
        const MutationCause* cause = std::ranges::find_if(mutationCauses, [&](const MutationCause& entry) {
            return objects.possibleMutations[index].contains(entry.mutation);
        });
        if (cause != std::end(mutationCauses))
            allocationClass.cause = cause->cause;
        classOfLayout[index] = AllocationClassID(tables.sites.allocationClasses.size());
        flow.allocationClasses = { classOfLayout[index] };
        flow.impossibleMutations = allocationClass.impossibleMutations;
        flow.cause = allocationClass.cause;
        tables.sites.allocationClasses.append(WTF::move(allocationClass));
    }
    return classOfLayout;
}

static void recordWitnesses(const ValueFlowResultInputs& inputs, PrototypeLinks& links, const BitVector& layoutsWithInstances, FlowResultTables& tables)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowSolver& solver = inputs.solver();
    ValueSetMaker& maker = links.maker;
    auto witnessAt = [&](FunctionID function, ValueID value) {
        return Witness { function.index(), value.index(), inputs.program.at(function).locationOf(value) };
    };
    auto flowOf = [&](ValueFlowObjectID object) -> LayoutFlow& {
        return tables.application->layouts[universe.layoutOf(object).index()].flow;
    };

    Vector<ValueFlowObjectID> objectsAskedAbout[std::size(mutationCauses)];
    for (uint32_t index = 0; index < universe.numberOfObjects(); ++index) {
        ValueFlowObjectID object(index);
        if (!isAllocatedObject(inputs, layoutsWithInstances, object) || flowOf(object).allocationClasses.isEmpty() || hasFunction(flowOf(object).witness))
            continue;
        if (const MutationCause* cause = std::ranges::find(mutationCauses, flowOf(object).cause, &MutationCause::cause); cause != std::end(mutationCauses))
            objectsAskedAbout[cause - mutationCauses].append(object);
    }

    for (size_t position = 0; position < std::size(mutationCauses); ++position) {
        HashMap<uint32_t, Witness, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> witnessOfMark;
        ValueFlowNodeID markNode = links.markNodes.get(static_cast<uint32_t>(mutationCauses[position].mutation));
        if (markNode && !objectsAskedAbout[position].isEmpty()) {
            ValueSetID remaining = maker.intersect(maker.unite(maker.of(objectsAskedAbout[position].span()), universe.placeholdersAlone()), solver.possibleValuesOf(markNode));
            Vector<std::pair<Witness, ValueFlowNodeID>> marks;
            solver.graph().forEachPredecessor(markNode, [&](ValueFlowNodeID predecessor) {
                ValueFlowStatementID id = solver.statementThatLogged({ predecessor, markNode });
                if (!id)
                    return;
                const ValueFlowStatement& statement = solver.statementAt(id);
                if (statement.function && statement.function.index() < inputs.program.functions.size() && statement.site)
                    marks.append({ witnessAt(statement.function, statement.site), predecessor });
            });
            AOT::sort(marks, [](const auto& first, const auto& second) {
                return std::tie(first.first.function, first.first.value) < std::tie(second.first.function, second.first.value);
            });
            for (const auto& [witness, predecessor] : marks) {
                ValueSetID brought = maker.intersect(solver.possibleValuesOf(predecessor), remaining);
                inputs.sets().forEachMember(brought, [&, witness = witness](ValueFlowObjectID object) {
                    witnessOfMark.add(object.index(), witness);
                });
                remaining = maker.intersect(remaining, ValueSets::complement(brought));
            }
        }
        for (ValueFlowObjectID object : objectsAskedAbout[position]) {
            LayoutFlow& flow = flowOf(object);
            if (hasFunction(flow.witness))
                continue;
            for (ValueFlowObjectID standIn : links.standInsOf(object)) {
                if (auto found = standIn ? witnessOfMark.find(standIn.index()) : witnessOfMark.end(); found != witnessOfMark.end() && !hasFunction(flow.witness))
                    flow.witness = found->value;
            }
            const ValueFlowSite* site = universe.siteOf(object);
            if (!hasFunction(flow.witness) && mutationCauses[position].mutation == ObjectMutation::ChangePrototype && site && describeCreation(inputs, links, maker, object, site->layout).mayHaveOtherPrototype)
                flow.witness = witnessAt(universe.functionOf(object), site->value);
            for (AllocationClassID allocationClass : flow.allocationClasses) {
                if (hasFunction(flow.witness))
                    tables.sites.allocationClasses[allocationClass.index()].witness = flow.witness;
            }
        }
    }
}

static KeysAddedToClosures collectKeysAddedToClosures(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex)
{
    const ValueFlowUniverse& universe = inputs.universe();
    const ParallelLoops& loops = inputs.loops();
    auto isListed = [](ValueFlowNameKind kind) {
        return kind == ValueFlowNameKind::Property || kind == ValueFlowNameKind::Getter || kind == ValueFlowNameKind::Setter;
    };
    Vector<ValueFlowHeapIndex::Stretch> stretches = heapIndex.stretchesOfNodesWrittenUnder([&](ValueFlowName name) {
        return isListed(name.kind) || name.kind == ValueFlowNameKind::Elements || name.kind == ValueFlowNameKind::UnknownKey || name.kind == ValueFlowNameKind::GetterUnderUnknownKey
            || name.kind == ValueFlowNameKind::SetterUnderUnknownKey || name.kind == ValueFlowNameKind::CopiedFrom;
    });
    Vector<KeysAddedToClosures> foundByWorker(loops.width());
    Vector<SetsMeetingOneSet> setsWithClosures(loops.width(), [&](size_t) {
        return SetsMeetingOneSet(inputs.sets(), universe.closures());
    });
    forEachIndexInParallel(loops, stretches.size(), [&](unsigned position, unsigned worker) {
        ValueFlowName name = stretches[position].name;
        for (const WrittenNode& node : stretches[position].nodes) {
            if (ValueSets::isComplement(node.objects)) {
                foundByWorker[worker].append({ UINT32_MAX, PropertyKeyID() });
                continue;
            }
            if (!setsWithClosures[worker].contains(node.objects))
                continue;
            inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID object) {
                if (universe.kindOf(object) == ValueFlowObjectKind::Closure)
                    foundByWorker[worker].append({ universe.closureFunction(object).index(), isListed(name.kind) ? PropertyKeyID(name.payload) : PropertyKeyID() });
            });
        }
    });
    KeysAddedToClosures added;
    for (KeysAddedToClosures& found : foundByWorker)
        added.appendVector(WTF::move(found));
    sortAndRemoveDuplicates(loops, added);
    return added;
}

static void noteClosuresBelowFunctions(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, std::span<AllocationClass> classes)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    auto listsItsNames = [&](const AllocationClass& closures) {
        return closures.addedKeysAreComplete && closures.impossibleMutations.contains(ObjectMutation::ReachUnknownJavaScript)
            && !program.at(closures.closureFunction).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator });
    };
    auto findClassOf = [&](FunctionID function) -> const AllocationClass* {
        auto found = std::ranges::lower_bound(classes, function, { }, &AllocationClass::closureFunction);
        return found != classes.end() && found->closureFunction == function ? &*found : nullptr;
    };

    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueFlowObjectID functionPrototype = universe.objectOf(Builtins::FunctionPrototype);
    Vector<std::pair<uint32_t, Vector<PropertyKeyID>>> namesOnTheWay;
    for (uint32_t index = 0; index < classes.size(); ++index) {
        if (!listsItsNames(classes[index]))
            continue;
        if (classes[index].impossibleMutations.contains(ObjectMutation::ChangePrototype)) {
            classes[index].inheritsFromListedFunctions = true;
            continue;
        }
        Vector<PropertyKeyID> names = classes[index].addedKeys;
        Vector<FunctionID, 4> passed { classes[index].closureFunction };
        bool isKnown = true;
        for (size_t next = 0; next < passed.size() && isKnown; ++next) {
            ValueSetID parents = links.linkedPrototypes(borrowedMaker.get(), universe.closureOf(passed[next]));
            if (ValueSets::isComplement(parents)) {
                isKnown = false;
                break;
            }
            inputs.sets().forEachMember(parents, [&](ValueFlowObjectID parent) {
                if (parent == functionPrototype)
                    return;
                FunctionID function = universe.kindOf(parent) == ValueFlowObjectKind::Closure ? universe.closureFunction(parent) : FunctionID();
                const AllocationClass* above = function ? findClassOf(function) : nullptr;
                if (!above || !listsItsNames(*above)) {
                    isKnown = false;
                    return;
                }
                if (passed.contains(function))
                    return;
                passed.append(function);
                names.appendVector(above->addedKeys);
            });
        }
        if (isKnown)
            namesOnTheWay.append({ index, WTF::move(names) });
    }
    for (auto& [index, names] : namesOnTheWay) {
        AOT::sort(names);
        names.shrink(std::ranges::unique(names).begin() - names.begin());
        classes[index].addedKeys = WTF::move(names);
        classes[index].inheritsFromListedFunctions = true;
    }
}

static void createClassesOfClosures(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, const PrototypeLinks& links, const KeysAddedToClosures& added, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    bool noKeysAreListed = !added.isEmpty() && added.last().first == UINT32_MAX;
    Vector<AllocationClass> classesOfClosures = collectInParallel<AllocationClass>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<AllocationClass>& classes) {
        if (!inputs.isReached(FunctionID(index)) || calls.functionsNamedByUnsolvedCode.get(index) || !inputs.build.isEnabled(FlowResultKind::HolderMutations, index))
            return;
        ValueFlowObjectID closure = inputs.universe().closureOf(FunctionID(index));
        OptionSet<ObjectMutation> possible = possibleMutationsOf(inputs, links, closure);
        auto keys = std::ranges::equal_range(added, index, { }, &std::pair<uint32_t, PropertyKeyID>::first);
        AllocationClass closures;
        closures.closureFunction = FunctionID(index);
        closures.impossibleMutations = allObjectMutations - possible;
        LayoutID constructedClass = program.functions[index].constructedClass;
        bool isDefinedByClass = constructedClass && !program.at(constructedClass).has(LayoutFlags::IsConstructorFunction);
        bool namesEveryStaticField = !constructedClass || std::ranges::all_of(program.at(constructedClass).staticFields, [](const StaticField& field) {
            return !!field.key;
        });
        closures.addedKeysAreComplete = !possible.containsAny({ ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript }) && !inputs.isLost(closure) && !noKeysAreListed && (keys.empty() || keys.back().second)
            && !isDefinedByClass && namesEveryStaticField && inputs.build.isEnabled(FlowResultKind::OwnNames, index);
        if (closures.addedKeysAreComplete) {
            for (auto [function, key] : keys)
                closures.addedKeys.append(key);
            if (constructedClass) {
                for (const StaticField& field : program.at(constructedClass).staticFields)
                    closures.addedKeys.append(field.key);
                AOT::sort(closures.addedKeys);
                closures.addedKeys.shrink(std::ranges::unique(closures.addedKeys).begin() - closures.addedKeys.begin());
            }
        }
        classes.append(WTF::move(closures));
    });
    noteClosuresBelowFunctions(inputs, links, classesOfClosures.mutableSpan());
    tables.sites.allocationClasses.appendVector(WTF::move(classesOfClosures));
}

void computeObjectResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    std::optional<PrototypeLinks> links;
    BitVector layoutsWithInstances;
    KeysAddedToClosures keysAddedToClosures;
    runTasksInParallel(inputs.loops(), {
        [&] {
            links.emplace(inputs, *inputs.heapIndex);
            times.didFinish("objects: prototype links"_s);
        },
        [&] {
            PhaseTimes timesBeside(inputs.build, PassID::AnalyzeFlow);
            layoutsWithInstances = findLayoutsWithInstances(inputs);
            timesBeside.didFinish("objects: layouts with instances, beside prototype links"_s);
        },
        [&] {
            PhaseTimes timesBeside(inputs.build, PassID::AnalyzeFlow);
            keysAddedToClosures = collectKeysAddedToClosures(inputs, *inputs.heapIndex);
            timesBeside.didFinish("objects: keys added to closures, beside prototype links"_s);
        },
    });
    times.didFinish("objects: the rest of layouts with instances and of keys added to closures"_s);
    ObjectsOfLayouts objects = collectObjectsOfLayouts(inputs, tables, *links, layoutsWithInstances);
    times.didFinish("objects: objects of layouts"_s);
    Vector<AllocationClassID> classOfLayout = createAllocationClasses(inputs, objects, tables);
    if (inputs.solver().input().remembersWhoLoggedEachEdge)
        recordWitnesses(inputs, *links, layoutsWithInstances, tables);
    times.didFinish("objects: allocation classes, witnesses"_s);

    for (Allocation allocation : findAllocations(inputs, tables, Opcode::NewClosure, ValueFlowNotedOperation::NewClosure)) {
        const Function& function = inputs.program.at(allocation.function);
        const Value& value = function.at(allocation.value);
        LayoutID layout = value.opcode == Opcode::NewObject ? value.layout() : LayoutID();
        AllocationClassID allocationClass = layout ? classOfLayout[layout.index()] : AllocationClassID();
        AllocationSiteID site(tables.sites.allocationSites.size());
        tables.sites.allocationSites.append(AllocationSite { allocation.function, layout, { }, allocationClass, function.locationOf(allocation.value) });
        tables.application->bodies[allocation.function.index()].allocations.append(FlowAllocationStamp { .value = allocation.value, .site = site, .allocationClass = allocationClass });
    }
    times.didFinish("objects: allocations stamped"_s);
    createClassesOfClosures(inputs, calls, *links, keysAddedToClosures, tables);
    times.didFinish("objects: classes of closures"_s);
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size());
    inputs.build.statistics().count(StatisticID::FlowAllocationClasses, tables.sites.allocationClasses.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

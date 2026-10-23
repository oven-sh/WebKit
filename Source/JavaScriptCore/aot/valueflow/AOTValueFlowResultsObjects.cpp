/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTFlowRecords.h"
#include "AOTLayout.h"
#include "AOTPhaseTimes.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// computeLayoutsNeverStoredOtherTypes() decides this one.
static constexpr OptionSet<ObjectMutation> mutationsWithoutMark {
    ObjectMutation::StoreOfOtherType,
};

// The others have their layout without an operation that says so.
static bool isAllocatedByOperations(LayoutKind kind)
{
    switch (kind) {
    default:
    // The code of a built-in makes its records as ordinary objects. Only an operation of compiled code makes an object with a layout of the table.
    case LayoutKind::BuiltinResult:
        return true;
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isLost(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    return inputs.sets().contains(inputs.solver().valueOf(ValueFlowProgramNodes::lost()), object);
}

namespace {

using SetsByObject = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
using NodesByMutation = HashMap<uint32_t, ValueFlowNodeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

struct ObjectsOfLayouts {
    void add(LayoutID layout, OptionSet<ObjectMutation> mutations)
    {
        isAllocated.quickSet(layout.index());
        possibleMutations[layout.index()].add(mutations);
    }

    BitVector isAllocated;
    Vector<OptionSet<ObjectMutation>> possibleMutations;
};

struct MutationCause {
    ObjectMutation mutation;
    UnprovenCause cause;
};

struct ObjectCreation {
    bool mayHaveOtherPrototype { false };
    // Of an allocation that mayAllocateOtherLayout(): every prototype it may be given. ClassTable::instanceWithPrototype() picks the layout.
    ValueSetID layoutSelectors { ValueSets::nothing };
};

// A prototype link is written when an object is created with a prototype operand and when a prototype is changed later. Only the later change marks ChangePrototype.
class PrototypeLinks {
public:
    PrototypeLinks(const ValueFlowResultInputs&, const ValueFlowHeapIndex&);

    ValueSetMaker& maker() { return m_maker; }
    ValueSetID classPrototypes() const { return m_classPrototypes; }
    ValueSetID linkedPrototypes(ValueSetMaker&, ValueFlowObjectID) const;
    bool mayBecomePrototype(ValueFlowObjectID object) const { return covers(m_prototypes, object); }
    bool prototypePropertyMayBeNonObject(ValueFlowObjectID function) const { return covers(m_functionsWithNonObjectPrototype, function); }
    ValueFlowNodeID markNode(ObjectMutation mutation) const { return m_markNodes.get(static_cast<uint32_t>(mutation)); }

private:
    bool covers(ValueSetID, ValueFlowObjectID) const;
    void unite(ValueSetID& into, ValueSetID value) { into = m_maker.unite(into, value); }
    void noteLink(ValueSetID objects, ValueSetID prototypes);
    void noteStoreToPrototypeProperty(ValueSetID holders, ValueSetID value);
    bool mayShareObject(ValueSetID, ValueSetID) const;
    void noteWhatCopiesBringAsPrototypeProperty();

    struct WriteIntoObjects {
        ValueSetID objects;
        ValueSetID value;
    };

    const ValueFlowResultInputs& m_inputs;
    BorrowedValueSetMaker m_borrowedMaker;
    ValueSetMaker& m_maker;
    ValueSetID m_escaped;
    ValueSetID m_lost;
    ValueSetID m_classPrototypes { ValueSets::nothing };
    SetsByObject m_linksByObject;
    ValueSetID m_linksOfAllObjects { ValueSets::nothing };
    ValueSetID m_prototypes { ValueSets::nothing };
    ValueSetID m_functionPrototypeValues { ValueSets::nothing };
    ValueSetID m_functionsWithNonObjectPrototype { ValueSets::nothing };
    // Under `prototype` or under a key nobody knows, into what cannot be called.
    Vector<WriteIntoObjects> m_storesToPrototypePropertyOfOtherObjects;
    // The targets, and the objects whose properties were copied onto them.
    Vector<WriteIntoObjects> m_copies;
    NodesByMutation m_markNodes;
};

} // anonymous namespace

// In the order in which the report prefers them.
static constexpr MutationCause mutationCauses[] {
    { ObjectMutation::ReachUnknownJavaScript, UnprovenCause::ReachesUnknownJavaScript },
    { ObjectMutation::DeleteField, UnprovenCause::FieldDeleted },
    { ObjectMutation::RedefineProperty, UnprovenCause::PropertyRedefined },
    { ObjectMutation::AddProperty, UnprovenCause::PropertyAdded },
    { ObjectMutation::ChangePrototype, UnprovenCause::PrototypeChanged },
    { ObjectMutation::ChangeIntegrity, UnprovenCause::IntegrityChanged },
};

PrototypeLinks::PrototypeLinks(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex)
    : m_inputs(inputs)
    , m_borrowedMaker(inputs.makerPool())
    , m_maker(m_borrowedMaker.get())
    , m_escaped(inputs.solver().valueOf(ValueFlowProgramNodes::escaped()))
    , m_lost(inputs.solver().valueOf(ValueFlowProgramNodes::lost()))
{
    const Program& program = inputs.program;
    const ValueFlowSolver& solver = inputs.solver();

    Vector<ValueFlowObjectID> classPrototypes;
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (isClassLayout(program.layouts[index]))
            classPrototypes.append(inputs.universe().prototypeOf(LayoutID(index)));
    }
    m_classPrototypes = m_maker.of(classPrototypes.span());

    for (ValueFlowNodeID node : heapIndex.nodesOfMarks())
        m_markNodes.add(solver.keyOfMadeNode(node)->payload, node);

    uint32_t prototypeKey = program.keyOf(BuiltinNames::prototype).index();
    Vector<ValueFlowHeapIndex::Stretch> stretches = heapIndex.stretchesOfNodesWrittenUnder([&](ValueFlowName name) {
        switch (name.kind) {
        case ValueFlowNameKind::PrototypeLink:
        case ValueFlowNameKind::UnknownKey:
        case ValueFlowNameKind::CopiedFrom:
            return true;
        case ValueFlowNameKind::Property:
            return name.payload == prototypeKey;
        default:
            return false;
        }
    });
    for (const ValueFlowHeapIndex::Stretch& stretch : stretches) {
        for (const ValueFlowHeapIndex::WrittenNode& node : stretch.nodes) {
            switch (stretch.name.kind) {
            case ValueFlowNameKind::PrototypeLink:
                noteLink(node.objects, node.value);
                break;
            case ValueFlowNameKind::Property:
            case ValueFlowNameKind::UnknownKey:
                noteStoreToPrototypeProperty(node.objects, node.value);
                break;
            case ValueFlowNameKind::CopiedFrom:
                m_copies.append({ node.objects, m_maker.intersect(node.value, inputs.universe().objects()) });
                if (inputs.sets().meet(node.objects, inputs.universe().callables()))
                    unite(m_functionsWithNonObjectPrototype, node.objects);
                break;
            default:
                break;
            }
        }
    }
    unite(m_prototypes, m_functionPrototypeValues);
    noteWhatCopiesBringAsPrototypeProperty();
}

bool PrototypeLinks::mayShareObject(ValueSetID first, ValueSetID second) const
{
    const ValueSets& sets = m_inputs.sets();
    auto standsForMemberOf = [&](ValueSetID holdingPlaceholder, ValueSetID other) {
        return (sets.contains(holdingPlaceholder, ValueFlowUniverse::unknownObject()) && sets.meet(other, m_escaped))
            || (sets.contains(holdingPlaceholder, ValueFlowUniverse::untrackedObject()) && sets.meet(other, m_lost));
    };
    return sets.meet(first, second) || standsForMemberOf(first, second) || standsForMemberOf(second, first);
}

// Properties that are copied onto a function may bring a `prototype` along: what a source of the copy holds under that name, or under a key nobody knows. A source may be the target of another copy.
void PrototypeLinks::noteWhatCopiesBringAsPrototypeProperty()
{
    const ValueSets& sets = m_inputs.sets();
    ValueSetID sources = ValueSets::nothing;
    for (bool hasGrown = true; hasGrown;) {
        hasGrown = false;
        for (const WriteIntoObjects& copy : m_copies) {
            if (sets.isSubset(copy.value, sources) || (!sets.meet(copy.objects, m_inputs.universe().callables()) && !mayShareObject(copy.objects, sources)))
                continue;
            unite(sources, copy.value);
            hasGrown = true;
        }
    }
    for (const WriteIntoObjects& store : m_storesToPrototypePropertyOfOtherObjects) {
        if (mayShareObject(store.objects, sources))
            unite(m_prototypes, store.value);
    }
}

// A placeholder in the set stands for every object that it covers.
bool PrototypeLinks::covers(ValueSetID set, ValueFlowObjectID object) const
{
    const ValueSets& sets = m_inputs.sets();
    if (sets.contains(set, object))
        return true;
    if (sets.contains(set, ValueFlowUniverse::unknownObject()) && sets.contains(m_escaped, object))
        return true;
    return sets.contains(set, ValueFlowUniverse::untrackedObject()) && sets.contains(m_lost, object);
}

void PrototypeLinks::noteLink(ValueSetID objects, ValueSetID prototypes)
{
    unite(m_prototypes, prototypes);
    if (ValueSets::isComplement(objects)) {
        unite(m_linksOfAllObjects, prototypes);
        return;
    }
    m_inputs.sets().forEachMember(objects, [&](ValueFlowObjectID object) {
        unite(m_linksByObject.add(object.index(), ValueSets::nothing).iterator->value, prototypes);
    });
}

void PrototypeLinks::noteStoreToPrototypeProperty(ValueSetID holders, ValueSetID value)
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    if (!m_inputs.sets().meet(holders, universe.callables())) {
        m_storesToPrototypePropertyOfOtherObjects.append({ holders, value });
        return;
    }
    unite(m_functionPrototypeValues, value);
    if (!m_inputs.sets().isSubset(value, universe.objects()))
        unite(m_functionsWithNonObjectPrototype, holders);
}

ValueSetID PrototypeLinks::linkedPrototypes(ValueSetMaker& maker, ValueFlowObjectID object) const
{
    auto linksThrough = [&](ValueFlowObjectID holder) {
        auto found = m_linksByObject.find(holder.index());
        return found == m_linksByObject.end() ? ValueSets::nothing : found->value;
    };

    ValueSetID links = maker.unite(m_linksOfAllObjects, linksThrough(object));
    if (m_inputs.sets().contains(m_escaped, object))
        links = maker.unite(links, linksThrough(ValueFlowUniverse::unknownObject()));
    if (m_inputs.sets().contains(m_lost, object))
        links = maker.unite(links, linksThrough(ValueFlowUniverse::untrackedObject()));
    return links;
}

// A construction hands the instance to the constructor and to whoever asked for it, so an instance that is ever made is in the value of some node.
static BitVector findLayoutsWithInstances(const ValueFlowResultInputs& inputs)
{
    // A set that several threads meet is tested by each of them, and adds the same.
    struct FoundByWorker {
        ValueSetsMet seenSets;
        Vector<ValueSetID> instancesInSets;
    };
    constexpr uint32_t nodesPerTask = 4096;

    const ValueFlowUniverse& universe = inputs.universe();
    const ValueFlowGraph& graph = inputs.solver().graph();
    uint32_t numberOfLayouts = inputs.program.layouts.size();
    BorrowedValueSetMaker borrowedMaker(inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    Vector<ValueFlowObjectID> everyInstance(numberOfLayouts, [&](size_t layout) {
        return universe.instanceOf(LayoutID(static_cast<uint32_t>(layout)));
    });
    ValueSetID instances = maker.of(everyInstance.span());

    uint32_t numberOfNodes = graph.numberOfNodes();
    ValueSetMakers makers(inputs);
    Vector<FoundByWorker> foundByWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), (numberOfNodes + nodesPerTask - 1) / nodesPerTask, [&](unsigned task, unsigned worker) {
        FoundByWorker& ofWorker = foundByWorker[worker];
        uint32_t end = std::min(numberOfNodes, (task + 1) * nodesPerTask);
        for (uint32_t index = task * nodesPerTask; index < end; ++index) {
            ValueSetID set = graph.valueOf(ValueFlowNodeID(index));
            if (ofWorker.seenSets.add(set) && inputs.sets().meet(set, instances))
                ofWorker.instancesInSets.append(makers.of(worker).intersect(set, instances));
        }
    });
    Vector<ValueSetID> instancesInSets;
    for (FoundByWorker& ofWorker : foundByWorker)
        instancesInSets.appendVector(WTF::move(ofWorker.instancesInSets));
    sortAndRemoveDuplicates(inputs.loops(), instancesInSets);
    ValueSetID found = maker.uniteAll(instancesInSets.span());

    BitVector layouts(numberOfLayouts);
    inputs.sets().forEachMember(found, [&](ValueFlowObjectID instance) {
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
    constexpr OptionSet<ObjectMutation> mutationsOfWhatAnObjectHolds {
        ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::AddProperty,
        ObjectMutation::ChangePrototype, ObjectMutation::StoreAfterCreation,
    };
    return !markedMutationsOf(inputs, object).containsAny(mutationsOfWhatAnObjectHolds);
}

static const Value* findNewObject(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const ValueFlowSite* site = inputs.universe().siteOf(object);
    if (!site || site->origin != ValueFlowSiteOrigin::Operation || site->ordinal)
        return nullptr;
    const Value& value = inputs.program.at(inputs.universe().functionOf(object)).at(site->value);
    return value.opcode == Opcode::NewObject ? &value : nullptr;
}

static ValueFlowObjectID expectedPrototype(const ValueFlowResultInputs& inputs, LayoutID layout)
{
    switch (inputs.program.at(layout).kind) {
    case LayoutKind::Instance:
        return inputs.universe().prototypeOf(declaredLayoutOf(inputs.program, layout));
    case LayoutKind::Record:
    case LayoutKind::Container:
        return inputs.universe().objectOf(Builtins::ObjectPrototype);
    default:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// GetConstructPrototype answers Object.prototype where new.target.prototype is no object, and its flow rule adds that answer every time.
static bool mayFallBackToObjectPrototype(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, FunctionID id, const Value& read)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const Function& function = program.at(id);
    std::optional<unsigned> newTargetIndex = function.newTargetParameterIndex();
    if (!newTargetIndex || sameObjectThroughCasts(function, read.newTargetOperand()) != function.at(entryBlock).parameters[*newTargetIndex])
        return true;

    ValueSetID newTargets = inputs.solver().valueOf(inputs.bodies().nodes().entryParameter(id, *newTargetIndex));
    if (ValueSets::isComplement(newTargets))
        return true;
    bool mayFallBack = false;
    inputs.sets().forEachMember(newTargets, [&](ValueFlowObjectID newTarget) {
        if (!universe.isObject(newTarget))
            return;
        FunctionID constructor = universe.closureFunction(newTarget);
        if (!constructor) {
            mayFallBack = true;
            return;
        }
        // The prototype property of a class is read-only.
        LayoutID constructed = program.at(constructor).constructedClass;
        if (!constructed || !isClassLayout(program.at(constructed)))
            mayFallBack |= links.prototypePropertyMayBeNonObject(newTarget);
    });
    return mayFallBack;
}

static ObjectCreation describeCreation(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, ValueSetMaker& maker, ValueFlowObjectID object, LayoutID layout)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();

    ValueSetID prototypes = links.linkedPrototypes(maker, object);
    if (prototypes == ValueSets::nothing)
        return { };
    ValueFlowObjectID expected = expectedPrototype(inputs, layout);
    if (!expected)
        return { .mayHaveOtherPrototype = true };

    ObjectCreation creation;
    ValueSetID allowed = maker.of(expected);
    if (universe.kindOf(object) == ValueFlowObjectKind::Instance) {
        // The link holds what a lookup of new.target.prototype finds, and a lookup always finds undefined too.
        FunctionID constructor = program.at(layout).constructor;
        if (constructor && !links.prototypePropertyMayBeNonObject(universe.closureOf(constructor)))
            allowed = maker.unite(allowed, universe.undefinedValue());
    } else if (const Value* allocation = findNewObject(inputs, object); allocation && allocation->prototypeOperand()) {
        FunctionID id = universe.functionOf(object);
        const Function& function = program.at(id);
        if (mayAllocateOtherLayout(program, *allocation)) {
            creation.layoutSelectors = prototypes;
            allowed = maker.unite(allowed, links.classPrototypes());
        }
        const Value& prototype = function.at(sameObjectThroughCasts(function, allocation->prototypeOperand()));
        if (prototype.opcode == Opcode::GetConstructPrototype && !mayFallBackToObjectPrototype(inputs, links, id, prototype))
            allowed = maker.unite(allowed, maker.of(universe.objectOf(Builtins::ObjectPrototype)));
    } else if (const ValueFlowSite* site = universe.siteOf(object); site && site->origin == ValueFlowSiteOrigin::BuiltinResult && !site->ordinal && program.prototypeMaySelectOtherClass(layout)) {
        // What a base class that is no class of the program makes for `super(..)` has the prototype that new.target says: the class of the operation, or one below it.
        if (OperandPosition::parent(program.at(universe.functionOf(object)).at(site->value).opcode)) {
            creation.layoutSelectors = prototypes;
            allowed = maker.unite(allowed, links.classPrototypes());
        }
    }
    creation.mayHaveOtherPrototype = !inputs.sets().isSubset(prototypes, allowed);
    return creation;
}

static OptionSet<ObjectMutation> possibleMutationsOf(const ValueFlowResultInputs& inputs, const PrototypeLinks& links, ValueFlowObjectID object)
{
    OptionSet<ObjectMutation> possible = markedMutationsOf(inputs, object) | mutationsWithoutMark;
    if (links.mayBecomePrototype(object))
        possible.add(ObjectMutation::BecomePrototype);
    return possible;
}

// An environment has no member of the universe, and nobody follows an object that unsolved code allocates.
static void noteAllocatingOperations(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, const PrototypeLinks& links, const BitVector& layoutsWithInstances, ObjectsOfLayouts& objects)
{
    struct AllocatedLayout {
        LayoutID layout;
        bool isAllocatedByUnsolvedCode;
    };

    const Program& program = inputs.program;
    Vector<AllocatedLayout> allocatedLayouts = collectInParallel<AllocatedLayout>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<AllocatedLayout>& found) {
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::NewObject, ValueFlowNotedOperation::CreateIteratorResult })))
            return;
        const Function& function = program.functions[index];
        bool isUnsolved = isUnsolvedCode(inputs, tables, FunctionID(index));
        if (!isUnsolved && !inputs.isReached(FunctionID(index)))
            return;
        for (const Value& value : function.values) {
            // ResolveBuiltins turns the second into the first.
            if (value.opcode != Opcode::NewObject && value.opcode != Opcode::CreateIteratorResult)
                continue;
            found.append({ value.opcode == Opcode::NewObject ? value.layout() : program.layoutOf(BuiltinLayouts::IteratorResult), isUnsolved });
        }
    });
    for (const AllocatedLayout& allocated : allocatedLayouts) {
        if (allocated.isAllocatedByUnsolvedCode)
            objects.add(allocated.layout, allObjectMutations);
        else
            objects.isAllocated.quickSet(allocated.layout.index());
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
            objects.possibleMutations[index] = isUnsolved ? allObjectMutations : escapedObjectMutations(inputs) | mutationsWithoutMark;
    }
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
    // Only a construction makes the object that stands for the instances of a layout. That of a record comes into a value where a placeholder passes the check of a type that lists the record: nothing is allocated there.
    if (kind == ValueFlowObjectKind::Instance)
        return inputs.program.at(layout).constructor && layoutsWithInstances.quickGet(layout.index());
    // Whoever allocates a typed result has marked its objects, and they are shared among calls.
    return universe.siteOf(object)->origin == ValueFlowSiteOrigin::TypedResult || inputs.isReached(universe.functionOf(object));
}

static ObjectsOfLayouts collectObjectsOfLayouts(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, PrototypeLinks& links, const BitVector& layoutsWithInstances)
{
    struct PlaceholderSelector {
        ValueFlowObjectID placeholder;
        ValueFlowNodeID coveredObjects;
        bool isUsed { false };
        OptionSet<ObjectMutation> possibleMutations { };
    };

    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();
    PlaceholderSelector placeholderSelectors[] {
        { ValueFlowUniverse::unknownObject(), ValueFlowProgramNodes::escaped() },
        { ValueFlowUniverse::untrackedObject(), ValueFlowProgramNodes::lost() },
    };

    ObjectsOfLayouts objects { BitVector(program.layouts.size()), Vector<OptionSet<ObjectMutation>>(program.layouts.size()) };
    noteAllocatingOperations(inputs, tables, links, layoutsWithInstances, objects);
    struct Found {
        LayoutID layout;
        uint32_t selector;
        OptionSet<ObjectMutation> possibleMutations;
    };
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
        sets.forEachMember(maker.intersect(creation.layoutSelectors, links.classPrototypes()), [&](ValueFlowObjectID prototype) {
            LayoutID selected = universe.classLayoutOf(prototype);
            if (selected != ownClass)
                found.append({ selected, 0, possible });
        });
        for (uint32_t selector = 0; selector < std::size(placeholderSelectors); ++selector) {
            if (sets.contains(creation.layoutSelectors, placeholderSelectors[selector].placeholder))
                found.append({ { }, selector, possible });
        }
    });
    for (const Found& found : allFound) {
        if (found.layout) {
            objects.add(found.layout, found.possibleMutations);
            continue;
        }
        placeholderSelectors[found.selector].isUsed = true;
        placeholderSelectors[found.selector].possibleMutations.add(found.possibleMutations);
    }

    for (const PlaceholderSelector& selector : placeholderSelectors) {
        if (!selector.isUsed)
            continue;
        ValueSetID covered = inputs.solver().valueOf(selector.coveredObjects);
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            const Layout& layout = program.layouts[index];
            if (isClassLayout(layout) && (sets.contains(covered, universe.prototypeOf(LayoutID(index))) || sets.contains(covered, universe.closureOf(layout.constructor))))
                objects.add(LayoutID(index), selector.possibleMutations);
        }
    }
    return objects;
}

static const MutationCause* findCause(OptionSet<ObjectMutation> possibleMutations)
{
    for (const MutationCause& entry : mutationCauses) {
        if (possibleMutations.contains(entry.mutation))
            return &entry;
    }
    return nullptr;
}

// One class for each layout: what may happen to one object of the layout may happen to every one.
static Vector<AllocationClassID> createAllocationClasses(const ValueFlowResultInputs& inputs, const ObjectsOfLayouts& objects, FlowResultTables& tables)
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
        if (!hasObjectsThatCodeAlters(kind))
            continue;
        if (!inputs.build.isEnabled(FlowResultKind::HolderMutations, index)) {
            flow.cause = UnprovenCause::ResultDisabled;
            continue;
        }

        AllocationClass allocationClass;
        allocationClass.layout = LayoutID(index);
        allocationClass.impossibleMutations = allObjectMutations - objects.possibleMutations[index];
        if (const MutationCause* cause = findCause(objects.possibleMutations[index]))
            allocationClass.cause = cause->cause;
        classOfLayout[index] = AllocationClassID(tables.sites.allocationClasses.size());
        flow.allocationClasses = { classOfLayout[index] };
        flow.impossibleMutations = allocationClass.impossibleMutations;
        flow.cause = allocationClass.cause;
        tables.sites.allocationClasses.append(WTF::move(allocationClass));
    }
    return classOfLayout;
}

static Witness witnessAt(const Program& program, FunctionID function, ValueID value)
{
    return { function.index(), value.index(), program.at(function).locationOf(value) };
}

namespace {

// The statement that has put a mark on an object, for all objects that are asked about.
class MarkWitnesses {
public:
    MarkWitnesses(const ValueFlowResultInputs& inputs, PrototypeLinks& links)
        : m_inputs(inputs)
        , m_links(links)
    {
    }

    void willAskAbout(ValueFlowObjectID object, ObjectMutation mutation) { m_objectsAskedAbout.ensure(static_cast<uint32_t>(mutation), [] { return Vector<ValueFlowObjectID>(); }).iterator->value.append(object); }

    void find()
    {
        const ValueFlowSolver& solver = m_inputs.solver();
        ValueSetMaker& maker = m_links.maker();
        for (auto& [mutation, objects] : m_objectsAskedAbout) {
            ValueFlowNodeID markNode = m_links.markNode(static_cast<ObjectMutation>(mutation));
            if (!markNode)
                continue;
            objects.append(ValueFlowUniverse::unknownObject());
            objects.append(ValueFlowUniverse::untrackedObject());
            AOT::sort(objects);
            objects.shrink(std::ranges::unique(objects).begin() - objects.begin());
            ValueSetID remaining = maker.intersect(maker.of(objects.span()), solver.valueOf(markNode));
            Vector<ValueFlowNodeID> predecessors;
            solver.graph().forEachPredecessor(markNode, [&](ValueFlowNodeID predecessor) {
                predecessors.append(predecessor);
            });
            AOT::sort(predecessors);
            for (ValueFlowNodeID predecessor : predecessors) {
                if (remaining == ValueSets::nothing)
                    break;
                ValueFlowStatementID id = solver.statementThatLogged({ predecessor, markNode });
                if (!id)
                    continue;
                const ValueFlowStatement& statement = solver.statementAt(id);
                if (!statement.function || statement.function.index() >= m_inputs.program.functions.size() || !statement.site)
                    continue;
                ValueSetID brought = maker.intersect(solver.valueOf(predecessor), remaining);
                if (brought == ValueSets::nothing)
                    continue;
                Witness witness = witnessAt(m_inputs.program, statement.function, statement.site);
                m_inputs.sets().forEachMember(brought, [&](ValueFlowObjectID object) {
                    m_witnesses.add(keyOf(object, static_cast<ObjectMutation>(mutation)), witness);
                });
                remaining = maker.intersect(remaining, ValueSets::complement(brought));
            }
        }
    }

    Witness of(ValueFlowObjectID object, ObjectMutation mutation) const
    {
        if (auto found = m_witnesses.find(keyOf(object, mutation)); found != m_witnesses.end())
            return found->value;
        if (m_inputs.hasEscaped(object)) {
            if (auto found = m_witnesses.find(keyOf(ValueFlowUniverse::unknownObject(), mutation)); found != m_witnesses.end())
                return found->value;
        }
        if (isLost(m_inputs, object)) {
            if (auto found = m_witnesses.find(keyOf(ValueFlowUniverse::untrackedObject(), mutation)); found != m_witnesses.end())
                return found->value;
        }
        return { };
    }

private:
    static uint64_t keyOf(ValueFlowObjectID object, ObjectMutation mutation) { return static_cast<uint64_t>(mutation) << 32 | object.index(); }

    const ValueFlowResultInputs& m_inputs;
    PrototypeLinks& m_links;
    HashMap<uint32_t, Vector<ValueFlowObjectID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_objectsAskedAbout;
    HashMap<uint64_t, Witness, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_witnesses;
};

} // anonymous namespace

// Only where the solver remembers which statement logged each edge.
static void recordWitnesses(const ValueFlowResultInputs& inputs, PrototypeLinks& links, const BitVector& layoutsWithInstances, FlowResultTables& tables)
{
    const ValueFlowUniverse& universe = inputs.universe();
    FlowApplication& application = *tables.application;
    MarkWitnesses witnesses(inputs, links);
    for (uint32_t index = 0; index < universe.numberOfObjects(); ++index) {
        ValueFlowObjectID object(index);
        if (!isAllocatedObject(inputs, layoutsWithInstances, object))
            continue;
        const LayoutFlow& flow = application.layouts[universe.layoutOf(object).index()].flow;
        if (flow.allocationClasses.isEmpty() || flow.witness.function != Wire::none)
            continue;
        if (const MutationCause* cause = std::ranges::find(mutationCauses, flow.cause, &MutationCause::cause); cause != std::end(mutationCauses))
            witnesses.willAskAbout(object, cause->mutation);
    }
    witnesses.find();

    for (uint32_t index = 0; index < universe.numberOfObjects(); ++index) {
        ValueFlowObjectID object(index);
        if (!isAllocatedObject(inputs, layoutsWithInstances, object))
            continue;
        LayoutFlow& flow = application.layouts[universe.layoutOf(object).index()].flow;
        if (flow.allocationClasses.isEmpty() || flow.witness.function != Wire::none)
            continue;
        const MutationCause* cause = std::ranges::find(mutationCauses, flow.cause, &MutationCause::cause);
        if (cause == std::end(mutationCauses))
            continue;

        Witness witness = witnesses.of(object, cause->mutation);
        const ValueFlowSite* site = universe.siteOf(object);
        if (witness.function == Wire::none && cause->mutation == ObjectMutation::ChangePrototype && site && describeCreation(inputs, links, links.maker(), object, site->layout).mayHaveOtherPrototype)
            witness = witnessAt(inputs.program, universe.functionOf(object), site->value);
        if (witness.function == Wire::none)
            continue;
        flow.witness = witness;
        for (AllocationClassID allocationClass : flow.allocationClasses)
            tables.sites.allocationClasses[allocationClass.index()].witness = witness;
    }
}

// A closure is born with the prototype of its kind of function, so only a mark says that the link changes. A class defines static members on its constructor without a mark.
namespace {

// By function: the names under which something is stored into or defined on its closures after they are created.
struct KeysAddedToClosures {
    Vector<Vector<PropertyKeyID>> keys;
    // Something is put there under a name that no key of the program spells, or nobody sees all that is put there.
    BitVector areIncomplete;
};

} // anonymous namespace

static KeysAddedToClosures collectKeysAddedToClosures(const ValueFlowResultInputs& inputs, const ValueFlowHeapIndex& heapIndex)
{
    struct FoundByWorker {
        Vector<std::pair<uint32_t, PropertyKeyID>> keysOfFunctions;
        BitVector areIncomplete;
        bool allAreIncomplete { false };
    };

    const ValueFlowUniverse& universe = inputs.universe();
    const ParallelLoops& loops = inputs.loops();
    uint32_t numberOfFunctions = inputs.program.functions.size();
    auto isListed = [](ValueFlowNameKind kind) {
        return kind == ValueFlowNameKind::Property || kind == ValueFlowNameKind::Getter || kind == ValueFlowNameKind::Setter;
    };
    Vector<ValueFlowHeapIndex::Stretch> stretches = heapIndex.stretchesOfNodesWrittenUnder([&](ValueFlowName name) {
        switch (name.kind) {
        case ValueFlowNameKind::Elements:
        case ValueFlowNameKind::UnknownKey:
        case ValueFlowNameKind::GetterUnderUnknownKey:
        case ValueFlowNameKind::SetterUnderUnknownKey:
        case ValueFlowNameKind::CopiedFrom:
            return true;
        default:
            return isListed(name.kind);
        }
    });
    Vector<FoundByWorker> foundByWorker(loops.width());
    Vector<SetsMeetingOneSet> setsWithClosures(loops.width(), [&](size_t) {
        return SetsMeetingOneSet(inputs.sets(), universe.closures());
    });
    forEachIndexInParallel(loops, stretches.size(), [&](unsigned position, unsigned worker) {
        FoundByWorker& found = foundByWorker[worker];
        ValueFlowName name = stretches[position].name;
        for (const ValueFlowHeapIndex::WrittenNode& node : stretches[position].nodes) {
            if (ValueSets::isComplement(node.objects)) {
                found.allAreIncomplete = true;
                continue;
            }
            if (!setsWithClosures[worker].contains(node.objects))
                continue;
            inputs.sets().forEachMember(node.objects, [&](ValueFlowObjectID object) {
                if (universe.kindOf(object) != ValueFlowObjectKind::Closure)
                    return;
                uint32_t function = universe.closureFunction(object).index();
                if (isListed(name.kind))
                    found.keysOfFunctions.append({ function, PropertyKeyID(name.payload) });
                else
                    found.areIncomplete.set(function);
            });
        }
    });

    KeysAddedToClosures added { Vector<Vector<PropertyKeyID>>(numberOfFunctions), BitVector(numberOfFunctions) };
    Vector<std::pair<uint32_t, PropertyKeyID>> keysOfFunctions;
    bool allAreIncomplete = false;
    for (FoundByWorker& found : foundByWorker) {
        keysOfFunctions.appendVector(WTF::move(found.keysOfFunctions));
        added.areIncomplete.merge(found.areIncomplete);
        allAreIncomplete |= found.allAreIncomplete;
    }
    if (allAreIncomplete) {
        for (uint32_t function = 0; function < numberOfFunctions; ++function)
            added.areIncomplete.quickSet(function);
    }
    sortAndRemoveDuplicates(loops, keysOfFunctions);
    for (auto [function, key] : keysOfFunctions)
        added.keys[function].append(key);
    return added;
}

static void createClassesOfClosures(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, const PrototypeLinks& links, KeysAddedToClosures&& added, FlowResultTables& tables)
{
    // Whoever holds a closure that nobody follows any more puts there what nobody lists.
    constexpr OptionSet<ObjectMutation> mutationsAddingUnlistedKeys { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript };
    const Program& program = inputs.program;
    tables.sites.allocationClasses.appendVector(collectInParallel<AllocationClass>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<AllocationClass>& classes) {
        if (!inputs.isReached(FunctionID(index)) || calls.functionsNamedByUnsolvedCode.get(index) || !inputs.build.isEnabled(FlowResultKind::HolderMutations, index))
            return;
        ValueFlowObjectID closure = inputs.universe().closureOf(FunctionID(index));
        OptionSet<ObjectMutation> possible = possibleMutationsOf(inputs, links, closure);
        AllocationClass closures;
        closures.closureFunction = FunctionID(index);
        closures.impossibleMutations = allObjectMutations - possible;
        closures.addedKeysAreComplete = !possible.containsAny(mutationsAddingUnlistedKeys) && !inputs.isLost(closure) && !added.areIncomplete.get(index)
            && !program.functions[index].constructedClass && inputs.build.isEnabled(FlowResultKind::OwnNames, index);
        if (closures.addedKeysAreComplete)
            closures.addedKeys = WTF::move(added.keys[index]);
        classes.append(WTF::move(closures));
    }));
}

// Every allocation of an object with a layout and of a closure has a site, by which the report on allocations names it. An environment, and an object about whose layout nothing is known, has no class.
static void stampAllocations(const ValueFlowResultInputs& inputs, std::span<const AllocationClassID> classOfLayout, FlowResultTables& tables)
{
    struct Allocation {
        FunctionID function;
        ValueID value;
    };

    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    Vector<Allocation> allocations = collectInParallel<Allocation>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<Allocation>& found) {
        if (!inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::NewObject, ValueFlowNotedOperation::NewClosure })))
            return;
        const Function& function = program.functions[index];
        if (!inputs.isReached(FunctionID(index)) && !isUnsolvedCode(inputs, tables, FunctionID(index)))
            return;
        for (uint32_t valueIndex = 0; valueIndex < function.values.size(); ++valueIndex) {
            Opcode opcode = function.values[valueIndex].opcode;
            if (opcode == Opcode::NewObject || opcode == Opcode::NewClosure)
                found.append({ FunctionID(index), ValueID(valueIndex) });
        }
    });
    tables.sites.allocationSites.reserveCapacity(tables.sites.allocationSites.size() + allocations.size());
    for (const Allocation& allocation : allocations) {
        const Function& function = program.at(allocation.function);
        const Value& value = function.at(allocation.value);
        LayoutID layout = value.opcode == Opcode::NewObject ? value.layout() : LayoutID();
        AllocationClassID allocationClass = layout ? classOfLayout[layout.index()] : AllocationClassID();
        AllocationSiteID site(tables.sites.allocationSites.size());
        tables.sites.allocationSites.append(AllocationSite { allocation.function, layout, { }, allocationClass, function.locationOf(allocation.value) });
        application.bodies[allocation.function.index()].allocations.append(FlowAllocationStamp { .value = allocation.value, .site = site, .allocationClass = allocationClass });
    }
}

void computeObjectResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    // Each of the three walks all that the solve has made and asks the solution alone.
    std::optional<PrototypeLinks> prototypeLinks;
    BitVector layoutsWithInstances;
    KeysAddedToClosures keysAddedToClosures;
    std::unique_ptr<ValueFlowHeapIndex> ownHeapIndex = inputs.heapIndex ? nullptr : makeUnique<ValueFlowHeapIndex>(inputs);
    const ValueFlowHeapIndex& heapIndex = inputs.heapIndex ? *inputs.heapIndex : *ownHeapIndex;
    runTasksInParallel(inputs.loops(), {
        [&] {
            prototypeLinks.emplace(inputs, heapIndex);
            times.didFinish("objects: prototype links"_s);
        },
        [&] {
            PhaseTimes timesBeside(inputs.build, PassID::AnalyzeFlow);
            layoutsWithInstances = findLayoutsWithInstances(inputs);
            timesBeside.didFinish("objects: layouts with instances, beside prototype links"_s);
        },
        [&] {
            PhaseTimes timesBeside(inputs.build, PassID::AnalyzeFlow);
            keysAddedToClosures = collectKeysAddedToClosures(inputs, heapIndex);
            timesBeside.didFinish("objects: keys added to closures, beside prototype links"_s);
        },
    });
    PrototypeLinks& links = *prototypeLinks;
    times.didFinish("objects: the rest of layouts with instances and of keys added to closures"_s);
    ObjectsOfLayouts objects = collectObjectsOfLayouts(inputs, tables, links, layoutsWithInstances);
    times.didFinish("objects: objects of layouts"_s);
    Vector<AllocationClassID> classOfLayout = createAllocationClasses(inputs, objects, tables);
    if (inputs.solver().input().remembersWhoLoggedEachEdge)
        recordWitnesses(inputs, links, layoutsWithInstances, tables);
    times.didFinish("objects: allocation classes, witnesses"_s);
    stampAllocations(inputs, classOfLayout.span(), tables);
    times.didFinish("objects: allocations stamped"_s);
    createClassesOfClosures(inputs, calls, links, WTF::move(keysAddedToClosures), tables);
    times.didFinish("objects: classes of closures"_s);
    inputs.build.statistics().count(StatisticID::FlowAllocationSites, tables.sites.allocationSites.size());
    inputs.build.statistics().count(StatisticID::FlowAllocationClasses, tables.sites.allocationClasses.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

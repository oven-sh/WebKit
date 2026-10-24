/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTKnownClosures.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTConcurrentComponents.h"
#include "AOTEffectsOfOperands.h"
#include "AOTCFG.h"
#include "AOTFlowRecords.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIntegerRelations.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowPositions.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<ObjectMutation> reachMutations {
    ObjectMutation::ReachNativeCode,
    ObjectMutation::ReachUnknownJavaScript,
};

static constexpr OptionSet<ObjectMutation> unprovenContentsMutations {
    ObjectMutation::ReachNativeCode,
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::RedefineProperty,
    ObjectMutation::ChangePrototype,
};

static constexpr OptionSet<ObjectMutation> elementStorageMutations {
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::RedefineProperty,
    ObjectMutation::ChangePrototype,
};

static constexpr OptionSet<ObjectMutation> builtinBehaviorMutations {
    ObjectMutation::ReachUnknownJavaScript,
    ObjectMutation::AddProperty,
    ObjectMutation::RedefineProperty,
    ObjectMutation::ChangePrototype,
};

// Whoever uses an operand in none of these ways leaves what the operand holds where it is.
static constexpr OperandUses usesWritingObject = operandWritingUses | OperandUses { OperandUse::DefinesName, OperandUse::TakesDescriptor };

namespace {

// An instance of a built-in class that the program allocates and that only followed code gets hold of.
struct Container {
    ValueFlowObjectID object;
    BuiltinClassID builtinClass;
    OptionSet<ObjectMutation> possibleMutations;
    // All that is stored into each slot that the type of an instance lists, by the index of the slot in the type. Empty for a class whose slots the rules do not write under names of their own.
    Vector<ValueSetID, 2> stored;
};

// What the operations and the built-ins that get hold of an array do to its elements.
struct ArrayWrites {
    enum Flag : uint16_t {
        // A literal without holes, the array of a rest parameter, the pair of a step over entries, or what a built-in makes that says so of its result.
        IsCreatedDense = 1 << 0,
        IsLiteral = 1 << 1,
        // Object.entries makes it with a key and a value. The key is a string, so whoever makes the pair gives it the one storage that holds everything.
        IsPairOfEntries = 1 << 2,
        // An array in the tree of a constant.
        IsInData = 1 << 3,
        // A hole is made, or something writes of which nobody knows where.
        MayGetHole = 1 << 4,
        // At a position that was below the length at some time before.
        IsStoredWithinLength = 1 << 5,
        MayGetShorter = 1 << 6,
        // The run time gives an array that is long enough another storage when it moves the elements.
        ElementsAreMoved = 1 << 7,
        // Something stores elements that is no operation with the stored value as an operand.
        HasOtherWriter = 1 << 8,
        MayBeMadeWithHoles = 1 << 9,
        IsPoisoned = 1 << 10,
        MeetsObjectThatIsNoArray = 1 << 11,
        // A built-in makes it out of elements of other arrays and of its arguments: `list.slice()`.
        HoldsWhatIsCopied = 1 << 12,
        // Undefined, null or NaN for certain: what the storages for numbers do not hold.
        IsGivenMoreThanNumbers = 1 << 13,
    };

    bool has(Flag flag) const { return flags.load(std::memory_order_relaxed) & flag; }
    void add(uint16_t added)
    {
        if ((flags.load(std::memory_order_relaxed) & added) != added)
            flags.fetch_or(added, std::memory_order_relaxed);
    }
    void set(Flag flag, bool value)
    {
        if (value)
            add(flag);
        else
            flags.fetch_and(static_cast<uint16_t>(~flag), std::memory_order_relaxed);
    }

    // An index that was below the length is below it for good only in an array that never gets shorter.
    bool getsNoHole() const { return !has(MayGetHole) && !(has(IsStoredWithinLength) && has(MayGetShorter)); }
    bool isDense() const { return has(IsCreatedDense) && getsNoHole(); }
    // The storage changes only with what is stored.
    // The types of what is stored bound the numbers in it.
    bool everyStoreIsKnown() const { return (has(IsLiteral) || has(IsInData) || has(HoldsWhatIsCopied)) && !has(HasOtherWriter); }
    bool keepsStorageOfLiteral() const { return (has(IsLiteral) || has(IsPairOfEntries)) && getsNoHole() && !has(ElementsAreMoved); }

    std::atomic<uint16_t> flags { 0 };
    // The numbers among the operands that are stored and among what the record of a constant lists.
    ValueFlowNumbers storedNumbers;
};

class ContainerAnalysis {
    WTF_MAKE_NONCOPYABLE(ContainerAnalysis);
public:
    ContainerAnalysis(const ValueFlowResultInputs&, const FlowResultTables&, TypeTable&);

    void collectWrites();
    ValueFlowContainerResults convertContents();

private:
    using DependencyList = Vector<uint32_t, 2>;

    const Program& program() const { return m_inputs.program; }
    const BuiltinTable& builtins() const { return m_inputs.program.builtins(); }
    const ValueSets& sets() const { return m_inputs.sets(); }
    ValueSetMaker& maker() { return m_heap.maker(); }

    // An array that a built-in makes without holes of its own, and the arrays whose holes it copies.
    struct ArrayMadeByBuiltin {
        uint32_t array;
        ValueSetID arraysWhoseHolesAreCopied;
        // Between the read of a length and the copy: a callback, the conversion of an operand, an accessor. An array that gets shorter there is copied as if it had holes at its end.
        bool mayRunCodeWhileCopying { false };
        ValueSetID arraysWhoseElementsAreCopied { ValueSets::nothing };
    };

    // `list[a] = list[b]`: what is read from an array was stored into it.
    struct ElementStoredAgain {
        uint32_t array;
        FunctionID function;
        ValueID read;
        // What the type of the read says, for a read from something of which less is known.
        ValueFlowNumbers numbersOfRead;
        ValueSetID arraysRead { ValueSets::nothing };
    };
    struct ElementRead {
        FunctionID function;
        ValueID read;
        ValueSetID receivers;
    };

    struct Walker {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Walker);

        Walker(ValueSetMaker& maker, const ValueSets& sets, ValueSetID containers, ValueSetID arrays)
            : maker(maker)
            , setsWithContainers(sets, containers)
            , setsWithArrays(sets, arrays)
        {
        }

        ValueSetMaker& maker;
        SetsMeetingOneSet setsWithContainers;
        SetsMeetingOneSet setsWithArrays;
        ValueSetID arraysWithUnknownWriter { ValueSets::nothing };
        ValueSetID objectsWithReplacedContents { ValueSets::nothing };
        Vector<ArrayMadeByBuiltin> arraysMadeByBuiltins;
        Vector<ElementStoredAgain> elementsStoredAgain;
        Vector<ElementRead> elementReads;
        HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> setsOfArraysSeen;
        // Of the function whose stores are looked at.
        FunctionID analyzedFunction;
        std::unique_ptr<FunctionAnalyses> analyses;
        std::optional<IntegerRelations> relations;
        // The reads of `length`, each with the identity of the object that it measures.
        Vector<std::pair<ValueID, ValueID>> lengthReads;
    };

    Walker& walkerOf(unsigned worker);
    Lock& lockOf(uint32_t container) { return m_locks[container % m_locks.size()]; }

    std::optional<uint32_t> find(ValueFlowObjectID) const;
    bool isArray(uint32_t container) const { return m_containers[container].builtinClass == BuiltinClasses::Array; }
    void forEachContainerIn(ValueSetMaker&, ValueSetID, ValueSetID among, NOESCAPE const Invocable<void(uint32_t)> auto&) const;

    void collectContainers();
    void collectStoredValues();
    void noteWritten(Walker&, ValueSetID through, ValueFlowName, ValueSetID value);

    void collectArrayWrites();
    void noteOperation(Walker&, FunctionID, const ValueFlowStatement&);
    void noteWriteByOperation(Walker&, FunctionID, const ValueFlowStatement&, uint32_t operandIndex, ValueSetID arrays);
    void noteApplication(Walker&, const ValueFlowBuiltinApplication&);
    void noteArrayMadeBy(Walker&, const ValueFlowBuiltinApplication&);
    void settleWhichResultsAreCreatedDense();
    void settleNumbersOfCopies();
    void noteData(const ValueFlowStatement&, const DataNode& root);
    void noteStoredOperand(Walker&, ValueSetID arrays, FunctionID, ValueID stored);
    ValueFlowNumbers numbersOf(FunctionID, ValueID);
    void noteUnknownWriter(Walker&, ValueSetID arrays);
    bool isWithinLength(Walker&, FunctionID, ValueID store);

    Vector<Vector<uint32_t>> scheduleConversions();
    void convertLevel(std::span<const uint32_t> containers);
    Type typeOfContents(ValueFlowPositions&, uint32_t container, const BuiltinSlot&);

    void noteSetOfArrays(Walker&, ValueSetID);
    void findComponents();
    void listMembersOfComponents();
    void shareStorageWithinComponents();

    bool isUnchangedHolder(BuiltinID holder) const;
    bool walkUnchangedChain(BuiltinID first, NOESCAPE const Invocable<IterationStatus(BuiltinID)> auto&) const;
    void lookAtClassesOfContainers();
    bool inheritsUnchangedMembers(ValueSetMaker&, uint32_t container) const;
    bool keepsPatternAndNumericLastIndex(ValueSetMaker&, const Container&) const;
    OptionSet<PristineAspects> provenPristineAspects(ValueSetMaker&, uint32_t container) const;

    const ValueFlowResultInputs& m_inputs;
    const FlowResultTables& m_tables;
    TypeTable& m_types;
    ValueFlowHeapView m_heap;
    ValueFlowPositionCounts m_counts;
    unsigned m_indexOfElements { 0 };

    // Ascending by object.
    Vector<Container> m_containers;
    ValueSetID m_allContainers { ValueSets::nothing };
    ValueSetID m_arrays { ValueSets::nothing };
    // Whatever may be an array and is none of these.
    ValueSetID m_otherArrays { ValueSets::nothing };
    Vector<ArrayWrites> m_arrayWrites;
    ValueSetID m_kindsOfBoundedNumbers { ValueSets::nothing };
    // By function, made when the first operand of the function is asked about.
    Vector<std::atomic<BitVector*>> m_valuesBehindChecks;
    ValueSetID m_objectsWithReplacedContents { ValueSets::nothing };
    BitVector m_builtinsThatWriteOperands;
    Vector<ArrayMadeByBuiltin> m_arraysMadeByBuiltins;
    Vector<ElementStoredAgain> m_elementsStoredAgain;
    Vector<ValueSetID> m_writtenPrototypeLinks;
    Vector<ValueFlowContainerFacts> m_facts;
    ValueFlowContainerResults m_results;

    std::optional<ValueSetMakers> m_makers;
    Vector<std::unique_ptr<Walker>> m_walkers;
    std::array<Lock, 1024> m_locks;

    ConcurrentComponents m_componentsOfArrays;
    Vector<Vector<uint32_t>> m_membersOfComponent;
    Vector<Vector<uint32_t>> m_levelsOfConversions;

    BitVector m_classInheritsUnchangedMembers;
    BitVector m_classHasDefaultSpecies;
};

} // anonymous namespace

static void forEachContentsSlot(const BuiltinTable& builtins, BuiltinClassID builtinClass, NOESCAPE const Invocable<void(const BuiltinSlot&)> auto& functor)
{
    for (BuiltinSlotID id : builtins.at(builtinClass).slots) {
        const BuiltinSlot& slot = builtins.at(id);
        if (slot.indexInType)
            functor(slot);
    }
}

// An element of an array is an indexed property, which the program also reads and writes by itself.
static ValueFlowName nameOf(const BuiltinSlot& slot)
{
    if (slot.heap == AbstractHeap::Elements || slot.heap == AbstractHeap::TypedArrayElements)
        return ValueFlowName::of(ValueFlowNameKind::Elements);
    return { ValueFlowNameKind::Slot, slot.id.index() };
}

// The rules write whatever goes into a slot of these under the name of the slot. What a generator or an iterator hands out has names of its own, and what a typed array holds is decided by its class.
static bool hasFollowedSlots(BuiltinClassID builtinClass)
{
    return builtinClass == BuiltinClasses::Array || builtinClass == BuiltinClasses::Map || builtinClass == BuiltinClasses::Set || builtinClass == BuiltinClasses::Promise;
}

ContainerAnalysis::ContainerAnalysis(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, TypeTable& types)
    : m_inputs(inputs)
    , m_tables(tables)
    , m_types(types)
    , m_heap(inputs)
    , m_counts(nullptr, ValueFlowTypesOfSets::Change)
    , m_classInheritsUnchangedMembers(inputs.program.builtins().classCount())
    , m_classHasDefaultSpecies(inputs.program.builtins().classCount())
{
    m_walkers.grow(inputs.loops().width());
    forEachContentsSlot(builtins(), BuiltinClasses::Array, [&](const BuiltinSlot& slot) {
        if (slot.contentsSlot == ContentsSlot::Element)
            m_indexOfElements = *slot.indexInType;
    });
}

std::optional<uint32_t> ContainerAnalysis::find(ValueFlowObjectID object) const
{
    auto found = std::ranges::lower_bound(m_containers, object, { }, &Container::object);
    if (found == m_containers.end() || found->object != object)
        return std::nullopt;
    return static_cast<uint32_t>(found - m_containers.begin());
}

void ContainerAnalysis::forEachContainerIn(ValueSetMaker& maker, ValueSetID set, ValueSetID among, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
{
    sets().forEachMember(maker.intersect(set, among), [&](ValueFlowObjectID object) {
        functor(*find(object));
    });
}

auto ContainerAnalysis::walkerOf(unsigned worker) -> Walker&
{
    if (!m_walkers[worker])
        m_walkers[worker] = makeUnique<Walker>(m_makers->of(worker), sets(), m_allContainers, m_arrays);
    return *m_walkers[worker];
}

static BuiltinClassID classOfCandidate(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    FunctionID function = universe.functionOf(object);
    if (!function || !inputs.isReached(function))
        return { };
    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Site: {
        const ValueFlowSite& site = *universe.siteOf(object);
        BuiltinClassID builtinClass = inputs.classOf(object);
        if (site.layout || !builtinClass)
            return { };
        // A template object is frozen and has the property `raw` from its creation, and no statement fills its elements.
        if (site.origin == ValueFlowSiteOrigin::Operation && program.at(function).at(site.value).opcode == Opcode::NewTemplateObject)
            return { };
        // No mark says which prototype a construction for another new.target gives the instance.
        if (site.origin == ValueFlowSiteOrigin::BuiltinResult) {
            const Value& call = program.at(function).at(site.value);
            std::optional<unsigned> newTarget = OperandPosition::newTarget(call.opcode);
            if (newTarget && call.operand(*newTarget))
                return { };
        }
        return builtinClass;
    }
    case ValueFlowObjectKind::RestArray:
        return program.at(function).hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }) ? universe.classOf(object) : BuiltinClassID();
    case ValueFlowObjectKind::CoroutineResult:
        return universe.classOf(object);
    case ValueFlowObjectKind::Primitive:
    case ValueFlowObjectKind::UnknownObject:
    case ValueFlowObjectKind::Builtin:
    case ValueFlowObjectKind::ModuleBuiltin:
    case ValueFlowObjectKind::Namespace:
    case ValueFlowObjectKind::Closure:
    case ValueFlowObjectKind::FunctionPrototype:
    case ValueFlowObjectKind::ConstructedObject:
    case ValueFlowObjectKind::Instance:
    case ValueFlowObjectKind::Prototype:
    case ValueFlowObjectKind::ResultOfBuiltin:
    case ValueFlowObjectKind::Key:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinClassID classOfFollowedContainer(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    BuiltinClassID builtinClass = classOfCandidate(inputs, object);
    if (!builtinClass || inputs.hasEscaped(object) || inputs.isLost(object) || inputs.isHeldByNativeCode(object) || markedMutationsOf(inputs, object).containsAny(reachMutations))
        return { };
    return builtinClass;
}

void ContainerAnalysis::collectContainers()
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    m_containers = collectInParallel<Container>(m_inputs.loops(), universe.numberOfObjects(), [&](unsigned index, Vector<Container>& found) {
        ValueFlowObjectID object(index);
        BuiltinClassID builtinClass = classOfFollowedContainer(m_inputs, object);
        if (!builtinClass)
            return;
        OptionSet<ObjectMutation> possibleMutations = markedMutationsOf(m_inputs, object);

        Container container { object, builtinClass, possibleMutations, { } };
        if (hasFollowedSlots(builtinClass))
            container.stored.fill(ValueSets::nothing, builtins().numberOfSlotsInType(builtinClass));
        found.append(WTF::move(container));
    });
    Vector<ValueFlowObjectID> objects;
    Vector<ValueFlowObjectID> arrays;
    objects.reserveInitialCapacity(m_containers.size());
    for (const Container& container : m_containers) {
        objects.append(container.object);
        if (container.builtinClass == BuiltinClasses::Array)
            arrays.append(container.object);
    }
    m_allContainers = maker().of(objects.span());
    m_arrays = maker().of(arrays.span());
    m_otherArrays = maker().intersect(universe.arrays(), ValueSets::complement(m_arrays));
    m_arrayWrites = Vector<ArrayWrites>(m_containers.size());
    m_facts = Vector<ValueFlowContainerFacts>(m_containers.size());
    m_writtenPrototypeLinks.fill(ValueSets::nothing, m_containers.size());
    m_results.setObjects(universe.numberOfObjects(), objects.span());
}

void ContainerAnalysis::noteWritten(Walker& walker, ValueSetID through, ValueFlowName name, ValueSetID value)
{
    forEachContainerIn(walker.maker, through, m_allContainers, [&](uint32_t index) {
        Container& container = m_containers[index];
        if (name == ValueFlowName::of(ValueFlowNameKind::PrototypeLink)) {
            Locker locker { lockOf(index) };
            m_writtenPrototypeLinks[index] = walker.maker.unite(m_writtenPrototypeLinks[index], value);
        }
        if (container.stored.isEmpty())
            return;
        forEachContentsSlot(builtins(), container.builtinClass, [&](const BuiltinSlot& slot) {
            ValueFlowName nameOfSlot = nameOf(slot);
            if (nameOfSlot == name || mayBeTheSameName(name.kind, nameOfSlot.kind)) {
                Locker locker { lockOf(index) };
                ValueSetID& stored = container.stored[*slot.indexInType];
                stored = walker.maker.unite(stored, value);
            }
        });
    });
}

// No container has escaped or is lost, so nothing that is written through a placeholder reaches one.
void ContainerAnalysis::collectStoredValues()
{
    Vector<ValueFlowHeapIndex::Stretch> stretches = m_heap.index().stretchesOfNodesWrittenUnder([](ValueFlowName name) {
        switch (name.kind) {
        case ValueFlowNameKind::PrototypeLink:
        case ValueFlowNameKind::Elements:
        case ValueFlowNameKind::UnknownKey:
        case ValueFlowNameKind::Slot:
        case ValueFlowNameKind::InternalFields:
            return true;
        case ValueFlowNameKind::Property:
        case ValueFlowNameKind::Getter:
        case ValueFlowNameKind::Setter:
        case ValueFlowNameKind::GetterUnderUnknownKey:
        case ValueFlowNameKind::SetterUnderUnknownKey:
        case ValueFlowNameKind::EveryProperty:
        case ValueFlowNameKind::EveryGetter:
        case ValueFlowNameKind::EverySetter:
        case ValueFlowNameKind::CopiedFrom:
        case ValueFlowNameKind::DeletedMember:
        case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
        case ValueFlowNameKind::RedefinedMember:
        case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
        case ValueFlowNameKind::PrivateField:
        case ValueFlowNameKind::HiddenSlot:
        case ValueFlowNameKind::SteppedValues:
        case ValueFlowNameKind::ReturnedValue:
        case ValueFlowNameKind::SentValues:
        case ValueFlowNameKind::OpenedBy:
        case ValueFlowNameKind::ResolvedPromises:
        case ValueFlowNameKind::RejectedPromises:
        case ValueFlowNameKind::AllocatedAs:
        case ValueFlowNameKind::OpenedIterators:
        case ValueFlowNameKind::IteratedValues:
        case ValueFlowNameKind::IteratorResults:
        case ValueFlowNameKind::AwaitedValue:
        case ValueFlowNameKind::MemberWritten:
        case ValueFlowNameKind::EveryMemberWritten:
        case ValueFlowNameKind::EveryMemberRead:
        case ValueFlowNameKind::RestArgument:
        case ValueFlowNameKind::AddedNames:
        case ValueFlowNameKind::FoundThroughEscapedInstances:
            return false;
        }
    });
    forEachIndexInParallel(m_inputs.loops(), stretches.size(), [&](unsigned index, unsigned worker) {
        Walker& walker = walkerOf(worker);
        for (const ValueFlowHeapIndex::WrittenNode& node : stretches[index].nodes) {
            if (walker.setsWithContainers.contains(node.objects))
                noteWritten(walker, node.objects, stretches[index].name, node.value);
        }
    });
}

// Where a check has failed, the generic version stores what the same operations make of whatever the check has turned away.
ValueFlowNumbers ContainerAnalysis::numbersOf(FunctionID id, ValueID value)
{
    const Function& function = program().at(id);
    std::atomic<BitVector*>& known = m_valuesBehindChecks[id.index()];
    const BitVector* behindChecks = m_inputs.bodies().valuesBehindChecks(id);
    if (!behindChecks)
        behindChecks = known.load(std::memory_order_acquire);
    if (!behindChecks) {
        std::unique_ptr<BitVector> found = makeUnique<BitVector>(findValuesBehindChecks(program(), function, CFG(function)));
        BitVector* madeByAnotherThread = nullptr;
        behindChecks = known.compare_exchange_strong(madeByAnotherThread, found.get(), std::memory_order_acq_rel, std::memory_order_acquire) ? found.release() : madeByAnotherThread;
    }
    if (behindChecks->get(value.index()))
        return m_inputs.numbersOfValues ? m_inputs.numbersOfValues->numbersOf(id, value) : ValueFlowNumbers::any();
    ValueFlowNumbers numbers;
    numbers.add(program().typeOf(function, value));
    return numbers;
}

void ContainerAnalysis::noteStoredOperand(Walker& walker, ValueSetID arrays, FunctionID function, ValueID stored)
{
    const Function& body = program().at(function);
    ValueID definition = definitionThroughSameValues(body, stored);
    bool isReadFromArray = body.at(definition).opcode == Opcode::ElemGet;
    const Type& type = program().typeOf(body, stored);
    bool isNaN = body.at(definition).opcode == Opcode::Double && std::isnan(body.at(definition).number());
    if (isNaN || (!type.isNever() && type.hasOnlyPrimitives(Primitive::UndefinedOrNull))) {
        forEachContainerIn(walker.maker, arrays, m_arrays, [&](uint32_t array) {
            m_arrayWrites[array].add(ArrayWrites::IsGivenMoreThanNumbers);
        });
    }
    // Only an array at which a number arrives has numbers to bound.
    std::optional<ValueFlowNumbers> numbers;
    forEachContainerIn(walker.maker, arrays, m_arrays, [&](uint32_t array) {
        if (!sets().meet(m_containers[array].stored[m_indexOfElements], m_kindsOfBoundedNumbers))
            return;
        if (!numbers)
            numbers = numbersOf(function, stored);
        if (isReadFromArray) {
            walker.elementsStoredAgain.append({ array, function, definition, *numbers });
            return;
        }
        Locker locker { lockOf(array) };
        m_arrayWrites[array].storedNumbers.unite(*numbers);
    });
}

void ContainerAnalysis::noteData(const ValueFlowStatement& statement, const DataNode& root)
{
    bool becomesLiterals = isExpandedForItsSizeAlone(root, m_inputs.build.limit(LimitID::MaxExpandedDataNodes));
    forEachDataNodeInPreorder(root, [&](uint32_t ordinal, const DataNode& node) {
        const auto* listed = node.tryAs<DataNode::Array>();
        ValueFlowObjectID object = listed ? m_inputs.universe().siteObject(statement, ordinal) : ValueFlowObjectID();
        std::optional<uint32_t> array = object ? find(object) : std::nullopt;
        if (!array)
            return;
        ArrayWrites& writes = m_arrayWrites[*array];
        bool hasHole = false;
        bool holdsMoreThanNumbers = false;
        ValueFlowNumbers numbers;
        for (const DataNode& element : listed->elements) {
            if (element.is<DataNode::Hole>())
                hasHole = true;
            const auto* number = element.tryAs<DataNode::Number>();
            holdsMoreThanNumbers |= number ? std::isnan(number->value) : element.is<DataNode::Undefined>() || element.is<DataNode::Null>();
            if (!number)
                continue;
            if (isTrackedInteger(number->value))
                numbers.integers = numbers.integers.unionWith(Range::one(static_cast<int64_t>(number->value)));
            else
                numbers.mayBeOtherThanIntegers = true;
        }
        writes.add(ArrayWrites::IsInData);
        writes.set(ArrayWrites::IsLiteral, becomesLiterals);
        writes.set(ArrayWrites::IsCreatedDense, !hasHole);
        if (holdsMoreThanNumbers)
            writes.add(ArrayWrites::IsGivenMoreThanNumbers);
        Locker locker { lockOf(*array) };
        writes.storedNumbers.unite(numbers);
    });
}

void ContainerAnalysis::noteUnknownWriter(Walker& walker, ValueSetID arrays)
{
    // Thousands of applications may each write any of the same tens of thousands of arrays.
    ValueSetID others = walker.maker.intersect(arrays, ValueSets::complement(walker.arraysWithUnknownWriter));
    if (others == ValueSets::nothing)
        return;
    walker.arraysWithUnknownWriter = walker.maker.unite(walker.arraysWithUnknownWriter, others);
    forEachContainerIn(walker.maker, others, m_arrays, [&](uint32_t array) {
        m_arrayWrites[array].add(ArrayWrites::MayGetHole | ArrayWrites::MayGetShorter | ArrayWrites::ElementsAreMoved | ArrayWrites::HasOtherWriter);
    });
}

// The position is not negative and lies below what a read of the length of the same object has answered before.
bool ContainerAnalysis::isWithinLength(Walker& walker, FunctionID id, ValueID store)
{
    const Function& function = program().at(id);
    ValueID position = function.at(store).operand(OperandIndex::ElemSet::property);
    if (!program().typeOf(function, position).isInteger())
        return false;

    if (walker.analyzedFunction != id) {
        walker.analyzedFunction = id;
        walker.relations.reset();
        walker.analyses = makeUnique<FunctionAnalyses>(AnalysisContext { m_inputs.build, program(), nullptr, nullptr, id, CallResults::NotFollowed }, function);
        walker.relations.emplace(*walker.analyses);
        walker.lengthReads.shrink(0);
        PropertyKeyID lengthKey = program().keyOf(BuiltinNames::length);
        unsigned receiver = *receiverOf(Opcode::PropGet);
        ValueSetID arraysOrUndefined = walker.maker.unite(m_inputs.universe().arrays(), m_inputs.universe().undefinedValue());
        for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(id.index())) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::PropGet || function.at(statement.site).key() != lengthKey)
                continue;
            walker.lengthReads.append({ walker.analyses->identityOf(function.at(statement.site).operand(receiver)), statement.site });
            // The length of an array is a data property of its own that nothing redefines, and a read from undefined answers nothing. The type of the result says so only once the receiver has a type.
            if (sets().isSubset(m_heap.valueOf(walker.maker, m_inputs.bodies().operandsOf(statement)[receiver]), arraysOrUndefined))
                walker.relations->assumeRange(statement.site, Range::uint32(), walker.analyses->useLists().blockOf(statement.site));
        }
    }

    using Answer = IntegerRelations::Answer;
    BlockID block = walker.analyses->useLists().blockOf(store);
    if (walker.relations->isAtLeast(position, 0, block) != Answer::Proven)
        return false;
    ValueID array = walker.analyses->identityOf(function.at(store).operand(OperandIndex::ElemSet::object));
    return std::ranges::any_of(walker.lengthReads, [&](const std::pair<ValueID, ValueID>& read) {
        return read.first == array && walker.relations->isAtMost(position, read.second, -1, block) == Answer::Proven;
    });
}

void ContainerAnalysis::noteWriteByOperation(Walker& walker, FunctionID function, const ValueFlowStatement& statement, uint32_t operandIndex, ValueSetID arrays)
{
    const Value& operation = program().at(function).at(statement.site);
    auto addToEachArray = [&](ArrayWrites::Flag flag) {
        forEachContainerIn(walker.maker, arrays, m_arrays, [&](uint32_t array) {
            m_arrayWrites[array].add(flag);
        });
    };

    switch (statement.opcode) {
    case Opcode::AppendElement:
        noteStoredOperand(walker, arrays, function, operation.operand(OperandIndex::AppendElement::value));
        return;
    case Opcode::AppendSpread:
        addToEachArray(ArrayWrites::HasOtherWriter);
        return;
    case Opcode::ElemSet: {
        static_assert(OperandIndex::ElemSet::object == 0);
        if (operandIndex != OperandIndex::ElemSet::object)
            break;
        noteStoredOperand(walker, arrays, function, operation.operand(OperandIndex::ElemSet::value));
        addToEachArray(isWithinLength(walker, function, statement.site) ? ArrayWrites::IsStoredWithinLength : ArrayWrites::MayGetHole);
        return;
    }
    case Opcode::PropSet:
    case Opcode::PropDefine:
        // A property under a name that is no index lies beside the elements.
        if (operation.key() != program().keyOf(BuiltinNames::length) && !program().isIndexLike(operation.key()))
            return;
        break;
    default:
        break;
    }
    noteUnknownWriter(walker, arrays);
}

void ContainerAnalysis::noteOperation(Walker& walker, FunctionID function, const ValueFlowStatement& statement)
{
    std::span<const ValueFlowOperand> operands = m_inputs.bodies().operandsOf(statement);
    const Value& operation = program().at(function).at(statement.site);
    if (statement.opcode == Opcode::NewArray) {
        ValueFlowObjectID object = m_inputs.universe().siteObject(function, statement.site);
        std::optional<uint32_t> array = object ? find(object) : std::nullopt;
        if (array) {
            m_arrayWrites[*array].add(ArrayWrites::IsLiteral);
            m_arrayWrites[*array].set(ArrayWrites::IsCreatedDense, operation.holes().empty());
            for (uint32_t index = OperandIndex::NewArray::elements; index < operation.numberOfOperands(); ++index)
                noteStoredOperand(walker, walker.maker.of(object), function, operation.operand(index));
        }
    }
    if (statement.opcode == Opcode::Data)
        noteData(statement, program().at(operation.data()).root);
    // The array of such an operation is the pair that a step over the entries of a collection hands out: a key and a value.
    if (statement.opcode == Opcode::IteratorResume || statement.opcode == Opcode::AppendSpread) {
        ValueFlowObjectID object = m_inputs.universe().siteObject(function, statement.site);
        if (std::optional<uint32_t> pair = object ? find(object) : std::nullopt)
            m_arrayWrites[*pair].add(ArrayWrites::IsCreatedDense);
    }

    // What a call does to an operand its callees do: a function of the program with operations of its own, a built-in as its records say. An operand of a callee that nobody knows has escaped.
    bool isCall = effectRuleOf(statement.opcode) == EffectRule::Call;
    for (uint32_t index = 0; index < operands.size(); ++index) {
        const ValueFlowOperand& operand = operands[index];
        bool mayHoldArray = (operand.constant && walker.setsWithArrays.contains(operand.constant)) || (operand.node && walker.setsWithArrays.contains(m_inputs.solver().valueOf(operand.node)));
        if (!mayHoldArray)
            continue;
        ValueSetID value = m_heap.valueOf(walker.maker, operand);
        noteSetOfArrays(walker, value);
        if (statement.opcode == Opcode::ElemGet && index == OperandIndex::ElemGet::object)
            walker.elementReads.append({ function, statement.site, value });
        ValueSetID arrays = walker.maker.intersect(value, m_arrays);
        // The row of a store by name says that it inspects its receiver: what it writes depends on the name, and `length` and an index write the elements.
        bool storesByName = (statement.opcode == Opcode::PropSet || statement.opcode == Opcode::PropDefine) && index == *receiverOf(statement.opcode);
        if (arrays == ValueSets::nothing || isCall || (!storesByName && !operandUsesOf(statement.opcode, index).containsAny(usesWritingObject)))
            continue;
        noteWriteByOperation(walker, function, statement, index, arrays);
    }
}

static BitVector findBuiltinsThatWriteOperands(const BuiltinTable& builtins)
{
    auto writes = [](const BuiltinParameter& parameter) {
        return parameter.roles.contains(BuiltinOperandRole::ReplacesContents) || parameter.uses.containsAny(usesWritingObject);
    };
    BitVector builtinsThatWrite(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        bool writesOperand = std::ranges::any_of(builtins.overloadsOf(BuiltinID(index)), [&](const BuiltinOverload& overload) {
            return (overload.receiver && writes(*overload.receiver)) || std::ranges::any_of(overload.parameters, writes);
        });
        if (writesOperand)
            builtinsThatWrite.quickSet(index);
    }
    return builtinsThatWrite;
}

// Which overload a call selects depends on its operands. Every overload of the mode counts.
void ContainerAnalysis::noteApplication(Walker& walker, const ValueFlowBuiltinApplication& application)
{
    ValueSetMaker& maker = walker.maker;
    if (!m_builtinsThatWriteOperands.quickGet(application.builtin.index()))
        return;
    BuiltinID builtin = builtins().canonicalID(application.builtin);
    auto is = [&](BuiltinID other) {
        return builtin == builtins().canonicalID(other);
    };
    auto noteParameter = [&](const BuiltinParameter& parameter) {
        ValueSetID value = application.valueAt(parameter.operand, maker);
        if (parameter.roles.contains(BuiltinOperandRole::ReplacesContents))
            walker.objectsWithReplacedContents = maker.unite(walker.objectsWithReplacedContents, maker.intersect(value, m_allContainers));
        if (!parameter.uses.containsAny(usesWritingObject))
            return;
        ValueSetID arrays = maker.intersect(value, m_arrays);
        if (arrays == ValueSets::nothing)
            return;
        bool appends = is(Builtins::ArrayPrototypePush);
        bool prepends = is(Builtins::ArrayPrototypeUnshift);
        bool takesLast = is(Builtins::ArrayPrototypePop);
        bool takesFirst = is(Builtins::ArrayPrototypeShift);
        if (!parameter.operand.isReceiver() || application.mode() != BuiltinCallMode::Call || (!appends && !prepends && !takesLast && !takesFirst)) {
            noteUnknownWriter(walker, arrays);
            return;
        }

        bool spellsOutArguments = application.site && program().at(application.function).at(application.site).opcode == Opcode::Call;
        if ((appends || prepends) && spellsOutArguments) {
            const Value& call = program().at(application.function).at(application.site);
            for (uint32_t index = 2; index < call.numberOfOperands(); ++index)
                noteStoredOperand(walker, arrays, application.function, call.operand(index));
        }
        uint16_t flags = 0;
        if ((appends || prepends) && !spellsOutArguments)
            flags |= ArrayWrites::HasOtherWriter;
        if (takesLast || takesFirst)
            flags |= ArrayWrites::MayGetShorter;
        if (prepends || takesFirst)
            flags |= ArrayWrites::ElementsAreMoved;
        forEachContainerIn(maker, arrays, m_arrays, [&](uint32_t array) {
            m_arrayWrites[array].add(flags);
        });
    };
    for (const BuiltinOverload& overload : builtins().overloadsOf(application.builtin)) {
        if (overload.mode != application.mode())
            continue;
        if (overload.receiver)
            noteParameter(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            noteParameter(parameter);
    }
}

static const BuiltinTypePart* findArrayMadeBy(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    if (!overload.result.type)
        return nullptr;
    const BuiltinType& type = builtins.at(overload.result.type);
    if (type.freshness == BuiltinFreshness::No)
        return nullptr;
    auto found = std::ranges::find_if(type.parts, [](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && part.builtinClass == BuiltinClasses::Array;
    });
    return found == type.parts.end() ? nullptr : &*found;
}

// The table says of a new array that every index below its length holds a value, and of an operand that its holes are copied: `list.slice()` has the holes of the list and no others.
void ContainerAnalysis::noteArrayMadeBy(Walker& walker, const ValueFlowBuiltinApplication& application)
{
    ValueSetMaker& maker = walker.maker;
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueFlowObjectID object = application.site ? universe.siteObject(application.function, application.site) : universe.resultOf(application.builtin, 0);
    std::optional<uint32_t> array = object ? find(object) : std::nullopt;
    if (!array || !isArray(*array))
        return;

    // Every number in what these make was in an array that they copy, or is an argument.
    static constexpr std::array copyingBuiltins { Builtins::ArrayPrototypeSlice, Builtins::ArrayPrototypeConcat, Builtins::ArrayPrototypeFilter, Builtins::ArrayPrototypeToReversed, Builtins::ArrayPrototypeToSorted, Builtins::ArrayPrototypeToSpliced, Builtins::ArrayPrototypeWith };
    bool spellsOutArguments = application.site && program().at(application.function).at(application.site).opcode == Opcode::Call;
    bool holdsWhatIsCopied = spellsOutArguments && application.mode() == BuiltinCallMode::Call && std::ranges::any_of(copyingBuiltins, [&](BuiltinID other) {
        return builtins().canonicalID(application.builtin) == builtins().canonicalID(other);
    });
    ValueSetID sources = ValueSets::nothing;
    ValueSetID copied = ValueSets::nothing;
    bool mayRunCode = false;
    auto noteParameter = [&](const BuiltinParameter& parameter) {
        ValueSetID objects = maker.intersect(application.valueAt(parameter.operand, maker), ValueSets::complement(universe.primitives()));
        if (parameter.operand.isReceiver() || parameter.roles.contains(BuiltinOperandRole::CopiesHolesToResult)) {
            holdsWhatIsCopied &= sets().isSubset(objects, m_arrays);
            sources = maker.unite(sources, objects);
        }
        if (!parameter.roles.contains(BuiltinOperandRole::CopiesHolesToResult)) {
            mayRunCode |= objects != ValueSets::nothing;
            return;
        }
        // A primitive has no holes to copy. Of an object that is no array that is followed nobody knows which indexes it has.
        if (sets().isSubset(objects, m_arrays))
            copied = maker.unite(copied, objects);
        else
            m_arrayWrites[*array].add(ArrayWrites::MayBeMadeWithHoles);
    };
    for (const BuiltinOverload& overload : builtins().overloadsOf(application.builtin)) {
        const BuiltinTypePart* made = overload.mode == application.mode() ? findArrayMadeBy(builtins(), overload) : nullptr;
        if (!made)
            continue;
        // An array that is never built, because only its first elements are read (`text.split(",")[0]`), is worth more than the proof.
        if (!made->pristine.contains(PristineAspects::Dense) || overload.code.prefixForm) {
            m_arrayWrites[*array].add(ArrayWrites::MayBeMadeWithHoles);
            holdsWhatIsCopied = false;
            continue;
        }
        if (overload.receiver)
            noteParameter(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            noteParameter(parameter);
    }
    forEachContainerIn(maker, copied, m_arrays, [&](uint32_t source) {
        mayRunCode |= m_containers[source].possibleMutations.containsAny({ ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype });
    });
    if (holdsWhatIsCopied) {
        const Value& call = program().at(application.function).at(application.site);
        for (uint32_t index = 2; index < call.numberOfOperands(); ++index)
            noteStoredOperand(walker, maker.of(object), application.function, call.operand(index));
    }
    m_arrayWrites[*array].add(holdsWhatIsCopied ? ArrayWrites::HoldsWhatIsCopied : ArrayWrites::HasOtherWriter);
    walker.arraysMadeByBuiltins.append({ *array, copied, mayRunCode, holdsWhatIsCopied ? sources : ValueSets::nothing });
}

// An array that copies holes has none where the arrays that it copies have none.
void ContainerAnalysis::settleWhichResultsAreCreatedDense()
{
    for (const ArrayMadeByBuiltin& made : m_arraysMadeByBuiltins)
        m_arrayWrites[made.array].set(ArrayWrites::IsCreatedDense, !m_arrayWrites[made.array].has(ArrayWrites::MayBeMadeWithHoles));
    bool didChange = true;
    while (didChange) {
        didChange = false;
        for (const ArrayMadeByBuiltin& made : m_arraysMadeByBuiltins) {
            ArrayWrites& writes = m_arrayWrites[made.array];
            if (!writes.has(ArrayWrites::IsCreatedDense))
                continue;
            bool copiesHole = false;
            forEachContainerIn(maker(), made.arraysWhoseHolesAreCopied, m_arrays, [&](uint32_t copied) {
                copiesHole |= !m_arrayWrites[copied].isDense() || (made.mayRunCodeWhileCopying && m_arrayWrites[copied].has(ArrayWrites::MayGetShorter));
            });
            if (copiesHole) {
                writes.set(ArrayWrites::IsCreatedDense, false);
                didChange = true;
            }
        }
    }
}

// A hole is read from the prototypes: no number comes from there while the array keeps the prototype that it was made with, on which no element can be defined.
void ContainerAnalysis::settleNumbersOfCopies()
{
    bool didChange = true;
    while (didChange) {
        didChange = false;
        for (const ArrayMadeByBuiltin& made : m_arraysMadeByBuiltins) {
            ArrayWrites& writes = m_arrayWrites[made.array];
            if (!writes.has(ArrayWrites::HoldsWhatIsCopied) || !writes.everyStoreIsKnown())
                continue;
            ValueFlowNumbers numbers = writes.storedNumbers;
            bool areBounded = true;
            forEachContainerIn(maker(), made.arraysWhoseElementsAreCopied, m_arrays, [&](uint32_t source) {
                const ArrayWrites& copied = m_arrayWrites[source];
                bool readsNoNumberFromHoles = copied.isDense() || !m_containers[source].possibleMutations.containsAny({ ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype });
                areBounded &= copied.everyStoreIsKnown() && readsNoNumberFromHoles && !(made.mayRunCodeWhileCopying && copied.has(ArrayWrites::MayGetShorter));
                numbers.unite(copied.storedNumbers);
            });
            if (!areBounded)
                writes.add(ArrayWrites::HasOtherWriter);
            didChange |= !areBounded || numbers.mayBeOtherThanIntegers != writes.storedNumbers.mayBeOtherThanIntegers || numbers.integers != writes.storedNumbers.integers;
            writes.storedNumbers = numbers;
        }
        for (const ElementStoredAgain& stored : m_elementsStoredAgain) {
            ArrayWrites& writes = m_arrayWrites[stored.array];
            if (!writes.everyStoreIsKnown())
                continue;
            ValueFlowNumbers numbers = writes.storedNumbers;
            bool areBounded = stored.arraysRead != ValueSets::nothing;
            forEachContainerIn(maker(), stored.arraysRead, m_arrays, [&](uint32_t source) {
                areBounded &= m_arrayWrites[source].everyStoreIsKnown() && !m_containers[source].possibleMutations.containsAny({ ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype });
                numbers.unite(m_arrayWrites[source].storedNumbers);
            });
            if (!areBounded)
                numbers.unite(stored.numbersOfRead);
            didChange |= numbers.mayBeOtherThanIntegers != writes.storedNumbers.mayBeOtherThanIntegers || numbers.integers != writes.storedNumbers.integers;
            writes.storedNumbers = numbers;
        }
    }
}

void ContainerAnalysis::collectArrayWrites()
{
    constexpr uint32_t containersPerTask = 1024;
    constexpr uint32_t functionsPerTask = 32;
    const ParallelLoops& loops = m_inputs.loops();
    m_kindsOfBoundedNumbers = maker().of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::Integer), ValueFlowUniverse::primitive(PrimitiveSet::WholeNumber), ValueFlowUniverse::primitive(PrimitiveSet::FractionalNumber) });
    m_valuesBehindChecks = Vector<std::atomic<BitVector*>>(program().functions.size());
    for (std::atomic<BitVector*>& known : m_valuesBehindChecks)
        known.store(nullptr, std::memory_order_relaxed);

    forEachContainerIn(maker(), m_inputs.solver().valueOf(m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::PairsOfEntries)), m_arrays, [&](uint32_t pair) {
        m_arrayWrites[pair].add(ArrayWrites::IsCreatedDense | ArrayWrites::IsPairOfEntries);
    });
    uint32_t numberOfContainers = m_containers.size();
    forEachIndexInParallel(loops, (numberOfContainers + containersPerTask - 1) / containersPerTask, [&](unsigned task) {
        for (uint32_t index = task * containersPerTask; index < std::min(numberOfContainers, (task + 1) * containersPerTask); ++index) {
            ValueFlowObjectID object = m_containers[index].object;
            if (m_inputs.universe().kindOf(object) == ValueFlowObjectKind::RestArray)
                m_arrayWrites[index].add(ArrayWrites::IsCreatedDense);
            // No statement stands for a literal without elements: nothing flows into it where it is made.
            const ValueFlowSite* site = m_inputs.universe().siteOf(object);
            if (!site || site->origin != ValueFlowSiteOrigin::Operation || site->ordinal)
                continue;
            const Value& creation = program().at(m_inputs.universe().functionOf(object)).at(site->value);
            if (creation.opcode == Opcode::NewArray) {
                m_arrayWrites[index].add(ArrayWrites::IsLiteral);
                m_arrayWrites[index].set(ArrayWrites::IsCreatedDense, creation.holes().empty());
            }
        }
    });

    std::span<const ValueFlowStatement> statements = m_inputs.bodies().statements();
    std::span<const ValueFlowBodyRange> ranges = m_inputs.bodies().ranges();
    uint32_t numberOfFunctions = program().functions.size();
    forEachIndexInParallel(loops, (numberOfFunctions + functionsPerTask - 1) / functionsPerTask, [&](unsigned task, unsigned worker) {
        Walker& walker = walkerOf(worker);
        for (uint32_t function = task * functionsPerTask; function < std::min(numberOfFunctions, (task + 1) * functionsPerTask); ++function) {
            if (!m_inputs.isReached(FunctionID(function)))
                continue;
            for (uint32_t index = ranges[function].statement; index < ranges[function + 1].statement; ++index) {
                if (statements[index].kind == ValueFlowStatementKind::Operation)
                    noteOperation(walker, FunctionID(function), statements[index]);
            }
        }
    });
    for (std::unique_ptr<Walker>& walker : m_walkers) {
        if (!walker)
            continue;
        walker->relations.reset();
        walker->analyses = nullptr;
    }

    m_builtinsThatWriteOperands = findBuiltinsThatWriteOperands(builtins());
    forEachBuiltinApplicationOnAllThreads(m_inputs, m_heap, *m_makers, m_builtinsThatWriteOperands, m_allContainers, [&](unsigned worker, const ValueFlowBuiltinApplication& application) {
        noteApplication(walkerOf(worker), application);
    });

    BitVector builtinsThatMakeArrays = builtinsWithOverload(builtins(), [&](const BuiltinOverload& overload) {
        return !!findArrayMadeBy(builtins(), overload);
    });
    forEachBuiltinApplicationOnAllThreads(m_inputs, m_heap, *m_makers, builtinsThatMakeArrays, ValueSets::anything, [&](unsigned worker, const ValueFlowBuiltinApplication& application) {
        noteArrayMadeBy(walkerOf(worker), application);
    });
    for (std::unique_ptr<Walker>& walker : m_walkers) {
        if (!walker)
            continue;
        m_objectsWithReplacedContents = maker().unite(m_objectsWithReplacedContents, walker->objectsWithReplacedContents);
        m_arraysMadeByBuiltins.appendVector(std::exchange(walker->arraysMadeByBuiltins, { }));
        m_elementsStoredAgain.appendVector(std::exchange(walker->elementsStoredAgain, { }));
    }
    // A read from anything but arrays that are followed, a view of bytes for one, says no more than its type.
    HashMap<uint64_t, ValueSetID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> receiversOfRead;
    auto keyOf = [](FunctionID function, ValueID read) {
        return static_cast<uint64_t>(function.index()) << 32 | read.index();
    };
    for (std::unique_ptr<Walker>& walker : m_walkers) {
        if (!walker)
            continue;
        for (const ElementRead& read : std::exchange(walker->elementReads, { }))
            receiversOfRead.add(keyOf(read.function, read.read), read.receivers);
    }
    for (ElementStoredAgain& stored : m_elementsStoredAgain) {
        ValueSetID objects = maker().intersect(receiversOfRead.get(keyOf(stored.function, stored.read)), ValueSets::complement(m_inputs.universe().primitives()));
        if (sets().isSubset(objects, m_arrays))
            stored.arraysRead = objects;
    }
    settleWhichResultsAreCreatedDense();
    settleNumbersOfCopies();

    forEachIndexInParallel(loops, (numberOfFunctions + containersPerTask - 1) / containersPerTask, [&](unsigned task) {
        for (uint32_t function = task * containersPerTask; function < std::min(numberOfFunctions, (task + 1) * containersPerTask); ++function)
            delete m_valuesBehindChecks[function].load(std::memory_order_relaxed);
    });
    m_valuesBehindChecks = { };
}

// The type of a container names what the containers in it hold, so those come first. Containers that hold each other say nothing of each other, and neither do those of the last level.
Vector<Vector<uint32_t>> ContainerAnalysis::scheduleConversions()
{
    AOT_THRESHOLD(uint32_t, lastLevel, 16);
    constexpr uint32_t containersPerTask = 256;
    uint32_t numberOfContainers = m_containers.size();
    Vector<DependencyList> dependencies(numberOfContainers);
    {
        ValueSetMakers makers(m_inputs);
        forEachIndexInParallel(m_inputs.loops(), (numberOfContainers + containersPerTask - 1) / containersPerTask, [&](unsigned task, unsigned worker) {
            for (uint32_t index = task * containersPerTask; index < std::min(numberOfContainers, (task + 1) * containersPerTask); ++index) {
                for (ValueSetID stored : m_containers[index].stored) {
                    if (ValueSets::isComplement(stored))
                        continue;
                    forEachContainerIn(makers.of(worker), stored, m_allContainers, [&](uint32_t dependency) {
                        dependencies[index].append(dependency);
                    });
                }
                AOT::sort(dependencies[index]);
                removeRepeatedElements(dependencies[index]);
            }
        });
    }
    StronglyConnectedComponents components = computeStronglyConnectedComponents(m_containers.size(), [&](unsigned index) -> const DependencyList& {
        return dependencies[index];
    });

    Vector<uint32_t> byComponent(m_containers.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::sort(byComponent, { }, [&](uint32_t index) {
        return std::pair { components.componentOf[index], index };
    });
    Vector<uint32_t> levelOfComponent;
    levelOfComponent.fill(0, components.numberOfComponents);
    uint32_t numberOfLevels = 0;
    for (uint32_t index : byComponent) {
        uint32_t component = components.componentOf[index];
        for (uint32_t dependency : dependencies[index]) {
            uint32_t dependencyComponent = components.componentOf[dependency];
            if (dependencyComponent != component)
                levelOfComponent[component] = std::max(levelOfComponent[component], std::min(levelOfComponent[dependencyComponent] + 1, lastLevel));
        }
        numberOfLevels = std::max(numberOfLevels, levelOfComponent[component] + 1);
    }

    Vector<Vector<uint32_t>> levels(numberOfLevels);
    for (uint32_t index = 0; index < m_containers.size(); ++index)
        levels[levelOfComponent[components.componentOf[index]]].append(index);
    return levels;
}

Type ContainerAnalysis::typeOfContents(ValueFlowPositions& positions, uint32_t index, const BuiltinSlot& slot)
{
    ValueFlowPositionResult result = positions.resultAt(Type::anything(), m_containers[index].stored[*slot.indexInType]);
    switch (result.proof) {
    case ValueFlowProof::NothingArrives:
        return Type::never();
    case ValueFlowProof::Proven:
        break;
    case ValueFlowProof::IllTyped:
    case ValueFlowProof::Untracked:
    case ValueFlowProof::OutsideFamily:
    case ValueFlowProof::UndecidedLayout:
    case ValueFlowProof::Disabled:
        return Type::anything();
    }
    Type type = result.narrowed ? WTF::move(*result.narrowed) : Type::anything();

    // The sets know numbers by kind alone. Where every store is an operation, the types of what the operations store bound them too.
    const ArrayWrites& writes = m_arrayWrites[index];
    if (isArray(index) && writes.everyStoreIsKnown() && !writes.storedNumbers.isEmpty())
        writes.storedNumbers.narrow(type);
    if (m_containers[index].builtinClass == BuiltinClasses::Map && slot.contentsSlot == ContentsSlot::Value && m_inputs.numbersOfValues) {
        const ValueFlowNumbers* numbers = m_inputs.numbersOfValues->ofValuesOfMap(m_containers[index].object);
        if (numbers && !numbers->isEmpty())
            numbers->narrow(type);
    }
    return type;
}

void ContainerAnalysis::convertLevel(std::span<const uint32_t> containers)
{
    ValueFlowResultInputs inputsWithContainers = m_inputs;
    inputsWithContainers.containers = &m_results;

    // The containers of a level say nothing of each other.
    constexpr size_t containersPerTask = 1024;
    unsigned numberOfTasks = static_cast<unsigned>((containers.size() + containersPerTask - 1) / containersPerTask);
    TypeInterningPhase phase(m_types, numberOfTasks);
    ValueSetMakers makers(m_inputs);
    forEachIndexInParallel(m_inputs.loops(), numberOfTasks, [&](unsigned task, unsigned worker) {
        TypeArena arena(phase, task);
        ValueFlowPositions positions(inputsWithContainers, arena, m_counts, 0, &makers.of(worker));
        for (uint32_t index : containers.subspan(task * containersPerTask, std::min(containersPerTask, containers.size() - task * containersPerTask))) {
            const Container& container = m_containers[index];
            ValueFlowContainerFacts& facts = m_facts[index];
            facts.contents.clear();
            facts.contentsAreProven = !container.stored.isEmpty() && !container.possibleMutations.containsAny(unprovenContentsMutations);
            if (!facts.contentsAreProven)
                continue;
            facts.contents.grow(container.stored.size());
            forEachContentsSlot(builtins(), container.builtinClass, [&](const BuiltinSlot& slot) {
                facts.contents[*slot.indexInType] = arena.intern(typeOfContents(positions, index, slot));
            });
        }
    });
    TypeRemap remap = phase.finish(StringRemap { }, &m_inputs.loops());
    forEachIndexInParallel(m_inputs.loops(), numberOfTasks, [&](unsigned task) {
        for (uint32_t index : containers.subspan(task * containersPerTask, std::min(containersPerTask, containers.size() - task * containersPerTask))) {
            for (TypeID& contents : m_facts[index].contents)
                remap.apply(contents);
            m_results.set(m_containers[index].object, ValueFlowContainerFacts(m_facts[index]));
        }
    });
}

// Whoever reads an array out of the set relies on one storage for all that the set holds.
void ContainerAnalysis::noteSetOfArrays(Walker& walker, ValueSetID set)
{
    if (!walker.setsOfArraysSeen.add(set.index()).isNewEntry)
        return;
    ValueSetMaker& maker = walker.maker;
    ValueSetID arrays = maker.intersect(set, m_arrays);
    if (arrays == ValueSets::nothing)
        return;

    // What a built-in allocates at a call may be anything for the universe, which is made before the callees are known. The arrays among them are followed like the others.
    bool holdsOtherArray = ValueSets::isComplement(set) || sets().meet(set, m_otherArrays) || sets().meet(maker.intersect(set, ValueSets::complement(m_arrays)), m_inputs.universe().objectsThatMayBeAnything());
    // Beside an object of another kind the set may lie outside the type of its position, which then keeps the type that was written: its readers test the storage which that type declares.
    bool holdsObjectThatIsNoArray = holdsOtherArray || sets().meet(maker.intersect(set, ValueSets::complement(m_arrays)), m_inputs.universe().objects());
    uint16_t flags = (holdsOtherArray ? ArrayWrites::IsPoisoned : 0) | (holdsObjectThatIsNoArray ? ArrayWrites::MeetsObjectThatIsNoArray : 0);
    std::optional<uint32_t> first;
    forEachContainerIn(maker, arrays, m_arrays, [&](uint32_t array) {
        m_arrayWrites[array].add(flags);
        if (!first) {
            first = array;
            return;
        }
        m_componentsOfArrays.join(*first, array);
    });
}

// Compiled code takes an array out of a value of a body or out of a position of the program.
void ContainerAnalysis::findComponents()
{
    const ValueFlowSolver& solver = m_inputs.solver();
    // The lists of all pairs are kept for rules, like nodes that a rule has made.
    ValueFlowNodeID pairsOfEntries = m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::PairsOfEntries);
    ValueFlowNodeID pairsOfSteps = m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::PairsOfSteps);
    constexpr uint32_t nodesPerShare = 1 << 12;
    uint32_t numberOfNodes = solver.input().numberOfNodes;
    forEachIndexInParallel(m_inputs.loops(), (numberOfNodes + nodesPerShare - 1) / nodesPerShare, [&](unsigned share, unsigned worker) {
        Walker& walker = walkerOf(worker);
        for (uint32_t index = share * nodesPerShare; index < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++index) {
            if (ValueFlowNodeID(index) == pairsOfEntries || ValueFlowNodeID(index) == pairsOfSteps)
                continue;
            ValueSetID set = solver.valueOf(ValueFlowNodeID(index));
            if (set != ValueSets::nothing && walker.setsWithArrays.contains(set))
                noteSetOfArrays(walker, set);
        }
    });
}

static bool mayGetDoubleStorage(ArrayStorage storage, const Type& elements)
{
    return storage == ArrayStorage::Int32 || (storage == ArrayStorage::Contiguous && elements.primitives & Primitive::Number);
}

// A creator allocates the storage that fits what the type of its array says the array holds, so all the arrays of a component get one type for their elements.
void ContainerAnalysis::listMembersOfComponents()
{
    m_membersOfComponent = Vector<Vector<uint32_t>>(m_containers.size());
    for (uint32_t index = 0; index < m_containers.size(); ++index) {
        if (isArray(index))
            m_membersOfComponent[m_componentsOfArrays.componentOf(index)].append(index);
    }
}

void ContainerAnalysis::shareStorageWithinComponents()
{
    const Vector<Vector<uint32_t>>& membersOfComponent = m_membersOfComponent;
    for (uint32_t index = 0; index < m_containers.size(); ++index) {
        if (isArray(index))
            m_facts[index].arrayStorage = m_facts[index].arrayStorageToldToCreator = ArrayStorage::Other;
    }

    TypeInterningPhase phase(m_types, 1);
    Vector<std::pair<uint32_t, TypeID>> elementsOfComponent;
    {
        TypeArena arena(phase, 0);
        for (uint32_t component = 0; component < membersOfComponent.size(); ++component) {
            const Vector<uint32_t>& members = membersOfComponent[component];
            bool isClean = !members.isEmpty() && std::ranges::all_of(members, [&](uint32_t array) {
                return !m_arrayWrites[array].has(ArrayWrites::IsPoisoned) && m_facts[array].contentsAreProven && m_arrayWrites[array].keepsStorageOfLiteral();
            });
            if (!isClean)
                continue;
            TypeID elements = m_facts[members[0]].contents[m_indexOfElements];
            for (uint32_t array : members.span().subspan(1))
                elements = arena.unionOf(elements, m_facts[array].contents[m_indexOfElements]);
            elementsOfComponent.append({ component, elements });
        }
    }
    TypeRemap remap = phase.finish(StringRemap { }, &m_inputs.loops());
    for (auto [component, elements] : elementsOfComponent) {
        remap.apply(elements);
        const Type& type = m_types.at(elements);
        // One array of the component turns into the storage that holds everything sooner or later, so all of them begin with it: their creators are told.
        bool holdsMoreThanNumbers = std::ranges::any_of(membersOfComponent[component], [&](uint32_t array) {
            return m_arrayWrites[array].has(ArrayWrites::IsGivenMoreThanNumbers);
        });
        ArrayStorage storage = holdsMoreThanNumbers ? ArrayStorage::Contiguous : arrayStorageHolding(type);
        if (!hasArrayStorageForm(storage) || !canStoreWithoutConversion(storage, type))
            continue;
        // A reader that expects fractions converts an array that fails its test and holds nothing but numbers: ConvertToDoubleStorage.
        bool mayBeConverted = mayGetDoubleStorage(storage, type) && std::ranges::any_of(membersOfComponent[component], [&](uint32_t array) {
            return m_arrayWrites[array].has(ArrayWrites::MeetsObjectThatIsNoArray);
        });
        if (mayBeConverted)
            continue;
        for (uint32_t array : membersOfComponent[component]) {
            m_facts[array].contents[m_indexOfElements] = elements;
            m_facts[array].arrayStorage = storage;
            m_facts[array].arrayStorageToldToCreator = storage == arrayStorageAtAllocation(type) ? ArrayStorage::Other : storage;
        }
    }
    // The arrays of a component share a kind because a test for numbers that fails may convert the array that it was asked of.
    for (uint32_t index = 0; index < m_containers.size(); ++index) {
        if (!isArray(index) || m_facts[index].arrayStorage != ArrayStorage::Other || !m_facts[index].contentsAreProven || !m_arrayWrites[index].keepsStorageOfLiteral())
            continue;
        const Type& elements = m_types.at(m_facts[index].contents[m_indexOfElements]);
        if (arrayStorageHolding(elements) == ArrayStorage::Contiguous && !mayGetDoubleStorage(ArrayStorage::Contiguous, elements))
            m_facts[index].arrayStorage = ArrayStorage::Contiguous;
    }
    for (uint32_t index = 0; index < m_containers.size(); ++index) {
        if (isArray(index))
            m_results.set(m_containers[index].object, ValueFlowContainerFacts(m_facts[index]));
    }
}

bool ContainerAnalysis::isUnchangedHolder(BuiltinID holder) const
{
    const FlowBuiltinTables& locations = m_tables.builtins;
    if (builtins().hasLockedPrototypeLink(holder))
        return true;
    if (!locations.intactHolders.get(holder.index()))
        return false;
    auto locationsOfHolder = std::ranges::equal_range(locations.locations, holder, { }, &BuiltinLocationFlow::holder);
    return std::ranges::all_of(locationsOfHolder, [](const BuiltinLocationFlow& location) {
        return location.state == BuiltinLocationState::Constant;
    });
}

// Visits the holders from the first one up its prototype chain until the functor answers Done. False when a holder may have changed or the table does not name the next one.
bool ContainerAnalysis::walkUnchangedChain(BuiltinID first, NOESCAPE const Invocable<IterationStatus(BuiltinID)> auto& functor) const
{
    BuiltinID holder = builtins().canonicalID(first);
    while (true) {
        if (functor(holder) == IterationStatus::Done)
            return true;
        if (!isUnchangedHolder(holder))
            return false;
        const Builtin& object = builtins().at(holder);
        switch (object.prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return false;
        case BuiltinPrototypeLink::Null:
            return true;
        case BuiltinPrototypeLink::Builtin:
            holder = builtins().canonicalID(object.inheritsFrom);
            break;
        }
    }
}

static bool isGeneratorClass(BuiltinClassID builtinClass)
{
    return builtinClass == BuiltinClasses::Generator || builtinClass == BuiltinClasses::AsyncGenerator;
}

void ContainerAnalysis::lookAtClassesOfContainers()
{
    BitVector hasContainers(builtins().classCount());
    for (const Container& container : m_containers)
        hasContainers.quickSet(container.builtinClass.index());
    for (size_t index : hasContainers) {
        BuiltinClassID builtinClass(static_cast<uint32_t>(index));
        const BuiltinClass& instanceClass = builtins().at(builtinClass);
        // Every generator function has a prototype object of its own, so start-up marks no Structure for its generators and the table says that the test at run time never passes. A proof needs no test.
        bool canBeProven = isGeneratorClass(builtinClass) || !instanceClass.isNeverPristine || canOnlyBeProvenPristine(builtins(), builtinClass);
        bool inheritsUnchangedMembers = instanceClass.prototype && !instanceClass.isPrimitiveWrapper && canBeProven && walkUnchangedChain(instanceClass.prototype, [](BuiltinID) {
            return IterationStatus::Continue;
        });
        m_classInheritsUnchangedMembers.quickSet(index, inheritsUnchangedMembers);

        bool hasDefaultSpecies = true;
        if (BuiltinID constructor = instanceClass.constructor) {
            bool isConstant = true;
            bool isChainUnchanged = walkUnchangedChain(constructor, [&](BuiltinID holder) {
                BuiltinID species = builtins().findOwnMember(holder, BuiltinNames::speciesSymbol);
                if (!species)
                    return IterationStatus::Continue;
                isConstant = m_tables.builtins.constantMembers.get(species.index());
                return IterationStatus::Done;
            });
            hasDefaultSpecies = isChainUnchanged && isConstant;
        }
        m_classHasDefaultSpecies.quickSet(index, hasDefaultSpecies);
    }
}

bool ContainerAnalysis::inheritsUnchangedMembers(ValueSetMaker& maker, uint32_t index) const
{
    const Container& container = m_containers[index];
    if (!m_classInheritsUnchangedMembers.quickGet(container.builtinClass.index()))
        return false;

    ValueSetID instance = maker.of(container.object);
    // Where the property that is asked for a prototype holds no object, the instance gets the prototype object of its class.
    ValueSetID link = maker.intersect(m_writtenPrototypeLinks[index], m_inputs.universe().objects());
    // Between a generator and the prototype object of its class lies the prototype property of the function, which the program can fill or replace.
    if (isGeneratorClass(container.builtinClass))
        return sets().isSubset(link, instance);

    // The rule that applies a built-in writes the link of what the built-in allocates: what new.target or the receiver says, and the unknown object where the instance may have names that the table does not declare.
    const ValueFlowSite* site = m_inputs.universe().siteOf(container.object);
    if (!site || site->origin != ValueFlowSiteOrigin::BuiltinResult)
        return true;
    Vector<ValueFlowObjectID, 32> prototypeObjects;
    m_inputs.universe().appendObjectsOf(builtins().at(container.builtinClass).prototype, prototypeObjects);
    return sets().isSubset(link, maker.of(prototypeObjects.span()));
}

// A match converts lastIndex to a length, and the conversion of anything but a number can run code of the program or throw.
bool ContainerAnalysis::keepsPatternAndNumericLastIndex(ValueSetMaker& maker, const Container& container) const
{
    if (sets().contains(m_objectsWithReplacedContents, container.object))
        return false;
    ValueSetID object = maker.of(container.object);
    ValueSetID stored = ValueSets::nothing;
    for (ValueFlowName name : { ValueFlowName::property(program().keyOf(BuiltinNames::lastIndex)), ValueFlowName { ValueFlowNameKind::Slot, BuiltinSlots::RegExpLastIndex.index() },
        ValueFlowName::of(ValueFlowNameKind::UnknownKey), ValueFlowName::of(ValueFlowNameKind::InternalFields) })
        stored = maker.unite(stored, m_heap.writtenUnder(maker, object, name));
    return sets().isSubset(stored, m_inputs.universe().builtinTables().heap().numberKinds());
}

// Storage says that the elements lie as the storage kind says and that no access under a number meets an accessor.
OptionSet<PristineAspects> ContainerAnalysis::provenPristineAspects(ValueSetMaker& maker, uint32_t index) const
{
    const Container& container = m_containers[index];
    OptionSet<ObjectMutation> impossibleMutations = allObjectMutations - container.possibleMutations;
    OptionSet<PristineAspects> aspects;
    if (!inheritsUnchangedMembers(maker, index))
        return aspects;
    if (isArray(index) && !impossibleMutations.contains(ObjectMutation::ChangeIntegrity))
        return aspects;
    if (impossibleMutations.containsAll(elementStorageMutations))
        aspects.add(PristineAspects::Storage);
    if (isArray(index) && aspects.contains(PristineAspects::Storage) && m_arrayWrites[index].isDense())
        aspects.add(PristineAspects::Dense);
    if (impossibleMutations.containsAll(builtinBehaviorMutations) && m_classHasDefaultSpecies.quickGet(container.builtinClass.index()))
        aspects.add(PristineAspects::Names);
    if (container.builtinClass != BuiltinClasses::RegExp || !aspects.contains(PristineAspects::Names))
        return aspects;
    if (keepsPatternAndNumericLastIndex(maker, container))
        aspects.add(PristineAspects::AsCreated);
    return aspects;
}

void ContainerAnalysis::collectWrites()
{
    constexpr uint32_t containersPerTask = 256;
    PhaseTimes times(m_inputs.build, PassID::AnalyzeFlow);
    collectContainers();
    times.didFinish("writes to containers: containers"_s);
    m_makers.emplace(m_inputs);
    collectStoredValues();
    times.didFinish("writes to containers: stored values"_s);

    uint32_t numberOfContainers = m_containers.size();
    m_componentsOfArrays = ConcurrentComponents(numberOfContainers);
    collectArrayWrites();
    times.didFinish("writes to containers: writes of arrays"_s);
    findComponents();
    times.didFinish("writes to containers: components"_s);

    lookAtClassesOfContainers();
    forEachIndexInParallel(m_inputs.loops(), (numberOfContainers + containersPerTask - 1) / containersPerTask, [&](unsigned task, unsigned worker) {
        for (uint32_t index = task * containersPerTask; index < std::min(numberOfContainers, (task + 1) * containersPerTask); ++index)
            m_facts[index].pristine = provenPristineAspects(m_makers->of(worker), index);
    });
    for (std::unique_ptr<Walker>& walker : m_walkers)
        walker = nullptr;
    m_makers.reset();
    times.didFinish("writes to containers: pristine aspects"_s);

    m_levelsOfConversions = scheduleConversions();
    listMembersOfComponents();
    times.didFinish("writes to containers: conversions scheduled"_s);
}

ValueFlowContainerResults ContainerAnalysis::convertContents()
{
    // The second round gives the type of a container the storage of the arrays in it.
    for (unsigned round = 0; round < 2; ++round) {
        for (const Vector<uint32_t>& level : m_levelsOfConversions)
            convertLevel(level.span());
        shareStorageWithinComponents();
    }

    uint64_t numberOfProvenContainers = std::ranges::count_if(m_facts, [](const ValueFlowContainerFacts& facts) {
        return facts.contentsAreProven;
    });
    m_inputs.build.statistics().count(StatisticID::FlowProvenContentsSites, numberOfProvenContainers);
    return WTF::move(m_results);
}

struct ValueFlowWritesToContainers {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowWritesToContainers);

    ValueFlowWritesToContainers(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, TypeTable& types)
        : analysis(inputs, tables, types)
    {
    }

    ContainerAnalysis analysis;
};

void ValueFlowWritesToContainersDeleter::operator()(ValueFlowWritesToContainers* writes) const
{
    delete writes;
}

std::unique_ptr<ValueFlowWritesToContainers, ValueFlowWritesToContainersDeleter> collectWritesToContainers(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, TypeTable& types)
{
    std::unique_ptr<ValueFlowWritesToContainers, ValueFlowWritesToContainersDeleter> writes(new ValueFlowWritesToContainers(inputs, tables, types));
    writes->analysis.collectWrites();
    return writes;
}

ValueFlowContainerResults computeContainerResults(ValueFlowWritesToContainers& writes)
{
    return writes.analysis.convertContents();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

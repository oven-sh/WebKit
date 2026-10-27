/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinPristine.h"
#include "AOTCFG.h"
#include "AOTClaimsOfFields.h"
#include "AOTConcurrentComponents.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIntegerRelations.h"
#include "AOTKnownClosures.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTTypeArena.h"
#include "AOTValueFlowHeapTables.h"
#include "AOTValueFlowPositions.h"
#include <atomic>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<ObjectMutation> reachMutations { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript };
static constexpr OptionSet<ObjectMutation> elementStorageMutations { ObjectMutation::ReachUnknownJavaScript, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype };
static constexpr OptionSet<ObjectMutation> builtinBehaviorMutations = elementStorageMutations | ObjectMutation::AddProperty;
static constexpr OperandUses usesWritingObject = operandWritingUses | OperandUses { OperandUse::DefinesName, OperandUse::TakesDescriptor };

namespace {

struct Container {
    ValueFlowObjectID object;
    BuiltinClassID builtinClass;
    OptionSet<ObjectMutation> possibleMutations;
    Vector<ValueSetID, 2> stored;
    ValueSetID writtenPrototypeLinks { ValueSets::nothing };
};

struct ArrayWrites {
    enum Flag : uint16_t {
        IsCreatedDense = 1 << 0,
        IsLiteral = 1 << 1,
        IsPairOfEntries = 1 << 2,
        IsInData = 1 << 3,
        MayGetHole = 1 << 4,
        IsStoredWithinLength = 1 << 5,
        MayGetShorter = 1 << 6,
        ElementsAreMoved = 1 << 7,
        HasOtherWriter = 1 << 8,
        MayBeMadeWithHoles = 1 << 9,
        IsPoisoned = 1 << 10,
        MeetsObjectThatIsNoArray = 1 << 11,
        HoldsWhatIsCopied = 1 << 12,
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

    bool getsNoHole() const { return !has(MayGetHole) && !(has(IsStoredWithinLength) && has(MayGetShorter)); }
    bool isDense() const { return has(IsCreatedDense) && getsNoHole(); }
    bool everyStoreIsKnown() const { return (has(IsLiteral) || has(IsInData) || has(HoldsWhatIsCopied)) && !has(HasOtherWriter); }
    bool keepsStorageOfLiteral() const { return (has(IsLiteral) || has(IsPairOfEntries)) && getsNoHole() && !has(ElementsAreMoved); }

    std::atomic<uint16_t> flags { 0 };
    ValueFlowNumbers storedNumbers;
};

struct ArrayMadeByBuiltin {
    uint32_t array;
    ValueSetID arraysWhoseHolesAreCopied;
    bool mayRunCodeWhileCopying { false };
    ValueSetID arraysWhoseElementsAreCopied { ValueSets::nothing };
};

struct ElementStoredAgain {
    uint32_t array;
    FunctionID function;
    ValueID read;
    ValueFlowNumbers numbersOfRead;
    ValueSetID arraysRead { ValueSets::nothing };
};

struct ElementRead {
    FunctionID function;
    ValueID read;
    ValueSetID receivers;
};

struct ContainerWalker {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ContainerWalker);

    ContainerWalker(ValueSetMaker& maker, const ValueSets& sets, ValueSetID containers, ValueSetID arrays)
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
    FunctionID analyzedFunction;
    std::unique_ptr<FunctionAnalyses> analyses;
    std::optional<IntegerRelations> relations;
    Vector<std::pair<ValueID, ValueID>> lengthReads;
    FunctionID functionOfValuesBehindChecks;
    BitVector valuesBehindChecks;
};

} // anonymous namespace

static void forEachContentsSlot(const BuiltinTable& builtins, BuiltinClassID builtinClass, NOESCAPE const Invocable<void(const BuiltinSlot&)> auto& functor)
{
    for (BuiltinSlotID id : builtins.at(builtinClass).slots) {
        if (builtins.at(id).indexInType)
            functor(builtins.at(id));
    }
}

static ValueFlowName nameOf(const BuiltinSlot& slot)
{
    bool isElement = slot.heap == AbstractHeap::Elements || slot.heap == AbstractHeap::TypedArrayElements;
    return isElement ? elementsName : ValueFlowName { ValueFlowNameKind::Slot, slot.id.index() };
}

static bool isGeneratorClass(BuiltinClassID builtinClass)
{
    return builtinClass == BuiltinClasses::Generator || builtinClass == BuiltinClasses::AsyncGenerator;
}

static const BuiltinTypePart* findArrayMadeBy(const BuiltinTable& builtins, const BuiltinOverload& overload)
{
    if (!overload.result.type || builtins.at(overload.result.type).freshness == BuiltinFreshness::No)
        return nullptr;
    const auto& parts = builtins.at(overload.result.type).parts;
    auto found = std::ranges::find_if(parts, [](const BuiltinTypePart& part) {
        return part.kind == BuiltinTypePartKind::Instance && part.builtinClass == BuiltinClasses::Array;
    });
    return found == parts.end() ? nullptr : &*found;
}

static void forEachParameter(const BuiltinOverload& overload, NOESCAPE const Invocable<void(const BuiltinParameter&)> auto& functor)
{
    if (overload.receiver)
        functor(*overload.receiver);
    std::ranges::for_each(overload.parameters, functor);
}

static bool mayGetDoubleStorage(ArrayStorage storage, const Type& elements)
{
    return storage == ArrayStorage::Int32 || (storage == ArrayStorage::Contiguous && elements.primitives & Primitive::Number);
}

static BuiltinClassID classOfCandidate(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const Program& program = inputs.program;
    const ValueFlowUniverse& universe = inputs.universe();
    // What native code makes where it calls a constructor of the table. Of a typed array nothing but its class and what is done to the object is asked.
    if (universe.kindOf(object) == ValueFlowObjectKind::ResultOfBuiltin) {
        BuiltinClassID builtinClass = inputs.classOf(object);
        return builtinClass && program.builtins().typedArrayClassOf(builtinClass) ? builtinClass : BuiltinClassID();
    }
    FunctionID function = universe.functionOf(object);
    if (!function || !inputs.isReached(function))
        return { };
    switch (universe.kindOf(object)) {
    case ValueFlowObjectKind::Site: {
        const ValueFlowSite& site = *universe.siteOf(object);
        BuiltinClassID builtinClass = inputs.classOf(object);
        if (site.layout || !builtinClass)
            return { };
        const Value& operation = program.at(function).at(site.value);
        if (site.origin == ValueFlowSiteOrigin::Operation && operation.opcode == Opcode::NewTemplateObject)
            return { };
        std::optional<unsigned> newTarget = site.origin == ValueFlowSiteOrigin::BuiltinResult ? OperandPosition::newTarget(operation.opcode) : std::nullopt;
        return newTarget && operation.operand(*newTarget) ? BuiltinClassID() : builtinClass;
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

struct ValueFlowWritesToContainers {
    WTF_MAKE_NONCOPYABLE(ValueFlowWritesToContainers);
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValueFlowWritesToContainers);

    using Walker = ContainerWalker;

    ValueFlowWritesToContainers(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, TypeTable& types)
        : m_inputs(inputs)
        , m_tables(tables)
        , m_types(types)
        , m_heap(inputs)
        , m_counts(nullptr, ValueFlowTypesOfSets::Change)
        , m_indexOfElements(indexOfArrayElementsInType(inputs.program.builtins()))
        , m_walkers(inputs.loops().width())
    {
        PhaseTimes times(m_inputs.build, PassID::AnalyzeFlow);
        collectContainers();
        times.didFinish("writes to containers: containers"_s);
        m_makers.emplace(m_inputs);
        collectStoredValues();
        times.didFinish("writes to containers: stored values"_s);
        m_componentsOfArrays = ConcurrentComponents(m_containers.size());
        collectArrayWrites();
        times.didFinish("writes to containers: writes of arrays"_s);
        findComponents();
        times.didFinish("writes to containers: components"_s);
        lookAtClassesOfContainers();
        forEachIndexInParallel(m_inputs.loops(), m_containers.size(), [&](unsigned index, unsigned worker) {
            m_facts[index].pristine = provenPristineAspects(m_makers->of(worker), index);
        });
        m_walkers.clear();
        times.didFinish("writes to containers: pristine aspects"_s);
        scheduleConversions();
        m_makers.reset();
        m_membersOfComponent = Vector<Vector<uint32_t>>(m_containers.size());
        for (uint32_t index = 0; index < m_containers.size(); ++index) {
            if (isArray(index))
                m_membersOfComponent[m_componentsOfArrays.componentOf(index)].append(index);
        }
        times.didFinish("writes to containers: conversions scheduled"_s);
    }

    ValueFlowContainerResults convertContents()
    {
        for (unsigned round = 0; round < 2; ++round) {
            for (const Vector<uint32_t>& level : m_levelsOfConversions)
                convertLevel(level.span());
            shareStorageWithinComponents();
        }
        m_inputs.build.statistics().count(StatisticID::FlowProvenContentsSites, std::ranges::count_if(m_facts, &ValueFlowContainerFacts::contentsAreProven));
        return WTF::move(m_results);
    }

private:
    const Program& program() const { return m_inputs.program; }
    const BuiltinTable& builtins() const { return m_inputs.program.builtins(); }
    const ValueFlowUniverse& universe() const { return m_inputs.universe(); }
    const ValueSets& sets() const { return m_inputs.sets(); }
    ValueSetMaker& maker() { return m_heap.maker(); }
    Lock& lockOf(uint32_t container) { return m_locks[container % m_locks.size()]; }
    bool isArray(uint32_t container) const { return m_containers[container].builtinClass == BuiltinClasses::Array; }

    Walker& walkerOf(unsigned worker)
    {
        if (!m_walkers[worker])
            m_walkers[worker] = makeUnique<Walker>(m_makers->of(worker), sets(), m_allContainers, m_arrays);
        return *m_walkers[worker];
    }

    std::optional<uint32_t> find(ValueFlowObjectID object) const
    {
        auto found = std::ranges::lower_bound(m_containers, object, { }, &Container::object);
        if (!object || found == m_containers.end() || found->object != object)
            return std::nullopt;
        return static_cast<uint32_t>(found - m_containers.begin());
    }

    void forEachContainerIn(ValueSetMaker& maker, ValueSetID set, ValueSetID among, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        sets().forEachMember(maker.intersect(set, among), [&](ValueFlowObjectID object) {
            functor(*find(object));
        });
    }

    void addToEachArray(ValueSetMaker& maker, ValueSetID arrays, uint16_t flags)
    {
        forEachContainerIn(maker, arrays, m_arrays, [&](uint32_t array) {
            m_arrayWrites[array].add(flags);
        });
    }

    void collectContainers()
    {
        m_containers = collectInParallel<Container>(m_inputs.loops(), universe().numberOfObjects(), [&](unsigned index, Vector<Container>& found) {
            BuiltinClassID builtinClass = classOfFollowedContainer(m_inputs, ValueFlowObjectID(index));
            if (!builtinClass)
                return;
            found.append(Container { ValueFlowObjectID(index), builtinClass, markedMutationsOf(m_inputs, ValueFlowObjectID(index)), { } });
            if (builtinClass == BuiltinClasses::Array || builtinClass == BuiltinClasses::Map || builtinClass == BuiltinClasses::Set || builtinClass == BuiltinClasses::Promise)
                found.last().stored.fill(ValueSets::nothing, builtins().numberOfSlotsInType(builtinClass));
        });
        Vector<ValueFlowObjectID> objects;
        Vector<ValueFlowObjectID> arrays;
        for (const Container& container : m_containers) {
            objects.append(container.object);
            if (container.builtinClass == BuiltinClasses::Array)
                arrays.append(container.object);
        }
        m_allContainers = maker().of(objects.span());
        m_arrays = maker().of(arrays.span());
        m_otherArrays = maker().intersect(universe().arrays(), ValueSets::complement(m_arrays));
        m_arrayWrites = Vector<ArrayWrites>(m_containers.size());
        m_facts = Vector<ValueFlowContainerFacts>(m_containers.size());
        m_results.setObjects(universe().numberOfObjects(), objects.span());
    }

    void collectStoredValues()
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
        forEachIndexInParallel(m_inputs.loops(), stretches.size(), [&](unsigned stretch, unsigned worker) {
            Walker& walker = walkerOf(worker);
            ValueFlowName name = stretches[stretch].name;
            for (const ValueFlowHeapIndex::WrittenNode& node : stretches[stretch].nodes) {
                if (!walker.setsWithContainers.contains(node.objects))
                    continue;
                forEachContainerIn(walker.maker, node.objects, m_allContainers, [&](uint32_t index) {
                    Container& container = m_containers[index];
                    Locker locker { lockOf(index) };
                    if (name == prototypeLinkName)
                        container.writtenPrototypeLinks = walker.maker.unite(container.writtenPrototypeLinks, node.value);
                    if (container.stored.isEmpty())
                        return;
                    forEachContentsSlot(builtins(), container.builtinClass, [&](const BuiltinSlot& slot) {
                        if (nameOf(slot) == name || mayBeTheSameName(name.kind, nameOf(slot).kind))
                            container.stored[*slot.indexInType] = walker.maker.unite(container.stored[*slot.indexInType], node.value);
                    });
                });
            }
        });
    }

    ValueFlowNumbers numbersOf(Walker& walker, FunctionID id, ValueID value)
    {
        const Function& function = program().at(id);
        const BitVector* behindChecks = m_inputs.bodies().valuesBehindChecks(id);
        if (!behindChecks) {
            if (std::exchange(walker.functionOfValuesBehindChecks, id) != id)
                walker.valuesBehindChecks = valuesTypedBehindChecks(program(), function, CFG(function));
            behindChecks = &walker.valuesBehindChecks;
        }
        if (behindChecks->get(value.index()))
            return m_inputs.numbersOfValues ? m_inputs.numbersOfValues->numbersOf(id, value) : ValueFlowNumbers::any();
        ValueFlowNumbers numbers;
        numbers.add(program().typeOf(function, value));
        return numbers;
    }

    void noteStoredOperand(Walker& walker, ValueSetID arrays, FunctionID function, ValueID stored)
    {
        const Function& body = program().at(function);
        ValueID definition = definitionThroughSameValues(body, stored);
        bool isReadFromArray = body.at(definition).opcode == Opcode::ElemGet;
        const Type& type = program().typeOf(body, stored);
        bool isNaN = body.at(definition).opcode == Opcode::Double && !canStoreWithoutConversion(ArrayStorage::Double, body.at(definition).number());
        if (isNaN || (!type.isNever() && type.hasOnlyPrimitives(Primitive::UndefinedOrNull)))
            addToEachArray(walker.maker, arrays, ArrayWrites::IsGivenMoreThanNumbers);
        std::optional<ValueFlowNumbers> numbers;
        forEachContainerIn(walker.maker, arrays, m_arrays, [&](uint32_t array) {
            if (!sets().meet(m_containers[array].stored[m_indexOfElements], m_kindsOfBoundedNumbers))
                return;
            if (!numbers)
                numbers = numbersOf(walker, function, stored);
            if (isReadFromArray) {
                walker.elementsStoredAgain.append({ array, function, definition, *numbers });
                return;
            }
            Locker locker { lockOf(array) };
            m_arrayWrites[array].storedNumbers.unite(*numbers);
        });
    }

    void noteData(const ValueFlowStatement& statement, const DataNode& root)
    {
        bool becomesLiterals = isExpandedForItsSizeAlone(root, m_inputs.build.limit(LimitID::MaxExpandedDataNodes));
        forEachDataNodeInPreorder(root, [&](uint32_t ordinal, const DataNode& node) {
            const auto* listed = node.tryAs<DataNode::Array>();
            std::optional<uint32_t> array = listed ? find(universe().siteObject(statement, ordinal)) : std::nullopt;
            if (!array)
                return;
            bool hasHole = false;
            ValueFlowNumbers numbers;
            for (const DataNode& element : listed->elements) {
                hasHole |= element.is<DataNode::Hole>();
                const auto* number = element.tryAs<DataNode::Number>();
                if (number && isTrackedInteger(number->value))
                    numbers.integers = numbers.integers.unionWith(Range::one(static_cast<int64_t>(number->value)));
                else if (number)
                    numbers.mayBeOtherThanIntegers = true;
            }
            ArrayWrites& writes = m_arrayWrites[*array];
            writes.add(ArrayWrites::IsInData);
            writes.set(ArrayWrites::IsLiteral, becomesLiterals);
            writes.set(ArrayWrites::IsCreatedDense, !hasHole);
            if (arrayStorageOfData(program(), *listed) == ArrayStorage::Contiguous)
                writes.add(ArrayWrites::IsGivenMoreThanNumbers);
            Locker locker { lockOf(*array) };
            writes.storedNumbers.unite(numbers);
        });
    }

    void noteUnknownWriter(Walker& walker, ValueSetID arrays)
    {
        ValueSetID others = walker.maker.intersect(arrays, ValueSets::complement(walker.arraysWithUnknownWriter));
        if (others == ValueSets::nothing)
            return;
        walker.arraysWithUnknownWriter = walker.maker.unite(walker.arraysWithUnknownWriter, others);
        addToEachArray(walker.maker, others, ArrayWrites::MayGetHole | ArrayWrites::MayGetShorter | ArrayWrites::ElementsAreMoved | ArrayWrites::HasOtherWriter);
    }

    bool isWithinLength(Walker& walker, FunctionID id, ValueID store)
    {
        const Function& function = program().at(id);
        ValueID position = function.at(store).operand(OperandIndex::ElemSet::property);
        if (!program().typeOf(function, position).isInteger())
            return false;

        if (std::exchange(walker.analyzedFunction, id) != id) {
            walker.relations.reset();
            walker.analyses = makeUnique<FunctionAnalyses>(AnalysisContext { m_inputs.build, program(), nullptr, nullptr, id, CallResults::NotFollowed }, function);
            walker.relations.emplace(*walker.analyses);
            walker.lengthReads.shrink(0);
            PropertyKeyID lengthKey = program().keyOf(BuiltinNames::length);
            unsigned receiver = *receiverOf(Opcode::PropGet);
            ValueSetID arraysOrUndefined = walker.maker.unite(universe().arrays(), universe().undefinedValue());
            for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(id.index())) {
                if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::PropGet || function.at(statement.site).key() != lengthKey)
                    continue;
                walker.lengthReads.append({ walker.analyses->identityOf(function.at(statement.site).operand(receiver)), statement.site });
                if (sets().isSubset(m_heap.possibleValuesOf(walker.maker, m_inputs.bodies().operandsOf(statement)[receiver]), arraysOrUndefined))
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

    void noteWriteByOperation(Walker& walker, FunctionID function, const ValueFlowStatement& statement, uint32_t operandIndex, ValueSetID arrays)
    {
        const Value& operation = program().at(function).at(statement.site);
        switch (statement.opcode) {
        case Opcode::AppendElement:
            noteStoredOperand(walker, arrays, function, operation.operand(OperandIndex::AppendElement::value));
            return;
        case Opcode::AppendSpread:
            addToEachArray(walker.maker, arrays, ArrayWrites::HasOtherWriter);
            return;
        case Opcode::ElemSet:
            if (operandIndex != OperandIndex::ElemSet::object)
                break;
            noteStoredOperand(walker, arrays, function, operation.operand(OperandIndex::ElemSet::value));
            addToEachArray(walker.maker, arrays, isWithinLength(walker, function, statement.site) ? ArrayWrites::IsStoredWithinLength : ArrayWrites::MayGetHole);
            return;
        case Opcode::PropSet:
        case Opcode::PropDefine:
            if (operation.key() != program().keyOf(BuiltinNames::length) && !program().isIndexLike(operation.key()))
                return;
            break;
        default:
            break;
        }
        noteUnknownWriter(walker, arrays);
    }

    void noteOperation(Walker& walker, FunctionID function, const ValueFlowStatement& statement)
    {
        std::span<const ValueFlowOperand> operands = m_inputs.bodies().operandsOf(statement);
        const Value& operation = program().at(function).at(statement.site);
        if (statement.opcode == Opcode::NewArray) {
            ValueFlowObjectID object = universe().siteObject(function, statement.site);
            if (std::optional<uint32_t> array = find(object)) {
                m_arrayWrites[*array].add(ArrayWrites::IsLiteral);
                m_arrayWrites[*array].set(ArrayWrites::IsCreatedDense, operation.holes().empty());
                for (uint32_t index = OperandIndex::NewArray::elements; index < operation.numberOfOperands(); ++index)
                    noteStoredOperand(walker, walker.maker.of(object), function, operation.operand(index));
            }
        }
        if (statement.opcode == Opcode::Data)
            noteData(statement, program().at(operation.data()).root);
        if (statement.opcode == Opcode::IteratorResume || statement.opcode == Opcode::AppendSpread) {
            if (std::optional<uint32_t> pair = find(universe().siteObject(function, statement.site)))
                m_arrayWrites[*pair].add(ArrayWrites::IsCreatedDense);
        }

        bool isCall = effectRuleOf(statement.opcode) == EffectRule::Call;
        for (uint32_t index = 0; index < operands.size(); ++index) {
            const ValueFlowOperand& operand = operands[index];
            bool mayHoldArray = (operand.constant && walker.setsWithArrays.contains(operand.constant)) || (operand.node && walker.setsWithArrays.contains(m_inputs.solver().possibleValuesOf(operand.node)));
            if (!mayHoldArray)
                continue;
            ValueSetID value = m_heap.possibleValuesOf(walker.maker, operand);
            noteSetOfArrays(walker, value);
            if (statement.opcode == Opcode::ElemGet && index == OperandIndex::ElemGet::object)
                walker.elementReads.append({ function, statement.site, value });
            ValueSetID arrays = walker.maker.intersect(value, m_arrays);
            bool storesByName = (statement.opcode == Opcode::PropSet || statement.opcode == Opcode::PropDefine) && index == *receiverOf(statement.opcode);
            if (arrays != ValueSets::nothing && !isCall && (storesByName || operandUsesOf(statement.opcode, index).containsAny(usesWritingObject)))
                noteWriteByOperation(walker, function, statement, index, arrays);
        }
    }

    void noteApplication(Walker& walker, const ValueFlowBuiltinApplication& application)
    {
        ValueSetMaker& maker = walker.maker;
        BuiltinID builtin = builtins().canonicalID(application.builtin);
        bool stores = builtin == builtins().canonicalID(Builtins::ArrayPrototypePush) || builtin == builtins().canonicalID(Builtins::ArrayPrototypeUnshift);
        bool takes = builtin == builtins().canonicalID(Builtins::ArrayPrototypePop) || builtin == builtins().canonicalID(Builtins::ArrayPrototypeShift);
        bool movesElements = builtin == builtins().canonicalID(Builtins::ArrayPrototypeUnshift) || builtin == builtins().canonicalID(Builtins::ArrayPrototypeShift);
        auto noteParameter = [&](const BuiltinParameter& parameter) {
            ValueSetID value = application.valueAt(parameter.operand, maker);
            if (parameter.roles.contains(BuiltinOperandRole::ReplacesContents))
                walker.objectsWithReplacedContents = maker.unite(walker.objectsWithReplacedContents, maker.intersect(value, m_allContainers));
            ValueSetID arrays = parameter.uses.containsAny(usesWritingObject) ? maker.intersect(value, m_arrays) : ValueSets::nothing;
            if (arrays == ValueSets::nothing)
                return;
            if (!parameter.operand.isReceiver() || application.mode() != BuiltinCallMode::Call || (!stores && !takes)) {
                noteUnknownWriter(walker, arrays);
                return;
            }
            bool spellsOutArguments = application.site && program().at(application.function).at(application.site).opcode == Opcode::Call;
            if (stores && spellsOutArguments) {
                const Value& call = program().at(application.function).at(application.site);
                for (uint32_t index = 2; index < call.numberOfOperands(); ++index)
                    noteStoredOperand(walker, arrays, application.function, call.operand(index));
            }
            addToEachArray(maker, arrays, (stores && !spellsOutArguments ? ArrayWrites::HasOtherWriter : 0) | (takes ? ArrayWrites::MayGetShorter : 0) | (movesElements ? ArrayWrites::ElementsAreMoved : 0));
        };
        for (const BuiltinOverload& overload : builtins().overloadsOf(application.builtin)) {
            if (overload.mode == application.mode())
                forEachParameter(overload, noteParameter);
        }
    }

    void noteArrayMadeBy(Walker& walker, const ValueFlowBuiltinApplication& application)
    {
        ValueSetMaker& maker = walker.maker;
        std::optional<uint32_t> array = find(application.site ? universe().siteObject(application.function, application.site) : universe().resultOf(application.builtin, 0));
        if (!array || !isArray(*array))
            return;

        static constexpr std::array copyingBuiltins { Builtins::ArrayPrototypeSlice, Builtins::ArrayPrototypeConcat, Builtins::ArrayPrototypeFilter, Builtins::ArrayPrototypeToReversed, Builtins::ArrayPrototypeToSorted, Builtins::ArrayPrototypeToSpliced, Builtins::ArrayPrototypeWith };
        bool spellsOutArguments = application.site && program().at(application.function).at(application.site).opcode == Opcode::Call;
        bool holdsWhatIsCopied = spellsOutArguments && application.mode() == BuiltinCallMode::Call && std::ranges::any_of(copyingBuiltins, [&](BuiltinID other) {
            return builtins().canonicalID(application.builtin) == builtins().canonicalID(other);
        });
        ValueSetID sources = ValueSets::nothing;
        ValueSetID copied = ValueSets::nothing;
        bool mayRunCode = false;
        auto noteParameter = [&](const BuiltinParameter& parameter) {
            ValueSetID objects = maker.intersect(application.valueAt(parameter.operand, maker), ValueSets::complement(universe().primitives()));
            if (parameter.operand.isReceiver() || parameter.roles.contains(BuiltinOperandRole::CopiesHolesToResult)) {
                holdsWhatIsCopied &= sets().isSubset(objects, m_arrays);
                sources = maker.unite(sources, objects);
            }
            if (!parameter.roles.contains(BuiltinOperandRole::CopiesHolesToResult))
                mayRunCode |= objects != ValueSets::nothing;
            else if (sets().isSubset(objects, m_arrays))
                copied = maker.unite(copied, objects);
            else
                m_arrayWrites[*array].add(ArrayWrites::MayBeMadeWithHoles);
        };
        for (const BuiltinOverload& overload : builtins().overloadsOf(application.builtin)) {
            const BuiltinTypePart* made = overload.mode == application.mode() ? findArrayMadeBy(builtins(), overload) : nullptr;
            if (!made)
                continue;
            if (!made->pristine.contains(PristineAspects::Dense) || overload.code.prefixForm) {
                m_arrayWrites[*array].add(ArrayWrites::MayBeMadeWithHoles);
                holdsWhatIsCopied = false;
            } else
                forEachParameter(overload, noteParameter);
        }
        forEachContainerIn(maker, copied, m_arrays, [&](uint32_t source) {
            mayRunCode |= m_containers[source].possibleMutations.containsAny({ ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype });
        });
        if (holdsWhatIsCopied) {
            const Value& call = program().at(application.function).at(application.site);
            for (uint32_t index = 2; index < call.numberOfOperands(); ++index)
                noteStoredOperand(walker, maker.of(m_containers[*array].object), application.function, call.operand(index));
        }
        m_arrayWrites[*array].add(holdsWhatIsCopied ? ArrayWrites::HoldsWhatIsCopied : ArrayWrites::HasOtherWriter);
        walker.arraysMadeByBuiltins.append({ *array, copied, mayRunCode, holdsWhatIsCopied ? sources : ValueSets::nothing });
    }

    void settleNumbersOfCopies(std::span<const ArrayMadeByBuiltin> arraysMadeByBuiltins, std::span<const ElementStoredAgain> elementsStoredAgain)
    {
        auto keepsPrototypeAndElements = [&](uint32_t array) {
            return !m_containers[array].possibleMutations.containsAny({ ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype });
        };
        auto differ = [](const ValueFlowNumbers& numbers, const ValueFlowNumbers& others) {
            return numbers.mayBeOtherThanIntegers != others.mayBeOtherThanIntegers || numbers.integers != others.integers;
        };
        for (bool didChange = true; std::exchange(didChange, false);) {
            for (const ArrayMadeByBuiltin& made : arraysMadeByBuiltins) {
                ArrayWrites& writes = m_arrayWrites[made.array];
                if (!writes.has(ArrayWrites::HoldsWhatIsCopied) || !writes.everyStoreIsKnown())
                    continue;
                ValueFlowNumbers numbers = writes.storedNumbers;
                bool areBounded = true;
                forEachContainerIn(maker(), made.arraysWhoseElementsAreCopied, m_arrays, [&](uint32_t source) {
                    const ArrayWrites& copied = m_arrayWrites[source];
                    bool readsNoNumberFromHoles = copied.isDense() || keepsPrototypeAndElements(source);
                    areBounded &= copied.everyStoreIsKnown() && readsNoNumberFromHoles && !(made.mayRunCodeWhileCopying && copied.has(ArrayWrites::MayGetShorter));
                    numbers.unite(copied.storedNumbers);
                });
                if (!areBounded)
                    writes.add(ArrayWrites::HasOtherWriter);
                didChange |= !areBounded || differ(numbers, writes.storedNumbers);
                writes.storedNumbers = numbers;
            }
            for (const ElementStoredAgain& stored : elementsStoredAgain) {
                ArrayWrites& writes = m_arrayWrites[stored.array];
                if (!writes.everyStoreIsKnown())
                    continue;
                ValueFlowNumbers numbers = writes.storedNumbers;
                bool areBounded = stored.arraysRead != ValueSets::nothing;
                forEachContainerIn(maker(), stored.arraysRead, m_arrays, [&](uint32_t source) {
                    areBounded &= m_arrayWrites[source].everyStoreIsKnown() && keepsPrototypeAndElements(source);
                    numbers.unite(m_arrayWrites[source].storedNumbers);
                });
                if (!areBounded)
                    numbers.unite(stored.numbersOfRead);
                didChange |= differ(numbers, writes.storedNumbers);
                writes.storedNumbers = numbers;
            }
        }
    }

    void collectArrayWrites()
    {
        const ParallelLoops& loops = m_inputs.loops();
        m_kindsOfBoundedNumbers = maker().of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::Integer), ValueFlowUniverse::primitive(PrimitiveSet::WholeNumber), ValueFlowUniverse::primitive(PrimitiveSet::FractionalNumber) });
        addToEachArray(maker(), m_inputs.solver().possibleValuesOf(m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::PairsOfEntries)), ArrayWrites::IsCreatedDense | ArrayWrites::IsPairOfEntries);
        forEachIndexInParallel(loops, m_containers.size(), [&](unsigned index) {
            ValueFlowObjectID object = m_containers[index].object;
            if (universe().kindOf(object) == ValueFlowObjectKind::RestArray)
                m_arrayWrites[index].add(ArrayWrites::IsCreatedDense);
            const ValueFlowSite* site = universe().siteOf(object);
            if (!site || site->origin != ValueFlowSiteOrigin::Operation || site->ordinal)
                return;
            const Value& creation = program().at(universe().functionOf(object)).at(site->value);
            if (creation.opcode == Opcode::NewArray) {
                m_arrayWrites[index].add(ArrayWrites::IsLiteral);
                m_arrayWrites[index].set(ArrayWrites::IsCreatedDense, creation.holes().empty());
            }
        });
        forEachIndexInParallel(loops, program().functions.size(), [&](unsigned function, unsigned worker) {
            if (!m_inputs.isReached(FunctionID(function)))
                return;
            for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(function)) {
                if (statement.kind == ValueFlowStatementKind::Operation)
                    noteOperation(walkerOf(worker), FunctionID(function), statement);
            }
        });

        auto writesOperand = [](const BuiltinParameter& parameter) {
            return parameter.roles.contains(BuiltinOperandRole::ReplacesContents) || parameter.uses.containsAny(usesWritingObject);
        };
        BitVector builtinsThatWriteOperands = builtinsWithOverload(builtins(), [&](const BuiltinOverload& overload) {
            return (overload.receiver && writesOperand(*overload.receiver)) || std::ranges::any_of(overload.parameters, writesOperand);
        });
        forEachBuiltinApplicationOnAllThreads(m_inputs, m_heap, *m_makers, builtinsThatWriteOperands, m_allContainers, [&](unsigned worker, const ValueFlowBuiltinApplication& application) {
            if (builtinsThatWriteOperands.quickGet(application.builtin.index()))
                noteApplication(walkerOf(worker), application);
        });
        BitVector builtinsThatMakeArrays = builtinsWithOverload(builtins(), [&](const BuiltinOverload& overload) {
            return !!findArrayMadeBy(builtins(), overload);
        });
        forEachBuiltinApplicationOnAllThreads(m_inputs, m_heap, *m_makers, builtinsThatMakeArrays, ValueSets::anything, [&](unsigned worker, const ValueFlowBuiltinApplication& application) {
            noteArrayMadeBy(walkerOf(worker), application);
        });
        Vector<ArrayMadeByBuiltin> arraysMadeByBuiltins;
        Vector<ElementStoredAgain> elementsStoredAgain;
        HashMap<uint64_t, ValueSetID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> receiversOfRead;
        auto keyOf = [](FunctionID function, ValueID read) {
            return static_cast<uint64_t>(function.index()) << 32 | read.index();
        };
        for (std::unique_ptr<Walker>& walker : m_walkers) {
            if (!walker)
                continue;
            walker->relations.reset();
            walker->analyses = nullptr;
            m_objectsWithReplacedContents = maker().unite(m_objectsWithReplacedContents, walker->objectsWithReplacedContents);
            arraysMadeByBuiltins.appendVector(std::exchange(walker->arraysMadeByBuiltins, { }));
            elementsStoredAgain.appendVector(std::exchange(walker->elementsStoredAgain, { }));
            for (const ElementRead& read : std::exchange(walker->elementReads, { }))
                receiversOfRead.add(keyOf(read.function, read.read), read.receivers);
        }
        for (ElementStoredAgain& stored : elementsStoredAgain) {
            ValueSetID objects = maker().intersect(receiversOfRead.get(keyOf(stored.function, stored.read)), ValueSets::complement(universe().primitives()));
            if (sets().isSubset(objects, m_arrays))
                stored.arraysRead = objects;
        }

        for (const ArrayMadeByBuiltin& made : arraysMadeByBuiltins)
            m_arrayWrites[made.array].set(ArrayWrites::IsCreatedDense, !m_arrayWrites[made.array].has(ArrayWrites::MayBeMadeWithHoles));
        for (bool didChange = true; std::exchange(didChange, false);) {
            for (const ArrayMadeByBuiltin& made : arraysMadeByBuiltins) {
                if (!m_arrayWrites[made.array].has(ArrayWrites::IsCreatedDense))
                    continue;
                bool copiesHole = false;
                forEachContainerIn(maker(), made.arraysWhoseHolesAreCopied, m_arrays, [&](uint32_t copied) {
                    copiesHole |= !m_arrayWrites[copied].isDense() || (made.mayRunCodeWhileCopying && m_arrayWrites[copied].has(ArrayWrites::MayGetShorter));
                });
                if (copiesHole) {
                    m_arrayWrites[made.array].set(ArrayWrites::IsCreatedDense, false);
                    didChange = true;
                }
            }
        }
        settleNumbersOfCopies(arraysMadeByBuiltins.span(), elementsStoredAgain.span());
    }

    void scheduleConversions()
    {
        AOT_THRESHOLD(uint32_t, lastLevel, 16);
        using DependencyList = Vector<uint32_t, 2>;
        Vector<DependencyList> dependencies(m_containers.size());
        forEachIndexInParallel(m_inputs.loops(), m_containers.size(), [&](unsigned index, unsigned worker) {
            for (ValueSetID stored : m_containers[index].stored) {
                if (ValueSets::isComplement(stored))
                    continue;
                forEachContainerIn(m_makers->of(worker), stored, m_allContainers, [&](uint32_t dependency) {
                    dependencies[index].append(dependency);
                });
            }
            AOT::sort(dependencies[index]);
            removeRepeatedElements(dependencies[index]);
        });
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
                if (components.componentOf[dependency] != component)
                    levelOfComponent[component] = std::max(levelOfComponent[component], std::min(levelOfComponent[components.componentOf[dependency]] + 1, lastLevel));
            }
            numberOfLevels = std::max(numberOfLevels, levelOfComponent[component] + 1);
        }
        m_levelsOfConversions = Vector<Vector<uint32_t>>(numberOfLevels);
        for (uint32_t index = 0; index < m_containers.size(); ++index)
            m_levelsOfConversions[levelOfComponent[components.componentOf[index]]].append(index);
    }

    Type typeOfContents(ValueFlowPositions& positions, uint32_t index, const BuiltinSlot& slot)
    {
        ValueFlowPositionResult result = positions.resultAmongAllValues(m_containers[index].stored[*slot.indexInType]);
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

    static OptionSet<Actor> actorsOpenOnContentsOf(const Container& container)
    {
        OptionSet<Actor> open;
        if (container.possibleMutations.contains(ObjectMutation::ReachUnknownJavaScript))
            open.add(Actor::UnseenCode);
        if (container.possibleMutations.contains(ObjectMutation::ReachNativeCode))
            open.add(Actor::NativeCode);
        return open;
    }

    static Vector<Write, 4> writesToContentsOf(const Container& container)
    {
        Vector<Write, 4> writes;
        for (ValueSetID stored : container.stored)
            writes.append(Write { Actor::Statements, WriteForm::Store, { }, stored });
        if (container.possibleMutations.contains(ObjectMutation::RedefineProperty))
            writes.append(Write { Actor::Statements, WriteForm::Define, { }, ValueSets::nothing });
        if (container.possibleMutations.contains(ObjectMutation::ChangePrototype))
            writes.append(Write { Actor::Statements, WriteForm::Link, { }, ValueSets::nothing });
        return writes;
    }

    void convertLevel(std::span<const uint32_t> containers)
    {
        ValueFlowResultInputs inputsWithContainers = m_inputs;
        inputsWithContainers.containers = &m_results;
        constexpr size_t containersPerTask = 1024;
        unsigned numberOfTasks = static_cast<unsigned>((containers.size() + containersPerTask - 1) / containersPerTask);
        auto containersOf = [&](unsigned task) {
            return containers.subspan(task * containersPerTask, std::min(containersPerTask, containers.size() - task * containersPerTask));
        };
        TypeInterningPhase phase(m_types, numberOfTasks);
        ValueSetMakers makers(m_inputs);
        forEachIndexInParallel(m_inputs.loops(), numberOfTasks, [&](unsigned task, unsigned worker) {
            TypeArena arena(phase, task);
            ValueFlowPositions positions(inputsWithContainers, arena, m_counts, 0, &makers.of(worker));
            for (uint32_t index : containersOf(task)) {
                const Container& container = m_containers[index];
                ValueFlowContainerFacts& facts = m_facts[index];
                facts.contents.clear();
                Vector<Write, 4> writes = writesToContentsOf(container);
                facts.contentsAreProven = !container.stored.isEmpty() && claimContentsAreWhatWasStored(Writers(writes.span(), actorsOpenOnContentsOf(container)));
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
            for (uint32_t index : containersOf(task)) {
                for (TypeID& contents : m_facts[index].contents)
                    remap.apply(contents);
                m_results.set(m_containers[index].object, ValueFlowContainerFacts(m_facts[index]));
            }
        });
    }

    void noteSetOfArrays(Walker& walker, ValueSetID set)
    {
        if (!walker.setsOfArraysSeen.add(set.index()).isNewEntry)
            return;
        ValueSetMaker& maker = walker.maker;
        ValueSetID arrays = maker.intersect(set, m_arrays);
        if (arrays == ValueSets::nothing)
            return;

        ValueSetID others = maker.intersect(set, ValueSets::complement(m_arrays));
        bool holdsOtherArray = ValueSets::isComplement(set) || sets().meet(set, m_otherArrays) || sets().meet(others, universe().objectsThatMayBeAnything());
        bool holdsObjectThatIsNoArray = holdsOtherArray || sets().meet(others, universe().objects());
        uint16_t flags = (holdsOtherArray ? ArrayWrites::IsPoisoned : 0) | (holdsObjectThatIsNoArray ? ArrayWrites::MeetsObjectThatIsNoArray : 0);
        std::optional<uint32_t> first;
        forEachContainerIn(maker, arrays, m_arrays, [&](uint32_t array) {
            m_arrayWrites[array].add(flags);
            if (first)
                m_componentsOfArrays.join(*first, array);
            else
                first = array;
        });
    }

    void findComponents()
    {
        const ValueFlowSolver& solver = m_inputs.solver();
        ValueFlowNodeID pairsOfEntries = m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::PairsOfEntries);
        ValueFlowNodeID pairsOfSteps = m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::PairsOfSteps);
        constexpr uint32_t nodesPerShare = 1 << 12;
        uint32_t numberOfNodes = solver.input().numberOfNodes;
        forEachIndexInParallel(m_inputs.loops(), (numberOfNodes + nodesPerShare - 1) / nodesPerShare, [&](unsigned share, unsigned worker) {
            Walker& walker = walkerOf(worker);
            for (uint32_t index = share * nodesPerShare; index < std::min(numberOfNodes, (share + 1) * nodesPerShare); ++index) {
                ValueSetID set = solver.possibleValuesOf(ValueFlowNodeID(index));
                if (ValueFlowNodeID(index) != pairsOfEntries && ValueFlowNodeID(index) != pairsOfSteps && set != ValueSets::nothing && walker.setsWithArrays.contains(set))
                    noteSetOfArrays(walker, set);
            }
        });
    }

    void shareStorageWithinComponents()
    {
        for (uint32_t index = 0; index < m_containers.size(); ++index) {
            if (isArray(index))
                m_facts[index].arrayStorage = ArrayStorage::Other;
        }

        TypeInterningPhase phase(m_types, 1);
        Vector<std::pair<uint32_t, TypeID>> elementsOfComponent;
        {
            TypeArena arena(phase, 0);
            for (uint32_t component = 0; component < m_membersOfComponent.size(); ++component) {
                const Vector<uint32_t>& members = m_membersOfComponent[component];
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
            bool holdsMoreThanNumbers = std::ranges::any_of(m_membersOfComponent[component], [&](uint32_t array) {
                return m_arrayWrites[array].has(ArrayWrites::IsGivenMoreThanNumbers);
            });
            ArrayStorage storage = holdsMoreThanNumbers ? ArrayStorage::Contiguous : arrayStorageHolding(type);
            if (!hasArrayStorageForm(storage) || !canStoreWithoutConversion(storage, type))
                continue;
            bool mayBeConverted = mayGetDoubleStorage(storage, type) && std::ranges::any_of(m_membersOfComponent[component], [&](uint32_t array) {
                return m_arrayWrites[array].has(ArrayWrites::MeetsObjectThatIsNoArray);
            });
            if (mayBeConverted)
                continue;
            for (uint32_t array : m_membersOfComponent[component]) {
                m_facts[array].contents[m_indexOfElements] = elements;
                m_facts[array].arrayStorage = storage;
            }
        }
        Vector<ValueFlowObjectID> arraysThatMustNotBeConverted;
        for (uint32_t index = 0; index < m_containers.size(); ++index) {
            if (!isArray(index))
                continue;
            if (m_facts[index].arrayStorage == ArrayStorage::Other && m_facts[index].contentsAreProven && m_arrayWrites[index].keepsStorageOfLiteral()) {
                const Type& elements = m_types.at(m_facts[index].contents[m_indexOfElements]);
                if (arrayStorageHolding(elements) == ArrayStorage::Contiguous && !mayGetDoubleStorage(ArrayStorage::Contiguous, elements))
                    m_facts[index].arrayStorage = ArrayStorage::Contiguous;
            }
            if (m_facts[index].arrayStorage != ArrayStorage::Other && mayGetDoubleStorage(m_facts[index].arrayStorage, m_types.at(m_facts[index].contents[m_indexOfElements])))
                arraysThatMustNotBeConverted.append(m_containers[index].object);
            m_results.set(m_containers[index].object, ValueFlowContainerFacts(m_facts[index]));
        }
        m_results.arraysThatMustNotBeConverted = maker().of(arraysThatMustNotBeConverted.span());
    }

    bool isUnchangedHolder(BuiltinID holder) const
    {
        if (builtins().hasLockedPrototypeLink(holder))
            return true;
        if (!m_tables.builtins.intactHolders.get(holder.index()))
            return false;
        return std::ranges::all_of(std::ranges::equal_range(m_tables.builtins.locations, holder, { }, &BuiltinLocationFlow::holder), [](const BuiltinLocationFlow& location) {
            return location.state == BuiltinLocationState::Constant;
        });
    }

    bool walkUnchangedChain(BuiltinID first, NOESCAPE const Invocable<IterationStatus(BuiltinID)> auto& functor) const
    {
        for (BuiltinID holder = builtins().canonicalID(first);; holder = builtins().canonicalID(builtins().at(holder).inheritsFrom)) {
            if (functor(holder) == IterationStatus::Done)
                return true;
            if (!isUnchangedHolder(holder) || builtins().at(holder).prototypeLink == BuiltinPrototypeLink::Unknown)
                return false;
            if (builtins().at(holder).prototypeLink == BuiltinPrototypeLink::Null)
                return true;
        }
    }

    void lookAtClassesOfContainers()
    {
        BitVector hasContainers;
        for (const Container& container : m_containers)
            hasContainers.set(container.builtinClass.index());
        for (size_t index : hasContainers) {
            BuiltinClassID builtinClass(static_cast<uint32_t>(index));
            const BuiltinClass& instanceClass = builtins().at(builtinClass);
            bool canBeProven = isGeneratorClass(builtinClass) || !instanceClass.isNeverPristine || canOnlyBeProvenPristine(builtins(), builtinClass);
            m_classInheritsUnchangedMembers.set(index, instanceClass.prototype && !instanceClass.isPrimitiveWrapper && canBeProven && walkUnchangedChain(instanceClass.prototype, [](BuiltinID) {
                return IterationStatus::Continue;
            }));

            bool isConstant = true;
            m_classHasDefaultSpecies.set(index, !instanceClass.constructor || (walkUnchangedChain(instanceClass.constructor, [&](BuiltinID holder) {
                BuiltinID species = builtins().findOwnMember(holder, BuiltinNames::speciesSymbol);
                if (!species)
                    return IterationStatus::Continue;
                isConstant = m_tables.builtins.constantMembers.get(species.index());
                return IterationStatus::Done;
            }) && isConstant));
        }
    }

    bool inheritsUnchangedMembers(ValueSetMaker& maker, const Container& container) const
    {
        if (!m_classInheritsUnchangedMembers.get(container.builtinClass.index()))
            return false;
        ValueSetID link = maker.intersect(container.writtenPrototypeLinks, universe().objects());
        if (isGeneratorClass(container.builtinClass))
            return sets().isSubset(link, maker.of(container.object));

        const ValueFlowSite* site = universe().siteOf(container.object);
        if (!site || site->origin != ValueFlowSiteOrigin::BuiltinResult)
            return true;
        Vector<ValueFlowObjectID, 32> prototypeObjects;
        universe().appendObjectsOf(builtins().at(container.builtinClass).prototype, prototypeObjects);
        return sets().isSubset(link, maker.of(prototypeObjects.span()));
    }

    bool keepsPatternAndNumericLastIndex(ValueSetMaker& maker, const Container& container) const
    {
        if (sets().contains(m_objectsWithReplacedContents, container.object))
            return false;
        ValueSetID stored = ValueSets::nothing;
        for (ValueFlowName name : { ValueFlowName::property(program().keyOf(BuiltinNames::lastIndex)), ValueFlowName { ValueFlowNameKind::Slot, BuiltinSlots::RegExpLastIndex.index() }, unknownKeyName, ValueFlowName::of(ValueFlowNameKind::InternalFields) })
            stored = maker.unite(stored, m_heap.writtenUnder(maker, maker.of(container.object), name));
        return sets().isSubset(stored, universe().builtinTables().heap().numberKinds());
    }

    OptionSet<PristineAspects> provenPristineAspects(ValueSetMaker& maker, uint32_t index) const
    {
        const Container& container = m_containers[index];
        OptionSet<PristineAspects> aspects;
        if (!inheritsUnchangedMembers(maker, container) || (isArray(index) && container.possibleMutations.contains(ObjectMutation::ChangeIntegrity)))
            return aspects;
        if (!container.possibleMutations.containsAny(elementStorageMutations))
            aspects.add(PristineAspects::Storage);
        if (isArray(index) && aspects.contains(PristineAspects::Storage) && m_arrayWrites[index].isDense())
            aspects.add(PristineAspects::Dense);
        if (!container.possibleMutations.containsAny(builtinBehaviorMutations) && m_classHasDefaultSpecies.get(container.builtinClass.index()))
            aspects.add(PristineAspects::Names);
        if (container.builtinClass == BuiltinClasses::RegExp && aspects.contains(PristineAspects::Names) && keepsPatternAndNumericLastIndex(maker, container))
            aspects.add(PristineAspects::AsCreated);
        return aspects;
    }

    const ValueFlowResultInputs& m_inputs;
    const FlowResultTables& m_tables;
    TypeTable& m_types;
    ValueFlowHeapView m_heap;
    ValueFlowPositionCounts m_counts;
    const unsigned m_indexOfElements;

    Vector<Container> m_containers;
    ValueSetID m_allContainers { ValueSets::nothing };
    ValueSetID m_arrays { ValueSets::nothing };
    ValueSetID m_otherArrays { ValueSets::nothing };
    Vector<ArrayWrites> m_arrayWrites;
    ValueSetID m_kindsOfBoundedNumbers { ValueSets::nothing };
    ValueSetID m_objectsWithReplacedContents { ValueSets::nothing };
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

void ValueFlowWritesToContainersDeleter::operator()(ValueFlowWritesToContainers* writes) const
{
    delete writes;
}

std::unique_ptr<ValueFlowWritesToContainers, ValueFlowWritesToContainersDeleter> collectWritesToContainers(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, TypeTable& types)
{
    return std::unique_ptr<ValueFlowWritesToContainers, ValueFlowWritesToContainersDeleter>(new ValueFlowWritesToContainers(inputs, tables, types));
}

ValueFlowContainerResults computeContainerResults(ValueFlowWritesToContainers& writes)
{
    return writes.convertContents();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

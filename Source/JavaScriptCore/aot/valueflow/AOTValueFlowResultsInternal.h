/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowApplication.h"
#include "AOTLayout.h"
#include "AOTThreads.h"
#include "AOTTypeTable.h"
#include "AOTValueFlowAnalysis.h"
#include "AOTValueFlowLog.h"
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// Tables of the sizes and in the orders that FlowResults and ApplyFlowResults expect, which state nothing about the program: applied, they leave it as it was read.
FlowResultTables createUnknownResultTables(const Program&, BuildContext&);

// What is proven about one array, map, set or promise that the program allocates and that nobody else gets hold of.
struct ValueFlowContainerFacts {
    Vector<TypeID, 2> contents;
    bool contentsAreProven { false };
    ArrayStorage arrayStorage { ArrayStorage::Other };
    OptionSet<PristineAspects> pristine;
};

class ValueFlowContainerResults {
public:
    // None for an object about which nothing is proven.
    const ValueFlowContainerFacts* find(ValueFlowObjectID object) const LIFETIME_BOUND
    {
        uint32_t position = object.index() < m_positionOfObject.size() ? m_positionOfObject[object.index()] : 0;
        return position && m_isKnown[position - 1] ? &m_facts[position - 1] : nullptr;
    }

    void setObjects(uint32_t numberOfObjectsOfUniverse, std::span<const ValueFlowObjectID> objects)
    {
        m_positionOfObject.fill(0, numberOfObjectsOfUniverse);
        for (uint32_t index = 0; index < objects.size(); ++index)
            m_positionOfObject[objects[index].index()] = index + 1;
        m_facts = Vector<ValueFlowContainerFacts>(objects.size());
        m_isKnown.fill(0, objects.size());
    }

    void set(ValueFlowObjectID object, ValueFlowContainerFacts&& facts)
    {
        uint32_t position = m_positionOfObject[object.index()] - 1;
        m_facts[position] = WTF::move(facts);
        m_isKnown[position] = 1;
    }

private:
    Vector<uint32_t> m_positionOfObject;
    Vector<ValueFlowContainerFacts> m_facts;
    Vector<uint8_t> m_isKnown;
};

// By the object that stands for what built-ins allocate as the result of one call operation: the class of that result. No class where the built-ins that the operation applies do not agree on one.
using ValueFlowClassesOfBuiltinResults = HashMap<uint32_t, BuiltinClassID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

enum class ValueFlowAccessThroughObjectKind : uint8_t {
    // The operation answers what lies under the name.
    Read,
    // Somebody else gets hold of what lies there, or finds out whether anything does.
    Observation,
    Store,
};

struct ValueFlowAccessThroughObject {
    VariableID variable;
    // Neither where no operation of a function spells the access out.
    FunctionID function;
    ValueID site;
    ValueFlowAccessThroughObjectKind kind { ValueFlowAccessThroughObjectKind::Observation };

    friend auto operator<=>(const ValueFlowAccessThroughObject&, const ValueFlowAccessThroughObject&) = default;
};

// Where an operation that stores or defines a property has the value. None for an operation that stores in some other way, as a call of a built-in does.
constexpr std::optional<uint32_t> indexOfValueStoredIntoProperty(Opcode opcode)
{
    switch (opcode) {
    case Opcode::PropSet:
        return OperandIndex::PropSet::value;
    case Opcode::PropDefine:
        return OperandIndex::PropDefine::value;
    case Opcode::ElemSet:
        return OperandIndex::ElemSet::value;
    case Opcode::ElemDefine:
        return OperandIndex::ElemDefine::value;
    default:
        return std::nullopt;
    }
}

// A variable may be a property of an object at once: what a module exports lies on its namespace object, and a static field lies on its class object.
struct ValueFlowVariablesOnObjects {
    std::span<const ValueFlowAccessThroughObject> accessesTo(VariableID variable) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(accesses, variable, { }, &ValueFlowAccessThroughObject::variable);
        return { found.begin(), found.end() };
    }

    // By module.
    Vector<NamespaceUse> namespaceUses;
    // Every access to a static field of a class object that the analysis follows, and to an export of a namespace object that is only read by name. Ascending, each once.
    Vector<ValueFlowAccessThroughObject> accesses;
    // By variable. The static fields of a class object that somebody holds whom nobody follows: all that can be done to a property is done to them.
    BitVector staticFieldsInUnknownHands;
    // By variable.
    BitVector staticFieldsOfEscapedClassObjects;
    // By variable. The run time empties the variable of a static field that is deleted, redefined or stored a value outside its type, and keeps the property elsewhere.
    BitVector deletedStaticFields;
    BitVector redefinedStaticFields;
    BitVector staticFieldsStoredOtherTypes;
    // By variable.
    BitVector staticFieldsStoredByBuiltins;
    // By layout. The analysis follows the class object, and but for the definitions of the class itself every store into it names one of its static fields.
    BitVector classObjectsStoredUnderStaticFieldsAlone;
};

// A set knows numbers by kind alone. This bounds the numbers among some values: whether one may be something else than an integer, and the range of the integers.
struct ValueFlowNumbers {
    static constexpr ValueFlowNumbers any() { return { true, Range::full() }; }

    bool isEmpty() const { return !mayBeOtherThanIntegers && integers.isEmpty(); }

    void add(const Type& type)
    {
        mayBeOtherThanIntegers |= !!(type.primitives & Primitive::Double);
        if (type.primitives & Primitive::Integer)
            integers = integers.unionWith(type.integers);
    }

    void unite(const ValueFlowNumbers& other)
    {
        mayBeOtherThanIntegers |= other.mayBeOtherThanIntegers;
        integers = integers.unionWith(other.integers);
    }

    void narrow(Type& type) const
    {
        if (!mayBeOtherThanIntegers)
            type.primitives &= ~Primitive::Double;
        if (!(type.primitives & Primitive::Integer))
            return;
        type.integers = type.integers.intersectWith(integers);
        if (type.integers.isEmpty()) {
            type.primitives &= ~Primitive::Integer;
            type.integers = Type().integers;
        }
    }

    ValueFlowNumbers meetWith(const ValueFlowNumbers& other) const
    {
        Range both = integers.intersectWith(other.integers);
        return { mayBeOtherThanIntegers && other.mayBeOtherThanIntegers, both.isEmpty() ? Range::empty() : both };
    }

    friend bool operator==(const ValueFlowNumbers&, const ValueFlowNumbers&) = default;

    bool mayBeOtherThanIntegers { false };
    Range integers { Range::empty() };
};

// Which numbers a value of a function can be, in the typed and in the generic version of the function.
class ValueFlowNumbersOfValues {
public:
    virtual ~ValueFlowNumbersOfValues() = default;
    virtual ValueFlowNumbers numbersOf(FunctionID, ValueID) const = 0;
    // The numbers among the values of a Map. None where something that nobody has looked at may store a value.
    virtual const ValueFlowNumbers* ofValuesOfMap(ValueFlowObjectID) const = 0;
};

struct ValueFlowLayoutsOfSites;
class ValueFlowHeapIndex;
class ValueFlowHeapView;
class ValueFlowNodesAskedFor;

using ValueFlowKindsOfMadeNodes = uint16_t;
constexpr ValueFlowKindsOfMadeNodes kindsOfMadeNodes(std::initializer_list<ValueFlowMadeNodeKind> kinds)
{
    ValueFlowKindsOfMadeNodes bits = 0;
    for (ValueFlowMadeNodeKind kind : kinds)
        bits |= 1u << static_cast<unsigned>(kind);
    return bits;
}

struct ValueFlowResultInputs {
    const Program& program;
    ValueFlowSolution& solution;
    BuildContext& build;
    // None until it is computed: until then a number is any number.
    const ValueFlowNumbersOfValues* numbersOfValues { nullptr };
    // None until it is computed: until then nothing is known about a namespace object or a static field.
    const ValueFlowVariablesOnObjects* variablesOnObjects { nullptr };
    // What the sets become types with. Each is none until it is computed: until then nothing is proven about a container, no symbol counts as created once, and what a built-in allocates is any object.
    const ValueFlowContainerResults* containers { nullptr };
    // By key.
    const BitVector* symbolsCreatedOnce { nullptr };
    const ValueFlowClassesOfBuiltinResults* classesOfBuiltinResults { nullptr };
    // By layout. None until it is computed, which is before the first set of objects becomes a type.
    const BitVector* layoutsOfDictionaries { nullptr };
    std::span<const std::pair<TypeID, TypeID>> typesBehindDictionaries;
    const BitVector* layoutSetsNamingDictionaries { nullptr };
    // What findLayoutsOfSites() answers. Whoever has none makes it when it is first asked for, once for each ValueFlowPositionCounts.
    const ValueFlowLayoutsOfSites* layoutsOfSites { nullptr };
    // Whoever has none gets one with each ValueFlowHeapView that it makes.
    const ValueFlowHeapIndex* heapIndex { nullptr };
    const ValueFlowNodesAskedFor* nodesAskedFor { nullptr };

    const ValueFlowBodies& bodies() const { return solution.bodies(); }
    const ValueFlowUniverse& universe() const { return solution.bodies().universe(); }
    const ValueFlowSolver& solver() const { return solution.solver(); }
    const ValueSets& sets() const { return solution.sets(); }
    const ParallelLoops& loops() const { return build.parallelLoops(); }
    ValueSetMakerPool& makerPool() const { return solution.makerPool(); }
    bool isReached(FunctionID function) const { return solution.solver().isReached(function); }
    void forEachMadeNodeAskedForBy(ValueFlowStatementID, ValueFlowKindsOfMadeNodes, bool alsoWithStatement, NOESCAPE const auto& functor) const;
    void forEachIndexOfMadeNodesAskedForBy(ValueFlowStatementID, ValueFlowKindsOfMadeNodes, bool alsoWithStatement, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const;
    void prefetchMadeNodesAskedForBy(uint32_t statement, ValueFlowKindsOfMadeNodes, bool alsoWithStatement) const;
    // Handed to code that nobody compiles: the unknown object stands for it wherever that code may put it.
    bool hasEscaped(ValueFlowObjectID object) const { return sets().contains(solver().valueOf(ValueFlowProgramNodes::escaped()), object); }
    // The analysis has stopped following it: the untracked object stands for it from there on.
    bool isLost(ValueFlowObjectID object) const { return sets().contains(solver().valueOf(ValueFlowProgramNodes::lost()), object); }
    // Code that nobody compiles holds it through what a module of the library exports, and the solve has not let that code call it.
    bool isHeldByNativeCode(ValueFlowObjectID object) const { return sets().contains(solver().valueOf(bodies().nodes().ofHost(ValueFlowHostNode::HeldByNativeCode)), object); }

    bool isLayoutOfDictionaries(LayoutID layout) const { return layoutsOfDictionaries && layout && layoutsOfDictionaries->get(layout.index()); }
    // The solve has followed it as an object with a layout. It is made as an ordinary object.
    bool isDictionary(ValueFlowObjectID object) const { return layoutsOfDictionaries && isLayoutOfDictionaries(universe().layoutOf(object)); }

    // The universe is made before anybody knows the callee of a call, so it has no class for what the callee allocates.
    BuiltinClassID classOf(ValueFlowObjectID object) const
    {
        if (isDictionary(object))
            return BuiltinClasses::Object;
        if (BuiltinClassID builtinClass = universe().classOf(object))
            return builtinClass;
        return classesOfBuiltinResults ? classesOfBuiltinResults->get(object.index()) : BuiltinClassID();
    }

    static constexpr bool isPlaceholder(ValueFlowObjectID object) { return object == ValueFlowUniverse::unknownObject() || object == ValueFlowUniverse::untrackedObject(); }

    // All that may arrive at the operand, as ValueFlowPositions takes it: beside a placeholder the set may or may not list what the placeholder covers.
    ValueSetID valueOf(ValueSetMaker& maker, const ValueFlowOperand& operand) const
    {
        ValueSetID held = operand.constant ? operand.constant : ValueSets::nothing;
        if (operand.node)
            held = maker.unite(held, solver().valueOf(operand.node));
        if (!operand.mask)
            return held;
        switch (operand.maskKind) {
        case ValueFlowMaskKind::Plain:
            return maker.intersect(held, operand.mask);
        case ValueFlowMaskKind::Checked:
            return maker.intersect(held, maker.unite(operand.mask, maker.unite(universe().untrackedObjectAlone(), universe().unknownObjectAlone())));
        case ValueFlowMaskKind::Declared:
        case ValueFlowMaskKind::Expected:
            return held;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
};

// A maker of sets takes megabytes to set up. A loop over the functions has one for each of its threads, made when the thread first asks for it.
class ValueSetMakers {
    WTF_MAKE_NONCOPYABLE(ValueSetMakers);
public:
    explicit ValueSetMakers(const ValueFlowResultInputs& inputs)
        : ValueSetMakers(inputs.makerPool(), inputs.loops())
    {
    }

    ValueSetMakers(ValueSetMakerPool& pool, const ParallelLoops& loops)
        : m_pool(pool)
        , m_makers(FillWith { }, loops.width(), nullptr)
    {
    }

    ~ValueSetMakers()
    {
        for (ValueSetMaker* maker : m_makers) {
            if (maker)
                m_pool.giveBack(*maker);
        }
    }

    // `worker` is what forEachIndexInParallel() hands to the body of the loop.
    ValueSetMaker& of(unsigned worker)
    {
        if (!m_makers[worker])
            m_makers[worker] = &m_pool.take();
        return *m_makers[worker];
    }

private:
    ValueSetMakerPool& m_pool;
    Vector<ValueSetMaker*> m_makers;
};

template<typename T>
Vector<T> collectWithMakers(const ValueFlowResultInputs& inputs, ValueSetMakers& makers, unsigned count, const Invocable<void(unsigned, ValueSetMaker&, Vector<T>&)> auto& collect)
{
    constexpr unsigned chunkSize = 512;
    Vector<Vector<T>> chunks((count + chunkSize - 1) / chunkSize);
    forEachIndexInParallel(inputs.loops(), chunks.size(), [&](unsigned chunk, unsigned worker) {
        ValueSetMaker& maker = makers.of(worker);
        for (unsigned index = chunk * chunkSize; index < std::min(count, (chunk + 1) * chunkSize); ++index)
            collect(index, maker, chunks[chunk]);
    });
    size_t size = 0;
    for (const Vector<T>& chunk : chunks)
        size += chunk.size();
    Vector<T> result;
    result.reserveInitialCapacity(size);
    for (Vector<T>& chunk : chunks)
        result.appendVector(WTF::move(chunk));
    return result;
}

// The solve has not reached the function, and its operations are compiled all the same: the host may enter it.
inline bool isUnsolvedCode(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, FunctionID function)
{
    if (inputs.isReached(function) || !inputs.program.at(function).hasBody())
        return false;
    return tables.functions.functions[function.index()].reachability != FunctionReachability::Unreached;
}

// A call of a member asks once more, receiver by receiver, for what the read of its callee has found, and hands each receiver to the functions that it answers.
inline bool isLookupOfCalleeByCall(const ValueFlowResultInputs& inputs, const ValueFlowStatement& statement, const ValueFlowNodeKey& key)
{
    if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::Call && statement.opcode != Opcode::CallList) || key.kind != ValueFlowMadeNodeKind::Lookup)
        return false;
    PropertyKeyID calleeKey = inputs.program.at(statement.function).at(statement.site).calleeKey();
    return calleeKey && key.name() == ValueFlowName::property(calleeKey) && inputs.sets().onlyMember(key.set);
}

// An environment is no object of the language, and a closure or a namespace object has its layout without an allocation.
inline bool hasObjectsThatCodeAlters(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Instance:
    case LayoutKind::Container:
        return true;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::BuiltinResult:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The sites of a typed result are made for every call that may turn out to have one. The rule that allocates the result marks each of its objects with a layout: a parser adds the members that the layout does not declare.
inline bool isSiteOfTypedResultThatNothingAllocates(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const ValueFlowSite* site = inputs.universe().siteOf(object);
    return site && site->origin == ValueFlowSiteOrigin::TypedResult && site->layout && !inputs.solver().marksOf(object).contains(ObjectMutation::AddProperty);
}

// What the rules have marked on the object.
OptionSet<ObjectMutation> markedMutationsOf(const ValueFlowResultInputs&, ValueFlowObjectID);
// Nothing is stored to, added to, taken from or redefined on the object once it is made, its prototype stays, and no unknown code gets it.
bool neverChangesOnceMade(const ValueFlowResultInputs&, ValueFlowObjectID);

enum class ValueFlowMembers : uint8_t {
    // The functor has seen every member, the placeholders among them. One that the node and the constant both hold came twice.
    Listed,
    // The operand holds all of the universe but a few members, and the functor has seen none.
    AlmostEverything,
};

// The members of ValueFlowResultInputs::valueOf() of the operand, without making a set.
ValueFlowMembers forEachMemberOf(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor)
{
    const ValueSets& sets = inputs.sets();
    bool cuts = operand.mask && operand.maskKind != ValueFlowMaskKind::Declared && operand.maskKind != ValueFlowMaskKind::Expected;
    ValueSetID mask = cuts ? operand.mask : ValueSets::anything;
    bool letsPlaceholdersPass = cuts && operand.maskKind == ValueFlowMaskKind::Checked;
    ValueSetID held[] { operand.constant ? operand.constant : ValueSets::nothing, operand.node ? inputs.solver().valueOf(operand.node) : ValueSets::nothing };
    for (ValueSetID set : held) {
        if (ValueSets::isComplement(set) && ValueSets::isComplement(mask))
            return ValueFlowMembers::AlmostEverything;
    }
    for (ValueSetID set : held) {
        bool listsMask = ValueSets::isComplement(set);
        sets.forEachMember(listsMask ? mask : set, [&](ValueFlowObjectID member) {
            if (sets.contains(listsMask ? set : mask, member))
                functor(member);
        });
        if (!letsPlaceholdersPass)
            continue;
        for (ValueFlowObjectID placeholder : { ValueFlowUniverse::unknownObject(), ValueFlowUniverse::untrackedObject() }) {
            if (sets.contains(set, placeholder) && !sets.contains(mask, placeholder))
                functor(placeholder);
        }
    }
    return ValueFlowMembers::Listed;
}

// What the report on sites says where the unknown object takes a proof away: how that object has come into being.
constexpr UnprovenCause unprovenCauseOf(ValueFlowUnknownSource source)
{
    switch (source) {
    case ValueFlowUnknownSource::UndescribedNativeResult:
    case ValueFlowUnknownSource::DeclaredNativeResult:
        return UnprovenCause::UndescribedNativeResult;
    case ValueFlowUnknownSource::UnseenCode:
        return UnprovenCause::UnseenCode;
    case ValueFlowUnknownSource::UncheckedConversionOfUntracked:
        return UnprovenCause::UncheckedConversionOfUntracked;
    case ValueFlowUnknownSource::DataFromOutside:
        return UnprovenCause::DataFromOutside;
    case ValueFlowUnknownSource::ThrownByRunTime:
        return UnprovenCause::ThrownByRunTime;
    case ValueFlowUnknownSource::ParameterOfEscapedFunction:
        return UnprovenCause::ParameterOfEscapedFunction;
    case ValueFlowUnknownSource::ContentsOfEscapedObject:
        return UnprovenCause::ContentsOfEscapedObject;
    case ValueFlowUnknownSource::ResultOfUnknownCallee:
        return UnprovenCause::ResultOfUnknownCallee;
    case ValueFlowUnknownSource::ObjectWithoutLayout:
        return UnprovenCause::ObjectWithoutLayout;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// For every node that holds the unknown object: the operation whose rule has brought that object into being, at the far end of a shortest way against the edges.
class ValueFlowBirthsOfUnknownObject {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowBirthsOfUnknownObject);
public:
    explicit ValueFlowBirthsOfUnknownObject(const ValueFlowResultInputs&);

    // None where the node does not hold the object, where the object comes out of the seed of a body, and where a statement has made it that lies in no function.
    Witness witnessOf(ValueFlowNodeID) const;

private:
    static constexpr uint32_t none = UINT32_MAX;

    // By node: where its witness stands in the list.
    Vector<uint32_t> m_birthOfNode;
    Vector<Witness> m_witnesses;
};

// Where the program makes the object: the operation of its site, or the declaration of the function that a closure, the result of a coroutine or a rest array belongs to. None for any other object.
Witness witnessOfAllocation(const ValueFlowResultInputs&, ValueFlowObjectID);
// The operation that the statement stands for, or the declaration of the function in whose body it lies. None for a statement of the host and for one that a rule has made.
Witness witnessOfStatement(const Program&, const ValueFlowStatement&);

// What the stamps say about the functions, for the results about functions.
struct ValueFlowStampedCalls {
    // The functions that the stamp of some call lists.
    BitVector functionsInStamps;
    // The functions that something enters that lists them in no stamp: an accessor, a protocol, a built-in that calls an operand, a bound function, a call of a placeholder that covers their closure.
    BitVector functionsEnteredWithoutStamp;
    // The functions that unsolved code calls or makes closures of, and those whose closures lie where it reads. Filled with the results about functions.
    BitVector functionsNamedByUnsolvedCode;
};

// All that a store may put into a field of an object, by the layout that declares the field. Nothing for a field that the layout inherits.
struct ValueFlowFieldContents {
    Vector<Vector<ValueSetID>> stored;
    // A set knows numbers by kind alone, and the sum of two numbers is any number there. These are the numbers among what is stored, by what the operations that store them make: any number as soon as something else stores one.
    Vector<Vector<ValueFlowNumbers>> storedNumbers;
    // The same for the fields that the results append to a layout, in the order of FlowLayoutUpdate::appendedFields.
    Vector<Vector<ValueSetID>> storedInAppendedFields;
};

class ValueFlowNodesAskedFor {
    WTF_MAKE_NONCOPYABLE(ValueFlowNodesAskedFor);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowNodesAskedFor);
public:
    explicit ValueFlowNodesAskedFor(const ValueFlowResultInputs&);

    void forEach(const ValueFlowSolver& solver, ValueFlowStatementID statement, ValueFlowKindsOfMadeNodes kinds, bool alsoWithStatement, NOESCAPE const auto& functor) const
    {
        for (uint32_t word : m_words.subspan(m_first[statement.index()], m_first[statement.index() + 1] - m_first[statement.index()])) {
            if (!isWanted(word, kinds, alsoWithStatement))
                continue;
            uint32_t index = word & maskOfIndex;
            if constexpr (std::is_invocable_v<decltype(functor), const ValueFlowNodeKey&, ValueFlowStatementID, ValueFlowNodeID>)
                functor(solver.keyOfMadeNodesAt(index), solver.statementOfMadeNodesAt(index), solver.firstOfMadeNodesAt(index));
            else
                functor(solver.keyOfMadeNodesAt(index), solver.statementOfMadeNodesAt(index));
        }
    }

    void forEachIndex(ValueFlowStatementID statement, ValueFlowKindsOfMadeNodes kinds, bool alsoWithStatement, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        for (uint32_t word : m_words.subspan(m_first[statement.index()], m_first[statement.index() + 1] - m_first[statement.index()])) {
            if (isWanted(word, kinds, alsoWithStatement))
                functor(word & maskOfIndex);
        }
    }

    void prefetch(const ValueFlowSolver& solver, uint32_t statement, ValueFlowKindsOfMadeNodes kinds, bool alsoWithStatement) const
    {
        if (statement + 1 >= m_first.size())
            return;
        for (uint32_t word : m_words.subspan(m_first[statement], m_first[statement + 1] - m_first[statement])) {
            if (isWanted(word, kinds, alsoWithStatement))
                __builtin_prefetch(&solver.keyOfMadeNodesAt(word & maskOfIndex));
        }
    }

private:
    static constexpr unsigned bitsOfIndex = 27;
    static constexpr uint32_t maskOfIndex = (1u << bitsOfIndex) - 1;
    static constexpr uint32_t hasStatement = 1u << bitsOfIndex;

    static bool isWanted(uint32_t word, ValueFlowKindsOfMadeNodes kinds, bool alsoWithStatement)
    {
        return (kinds >> (word >> (bitsOfIndex + 1)) & 1) || (alsoWithStatement && (word & hasStatement));
    }

    Vector<uint32_t> m_first;
    Vector<uint32_t> m_words;
};

void ValueFlowResultInputs::forEachMadeNodeAskedForBy(ValueFlowStatementID statement, ValueFlowKindsOfMadeNodes kinds, bool alsoWithStatement, NOESCAPE const auto& functor) const
{
    if (nodesAskedFor) {
        nodesAskedFor->forEach(solver(), statement, kinds, alsoWithStatement, functor);
        return;
    }
    solver().forEachMadeNodeAskedForBy(statement, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made, ValueFlowNodeID first) {
        if (!(kinds >> static_cast<unsigned>(key.kind) & 1) && !(alsoWithStatement && made))
            return;
        if constexpr (std::is_invocable_v<decltype(functor), const ValueFlowNodeKey&, ValueFlowStatementID, ValueFlowNodeID>)
            functor(key, made, first);
        else
            functor(key, made);
    });
}

void ValueFlowResultInputs::forEachIndexOfMadeNodesAskedForBy(ValueFlowStatementID statement, ValueFlowKindsOfMadeNodes kinds, bool alsoWithStatement, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
{
    if (nodesAskedFor) {
        nodesAskedFor->forEachIndex(statement, kinds, alsoWithStatement, functor);
        return;
    }
    solver().forEachMadeNodeAskedForBy(statement, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
        if ((kinds >> static_cast<unsigned>(key.kind) & 1) || (alsoWithStatement && made))
            functor(solver().indexOfMadeNodes(key));
    });
}

inline void ValueFlowResultInputs::prefetchMadeNodesAskedForBy(uint32_t statement, ValueFlowKindsOfMadeNodes kinds, bool alsoWithStatement) const
{
    if (nodesAskedFor)
        nodesAskedFor->prefetch(solver(), statement, kinds, alsoWithStatement);
}

class ValueFlowAnswersAboutSets {
    WTF_MAKE_NONCOPYABLE(ValueFlowAnswersAboutSets);
public:
    ValueFlowAnswersAboutSets() = default;

    ValueSetID ensure(uint64_t question, NOESCAPE const Invocable<ValueSetID()> auto& answer)
    {
        Part& part = m_parts[(question * 0x9e3779b97f4a7c15ull) >> 58];
        {
            Locker locker { part.lock };
            auto found = part.answers.find(question);
            if (found != part.answers.end())
                return found->value;
        }
        ValueSetID answered = answer();
        Locker locker { part.lock };
        return part.answers.add(question, answered).iterator->value;
    }

private:
    struct alignas(64) Part {
        Lock lock;
        HashMap<uint64_t, ValueSetID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> answers WTF_GUARDED_BY_LOCK(lock);
    };

    std::array<Part, 64> m_parts;
};

// What the views of the heap look things up in.
class ValueFlowHeapIndex {
    WTF_MAKE_NONCOPYABLE(ValueFlowHeapIndex);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowHeapIndex);
public:
    struct WrittenNode {
        ValueSetID objects;
        ValueSetID value;
    };

    struct NodeOfMember {
        uint32_t member;
        uint32_t node;

        friend auto operator<=>(const NodeOfMember&, const NodeOfMember&) = default;
    };

    struct NodesWrittenUnderName {
        uint32_t number { 0 };
        Vector<WrittenNode> nodes;
        bool isIndexed { false };
        ValueSetID writtenThroughUnknownObject { ValueSets::nothing };
        ValueSetID writtenThroughUntrackedObject { ValueSets::nothing };
        Vector<uint32_t> nodesOfManyMembers;
        Vector<NodeOfMember> nodesOfMembers;
    };

    struct CallOfValue {
        // In ValueFlowBodies::statements().
        uint32_t statement;
        // What the rules see at the callee.
        ValueSetID callees;
    };

    struct OperandsOfCall {
        ValueSetID thisValue { ValueSets::nothing };
        ValueSetID otherArguments { ValueSets::nothing };
        uint32_t firstArgument { 0 };
        uint32_t numberOfArguments { 0 };
        uint32_t firstBoundFunction { 0 };
        uint32_t numberOfBoundFunctions { 0 };
    };

    explicit ValueFlowHeapIndex(const ValueFlowResultInputs&);

    // The nodes that hold something, in the order of their numbers. None where nothing was written under the name.
    const NodesWrittenUnderName* findNodesWrittenUnder(ValueFlowName name) const LIFETIME_BOUND
    {
        uint64_t number = static_cast<uint64_t>(name.kind) << 32 | name.payload;
        std::optional<uint32_t> position = m_positionOfNameWritten.find(IntHash<uint64_t>::hash(number), [&](uint32_t candidate) {
            return m_namesWritten[candidate] == number;
        });
        return position ? &m_nodesOfNamesWritten[*position] : nullptr;
    }
    struct Stretch {
        ValueFlowName name;
        std::span<const WrittenNode> nodes;
    };
    Vector<Stretch> stretchesOfNodesWrittenUnder(NOESCAPE const Invocable<bool(ValueFlowName)> auto& isWanted) const
    {
        constexpr size_t nodesPerStretch = 512;
        Vector<Stretch> stretches;
        for (uint32_t position = 0; position < m_namesWritten.size(); ++position) {
            ValueFlowName name { static_cast<ValueFlowNameKind>(m_namesWritten[position] >> 32), static_cast<uint32_t>(m_namesWritten[position]) };
            if (!isWanted(name))
                continue;
            std::span<const WrittenNode> nodes = m_nodesOfNamesWritten[position].nodes.span();
            for (size_t first = 0; first < nodes.size(); first += nodesPerStretch)
                stretches.append({ name, nodes.subspan(first, std::min(nodesPerStretch, nodes.size() - first)) });
        }
        return stretches;
    }
    // The calls and constructions of a value in reached functions at whose callee a built-in function arrives, or a callable object that is neither a closure nor a built-in function. In the order of the statements.
    std::span<const CallOfValue> callsThatMayApplyBuiltins() const LIFETIME_BOUND { return m_callsThatMayApplyBuiltins.span(); }
    void findOperandsOfCalls(const ValueFlowResultInputs&, const ValueFlowHeapView&);
    const OperandsOfCall* operandsOfCallAt(size_t position) const LIFETIME_BOUND { return m_operandsOfCalls.isEmpty() ? nullptr : &m_operandsOfCalls[position]; }
    std::span<const ValueSetID> argumentsOf(const OperandsOfCall& operands) const LIFETIME_BOUND { return m_argumentsOfCalls.subspan(operands.firstArgument, operands.numberOfArguments); }
    std::span<const ValueFlowObjectID> boundFunctionsOf(const OperandsOfCall& operands) const LIFETIME_BOUND { return m_boundFunctionsOfCalls.subspan(operands.firstBoundFunction, operands.numberOfBoundFunctions); }
    // Ascending.
    std::span<const ValueFlowNodeID> firstNodesOfCallGroups() const LIFETIME_BOUND { return m_firstNodesOfCallGroups.span(); }
    std::span<const ValueFlowNodeID> nodesOfMarks() const LIFETIME_BOUND { return m_nodesOfMarks.span(); }
    ValueFlowAnswersAboutSets& prototypesOfSets() const LIFETIME_BOUND { return m_prototypesOfSets; }
    ValueFlowAnswersAboutSets& chainsOfSets() const LIFETIME_BOUND { return m_chainsOfSets; }
    ValueFlowAnswersAboutSets& writtenUnderNames() const LIFETIME_BOUND { return m_writtenUnderNames; }

private:
    void collectNodesWritten(const ValueFlowResultInputs&);
    static void indexNodesWrittenUnder(const ValueFlowResultInputs&, NodesWrittenUnderName&);
    void collectCalls(const ValueFlowResultInputs&);

    Vector<uint64_t> m_namesWritten;
    ParallelArray<NodesWrittenUnderName> m_nodesOfNamesWritten;
    HashedIndex m_positionOfNameWritten;
    Vector<CallOfValue> m_callsThatMayApplyBuiltins;
    Vector<OperandsOfCall> m_operandsOfCalls;
    Vector<ValueSetID> m_argumentsOfCalls;
    Vector<ValueFlowObjectID> m_boundFunctionsOfCalls;
    Vector<ValueFlowNodeID> m_firstNodesOfCallGroups;
    Vector<ValueFlowNodeID> m_nodesOfMarks;
    mutable ValueFlowAnswersAboutSets m_prototypesOfSets;
    mutable ValueFlowAnswersAboutSets m_chainsOfSets;
    mutable ValueFlowAnswersAboutSets m_writtenUnderNames;
};

// The heap as the solve has left it, for questions that no rule has asked.
class ValueFlowHeapView {
    WTF_MAKE_NONCOPYABLE(ValueFlowHeapView);
public:
    explicit ValueFlowHeapView(const ValueFlowResultInputs&);
    ~ValueFlowHeapView();

    const ValueFlowHeapIndex& index() const LIFETIME_BOUND { return m_index; }
    ValueSetMaker& maker() { return m_maker; }
    ValueSetID escaped() const { return m_escaped; }
    ValueSetID lost() const { return m_lost; }
    // What the rules see at the operand.
    ValueSetID valueOf(const ValueFlowOperand&);
    // The set with all that its placeholders cover.
    ValueSetID expand(ValueSetID);
    // A store through the first set may reach one of the objects.
    bool reaches(ValueSetID writtenThrough, ValueSetID objects) const;
    // All that may have been written into one of the objects under exactly this name.
    ValueSetID writtenUnder(ValueSetID objects, ValueFlowName);
    // What the objects may inherit from directly. The unknown object stands for a prototype that nothing names.
    ValueSetID prototypesOf(ValueSetID objects);
    // The objects with all that lies above them.
    ValueSetID chainOf(ValueSetID objects);
    // The objects with what the program puts above them: the prototypes that they are created with and what a store that names them puts into their prototype links. Not what a placeholder stands for.
    ValueSetID chainBuiltByProgram(ValueSetID objects);

    ValueSetID valueOf(ValueSetMaker&, const ValueFlowOperand&) const;
    ValueSetID expand(ValueSetMaker&, ValueSetID) const;
    ValueSetID expandAmong(ValueSetMaker&, ValueSetID, ValueSetID among) const;
    ValueSetID writtenUnder(ValueSetMaker&, ValueSetID objects, ValueFlowName) const;
    ValueSetID prototypesOf(ValueSetMaker&, ValueSetID objects) const;
    ValueSetID chainOf(ValueSetMaker&, ValueSetID objects) const;

    void forEachNodeWrittenUnder(ValueFlowName name, NOESCAPE const Invocable<void(ValueSetID objects, ValueSetID value)> auto& functor) const
    {
        const NodesWrittenUnderName* nodes = m_index.findNodesWrittenUnder(name);
        if (!nodes)
            return;
        for (const WrittenNode& node : nodes->nodes)
            functor(node.objects, node.value);
    }

private:
    using WrittenNode = ValueFlowHeapIndex::WrittenNode;
    using NodeOfMember = ValueFlowHeapIndex::NodeOfMember;
    using NodesWrittenUnderName = ValueFlowHeapIndex::NodesWrittenUnderName;

    struct ChainOfPlaceholder {
        std::once_flag isComputed;
        ValueSetID chain;
    };

    struct PrototypesOfPlaceholder {
        std::once_flag isComputed;
        ValueSetID heirs;
        ValueSetID prototypes;
    };

    void appendPrototypesAtCreation(ValueFlowObjectID, Vector<ValueFlowObjectID, 8>&) const;
    ValueSetID prototypesOfHeirs(ValueSetMaker&, ValueSetID heirs) const;
    ValueSetID findPrototypesOf(ValueSetMaker&, ValueSetID objects) const;
    ValueSetID findWrittenUnder(ValueSetMaker&, ValueSetID objects, const NodesWrittenUnderName&) const;
    ValueSetID chainWalkedInFull(ValueSetMaker&, ValueSetID objects) const;
    ValueSetID walkChainOf(ValueSetMaker&, ValueSetID objects) const;
    void indexStoresIntoPrototypeLinks();

    const ValueFlowResultInputs& m_inputs;
    std::unique_ptr<ValueFlowHeapIndex> m_ownIndex;
    const ValueFlowHeapIndex& m_index;
    ValueSetMakerPool& m_makerPool;
    ValueSetMaker& m_maker;
    ValueSetID m_escaped;
    ValueSetID m_lost;
    // The escaped objects that a store through the unknown object may hit, as the rules have it.
    ValueSetID m_escapedAndWritable;
    mutable ChainOfPlaceholder m_chainOfUnknownObject;
    mutable ChainOfPlaceholder m_chainOfUntrackedObject;
    mutable PrototypesOfPlaceholder m_prototypesOfUnknownObject;
    mutable PrototypesOfPlaceholder m_prototypesOfUntrackedObject;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_prototypesOfSet;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_chainOfSet;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_chainBuiltByProgramOfSet;
    // What the stores that list an object put into its prototype link, by the number of the object. A store through all objects but some is kept as it is.
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_storedIntoPrototypeLinkOf;
    Vector<WrittenNode> m_prototypeLinksStoredThroughComplements;
    bool m_hasIndexedStoresIntoPrototypeLinks { false };
};

// One call of a built-in as the rules have applied it.
class SetsMeetingOneSet {
public:
    SetsMeetingOneSet(const ValueSets& sets, ValueSetID other)
        : m_sets(sets)
        , m_other(other)
    {
    }

    bool contains(ValueSetID set)
    {
        if (set == ValueSets::nothing || m_other == ValueSets::nothing)
            return false;
        auto result = m_answers.add(set.index(), false);
        if (result.isNewEntry)
            result.iterator->value = m_sets.meet(set, m_other);
        return result.iterator->value;
    }

private:
    const ValueSets& m_sets;
    ValueSetID m_other;
    UncheckedKeyHashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_answers;
};

struct ValueFlowBuiltinApplication {
    BuiltinID builtin;
    ValueFlowCallKind kind { ValueFlowCallKind::Call };
    // The operation that spells the call out. None for what all calls of one set of callees share, and for a call by a protocol.
    FunctionID function;
    ValueID site;
    // Of a construction: new.target.
    ValueSetID thisValue { ValueSets::nothing };
    Vector<ValueSetID, numberOfGroupedArguments> arguments;
    // They may land at any position.
    ValueSetID otherArguments { ValueSets::nothing };

    BuiltinCallMode mode() const;
    ValueSetID valueAt(BuiltinOperand, ValueSetMaker&) const;
};

using ValueFlowBuiltinApplicationFunctor = ScopedLambda<void(const ValueFlowBuiltinApplication&)>;
// Every call of a built-in function by an operation and by a group of callees.
void forEachBuiltinApplication(const ValueFlowResultInputs&, ValueFlowHeapView&, NOESCAPE const ValueFlowBuiltinApplicationFunctor&);
void forEachBuiltinApplication(const ValueFlowResultInputs&, ValueFlowHeapView&, const BitVector& onlyOfBuiltins, NOESCAPE const ValueFlowBuiltinApplicationFunctor&);
void forEachBuiltinApplication(const ValueFlowResultInputs&, ValueFlowHeapView&, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, NOESCAPE const ValueFlowBuiltinApplicationFunctor&);
void forEachBuiltinApplicationOnAllThreads(const ValueFlowResultInputs&, const ValueFlowHeapView&, ValueSetMakers&, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, NOESCAPE const ScopedLambda<void(unsigned worker, const ValueFlowBuiltinApplication&)>&);

// Defined in AOTValueFlowResultsFunctions.cpp.
void computeClosuresNeverCompared(const ValueFlowResultInputs&, ValueFlowHeapView&, FlowResultTables&);

BitVector builtinsWithOverload(const BuiltinTable& builtins, NOESCAPE const Invocable<bool(const BuiltinOverload&)> auto& predicate)
{
    BitVector found(builtins.builtinCount());
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        if (std::ranges::any_of(builtins.overloadsOf(BuiltinID(index)), predicate))
            found.quickSet(index);
    }
    return found;
}

// The view of the heap, and who looks at the objects in ways that no mark records.
struct ValueFlowUsesOfHeap;
struct ValueFlowUsesOfHeapDeleter {
    void operator()(ValueFlowUsesOfHeap*) const;
};
std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter> computeUsesOfHeap(const ValueFlowResultInputs&);
void listStoresByLayout(const ValueFlowResultInputs&, ValueFlowUsesOfHeap&, const FlowResultTables&);
ValueFlowHeapView& heapViewOf(ValueFlowUsesOfHeap&);
// The objects whose keys somebody may list, with all that lies above them.
ValueSetID objectsThatMayBeEnumerated(const ValueFlowUsesOfHeap&);

// What the constructors of the program do to the objects that they make, up to the place where somebody else may get hold of the object.
struct ValueFlowConstructions {
    struct OfLayout {
        // Every object with the layout is made by the constructor of the layout, and the analysis has followed that constructor.
        bool isFollowed { false };
        // The constructor and its helpers create fields in one of the orders and nowhere else, and the object does not leave their hands.
        bool createsFieldsOnlyInListedOrders { false };
        // What every construction has stored when the object leaves the hands of the constructor. Ascending. A field without a key stands here by its declaration.
        Vector<PropertyKeyID> keysStoredBeforeObjectLeaks;
        Vector<FieldReference> fieldsStoredBeforeObjectLeaks;
        Vector<PropertyKeyID> keysStoredAtBirth;
        // By a store of something that may be another value than undefined and null.
        Vector<PropertyKeyID> keysStoredWithValueAtBirth;
        Vector<FieldReference> fieldsStoredAtBirth;
        // Of the fields that the layout declares itself.
        Vector<FieldOrder> orders;
        // Of a class that is not followed: the fields of the classes above under whose names the class, or something between, has an accessor or a member that cannot be written. Ascending.
        Vector<PropertyKeyID> inheritedKeysThatStoresMayNotCreate;
    };

    bool isFollowed(LayoutID layout) const { return layouts[layout.index()].isFollowed; }
    bool createsFieldsOnlyInListedOrders(LayoutID layout) const { return layouts[layout.index()].createsFieldsOnlyInListedOrders; }
    const Vector<FieldOrder>& ordersOf(LayoutID layout) const LIFETIME_BOUND { return layouts[layout.index()].orders; }
    bool isStoredBeforeObjectLeaks(LayoutID layout, PropertyKeyID key, FieldReference declaration) const
    {
        const OfLayout& construction = layouts[layout.index()];
        if (key)
            return std::ranges::binary_search(construction.keysStoredBeforeObjectLeaks, key);
        return std::ranges::binary_search(construction.fieldsStoredBeforeObjectLeaks, declaration);
    }
    bool mayBeKeptFromCreatingInheritedField(LayoutID layout, PropertyKeyID key) const { return key && std::ranges::binary_search(layouts[layout.index()].inheritedKeysThatStoresMayNotCreate, key); }
    bool isStoredAtBirth(LayoutID layout, PropertyKeyID key, FieldReference declaration) const
    {
        const OfLayout& construction = layouts[layout.index()];
        if (key)
            return std::ranges::binary_search(construction.keysStoredAtBirth, key);
        return std::ranges::binary_search(construction.fieldsStoredAtBirth, declaration);
    }
    bool isStoredWithValueAtBirth(LayoutID layout, PropertyKeyID key) const
    {
        return key && std::ranges::binary_search(layouts[layout.index()].keysStoredWithValueAtBirth, key);
    }
    // Neither the constructor of a layout nor a function that gets the object of one before the object has leaked.
    bool seesOnlyFinishedObjects(FunctionID function) const { return functionsSeeingOnlyFinishedObjects.get(function.index()); }

    Vector<OfLayout> layouts;
    BitVector functionsSeeingOnlyFinishedObjects;
};

// Asks the solver, and which functions are reached. Defined in AOTFlowConstructionOrders.cpp.
ValueFlowConstructions computeConstructions(const ValueFlowResultInputs&, ValueFlowHeapView&, const FlowResultTables&);
// The orders in which object literals that are completed piece by piece get their keys. Rests on the field results. Defined in AOTFlowLiteralOrderResults.cpp.
void computeLiteralKeyOrders(const ValueFlowResultInputs&, ValueFlowHeapView&, Vector<PropertyKeyKind>&& kindsOfKeys, FlowResultTables&);

// A write that a built-in object may refuse, under a text that names no property anywhere else in the program.
struct ValueFlowWriteUnderNewName {
    BuiltinID holder;
    StringID text;
    Witness witness;
};

// Uses of built-ins, features and locales, and the code that the build has not seen.
Vector<ValueFlowWriteUnderNewName> computeBuiltinResults(const ValueFlowResultInputs&, FlowResultTables&);
// The records of those writes that the run time refuses. Adds keys to the program, so it comes last.
void recordLockedWritesUnderNewNames(const Program&, TypeTable&, std::span<const ValueFlowWriteUnderNewName>, FlowResultTables&);
ValueFlowClassesOfBuiltinResults computeClassesOfBuiltinResults(const ValueFlowResultInputs&);
// Callee sets.
ValueFlowStampedCalls computeCallResults(const ValueFlowResultInputs&, TypeTable&, FlowResultTables&);
// Functions that are never reached, functions whose callers are all known, the calls that no stamp lists, and the closures and bound functions that are only called. Rests on the stamps.
void computeFunctionResults(const ValueFlowResultInputs&, ValueFlowStampedCalls&, FlowResultTables&);
// Functions that run at most once. Rests on the stamps and on the callers.
void computeExecutionResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
// By key: the symbols that one call creates, which runs at most once. Rests on the executions.
BitVector computeCreatedSymbolResults(const ValueFlowResultInputs&, const FlowResultTables&);
void computeClosuresMadeOnce(const ValueFlowResultInputs&, FlowResultTables&);
// How the namespace objects are used, and who gets at a variable through the object that it is a property of. Rests on the functions that unsolved code names.
struct ValueFlowAccessesThroughObjects;
struct ValueFlowAccessesThroughObjectsDeleter {
    void operator()(ValueFlowAccessesThroughObjects*) const;
};
std::unique_ptr<ValueFlowAccessesThroughObjects, ValueFlowAccessesThroughObjectsDeleter> collectAccessesThroughObjects(const ValueFlowResultInputs&, const ValueFlowStampedCalls&);
ValueFlowVariablesOnObjects computeVariablesOnObjects(const ValueFlowResultInputs&, ValueFlowAccessesThroughObjects&, const ValueFlowUsesOfHeap&);
// Who writes, stores to and reads the variables and the fields of environments. Rests on the executions and on the variables on objects.
struct ValueFlowOperationsOnLocations;
struct ValueFlowOperationsOnLocationsDeleter {
    void operator()(ValueFlowOperationsOnLocations*) const;
};
std::unique_ptr<ValueFlowOperationsOnLocations, ValueFlowOperationsOnLocationsDeleter> noteOperationsOnLocations(const ValueFlowResultInputs&, const FlowResultTables&);
void computeLocationResults(const ValueFlowResultInputs&, ValueFlowOperationsOnLocations&, FlowResultTables&);
// Which variables and fields of environments are stored before every read. Rests on the stamps, on the writers of the locations and on the variables on objects.
void computeOrderResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
// The layouts that are never allocated, and what can never happen to an object of a layout.
void computeObjectResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
// How often a class is evaluated, what it extends, and which of its members and links stay as it defines them. Rests on the executions and on the variables on objects.
void computeClassResults(const ValueFlowResultInputs&, FlowResultTables&);
// Who writes the fields of objects, which are never deleted or redefined, and which every allocation fills. Rests on what never happens to the objects of a layout.
ValueFlowFieldContents computeFieldResults(const ValueFlowResultInputs&, ValueFlowUsesOfHeap&, const ValueFlowConstructions&, FlowResultTables&);
// What the arrays, maps, sets and promises of the program hold, how arrays store it, and which of them behave as the language defines them.
struct ValueFlowWritesToContainers;
struct ValueFlowWritesToContainersDeleter {
    void operator()(ValueFlowWritesToContainers*) const;
};
std::unique_ptr<ValueFlowWritesToContainers, ValueFlowWritesToContainersDeleter> collectWritesToContainers(const ValueFlowResultInputs&, const FlowResultTables&, TypeTable&);
ValueFlowContainerResults computeContainerResults(ValueFlowWritesToContainers&);
// The types of variables, fields, parameters and results of functions and of what operations take and give.
std::unique_ptr<ValueFlowNumbersOfValues> prepareNumbersOfPositions(const ValueFlowResultInputs&, ValueFlowHeapView&, const FlowResultTables&);
void computeNumbersOfPositions(ValueFlowNumbersOfValues&, const FlowResultTables&);
// The class of an instance of a built-in class that the program allocates and that only followed code gets hold of.
BuiltinClassID classOfFollowedContainer(const ValueFlowResultInputs&, ValueFlowObjectID);
void computeTypeResults(const ValueFlowResultInputs&, const ValueFlowFieldContents&, TypeTable&, FlowResultTables&, Vector<String>* setsOutsideTypes);
// The types of the appended fields, and the layouts none of whose fields is ever stored a value outside its type. Rests on the types.
void computeLayoutsNeverStoredOtherTypes(const ValueFlowResultInputs&, const ValueFlowFieldContents&, TypeTable&, FlowResultTables&);
// Receiver sets.
void computeAccessResults(const ValueFlowResultInputs&, TypeTable&, FlowResultTables&);
// The fields every store to which names them. Rests on the receiver sets.
void computeStoreNamingResults(const ValueFlowResultInputs&, ValueFlowUsesOfHeap&, FlowResultTables&);
// The calls of Symbol that give a symbol which is created once: each becomes the constant. Takes the proof about such a call away, so it comes behind the types. Rests on ValueFlowResultInputs::symbolsCreatedOnce.
void computeSymbolConstantResults(const ValueFlowResultInputs&, FlowResultTables&);
// The calls whose results compiled parsers and validators allocate by type, and the site that stands for all such objects of a layout. Rests on what never happens to the objects of a layout.
void computeTypedResultResults(const ValueFlowResultInputs&, FlowResultTables&);
// The site, the class and the elements of each object and array in the tree of a Data value. Rests on what never happens to the objects of a layout and on the containers.
void computeDataNodeResults(const ValueFlowResultInputs&, const ValueFlowContainerResults&, TypeTable&, FlowResultTables&);
BitVector computeLayoutsOfDictionaries(const ValueFlowResultInputs&, TypeTable&, FlowResultTables&);
// The type with Object in the place of the layouts whose objects are dictionaries, in what a container holds too.
Type typeBehindDictionaries(const Program&, const BitVector& layoutsOfDictionaries, std::span<const std::pair<TypeID, TypeID>> ascendingTypes, const Type&, TypeArena&);
// The constant texts that arrive at an argument whose text decides what a built-in makes: the pattern and the flags of a RegExp. Rests on the stamps and on the callers of the functions.
void computeSinkOperandResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

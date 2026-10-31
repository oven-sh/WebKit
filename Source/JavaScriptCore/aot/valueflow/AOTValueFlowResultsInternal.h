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
#include "AOTSharedMemo.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeTable.h"
#include "AOTValueFlowAnalysis.h"
#include "AOTValueFlowLog.h"
#include "AOTValueFlowResults.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include "AOTBitVector.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class Ledger;
class LedgerOfVariables;
class LedgerPart;

FlowResultTables createUnknownResultTables(const Program&, BuildContext&);

struct ValueFlowContainerFacts {
    Vector<TypeID, 2> contents;
    bool contentsAreProven { false };
    ArrayStorage arrayStorage { ArrayStorage::Other };
    OptionSet<PristineAspects> pristine;
};

class ValueFlowContainerResults {
public:
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

    // The arrays whose kind of storage is decided and would not be what it is behind a conversion to the storage of numbers.
    ValueSetID arraysThatMustNotBeConverted { ValueSets::nothing };

private:
    Vector<uint32_t> m_positionOfObject;
    Vector<ValueFlowContainerFacts> m_facts;
    Vector<uint8_t> m_isKnown;
};

using ValueFlowClassesOfBuiltinResults = HashMap<uint32_t, BuiltinClassID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

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

class ValueFlowNumbersOfValues {
public:
    virtual ~ValueFlowNumbersOfValues() = default;
    virtual ValueFlowNumbers numbersOf(FunctionID, ValueID) const = 0;
    virtual const ValueFlowNumbers* ofValuesOfMap(ValueFlowObjectID) const = 0;
};

struct ValueFlowLayoutsOfSites;
class ValueFlowHeapIndex;
class ValueFlowHeapView;
class ValueFlowNodesAskedFor;
class LedgerOfClassesAndClosures;

using ValueFlowKindsOfMadeNodes = uint16_t;
constexpr ValueFlowKindsOfMadeNodes kindsOfMadeNodes(std::initializer_list<ValueFlowMadeNodeKind> kinds)
{
    ValueFlowKindsOfMadeNodes bits = 0;
    for (ValueFlowMadeNodeKind kind : kinds)
        bits |= 1u << static_cast<unsigned>(kind);
    return bits;
}

class ValueFlowBirthsOfUnknownObject;

struct ValueFlowResultInputs {
    const Program& program;
    ValueFlowSolution& solution;
    BuildContext& build;
    const ValueFlowNumbersOfValues* numbersOfValues { nullptr };
    const Ledger* ledger { nullptr };
    const LedgerOfVariables* ledgerOfVariables { nullptr };
    const BitVector* functionsNamedByUnsolvedCode { nullptr };
    const ValueFlowContainerResults* containers { nullptr };
    const BitVector* symbolsCreatedOnce { nullptr };
    const ValueFlowClassesOfBuiltinResults* classesOfBuiltinResults { nullptr };
    const BitVector* layoutsOfDictionaries { nullptr };
    std::span<const std::pair<TypeID, TypeID>> typesBehindDictionaries;
    const BitVector* layoutSetsNamingDictionaries { nullptr };
    const ValueFlowLayoutsOfSites* layoutsOfSites { nullptr };
    const ValueFlowHeapIndex* heapIndex { nullptr };
    const ValueFlowNodesAskedFor* nodesAskedFor { nullptr };
    const LedgerOfClassesAndClosures* classesAndClosures { nullptr };
    const ValueFlowBirthsOfUnknownObject* birthsOfUnknownObject { nullptr };

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
    bool hasEscaped(ValueFlowObjectID object) const { return sets().contains(solver().possibleValuesOf(ValueFlowProgramNodes::escaped()), object); }
    bool isLost(ValueFlowObjectID object) const { return sets().contains(solver().possibleValuesOf(ValueFlowProgramNodes::lost()), object); }
    bool isHeldByNativeCode(ValueFlowObjectID object) const { return sets().contains(solver().possibleValuesOf(bodies().nodes().ofHost(ValueFlowHostNode::HeldByNativeCode)), object); }
    // Of an object that has neither escaped nor is lost.

    bool isLayoutOfDictionaries(LayoutID layout) const { return layoutsOfDictionaries && layout && layoutsOfDictionaries->get(layout.index()); }
    bool isDictionary(ValueFlowObjectID object) const { return layoutsOfDictionaries && isLayoutOfDictionaries(universe().layoutOf(object)); }

    BuiltinClassID classOf(ValueFlowObjectID object) const
    {
        if (isDictionary(object))
            return BuiltinClasses::Object;
        if (BuiltinClassID builtinClass = universe().classOf(object))
            return builtinClass;
        return classesOfBuiltinResults ? classesOfBuiltinResults->get(object.index()) : BuiltinClassID();
    }

    static constexpr bool isPlaceholder(ValueFlowObjectID object) { return object == ValueFlowUniverse::unknownObject() || object == ValueFlowUniverse::untrackedObject(); }

    ValueSetID possibleValuesOf(ValueSetMaker& maker, const ValueFlowOperand& operand) const { return throughMaskOf(maker, operand, operand.node ? solver().possibleValuesOf(operand.node) : ValueSets::nothing); }
    ValueSetID seenValuesOf(ValueSetMaker& maker, const ValueFlowOperand& operand) const { return throughMaskOf(maker, operand, operand.node ? solver().seenValuesOf(operand.node) : ValueSets::nothing); }

    ValueSetID throughMaskOf(ValueSetMaker& maker, const ValueFlowOperand& operand, ValueSetID valuesOfNode) const
    {
        ValueSetID held = operand.constant ? maker.unite(operand.constant, valuesOfNode) : valuesOfNode;
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

class ValueSetsMet {
public:
    bool add(ValueSetID set)
    {
        uint32_t& metLately = m_metLately[(set.index() * 0x9e3779b1u) >> 22];
        if (metLately == set.index() + 1)
            return false;
        metLately = set.index() + 1;
        return m_met.add(set.index()).isNewEntry;
    }

private:
    std::array<uint32_t, 1024> m_metLately { };
    HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_met;
};

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

inline bool isUnsolvedCode(const ValueFlowResultInputs& inputs, const FlowResultTables& tables, FunctionID function)
{
    if (inputs.isReached(function) || !inputs.program.at(function).hasBody())
        return false;
    return tables.functions.functions[function.index()].reachability != FunctionReachability::Unreached;
}

inline bool isLookupOfCalleeByCall(const ValueFlowResultInputs& inputs, const ValueFlowStatement& statement, const ValueFlowNodeKey& key)
{
    if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::Call && statement.opcode != Opcode::CallList) || key.kind != ValueFlowMadeNodeKind::Lookup)
        return false;
    PropertyKeyID calleeKey = inputs.program.at(statement.function).at(statement.site).calleeKey();
    return calleeKey && key.name() == ValueFlowName::property(inputs.program, calleeKey) && inputs.sets().onlyMember(key.set);
}

inline bool hasObjectsThatCodeAlters(LayoutKind kind) { return hasObjectsThatCodeAltersOf(kind); }

inline bool isSiteOfTypedResultThatNothingAllocates(const ValueFlowResultInputs& inputs, ValueFlowObjectID object)
{
    const ValueFlowSite* site = inputs.universe().siteOf(object);
    return site && site->origin == ValueFlowSiteOrigin::TypedResult && site->layout && !inputs.solver().marksOf(object).contains(ObjectMutation::AddProperty);
}

OptionSet<ObjectMutation> markedMutationsOf(const ValueFlowResultInputs&, ValueFlowObjectID);
bool neverChangesOnceMade(const ValueFlowResultInputs&, ValueFlowObjectID);

enum class ValueFlowMembers : uint8_t {
    Listed,
    AlmostEverything,
};

ValueFlowMembers forEachMemberOf(const ValueFlowResultInputs& inputs, const ValueFlowOperand& operand, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor)
{
    const ValueSets& sets = inputs.sets();
    bool cuts = operand.mask && operand.maskKind != ValueFlowMaskKind::Declared && operand.maskKind != ValueFlowMaskKind::Expected;
    ValueSetID mask = cuts ? operand.mask : ValueSets::anything;
    bool letsPlaceholdersPass = cuts && operand.maskKind == ValueFlowMaskKind::Checked;
    ValueSetID held[] { operand.constant ? operand.constant : ValueSets::nothing, operand.node ? inputs.solver().possibleValuesOf(operand.node) : ValueSets::nothing };
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

constexpr UnprovenCause unprovenCauseOf(ValueFlowUnknownSource source)
{
    switch (source) {
    case ValueFlowUnknownSource::UndescribedNativeResult:
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

// How each object held by code outside the program escaped: at a statement that passes it on, or through another object:
// contents through their holder, a prototype and its constructor through an instance, an instance through its constructor.
class ValueFlowEscapeOrigins {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowEscapeOrigins);
public:
    explicit ValueFlowEscapeOrigins(const ValueFlowResultInputs&);

    // The objects in `escaping` escape at `statement`, or once an object in `through` has escaped.
    struct EscapePath {
        ValueFlowStatementID statement;
        ValueSetID through;
        ValueSetID escaping;
    };

    std::span<const EscapePath> paths() const LIFETIME_BOUND { return m_paths.span(); }
    ValueSetID objectsWithPaths() const { return m_objectsWithPaths; }

private:
    Vector<EscapePath> m_paths;
    ValueSetID m_objectsWithPaths { ValueSets::nothing };
};

class ValueFlowBirthsOfUnknownObject {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowBirthsOfUnknownObject);
public:
    explicit ValueFlowBirthsOfUnknownObject(const ValueFlowResultInputs&);

    Witness witnessOf(ValueFlowNodeID) const;
    std::optional<ValueFlowUnknownSource> sourceOf(ValueFlowNodeID) const;
    bool hasSeveralRoots(ValueFlowNodeID node) const { return node && m_hasSeveralRoots.get(node.index()); }
    // The next independent roots in source order, as FlowSiteLoss::otherOrigins names them.
    uint16_t otherOriginsOf(ValueFlowNodeID node) const { return node && node.index() < m_otherOriginsOfNode.size() ? m_otherOriginsOfNode[node.index()] : 0; }
    // The statement where an object held by code outside the program escaped, or where the first object that led to it did.
    Witness escapeOriginOf(ValueFlowObjectID object) const { return object.index() < m_originOfObject.size() ? m_originOfObject[object.index()] : noWitness; }
    // Without that statement the object still escapes, by a path that starts elsewhere.
    bool hasOtherEscapeOrigins(ValueFlowObjectID object) const { return m_hasOtherOrigins.get(object.index()); }
    uint16_t otherEscapeOriginsOf(ValueFlowObjectID object) const { return object.index() < m_otherOriginsOfObject.size() ? m_otherOriginsOfObject[object.index()] : 0; }
    Vector<Wire::OtherOriginsRecord> takeOtherOrigins() { return std::exchange(m_otherOrigins, { }); }

private:
    static constexpr uint32_t none = UINT32_MAX;

    struct Root {
        Witness witness { noWitness };
        ValueFlowUnknownSource source { ValueFlowUnknownSource::UnseenCode };
    };
    struct Holders;

    void findRoots(const ValueFlowResultInputs&, const Holders&);

    Vector<uint32_t> m_birthOfNode;
    Vector<Witness> m_witnesses;
    Vector<uint32_t> m_rootOfNode;
    Vector<Root> m_roots;
    BitVector m_hasSeveralRoots;
    Vector<Witness> m_originOfObject;
    BitVector m_hasOtherOrigins;
    Vector<uint16_t> m_otherOriginsOfNode;
    Vector<uint16_t> m_otherOriginsOfObject;
    Vector<Wire::OtherOriginsRecord> m_otherOrigins;
};

Witness witnessOfAllocation(const ValueFlowResultInputs&, ValueFlowObjectID);
Witness witnessOfStatement(const Program&, const ValueFlowStatement&);

struct ValueFlowStampedCalls {
    BitVector functionsInStamps;
    BitVector functionsEnteredWithoutStamp;
    BitVector functionsEnteredWithoutRecord;
    BitVector functionsNamedByUnsolvedCode;
};

struct ValueFlowFieldContents {
    Vector<Vector<ValueSetID>> stored;
    Vector<Vector<ValueFlowNumbers>> storedNumbers;
    Vector<Vector<ValueSetID>> storedInAppendedFields;
    Vector<Vector<ValueFlowNumbers>> numbersStoredInAppendedFields;
    Vector<BitVector> appendedFieldsWithUnknownValues;
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
        uint32_t hash = static_cast<uint32_t>((question * 0x9e3779b97f4a7c15ull) >> 32);
        auto isWanted = [&](const Answer& known) {
            return known.question == question;
        };
        if (const Answer* known = m_answers.find(hash, isWanted))
            return known->answer;
        return m_answers.add(hash, Answer { question, answer() }, isWanted).answer;
    }

    std::optional<ValueSetID> find(uint64_t question) const
    {
        const Answer* known = m_answers.find(static_cast<uint32_t>((question * 0x9e3779b97f4a7c15ull) >> 32), [&](const Answer& known) {
            return known.question == question;
        });
        return known ? std::optional(known->answer) : std::nullopt;
    }

private:
    struct Answer {
        uint64_t question;
        ValueSetID answer;
    };

    SharedMemo<Answer> m_answers;
};

class ValueFlowHeapIndex {
    WTF_MAKE_NONCOPYABLE(ValueFlowHeapIndex);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowHeapIndex);
public:
    struct WrittenNode {
        ValueSetID objects;
        ValueSetID value;
        ValueSetID seenValue;
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
        AOT_CLASS_THRESHOLD(size_t, nodesInGroup, 8);
        struct WrittenThroughManyMembers {
            ValueSetID objects;
            ValueSetID value;
        };
        // What the index by member leaves out, each value once, by the number of members.
        Vector<WrittenThroughManyMembers> writtenThroughManyMembers;
        // The members of each `nodesInGroup` of them together, and of all.
        Vector<ValueSetID> membersOfGroupsOfNodes;
        ValueSetID membersOfNodesOfManyMembers { ValueSets::nothing };
        Vector<NodeOfMember> nodesOfMembers;
    };

    struct CallOfValue {
        uint32_t statement;
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

    const NodesWrittenUnderName* findNodesWrittenUnder(ValueFlowName name) const LIFETIME_BOUND
    {
        ASSERT(!namesElementByText(m_program, name));
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
    std::span<const CallOfValue> callsThatMayApplyBuiltins() const LIFETIME_BOUND { return m_callsThatMayApplyBuiltins.span(); }
    void findOperandsOfCalls(const ValueFlowResultInputs&, const ValueFlowHeapView&);
    const OperandsOfCall* operandsOfCallAt(size_t position) const LIFETIME_BOUND { return m_operandsOfCalls.isEmpty() ? nullptr : &m_operandsOfCalls[position]; }
    std::span<const ValueSetID> argumentsOf(const OperandsOfCall& operands) const LIFETIME_BOUND { return m_argumentsOfCalls.subspan(operands.firstArgument, operands.numberOfArguments); }
    std::span<const ValueFlowObjectID> boundFunctionsOf(const OperandsOfCall& operands) const LIFETIME_BOUND { return m_boundFunctionsOfCalls.subspan(operands.firstBoundFunction, operands.numberOfBoundFunctions); }
    std::span<const ValueFlowNodeID> firstNodesOfCallGroups() const LIFETIME_BOUND { return m_firstNodesOfCallGroups.span(); }
    std::span<const ValueFlowNodeID> nodesOfMarks() const LIFETIME_BOUND { return m_nodesOfMarks.span(); }
    ValueFlowAnswersAboutSets& prototypesOfSets() const LIFETIME_BOUND { return m_prototypesOfSets; }
    ValueFlowAnswersAboutSets& prototypesOfBlocks() const LIFETIME_BOUND { return m_prototypesOfBlocks; }
    ValueFlowAnswersAboutSets& chainsOfSets() const LIFETIME_BOUND { return m_chainsOfSets; }
    ValueFlowAnswersAboutSets& writtenUnderNames() const LIFETIME_BOUND { return m_writtenUnderNames; }

private:
    void collectNodesWritten(const ValueFlowResultInputs&);
    static void indexNodesWrittenUnder(const ValueFlowResultInputs&, NodesWrittenUnderName&);
    void collectCalls(const ValueFlowResultInputs&);

#if ASSERT_ENABLED
    const Program& m_program;
#endif
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
    mutable ValueFlowAnswersAboutSets m_prototypesOfBlocks;
    mutable ValueFlowAnswersAboutSets m_chainsOfSets;
    mutable ValueFlowAnswersAboutSets m_writtenUnderNames;
};

class ValueFlowHeapView {
    WTF_MAKE_NONCOPYABLE(ValueFlowHeapView);
public:
    explicit ValueFlowHeapView(const ValueFlowResultInputs&);
    ~ValueFlowHeapView();

    const ValueFlowHeapIndex& index() const LIFETIME_BOUND { return m_index; }
    ValueSetMaker& maker() { return m_maker; }
    ValueSetID escaped() const { return m_escaped; }
    ValueSetID lost() const { return m_lost; }
    ValueSetID possibleValuesOf(const ValueFlowOperand&);
    ValueSetID expand(ValueSetID);
    bool reaches(ValueSetID writtenThrough, ValueSetID objects) const;
    ValueSetID writtenUnder(ValueSetID objects, ValueFlowName);
    ValueSetID prototypesOf(ValueSetID objects);
    ValueSetID chainOf(ValueSetID objects);
    ValueSetID chainBuiltByProgram(ValueSetID objects);

    ValueSetID possibleValuesOf(ValueSetMaker&, const ValueFlowOperand&) const;
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

    void forEachSeenValueWrittenUnder(ValueFlowName name, NOESCAPE const Invocable<void(ValueSetID seenValue)> auto& functor) const
    {
        const NodesWrittenUnderName* nodes = m_index.findNodesWrittenUnder(name);
        if (!nodes)
            return;
        for (const WrittenNode& node : nodes->nodes)
            functor(node.seenValue);
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
    ValueSetID prototypesOfEachHeir(ValueSetMaker&, ValueSetID heirs) const;
    ValueSetID findWrittenUnder(ValueSetMaker&, ValueSetID objects, const NodesWrittenUnderName&) const;
    ValueSetID chainWalkedInFull(ValueSetMaker&, ValueSetID objects) const;

    const ValueFlowResultInputs& m_inputs;
    std::unique_ptr<ValueFlowHeapIndex> m_ownIndex;
    const ValueFlowHeapIndex& m_index;
    ValueSetMakerPool& m_makerPool;
    ValueSetMaker& m_maker;
    ValueSetID m_escaped;
    ValueSetID m_lost;
    ValueSetID m_escapedAndWritable;
    mutable ChainOfPlaceholder m_chainOfUnknownObject;
    mutable ChainOfPlaceholder m_chainOfUntrackedObject;
    mutable PrototypesOfPlaceholder m_prototypesOfUnknownObject;
    mutable PrototypesOfPlaceholder m_prototypesOfUntrackedObject;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_prototypesOfSet;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_chainOfSet;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_chainBuiltByProgramOfSet;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_storedIntoPrototypeLinkOf;
    Vector<WrittenNode> m_prototypeLinksStoredThroughComplements;
    bool m_hasIndexedStoresIntoPrototypeLinks { false };
};

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
    FunctionID function;
    ValueID site;
    const ValueFlowStatement* statement { nullptr };
    ValueSetID thisValue { ValueSets::nothing };
    Vector<ValueSetID, numberOfGroupedArguments> arguments;
    ValueSetID otherArguments { ValueSets::nothing };

    BuiltinCallMode mode() const;
    ValueSetID valueAt(BuiltinOperand, ValueSetMaker&) const;
};

using ValueFlowBuiltinApplicationFunctor = ScopedLambda<void(const ValueFlowBuiltinApplication&)>;
void forEachBuiltinApplication(const ValueFlowResultInputs&, ValueFlowHeapView&, NOESCAPE const ValueFlowBuiltinApplicationFunctor&);
void forEachBuiltinApplication(const ValueFlowResultInputs&, ValueFlowHeapView&, const BitVector& onlyOfBuiltins, NOESCAPE const ValueFlowBuiltinApplicationFunctor&);
void forEachBuiltinApplication(const ValueFlowResultInputs&, ValueFlowHeapView&, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, NOESCAPE const ValueFlowBuiltinApplicationFunctor&);
void forEachBuiltinApplicationOnAllThreads(const ValueFlowResultInputs&, const ValueFlowHeapView&, ValueSetMakers&, const BitVector& onlyOfBuiltins, ValueSetID onlyPassingAnyOf, NOESCAPE const ScopedLambda<void(unsigned worker, const ValueFlowBuiltinApplication&)>&);

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

struct ValueFlowUsesOfHeap;
struct ValueFlowUsesOfHeapDeleter {
    void operator()(ValueFlowUsesOfHeap*) const;
};
std::unique_ptr<ValueFlowUsesOfHeap, ValueFlowUsesOfHeapDeleter> computeUsesOfHeap(const ValueFlowResultInputs&);
void listStoresByLayout(const ValueFlowResultInputs&, ValueFlowUsesOfHeap&, const FlowResultTables&);
void computeLayoutsNeverHeldWeakly(const ValueFlowResultInputs&, ValueFlowHeapView&, const ValueFlowUsesOfHeap&, FlowResultTables&);
ValueFlowHeapView& heapViewOf(ValueFlowUsesOfHeap&);
ValueSetID objectsThatMayBeEnumerated(const ValueFlowUsesOfHeap&);
ValueSetID objectsWithAllKeysShown(const ValueFlowUsesOfHeap&);
ValueSetID objectsWithEnumerableKeysShown(const ValueFlowUsesOfHeap&);

struct ValueFlowConstructions {
    struct OfLayout {
        bool isFollowed { false };
        bool isNewTargetOfOtherFunctions { false };
        bool createsFieldsOnlyInListedOrders { false };
        Vector<PropertyKeyID> keysStoredBeforeObjectLeaks;
        Vector<FieldReference> fieldsStoredBeforeObjectLeaks;
        Vector<PropertyKeyID> keysStoredAtBirth;
        Vector<PropertyKeyID> keysStoredWithValueAtBirth;
        Vector<FieldReference> fieldsStoredAtBirth;
        Vector<FieldOrder> orders;
        Vector<PropertyKeyID> inheritedKeysThatStoresMayNotCreate;
    };

    bool isFollowed(LayoutID layout) const { return layouts[layout.index()].isFollowed; }
    bool isNewTargetOfOtherFunctions(LayoutID layout) const { return layouts[layout.index()].isNewTargetOfOtherFunctions; }
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
    bool seesOnlyFinishedObjects(FunctionID function) const { return functionsSeeingOnlyFinishedObjects.get(function.index()); }

    Vector<OfLayout> layouts;
    BitVector functionsSeeingOnlyFinishedObjects;
};

ValueFlowConstructions computeConstructions(const ValueFlowResultInputs&, ValueFlowHeapView&, const FlowResultTables&);
void computeLiteralKeyOrders(const ValueFlowResultInputs&, ValueFlowHeapView&, Vector<PropertyKeyKind>&& kindsOfKeys, FlowResultTables&);
// Whether the operation gives properties to the literal, between its NewObject and its FinishObject.
bool isPieceOfLiteral(const Value& operation, ValueID literal);

struct ValueFlowWriteUnderNewName {
    BuiltinID holder;
    StringID text;
    Witness witness;
};

// What the solve alone tells.
struct ValueFlowUsesOfLibraryCode {
    Vector<UnseenCodeRecord> unseenCode;
    Vector<UnseenCodeRecord> loadsOfMachineCode;
    Vector<BuiltinUseRecord> builtinUses;
    Vector<RuntimeFunctionID> libraryFunctionsLacked;
};
ValueFlowUsesOfLibraryCode findUsesOfLibraryCode(const ValueFlowResultInputs&);

// Hands out the part of the ledger that holds the places on the objects of the table.
Vector<ValueFlowWriteUnderNewName> computeBuiltinResults(const ValueFlowResultInputs&, ValueFlowUsesOfLibraryCode&&, FlowResultTables&, std::unique_ptr<LedgerPart>&);
void findHoldersWithListedKeys(const ValueFlowResultInputs&, const ValueFlowUsesOfHeap&, FlowBuiltinTables&);
void recordLockedWritesUnderNewNames(const Program&, TypeTable&, std::span<const ValueFlowWriteUnderNewName>, FlowResultTables&);
ValueFlowClassesOfBuiltinResults computeClassesOfBuiltinResults(const ValueFlowResultInputs&, const ValueFlowHeapIndex&);
ValueFlowStampedCalls computeCallResults(const ValueFlowResultInputs&, TypeTable&, FlowResultTables&);
void computeFunctionResults(const ValueFlowResultInputs&, ValueFlowStampedCalls&, FlowResultTables&);
void computeExecutionResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
BitVector computeCreatedSymbolResults(const ValueFlowResultInputs&, const FlowResultTables&);
void reportOperationsWithoutValue(const ValueFlowResultInputs&);
void computeClosuresMadeOnce(const ValueFlowResultInputs&, FlowResultTables&);
void computeOrderResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
void computeObjectResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);
void computeClassResults(const ValueFlowResultInputs&, FlowResultTables&);
inline LayoutID provenBaseOf(const FlowApplication& application, LayoutID id)
{
    const LayoutFlow& flow = application.layouts[id.index()].flow;
    return flow.baseState == BaseState::Layout ? flow.base : LayoutID();
}

ValueFlowFieldContents computeFieldResults(const ValueFlowResultInputs&, ValueFlowUsesOfHeap&, const ValueFlowConstructions&, FlowResultTables&);
const LedgerPart& ledgerOfFieldsIn(const ValueFlowUsesOfHeap&);
struct ValueFlowWritesToContainers;
struct ValueFlowWritesToContainersDeleter {
    void operator()(ValueFlowWritesToContainers*) const;
};
std::unique_ptr<ValueFlowWritesToContainers, ValueFlowWritesToContainersDeleter> collectWritesToContainers(const ValueFlowResultInputs&, const FlowResultTables&, TypeTable&);
ValueFlowContainerResults computeContainerResults(ValueFlowWritesToContainers&);
std::unique_ptr<ValueFlowNumbersOfValues> prepareNumbersOfPositions(const ValueFlowResultInputs&, ValueFlowHeapView&, const FlowResultTables&);
void computeNumbersOfPositions(ValueFlowNumbersOfValues&, const FlowResultTables&);
BuiltinClassID classOfFollowedContainer(const ValueFlowResultInputs&, ValueFlowObjectID);
void computeTypeResults(const ValueFlowResultInputs&, const ValueFlowFieldContents&, const LedgerPart& ledgerOfFields, TypeTable&, FlowResultTables&, const ListenerForSetsOutsideTypes*);
void computeLayoutsNeverStoredOtherTypes(const ValueFlowResultInputs&, const ValueFlowFieldContents&, TypeTable&, FlowResultTables&);
void replaceUnobservedDefinitions(const ValueFlowResultInputs&, TypeTable&, FlowResultTables&);
void computeAccessResults(const ValueFlowResultInputs&, TypeTable&, FlowResultTables&);
void computeStoreNamingResults(const ValueFlowResultInputs&, ValueFlowUsesOfHeap&, FlowResultTables&);
void computeSymbolConstantResults(const ValueFlowResultInputs&, FlowResultTables&);
void computeTypedResultResults(const ValueFlowResultInputs&, FlowResultTables&);
void computeDataNodeResults(const ValueFlowResultInputs&, const ValueFlowContainerResults&, TypeTable&, FlowResultTables&);
BitVector computeLayoutsOfDictionaries(const ValueFlowResultInputs&, TypeTable&, FlowResultTables&);
Type typeBehindDictionaries(const Program&, const BitVector& layoutsOfDictionaries, std::span<const std::pair<TypeID, TypeID>> ascendingTypes, const Type&, TypeArena&);
void computeSinkOperandResults(const ValueFlowResultInputs&, const ValueFlowStampedCalls&, FlowResultTables&);

inline bool isClassLayout(const Layout& layout)
{
    return layout.kind == LayoutKind::Instance && layout.constructor && !layout.has(LayoutFlags::IsConstructorFunction);
}

inline unsigned indexOfArrayElementsInType(const BuiltinTable& builtins)
{
    std::optional<unsigned> index;
    for (BuiltinSlotID id : builtins.at(BuiltinClasses::Array).slots) {
        const BuiltinSlot& slot = builtins.at(id);
        if (slot.indexInType && slot.contentsSlot == ContentsSlot::Element)
            index = *slot.indexInType;
    }
    return index.value();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

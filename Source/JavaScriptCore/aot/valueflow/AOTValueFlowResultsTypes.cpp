/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTPredictedValues.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTCFG.h"
#include "AOTClaims.h"
#include "AOTEffectsOfOperands.h"
#include "AOTEntryParameterIndexes.h"
#include "AOTFlowApply.h"
#include "AOTFlowNamespaces.h"
#include "AOTFlowRecords.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInductionVariables.h"
#include "AOTKnownClosures.h"
#include "AOTLedger.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPhaseTimes.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSharedMemo.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTTypePropagation.h"
#include "AOTValueFlowPositions.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <optional>
#include <tuple>
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned variablesPerTask = 256;

static DeadZone deadZoneOf(const LocationFlow& flow)
{
    return flow.initialization == InitializationState::Unknown ? DeadZone::MayBeObserved : DeadZone::IsNeverObserved;
}

// At 2 ** 53 a step of one is lost to rounding: 2 ** 53 + 1 lies halfway between two doubles and goes to the even one, 2 ** 53 again. So an integer that a double holds exactly is one still after a step of one.
// The bound is 2 ** 53 and not the largest safe integer, 2 ** 53 - 1: a step up from there is exact and leaves that range.
static std::optional<Type> typeAfterStepOfOne(Opcode opcode, std::span<const Type> operands)
{
    auto stepIn = [](const Type& type) -> int64_t {
        bool isStep = type.isInteger() && type.integers.isSingleValue() && std::abs(type.integers.minimum) == 1;
        return isStep ? type.integers.minimum : 0;
    };
    const Type* stepped = &operands[0];
    int64_t step = 0;
    if (opcode == Opcode::Increment)
        step = 1;
    if (opcode == Opcode::Decrement)
        step = -1;
    if (opcode == Opcode::Sub)
        step = -stepIn(operands[1]);
    if (opcode == Opcode::Add) {
        step = stepIn(operands[1]);
        if (!step) {
            step = stepIn(operands[0]);
            stepped = &operands[1];
        }
    }
    if (!step || !stepped->isInteger() || !stepped->integers.isSubsetOf(Range::exact()))
        return std::nullopt;
    auto afterStep = [&](int64_t integer) {
        return std::clamp(integer + step, -maxExactInteger, maxExactInteger);
    };
    return Type::integer({ afterStep(stepped->integers.minimum), afterStep(stepped->integers.maximum) });
}

static bool mayBeStepOfOne(Opcode opcode)
{
    return opcode == Opcode::Increment || opcode == Opcode::Decrement || opcode == Opcode::Add || opcode == Opcode::Sub;
}

namespace {

class NumbersOfPositions final : public ValueFlowNumbersOfValues {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NumbersOfPositions);
public:
    NumbersOfPositions(const ValueFlowResultInputs&, ValueFlowHeapView&, const FlowResultTables&);
    void followPositions(const FlowResultTables&);

    ValueFlowNumbers numbersOf(FunctionID, ValueID) const final;
    const ValueFlowNumbers* ofValuesOfMap(ValueFlowObjectID) const final;

    const ValueFlowNumbers* ofVariable(VariableID id) const { return find(id.index()); }
    const ValueFlowNumbers* ofEnvironmentField(LayoutID layout, uint32_t field) const { return find(m_firstField[layout.index()] + field); }
    const ValueFlowNumbers* ofEntryParameter(FunctionID function, uint32_t index) const { return find(m_firstEntryParameter[function.index()] + index); }
    const ValueFlowNumbers* ofResult(FunctionID function, uint32_t index) const { return find(m_firstResult[function.index()] + index); }
    std::optional<ValueFlowNumbers> ofCall(FunctionID caller, ValueID call) const;
    ValueFlowNumbers numbersOfStep(FunctionID, ValueID) const;

private:
    struct Writer {
        friend auto operator<=>(const Writer&, const Writer&) = default;

        uint32_t position;
        FunctionID function;
        ValueID value;
    };

    struct CallByValue {
        friend auto operator<=>(const CallByValue&, const CallByValue&) = default;

        FunctionID caller;
        ValueID site;
        ValueSetID callees;
    };

    struct ReadOfMaps {
        auto order() const { return std::tuple { caller, site }; }

        FunctionID caller;
        ValueID site;
        ValueSetID maps;
    };

    struct PassedValue {
        auto order() const { return std::tuple { parameter, value }; }

        ValueID parameter;
        ValueID value;
    };

    struct Evaluation {
        TypeArena& arena;
        Vector<uint32_t>* positionsRead { nullptr };
        unsigned stepsLeft { maxSteps };
        // What flow says of a place that holds primitives alone goes through casts and conversions whole, not only its numbers.
        bool tellsKindsApart { false };
    };

    class KnownTypes final : public KnownPositionTypes {
    public:
        KnownTypes(const NumbersOfPositions& numbers, Vector<uint32_t>& positionsAsked)
            : m_numbers(numbers)
            , m_positionsAsked(positionsAsked)
        {
        }

        std::optional<Type> find(const DeclaredType& declared) const final
        {
            auto found = m_numbers.m_positionsOfTypes.find(&declared);
            if (found == m_numbers.m_positionsOfTypes.end())
                return std::nullopt;
            m_positionsAsked.append(found->value);
            return m_numbers.typeAt(found->value, m_numbers.typeWithoutProof(declared));
        }

    private:
        const NumbersOfPositions& m_numbers;
        Vector<uint32_t>& m_positionsAsked;
    };

    struct FollowedPosition {
        uint32_t position;
        uint32_t firstWriter;
        uint32_t endOfWriters;
        uint32_t firstPositionRead { 0 };
        uint32_t endOfPositionsRead { 0 };
        uint32_t firstSharedRange { 0 };
        uint32_t endOfSharedRanges { 0 };
    };

    // What all calls of one set of many callees have in common. n calls of any of n functions are n * n pairs of a call and a callee: each round looks at the set once, and then at each call once.
    struct ManyCallees {
        ValueSetID set;
        Type result { };
        ValueFlowNumbers numbersOfResult { };
    };

    // The arguments that the calls of one set of many callees pass at one position.
    struct SharedRange {
        uint32_t firstPart { 0 };
        uint32_t endOfParts { 0 };
        ValueFlowNumbers arriving { };
        bool hasMoved { false };
    };

    struct PartOfSharedRange {
        uint32_t firstWriter;
        uint32_t endOfWriters;
        ValueFlowNumbers arriving { };
        Vector<uint32_t> positionsRead { };
    };

    struct LoopingFunction {
        uint32_t firstWriter;
        uint32_t endOfWriters;
        bool isTyped { false };
        Vector<uint32_t> positionsAsked { };
    };

    AOT_CLASS_THRESHOLD(unsigned, maxRounds, 6);
    AOT_CLASS_THRESHOLD(unsigned, maxDepth, 8);
    AOT_CLASS_THRESHOLD(unsigned, maxSteps, 256);
    AOT_CLASS_THRESHOLD(unsigned, maxTypingsOfLoops, 3);
    AOT_CLASS_THRESHOLD(size_t, minCalleesOfSharedSet, 16);
    static constexpr unsigned positionsPerTask = 64;
    static constexpr uint32_t writersPerPart = 64;

    const ValueFlowNumbers* find(uint32_t position) const { return m_isFollowed.quickGet(position) ? &m_numbers[position] : nullptr; }
    void startFromTypes();
    void collectCalls();
    void choosePositions(const FlowResultTables&);
    void collectWriters(const FlowResultTables&);
    void addSharedWriters();
    const ManyCallees* findManyCallees(ValueSetID) const;
    uint32_t positionOf(const ManyCallees& callees) const { return m_numbers.size() + static_cast<uint32_t>(&callees - m_manyCallees.span().data()); }
    void updateManyCallees(bool onlyWhereResultsHaveMoved);
    ValueFlowNumbers numbersArrivingFrom(TypeArena&, uint32_t firstWriter, uint32_t endOfWriters, Vector<uint32_t>* positionsRead, bool tellsKindsApart = false) const;
    void evaluateSharedRanges(bool isFirstRound);
    BitVector collectWritersOfMaps(ValueFlowHeapView&);
    std::optional<uint32_t> indexOfMap(ValueFlowObjectID) const;
    const ReadOfMaps* readOfMapsAt(FunctionID caller, ValueID site) const;
    void findValuesBehindEveryCheck(const FlowResultTables&);
    std::span<const CallByValue> callsAt(FunctionID caller, ValueID site) const;
    Type typeAt(uint32_t position, Type&& known) const;
    Type typeAt(Evaluation&, uint32_t position, Type&& known) const;
    std::optional<Type> typeOfCall(Evaluation&, FunctionID caller, ValueID site) const;
    Type typeOf(Evaluation&, FunctionID, ValueID, unsigned depth) const;
    Type typeWithoutProof(const DeclaredType& declared) const { return m_inputs.program.at(m_inputs.program.provenTypeOf(declared)); }
    bool typeWritersByRules(TypeArena&, FunctionID, std::span<const uint32_t> writers, Vector<uint32_t>& positionsAsked);
    bool typeLoopsByRules();
    void assumeIntegersOfCounters();
    BitVector findPlacesTakenAsValues(const BitVector& places) const;
    std::optional<uint32_t> staticFieldNamedBy(const Function&, const Value& access) const;
    bool runRound();

    const ValueFlowResultInputs& m_inputs;
    Vector<uint32_t> m_firstField;
    Vector<uint32_t> m_firstEntryParameter;
    Vector<uint32_t> m_firstResult;
    uint32_t m_firstMap { 0 };
    Vector<ValueFlowObjectID> m_maps;
    Vector<Writer> m_writersOfMaps;
    Vector<ValueFlowNumbers> m_numbersInConstantsOfMaps;
    Vector<ReadOfMaps> m_readsOfMaps;
    Vector<ValueFlowNumbers> m_numbers;
    BitVector m_isFollowed;
    BitVector m_isStaticFieldReadBehindItsStore;
    Vector<CallByValue> m_calls;
    Vector<ManyCallees> m_manyCallees;
    Vector<SharedRange> m_sharedRanges;
    Vector<PartOfSharedRange> m_partsOfSharedRanges;
    Vector<uint32_t> m_sharedRangesOfPositions;
    Vector<uint8_t> m_hasOtherEntry;
    bool m_someCallEntersAnything { false };
    Vector<BitVector> m_valuesBehindChecks;
    Vector<Vector<PassedValue>> m_passedValues;
    BitVector m_holdsPrimitivesAlone;
    Vector<uint16_t> m_primitives;
    Vector<Writer> m_writers;
    Vector<FollowedPosition> m_followed;
    Vector<ValueFlowNumbers> m_numbersByRules;
    Vector<uint32_t> m_writersInLoopingFunctions;
    Vector<LoopingFunction> m_loopingFunctions;
    Vector<Vector<uint32_t>> m_positionsReadInTask;
    BitVector m_hasMovedInLastRound;
    BitVector m_hasMovedSinceTyping;
    Vector<uint8_t> m_rulesHaveMoved;
    bool m_someRulesHaveMoved { false };
    uint64_t m_numberOfRounds { 0 };
    uint64_t m_numberOfPositionsLookedAt { 0 };
    uint64_t m_numberOfTypings { 0 };
    uint64_t m_numberOfFunctionsTyped { 0 };
    HashMap<const DeclaredType*, uint32_t> m_positionsOfTypes;
    BitVector m_mapsWithOtherWriter;
};

struct LocationsStoredUndefined {
    BitVector variables;
    HashSet<uint32_t> environmentFields;
};

class QuestionsAnsweredWithNo {
    WTF_MAKE_NONCOPYABLE(QuestionsAnsweredWithNo);
public:
    QuestionsAnsweredWithNo(const ParallelLoops& loops, size_t mostQuestions)
        : m_mask(std::bit_ceil(std::max<size_t>(1024, mostQuestions * 2)) - 1)
        , m_slots(MallocSpan<std::atomic<uint64_t>>::malloc((m_mask + 1) * sizeof(std::atomic<uint64_t>)))
    {
        constexpr size_t placesPerTask = 32 * KB;
        forEachIndexInParallel(loops, static_cast<unsigned>((m_mask + placesPerTask) / placesPerTask), [&](unsigned task) {
            for (size_t place = task * placesPerTask, end = std::min(m_mask + 1, place + placesPerTask); place < end; ++place)
                new (NotNull, &m_slots[place]) std::atomic<uint64_t>(0);
        });
    }

    bool contains(uint64_t question) const
    {
        for (size_t place = placeOf(question), steps = 0; steps < mostSteps; place = (place + 1) & m_mask, ++steps) {
            uint64_t held = m_slots[place].load(std::memory_order_relaxed);
            if (held == question + 1)
                return true;
            if (!held)
                return false;
        }
        return false;
    }

    void add(uint64_t question)
    {
        for (size_t place = placeOf(question), steps = 0; steps < mostSteps; place = (place + 1) & m_mask, ++steps) {
            uint64_t held = 0;
            if (m_slots[place].compare_exchange_strong(held, question + 1, std::memory_order_relaxed) || held == question + 1)
                return;
        }
    }

private:
    AOT_CLASS_THRESHOLD(size_t, mostSteps, 16);

    size_t placeOf(uint64_t question) const
    {
        uint64_t hash = question * 0x9e3779b97f4a7c15ull;
        return (hash ^ hash >> 29) & m_mask;
    }

    size_t m_mask;
    MallocSpan<std::atomic<uint64_t>> m_slots;
};

struct ValuesWithoutProof {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ValuesWithoutProof);

    ValuesWithoutProof(const ParallelLoops& loops, size_t mostQuestions)
        : inside(loops, mostQuestions)
        , outsideFamily(loops, mostQuestions / 8)
    {
    }

    QuestionsAnsweredWithNo inside;
    QuestionsAnsweredWithNo outsideFamily;
};

} // anonymous namespace

NumbersOfPositions::NumbersOfPositions(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const FlowResultTables& tables)
    : m_inputs(inputs)
    , m_hasOtherEntry(FillWith { }, inputs.program.functions.size(), static_cast<uint8_t>(0))
{
    const Program& program = inputs.program;
    uint32_t next = program.variables.size();
    auto take = [&](size_t numberOfPositions) {
        return std::exchange(next, static_cast<uint32_t>(next + numberOfPositions));
    };
    m_firstField = Vector<uint32_t>(program.layouts.size(), [&](size_t index) { return take(program.layouts[index].isEnvironment() ? program.layouts[index].fields.size() : 0); });
    m_firstEntryParameter = Vector<uint32_t>(program.functions.size(), [&](size_t index) { return take(program.functions[index].numberOfEntryParameters()); });
    m_firstResult = Vector<uint32_t>(program.functions.size(), [&](size_t index) { return take(program.functions[index].results.size()); });
    m_firstMap = next;
    ValueSetID filledAsRecordsSay = inputs.solver().possibleValuesOf(inputs.bodies().nodes().ofHost(ValueFlowHostNode::MapsFilledAsRecordsSay));
    m_maps = collectInParallel<ValueFlowObjectID>(inputs.loops(), inputs.universe().numberOfSiteObjects(), [&](unsigned position, Vector<ValueFlowObjectID>& maps) {
        ValueFlowObjectID object = inputs.universe().siteObjectAt(position);
        if (classOfFollowedContainer(inputs, object) != BuiltinClasses::Map || inputs.sets().contains(filledAsRecordsSay, object))
            return;
        if (program.at(inputs.universe().functionOf(object)).at(inputs.universe().siteOf(object)->value).opcode == Opcode::Construct)
            maps.append(object);
    });
    next += m_maps.size();
    m_numbers = Vector<ValueFlowNumbers>(next, [](size_t) { return ValueFlowNumbers::any(); });
    m_isFollowed = BitVector(next);
    m_holdsPrimitivesAlone = BitVector(next);
    m_hasMovedInLastRound = BitVector(next);
    m_hasMovedSinceTyping = BitVector(next);
    m_primitives = Vector<uint16_t>(FillWith { }, next, static_cast<uint16_t>(0));

    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    runTasksInParallel(inputs.loops(), {
        [&] {
            startFromTypes();
            findValuesBehindEveryCheck(tables);
            times.didFinish("numbers: values that rest on checks"_s);
            collectCalls();
            times.didFinish("numbers: calls"_s);
        },
        [&] {
            PhaseTimes timesBeside(inputs.build, PassID::AnalyzeFlow);
            m_mapsWithOtherWriter = collectWritersOfMaps(heap);
            timesBeside.didFinish("numbers: writers of maps, beside values that rest on checks and calls"_s);
        },
    });
}

void NumbersOfPositions::followPositions(const FlowResultTables& tables)
{
    PhaseTimes times(m_inputs.build, PassID::AnalyzeFlow);
    for (uint32_t variable = 0; variable < m_inputs.program.variables.size(); ++variable) {
        if (m_inputs.program.variables[variable].owner && isStoredIntegersThatOnlyChecksBound(m_inputs.ledger->writersOf(Place::variable(VariableID(variable)))))
            m_numbers[variable].integers = Range::full();
    }
    choosePositions(tables);
    times.didFinish("numbers: positions chosen"_s);
    for (uint32_t index = 0; index < m_maps.size(); ++index) {
        if (!m_mapsWithOtherWriter.quickGet(index))
            m_isFollowed.quickSet(m_firstMap + index);
    }
    m_mapsWithOtherWriter = { };
    collectWriters(tables);
    updateManyCallees(false);
    assumeIntegersOfCounters();
    times.didFinish("numbers: writers"_s);
    for (unsigned typing = 0; typing <= maxTypingsOfLoops; ++typing) {
        for (unsigned round = 0; round < maxRounds && runRound(); ++round) { }
        if (typing == maxTypingsOfLoops || !typeLoopsByRules())
            break;
    }
    if (m_inputs.build.options().logging.logsPhaseTimes) [[unlikely]]
        dataLogLn("AOT flow: numbers: ", m_numberOfRounds, " rounds over ", m_followed.size(), " positions have looked at ", m_numberOfPositionsLookedAt, "; ", m_numberOfTypings, " typings of ", m_loopingFunctions.size(), " functions with a loop have typed ", m_numberOfFunctionsTyped);
    m_positionsReadInTask.clear();
    m_partsOfSharedRanges.clear();
    m_loopingFunctions.clear();
    times.didFinish("numbers: rounds"_s);
}

void NumbersOfPositions::startFromTypes()
{
    const Program& program = m_inputs.program;
    auto start = [&](uint32_t position, const DeclaredType* declared) {
        if (!declared || !declared->written() || declared->isExpected())
            return;
        m_numbers[position] = { };
        m_numbers[position].add(program.at(*declared->established()));
    };
    forEachIndexInParallel(m_inputs.loops(), program.variables.size(), [&](unsigned index) {
        start(index, &program.variables[index].declared);
    });
    forEachIndexInParallel(m_inputs.loops(), program.layouts.size(), [&](unsigned index) {
        const Layout& layout = program.layouts[index];
        for (uint32_t field = 0; layout.isEnvironment() && field < layout.fields.size(); ++field)
            start(m_firstField[index] + field, &layout.fields[field].declared);
    });
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        for (uint32_t parameter = 0; parameter < function.numberOfEntryParameters(); ++parameter)
            start(m_firstEntryParameter[index] + parameter, function.declaredEntryParameter(parameter));
        for (uint32_t result = 0; result < function.results.size(); ++result)
            start(m_firstResult[index] + result, &function.results[result]);
    });
}

void NumbersOfPositions::findValuesBehindEveryCheck(const FlowResultTables& tables)
{
    const Program& program = m_inputs.program;
    m_valuesBehindChecks = Vector<BitVector>(program.functions.size());
    m_passedValues = Vector<Vector<PassedValue>>(program.functions.size());
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || (!m_inputs.isReached(FunctionID(index)) && !isUnsolvedCode(m_inputs, tables, FunctionID(index))))
            return;
        Vector<ValueID, 16> checks;
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                if (function.at(id).opcode == Opcode::Check)
                    checks.append(id);
            }
        }
        if (checks.isEmpty())
            return;
        CFG cfg(function);
        m_valuesBehindChecks[index] = valuesBehind(function, cfg, checks.span());

        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            BlockID block(blockIndex);
            const Block& contents = function.at(block);
            if (block == entryBlock || contents.isHandlerBlock())
                continue;
            for (uint32_t parameter = 0; parameter < contents.parameters.size(); ++parameter) {
                if (!m_valuesBehindChecks[index].get(contents.parameters[parameter].index()))
                    continue;
                for (Edge edge : cfg.incomingEdges(block))
                    m_passedValues[index].append({ contents.parameters[parameter], valuePassedOn(function, edge, parameter) });
            }
        }
        AOT::sort(m_passedValues[index], { }, &PassedValue::order);
    });
}

void NumbersOfPositions::collectCalls()
{
    const ValueFlowSolver& solver = m_inputs.solver();
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueSets& sets = m_inputs.sets();
    ValueSetID closuresAndPrimitives = BorrowedValueSetMaker(m_inputs.makerPool()).get().unite(universe.closures(), universe.primitives());
    std::atomic<bool> someCallEntersAnything { false };
    // n calls of any of n functions ask the same of the same two sets.
    struct SetInGroup {
        ValueSetID set;
        ValueSetID group;
        bool holdsEveryClosure { false };
    };
    SharedMemo<SetInGroup> setsInGroups;
    SharedMemo<ValueSetID> groupsWithOtherEntry;
    auto hashOf = [](ValueSetID set, ValueSetID group) {
        return static_cast<uint32_t>((static_cast<uint64_t>(set.index()) << 32 | group.index()) * 0x9e3779b97f4a7c15ull >> 32);
    };
    m_calls = collectInParallel<CallByValue>(m_inputs.loops(), solver.numberOfStatements(), [&](unsigned index, Vector<CallByValue>& calls) {
        ValueFlowStatementID id(index);
        m_inputs.prefetchMadeNodesAskedForBy(index + 16, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), false);
        const ValueFlowStatement& statement = solver.statementAt(id);
        bool isMadeByRule = solver.isMadeByRule(id);
        bool hasRun = isMadeByRule ? !solver.isAtRest(id) : !statement.function || m_inputs.isReached(statement.function);
        if (!hasRun)
            return;

        bool callsClosuresAlone = false;
        std::array<ValueSetID, 2> calleeSets { ValueSets::nothing, ValueSets::nothing };
        if (!isMadeByRule && statement.kind == ValueFlowStatementKind::Operation && statement.function && statement.site && (statement.opcode == Opcode::Call || statement.opcode == Opcode::Construct)) {
            const ValueFlowOperand& callee = m_inputs.bodies().operandsOf(statement)[0];
            calleeSets = { callee.node ? solver.possibleValuesOf(callee.node) : ValueSets::nothing, callee.constant ? callee.constant : ValueSets::nothing };
            callsClosuresAlone = std::ranges::all_of(calleeSets, [&](ValueSetID set) {
                return sets.isSubset(set, closuresAndPrimitives);
            });
        }
        auto holdsEveryClosureOf = [&](ValueSetID group, ValueSetID set) {
            bool holdsAll = true;
            sets.forEachMemberInLine(set, [&](ValueFlowObjectID member) {
                holdsAll &= !universe.closureFunction(member) || sets.contains(group, member);
            });
            return holdsAll;
        };
        auto holdsEveryCallee = [&](ValueSetID group) {
            return std::ranges::all_of(calleeSets, [&](ValueSetID set) {
                if (sets.size(set) < minCalleesOfSharedSet)
                    return holdsEveryClosureOf(group, set);
                auto isWanted = [&](const SetInGroup& entry) {
                    return entry.set == set && entry.group == group;
                };
                const SetInGroup* known = setsInGroups.find(hashOf(set, group), isWanted);
                if (!known)
                    known = &setsInGroups.add(hashOf(set, group), { set, group, holdsEveryClosureOf(group, set) }, isWanted);
                return known->holdsEveryClosure;
            });
        };
        ValueFlowCallKind ownKind = statement.opcode == Opcode::Construct ? ValueFlowCallKind::Construct : ValueFlowCallKind::Call;
        m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            if (made == id)
                return;
            if (ValueSets::isComplement(key.set)) {
                someCallEntersAnything.store(true, std::memory_order_relaxed);
                return;
            }
            if (callsClosuresAlone && key.callKind() == ownKind && holdsEveryCallee(key.set)) {
                calls.append({ statement.function, statement.site, key.set });
                return;
            }
            auto isSameGroup = [&](ValueSetID group) {
                return group == key.set;
            };
            bool isLarge = sets.size(key.set) >= minCalleesOfSharedSet;
            if (isLarge && groupsWithOtherEntry.find(hashOf(key.set, key.set), isSameGroup))
                return;
            sets.forEachMember(key.set, [&](ValueFlowObjectID member) {
                if (FunctionID function = universe.closureFunction(member))
                    AtomicRef(m_hasOtherEntry[function.index()]).store(1, std::memory_order_relaxed);
            });
            if (isLarge)
                groupsWithOtherEntry.add(hashOf(key.set, key.set), ValueSetID(key.set), isSameGroup);
        });
    });
    m_someCallEntersAnything = someCallEntersAnything.load(std::memory_order_relaxed);
    sortAndRemoveDuplicates(m_inputs.loops(), m_calls);

    Vector<ValueSetID> setsOfManyCallees;
    for (const CallByValue& call : m_calls) {
        if (sets.size(call.callees) >= minCalleesOfSharedSet)
            setsOfManyCallees.append(call.callees);
    }
    AOT::sort(setsOfManyCallees);
    removeRepeatedElements(setsOfManyCallees);
    m_manyCallees = setsOfManyCallees.map<Vector<ManyCallees>>([](ValueSetID set) { return ManyCallees { set }; });
    m_hasMovedInLastRound = BitVector(m_numbers.size() + m_manyCallees.size());
}

const NumbersOfPositions::ManyCallees* NumbersOfPositions::findManyCallees(ValueSetID set) const
{
    auto found = std::ranges::lower_bound(m_manyCallees, set, { }, &ManyCallees::set);
    return found != m_manyCallees.end() && found->set == set ? &*found : nullptr;
}

// Brings what is kept for each set of many callees up to date with m_numbers.
void NumbersOfPositions::updateManyCallees(bool onlyWhereResultsHaveMoved)
{
    const Program& program = m_inputs.program;
    Vector<uint8_t> hasMoved(FillWith { }, m_manyCallees.size(), static_cast<uint8_t>(0));
    forEachIndexInParallel(m_inputs.loops(), m_manyCallees.size(), [&](unsigned index) {
        ManyCallees& callees = m_manyCallees[index];
        auto forEachCallee = [&](const auto& functor) {
            m_inputs.sets().forEachMember(callees.set, [&](ValueFlowObjectID member) {
                functor(m_inputs.universe().closureFunction(member));
            });
        };
        if (onlyWhereResultsHaveMoved) {
            forEachCallee([&](FunctionID id) {
                if (id && program.at(id).results.size() == 1 && m_hasMovedInLastRound.quickGet(m_firstResult[id.index()]))
                    hasMoved[index] = 1;
            });
            if (!hasMoved[index])
                return;
        }
        ScratchTypeArena arena(program.types);
        callees.result = { };
        callees.numbersOfResult = { };
        forEachCallee([&](FunctionID id) {
            bool isUnsplitCoroutine = id && program.at(id).isUnsplitCoroutine();
            bool hasOneResult = id && program.at(id).results.size() == 1;
            if (hasOneResult && !isUnsplitCoroutine)
                callees.result = callees.result.unionWith(typeAt(m_firstResult[id.index()], Type::anything()), arena);
            else
                callees.result = Type::anything();
            if (!isUnsplitCoroutine)
                callees.numbersOfResult.unite(hasOneResult ? m_numbers[m_firstResult[id.index()]] : ValueFlowNumbers::any());
        });
        // Nothing in it belongs to the arena.
        RELEASE_ASSERT(!callees.result.layouts && callees.result.classes.isEmpty());
    });
    for (uint32_t index = 0; index < hasMoved.size(); ++index) {
        if (hasMoved[index])
            m_hasMovedInLastRound.quickSet(positionOf(m_manyCallees[index]));
    }
}

void NumbersOfPositions::choosePositions(const FlowResultTables& tables)
{
    const Program& program = m_inputs.program;
    const FlowApplication& application = *tables.application;
    const ValueFlowProgramNodes& nodes = m_inputs.bodies().nodes();
    const ValueSets& sets = m_inputs.sets();
    BorrowedValueSetMaker borrowedMaker(m_inputs.makerPool());
    ValueSetMaker& maker = borrowedMaker.get();
    std::array<ValueFlowObjectID, 3> kindsOfNumbers { ValueFlowUniverse::primitive(PrimitiveSet::Integer), ValueFlowUniverse::primitive(PrimitiveSet::WholeNumber), ValueFlowUniverse::primitive(PrimitiveSet::FractionalNumber) };
    ValueSetID numbers = maker.of(kindsOfNumbers);
    ValueFlowPositionCounts counts(nullptr);
    struct Followed {
        uint32_t position;
        bool holdsPrimitivesAlone;
    };
    ValueSetMakers makers(m_inputs);
    constexpr unsigned indicesPerTask = 1024;
    auto forEachIndex = [&](unsigned count, const auto& functor) {
        unsigned numberOfTasks = (count + indicesPerTask - 1) / indicesPerTask;
        Vector<Vector<Followed>> followedOfTask(numberOfTasks);
        forEachIndexInParallel(m_inputs.loops(), numberOfTasks, [&](unsigned task, unsigned worker) {
            ScratchTypeArena arena(program.types);
            ValueFlowPositions positions(m_inputs, arena, counts, 0, &makers.of(worker));
            auto follow = [&](uint32_t position, ValueFlowNodeID node) {
                ValueSetID held = m_inputs.solver().possibleValuesOf(node);
                if (!ValueSets::isComplement(held) && !sets.meet(held, numbers))
                    return;
                ValueFlowPositionResult result = positions.resultAmongAllValues(held);
                bool holdsPrimitivesAlone = result.proof == ValueFlowProof::Proven && result.narrowed && !result.narrowed->hasObjects();
                followedOfTask[task].append({ position, holdsPrimitivesAlone });
                if (holdsPrimitivesAlone)
                    m_primitives[position] = result.narrowed->primitives | (result.narrowed->stringLiterals.isEmpty() ? 0 : Primitive::String) | (result.narrowed->symbolLiterals.isEmpty() ? 0 : Primitive::Symbol);
            };
            for (unsigned index = task * indicesPerTask; index < std::min(count, (task + 1) * indicesPerTask); ++index)
                functor(follow, index);
        });
        for (const Vector<Followed>& followedInTask : followedOfTask) {
            for (Followed followed : followedInTask) {
                m_isFollowed.quickSet(followed.position);
                if (followed.holdsPrimitivesAlone)
                    m_holdsPrimitivesAlone.quickSet(followed.position);
            }
        }
    };
    auto namesEveryWriter = [](const LocationFlow& flow) {
        return flow.writers == WriterState::AllKnown && flow.storeNaming == StoreNaming::EveryStoreNamesLocation;
    };
    auto everyStoreThroughObjectSpellsItsValue = [&](VariableID id) {
        return std::ranges::all_of(m_inputs.ledger->writersOf(Place::variable(id)).seen().predicted(), [&](const Write& write) {
            return write.traits.contains(WriteTrait::SpellsItsValue);
        });
    };

    m_isStaticFieldReadBehindItsStore = BitVector(program.variables.size());
    for (unsigned index = 0; index < program.variables.size(); ++index) {
        if (program.variables[index].owner && application.variables[index].flow.initialization == InitializationState::StoredBeforeEveryRead)
            m_isStaticFieldReadBehindItsStore.quickSet(index);
    }
    forEachIndex(program.variables.size(), [&](const auto& follow, unsigned index) {
        const LocationFlow& flow = application.variables[index].flow;
        if (namesEveryWriter(flow) || (program.variables[index].owner && flow.writers == WriterState::AllKnown && everyStoreThroughObjectSpellsItsValue(VariableID(index))))
            follow(index, nodes.variable(VariableID(index)));
    });
    forEachIndex(program.layouts.size(), [&](const auto& follow, unsigned index) {
        const Layout& layout = program.layouts[index];
        for (uint32_t field = 0; layout.isEnvironment() && field < layout.fields.size(); ++field) {
            if (namesEveryWriter(application.layouts[index].fields[field].flow.location))
                follow(m_firstField[index] + field, nodes.environmentField(LayoutID(index), field));
        }
    });
    forEachIndex(program.functions.size(), [&](const auto& follow, unsigned index) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        if (!m_inputs.isReached(id) || !function.hasBody())
            return;
        ValueFlowObjectID closure = m_inputs.universe().closureOf(id);
        bool everyEntryIsCall = !m_someCallEntersAnything && !m_hasOtherEntry[index] && !function.isModuleBody() && tables.functions.functions[index].callers == CallerState::AllKnown
            && !m_inputs.hasEscaped(closure) && !m_inputs.isLost(closure);
        for (uint32_t parameter = 0; parameter < function.numberOfEntryParameters(); ++parameter) {
            EntryParameter kind = function.entryParameterKind(parameter);
            if (kind == EntryParameter::Capture || (kind == EntryParameter::Parameter && everyEntryIsCall))
                follow(m_firstEntryParameter[index] + parameter, nodes.entryParameter(id, parameter));
        }
        if (function.results.size() == 1 && !function.isUnsplitCoroutine())
            follow(m_firstResult[index], nodes.result(id, 0));
    });
}

// Appends, for each set of many callees and each argument position, the arguments that calls of the set pass there. Each parameter that they reach names the range.
void NumbersOfPositions::addSharedWriters()
{
    const Program& program = m_inputs.program;
    struct RangeKey {
        uint32_t set;
        uint32_t argument;
    };
    using Argument = std::tuple<uint32_t, uint32_t, FunctionID, ValueID>;
    Vector<Argument> arguments;
    for (const CallByValue& call : m_calls) {
        if (!findManyCallees(call.callees))
            continue;
        std::span<const ValueID> operands = program.at(call.caller).at(call.site).variadicOperands();
        for (uint32_t argument = 0; argument < operands.size(); ++argument)
            arguments.append({ call.callees.index(), argument, call.caller, operands[argument] });
    }
    if (arguments.isEmpty())
        return;
    AOT::sort(arguments);
    removeRepeatedElements(arguments);

    Vector<RangeKey> ranges;
    for (auto [set, argument, function, value] : arguments) {
        bool beginsRange = ranges.isEmpty() || ranges.last().set != set || ranges.last().argument != argument;
        if (beginsRange) {
            ranges.append({ set, argument });
            m_sharedRanges.append({ static_cast<uint32_t>(m_partsOfSharedRanges.size()), static_cast<uint32_t>(m_partsOfSharedRanges.size()) });
        }
        if (beginsRange || m_partsOfSharedRanges.last().endOfWriters - m_partsOfSharedRanges.last().firstWriter == writersPerPart) {
            m_partsOfSharedRanges.append({ static_cast<uint32_t>(m_writers.size()), static_cast<uint32_t>(m_writers.size()) });
            m_sharedRanges.last().endOfParts = m_partsOfSharedRanges.size();
        }
        m_writers.append({ UINT32_MAX, function, value });
        m_partsOfSharedRanges.last().endOfWriters = m_writers.size();
    }

    // The ranges that reach each followed parameter.
    Vector<std::pair<uint32_t, uint32_t>> rangesOfPositions;
    for (uint32_t first = 0; first < ranges.size();) {
        uint32_t end = first;
        while (end < ranges.size() && ranges[end].set == ranges[first].set)
            ++end;
        m_inputs.sets().forEachMember(ValueSetID(ranges[first].set), [&](ValueFlowObjectID member) {
            FunctionID id = m_inputs.universe().closureFunction(member);
            if (!id)
                return;
            const Function& callee = program.at(id);
            uint32_t firstPosition = m_firstEntryParameter[id.index()] + callee.firstParameterIndex();
            for (uint32_t range = first; range < end && ranges[range].argument < callee.numberOfPositionalParameters(); ++range) {
                if (m_isFollowed.quickGet(firstPosition + ranges[range].argument))
                    rangesOfPositions.append({ firstPosition + ranges[range].argument, range });
            }
        });
        first = end;
    }
    AOT::sort(rangesOfPositions);
    removeRepeatedElements(rangesOfPositions);

    size_t next = 0;
    for (FollowedPosition& followed : m_followed) {
        followed.firstSharedRange = m_sharedRangesOfPositions.size();
        for (; next < rangesOfPositions.size() && rangesOfPositions[next].first == followed.position; ++next)
            m_sharedRangesOfPositions.append(rangesOfPositions[next].second);
        followed.endOfSharedRanges = m_sharedRangesOfPositions.size();
    }
    RELEASE_ASSERT(next == rangesOfPositions.size());
}

void NumbersOfPositions::collectWriters(const FlowResultTables& tables)
{
    const Program& program = m_inputs.program;
    m_writers = collectInParallel<Writer>(m_inputs.loops(), program.functions.size(), [&](unsigned index, Vector<Writer>& writers) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        if (!function.hasBody() || (!m_inputs.isReached(id) && !isUnsolvedCode(m_inputs, tables, id)))
            return;
        auto add = [&](uint32_t position, ValueID value) {
            if (m_isFollowed.quickGet(position))
                writers.append({ position, id, value });
        };
        bool holdsWriter = m_inputs.bodies().holdsAnyOf(id, notedOperations({ ValueFlowNotedOperation::StoreToLocation, ValueFlowNotedOperation::NewObject, ValueFlowNotedOperation::NewClosure, ValueFlowNotedOperation::CallDirect }));
        for (const Value& value : holdsWriter ? function.values.span() : std::span<const Value> { }) {
            switch (value.opcode) {
            case Opcode::PutModuleVariable:
                add(value.variable().index(), value.valueOperand());
                break;
            case Opcode::PutEnvironmentField:
                add(m_firstField[value.layout().index()] + value.field(), value.valueOperand());
                break;
            case Opcode::NewObject: {
                if (!program.at(value.layout()).isEnvironment())
                    break;
                auto fields = value.fields();
                for (uint32_t field = 0; field < fields.size(); ++field)
                    add(m_firstField[value.layout().index()] + fields[field], value.operand(OperandIndex::NewObject::values + field));
                break;
            }
            case Opcode::NewClosure:
            case Opcode::CallDirect: {
                std::span<const ValueID> operands = value.variadicOperands();
                for (uint32_t operand = 0; operand < operands.size(); ++operand)
                    add(m_firstEntryParameter[value.function().index()] + operand, operands[operand]);
                break;
            }
            default:
                break;
            }
        }
        if (function.results.size() != 1)
            return;
        for (const Block& block : function.blocks) {
            const Terminator& terminator = block.terminator;
            if (terminator.kind == TerminatorKind::ReturnCall)
                add(m_firstResult[id.index()], terminator.callOperand());
            if (terminator.kind == TerminatorKind::Return && terminator.valueOperand())
                add(m_firstResult[id.index()], terminator.valueOperand());
        }
    });
    // The same arguments reach every callee of a call. With many callees they are listed once for each set of callees, below: n calls of any of n functions would be n * n writers.
    auto sharesWriters = [&](const CallByValue& call) {
        return m_inputs.sets().size(call.callees) >= minCalleesOfSharedSet;
    };
    m_writers.appendVector(collectInParallel<Writer>(m_inputs.loops(), m_calls.size(), [&](unsigned index, Vector<Writer>& writers) {
        const CallByValue& call = m_calls[index];
        if (sharesWriters(call))
            return;
        std::span<const ValueID> arguments = program.at(call.caller).at(call.site).variadicOperands();
        m_inputs.sets().forEachMember(call.callees, [&](ValueFlowObjectID member) {
            FunctionID id = m_inputs.universe().closureFunction(member);
            if (!id)
                return;
            const Function& callee = program.at(id);
            uint32_t first = m_firstEntryParameter[id.index()] + callee.firstParameterIndex();
            for (uint32_t position = 0; position < std::min<size_t>(arguments.size(), callee.numberOfPositionalParameters()); ++position) {
                if (m_isFollowed.quickGet(first + position))
                    writers.append({ first + position, call.caller, arguments[position] });
            }
        });
    }));
    for (uint32_t variable = 0; variable < program.variables.size(); ++variable) {
        if (!program.variables[variable].owner || !m_isFollowed.quickGet(variable))
            continue;
        for (const Write& write : m_inputs.ledger->writersOf(Place::variable(VariableID(variable))).seen().predicted()) {
            if (write.traits.contains(WriteTrait::NamesPlace))
                continue;
            const Value& store = program.at(write.function).at(write.site);
            m_writers.append({ variable, write.function, store.operand(*indexOfValueStoredIntoProperty(store.opcode)) });
        }
    }
    m_writers.appendVector(std::exchange(m_writersOfMaps, { }));
    sortAndRemoveDuplicates(m_inputs.loops(), m_writers);

    uint32_t writer = 0;
    for (size_t position : m_isFollowed) {
        uint32_t first = writer;
        while (writer < m_writers.size() && m_writers[writer].position == position)
            ++writer;
        m_followed.append({ static_cast<uint32_t>(position), first, writer });
    }
    RELEASE_ASSERT(writer == m_writers.size());
    addSharedWriters();

    m_numbersByRules = Vector<ValueFlowNumbers>(m_writers.size(), [](size_t) { return ValueFlowNumbers::any(); });
    m_rulesHaveMoved = Vector<uint8_t>(FillWith { }, m_writers.size(), static_cast<uint8_t>(0));
    BitVector hasWriter(program.functions.size());
    for (const Writer& entry : m_writers)
        hasWriter.quickSet(entry.function.index());
    Vector<uint32_t> functionsWithLoop = findIndicesInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index) {
        return hasWriter.quickGet(index) && m_inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::LoopHint }));
    });
    BitVector hasLoop(program.functions.size());
    for (uint32_t index : functionsWithLoop)
        hasLoop.quickSet(index);
    for (uint32_t index = 0; index < m_writers.size(); ++index) {
        if (hasLoop.quickGet(m_writers[index].function.index()))
            m_writersInLoopingFunctions.append(index);
    }
    parallelSort(m_inputs.loops(), m_writersInLoopingFunctions.mutableSpan(), [&](uint32_t a, uint32_t b) {
        return std::tuple { m_writers[a].function, a } < std::tuple { m_writers[b].function, b };
    });
    for (uint32_t first = 0; first < m_writersInLoopingFunctions.size();) {
        uint32_t end = first;
        while (end < m_writersInLoopingFunctions.size() && m_writers[m_writersInLoopingFunctions[end]].function == m_writers[m_writersInLoopingFunctions[first]].function)
            ++end;
        m_loopingFunctions.append({ first, end });
        first = end;
    }

    auto note = [&](uint32_t position, const DeclaredType& declared) {
        if (m_isFollowed.quickGet(position))
            m_positionsOfTypes.add(&declared, position);
    };
    for (uint32_t index = 0; index < program.variables.size(); ++index)
        note(index, program.variables[index].declared);
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        for (uint32_t field = 0; layout.isEnvironment() && field < layout.fields.size(); ++field)
            note(m_firstField[index] + field, layout.fields[field].declared);
    }
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        for (uint32_t result = 0; result < program.functions[index].results.size(); ++result)
            note(m_firstResult[index] + result, program.functions[index].results[result]);
    }
}

std::optional<uint32_t> NumbersOfPositions::indexOfMap(ValueFlowObjectID object) const
{
    auto found = std::ranges::lower_bound(m_maps, object);
    if (found == m_maps.end() || *found != object)
        return std::nullopt;
    return static_cast<uint32_t>(found - m_maps.begin());
}

BitVector NumbersOfPositions::collectWritersOfMaps(ValueFlowHeapView& heap)
{
    BitVector hasOtherWriter(m_maps.size());
    if (m_maps.isEmpty())
        return hasOtherWriter;
    const Program& program = m_inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueFlowSolver& solver = m_inputs.solver();
    const ValueSets& sets = m_inputs.sets();
    ValueSetMaker& maker = heap.maker();
    constexpr OperandUses usesWritingObject = operandWritingUses | OperandUses { OperandUse::DefinesName, OperandUse::TakesDescriptor };
    constexpr ValueFlowName copiedFrom = ValueFlowName::of(ValueFlowNameKind::CopiedFrom);

    struct Source {
        auto order() const { return std::tuple { object, map }; }

        ValueFlowObjectID object;
        uint32_t map;
    };
    Vector<Source> sources;
    m_numbersInConstantsOfMaps = Vector<ValueFlowNumbers>(m_maps.size());

    auto isFilledByOperationsAlone = [&](ValueFlowObjectID object) {
        const ValueFlowSite* site = universe.kindOf(object) == ValueFlowObjectKind::Site ? universe.siteOf(object) : nullptr;
        FunctionID function = universe.functionOf(object);
        if (!site || !function || m_inputs.classOf(object))
            return false;
        bool isLiteral = site->origin == ValueFlowSiteOrigin::DataNode || (site->origin == ValueFlowSiteOrigin::Operation && program.at(function).at(site->value).opcode == Opcode::NewObject);
        if (!isLiteral)
            return false;
        if (m_inputs.hasEscaped(object) || m_inputs.isLost(object) || m_inputs.isHeldByNativeCode(object))
            return false;
        return !markedMutationsOf(m_inputs, object).containsAny({ ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::RedefineProperty });
    };
    for (uint32_t index = 0; index < m_maps.size(); ++index) {
        ValueSetID objects = heap.writtenUnder(heap.writtenUnder(maker.of(m_maps[index]), entriesPairsOfMap), sourcesOfEntriesPair);
        for (;;) {
            ValueSetID copied = heap.writtenUnder(objects, copiedFrom);
            if (sets.isSubset(copied, objects))
                break;
            objects = maker.unite(objects, copied);
        }
        if (ValueSets::isComplement(objects)) {
            hasOtherWriter.quickSet(index);
            continue;
        }
        sets.forEachMember(maker.intersect(objects, universe.objects()), [&](ValueFlowObjectID object) {
            if (isFilledByOperationsAlone(object))
                sources.append({ object, index });
            else
                hasOtherWriter.quickSet(index);
        });
    }
    AOT::sort(sources, { }, &Source::order);

    Vector<ValueFlowObjectID> objectsOfSources;
    for (const Source& source : sources) {
        if (objectsOfSources.isEmpty() || objectsOfSources.last() != source.object)
            objectsOfSources.append(source.object);
    }
    ValueSetID allSources = maker.of(objectsOfSources.span());
    ValueSetID allMaps = maker.of(m_maps.span());
    ValueSetID watched = maker.unite(allSources, allMaps);

    struct Found {
        Found(ValueSetMaker& maker, ValueSetID watched, size_t numberOfMaps)
            : maker(maker)
            , setsWithWatched(maker.sets(), watched)
            , hasOtherWriter(numberOfMaps)
            , numbersInConstants(numberOfMaps)
        {
        }

        ValueSetMaker& maker;
        SetsMeetingOneSet setsWithWatched;
        Vector<Writer> writers;
        Vector<ReadOfMaps> reads;
        BitVector hasOtherWriter;
        Vector<ValueFlowNumbers> numbersInConstants;
    };

    auto forEachMapFedBy = [&](Found& found, ValueSetID objects, NOESCAPE const Invocable<void(uint32_t)> auto& functor) {
        sets.forEachMember(found.maker.intersect(objects, allSources), [&](ValueFlowObjectID object) {
            for (const Source& source : std::ranges::equal_range(sources, object, { }, &Source::object))
                functor(source.map);
        });
    };
    auto noteStoredIntoSources = [&](Found& found, ValueSetID objects, FunctionID function, ValueID value) {
        forEachMapFedBy(found, objects, [&](uint32_t map) {
            found.writers.append({ m_firstMap + map, function, value });
        });
    };
    auto noteOtherWriter = [&](Found& found, ValueSetID objects) {
        forEachMapFedBy(found, objects, [&](uint32_t map) {
            found.hasOtherWriter.quickSet(map);
        });
        sets.forEachMember(found.maker.intersect(objects, allMaps), [&](ValueFlowObjectID map) {
            found.hasOtherWriter.quickSet(*indexOfMap(map));
        });
    };
    auto noteConstant = [&](Found& found, const ValueFlowStatement& statement, const DataNode& root) {
        forEachDataNodeInPreorder(root, [&](uint32_t ordinal, const DataNode& node) {
            const auto* listed = node.tryAs<DataNode::Object>();
            ValueFlowObjectID object = listed ? universe.siteObject(statement, ordinal) : ValueFlowObjectID();
            if (!object || !sets.contains(allSources, object))
                return;
            ValueFlowNumbers numbers;
            for (const DataMember& member : listed->members) {
                const auto* number = member.value.tryAs<DataNode::Number>();
                if (!number)
                    continue;
                if (isTrackedInteger(number->value))
                    numbers.integers = numbers.integers.unionWith(Range::one(static_cast<int64_t>(number->value)));
                else
                    numbers.mayBeOtherThanIntegers = true;
            }
            forEachMapFedBy(found, found.maker.of(object), [&](uint32_t map) {
                found.numbersInConstants[map].unite(numbers);
            });
        });
    };

    auto wholeValueOf = [&](Found& found, const ValueFlowOperand& operand) {
        return found.maker.unite(operand.node ? solver.possibleValuesOf(operand.node) : ValueSets::nothing, operand.constant ? operand.constant : ValueSets::nothing);
    };
    ValueSetID getOrPrimitive = maker.unite(maker.of(universe.objectOf(builtins.canonicalID(Builtins::MapPrototypeGet))), universe.primitives());
    ValueSetID mapsOrPrimitives = maker.unite(allMaps, ValueSets::complement(universe.objects()));

    ValueSetMakers makers(m_inputs);
    Vector<std::optional<Found>> foundByWorker(m_inputs.loops().width());
    forEachIndexInParallel(m_inputs.loops(), program.functions.size(), [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        if (!m_inputs.isReached(id))
            return;
        if (!foundByWorker[worker])
            foundByWorker[worker].emplace(makers.of(worker), watched, m_maps.size());
        Found& found = *foundByWorker[worker];
        for (const ValueFlowStatement& statement : m_inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            const Value& operation = program.at(id).at(statement.site);
            std::span<const ValueFlowOperand> operands = m_inputs.bodies().operandsOf(statement);
            if (statement.opcode == Opcode::NewObject) {
                ValueFlowObjectID object = universe.siteObject(id, statement.site);
                for (uint32_t operand = OperandIndex::NewObject::values; object && operand < operation.numberOfOperands(); ++operand)
                    noteStoredIntoSources(found, found.maker.of(object), id, operation.operand(operand));
            }
            if (statement.opcode == Opcode::Data)
                noteConstant(found, statement, program.at(operation.data()).root);
            if (statement.opcode == Opcode::Call && operands.size() >= 2 && sets.isSubset(wholeValueOf(found, operands[0]), getOrPrimitive)) {
                ValueSetID receivers = wholeValueOf(found, operands[1]);
                if (sets.isSubset(receivers, mapsOrPrimitives))
                    found.reads.append({ id, statement.site, found.maker.intersect(receivers, allMaps) });
            }

            if (effectRuleOf(statement.opcode) == EffectRule::Call)
                continue;
            std::optional<uint8_t> receiver = receiverOf(statement.opcode);
            std::optional<uint32_t> stored = indexOfValueStoredIntoProperty(statement.opcode);
            for (uint32_t operand = 0; operand < operands.size(); ++operand) {
                bool mayBeWatched = (operands[operand].constant && found.setsWithWatched.contains(operands[operand].constant)) || (operands[operand].node && found.setsWithWatched.contains(solver.possibleValuesOf(operands[operand].node)));
                if (!mayBeWatched)
                    continue;
                bool storesIntoProperty = stored && receiver && operand == *receiver;
                if (!storesIntoProperty && !operandUsesOf(statement.opcode, operand).containsAny(usesWritingObject))
                    continue;
                ValueSetID written = heap.possibleValuesOf(found.maker, operands[operand]);
                if (storesIntoProperty) {
                    noteStoredIntoSources(found, written, id, operation.operand(*stored));
                    continue;
                }
                if (statement.opcode == Opcode::CopyDataProperties || statement.opcode == Opcode::SetLiteralPrototype || statement.opcode == Opcode::FinishObject)
                    continue;
                noteOtherWriter(found, written);
            }
        }
    });

    auto writes = [&](const BuiltinParameter& parameter) {
        return parameter.roles.contains(BuiltinOperandRole::ReplacesContents) || parameter.uses.containsAny(usesWritingObject);
    };
    BitVector builtinsThatWrite = builtinsWithOverload(builtins, [&](const BuiltinOverload& overload) {
        return (overload.receiver && writes(*overload.receiver)) || std::ranges::any_of(overload.parameters, writes);
    });
    Found foundInApplications(maker, watched, m_maps.size());
    forEachBuiltinApplication(m_inputs, heap, builtinsThatWrite, watched, [&](const ValueFlowBuiltinApplication& application) {
        BuiltinID builtin = builtins.canonicalID(application.builtin);
        const Value* call = application.site ? &program.at(application.function).at(application.site) : nullptr;
        bool isCallOfMethod = application.mode() == BuiltinCallMode::Call;
        bool onlyRemoves = isCallOfMethod && (builtin == builtins.canonicalID(Builtins::MapPrototypeDelete) || builtin == builtins.canonicalID(Builtins::MapPrototypeClear));
        bool spellsOutValue = isCallOfMethod && builtin == builtins.canonicalID(Builtins::MapPrototypeSet) && call && call->opcode == Opcode::Call;
        auto noteParameter = [&](const BuiltinParameter& parameter) {
            if (!writes(parameter))
                return;
            ValueSetID written = application.valueAt(parameter.operand, maker);
            if (!parameter.operand.isReceiver() || (!onlyRemoves && !spellsOutValue)) {
                noteOtherWriter(foundInApplications, written);
                return;
            }
            noteOtherWriter(foundInApplications, maker.intersect(written, allSources));
            constexpr uint32_t indexOfValue = 3;
            if (!spellsOutValue || call->numberOfOperands() <= indexOfValue)
                return;
            sets.forEachMember(maker.intersect(written, allMaps), [&](ValueFlowObjectID map) {
                foundInApplications.writers.append({ m_firstMap + *indexOfMap(map), application.function, call->operand(indexOfValue) });
            });
        };
        for (const BuiltinOverload& overload : builtins.overloadsOf(application.builtin)) {
            if (overload.mode != application.mode())
                continue;
            if (overload.receiver)
                noteParameter(*overload.receiver);
            for (const BuiltinParameter& parameter : overload.parameters)
                noteParameter(parameter);
        }
    });

    auto take = [&](Found& found) {
        m_writersOfMaps.appendVector(WTF::move(found.writers));
        m_readsOfMaps.appendVector(WTF::move(found.reads));
        hasOtherWriter.merge(found.hasOtherWriter);
        for (uint32_t index = 0; index < m_maps.size(); ++index)
            m_numbersInConstantsOfMaps[index].unite(found.numbersInConstants[index]);
    };
    for (std::optional<Found>& found : foundByWorker) {
        if (found)
            take(*found);
    }
    take(foundInApplications);

    m_writersOfMaps.removeAllMatching([&](const Writer& writer) {
        return hasOtherWriter.quickGet(writer.position - m_firstMap);
    });
    AOT::sort(m_readsOfMaps, { }, &ReadOfMaps::order);
    return hasOtherWriter;
}

const NumbersOfPositions::ReadOfMaps* NumbersOfPositions::readOfMapsAt(FunctionID caller, ValueID site) const
{
    auto found = std::ranges::lower_bound(m_readsOfMaps, std::tuple { caller, site }, { }, &ReadOfMaps::order);
    return found != m_readsOfMaps.end() && found->caller == caller && found->site == site ? &*found : nullptr;
}

const ValueFlowNumbers* NumbersOfPositions::ofValuesOfMap(ValueFlowObjectID object) const
{
    std::optional<uint32_t> index = indexOfMap(object);
    return index ? find(m_firstMap + *index) : nullptr;
}

std::span<const NumbersOfPositions::CallByValue> NumbersOfPositions::callsAt(FunctionID caller, ValueID site) const
{
    auto found = std::ranges::equal_range(m_calls, std::pair { caller, site }, { }, [](const CallByValue& call) {
        return std::pair { call.caller, call.site };
    });
    return { found.begin(), found.end() };
}

std::optional<ValueFlowNumbers> NumbersOfPositions::ofCall(FunctionID caller, ValueID site) const
{
    const Program& program = m_inputs.program;
    if (const ReadOfMaps* read = readOfMapsAt(caller, site)) {
        ValueFlowNumbers numbers;
        m_inputs.sets().forEachMember(read->maps, [&](ValueFlowObjectID map) {
            numbers.unite(m_numbers[m_firstMap + *indexOfMap(map)]);
        });
        return numbers;
    }
    auto found = callsAt(caller, site);
    if (found.empty())
        return std::nullopt;
    ValueFlowNumbers numbers;
    for (const CallByValue& call : found) {
        if (const ManyCallees* callees = findManyCallees(call.callees)) {
            numbers.unite(callees->numbersOfResult);
            continue;
        }
        m_inputs.sets().forEachMember(call.callees, [&](ValueFlowObjectID member) {
            FunctionID id = m_inputs.universe().closureFunction(member);
            if (id && program.at(id).isUnsplitCoroutine())
                return;
            numbers.unite(id && program.at(id).results.size() == 1 ? m_numbers[m_firstResult[id.index()]] : ValueFlowNumbers::any());
        });
    }
    return numbers;
}

Type NumbersOfPositions::typeAt(uint32_t position, Type&& known) const
{
    if (m_holdsPrimitivesAlone.quickGet(position)) {
        uint16_t primitives = known.primitives & m_primitives[position];
        known = { };
        known.primitives = primitives;
        if (primitives & Primitive::Integer)
            known.integers = Range::full();
    }
    m_numbers[position].narrow(known);
    return WTF::move(known);
}

Type NumbersOfPositions::typeAt(Evaluation& evaluation, uint32_t position, Type&& known) const
{
    if (evaluation.positionsRead && m_isFollowed.quickGet(position))
        evaluation.positionsRead->append(position);
    return typeAt(position, WTF::move(known));
}

std::optional<Type> NumbersOfPositions::typeOfCall(Evaluation& evaluation, FunctionID caller, ValueID site) const
{
    const Program& program = m_inputs.program;
    TypeArena& arena = evaluation.arena;
    if (const ReadOfMaps* read = readOfMapsAt(caller, site)) {
        Type answered = program.at(TypeTable::undefined);
        m_inputs.sets().forEachMember(read->maps, [&](ValueFlowObjectID map) {
            answered = answered.unionWith(typeAt(evaluation, m_firstMap + *indexOfMap(map), Type::anything()), arena);
        });
        return answered;
    }
    auto found = callsAt(caller, site);
    if (found.empty())
        return std::nullopt;
    Type answered;
    for (const CallByValue& call : found) {
        if (const ManyCallees* callees = findManyCallees(call.callees)) {
            if (evaluation.positionsRead)
                evaluation.positionsRead->append(positionOf(*callees));
            answered = &call == found.data() ? callees->result : answered.unionWith(callees->result, arena);
            continue;
        }
        m_inputs.sets().forEachMember(call.callees, [&](ValueFlowObjectID member) {
            FunctionID id = m_inputs.universe().closureFunction(member);
            if (id && program.at(id).results.size() == 1 && !program.at(id).isUnsplitCoroutine())
                answered = answered.unionWith(typeAt(evaluation, m_firstResult[id.index()], Type::anything()), arena);
            else
                answered = Type::anything();
        });
    }
    return answered;
}

// `Ticket.issued`, where the receiver is the class and nothing else. In front of the field's own store the name finds what the class inherits: `class B extends A { static m = B.n; static n = 0; }` reads A.n.
std::optional<uint32_t> NumbersOfPositions::staticFieldNamedBy(const Function& function, const Value& access) const
{
    const Program& program = m_inputs.program;
    FunctionID classConstructor = access.key() ? program.typeOf(function, access.operand(0)).singleFunction() : FunctionID();
    VariableID variable = classConstructor ? findStaticFieldVariable(program, classConstructor, access.key()) : VariableID();
    if (!variable || !m_isFollowed.quickGet(variable.index()) || !m_isStaticFieldReadBehindItsStore.quickGet(variable.index()))
        return std::nullopt;
    return variable.index();
}

Type NumbersOfPositions::typeOf(Evaluation& evaluation, FunctionID id, ValueID valueID, unsigned depth) const
{
    const Program& program = m_inputs.program;
    const Function& function = program.at(id);
    const Value& value = function.at(valueID);
    bool isBehindCheck = m_valuesBehindChecks[id.index()].get(valueID.index());
    Type known = program.at(isBehindCheck || !value.type ? TypeTable::anything : value.type);
    if (!depth || !evaluation.stepsLeft || !(known.primitives & Primitive::Number))
        return known;
    --evaluation.stepsLeft;
    auto withNumbersOf = [&](const Type& type) {
        if (evaluation.tellsKindsApart && !type.hasObjects())
            return known.intersectWith(type, evaluation.arena);
        ValueFlowNumbers numbers;
        numbers.add(type);
        numbers.narrow(known);
        return known;
    };

    switch (value.opcode) {
    case Opcode::GetModuleVariable:
        return typeAt(evaluation, value.variable().index(), WTF::move(known));
    case Opcode::GetEnvironmentField:
        return typeAt(evaluation, m_firstField[value.layout().index()] + value.field(), WTF::move(known));
    case Opcode::PropGet: {
        std::optional<uint32_t> variable = staticFieldNamedBy(function, value);
        return variable ? typeAt(evaluation, *variable, WTF::move(known)) : known;
    }
    case Opcode::CallDirect: {
        const Function& callee = program.at(value.function());
        if (callee.results.size() != 1 || callee.isUnsplitCoroutine())
            return known;
        return typeAt(evaluation, m_firstResult[value.function().index()], WTF::move(known));
    }
    case Opcode::Call: {
        std::optional<Type> answered = typeOfCall(evaluation, id, valueID);
        return answered ? *answered : known;
    }
    case Opcode::Parameter: {
        size_t index = function.at(entryBlock).parameters.find(valueID);
        if (index != notFound)
            return typeAt(evaluation, m_firstEntryParameter[id.index()] + index, WTF::move(known));
        if (!isBehindCheck)
            return known;
        auto passed = std::ranges::equal_range(m_passedValues[id.index()], valueID, { }, &PassedValue::parameter);
        if (passed.empty())
            return known;
        Type joined;
        for (const PassedValue& entry : passed) {
            if (!entry.value)
                return known;
            joined = joined.unionWith(typeOf(evaluation, id, entry.value, depth - 1), evaluation.arena);
        }
        return joined;
    }
    default:
        break;
    }
    if (isCast(value.opcode)) {
        Type operand = typeOf(evaluation, id, value.operand(0), depth - 1);
        if (!isBehindCheck)
            return withNumbersOf(operand);
        operand.primitives &= ~Primitive::Absent;
        return operand;
    }
    if (!Value::opcodesWithForm.contains(value.opcode))
        return known;

    Vector<Type, 2> operandTypes;
    for (uint32_t index = 0; index < numberOfNamedOperandsOf(value.opcode); ++index) {
        if (!value.operand(index))
            return known;
        operandTypes.append(typeOf(evaluation, id, value.operand(index), depth - 1));
        operandTypes.last().primitives &= ~Primitive::Absent;
    }
    std::optional<Type> stepped = typeAfterStepOfOne(value.opcode, operandTypes.span());
    Type computed = stepped ? *stepped : computeOperatorResultType(TypingContext { program, function, evaluation.arena }, value.opcode, operandTypes.span());
    return isBehindCheck ? computed : withNumbersOf(computed);
}

ValueFlowNumbers NumbersOfPositions::numbersOf(FunctionID id, ValueID value) const
{
    ScratchTypeArena scratch(m_inputs.program.types);
    Evaluation evaluation { scratch.arena() };
    ValueFlowNumbers numbers;
    numbers.add(typeOf(evaluation, id, value, maxDepth));
    return numbers;
}

ValueFlowNumbers NumbersOfPositions::numbersOfStep(FunctionID id, ValueID value) const
{
    ScratchTypeArena scratch(m_inputs.program.types);
    Evaluation evaluation { scratch.arena(), nullptr, maxSteps, true };
    ValueFlowNumbers numbers;
    numbers.add(typeOf(evaluation, id, value, maxDepth));
    return numbers;
}

std::unique_ptr<ValueFlowNumbersOfValues> prepareNumbersOfPositions(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const FlowResultTables& tables)
{
    return makeUnique<NumbersOfPositions>(inputs, heap, tables);
}

void computeNumbersOfPositions(ValueFlowNumbersOfValues& prepared, const FlowResultTables& tables)
{
    static_cast<NumbersOfPositions&>(prepared).followPositions(tables);
}

bool NumbersOfPositions::typeWritersByRules(TypeArena& arena, FunctionID id, std::span<const uint32_t> writers, Vector<uint32_t>& positionsAsked)
{
    const Program& program = m_inputs.program;
    Function copy = program.at(id);
    for (uint32_t index = 0; index < copy.numberOfEntryParameters(); ++index) {
        DeclaredType* declared = copy.declaredEntryParameter(index);
        uint32_t position = m_firstEntryParameter[id.index()] + index;
        if (!declared || !declared->written() || !m_isFollowed.quickGet(position))
            continue;
        positionsAsked.append(position);
        declared->establish(Proven::byFlowOfPosition(arena.intern(typeAt(position, typeWithoutProof(*declared)))));
    }
    for (size_t index : m_valuesBehindChecks[id.index()]) {
        Value& value = copy.edit(ValueID(index));
        if (value.opcode == Opcode::Check || value.opcode == Opcode::AssumeType) {
            ValueID operand = value.valueOperand();
            value.become(Opcode::Upcast);
            value.setValueOperand(operand);
        }
        if (value.opcode == Opcode::Upcast)
            value.setCastType(TypeTable::anythingOrAbsent);
    }

    AnalysisContext context { m_inputs.build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, copy);
    KnownTypes knownTypes(*this, positionsAsked);
    TypePropagation propagation(TypePropagationInputs { program, id, copy, analyses, arena, m_inputs.build, nullptr, &knownTypes });
    propagation.restartAllValues();
    propagation.solveTypes();
    bool hasFoundSomething = false;
    for (uint32_t writer : writers) {
        ValueFlowNumbers typed;
        typed.add(arena.at(copy.at(m_writers[writer].value).type));
        ValueFlowNumbers both = m_numbersByRules[writer].meetWith(typed);
        if (both == m_numbersByRules[writer])
            continue;
        hasFoundSomething = true;
        m_rulesHaveMoved[writer] = 1;
        m_numbersByRules[writer] = both;
    }
    return hasFoundSomething;
}

bool NumbersOfPositions::typeLoopsByRules()
{
    bool verifies = m_inputs.build.options().verifiesAfterEveryPass;
    std::atomic<bool> hasFoundSomething { false };
    std::atomic<uint64_t> numberOfFunctionsTyped { 0 };
    forEachIndexInParallel(m_inputs.loops(), m_loopingFunctions.size(), [&](unsigned index) {
        LoopingFunction& function = m_loopingFunctions[index];
        bool getsOtherAnswers = !function.isTyped || std::ranges::any_of(function.positionsAsked, [&](uint32_t position) {
            return m_hasMovedSinceTyping.quickGet(position);
        });
        if (!getsOtherAnswers && !verifies)
            return;
        function.isTyped = true;
        function.positionsAsked.shrink(0);
        ScratchTypeArena arena(m_inputs.program.types);
        std::span<const uint32_t> writers = m_writersInLoopingFunctions.subspan(function.firstWriter, function.endOfWriters - function.firstWriter);
        bool hasFoundSomethingHere = typeWritersByRules(arena, m_writers[writers[0]].function, writers, function.positionsAsked);
        RELEASE_ASSERT(getsOtherAnswers || !hasFoundSomethingHere);
        AOT::sort(function.positionsAsked);
        removeRepeatedElements(function.positionsAsked);
        function.positionsAsked.shrinkToFit();
        numberOfFunctionsTyped.fetch_add(1, std::memory_order_relaxed);
        if (hasFoundSomethingHere)
            hasFoundSomething.store(true, std::memory_order_relaxed);
    });
    m_hasMovedSinceTyping.clearAll();
    m_someRulesHaveMoved = hasFoundSomething.load(std::memory_order_relaxed);
    ++m_numberOfTypings;
    m_numberOfFunctionsTyped += numberOfFunctionsTyped.load(std::memory_order_relaxed);
    return m_someRulesHaveMoved;
}

ValueFlowNumbers NumbersOfPositions::numbersArrivingFrom(TypeArena& arena, uint32_t firstWriter, uint32_t endOfWriters, Vector<uint32_t>* positionsRead, bool tellsKindsApart) const
{
    ValueFlowNumbers arriving;
    for (uint32_t index = firstWriter; index < endOfWriters; ++index) {
        Evaluation evaluation { arena, positionsRead, maxSteps, tellsKindsApart };
        ValueFlowNumbers handedOver;
        handedOver.add(typeOf(evaluation, m_writers[index].function, m_writers[index].value, maxDepth));
        arriving.unite(handedOver.meetWith(m_numbersByRules[index]));
    }
    return arriving;
}

// A double takes a step at no cost, an integer beyond 32 bits does not. The integers of a place are worth knowing where something is done with the number that an integer is better for, in code that may run more than once.
// A place that is compared there counts the turns of a loop or stands against a limit: what bounds, unrolls and folds those knows it as a number, and it stays one.
BitVector NumbersOfPositions::findPlacesTakenAsValues(const BitVector& places) const
{
    const Program& program = m_inputs.program;
    constexpr OpcodeSet comparisons = relationalOperators | OpcodeSet { Opcode::StrictEq, Opcode::Eq };
    constexpr OpcodeSet conversions { Opcode::ToNumeric, Opcode::ToNumber };
    constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    constexpr size_t maxNames = 32;
    constexpr OpcodeSet readsOfPlaces { Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::PropGet };
    constexpr OpcodeSet storesIntoPlaces { Opcode::PutModuleVariable, Opcode::PutEnvironmentField, Opcode::PropSet };
    auto placeOf = [&](const Function& function, const Value& value) -> std::optional<uint32_t> {
        if (value.opcode == Opcode::GetModuleVariable || value.opcode == Opcode::PutModuleVariable)
            return value.variable().index();
        if (value.opcode == Opcode::GetEnvironmentField || value.opcode == Opcode::PutEnvironmentField)
            return m_firstField[value.layout().index()] + value.field();
        return staticFieldNamedBy(function, value);
    };
    constexpr uint32_t isCompared = 1u << 31;
    BitVector taken(places.size());
    BitVector compared(places.size());
    for (uint32_t place : collectInParallel<uint32_t>(m_inputs.loops(), program.functions.size(), [&](unsigned index, Vector<uint32_t>& found) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        if (!function.hasBody() || !m_inputs.isReached(id))
            return;
        Vector<ValueID, 8> reads;
        for (uint32_t value = 0; value < function.values.size(); ++value) {
            std::optional<uint32_t> place = readsOfPlaces.contains(function.values[value].opcode) ? placeOf(function, function.values[value]) : std::nullopt;
            if (place && places.get(*place))
                reads.append(ValueID(value));
        }
        if (reads.isEmpty())
            return;
        Function copy = function;
        AnalysisContext context { m_inputs.build, program, nullptr, nullptr, id };
        FunctionAnalyses analyses(context, copy);
        auto runsOnce = [&](BlockID block) {
            return function.isModuleBody() && !analyses.naturalLoops().innerMostLoopOf(block);
        };
        // Beside a number that is held as a double an integer is converted each time, and a quotient is a double whatever goes in.
        auto isReckonedAsDouble = [&](const Value& user, uint32_t operandIndex) {
            if (!arithmetic.contains(user.opcode))
                return false;
            const Type& other = program.typeOf(copy, user.operand(1 - operandIndex));
            return user.opcode == Opcode::Div || (other.isNumber() && !other.isInteger());
        };
        for (ValueID read : reads) {
            uint32_t place = *placeOf(copy, copy.at(read));
            Vector<ValueID, maxNames> names { read };
            bool isTaken = false;
            for (size_t name = 0; name < names.size() && !isTaken; ++name) {
                for (const Use& use : analyses.useLists().usesOf(names[name])) {
                    if (runsOnce(use.block))
                        continue;
                    const Value* user = use.kind == Use::Kind::Operand ? &copy.at(use.user) : nullptr;
                    if (user && comparisons.contains(user->opcode)) {
                        found.append(place | isCompared);
                        continue;
                    }
                    if (user && (isReckonedAsDouble(*user, use.operandIndex) || (storesIntoPlaces.contains(user->opcode) && placeOf(copy, *user) == place)))
                        continue;
                    bool isFollowed = user && (isCast(user->opcode) || conversions.contains(user->opcode) || mayBeStepOfOne(user->opcode)) && names.size() < maxNames;
                    if (!isFollowed) {
                        isTaken = true;
                        break;
                    }
                    if (!names.contains(use.user))
                        names.append(use.user);
                }
            }
            if (isTaken)
                found.append(place);
        }
    }))
        (place & isCompared ? compared : taken).set(place & ~isCompared);
    taken.exclude(compared);
    return taken;
}

// The rounds only ever take numbers away, and a place that is stored what it held plus one is handed back whatever it is said to hold: they never get below "any number without a fraction".
// So such a place is taken to hold integers in a range. If all that its writers then hand over lies in that range, it does: they start inside it and no store leaves it.
void NumbersOfPositions::assumeIntegersOfCounters()
{
    const Program& program = m_inputs.program;
    uint32_t endOfPlaces = m_firstEntryParameter.isEmpty() ? m_firstMap : m_firstEntryParameter[0];
    auto isStep = [&](const Writer& writer) {
        const Function& function = program.at(writer.function);
        const Value& value = function.at(definitionThroughSameValues(function, writer.value));
        auto isOne = [&](ValueID operand) {
            const Type& type = program.typeOf(function, operand);
            return type.isInteger() && type.integers.isSingleValue() && std::abs(type.integers.minimum) == 1;
        };
        if (value.opcode == Opcode::Add || value.opcode == Opcode::Sub)
            return isOne(value.operand(1)) || (value.opcode == Opcode::Add && isOne(value.operand(0)));
        return value.opcode == Opcode::Increment || value.opcode == Opcode::Decrement;
    };
    BitVector counters(endOfPlaces);
    for (const FollowedPosition& followed : m_followed) {
        if (followed.position < endOfPlaces && m_numbers[followed.position].mayBeOtherThanIntegers && followed.firstSharedRange == followed.endOfSharedRanges
            && std::ranges::any_of(m_writers.subspan(followed.firstWriter, followed.endOfWriters - followed.firstWriter), isStep))
            counters.quickSet(followed.position);
    }
    if (counters.isEmpty())
        return;
    counters.filter(findPlacesTakenAsValues(counters));

    ScratchTypeArena arena(program.types);
    auto assume = [&](const FollowedPosition& followed) {
        ValueFlowNumbers& numbers = m_numbers[followed.position];
        ValueFlowNumbers known = numbers;
        auto holds = [&](Range range) {
            numbers = { false, range };
            ValueFlowNumbers arriving = numbersArrivingFrom(arena, followed.firstWriter, followed.endOfWriters, nullptr, true);
            return std::pair { !arriving.mayBeOtherThanIntegers && arriving.integers.isSubsetOf(range), arriving.integers };
        };
        Range widest = known.integers.intersectWith(Range::exact());
        if (widest.isEmpty() || !holds(widest).first) {
            numbers = known;
            return false;
        }
        // From what the other writers hand over, an end that a step moves goes all the way: the rounds would take it back one step at a time.
        ValueFlowNumbers start;
        for (uint32_t writer = followed.firstWriter; writer < followed.endOfWriters; ++writer) {
            if (!isStep(m_writers[writer]))
                start.unite(numbersArrivingFrom(arena, writer, writer + 1, nullptr, true));
        }
        Range range = start.integers;
        while (!range.isEmpty() && range != widest) {
            auto [isKept, arriving] = holds(range);
            if (isKept)
                break;
            Range wider { arriving.minimum < range.minimum ? widest.minimum : range.minimum, arriving.maximum > range.maximum ? widest.maximum : range.maximum };
            range = wider == range ? widest : wider;
        }
        numbers = { false, range.isEmpty() ? widest : range };
        return true;
    };
    Vector<const FollowedPosition*, 8> waiting;
    for (const FollowedPosition& followed : m_followed) {
        if (followed.position < endOfPlaces && counters.quickGet(followed.position))
            waiting.append(&followed);
    }
    // A counter that is also stored another counter waits for that one.
    while (waiting.removeAllMatching([&](const FollowedPosition* followed) { return assume(*followed); })) { }
}

void NumbersOfPositions::evaluateSharedRanges(bool isFirstRound)
{
    bool verifies = m_inputs.build.options().verifiesAfterEveryPass;
    forEachIndexInParallel(m_inputs.loops(), m_partsOfSharedRanges.size(), [&](unsigned index) {
        PartOfSharedRange& part = m_partsOfSharedRanges[index];
        bool isHandedOtherNumbers = isFirstRound
            || (m_someRulesHaveMoved && std::ranges::any_of(m_rulesHaveMoved.subspan(part.firstWriter, part.endOfWriters - part.firstWriter), [](uint8_t hasMoved) { return !!hasMoved; }))
            || std::ranges::any_of(part.positionsRead, [&](uint32_t position) { return m_hasMovedInLastRound.quickGet(position); });
        if (!isHandedOtherNumbers && !verifies)
            return;
        ScratchTypeArena arena(m_inputs.program.types);
        ValueFlowNumbers arriving = numbersArrivingFrom(arena, part.firstWriter, part.endOfWriters, isFirstRound ? &part.positionsRead : nullptr);
        RELEASE_ASSERT(isHandedOtherNumbers || arriving == part.arriving);
        part.arriving = arriving;
        if (isFirstRound) {
            AOT::sort(part.positionsRead);
            removeRepeatedElements(part.positionsRead);
            part.positionsRead.shrinkToFit();
        }
    });
    for (SharedRange& range : m_sharedRanges) {
        ValueFlowNumbers arriving;
        for (const PartOfSharedRange& part : m_partsOfSharedRanges.subspan(range.firstPart, range.endOfParts - range.firstPart))
            arriving.unite(part.arriving);
        range.hasMoved = isFirstRound || arriving != range.arriving;
        range.arriving = arriving;
    }
}

bool NumbersOfPositions::runRound()
{
    unsigned numberOfTasks = (m_followed.size() + positionsPerTask - 1) / positionsPerTask;
    bool isFirstRound = m_positionsReadInTask.isEmpty();
    if (isFirstRound)
        m_positionsReadInTask.grow(numberOfTasks);
    bool verifies = m_inputs.build.options().verifiesAfterEveryPass;
    evaluateSharedRanges(isFirstRound);
    std::atomic<uint64_t> numberOfPositionsLookedAt { 0 };
    Vector<ValueFlowNumbers> found(m_followed.size());
    forEachIndexInParallel(m_inputs.loops(), numberOfTasks, [&](unsigned task) {
        std::optional<ScratchTypeArena> arena;
        Vector<uint32_t>& positionsRead = m_positionsReadInTask[task];
        uint64_t numberLookedAt = 0;
        for (size_t index = task * positionsPerTask; index < std::min<size_t>((task + 1) * positionsPerTask, m_followed.size()); ++index) {
            FollowedPosition& followed = m_followed[index];
            size_t numberOfWriters = followed.endOfWriters - followed.firstWriter;
            std::span<const uint32_t> sharedRanges = m_sharedRangesOfPositions.subspan(followed.firstSharedRange, followed.endOfSharedRanges - followed.firstSharedRange);
            bool isHandedOtherNumbers = isFirstRound
                || (m_someRulesHaveMoved && std::ranges::any_of(m_rulesHaveMoved.subspan(followed.firstWriter, numberOfWriters), [](uint8_t hasMoved) { return !!hasMoved; }))
                || std::ranges::any_of(positionsRead.subspan(followed.firstPositionRead, followed.endOfPositionsRead - followed.firstPositionRead), [&](uint32_t position) { return m_hasMovedInLastRound.quickGet(position); })
                || std::ranges::any_of(sharedRanges, [&](uint32_t range) { return m_sharedRanges[range].hasMoved; });
            if (!isHandedOtherNumbers && !verifies) {
                found[index] = m_numbers[followed.position];
                continue;
            }
            ++numberLookedAt;
            if (!arena)
                arena.emplace(m_inputs.program.types);
            if (isFirstRound)
                followed.firstPositionRead = positionsRead.size();
            ValueFlowNumbers arriving;
            if (followed.position >= m_firstMap)
                arriving = m_numbersInConstantsOfMaps[followed.position - m_firstMap];
            arriving.unite(numbersArrivingFrom(*arena, followed.firstWriter, followed.endOfWriters, isFirstRound ? &positionsRead : nullptr));
            for (uint32_t range : sharedRanges)
                arriving.unite(m_sharedRanges[range].arriving);
            found[index] = m_numbers[followed.position].meetWith(arriving);
            RELEASE_ASSERT(isHandedOtherNumbers || found[index] == m_numbers[followed.position]);
            if (isFirstRound) {
                std::span<uint32_t> readHere = positionsRead.mutableSpan().subspan(followed.firstPositionRead);
                AOT::sort(readHere);
                positionsRead.shrink(positionsRead.size() - std::ranges::unique(readHere).size());
                followed.endOfPositionsRead = positionsRead.size();
            }
        }
        if (isFirstRound)
            positionsRead.shrinkToFit();
        numberOfPositionsLookedAt.fetch_add(numberLookedAt, std::memory_order_relaxed);
    });

    if (std::exchange(m_someRulesHaveMoved, false))
        m_rulesHaveMoved.fill(static_cast<uint8_t>(0), m_rulesHaveMoved.size());
    m_hasMovedInLastRound.clearAll();
    bool hasFoundSomething = false;
    for (size_t index = 0; index < m_followed.size(); ++index) {
        uint32_t position = m_followed[index].position;
        ValueFlowNumbers& numbers = m_numbers[position];
        if (numbers == found[index])
            continue;
        hasFoundSomething = true;
        numbers = found[index];
        m_hasMovedInLastRound.quickSet(position);
        m_hasMovedSinceTyping.quickSet(position);
    }
    if (hasFoundSomething)
        updateManyCallees(true);
    ++m_numberOfRounds;
    m_numberOfPositionsLookedAt += numberOfPositionsLookedAt.load(std::memory_order_relaxed);
    return hasFoundSomething;
}

static void narrowEnvironmentFields(const ValueFlowResultInputs& inputs, LayoutID id, const HashSet<uint32_t>& nodesStoredUndefined, const NumbersOfPositions& numbers, ValueFlowPositions& positions, FlowLayoutUpdate& update)
{
    const Layout& layout = inputs.program.at(id);
    if (!layout.isEnvironment())
        return;
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        FieldFlow& flow = update.fields[index].flow;
        if (flow.location.writers != WriterState::AllKnown)
            continue;
        ValueFlowNodeID node = inputs.bodies().nodes().environmentField(id, index);
        DeadZone deadZone = deadZoneOf(flow.location);
        if (update.fields[index].undefinedAtStartIsNeverRead && !nodesStoredUndefined.contains(node.index()))
            deadZone = DeadZone::UndefinedAtStartIsNeverObserved;
        update.fields[index].position = positions.updateFor(layout.fields[index].declared, node, deadZone, numbers.ofEnvironmentField(id, index));
        if (inputs.build.isEnabled(FlowResultKind::HolderMutations, id.index()))
            flow.stability = everyFlagOf<FieldStability>();
        if (flow.location.initialization == InitializationState::SetAtCreation && flow.stability == everyFlagOf<FieldStability>())
            flow.occupancy = Occupancy::AlwaysFilled;
    }
}

static std::optional<Value> findValueHeldAnyway(const Program& program, const Type& type)
{
    if (type.primitives & Primitive::Integer && !type.integers.isEmpty()) {
        Value integer(Opcode::Integer);
        integer.setInteger(std::clamp<int64_t>(0, type.integers.minimum, type.integers.maximum));
        return integer;
    }
    if (type.primitives & Primitive::Double) {
        Value number(Opcode::Double);
        number.setNumber(type.primitives & Primitive::FractionalNumber ? 0.5 : -0.0);
        return number;
    }
    if (type.primitives & Primitive::Boolean) {
        Value boolean(Opcode::Boolean);
        boolean.setBoolean(!(type.primitives & Primitive::False));
        return boolean;
    }
    if (type.primitives & Primitive::Null)
        return Value(Opcode::Null);
    if (type.primitives & Primitive::String || !type.stringLiterals.isEmpty()) {
        Value string(Opcode::String);
        string.setString(type.primitives & Primitive::String ? program.strings.emptyString() : type.stringLiterals[0]);
        return string;
    }
    return std::nullopt;
}

struct AppendedFieldReference {
    LayoutID layout;
    uint32_t field { 0 };

    friend bool operator==(const AppendedFieldReference&, const AppendedFieldReference&) = default;
};

struct FieldsDefined {
    Vector<FieldReference, 2> declared;
    Vector<AppendedFieldReference, 1> appended;
};

static void addFieldsOfObjectsThatInitializerMeets(const ValueFlowResultInputs& inputs, const FlowApplication& application, const UnobservedFieldDefinition& definition, FieldsDefined& fields)
{
    const Program& program = inputs.program;
    ValueFlowNodeID receiver = inputs.bodies().nodes().entryParameter(definition.initializer, *program.at(definition.initializer).thisParameterIndex());
    inputs.sets().forEachMember(inputs.solver().possibleValuesOf(receiver), [&](ValueFlowObjectID object) {
        LayoutID current = inputs.universe().layoutOf(object);
        for (size_t steps = 0; current && steps < program.layouts.size(); ++steps, current = provenBaseOf(application, current)) {
            if (std::optional<uint32_t> field = program.at(current).findField(definition.key)) {
                fields.declared.appendIfNotContains(declarationOf(program, { current, *field }));
                return;
            }
            const Vector<FlowAppendedField>& appended = application.layouts[current.index()].appendedFields;
            size_t field = appended.findIf([&](const FlowAppendedField& candidate) {
                return candidate.key == definition.key;
            });
            if (field != notFound) {
                fields.appended.appendIfNotContains(AppendedFieldReference { current, static_cast<uint32_t>(field) });
                return;
            }
        }
    });
}

static Vector<FieldsDefined> findFieldsDefinedUnobserved(const ValueFlowResultInputs& inputs, const FlowApplication& application)
{
    const Program& program = inputs.program;
    std::span<const UnobservedFieldDefinition> definitions = inputs.bodies().unobservedDefinitions().all();
    Vector<FieldsDefined> fields(definitions.size());
    Vector<Vector<uint32_t, 2>> definitionsOfClass(definitions.empty() ? 0 : program.layouts.size());
    for (uint32_t index = 0; index < definitions.size(); ++index) {
        fields[index].declared.append(declarationOf(program, { definitions[index].classLayout, *program.at(definitions[index].classLayout).findField(definitions[index].key) }));
        addFieldsOfObjectsThatInitializerMeets(inputs, application, definitions[index], fields[index]);
        definitionsOfClass[definitions[index].classLayout.index()].append(index);
    }
    if (definitions.empty())
        return fields;
    for (uint32_t heir = 0; heir < program.layouts.size(); ++heir) {
        LayoutID base = program.layouts[heir].base;
        for (size_t steps = 0; base && steps < program.layouts.size(); ++steps, base = program.at(base).base) {
            for (uint32_t index : definitionsOfClass[base.index()]) {
                if (std::optional<uint32_t> field = program.layouts[heir].findField(definitions[index].key))
                    fields[index].declared.appendIfNotContains(declarationOf(program, { LayoutID(heir), *field }));
            }
        }
    }
    return fields;
}

void replaceUnobservedDefinitions(const ValueFlowResultInputs& inputs, TypeTable& types, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    std::span<const UnobservedFieldDefinition> definitions = inputs.bodies().unobservedDefinitions().all();
    Vector<FieldsDefined> fieldsDefined = findFieldsDefinedUnobserved(inputs, application);
    auto positionOfAppended = [&](AppendedFieldReference reference) -> FlowPositionUpdate& {
        return application.layouts[reference.layout.index()].appendedFields[reference.field].position;
    };
    auto typeOfAppended = [&](AppendedFieldReference reference) {
        const FlowPositionUpdate& position = positionOfAppended(reference);
        return position.isProven && position.type ? position.type : TypeTable::anything;
    };
    auto appendedHoldsUndefined = [&](AppendedFieldReference reference) {
        return !!(types.at(typeOfAppended(reference)).primitives & Primitive::Undefined);
    };
    auto positionOf = [&](FieldReference reference) -> FlowPositionUpdate& {
        return application.layouts[reference.layout.index()].fields[reference.field].position;
    };
    auto typeOf = [&](FieldReference reference) {
        const Field& field = fieldAt(program, reference);
        const FlowPositionUpdate& position = positionOf(reference);
        TypeID typeIfProven = position.type ? position.type : field.declared.written();
        return position.isProven ? typeIfProven : slotTypeOf(program, field);
    };
    auto holdsUndefined = [&](FieldReference reference) {
        return !!(types.at(typeOf(reference)).primitives & Primitive::Undefined);
    };
    for (uint32_t index = 0; index < definitions.size(); ++index) {
        const UnobservedFieldDefinition& definition = definitions[index];
        if (!inputs.isReached(definition.initializer) || (std::ranges::all_of(fieldsDefined[index].declared, holdsUndefined) && std::ranges::all_of(fieldsDefined[index].appended, appendedHoldsUndefined)))
            continue;
        ValueFlowNodeID defined = inputs.bodies().nodes().valueOf(definition);
        if (defined && !inputs.sets().isEmpty(inputs.solver().possibleValuesOf(defined)))
            continue;
        ScratchTypeArena scratch(types);
        Type common = types.at(typeOf(fieldsDefined[index].declared.first()));
        for (FieldReference reference : fieldsDefined[index].declared.span().subspan(1))
            common = common.intersectWith(types.at(typeOf(reference)), scratch);
        for (AppendedFieldReference reference : fieldsDefined[index].appended)
            common = common.intersectWith(types.at(typeOfAppended(reference)), scratch);
        if (std::optional<Value> value = findValueHeldAnyway(program, common)) {
            application.bodies[definition.initializer.index()].definedValues.append({ definition.definition, WTF::move(*value) });
            continue;
        }
        for (FieldReference reference : fieldsDefined[index].declared) {
            if (positionOf(reference).isProven && !holdsUndefined(reference) && program.at(fieldAt(program, reference).declared.written()).primitives & Primitive::Undefined)
                positionOf(reference).type = types.intern(types.at(typeOf(reference)).withPrimitives(Primitive::Undefined));
        }
        for (AppendedFieldReference reference : fieldsDefined[index].appended) {
            if (!appendedHoldsUndefined(reference))
                positionOfAppended(reference).type = types.intern(types.at(typeOfAppended(reference)).withPrimitives(Primitive::Undefined));
        }
    }
}

static void narrowFieldsOfObjects(const ValueFlowResultInputs& inputs, LayoutID id, const ValueFlowFieldContents& contents, const LedgerPart& ledger, const BitVector& fieldsWithUnobservedDefinition, ValueFlowPositions& positions, TypeArena& types, FlowLayoutUpdate& update)
{
    const Program& program = inputs.program;
    const Layout& layout = program.at(id);
    if (!hasObjectsThatCodeAlters(layout.kind))
        return;
    ValueSetMaker& maker = positions.maker();
    ValueSetID numbersThatAreNoIntegers = maker.of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::WholeNumber), ValueFlowUniverse::primitive(PrimitiveSet::FractionalNumber) });
    // A store tests what it is handed, in every version of its function, and turns away what does not fit the slot: of what arrives there only what passes counts.
    // An allocation tests nothing, so what it lists counts as it arrives. Whoever applies this stores such a value behind the allocation, or takes the type of the field back.
    auto noteValuesListedWithOtherTypes = [&](uint32_t index, const Type& asRead, ValueSetID primitivesInside, bool everyObjectLiesInside) {
        if (layout.fields[index].flags.contains(FieldFlags::IsInherited) || !asRead.isJavaScriptValue() || asRead.isAnything())
            return;
        const ValueFlowNumbers& storedNumbers = contents.storedNumbers[id.index()][index];
        for (const Write& write : ledger.writersOf(Place::field(program, id, index))->seen().predicted()) {
            if (write.form != WriteForm::AtCreation || !write.site)
                continue;
            if (ValueSets::isComplement(write.arrives)) {
                update.valuesListedWithOtherTypes.append({ .field = index, .function = write.function, .allocation = write.site, .primitivesOutside = Primitive::All });
                continue;
            }
            ValueSetID primitives = maker.intersect(write.arrives, inputs.universe().primitives());
            if (!storedNumbers.mayBeOtherThanIntegers)
                primitives = maker.intersect(primitives, ValueSets::complement(numbersThatAreNoIntegers));
            ValueSetID outside = maker.intersect(primitives, ValueSets::complement(primitivesInside));
            if (inputs.sets().isEmpty(outside) && (everyObjectLiesInside || positions.isSurelyInside(maker.intersect(write.arrives, inputs.universe().objects()), asRead)))
                continue;
            uint16_t primitivesOutside = 0;
            inputs.sets().anyMember(outside, [&](ValueFlowObjectID member) {
                primitivesOutside |= positions.typeOfObject(member).primitives;
                return false;
            });
            update.valuesListedWithOtherTypes.append({ .field = index, .function = write.function, .allocation = write.site, .primitivesOutside = primitivesOutside });
        }
    };
    ValueSetID fractions = maker.of(ValueFlowUniverse::primitive(PrimitiveSet::FractionalNumber));
    ValueSetID numbersAndNullish = maker.of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::Integer), ValueFlowUniverse::primitive(PrimitiveSet::WholeNumber), ValueFlowUniverse::primitive(PrimitiveSet::FractionalNumber),
        ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null) });
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        const Field& field = layout.fields[index];
        FieldFlow& flow = update.fields[index].flow;
        if (flow.location.writers != WriterState::AllKnown) {
            // The type was read off the stores that name the field. A value that does not fit would leave the slot for good, and with it every guarded read of the object would fail.
            bool isStoredByLoopOverNames = id.index() < contents.fieldsStoredUnderNamesOfEnumerations.size() && contents.fieldsStoredUnderNamesOfEnumerations[id.index()].get(index);
            if (isStoredByLoopOverNames && !field.declared.site && !field.flags.contains(FieldFlags::IsInherited) && program.at(field.declared.written()).isJavaScriptValue()) {
                update.fields[index].position.type = TypeTable::anything;
                continue;
            }
            const Type& asRead = program.at(field.declared.written());
            ValueSetID primitivesInside = inputs.universe().membersSurelyInside(maker, program, field.declared.written());
            if (asRead.primitives & Primitive::Integer && contents.storedNumbers[id.index()][index].integers.isSubsetOf(asRead.integers))
                primitivesInside = maker.unite(primitivesInside, maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Integer)));
            noteValuesListedWithOtherTypes(index, asRead, primitivesInside, false);
            continue;
        }
        ValueSetID stored = contents.stored[id.index()][index];
        const ValueFlowNumbers& storedNumbers = contents.storedNumbers[id.index()][index];
        auto primitivesAmong = [&](ValueSetID values) {
            ValueSetID primitives = maker.intersect(values, inputs.universe().primitives());
            return storedNumbers.mayBeOtherThanIntegers ? primitives : maker.intersect(primitives, ValueSets::complement(numbersThatAreNoIntegers));
        };
        // A field that was narrowed to integers which the numbers stored into it outgrow has the type again that it had before.
        // Every sum of integers may be a whole number. A field that is counted with stays an integer, which is added to in memory and is an index as it stands, until a fraction arrives.
        // Where the field would hold a boxed value, a read of an integer would cost a test of its own: one bit of the cell answers for all the integers in slots.
        DeclaredType declared = field.declared;
        if (TypeID before = field.typeBeforeNarrowing) {
            ValueSetID numbersThatOutgrowField = field.flags.contains(FieldFlags::IsCountedWith) ? fractions : numbersThatAreNoIntegers;
            bool holdsNumbersAlone = program.at(before).isNumberOrNullish() || inputs.sets().isSubset(stored, numbersAndNullish);
            // What arrives under a key or through a receiver that nobody knows is not told apart from what arrives under the name of the field, and says nothing of its numbers.
            bool everyWriteNamesField = std::ranges::none_of(ledger.writersOf(Place::field(program, id, index))->seen().predicted(), [](const Write& write) {
                return write.actor == Actor::KeysNobodyKnows || write.actor == Actor::ReceiversNobodyKnows;
            });
            if (holdsNumbersAlone && everyWriteNamesField && inputs.sets().meet(primitivesAmong(stored), numbersThatOutgrowField))
                declared.changeType(Proven::byTestsAtStores(ExpectedType(before)));
        }
        const Type& asRead = program.at(declared.written());
        bool startsAbsent = asRead.primitives & Primitive::Absent && !inputs.sets().isEmpty(stored);
        auto resultWith = [&](ValueSetID held) {
            return positions.resultAt(declared, startsAbsent ? maker.unite(held, maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Absent))) : held);
        };
        ValueFlowPositionResult result = resultWith(stored);
        bool hasUnobservedDefinition = fieldsWithUnobservedDefinition.get(index);
        const Type& proven = result.narrowed ? *result.narrowed : asRead;
        if (hasUnobservedDefinition && result.proof == ValueFlowProof::Proven && !(proven.primitives & Primitive::Undefined) && !findValueHeldAnyway(program, proven)) {
            stored = maker.unite(stored, inputs.universe().undefinedValue());
            result = resultWith(stored);
        }
        update.fields[index].position = positions.updateFrom(result, asRead);
        if (declared != field.declared && !update.fields[index].position.type)
            update.fields[index].position.type = declared.written();
        // `{ run: null }` in code without types: only the first value says that this field holds null, and every store is known.
        bool takesWhatIsStored = false;
        if (result.proof == ValueFlowProof::OutsideFamily && declared.isExpected() && !declared.site && !field.typeBeforeNarrowing && flow.location.reads == ReadState::OnlyCalled && asRead.hasOnlyPrimitives(Primitive::UndefinedOrNull) && inputs.sets().meet(stored, inputs.universe().closures())) {
            ValueFlowPositionResult amongAllValues = positions.resultAmongAllValues(startsAbsent ? maker.unite(stored, maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Absent))) : stored);
            if (amongAllValues.proof == ValueFlowProof::Proven && amongAllValues.narrowed) {
                update.fields[index].position = { .type = types.intern(*amongAllValues.narrowed), .isProven = true };
                takesWhatIsStored = true;
            }
        }

        ValueSetID surelyInside = inputs.universe().membersSurelyInside(maker, program, declared.written());
        bool storesOnlyObjectsOfType = (result.proof == ValueFlowProof::Proven && !positions.holdsObjectThatMayLieOutside(stored, asRead)) || result.proof == ValueFlowProof::NothingArrives
            || positions.isSurelyInside(maker.intersect(stored, inputs.universe().objects()), asRead);
        Range storedIntegers = storedNumbers.integers;
        bool isNeverEmptied = flow.location.initialization == InitializationState::SetAtCreation && flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined });
        bool leavesRoomForMarkers = asRead.hasOnlyPrimitives(Primitive::Integer) && asRead.integers == Range { minInlineInt32, INT32_MAX };
        bool needsNoMarkers = isNeverEmptied && leavesRoomForMarkers && storedIntegers.isSubsetOf(Range::int32());
        if (asRead.primitives & Primitive::Integer && (storedIntegers.isSubsetOf(asRead.integers) || needsNoMarkers))
            surelyInside = maker.unite(surelyInside, maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Integer)));
        bool storesOnlyPrimitivesOfType = inputs.sets().isSubset(primitivesAmong(stored), surelyInside);
        if (inputs.build.isEnabled(FlowResultKind::HolderMutations, id.index()) && !update.fields[index].accessor.isAccessor() && (asRead.isAnything() || takesWhatIsStored || (storesOnlyObjectsOfType && storesOnlyPrimitivesOfType)))
            flow.stability.add(FieldStability::IsNeverStoredWithOtherType);
        else if (!takesWhatIsStored)
            noteValuesListedWithOtherTypes(index, asRead, surelyInside, storesOnlyObjectsOfType);
        if (!flow.stability.contains(FieldStability::IsNeverStoredWithOtherType) && flow.location.stores == StoreCount::AtMostOnce) {
            flow.location.stores = StoreCount::Unknown;
            flow.location.isUnchangedDuringActivation = false;
        }
        if (needsNoMarkers && !storedIntegers.isSubsetOf(asRead.integers) && flow.stability.contains(FieldStability::IsNeverStoredWithOtherType))
            update.fields[index].position = { .type = types.intern(Type::integer(Range::int32())), .isProven = true };
        bool isEverFilled = hasUnobservedDefinition || !inputs.sets().isEmpty(stored) || !asRead.isJavaScriptValue();
        bool slotStaysWide = slotTakesEveryValue(program, field) && !inputs.sets().meet(stored, inputs.universe().closures());
        bool staysInSlot = flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined }) && (flow.stability.contains(FieldStability::IsNeverStoredWithOtherType) || slotStaysWide);
        if (isEverFilled && flow.location.initialization == InitializationState::SetAtCreation && staysInSlot) {
            flow.occupancy = Occupancy::AlwaysFilled;
            flow.prototypeName = PrototypeNameState::Unknown;
        }
        if (!isEverFilled && flow.stability == everyFlagOf<FieldStability>() && inputs.build.isEnabled(FlowResultKind::StoreCounts, id.index()))
            flow.location.stores = StoreCount::Never;
    }
}

static LocationsStoredUndefined findLocationsStoredUndefined(const ValueFlowResultInputs& inputs, const FlowApplication& application)
{
    const ValueFlowBodies& bodies = inputs.bodies();
    const ParallelLoops& loops = inputs.loops();
    uint32_t numberOfVariables = inputs.program.variables.size();
    uint32_t firstVariable = numberOfVariables ? bodies.nodes().variable(VariableID(0u)).index() : 0;
    BitVector isAskedAbout;
    for (uint32_t node : collectInParallel<uint32_t>(loops, application.layouts.size(), [&](unsigned layout, Vector<uint32_t>& found) {
        const Vector<FlowFieldUpdate>& fields = application.layouts[layout].fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            if (fields[field].undefinedAtStartIsNeverRead)
                found.append(bodies.nodes().environmentField(LayoutID(layout), field).index());
        }
    }))
        isAskedAbout.set(node);

    Vector<uint8_t> variableIsStored(FillWith { }, numberOfVariables, static_cast<uint8_t>(0));
    Vector<uint32_t> fieldsStored = collectInParallel<uint32_t>(loops, bodies.hostBody(), [&](unsigned body, Vector<uint32_t>& found) {
        auto note = [&](ValueFlowNodeID target, NOESCAPE const Invocable<ValueSetID()> auto& findStored) {
            uint32_t node = target.index();
            if (node >= firstVariable && node - firstVariable < numberOfVariables) {
                uint32_t variable = node - firstVariable;
                AtomicRef isStored(variableIsStored[variable]);
                if (inputs.program.variables[variable].initialization == VariableInitialization::Undefined && !isStored.load(std::memory_order_relaxed) && inputs.sets().meet(findStored(), inputs.universe().undefinedValue()))
                    isStored.store(1, std::memory_order_relaxed);
            }
            if (isAskedAbout.get(node) && inputs.sets().meet(findStored(), inputs.universe().undefinedValue()))
                found.append(node);
        };
        for (const ValueFlowEdge& edge : bodies.edgesOnceReached(body))
            note(edge.to, [&] { return inputs.solver().possibleValuesOf(edge.from); });
        for (const ValueFlowNodeSet& seed : bodies.seedsOnceReached(body))
            note(seed.node, [&] { return seed.set; });
    });

    LocationsStoredUndefined stored;
    stored.variables = BitVector(numberOfVariables);
    for (uint32_t variable = 0; variable < numberOfVariables; ++variable) {
        if (variableIsStored[variable])
            stored.variables.quickSet(variable);
    }
    for (uint32_t node : fieldsStored)
        stored.environmentFields.add(node);
    return stored;
}

static void widenToStoredIntegers(const Type& declared, const ValueFlowNumbers* numbers, TypeArena& types, FlowPositionUpdate& update)
{
    Range stored = numbers ? numbers->integers : Range::full();
    if (stored.isSubsetOf(declared.integers))
        return;
    Type held = update.type ? types.at(update.type) : declared;
    held.primitives |= Primitive::Integer;
    held.integers = stored;
    update.type = types.intern(held);
}

// Numbers alone arrive, and not only the integers that were expected: the location holds a number, as one that is declared a number does.
static void holdNumbersThatArrive(const ValueFlowResultInputs& inputs, const DeclaredType& declared, ValueFlowNodeID node, DeadZone deadZone, const ValueFlowNumbers* numbers, ValueFlowPositions& positions, TypeArena& types, FlowPositionUpdate& update)
{
    if (update.isProven || !declared.isExpected() || declared.site || deadZone != DeadZone::IsNeverObserved)
        return;
    ValueFlowPositionResult arriving = positions.resultAmongAllValues(inputs.solver().possibleValuesOf(node));
    if (arriving.proof != ValueFlowProof::Proven || !arriving.narrowed || !arriving.narrowed->isNumber())
        return;
    Type held = *arriving.narrowed;
    if (numbers)
        numbers->narrow(held);
    update = { .type = types.intern(held), .isProven = true };
}

static void narrowVariable(const ValueFlowResultInputs& inputs, VariableID id, const BitVector& variablesStoredUndefined, const NumbersOfPositions& numbers, ValueFlowPositions& positions, TypeArena& types, FlowVariableUpdate& update)
{
    if (update.flow.writers != WriterState::AllKnown)
        return;
    DeadZone deadZone = deadZoneOf(update.flow);
    bool everyReadSeesStore = update.flow.initialization == InitializationState::StoredBeforeEveryRead && update.flow.storeNaming == StoreNaming::EveryStoreNamesLocation;
    if (everyReadSeesStore && inputs.program.at(id).initialization == VariableInitialization::Undefined && !variablesStoredUndefined.quickGet(id.index()))
        deadZone = DeadZone::UndefinedAtStartIsNeverObserved;
    else if (!everyReadSeesStore && startsWithoutValueInSolve(inputs.program, inputs.program.at(id)))
        deadZone = DeadZone::UndefinedAtStartMayBeObserved;
    const DeclaredType& declared = inputs.program.at(id).declared;
    ValueFlowNodeID node = inputs.bodies().nodes().variable(id);
    update.position = positions.updateFor(declared, node, deadZone, numbers.ofVariable(id));
    holdNumbersThatArrive(inputs, declared, node, deadZone, numbers.ofVariable(id), positions, types, update.position);
    if (isStoredIntegersThatOnlyChecksBound(inputs.ledger->writersOf(Place::variable(id))))
        widenToStoredIntegers(inputs.program.at(inputs.program.at(id).declared.written()), numbers.ofVariable(id), types, update.position);
}

using LayoutsCreatedBelowMembers = Vector<Vector<LayoutID, 2>>;

static LayoutsCreatedBelowMembers collectLayoutsCreatedBelowMembers(const Program& program, const FlowApplication& application)
{
    LayoutsCreatedBelowMembers createdBelow(program.functions.size());
    for (uint32_t index = program.numberOfLayoutsAsRead(); index < program.layouts.size(); ++index) {
        if (!program.layouts[index].has(LayoutFlags::IsOfCreatedObject) || application.layouts[index].flow.allocation == AllocationState::NeverAllocated)
            continue;
        LayoutID current(index);
        for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
            const ClassFlow& classFlow = application.layouts[current.index()].classFlow;
            for (const ClassMember& member : classFlow.members) {
                if (!createdBelow[member.function.index()].contains(LayoutID(index)))
                    createdBelow[member.function.index()].append(LayoutID(index));
            }
            current = classFlow.prototypeBase;
        }
    }
    return createdBelow;
}

static void expectThisToBeCreatedBelow(const Program& program, const FlowApplication& application, const DeclaredType& declared, std::span<const LayoutID> createdBelow, TypeArena& types, FlowPositionUpdate& update)
{
    bool isUnchecked = !declared.site && (declared.isExpected() || declared.written() == TypeTable::anything);
    if (createdBelow.empty() || !isUnchecked)
        return;
    Vector<LayoutID, 4> expected(createdBelow);
    if (LayoutSetID layouts = program.at(declared.expected().predicted()).layouts) {
        for (LayoutID layout : program.at(layouts).completeMembers()) {
            if (!application.layouts[layout.index()].objectsAreDictionaries && !expected.contains(layout))
                expected.append(layout);
        }
    }
    update.type = types.intern(Type::forLayouts(expected.span(), types));
    update.replacesExpectation = true;
}

// Treats the argument types that the library declares for a callback like an annotation on the callback, and on every function the callback forwards them to.
static void predictDeclaredCallbackArguments(const ValueFlowResultInputs& inputs, const FlowApplication& application, const DeclaredType& declared, ValueFlowNodeID node, TypeArena& types, FlowPositionUpdate& update)
{
    ValueSetID arguments = inputs.solver().declaredCallbackArguments();
    bool isUnchecked = !declared.site && (declared.isExpected() || declared.written() == TypeTable::anything);
    if (arguments == ValueSets::nothing || update.isProven || !isUnchecked || inputs.program.at(declared.expected().predicted()).layouts)
        return;
    ValueSetID seen = inputs.solver().seenValuesOf(node);
    if (ValueSets::isComplement(seen) || !inputs.sets().meet(seen, arguments))
        return;
    Vector<LayoutID, 4> expected;
    bool hasTooManyLayouts = false;
    inputs.sets().forEachMember(seen, [&](ValueFlowObjectID object) {
        LayoutID layout = inputs.universe().isObject(object) && !ValueFlowResultInputs::isPlaceholder(object) ? inputs.universe().layoutOf(object) : LayoutID();
        if (!layout || application.layouts[layout.index()].objectsAreDictionaries || expected.contains(layout))
            return;
        if (expected.size() == expected.capacity())
            hasTooManyLayouts = true;
        else
            expected.append(layout);
    });
    if (hasTooManyLayouts || expected.isEmpty())
        return;
    update.type = types.intern(Type::forLayouts(expected.span(), types));
    update.replacesExpectation = true;
}

static void narrowEntryParameters(const ValueFlowResultInputs& inputs, FunctionID id, CallerState callers, const NumbersOfPositions& numbers, const FlowApplication& application, std::span<const LayoutID> createdBelow, ValueFlowPositions& positions, TypeArena& types, FlowSignatureUpdate& update)
{
    const Function& function = inputs.program.at(id);
    if (!function.mayChangeSignature())
        return;
    Vector<FlowPositionUpdate> parameters(function.numberOfEntryParameters());
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (callers != CallerState::AllKnown && function.entryParameterKind(index) != EntryParameter::Capture) {
            const DeclaredType* declared = function.thisParameterIndex() == index ? function.declaredEntryParameter(index) : nullptr;
            if (declared)
                expectThisToBeCreatedBelow(inputs.program, application, *declared, createdBelow, types, parameters[index]);
            else if (const DeclaredType* position = function.declaredEntryParameter(index))
                predictDeclaredCallbackArguments(inputs, application, *position, inputs.bodies().nodes().entryParameter(id, index), types, parameters[index]);
            continue;
        }
        const DeclaredType* declared = function.declaredEntryParameter(index);
        if (!declared)
            continue;
        ValueFlowNodeID node = inputs.bodies().nodes().entryParameter(id, index);
        parameters[index] = positions.updateFor(*declared, node, DeadZone::MayBeObserved, numbers.ofEntryParameter(id, index));
        positions.takeWhatArrives(*declared, node, parameters[index]);
        predictDeclaredCallbackArguments(inputs, application, *declared, node, types, parameters[index]);
    }
    if (!std::ranges::all_of(parameters, &FlowPositionUpdate::isEmpty))
        update.entryParameters = WTF::move(parameters);
}

static void narrowResults(const ValueFlowResultInputs& inputs, FunctionID id, const NumbersOfPositions& numbers, ValueFlowPositions& positions, FlowSignatureUpdate& update)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    if (!function.mayChangeSignature())
        return;
    ValueSetMaker& maker = positions.maker();
    Vector<FlowPositionUpdate, 1> results(function.results.size(), [&](size_t index) {
        if (!function.isUnsplitCoroutine())
            return positions.updateFor(function.results[index], inputs.bodies().nodes().result(id, index), DeadZone::MayBeObserved, numbers.ofResult(id, index));
        return positions.updateFrom(positions.resultAt(function.results[index], maker.of(inputs.universe().coroutineResultOf(id))), program.at(function.results[index].written()));
    });
    if (!std::ranges::all_of(results, &FlowPositionUpdate::isEmpty))
        update.results = WTF::move(results);
}

static bool hasTypeOfNarrowedLocation(const FlowApplication& application, const Value& value)
{
    switch (value.opcode) {
    case Opcode::GetModuleVariable:
        return application.variables[value.variable().index()].flow.writers == WriterState::AllKnown;
    case Opcode::GetEnvironmentField:
        return application.layouts[value.layout().index()].fields[value.field()].flow.location.writers == WriterState::AllKnown;
    default:
        return false;
    }
}

static bool hasCheck(const Terminator& terminator)
{
    for (TailCast cast : terminator.casts()) {
        if (cast.resolution == CastResolution::Check)
            return true;
    }
    return false;
}

using InstancesWrittenUnderKeys = HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

// Everything that the program writes under the name, where that is instances of one built-in class beside primitives. A name that is written in many places says little.
static ValueSetID findInstancesWrittenUnder(const ValueFlowResultInputs& inputs, const ValueFlowHeapView& heap, ValueFlowPositions& positions, PropertyKeyID key)
{
    AOT_THRESHOLD(unsigned, mostPlacesWritten, 8);
    const ValueFlowUniverse& universe = inputs.universe();
    ValueSetMaker& maker = positions.maker();
    ValueFlowName name = ValueFlowName::property(inputs.program, key);
    unsigned places = 0;
    heap.forEachNodeWrittenUnder(name, [&](ValueSetID, ValueSetID) {
        ++places;
    });
    if (!places || places > mostPlacesWritten)
        return ValueSets::nothing;
    ValueSetID written = heap.writtenUnder(maker, ValueSets::anything, name);
    if (ValueSets::isComplement(written) || maker.meet(written, universe.placeholdersAlone()))
        return ValueSets::nothing;
    BuiltinClassID builtinClass;
    bool areOfOneClass = true;
    inputs.sets().forEachMember(maker.intersect(written, universe.objects()), [&](ValueFlowObjectID object) {
        BuiltinReceiver receiver = builtinReceiverOf(inputs.program, positions.typeOfObject(object));
        areOfOneClass &= receiver.kind == BuiltinReceiverKind::Instance && receiver.builtinClass && (!builtinClass || builtinClass == receiver.builtinClass);
        builtinClass = receiver.builtinClass;
    });
    if (!areOfOneClass || !builtinClass || builtinClass == BuiltinClasses::Object || builtinClass == BuiltinClasses::Function)
        return ValueSets::nothing;
    return written;
}

static void expectWhatIsWrittenUnderNamesOfReads(const ValueFlowResultInputs& inputs, FunctionID id, const ValueFlowHeapView& heap, InstancesWrittenUnderKeys& writtenUnderKeys, ValueFlowPositions& positions, TypeArena& types, FlowBodyUpdate& update)
{
    const Function& function = inputs.program.at(id);
    ValueSetMaker& maker = positions.maker();
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::PropGet || !statement.site)
            continue;
        PropertyKeyID key = function.at(statement.site).key();
        ValueSetID receivers = inputs.possibleValuesOf(maker, inputs.bodies().operandsOf(statement)[0]);
        if (!key || (!ValueSets::isComplement(receivers) && !maker.meet(receivers, inputs.universe().placeholdersAlone()) && !maker.meet(receivers, inputs.universe().objectsThatMayBeAnything())))
            continue;
        ValueSetID written = writtenUnderKeys.ensure(key.index(), [&] {
            return findInstancesWrittenUnder(inputs, heap, positions, key);
        }).iterator->value;
        if (written == ValueSets::nothing)
            continue;
        if (std::optional<Type> expected = positions.resultAt(Proven::byRulesBehindChecks(TypeTable::anything), written).narrowed)
            update.expectations.append(FlowValueExpectation { statement.site, types.intern(*expected) });
    }
    AOT::sort(update.expectations, { }, &FlowValueExpectation::value);
}

static void proveValuesOfOperations(const ValueFlowResultInputs& inputs, FunctionID id, CallerState callers, const NumbersOfPositions& numbers, ValueFlowPositions& positions, TypeArena& types, const FlowApplication& application, ValuesWithoutProof* valuesWithoutProof, ValueFlowPositionCounts& counts, FlowBodyUpdate& update)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    ValueSetMaker& maker = positions.maker();
    BitVector isDecided(function.values.size());
    BitVector isTypedByReplacedCheck;
    std::optional<Type> anyValue;
    EntryParameterIndexes entryParameters;

    // A cast of a parameter says no more than the parameter does: an unseen caller may hand in an object that only inherits from those flow holds.
    auto mayComeFromUnseenCaller = [&](ValueID value) {
        return callers != CallerState::AllKnown && entryParameters.find(function, definitionThroughSameValues(function, value));
    };
    // runConversionToObject() leaves the global object out of what the uses behind the test meet. In front of the test the value holds it.
    Vector<ValueID, 1> conversionsTestedBeforeUse;
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::ToThis || !isUsedOnlyAsInstanceOfOwnFunction(program, id, statement.site))
            continue;
        if (maker.meet(inputs.possibleValuesOf(maker, inputs.bodies().operandsOf(statement)[0]), maker.of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::Undefined), ValueFlowUniverse::primitive(PrimitiveSet::Null) })))
            conversionsTestedBeforeUse.append(statement.site);
    }
    auto aliveForComparisonsOnly = std::ranges::equal_range(inputs.bodies().valuesAliveForComparisonsOnly(), id, { }, &ValueFlowValueOfFunction::function);
    // The step that finds no further name answers with the empty text, which flow leaves out: nobody reads it.
    auto isNameOfEnumeration = [&](ValueID value) {
        const Value& definition = function.at(value);
        return definition.opcode == Opcode::ResultAt && definition.index() == static_cast<uint32_t>(ForInStepResult::Key) && function.at(definition.resultsOperand()).opcode == Opcode::ForInStep;
    };
    auto prove = [&](ValueID value, NOESCAPE const Invocable<ValueSetID()> auto& findHeld) {
        if (std::ranges::binary_search(aliveForComparisonsOnly, value, { }, &ValueFlowValueOfFunction::value) || isNameOfEnumeration(value))
            return;
        if (isDecided.quickSet(value.index()) || mayComeFromUnseenCaller(value) || hasTypeOfNarrowedLocation(application, function.at(value)))
            return;
        ValueSetID held = findHeld();
        if (!conversionsTestedBeforeUse.isEmpty() && conversionsTestedBeforeUse.contains(definitionThroughSameValues(function, value)))
            held = maker.unite(held, maker.of(inputs.universe().objectOf(Builtins::GlobalThis)));
        bool keepsTypeAsRead = !isTypedByReplacedCheck.get(value.index());
        const Type& asRead = keepsTypeAsRead ? program.typeOf(function, value) : *anyValue;
        std::optional<ValueFlowNumbers> answered = function.at(value).opcode == Opcode::Call ? numbers.ofCall(id, value) : std::nullopt;
        if (mayBeStepOfOne(function.at(value).opcode) && !program.typeOf(function, value).isInteger())
            answered = numbers.numbersOfStep(id, value);
        bool isRemembered = valuesWithoutProof && !answered && keepsTypeAsRead;
        bool isReadByName = function.at(value).opcode == Opcode::PropGet;
        uint64_t question = static_cast<uint64_t>(function.at(value).type.index()) << 33 | static_cast<uint64_t>(isReadByName) << 32 | held.index();
        if (isRemembered) {
            if (valuesWithoutProof->inside.contains(question))
                return;
            if (valuesWithoutProof->outsideFamily.contains(question)) {
                counts.setsOutsideFamilies.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        ProvenType typeByRules = Proven::byRulesBehindChecks(keepsTypeAsRead ? function.at(value).type : TypeTable::anythingOrAbsent);
        ValueFlowPositionResult result = positions.resultAt(typeByRules, held, DeadZone::MayBeObserved, answered ? &*answered : nullptr);
        std::optional<Type>& narrowed = result.narrowed;
        bool isTestedLookup = narrowed && inputs.build.validates() && function.at(value).opcode == Opcode::PropGet && !asRead.isSubsetOf(*narrowed, program);
        bool readsNoObject = narrowed && isReadByName && asRead.hasObjects() && !narrowed->hasObjects();
        if (!narrowed || (!isTestedLookup && !readsNoObject && !isWorthNarrowing(program, asRead, *narrowed))) {
            if (isRemembered)
                (result.proof == ValueFlowProof::OutsideFamily ? valuesWithoutProof->outsideFamily : valuesWithoutProof->inside).add(question);
            return;
        }
        std::optional<uint32_t> parameter = entryParameters.find(function, value);
        const Vector<FlowPositionUpdate>& parameters = application.signatures[id.index()].entryParameters;
        bool isSaidByPosition = parameter && *parameter < parameters.size() && parameters[*parameter].type && !parameters[*parameter].replacesExpectation && types.at(parameters[*parameter].type).isSubsetOf(*narrowed, program);
        if (!isSaidByPosition || inputs.build.validates())
            update.proofs.append(FlowValueProof { value, types.intern(*narrowed), result.isWhatFlowHolds });
    };
    // One thing flow does take from the types as read: an object that it does not follow counts as one of the objects of the type, behind a check and where an operation looks through it.
    Vector<ValueID, 8> valuesTakenForTheirTypes;
    auto holdsObjectThatIsNotFollowed = [&](ValueSetID held) {
        return ValueSets::isComplement(held) || inputs.sets().contains(held, ValueFlowUniverse::untrackedObject());
    };
    auto proveValuesOfStatements = [&] {
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            const Value& operation = function.at(statement.site);
            for (const ValueFlowOperand& operand : inputs.bodies().operandsOf(statement)) {
                if (operand.mask && operand.maskKind == ValueFlowMaskKind::Expected && operand.node && holdsObjectThatIsNotFollowed(inputs.solver().possibleValuesOf(operand.node)))
                    valuesTakenForTheirTypes.append(statement.site);
            }
            if (statement.result && !numberOfResultsOf(program, function, operation))
                prove(statement.site, [&] { return inputs.solver().possibleValuesOf(statement.result); });
            if (!statement.result && PayloadWord::allocationSite(statement.opcode)) {
                ValueFlowObjectID object = inputs.universe().siteObject(statement);
                const ValueFlowSite* site = object ? inputs.universe().siteOf(object) : nullptr;
                if (site && site->origin == ValueFlowSiteOrigin::Operation && !site->layout && site->builtinClass)
                    prove(statement.site, [&] { return maker.of(object); });
            }

            std::span<const ValueFlowOperand> operands = inputs.bodies().operandsOf(statement);
            if (operands.size() != operation.numberOfOperands())
                continue;
            for (uint32_t index = 0; index < operands.size(); ++index) {
                if (operation.operand(index))
                    prove(operation.operand(index), [&] { return inputs.possibleValuesOf(maker, operands[index]); });
            }
        }
    };
    proveValuesOfStatements();

    auto observedValues = std::ranges::equal_range(inputs.bodies().observedValues(), id, { }, &ValueFlowObservedValue::function);
    Vector<ValueID, 4> replacedChecks;
    BitVector checksThatLetAllPass(function.values.size());
    for (const ValueFlowObservedValue& observed : observedValues) {
        if (!observed.test)
            continue;
        const Value& test = function.at(observed.test);
        ValueSetID held = inputs.possibleValuesOf(maker, observed.operand);
        prove(test.operand(0), [&] { return held; });
        if (test.opcode == Opcode::Check && !holdsObjectThatIsNotFollowed(held))
            checksThatLetAllPass.quickSet(observed.test.index());
        if (test.opcode != Opcode::Check || !inputs.build.isEnabled(FlowResultKind::NarrowedTypes, id.index()) || inputs.sets().isEmpty(held))
            continue;
        size_t numberOfClaimsBefore = update.claims.size();
        if (mayComeFromUnseenCaller(test.operand(0))) {
        } else if (positions.isSurelyInside(held, program.at(test.expectedType().predicted()), test.expectedType().predicted()))
            update.claims.append(FlowClaimUpdate { .claim = observed.test, .isRedundant = true, .holdsInEveryVersion = true });
        else if (hasTypeOfNarrowedLocation(application, function.at(test.operand(0)))) {
            if (std::optional<Type> arriving = positions.typeWhereNothingPasses(held, program.at(test.expectedType().predicted()))) {
                update.claims.append(FlowClaimUpdate { .claim = observed.test, .arriving = types.intern(*arriving), .holdsInEveryVersion = true });
                replacedChecks.append(observed.test);
            }
        } else if (std::optional<Type> arriving = positions.typeWhereOnlyInstancesOfBuiltinClassesAreTurnedAway(held, program.at(test.expectedType().predicted()))) {
            update.claims.append(FlowClaimUpdate { .claim = observed.test, .arriving = types.intern(*arriving), .holdsInEveryVersion = true });
            replacedChecks.append(observed.test);
        }
        const Type& expected = program.at(test.expectedType().predicted());
        ValueSetID seen = inputs.seenValuesOf(maker, observed.operand);
        if (update.claims.size() == numberOfClaimsBefore && (positions.turnsAwayConstructedObject(seen, expected) || positions.turnsAwayEveryObject(held, expected) || positions.turnsAwayEverySeenObject(seen, expected)))
            update.claims.append(FlowClaimUpdate { .claim = observed.test, .mayFail = true });
    }

    // What stands behind a replaced check is typed anew from what arrives, so only what flow holds is known of it.
    if (!replacedChecks.isEmpty()) {
        isTypedByReplacedCheck = valuesBehind(function, CFG(function), replacedChecks.span());
        bool provesAnew = update.proofs.removeAllMatching([&](const FlowValueProof& proof) {
            if (!isTypedByReplacedCheck.get(proof.value.index()))
                return false;
            isDecided.quickClear(proof.value.index());
            return true;
        });
        if (provesAnew) {
            anyValue = Type::anything().withPrimitives(Primitive::Absent);
            proveValuesOfStatements();
            for (const ValueFlowObservedValue& observed : observedValues) {
                if (observed.test)
                    prove(function.at(observed.test).operand(0), [&] { return inputs.possibleValuesOf(maker, observed.operand); });
            }
        }
    }
    if (std::ranges::any_of(update.proofs, &FlowValueProof::holdsInEveryVersion) || std::ranges::any_of(update.claims, &FlowClaimUpdate::holdsInEveryVersion)) {
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            if (function.values[index].opcode == Opcode::Check && !checksThatLetAllPass.quickGet(index))
                valuesTakenForTheirTypes.append(ValueID(index));
        }
        if (!valuesTakenForTheirTypes.isEmpty()) {
            BitVector isMadeFromSuchValue = valuesBehind(function, CFG(function), valuesTakenForTheirTypes.span());
            for (FlowValueProof& proof : update.proofs)
                proof.holdsInEveryVersion &= !isMadeFromSuchValue.get(proof.value.index());
            for (FlowClaimUpdate& claim : update.claims)
                claim.holdsInEveryVersion &= !isMadeFromSuchValue.get(function.at(claim.claim).valueOperand().index());
        }
    }
    AOT::sort(update.proofs, { }, &FlowValueProof::value);
    AOT::sort(update.claims, { }, &FlowClaimUpdate::claim);

    Vector<std::pair<ValueID, BlockID>, 2> checkedTailCalls;
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Terminator& terminator = function.blocks[index].terminator;
        if (terminator.kind == TerminatorKind::ReturnCall && hasCheck(terminator))
            checkedTailCalls.append({ terminator.callOperand(), BlockID(index) });
    }
    if (checkedTailCalls.isEmpty() || !inputs.build.isEnabled(FlowResultKind::NarrowedTypes, id.index()))
        return;
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation || !statement.result)
            continue;
        size_t found = checkedTailCalls.findIf([&](const auto& entry) {
            return entry.first == statement.site;
        });
        if (found == notFound)
            continue;
        ValueSetID held = inputs.solver().possibleValuesOf(statement.result);
        if (inputs.sets().isEmpty(held))
            continue;
        bool passesEveryCheck = true;
        for (TailCast cast : function.at(checkedTailCalls[found].second).terminator.casts())
            passesEveryCheck &= cast.resolution != CastResolution::Check || positions.isSurelyInside(held, program.at(cast.declared.expected().predicted()), cast.declared.expected().predicted());
        if (passesEveryCheck)
            update.tailCalls.append(FlowTailCallUpdate { .call = statement.site, .claimsAreRedundant = true });
    }
    AOT::sort(update.tailCalls, { }, &FlowTailCallUpdate::call);
}

static void proveReadsAfterInitialization(const Program& program, const ParallelLoops& loops, TypeTable& types, FlowApplication& application)
{
    for (uint32_t index : findIndicesInParallel(loops, application.bodies.size(), [&](unsigned index) {
        return !application.bodies[index].readsAfterInitialization.isEmpty();
    })) {
        FlowBodyUpdate& update = application.bodies[index];
        const Function& function = program.functions[index];
        for (ValueID read : update.readsAfterInitialization) {
            const Value& value = function.at(read);
            bool isVariable = value.opcode == Opcode::GetModuleVariable;
            const FlowPositionUpdate& position = isVariable ? application.variables[value.variable().index()].position : application.layouts[value.layout().index()].fields[value.field()].position;
            const DeclaredType& declared = isVariable ? program.at(value.variable()).declared : program.at(value.layout()).fields[value.field()].declared;
            const Type& held = types.at(position.type ? position.type : declared.written());
            if (!(held.primitives & Primitive::Absent) || !held.isJavaScriptValue())
                continue;
            bool isOnlyExpected = declared.isExpected() && !position.isProven;
            update.proofs.append(FlowValueProof { read, isOnlyExpected ? TypeTable::anything : types.intern(held.withoutPrimitives(Primitive::Absent)) });
        }
        AOT::sort(update.proofs, { }, &FlowValueProof::value);
    }
}

void computeTypeResults(const ValueFlowResultInputs& inputs, const ValueFlowFieldContents& fieldContents, const LedgerPart& ledgerOfFields, TypeTable& types, FlowResultTables& tables, const ListenerForSetsOutsideTypes* listenerForSetsOutsideTypes)
{
    const Program& program = inputs.program;
    FlowApplication& application = *tables.application;
    unsigned numberOfLayouts = program.layouts.size();
    unsigned numberOfVariableTasks = (program.variables.size() + variablesPerTask - 1) / variablesPerTask;
    unsigned firstFunctionTask = numberOfLayouts + numberOfVariableTasks;
    unsigned numberOfTasks = firstFunctionTask + program.functions.size();
    auto forEachVariableOf = [&](unsigned task, const auto& functor) {
        uint32_t first = (task - numberOfLayouts) * variablesPerTask;
        for (uint32_t index = first; index < std::min<uint32_t>(first + variablesPerTask, program.variables.size()); ++index)
            functor(index);
    };

    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    LocationsStoredUndefined storedUndefined = findLocationsStoredUndefined(inputs, application);
    Vector<BitVector> fieldsWithUnobservedDefinition(numberOfLayouts);
    for (const FieldsDefined& defined : findFieldsDefinedUnobserved(inputs, application)) {
        for (FieldReference field : defined.declared)
            fieldsWithUnobservedDefinition[field.layout.index()].set(field.field);
    }
    RELEASE_ASSERT(inputs.numbersOfValues);
    const auto& numbers = static_cast<const NumbersOfPositions&>(*inputs.numbersOfValues);

    ValueFlowPositionCounts counts(listenerForSetsOutsideTypes);
    auto phase = makeUnique<TypeInterningPhase>(types, numberOfTasks);
    ValueSetMakers makers(inputs);
    constexpr FlowResultKind kindsThatDecideProofs[] {
        FlowResultKind::NarrowedTypes, FlowResultKind::Initialization, FlowResultKind::RemovedNullish, FlowResultKind::ProvenLayoutSets, FlowResultKind::LiteralValues,
        FlowResultKind::ExactFunctions, FlowResultKind::ProvenContents, FlowResultKind::ArrayStorageKinds, FlowResultKind::PristineInstances,
    };
    bool answersAreTheSameForEverySubject = std::ranges::all_of(kindsThatDecideProofs, [&](FlowResultKind kind) {
        return inputs.build.isEnabledEverywhere(kind);
    });
    std::unique_ptr<ValuesWithoutProof> valuesWithoutProof;
    if (answersAreTheSameForEverySubject && !inputs.build.validates() && !listenerForSetsOutsideTypes)
        valuesWithoutProof = makeUnique<ValuesWithoutProof>(inputs.loops(), inputs.solver().input().numberOfNodes / 4);
    LayoutsCreatedBelowMembers createdBelowMembers = collectLayoutsCreatedBelowMembers(program, application);
    ValueFlowHeapView heap(inputs);
    Vector<InstancesWrittenUnderKeys> writtenUnderKeysOfWorker(inputs.loops().width());
    forEachIndexInParallel(inputs.loops(), numberOfTasks, [&](unsigned task, unsigned worker) {
        if (task >= firstFunctionTask && (!inputs.isReached(FunctionID(task - firstFunctionTask)) || !program.functions[task - firstFunctionTask].hasBody()))
            return;
        TypeArena arena(*phase, task);
        ValueSetMaker* maker = &makers.of(worker);
        if (task < numberOfLayouts) {
            ValueFlowPositions positions(inputs, arena, counts, task, maker);
            narrowEnvironmentFields(inputs, LayoutID(task), storedUndefined.environmentFields, numbers, positions, application.layouts[task]);
            narrowFieldsOfObjects(inputs, LayoutID(task), fieldContents, ledgerOfFields, fieldsWithUnobservedDefinition[task], positions, arena, application.layouts[task]);
            return;
        }
        if (task < firstFunctionTask) {
            forEachVariableOf(task, [&](uint32_t index) {
                ValueFlowPositions positions(inputs, arena, counts, index, maker);
                narrowVariable(inputs, VariableID(index), storedUndefined.variables, numbers, positions, arena, application.variables[index]);
            });
            return;
        }
        FunctionID id(task - firstFunctionTask);
        ValueFlowPositions positions(inputs, arena, counts, id.index(), maker);
        CallerState callers = tables.functions.functions[id.index()].callers;
        narrowEntryParameters(inputs, id, callers, numbers, application, createdBelowMembers[id.index()].span(), positions, arena, application.signatures[id.index()]);
        narrowResults(inputs, id, numbers, positions, application.signatures[id.index()]);
        proveValuesOfOperations(inputs, id, callers, numbers, positions, arena, application, valuesWithoutProof.get(), counts, application.bodies[id.index()]);
        expectWhatIsWrittenUnderNamesOfReads(inputs, id, heap, writtenUnderKeysOfWorker[worker], positions, arena, application.bodies[id.index()]);
    });
    inputs.build.destroyLater(WTF::move(valuesWithoutProof));

    times.didFinish("types: layouts, variables, functions"_s);
    inputs.build.statistics().count(StatisticID::FlowSetsOutsideTypes, counts.setsOutsideFamilies.load());
    inputs.build.statistics().count(StatisticID::FlowProvenPositions, counts.provenPositions.load());
    inputs.build.statistics().count(StatisticID::FlowUnprovenPositions, counts.unprovenPositions.load());
    inputs.build.statistics().count(StatisticID::FlowNarrowedPositions, counts.narrowedPositions.load());

    TypeRemap remap = phase->finish(StringRemap { }, &inputs.loops());
    inputs.build.destroyLater(WTF::move(phase));
    times.didFinish("types: arenas merged"_s);
    forEachIndexInParallel(inputs.loops(), numberOfTasks, [&](unsigned task) {
        if (remap.isIdentityFor(task))
            return;
        if (task < numberOfLayouts) {
            remapTypes(application.layouts[task], remap);
            return;
        }
        if (task < firstFunctionTask) {
            forEachVariableOf(task, [&](uint32_t index) {
                remapTypes(application.variables[index].position, remap);
            });
            return;
        }
        remapTypes(application.signatures[task - firstFunctionTask], remap);
        remapTypes(application.bodies[task - firstFunctionTask], remap);
    });
    times.didFinish("types: remapped"_s);
    proveReadsAfterInitialization(program, inputs.loops(), types, application);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

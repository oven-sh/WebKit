/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTAddGenericVersion.h"
#include "AOTBuiltinIDs.h"
#include "AOTCFG.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowApply.h"
#include "AOTFlowRecords.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPhaseTimes.h"
#include "AOTResultRules.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTTypePropagation.h"
#include "AOTUseLists.h"
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
#include <wtf/Scope.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr unsigned variablesPerTask = 256;

// Only the start of a location puts `absent` into its set. No read sees it where a store comes before every read.
static DeadZone deadZoneOf(const LocationFlow& flow)
{
    return flow.initialization == InitializationState::Unknown ? DeadZone::MayBeObserved : DeadZone::IsNeverObserved;
}

// For a position that no node of its own stands for.
static FlowPositionUpdate updateFrom(const ValueFlowPositionResult& result, const Type& family, TypeArena& types, ValueFlowPositionCounts& counts)
{
    FlowPositionUpdate update;
    update.isProven = result.proof == ValueFlowProof::Proven;
    if (family.hasObjects() && result.proof != ValueFlowProof::NothingArrives)
        (update.isProven ? counts.provenPositions : counts.unprovenPositions).fetch_add(1, std::memory_order_relaxed);
    if (!update.isProven || !result.narrowed)
        return update;
    counts.narrowedPositions.fetch_add(1, std::memory_order_relaxed);
    update.type = types.intern(*result.narrowed);
    return update;
}

namespace {

// A set knows numbers by kind alone: an integer is any integer, and the sum of two numbers is any number.
class NumbersOfPositions final : public ValueFlowNumbersOfValues {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NumbersOfPositions);
public:
    NumbersOfPositions(const ValueFlowResultInputs&, ValueFlowHeapView&, const FlowResultTables&);
    void followPositions(const FlowResultTables&);

    ValueFlowNumbers numbersOf(FunctionID, ValueID) const final;
    const ValueFlowNumbers* ofValuesOfMap(ValueFlowObjectID) const final;

    // None where something that nobody has looked at may hand over a number.
    const ValueFlowNumbers* ofVariable(VariableID id) const { return find(id.index()); }
    const ValueFlowNumbers* ofEnvironmentField(LayoutID layout, uint32_t field) const { return find(m_firstField[layout.index()] + field); }
    const ValueFlowNumbers* ofEntryParameter(FunctionID function, uint32_t index) const { return find(m_firstEntryParameter[function.index()] + index); }
    const ValueFlowNumbers* ofResult(FunctionID function, uint32_t index) const { return find(m_firstResult[function.index()] + index); }
    // What a call by value answers. None where something else than a function of the program may be called.
    std::optional<ValueFlowNumbers> ofCall(FunctionID caller, ValueID call) const;

private:
    struct Writer {
        auto order() const { return std::tuple { position, function, value }; }
        friend bool operator==(const Writer&, const Writer&) = default;

        uint32_t position;
        FunctionID function;
        // None for something that nobody has looked at.
        ValueID value;
    };

    // A call whose callees are closures alone, with the set of them.
    struct CallByValue {
        auto order() const { return std::tuple { caller, site, callees.index() }; }
        friend bool operator==(const CallByValue&, const CallByValue&) = default;

        FunctionID caller;
        ValueID site;
        ValueSetID callees;
    };

    // A call of Map.prototype.get and of nothing else, whose receiver is no other object than these Maps.
    struct ReadOfMaps {
        auto order() const { return std::tuple { caller, site }; }

        FunctionID caller;
        ValueID site;
        ValueSetID maps;
    };

    // What an edge hands to a parameter of a block. None for something that no value of the function stands for.
    struct PassedValue {
        auto order() const { return std::tuple { parameter, value }; }

        ValueID parameter;
        ValueID value;
    };

    struct Evaluation {
        TypeArena& arena;
        // Where given, it takes the followed positions whose numbers the evaluation reads.
        Vector<uint32_t>* positionsRead { nullptr };
        unsigned stepsLeft { maxSteps };
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
        // In the list of its task: the followed positions that its writers are computed from.
        uint32_t firstPositionRead { 0 };
        uint32_t endOfPositionsRead { 0 };
    };

    // The writers of one function with a loop, in m_writersInLoopingFunctions.
    struct LoopingFunction {
        uint32_t firstWriter;
        uint32_t endOfWriters;
        bool isTyped { false };
        // The followed positions that the last typing has asked about, ascending. A typing that gets the same answers asks the same questions and finds the same types.
        Vector<uint32_t> positionsAsked { };
    };

    AOT_CLASS_THRESHOLD(unsigned, maxRounds, 6);
    AOT_CLASS_THRESHOLD(unsigned, maxDepth, 8);
    AOT_CLASS_THRESHOLD(unsigned, maxSteps, 256);
    AOT_CLASS_THRESHOLD(unsigned, maxTypingsOfLoops, 3);
    static constexpr unsigned positionsPerTask = 64;

    const ValueFlowNumbers* find(uint32_t position) const { return m_isFollowed.quickGet(position) ? &m_numbers[position] : nullptr; }
    void startFromTypes();
    void collectCalls();
    void choosePositions(const FlowResultTables&);
    void collectWritersOf(FunctionID, Vector<Writer>&) const;
    void collectWriters(const FlowResultTables&);
    void findMaps();
    BitVector collectWritersOfMaps(ValueFlowHeapView&);
    std::optional<uint32_t> indexOfMap(ValueFlowObjectID) const;
    const ReadOfMaps* readOfMapsAt(FunctionID caller, ValueID site) const;
    void findValuesThatRestOnChecks(const FlowResultTables&);
    std::span<const CallByValue> callsAt(FunctionID caller, ValueID site) const;
    Type typeAt(uint32_t position, Type&& known) const;
    Type typeAt(Evaluation&, uint32_t position, Type&& known) const;
    std::optional<Type> typeOfCall(Evaluation&, FunctionID caller, ValueID site) const;
    Type typeOf(Evaluation&, FunctionID, ValueID, unsigned depth) const;
    Type typeWithoutProof(const DeclaredType&) const;
    bool typeWritersByRules(TypeArena&, FunctionID, std::span<const uint32_t> writers, Vector<uint32_t>& positionsAsked);
    bool typeLoopsByRules();
    bool runRound();

    const ValueFlowResultInputs& m_inputs;
    // The variables, then the fields of the environments, the entry parameters, the results and the Maps.
    Vector<uint32_t> m_firstField;
    Vector<uint32_t> m_firstEntryParameter;
    Vector<uint32_t> m_firstResult;
    uint32_t m_firstMap { 0 };
    // The Maps that the program allocates and that only followed code gets hold of, ascending.
    Vector<ValueFlowObjectID> m_maps;
    Vector<Writer> m_writersOfMaps;
    // By Map: the numbers in the constants out of which values of the Map are taken.
    Vector<ValueFlowNumbers> m_numbersInConstantsOfMaps;
    Vector<ReadOfMaps> m_readsOfMaps;
    Vector<ValueFlowNumbers> m_numbers;
    BitVector m_isFollowed;
    Vector<CallByValue> m_calls;
    // By function. The threads of a loop set them: a rule, an accessor, a protocol or the code of a built-in enters the function.
    Vector<uint8_t> m_hasOtherEntry;
    bool m_someCallEntersAnything { false };
    // By function. Empty for a function without a check.
    Vector<BitVector> m_restsOnCheck;
    // By function, in the order of the parameters: what arrives at the parameters of blocks that rest on a check.
    Vector<Vector<PassedValue>> m_passedValues;
    // Of the positions that are followed: no object and no placeholder arrives, and which kinds of primitives do.
    BitVector m_holdsPrimitivesAlone;
    Vector<uint16_t> m_primitives;
    Vector<Writer> m_writers;
    Vector<FollowedPosition> m_followed;
    // By writer: what the rules of the IR say about the value, in a function with a loop.
    Vector<ValueFlowNumbers> m_numbersByRules;
    // The writers in functions with a loop, in the order of the functions.
    Vector<uint32_t> m_writersInLoopingFunctions;
    Vector<LoopingFunction> m_loopingFunctions;
    // By task of a round. Empty until the first round.
    Vector<Vector<uint32_t>> m_positionsReadInTask;
    // By position: the last round has found other numbers than the round before it, and some round has since the last typing.
    BitVector m_hasMovedInLastRound;
    BitVector m_hasMovedSinceTyping;
    // By writer: the last typing has found other numbers than the typing before it, and no round has looked at them.
    Vector<uint8_t> m_rulesHaveMoved;
    bool m_someRulesHaveMoved { false };
    uint64_t m_numberOfRounds { 0 };
    uint64_t m_numberOfPositionsLookedAt { 0 };
    uint64_t m_numberOfTypings { 0 };
    uint64_t m_numberOfFunctionsTyped { 0 };
    // The variables, the fields of environments and the results that are followed.
    HashMap<const DeclaredType*, uint32_t> m_positionsOfTypes;
    BitVector m_mapsWithOtherWriter;
};

} // anonymous namespace

NumbersOfPositions::NumbersOfPositions(const ValueFlowResultInputs& inputs, ValueFlowHeapView& heap, const FlowResultTables& tables)
    : m_inputs(inputs)
    , m_hasOtherEntry(FillWith { }, inputs.program.functions.size(), static_cast<uint8_t>(0))
{
    const Program& program = inputs.program;
    uint32_t next = program.variables.size();
    m_firstField = Vector<uint32_t>(program.layouts.size(), [&](size_t index) {
        uint32_t first = next;
        next += program.layouts[index].isEnvironment() ? program.layouts[index].fields.size() : 0;
        return first;
    });
    m_firstEntryParameter = Vector<uint32_t>(program.functions.size(), [&](size_t index) {
        uint32_t first = next;
        next += program.functions[index].numberOfEntryParameters();
        return first;
    });
    m_firstResult = Vector<uint32_t>(program.functions.size(), [&](size_t index) {
        uint32_t first = next;
        next += program.functions[index].results.size();
        return first;
    });
    m_firstMap = next;
    findMaps();
    next += m_maps.size();
    m_numbers = Vector<ValueFlowNumbers>(next, [](size_t) { return ValueFlowNumbers::any(); });
    m_isFollowed = BitVector(next);
    m_holdsPrimitivesAlone = BitVector(next);
    m_hasMovedInLastRound = BitVector(next);
    m_hasMovedSinceTyping = BitVector(next);
    m_primitives = Vector<uint16_t>(FillWith { }, next, static_cast<uint16_t>(0));

    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    // Neither asks for what the other finds.
    runTasksInParallel(inputs.loops(), {
        [&] {
            startFromTypes();
            findValuesThatRestOnChecks(tables);
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
    const ValueFlowResultInputs& inputs = m_inputs;
    PhaseTimes times(inputs.build, PassID::AnalyzeFlow);
    choosePositions(tables);
    times.didFinish("numbers: positions chosen"_s);
    // Which Maps are followed is noted behind the positions: the bits of the Maps share a word with those of the results.
    for (uint32_t index = 0; index < m_maps.size(); ++index) {
        if (!m_mapsWithOtherWriter.quickGet(index))
            m_isFollowed.quickSet(m_firstMap + index);
    }
    m_mapsWithOtherWriter = { };
    collectWriters(tables);
    times.didFinish("numbers: writers"_s);
    for (unsigned typing = 0; typing <= maxTypingsOfLoops; ++typing) {
        for (unsigned round = 0; round < maxRounds && runRound(); ++round) { }
        if (typing == maxTypingsOfLoops || !typeLoopsByRules())
            break;
    }
    if (inputs.build.options().logging.logsPhaseTimes) [[unlikely]]
        dataLogLn("AOT flow: numbers: ", m_numberOfRounds, " rounds over ", m_followed.size(), " positions have looked at ", m_numberOfPositionsLookedAt, "; ", m_numberOfTypings, " typings of ", m_loopingFunctions.size(), " functions with a loop have typed ", m_numberOfFunctionsTyped);
    m_positionsReadInTask.clear();
    m_loopingFunctions.clear();
    times.didFinish("numbers: rounds"_s);
}

// Nothing checks what enters a position that is only expected to hold its type.
void NumbersOfPositions::startFromTypes()
{
    const Program& program = m_inputs.program;
    auto start = [&](uint32_t position, const DeclaredType* declared) {
        if (!declared || !declared->type || declared->flags.contains(PositionFlags::IsExpected))
            return;
        m_numbers[position] = { };
        m_numbers[position].add(program.at(declared->type));
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

void NumbersOfPositions::findValuesThatRestOnChecks(const FlowResultTables& tables)
{
    const Program& program = m_inputs.program;
    m_restsOnCheck = Vector<BitVector>(program.functions.size());
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
        m_restsOnCheck[index] = computeTaintedValues(function, cfg, checks.span());

        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            BlockID block(blockIndex);
            const Block& contents = function.at(block);
            if (block == entryBlock || contents.isHandlerBlock())
                continue;
            for (uint32_t parameter = 0; parameter < contents.parameters.size(); ++parameter) {
                if (!m_restsOnCheck[index].get(contents.parameters[parameter].index()))
                    continue;
                for (Edge edge : cfg.incomingEdges(block))
                    m_passedValues[index].append({ contents.parameters[parameter], valuePassedOn(function, edge, parameter) });
            }
        }
        AOT::sort(m_passedValues[index], { }, &PassedValue::order);
    });
}

// Every rule that enters a closure asks for the group of a set that holds it. A call whose callees are closures alone asks for the group of its callees and for no other, and hands its arguments to their parameters one by one.
void NumbersOfPositions::collectCalls()
{
    const ValueFlowSolver& solver = m_inputs.solver();
    const ValueFlowUniverse& universe = m_inputs.universe();
    const ValueSets& sets = m_inputs.sets();
    ValueSetID closuresAndPrimitives = BorrowedValueSetMaker(m_inputs.makerPool()).get().unite(universe.closures(), universe.primitives());
    std::atomic<bool> someCallEntersAnything { false };
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
            // A node holds what the value is in the typed and in the generic version. A call of a primitive throws.
            const ValueFlowOperand& callee = m_inputs.bodies().operandsOf(statement)[0];
            calleeSets = { callee.node ? solver.valueOf(callee.node) : ValueSets::nothing, callee.constant ? callee.constant : ValueSets::nothing };
            callsClosuresAlone = std::ranges::all_of(calleeSets, [&](ValueSetID set) {
                return sets.isSubset(set, closuresAndPrimitives);
            });
        }
        // A mask on the callee may have kept a closure out of the group.
        auto holdsEveryCallee = [&](ValueSetID group) {
            bool holdsAll = true;
            for (ValueSetID set : calleeSets) {
                sets.forEachMember(set, [&](ValueFlowObjectID member) {
                    holdsAll &= !universe.closureFunction(member) || sets.contains(group, member);
                });
            }
            return holdsAll;
        };
        ValueFlowCallKind ownKind = statement.opcode == Opcode::Construct ? ValueFlowCallKind::Construct : ValueFlowCallKind::Call;
        m_inputs.forEachMadeNodeAskedForBy(id, kindsOfMadeNodes({ ValueFlowMadeNodeKind::CallGroup }), false, [&](const ValueFlowNodeKey& key, ValueFlowStatementID made) {
            // The statement of a group asks for the nodes of that group.
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
            sets.forEachMember(key.set, [&](ValueFlowObjectID member) {
                if (FunctionID function = universe.closureFunction(member))
                    AtomicRef(m_hasOtherEntry[function.index()]).store(1, std::memory_order_relaxed);
            });
        });
    });
    m_someCallEntersAnything = someCallEntersAnything.load(std::memory_order_relaxed);
    sortAndRemoveDuplicates(m_inputs.loops(), m_calls, [](const CallByValue& a, const CallByValue& b) {
        return a.order() < b.order();
    }, std::equal_to<CallByValue>());
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
    const Type& anything = program.at(TypeTable::anything);
    struct Followed {
        uint32_t position;
        bool holdsPrimitivesAlone;
    };
    struct Chooser {
        Chooser(const ValueFlowResultInputs& inputs, ValueFlowPositionCounts& counts, ValueSetMaker& maker, Vector<Followed>& followed)
            : arena(inputs.program.types)
            , positions(inputs, arena, counts, 0, &maker)
            , followed(followed)
        {
        }

        ScratchTypeArena arena;
        ValueFlowPositions positions;
        Vector<Followed>& followed;
    };
    auto follow = [&](Chooser& chooser, uint32_t position, ValueFlowNodeID node) {
        ValueSetID held = m_inputs.solver().valueOf(node);
        if (!ValueSets::isComplement(held) && !sets.meet(held, numbers))
            return;
        ValueFlowPositionResult result = chooser.positions.resultAt(anything, held);
        bool holdsPrimitivesAlone = result.proof == ValueFlowProof::Proven && result.narrowed && !result.narrowed->hasObjects();
        chooser.followed.append({ position, holdsPrimitivesAlone });
        if (holdsPrimitivesAlone)
            m_primitives[position] = result.narrowed->primitives | (result.narrowed->stringLiterals.isEmpty() ? 0 : Primitive::String) | (result.narrowed->symbolLiterals.isEmpty() ? 0 : Primitive::Symbol);
    };
    ValueSetMakers makers(m_inputs);
    constexpr unsigned indicesPerTask = 1024;
    auto forEachIndex = [&](unsigned count, const auto& functor) {
        unsigned numberOfTasks = (count + indicesPerTask - 1) / indicesPerTask;
        Vector<Vector<Followed>> followedOfTask(numberOfTasks);
        forEachIndexInParallel(m_inputs.loops(), numberOfTasks, [&](unsigned task, unsigned worker) {
            Chooser chooser(m_inputs, counts, makers.of(worker), followedOfTask[task]);
            for (unsigned index = task * indicesPerTask; index < std::min(count, (task + 1) * indicesPerTask); ++index)
                functor(chooser, index);
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

    // A store into a static field whose writers are known names the variable or is listed among the accesses through objects.
    auto everyStoreThroughObjectSpellsItsValue = [&](VariableID id) {
        return std::ranges::all_of(m_inputs.variablesOnObjects->accessesTo(id), [&](const ValueFlowAccessThroughObject& access) {
            return access.kind != ValueFlowAccessThroughObjectKind::Store || indexOfValueStoredIntoProperty(program.at(access.function).at(access.site).opcode);
        });
    };

    forEachIndex(program.variables.size(), [&](Chooser& chooser, unsigned index) {
        const LocationFlow& flow = application.variables[index].flow;
        if (namesEveryWriter(flow) || (program.variables[index].owner && flow.writers == WriterState::AllKnown && everyStoreThroughObjectSpellsItsValue(VariableID(index))))
            follow(chooser, index, nodes.variable(VariableID(index)));
    });
    forEachIndex(program.layouts.size(), [&](Chooser& chooser, unsigned index) {
        const Layout& layout = program.layouts[index];
        for (uint32_t field = 0; layout.isEnvironment() && field < layout.fields.size(); ++field) {
            if (namesEveryWriter(application.layouts[index].fields[field].flow.location))
                follow(chooser, m_firstField[index] + field, nodes.environmentField(LayoutID(index), field));
        }
    });
    forEachIndex(program.functions.size(), [&](Chooser& chooser, unsigned index) {
        FunctionID id(index);
        const Function& function = program.functions[index];
        if (!m_inputs.isReached(id) || !function.hasBody())
            return;
        ValueFlowObjectID closure = m_inputs.universe().closureOf(id);
        bool everyEntryIsCall = !m_someCallEntersAnything && !m_hasOtherEntry[index] && !function.isModuleBody() && tables.functions.functions[index].callers == CallerState::AllKnown
            && !m_inputs.hasEscaped(closure) && !m_inputs.isLost(closure);
        for (uint32_t parameter = 0; parameter < function.numberOfEntryParameters(); ++parameter) {
            EntryParameter kind = function.entryParameterKind(parameter);
            // Only an operation that makes a closure, and a call that names the function, fill a capture.
            if (kind == EntryParameter::Capture || (kind == EntryParameter::Parameter && everyEntryIsCall))
                follow(chooser, m_firstEntryParameter[index] + parameter, nodes.entryParameter(id, parameter));
        }
        if (function.results.size() == 1 && !function.isUnsplitCoroutine())
            follow(chooser, m_firstResult[index], nodes.result(id, 0));
    });
}

void NumbersOfPositions::collectWritersOf(FunctionID id, Vector<Writer>& writers) const
{
    const Program& program = m_inputs.program;
    const Function& function = program.at(id);
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
            for (uint32_t index = 0; index < fields.size(); ++index)
                add(m_firstField[value.layout().index()] + fields[index], value.operand(OperandIndex::NewObject::values + index));
            break;
        }
        case Opcode::NewClosure:
        case Opcode::CallDirect: {
            std::span<const ValueID> operands = value.variadicOperands();
            for (uint32_t index = 0; index < operands.size(); ++index)
                add(m_firstEntryParameter[value.function().index()] + index, operands[index]);
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
        // A return without a value answers undefined.
        if (terminator.kind == TerminatorKind::Return && terminator.valueOperand())
            add(m_firstResult[id.index()], terminator.valueOperand());
    }
}

void NumbersOfPositions::collectWriters(const FlowResultTables& tables)
{
    const Program& program = m_inputs.program;
    m_writers = collectInParallel<Writer>(m_inputs.loops(), program.functions.size(), [&](unsigned index, Vector<Writer>& writers) {
        FunctionID id(index);
        if (program.functions[index].hasBody() && (m_inputs.isReached(id) || isUnsolvedCode(m_inputs, tables, id)))
            collectWritersOf(id, writers);
    });
    m_writers.appendVector(collectInParallel<Writer>(m_inputs.loops(), m_calls.size(), [&](unsigned index, Vector<Writer>& writers) {
        const CallByValue& call = m_calls[index];
        std::span<const ValueID> arguments = program.at(call.caller).at(call.site).variadicOperands();
        m_inputs.sets().forEachMember(call.callees, [&](ValueFlowObjectID member) {
            FunctionID id = m_inputs.universe().closureFunction(member);
            if (!id)
                return;
            const Function& callee = program.at(id);
            uint32_t first = m_firstEntryParameter[id.index()] + callee.firstParameterIndex();
            // A position that the call leaves empty gets undefined.
            for (uint32_t position = 0; position < std::min<size_t>(arguments.size(), callee.numberOfPositionalParameters()); ++position) {
                if (m_isFollowed.quickGet(first + position))
                    writers.append({ first + position, call.caller, arguments[position] });
            }
        });
    }));
    for (const ValueFlowAccessThroughObject& access : m_inputs.variablesOnObjects->accesses) {
        if (access.kind != ValueFlowAccessThroughObjectKind::Store || !m_isFollowed.quickGet(access.variable.index()))
            continue;
        const Value& store = program.at(access.function).at(access.site);
        m_writers.append({ access.variable.index(), access.function, store.operand(*indexOfValueStoredIntoProperty(store.opcode)) });
    }
    m_writers.appendVector(std::exchange(m_writersOfMaps, { }));
    parallelSort(m_inputs.loops(), m_writers.mutableSpan(), [](const Writer& a, const Writer& b) {
        return a.order() < b.order();
    });
    removeRepeatedElements(m_writers);

    uint32_t writer = 0;
    for (size_t position : m_isFollowed) {
        uint32_t first = writer;
        while (writer < m_writers.size() && m_writers[writer].position == position)
            ++writer;
        m_followed.append({ static_cast<uint32_t>(position), first, writer });
    }
    RELEASE_ASSERT(writer == m_writers.size());

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

void NumbersOfPositions::findMaps()
{
    const ValueFlowUniverse& universe = m_inputs.universe();
    ValueSetID filledAsRecordsSay = m_inputs.solver().valueOf(m_inputs.bodies().nodes().ofHost(ValueFlowHostNode::MapsFilledAsRecordsSay));
    m_maps = collectInParallel<ValueFlowObjectID>(m_inputs.loops(), universe.numberOfObjects(), [&](unsigned index, Vector<ValueFlowObjectID>& maps) {
        ValueFlowObjectID object(index);
        if (classOfFollowedContainer(m_inputs, object) != BuiltinClasses::Map || m_inputs.sets().contains(filledAsRecordsSay, object))
            return;
        // A Map that another built-in answers was filled by that built-in, which spells out no value.
        if (m_inputs.program.at(universe.functionOf(object)).at(universe.siteOf(object)->value).opcode == Opcode::Construct)
            maps.append(object);
    });
}

std::optional<uint32_t> NumbersOfPositions::indexOfMap(ValueFlowObjectID object) const
{
    auto found = std::ranges::lower_bound(m_maps, object);
    if (found == m_maps.end() || *found != object)
        return std::nullopt;
    return static_cast<uint32_t>(found - m_maps.begin());
}

// A value gets into a Map by a call of `set` that spells it out, or out of the pairs that Object.entries has made of objects that operations alone fill.
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
        // What was copied onto an object lies on the objects that it was copied from.
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
        // A string has strings under its indexes, and any other primitive has nothing.
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

    // A node holds what the value is in the typed and in the generic version.
    auto wholeValueOf = [&](Found& found, const ValueFlowOperand& operand) {
        return found.maker.unite(operand.node ? solver.valueOf(operand.node) : ValueSets::nothing, operand.constant ? operand.constant : ValueSets::nothing);
    };
    // A call of a primitive throws, and so does the method on a receiver that is a primitive.
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

            // What a call does to an operand its callees do: a function of the program with operations of its own, a built-in as its records say. An operand of a callee that nobody knows has escaped.
            if (effectRuleOf(statement.opcode) == EffectRule::Call)
                continue;
            std::optional<uint8_t> receiver = receiverOf(statement.opcode);
            std::optional<uint32_t> stored = indexOfValueStoredIntoProperty(statement.opcode);
            for (uint32_t operand = 0; operand < operands.size(); ++operand) {
                bool mayBeWatched = (operands[operand].constant && found.setsWithWatched.contains(operands[operand].constant)) || (operands[operand].node && found.setsWithWatched.contains(solver.valueOf(operands[operand].node)));
                if (!mayBeWatched)
                    continue;
                bool storesIntoProperty = stored && receiver && operand == *receiver;
                if (!storesIntoProperty && !operandUsesOf(statement.opcode, operand).containsAny(usesWritingObject))
                    continue;
                ValueSetID written = heap.valueOf(found.maker, operands[operand]);
                // A property of a Map lies beside what the Map holds.
                if (storesIntoProperty) {
                    noteStoredIntoSources(found, written, id, operation.operand(*stored));
                    continue;
                }
                // The objects that properties are copied from are among the sources. The other two put no value on the object.
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

    // The writers are sorted with all the others, the reads below, and the rest is the same in whatever order it is united.
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
        m_inputs.sets().forEachMember(call.callees, [&](ValueFlowObjectID member) {
            FunctionID id = m_inputs.universe().closureFunction(member);
            // The caller of a coroutine gets a promise or a generator.
            if (id && program.at(id).isUnsplitCoroutine())
                return;
            numbers.unite(id && program.at(id).results.size() == 1 ? m_numbers[m_firstResult[id.index()]] : ValueFlowNumbers::any());
        });
    }
    return numbers;
}

// The set of a position that is followed holds whatever arrives there, in the typed and in the generic versions of the functions. It says which kinds of primitives those are.
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

// Only the numbers of a followed position ever move.
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

// What the value is in the typed and in the generic version of its function.
Type NumbersOfPositions::typeOf(Evaluation& evaluation, FunctionID id, ValueID valueID, unsigned depth) const
{
    const Program& program = m_inputs.program;
    const Function& function = program.at(id);
    const Value& value = function.at(valueID);
    bool restsOnCheck = m_restsOnCheck[id.index()].get(valueID.index());
    Type known = program.at(restsOnCheck || !value.type ? TypeTable::anything : value.type);
    if (!depth || !evaluation.stepsLeft || !(known.primitives & Primitive::Number))
        return known;
    --evaluation.stepsLeft;
    auto withNumbersOf = [&](const Type& type) {
        ValueFlowNumbers numbers;
        numbers.add(type);
        numbers.narrow(known);
        return known;
    };

    // What was found about a position holds for whoever reads it, with whatever operands.
    switch (value.opcode) {
    case Opcode::GetModuleVariable:
        return typeAt(evaluation, value.variable().index(), WTF::move(known));
    case Opcode::GetEnvironmentField:
        return typeAt(evaluation, m_firstField[value.layout().index()] + value.field(), WTF::move(known));
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
        if (!restsOnCheck)
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
        if (!restsOnCheck)
            return withNumbersOf(operand);
        // No cast lets the mark of a dead zone through.
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
    Type computed = computeOperatorResultType(TypingContext { program, function, evaluation.arena }, value.opcode, operandTypes.span());
    return restsOnCheck ? computed : withNumbersOf(computed);
}

ValueFlowNumbers NumbersOfPositions::numbersOf(FunctionID id, ValueID value) const
{
    ScratchTypeArena scratch(m_inputs.program.types);
    Evaluation evaluation { scratch.arena() };
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

// What a read of the position gives by its type alone.
Type NumbersOfPositions::typeWithoutProof(const DeclaredType& declared) const
{
    return m_inputs.program.at(m_inputs.program.provenTypeOf(declared));
}

// A counter of a loop is an integer by induction, and under a bound by the test that the loop ends on.
bool NumbersOfPositions::typeWritersByRules(TypeArena& arena, FunctionID id, std::span<const uint32_t> writers, Vector<uint32_t>& positionsAsked)
{
    const Program& program = m_inputs.program;
    Function copy = program.at(id);
    for (uint32_t index = 0; index < copy.numberOfEntryParameters(); ++index) {
        DeclaredType* declared = copy.declaredEntryParameter(index);
        uint32_t position = m_firstEntryParameter[id.index()] + index;
        if (!declared || !declared->type || !m_isFollowed.quickGet(position))
            continue;
        positionsAsked.append(position);
        declared->type = arena.intern(typeAt(position, typeWithoutProof(*declared)));
        declared->flags.remove(PositionFlags::IsExpected);
    }
    for (size_t index : m_restsOnCheck[id.index()]) {
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

// Answers whether it has found something.
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

// Answers whether it has found something.
bool NumbersOfPositions::runRound()
{
    unsigned numberOfTasks = (m_followed.size() + positionsPerTask - 1) / positionsPerTask;
    bool isFirstRound = m_positionsReadInTask.isEmpty();
    if (isFirstRound)
        m_positionsReadInTask.grow(numberOfTasks);
    bool verifies = m_inputs.build.options().verifiesAfterEveryPass;
    std::atomic<uint64_t> numberOfPositionsLookedAt { 0 };
    Vector<ValueFlowNumbers> found(m_followed.size());
    forEachIndexInParallel(m_inputs.loops(), numberOfTasks, [&](unsigned task) {
        std::optional<ScratchTypeArena> arena;
        Vector<uint32_t>& positionsRead = m_positionsReadInTask[task];
        uint64_t numberLookedAt = 0;
        for (size_t index = task * positionsPerTask; index < std::min<size_t>((task + 1) * positionsPerTask, m_followed.size()); ++index) {
            FollowedPosition& followed = m_followed[index];
            size_t numberOfWriters = followed.endOfWriters - followed.firstWriter;
            bool isHandedOtherNumbers = isFirstRound
                || (m_someRulesHaveMoved && std::ranges::any_of(m_rulesHaveMoved.subspan(followed.firstWriter, numberOfWriters), [](uint8_t hasMoved) { return !!hasMoved; }))
                || std::ranges::any_of(positionsRead.subspan(followed.firstPositionRead, followed.endOfPositionsRead - followed.firstPositionRead), [&](uint32_t position) { return m_hasMovedInLastRound.quickGet(position); });
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
            for (const Writer& writer : m_writers.subspan(followed.firstWriter, numberOfWriters)) {
                Evaluation evaluation { *arena, isFirstRound ? &positionsRead : nullptr };
                ValueFlowNumbers handedOver;
                handedOver.add(typeOf(evaluation, writer.function, writer.value, maxDepth));
                arriving.unite(handedOver.meetWith(m_numbersByRules[&writer - m_writers.span().data()]));
            }
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
    ++m_numberOfRounds;
    m_numberOfPositionsLookedAt += numberOfPositionsLookedAt.load(std::memory_order_relaxed);
    return hasFoundSomething;
}

// What is written by something other than an operation is not asked of the set.
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
        // Nothing deletes or redefines a field of an environment, and each operation that stores to it stores a value of its type.
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

static bool canHoldUnobservedDefinition(const Program& program, const Type& type)
{
    return type.primitives & Primitive::Undefined || findValueHeldAnyway(program, type);
}

static FieldReference fieldDefinedBy(const Program& program, const UnobservedFieldDefinition& definition)
{
    return declarationOf(program, { definition.classLayout, *program.at(definition.classLayout).findField(definition.key) });
}

// The initializer of a class runs on the objects of the classes below it. There the name may be a field of its own, with a type of its own: code of the class below stores to it.
using FieldsDefined = Vector<FieldReference, 2>;
static Vector<FieldsDefined> findFieldsDefinedUnobserved(const ValueFlowResultInputs& inputs)
{
    const Program& program = inputs.program;
    std::span<const UnobservedFieldDefinition> definitions = inputs.bodies().unobservedDefinitions().all();
    Vector<FieldsDefined> fields(definitions.size());
    Vector<Vector<uint32_t, 2>> definitionsOfClass(definitions.empty() ? 0 : program.layouts.size());
    for (uint32_t index = 0; index < definitions.size(); ++index) {
        fields[index].append(fieldDefinedBy(program, definitions[index]));
        definitionsOfClass[definitions[index].classLayout.index()].append(index);
    }
    if (definitions.empty())
        return fields;
    for (uint32_t heir = 0; heir < program.layouts.size(); ++heir) {
        LayoutID base = program.layouts[heir].base;
        for (size_t steps = 0; base && steps < program.layouts.size(); ++steps, base = program.at(base).base) {
            for (uint32_t index : definitionsOfClass[base.index()]) {
                if (std::optional<uint32_t> field = program.layouts[heir].findField(definitions[index].key))
                    fields[index].appendIfNotContains(declarationOf(program, { LayoutID(heir), *field }));
            }
        }
    }
    return fields;
}

static Vector<BitVector> findFieldsWithUnobservedDefinition(const ValueFlowResultInputs& inputs, const Vector<FieldsDefined>& fieldsDefined)
{
    Vector<BitVector> fields(inputs.program.layouts.size());
    for (const FieldsDefined& defined : fieldsDefined) {
        for (FieldReference field : defined)
            fields[field.layout.index()].set(field.field);
    }
    return fields;
}

static void replaceUnobservedDefinitions(const ValueFlowResultInputs& inputs, const Vector<FieldsDefined>& fieldsDefined, TypeTable& types, FlowApplication& application)
{
    const Program& program = inputs.program;
    std::span<const UnobservedFieldDefinition> definitions = inputs.bodies().unobservedDefinitions().all();
    auto positionOf = [&](FieldReference reference) -> FlowPositionUpdate& {
        return application.layouts[reference.layout.index()].fields[reference.field].position;
    };
    auto typeOf = [&](FieldReference reference) {
        const Field& field = fieldAt(program, reference);
        const FlowPositionUpdate& position = positionOf(reference);
        TypeID typeIfProven = position.type ? position.type : field.declared.type;
        return position.isProven ? typeIfProven : slotTypeOf(program, field);
    };
    auto holdsUndefined = [&](FieldReference reference) {
        return !!(types.at(typeOf(reference)).primitives & Primitive::Undefined);
    };
    for (uint32_t index = 0; index < definitions.size(); ++index) {
        const UnobservedFieldDefinition& definition = definitions[index];
        if (!inputs.isReached(definition.initializer) || std::ranges::all_of(fieldsDefined[index], holdsUndefined))
            continue;
        // One operation defines them all, so it defines what all of them hold.
        ScratchTypeArena scratch(types);
        Type common = types.at(typeOf(fieldsDefined[index].first()));
        for (FieldReference reference : fieldsDefined[index].span().subspan(1))
            common = common.intersectWith(types.at(typeOf(reference)), scratch);
        if (std::optional<Value> value = findValueHeldAnyway(program, common)) {
            application.bodies[definition.initializer.index()].definedValues.append({ definition.definition, WTF::move(*value) });
            continue;
        }
        for (FieldReference reference : fieldsDefined[index]) {
            if (positionOf(reference).isProven && !holdsUndefined(reference) && program.at(fieldAt(program, reference).declared.type).primitives & Primitive::Undefined)
                positionOf(reference).type = types.intern(types.at(typeOf(reference)).withPrimitives(Primitive::Undefined));
        }
    }
}

// A field is absent until it is stored, which no store says. A value outside the type of a field takes the field out of its slot, so that no such value is ever stored is a result of its own.
static void narrowFieldsOfObjects(const ValueFlowResultInputs& inputs, LayoutID id, const ValueFlowFieldContents& contents, const BitVector& fieldsWithUnobservedDefinition, ValueFlowPositions& positions, TypeArena& types, ValueFlowPositionCounts& counts, FlowLayoutUpdate& update)
{
    const Program& program = inputs.program;
    const Layout& layout = program.at(id);
    if (!hasObjectsThatCodeAlters(layout.kind))
        return;
    ValueSetMaker& maker = positions.maker();
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        FieldFlow& flow = update.fields[index].flow;
        if (flow.location.writers != WriterState::AllKnown)
            continue;
        ValueSetID stored = contents.stored[id.index()][index];
        const Type& asRead = program.at(layout.fields[index].declared.type);
        bool startsAbsent = asRead.primitives & Primitive::Absent && !inputs.sets().isEmpty(stored);
        auto resultWith = [&](ValueSetID held) {
            return positions.resultAt(layout.fields[index].declared, startsAbsent ? maker.unite(held, maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Absent))) : held);
        };
        ValueFlowPositionResult result = resultWith(stored);
        bool hasUnobservedDefinition = fieldsWithUnobservedDefinition.get(index);
        if (hasUnobservedDefinition && result.proof == ValueFlowProof::Proven && !canHoldUnobservedDefinition(program, result.narrowed ? *result.narrowed : asRead)) {
            stored = maker.unite(stored, inputs.universe().undefinedValue());
            result = resultWith(stored);
        }
        update.fields[index].position = updateFrom(result, asRead, types, counts);

        // The proof speaks for the objects: none is ill-typed and none is a stranger to the family.
        ValueSetID surelyInside = inputs.universe().membersSurelyInside(maker, program, layout.fields[index].declared.type);
        // A store through a wider view of the object is tested against the view, so the proof, which speaks for what the slot holds, takes nothing on trust here.
        bool storesOnlyObjectsOfType = (result.proof == ValueFlowProof::Proven && !positions.holdsObjectThatMayLieOutside(stored, asRead)) || result.proof == ValueFlowProof::NothingArrives
            || positions.isSurelyInside(maker.intersect(stored, inputs.universe().objects()), asRead);
        // A set knows integers by kind alone, so the kind is never surely inside a range. The proven types of the values that the operations store say which integers they are.
        const ValueFlowNumbers& storedNumbers = contents.storedNumbers[id.index()][index];
        Range storedIntegers = storedNumbers.integers;
        bool isNeverEmptied = flow.location.initialization == InitializationState::SetAtCreation && flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined });
        bool leavesRoomForMarkers = asRead.hasOnlyPrimitives(Primitive::Integer) && asRead.integers == Range { minInlineInt32, INT32_MAX };
        bool needsNoMarkers = isNeverEmptied && leavesRoomForMarkers && storedIntegers.isSubsetOf(Range::int32());
        if (asRead.primitives & Primitive::Integer && (storedIntegers.isSubsetOf(asRead.integers) || needsNoMarkers))
            surelyInside = maker.unite(surelyInside, maker.of(ValueFlowUniverse::primitive(PrimitiveSet::Integer)));
        ValueSetID storedPrimitives = maker.intersect(stored, inputs.universe().primitives());
        if (!storedNumbers.mayBeOtherThanIntegers)
            storedPrimitives = maker.intersect(storedPrimitives, ValueSets::complement(maker.of(std::array { ValueFlowUniverse::primitive(PrimitiveSet::WholeNumber), ValueFlowUniverse::primitive(PrimitiveSet::FractionalNumber) })));
        bool storesOnlyPrimitivesOfType = inputs.sets().isSubset(storedPrimitives, surelyInside);
        // Which getters the slot of an accessor takes is a guess: a pair with another getter, or with a setter alone, is turned away and lies beside the slots.
        if (inputs.build.isEnabled(FlowResultKind::HolderMutations, id.index()) && !update.fields[index].accessor.isAccessor() && (asRead.isAnything() || (storesOnlyObjectsOfType && storesOnlyPrimitivesOfType)))
            flow.stability.add(FieldStability::IsNeverStoredWithOtherType);
        // An allocation writes the slot without a test.
        if (!flow.stability.contains(FieldStability::IsNeverStoredWithOtherType) && flow.location.stores == StoreCount::AtMostOnce) {
            flow.location.stores = StoreCount::Unknown;
            flow.location.isUnchangedDuringActivation = false;
        }
        if (needsNoMarkers && !storedIntegers.isSubsetOf(asRead.integers) && flow.stability.contains(FieldStability::IsNeverStoredWithOtherType))
            update.fields[index].position = { .type = types.intern(Type::integer(Range::int32())), .isProven = true };
        // No set stands for a word that is no value of the language, like the token in the brand of a class.
        bool isEverFilled = hasUnobservedDefinition || !inputs.sets().isEmpty(stored) || !asRead.isJavaScriptValue();
        // DeferClosuresInFields may narrow the slot of a field that is given a closure to objects alone.
        bool slotStaysWide = slotTakesEveryValue(program, layout.fields[index]) && !inputs.sets().meet(stored, inputs.universe().closures());
        bool staysInSlot = flow.stability.containsAll({ FieldStability::IsNeverDeleted, FieldStability::IsNeverRedefined }) && (flow.stability.contains(FieldStability::IsNeverStoredWithOtherType) || slotStaysWide);
        // No lookup goes on past a slot that is always filled, so nobody asks what lies above it.
        if (isEverFilled && flow.location.initialization == InitializationState::SetAtCreation && staysInSlot) {
            flow.occupancy = Occupancy::AlwaysFilled;
            flow.prototypeName = PrototypeNameState::Unknown;
        }
        // No allocation lists the field, no operation stores under its name or under a key nobody knows, and no accessor takes its place.
        if (!isEverFilled && flow.stability == everyFlagOf<FieldStability>() && inputs.build.isEnabled(FlowResultKind::StoreCounts, id.index()))
            flow.location.stores = StoreCount::Never;
    }
}

namespace {

struct LocationsStoredUndefined {
    BitVector variables;
    HashSet<uint32_t> environmentFields;
};

} // anonymous namespace

// The host's body puts `undefined` into a variable of `var`. Every other `undefined` in its set comes along an edge or as a constant from the body of a function that stores to the variable.
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
            note(edge.to, [&] { return inputs.solver().valueOf(edge.from); });
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

static void narrowVariable(const ValueFlowResultInputs& inputs, VariableID id, const BitVector& variablesStoredUndefined, const NumbersOfPositions& numbers, ValueFlowPositions& positions, FlowVariableUpdate& update)
{
    if (update.flow.writers != WriterState::AllKnown)
        return;
    DeadZone deadZone = deadZoneOf(update.flow);
    bool everyReadSeesStore = update.flow.initialization == InitializationState::StoredBeforeEveryRead && update.flow.storeNaming == StoreNaming::EveryStoreNamesLocation;
    if (everyReadSeesStore && inputs.program.at(id).initialization == VariableInitialization::Undefined && !variablesStoredUndefined.quickGet(id.index()))
        deadZone = DeadZone::UndefinedAtStartIsNeverObserved;
    else if (!everyReadSeesStore && startsWithoutValueInSolve(inputs.program, inputs.program.at(id)))
        deadZone = DeadZone::UndefinedAtStartMayBeObserved;
    update.position = positions.updateFor(inputs.program.at(id).declared, inputs.bodies().nodes().variable(id), deadZone, numbers.ofVariable(id));
}

// The node of a capture holds what every reached operation that makes a closure of the function stores there, and what a call that names the function hands on.
static void narrowEntryParameters(const ValueFlowResultInputs& inputs, FunctionID id, CallerState callers, const NumbersOfPositions& numbers, ValueFlowPositions& positions, FlowSignatureUpdate& update)
{
    const Function& function = inputs.program.at(id);
    if (!function.mayChangeSignature())
        return;
    Vector<FlowPositionUpdate> parameters(function.numberOfEntryParameters());
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        if (callers != CallerState::AllKnown && function.entryParameterKind(index) != EntryParameter::Capture)
            continue;
        const DeclaredType* declared = function.declaredEntryParameter(index);
        if (!declared)
            continue;
        ValueFlowNodeID node = inputs.bodies().nodes().entryParameter(id, index);
        parameters[index] = positions.updateFor(*declared, node, DeadZone::MayBeObserved, numbers.ofEntryParameter(id, index));
        positions.takeWhatArrives(*declared, node, parameters[index]);
    }
    if (!std::ranges::all_of(parameters, &FlowPositionUpdate::isEmpty))
        update.entryParameters = WTF::move(parameters);
}

// The result of a coroutine as its callers see it is the promise or the generator that stands for all its calls. The node holds what the body returns, which whoever awaits or steps that object gets.
static void narrowResults(const ValueFlowResultInputs& inputs, FunctionID id, const NumbersOfPositions& numbers, ValueFlowPositions& positions, TypeArena& types, ValueFlowPositionCounts& counts, FlowSignatureUpdate& update)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    if (!function.mayChangeSignature())
        return;
    ValueSetMaker& maker = positions.maker();
    Vector<FlowPositionUpdate, 1> results(function.results.size(), [&](size_t index) {
        if (!function.isUnsplitCoroutine())
            return positions.updateFor(function.results[index], inputs.bodies().nodes().result(id, index), DeadZone::MayBeObserved, numbers.ofResult(id, index));
        const Type& asRead = program.at(function.results[index].type);
        return updateFrom(positions.resultAt(asRead, maker.of(inputs.universe().coroutineResultOf(id))), asRead, types, counts);
    });
    if (!std::ranges::all_of(results, &FlowPositionUpdate::isEmpty))
        update.results = WTF::move(results);
}

// The values that the reduced body names: what an operation gives and what it takes, what a branch tests and what a cast checks.
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

namespace {

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

static bool hasCheck(const Terminator& terminator)
{
    for (TailCast cast : terminator.casts()) {
        if (cast.resolution == CastResolution::Check)
            return true;
    }
    return false;
}

static void proveValuesOfOperations(const ValueFlowResultInputs& inputs, FunctionID id, CallerState callers, const NumbersOfPositions& numbers, ValueFlowPositions& positions, TypeArena& types, const FlowApplication& application, ValuesWithoutProof* valuesWithoutProof, ValueFlowPositionCounts& counts, FlowBodyUpdate& update)
{
    const Program& program = inputs.program;
    const Function& function = program.at(id);
    ValueSetMaker& maker = positions.maker();
    ValueSetID placeholders = maker.unite(inputs.universe().untrackedObjectAlone(), inputs.universe().unknownObjectAlone());
    BitVector isDecided(function.values.size());

    auto setOf = [&](const ValueFlowOperand& operand) {
        ValueSetID held = operand.constant ? operand.constant : ValueSets::nothing;
        if (operand.node)
            held = maker.unite(held, inputs.solver().valueOf(operand.node));
        if (!operand.mask)
            return held;
        switch (operand.maskKind) {
        case ValueFlowMaskKind::Plain:
            return maker.intersect(held, operand.mask);
        case ValueFlowMaskKind::Checked:
            // A placeholder that has passed the test is still whatever it stands for. The positions say what that is inside the type of the value.
            return maker.unite(maker.intersect(held, operand.mask), maker.intersect(held, placeholders));
        case ValueFlowMaskKind::Declared:
        case ValueFlowMaskKind::Expected:
            // The type is taken on trust, so it cuts nothing: a stranger that has arrived takes the proof away.
            return held;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto mayComeFromUnseenCaller = [&](ValueID value) {
        return callers != CallerState::AllKnown && function.at(entryBlock).parameters.find(value) != notFound;
    };
    // The first use of a value decides. A set is a union or an intersection that has to be made, so it is made for that use alone.
    auto aliveForComparisonsOnly = std::ranges::equal_range(inputs.bodies().valuesAliveForComparisonsOnly(), id, { }, &ValueFlowValueOfFunction::function);
    auto prove = [&](ValueID value, NOESCAPE const Invocable<ValueSetID()> auto& findHeld) {
        // The body keeps it for its set alone, which says which closures somebody can tell apart.
        if (std::ranges::binary_search(aliveForComparisonsOnly, value, { }, &ValueFlowValueOfFunction::value))
            return;
        if (isDecided.quickSet(value.index()) || mayComeFromUnseenCaller(value) || hasTypeOfNarrowedLocation(application, function.at(value)))
            return;
        ValueSetID held = findHeld();
        const Type& asRead = program.typeOf(function, value);
        std::optional<ValueFlowNumbers> answered = function.at(value).opcode == Opcode::Call ? numbers.ofCall(id, value) : std::nullopt;
        bool isRemembered = valuesWithoutProof && !answered;
        uint64_t question = static_cast<uint64_t>(function.at(value).type.index()) << 32 | held.index();
        if (isRemembered) {
            if (valuesWithoutProof->inside.contains(question))
                return;
            if (valuesWithoutProof->outsideFamily.contains(question)) {
                counts.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        ValueFlowPositionResult result = positions.resultAt(asRead, held, DeadZone::MayBeObserved, answered ? &*answered : nullptr);
        std::optional<Type>& narrowed = result.narrowed;
        // Later passes answer a lookup by name from the records, also where its narrower type buys nothing. In a validating build the cast brings the test that keeps the lookup.
        bool isTestedLookup = narrowed && inputs.build.validates() && function.at(value).opcode == Opcode::PropGet && !asRead.isSubsetOf(*narrowed, program);
        if (!narrowed || (!isTestedLookup && !isWorthNarrowing(program, asRead, *narrowed))) {
            if (isRemembered)
                (result.proof == ValueFlowProof::OutsideFamily ? valuesWithoutProof->outsideFamily : valuesWithoutProof->inside).add(question);
            return;
        }
        // An entry parameter has the type of its position, so a proof that says no more than that type puts no cast into the body. In a validating build the cast brings the test of that type.
        size_t parameter = function.at(entryBlock).parameters.find(value);
        const Vector<FlowPositionUpdate>& parameters = application.signatures[id.index()].entryParameters;
        bool isSaidByPosition = parameter != notFound && parameter < parameters.size() && parameters[parameter].type && !parameters[parameter].replacesExpectation && types.at(parameters[parameter].type).isSubsetOf(*narrowed, program);
        if (!isSaidByPosition || inputs.build.validates())
            update.proofs.append(FlowValueProof { value, types.intern(*narrowed) });
    };
    for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(id.index())) {
        if (statement.kind != ValueFlowStatementKind::Operation)
            continue;
        const Value& operation = function.at(statement.site);
        if (statement.result && !numberOfResultsOf(program, function, operation))
            prove(statement.site, [&] { return inputs.solver().valueOf(statement.result); });
        // No node holds what an operation allocates itself: its value is the object that stands for all that it allocates. A direct call, which is edges alone, may be the only one that takes the value.
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
                prove(operation.operand(index), [&] { return setOf(operands[index]); });
        }
    }

    // What a branch tests and what a cast checks. A check that every member of the set passes tests nothing. No placeholder is sure to pass one.
    for (const ValueFlowObservedValue& observed : std::ranges::equal_range(inputs.bodies().observedValues(), id, { }, &ValueFlowObservedValue::function)) {
        if (!observed.test)
            continue;
        const Value& test = function.at(observed.test);
        ValueSetID held = setOf(observed.operand);
        prove(test.operand(0), [&] { return held; });
        if (test.opcode != Opcode::Check || !inputs.build.isEnabled(FlowResultKind::NarrowedTypes, id.index()) || inputs.sets().isEmpty(held))
            continue;
        size_t numberOfClaimsBefore = update.claims.size();
        // Nothing is proven of what an unseen caller passes.
        if (mayComeFromUnseenCaller(test.operand(0))) {
        } else if (positions.isSurelyInside(held, test.castType()))
            update.claims.append(FlowClaimUpdate { .claim = observed.test, .isRedundant = true });
        else if (hasTypeOfNarrowedLocation(application, function.at(test.operand(0)))) {
            // The type of such a location was only expected, or the check would not stand behind the read. Of any other value the proof of its type stands in front of the check, which then fails by the types alone.
            if (std::optional<Type> arriving = positions.typeWhereNothingPasses(held, program.at(test.castType())))
                update.claims.append(FlowClaimUpdate { .claim = observed.test, .arriving = types.intern(*arriving) });
        }
        if (update.claims.size() == numberOfClaimsBefore && positions.turnsAwayConstructedObject(held, program.at(test.castType())))
            update.claims.append(FlowClaimUpdate { .claim = observed.test, .mayFail = true });
    }
    AOT::sort(update.proofs, { }, &FlowValueProof::value);
    AOT::sort(update.claims, { }, &FlowClaimUpdate::claim);

    // A test behind a call ends the tail call, so the call stays one where whatever it answers passes every check.
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
        ValueSetID held = inputs.solver().valueOf(statement.result);
        if (inputs.sets().isEmpty(held))
            continue;
        bool passesEveryCheck = true;
        for (TailCast cast : function.at(checkedTailCalls[found].second).terminator.casts())
            passesEveryCheck &= cast.resolution != CastResolution::Check || positions.isSurelyInside(held, cast.declared.type);
        if (passesEveryCheck)
            update.tailCalls.append(FlowTailCallUpdate { .call = statement.site, .claimsAreRedundant = true });
    }
    AOT::sort(update.tailCalls, { }, &FlowTailCallUpdate::call);
}

// Such a read has the type of its location, which holds `absent` for the sake of other reads.
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
            const Type& held = types.at(position.type ? position.type : declared.type);
            if (!(held.primitives & Primitive::Absent) || !held.isJavaScriptValue())
                continue;
            // Nothing is proven about what a position holds that is only expected to hold its type.
            bool isOnlyExpected = declared.flags.contains(PositionFlags::IsExpected) && !position.isProven;
            update.proofs.append(FlowValueProof { read, isOnlyExpected ? TypeTable::anything : types.intern(held.withoutPrimitives(Primitive::Absent)) });
        }
        AOT::sort(update.proofs, { }, &FlowValueProof::value);
    }
}

void computeTypeResults(const ValueFlowResultInputs& inputs, const ValueFlowFieldContents& fieldContents, TypeTable& types, FlowResultTables& tables, const ListenerForSetsOutsideTypes* listenerForSetsOutsideTypes)
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
    LocationsStoredUndefined locationsStoredUndefined = findLocationsStoredUndefined(inputs, application);
    const BitVector& variablesStoredUndefined = locationsStoredUndefined.variables;
    const HashSet<uint32_t>& environmentFieldsStoredUndefined = locationsStoredUndefined.environmentFields;
    Vector<FieldsDefined> fieldsDefinedUnobserved = findFieldsDefinedUnobserved(inputs);
    Vector<BitVector> fieldsWithUnobservedDefinition = findFieldsWithUnobservedDefinition(inputs, fieldsDefinedUnobserved);
    RELEASE_ASSERT(inputs.numbersOfValues);
    const auto& numbers = static_cast<const NumbersOfPositions&>(*inputs.numbersOfValues);

    // The subject of the switches is the variable, the layout or the function that the positions belong to.
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
    forEachIndexInParallel(inputs.loops(), numberOfTasks, [&](unsigned task, unsigned worker) {
        if (task >= firstFunctionTask && (!inputs.isReached(FunctionID(task - firstFunctionTask)) || !program.functions[task - firstFunctionTask].hasBody()))
            return;
        TypeArena arena(*phase, task);
        ValueSetMaker* maker = &makers.of(worker);
        if (task < numberOfLayouts) {
            ValueFlowPositions positions(inputs, arena, counts, task, maker);
            narrowEnvironmentFields(inputs, LayoutID(task), environmentFieldsStoredUndefined, numbers, positions, application.layouts[task]);
            narrowFieldsOfObjects(inputs, LayoutID(task), fieldContents, fieldsWithUnobservedDefinition[task], positions, arena, counts, application.layouts[task]);
            return;
        }
        if (task < firstFunctionTask) {
            forEachVariableOf(task, [&](uint32_t index) {
                ValueFlowPositions positions(inputs, arena, counts, index, maker);
                narrowVariable(inputs, VariableID(index), variablesStoredUndefined, numbers, positions, application.variables[index]);
            });
            return;
        }
        FunctionID id(task - firstFunctionTask);
        ValueFlowPositions positions(inputs, arena, counts, id.index(), maker);
        CallerState callers = tables.functions.functions[id.index()].callers;
        narrowEntryParameters(inputs, id, callers, numbers, positions, application.signatures[id.index()]);
        narrowResults(inputs, id, numbers, positions, arena, counts, application.signatures[id.index()]);
        proveValuesOfOperations(inputs, id, callers, numbers, positions, arena, application, valuesWithoutProof.get(), counts, application.bodies[id.index()]);
    });
    inputs.build.destroyLater(WTF::move(valuesWithoutProof));

    times.didFinish("types: layouts, variables, functions"_s);
    inputs.build.statistics().count(StatisticID::FlowSetsOutsideTypes, counts.count.load());
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
    replaceUnobservedDefinitions(inputs, fieldsDefinedUnobserved, types, application);
    proveReadsAfterInitialization(program, inputs.loops(), types, application);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

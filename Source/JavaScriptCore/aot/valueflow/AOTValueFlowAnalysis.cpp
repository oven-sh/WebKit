/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTKnownClosures.h"
#include "AOTPhaseTimes.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTValueFlowRules.h"
#include <wtf/MonotonicTime.h>

namespace JSC { namespace AOT { namespace Typed {

static bool mayHaveInstances(const Function& function)
{
    bool isPlainBody = function.hasBody() && (function.role == FunctionRole::Source || function.role == FunctionRole::Library) && (function.kind == FunctionKind::Plain || function.kind == FunctionKind::Method || function.kind == FunctionKind::Arrow);
    return isPlainBody && !function.isUnsplitCoroutine() && !function.hiddenRestParameterIndex() && !function.argumentCountParameterIndex();
}

// Which positional parameter the value is, as it came. None for any other value.
static std::optional<unsigned> positionOfParameter(const Function& function, ValueID id)
{
    if (!id)
        return std::nullopt;
    size_t index = function.at(entryBlock).parameters.find(definitionThroughSameValues(function, id));
    if (index == notFound || function.entryParameterKind(index) != EntryParameter::Parameter)
        return std::nullopt;
    unsigned position = index - function.firstParameterIndex();
    return position < function.numberOfPositionalParameters() ? std::optional(position) : std::nullopt;
}

// A parameter is a key where the body stores under it, where the body calls what it has read under it, or where the body hands it on to a function that takes a key there.
Vector<uint32_t> ValueFlowInstancedFunctions::findParametersThatAreKeys(const Program& program, const ParallelLoops& loops)
{
    struct Argument {
        uint32_t index;
        uint32_t positionOfParameter;
    };
    // A call that passes a parameter of its function on.
    struct CallPassingParameters {
        uint32_t caller;
        FunctionID callee;
        PropertyKeyID nameOfCallee;
        uint32_t endOfArguments;
    };
    struct Found {
        Vector<std::pair<uint32_t, uint32_t>> functionsStoredUnderNames;
        Vector<std::pair<uint32_t, uint32_t>> namesStoredAgain;
        Vector<CallPassingParameters> calls;
        Vector<Argument> arguments;
    };

    auto functionBoundTo = [&](const Function& function, ValueID id) -> FunctionID {
        const Value& value = function.at(definitionThroughSameValues(function, id));
        if (value.opcode == Opcode::NewClosure)
            return value.function();
        if (value.opcode != Opcode::GetModuleVariable)
            return { };
        const Variable& variable = program.at(value.variable());
        return variable.initialization == VariableInitialization::Function && variable.flags.contains(VariableFlags::IsImmutable) ? variable.function : FunctionID();
    };

    Vector<uint32_t> positionOfKey;
    positionOfKey.fill(noKey, program.functions.size());
    auto scan = [&](unsigned index, Found& body) {
        const Function& function = program.functions[index];
        bool hasInstances = mayHaveInstances(function);
        auto note = [&](unsigned position) {
            positionOfKey[index] = std::min<uint32_t>(positionOfKey[index], position);
        };
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                switch (value.opcode) {
                case Opcode::ElemSet:
                case Opcode::ElemDefine:
                    static_assert(OperandIndex::ElemSet::property == 1 && OperandIndex::ElemDefine::property == 1);
                    if (std::optional<unsigned> position = hasInstances ? positionOfParameter(function, value.operand(1)) : std::nullopt)
                        note(*position);
                    break;
                case Opcode::Call:
                case Opcode::CallList:
                    // `this.events[type].call(this, a)`, `this.events[type](a)`.
                    for (ValueID called : { value.calleeOperand(), value.thisValueOperand() }) {
                        if (!called || !hasInstances)
                            continue;
                        const Value& read = function.at(definitionThroughTests(function, called));
                        if (std::optional<unsigned> position = read.opcode == Opcode::ElemGet ? positionOfParameter(function, read.operand(OperandIndex::ElemGet::property)) : std::nullopt)
                            note(*position);
                    }
                    break;
                case Opcode::PropSet:
                case Opcode::PropDefine:
                case Opcode::DefineMethod: {
                    static_assert(OperandIndex::PropSet::value == 1 && OperandIndex::PropDefine::value == 1 && OperandIndex::DefineMethod::function == 1);
                    if (!value.key())
                        break;
                    if (FunctionID stored = functionBoundTo(function, value.operand(1)))
                        body.functionsStoredUnderNames.append({ value.key().index(), stored.index() });
                    else if (const Value& read = function.at(definitionThroughSameValues(function, value.operand(1))); read.opcode == Opcode::PropGet && read.key())
                        body.namesStoredAgain.append({ read.key().index(), value.key().index() });
                    break;
                }
                default:
                    break;
                }
                if (!hasInstances || (value.opcode != Opcode::Call && value.opcode != Opcode::Construct))
                    continue;
                FunctionID callee = functionBoundTo(function, value.calleeOperand());
                PropertyKeyID nameOfCallee = !callee && value.opcode == Opcode::Call ? value.calleeKey() : PropertyKeyID();
                if (!callee && !nameOfCallee)
                    continue;
                size_t numberOfArgumentsBefore = body.arguments.size();
                for (uint32_t operand = 2; operand < value.numberOfOperands(); ++operand) {
                    if (std::optional<unsigned> position = positionOfParameter(function, value.operand(operand)))
                        body.arguments.append({ operand - 2, *position });
                }
                if (body.arguments.size() != numberOfArgumentsBefore)
                    body.calls.append({ index, callee, nameOfCallee, static_cast<uint32_t>(body.arguments.size()) });
            }
        }
    };
    constexpr unsigned functionsInChunk = 256;
    unsigned numberOfFunctions = program.functions.size();
    ParallelArray<Found> bodies(loops, (numberOfFunctions + functionsInChunk - 1) / functionsInChunk, [&](unsigned chunk) {
        Found found;
        unsigned end = std::min(numberOfFunctions, (chunk + 1) * functionsInChunk);
        for (unsigned index = chunk * functionsInChunk; index < end; ++index)
            scan(index, found);
        return found;
    });

    // By the name: the functions that are stored under it, and the names under which what lies under it is stored again, `prototype.on = prototype.addListener`.
    constexpr uint32_t noList = UINT32_MAX;
    Vector<uint32_t> listOfName;
    listOfName.fill(noList, program.types.numberOfPropertyKeys());
    Vector<Vector<uint32_t, 2>> functionsUnderName;
    // Only a list that takes the functions of another one is asked what it holds.
    BitVector takesFunctionsOfOtherNames(program.types.numberOfPropertyKeys());
    using SetOfFunctions = HashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    Vector<SetOfFunctions> listed;
    auto ensureListOf = [&](uint32_t name) {
        uint32_t& list = listOfName[name];
        if (list == noList) {
            list = static_cast<uint32_t>(functionsUnderName.size());
            functionsUnderName.append(Vector<uint32_t, 2>());
            listed.append(SetOfFunctions());
        }
        return list;
    };
    struct NameStoredAgain {
        uint32_t from;
        uint32_t to;
        // Where numberTaken says how many functions of the first list the second one has got. The same two lists share it wherever they stand.
        uint32_t pair;
    };
    Vector<NameStoredAgain> namesStoredAgain;
    Vector<uint32_t> numberTaken;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> numberOfPair;
    for (unsigned index = 0; index < bodies.size(); ++index) {
        for (auto [from, to] : bodies[index].namesStoredAgain)
            takesFunctionsOfOtherNames.quickSet(to);
    }
    for (unsigned index = 0; index < bodies.size(); ++index) {
        for (auto [name, function] : bodies[index].functionsStoredUnderNames) {
            uint32_t list = ensureListOf(name);
            functionsUnderName[list].append(function);
            if (takesFunctionsOfOtherNames.quickGet(name))
                listed[list].add(function);
        }
    }
    for (unsigned index = 0; index < bodies.size(); ++index) {
        for (auto [nameFrom, nameTo] : bodies[index].namesStoredAgain) {
            uint32_t from = ensureListOf(nameFrom);
            uint32_t to = ensureListOf(nameTo);
            auto pair = numberOfPair.add(static_cast<uint64_t>(from) << 32 | to, static_cast<uint32_t>(numberTaken.size()));
            if (pair.isNewEntry)
                numberTaken.append(0);
            namesStoredAgain.append({ from, to, pair.iterator->value });
        }
    }
    for (bool hasMoved = true; hasMoved;) {
        hasMoved = false;
        for (const NameStoredAgain& stored : namesStoredAgain) {
            uint32_t end = functionsUnderName[stored.from].size();
            for (uint32_t& taken = numberTaken[stored.pair]; taken < end; ++taken) {
                uint32_t function = functionsUnderName[stored.from][taken];
                if (listed[stored.to].add(function).isNewEntry) {
                    functionsUnderName[stored.to].append(function);
                    hasMoved = true;
                }
            }
        }
    }

    // Of the functions under a name a call by that name asks only at which positions they take a key.
    constexpr uint32_t positionsCounted = 8;
    struct PositionsTaken {
        std::array<uint32_t, positionsCounted> functionsTakingKeyAt { };
        uint32_t functionsTakingKeyFurtherOn { 0 };
    };
    Vector<PositionsTaken> positionsTakenUnder(functionsUnderName.size());
    auto count = [&](uint32_t list, uint32_t position, uint32_t change) {
        if (position == noKey)
            return;
        PositionsTaken& taken = positionsTakenUnder[list];
        (position < positionsCounted ? taken.functionsTakingKeyAt[position] : taken.functionsTakingKeyFurtherOn) += change;
    };
    // Only a function with such a call moves. By function: the lists that hold it, once for each time they do.
    BitVector mayMove(numberOfFunctions);
    for (const Found& body : bodies.span()) {
        for (const CallPassingParameters& call : body.calls)
            mayMove.quickSet(call.caller);
    }
    Vector<uint32_t> endOfListsHolding;
    endOfListsHolding.fill(0, numberOfFunctions + 1);
    for (uint32_t list = 0; list < functionsUnderName.size(); ++list) {
        for (uint32_t function : functionsUnderName[list]) {
            count(list, positionOfKey[function], 1);
            endOfListsHolding[function] += mayMove.quickGet(function);
        }
    }
    for (uint32_t function = 0, sum = 0; function <= numberOfFunctions; ++function)
        endOfListsHolding[function] = sum += endOfListsHolding[function];
    Vector<uint32_t> listsHoldingFunctions(endOfListsHolding[numberOfFunctions]);
    for (uint32_t list = functionsUnderName.size(); list--;) {
        for (uint32_t function : functionsUnderName[list]) {
            if (mayMove.quickGet(function))
                listsHoldingFunctions[--endOfListsHolding[function]] = list;
        }
    }
    // The ends have become the starts.
    auto listsHolding = [&](uint32_t function) {
        return listsHoldingFunctions.span().subspan(endOfListsHolding[function], endOfListsHolding[function + 1] - endOfListsHolding[function]);
    };
    BitVector liesUnderNameItCalls;
    {
        size_t numberOfCalls = 0;
        for (const Found& body : bodies.span()) {
            for (const CallPassingParameters& call : body.calls) {
                size_t number = numberOfCalls++;
                if (!call.callee && std::ranges::contains(listsHolding(call.caller), listOfName[call.nameOfCallee.index()]))
                    liesUnderNameItCalls.set(number);
            }
        }
    }

    for (bool hasMoved = true; hasMoved;) {
        hasMoved = false;
        size_t numberOfCall = 0;
        for (const Found& body : bodies.span()) {
            uint32_t firstArgument = 0;
            for (const CallPassingParameters& call : body.calls) {
                uint32_t index = call.caller;
                uint32_t positionBefore = positionOfKey[index];
                size_t number = numberOfCall++;
                std::span<const Argument> arguments = body.arguments.span().subspan(firstArgument, call.endOfArguments - firstArgument);
                firstArgument = call.endOfArguments;
                auto handOnTo = [&](uint32_t callee) {
                    uint32_t taken = positionOfKey[callee];
                    if (taken == noKey)
                        return;
                    for (const Argument& argument : arguments) {
                        if (argument.index == taken && argument.positionOfParameter < positionOfKey[index]) {
                            positionOfKey[index] = argument.positionOfParameter;
                            hasMoved = true;
                        }
                    }
                };
                if (call.callee)
                    handOnTo(call.callee.index());
                else if (uint32_t list = listOfName[call.nameOfCallee.index()]; list != noList) {
                    const PositionsTaken& taken = positionsTakenUnder[list];
                    if (taken.functionsTakingKeyFurtherOn || liesUnderNameItCalls.get(number)) {
                        for (uint32_t callee : functionsUnderName[list])
                            handOnTo(callee);
                    } else {
                        for (const Argument& argument : arguments) {
                            if (argument.index < positionsCounted && taken.functionsTakingKeyAt[argument.index] && argument.positionOfParameter < positionOfKey[index]) {
                                positionOfKey[index] = argument.positionOfParameter;
                                hasMoved = true;
                            }
                        }
                    }
                }
                if (positionOfKey[index] != positionBefore) {
                    for (uint32_t list : listsHolding(index)) {
                        count(list, positionBefore, -1);
                        count(list, positionOfKey[index], 1);
                    }
                }
            }
        }
    }
    return positionOfKey;
}

// A text is a constant of flow where it is a property key of the program. So a text that a call passes where some function takes a key becomes one.
static void makeKeysOfTextsThatCallsPass(Program& program, const ParallelLoops& loops, std::span<const uint32_t> positionOfKey)
{
    program.types.didInternKeysThatOperationsSpell();
    uint64_t positionsOfKeys = 0;
    for (uint32_t position : positionOfKey) {
        if (position < 64)
            positionsOfKeys |= 1ull << position;
    }
    if (!positionsOfKeys)
        return;
    using Text = std::remove_cvref_t<decltype(std::declval<const Value&>().string())>;
    Vector<Text> texts = collectInParallel<Text>(loops, program.functions.size(), [&](unsigned index, Vector<Text>& found) {
        const Function& function = program.functions[index];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& call = function.at(id);
                if (call.opcode != Opcode::Call && call.opcode != Opcode::Construct)
                    continue;
                for (uint32_t operand = 2; operand < call.numberOfOperands() && operand < 66; ++operand) {
                    if (!(positionsOfKeys >> (operand - 2) & 1) || !call.operand(operand))
                        continue;
                    const Value& argument = function.at(definitionThroughSameValues(function, call.operand(operand)));
                    if (argument.opcode == Opcode::String)
                        found.append(argument.string());
                }
            }
        }
    });
    for (const Text& text : texts)
        program.types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
}

// So does a text that the type of a declared position lists: a String operation with that text is then a member of the type, and not any string.
static void internKeysOfDeclaredStringLiterals(Program& program, const ParallelLoops& loops)
{
    BitVector isVisited(program.types.size());
    Vector<TypeID, 16> worklist;
    auto visit = [&](TypeID type) {
        if (type && !isVisited.quickSet(type.index()))
            worklist.append(type);
    };
    auto visitAll = [&](const Vector<TypeID>& types) {
        for (TypeID type : types)
            visit(type);
    };
    visitAll(collectInParallel<TypeID>(loops, program.layouts.size(), [&](unsigned index, Vector<TypeID>& found) {
        for (const Field& field : program.layouts[index].fields)
            found.append(field.declared.type);
    }));
    for (const Variable& variable : program.variables)
        visit(variable.declared.type);
    visitAll(collectInParallel<TypeID>(loops, program.functions.size(), [&](unsigned functionIndex, Vector<TypeID>& found) {
        const Function& function = program.functions[functionIndex];
        for (uint32_t index = 0; index < function.numberOfEntryParameters(); ++index) {
            if (const DeclaredType* declared = function.declaredEntryParameter(index))
                found.append(declared->type);
        }
        for (const DeclaredType& result : function.results)
            found.append(result.type);
    }));
    while (!worklist.isEmpty()) {
        const Type& type = program.types.at(worklist.takeLast());
        for (StringID literal : type.stringLiterals)
            program.types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = literal });
        for (const ClassPart& part : type.classes) {
            for (TypeID contents : part.contents)
                visit(contents);
        }
    }
}

static ValueFlowBodies reduceBodiesTimed(Program& program, BuildContext& build, Vector<uint32_t>& positionOfKey, Seconds& time)
{
    MonotonicTime start = MonotonicTime::now();
    PhaseTimes times(build, PassID::AnalyzeFlow);
    positionOfKey = ValueFlowInstancedFunctions::findParametersThatAreKeys(program, build.parallelLoops());
    makeKeysOfTextsThatCallsPass(program, build.parallelLoops(), positionOfKey.span());
    internKeysOfDeclaredStringLiterals(program, build.parallelLoops());
    times.didFinish("solve: keys"_s);
    ValueFlowBodies bodies = reduceBodies(program, build);
    times.didFinish("solve: bodies"_s);
    time = MonotonicTime::now() - start;
    return bodies;
}

ValueFlowInstancedFunctions::ValueFlowInstancedFunctions(const Program& program, const ValueFlowBodies& bodies, std::span<const uint32_t> positionOfKey, ValueSetMaker& maker, const ParallelLoops& loops)
    : m_closures(ValueSets::nothing)
{
    const ValueFlowProgramNodes& nodes = bodies.nodes();
    const ValueFlowUniverse& universe = bodies.universe();
    m_indexOfFunction.fill(0u, program.functions.size());
    Vector<ValueFlowObjectID, 16> closures;
    auto functionsToInstance = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        FunctionID function(index);
        // A function that tests its arguments runs three bodies for one call.
        return positionOfKey[index] != noKey && !universe.genericVersionOf(function) && nodes.endOfNodesOf(function) - nodes.firstNodeOf(function) <= mostNodesOfBody;
    });
    for (uint32_t index : functionsToInstance) {
        FunctionID function(index);
        m_functions.append(ValueFlowInstancedFunction { function, positionOfKey[index], { }, { } });
        m_indexOfFunction[index] = m_functions.size();
        closures.append(universe.closureOf(function));
    }
    if (m_functions.isEmpty())
        return;
    m_closures = maker.of(closures.span());

    m_isNodeOfFunction.ensureSize(nodes.numberOfNodes());
    for (const ValueFlowInstancedFunction& instanced : m_functions) {
        for (uint32_t node = nodes.firstNodeOf(instanced.function); node < nodes.endOfNodesOf(instanced.function); ++node)
            m_isNodeOfFunction.quickSet(node);
    }
    auto functionHolding = [&](ValueFlowNodeID node) -> ValueFlowInstancedFunction* {
        return holdsNode(node) ? &m_functions[m_indexOfFunction[nodes.functionOf(node).index()] - 1] : nullptr;
    };
    for (std::span<const ValueFlowEdge> edges : { bodies.edges(), bodies.edgesOnceReached() }) {
        for (const ValueFlowEdge& edge : edges) {
            if (ValueFlowInstancedFunction* instanced = functionHolding(edge.to))
                instanced->edges.append(edge);
        }
    }
    for (const ValueFlowNodeSet& seed : bodies.seedsOnceReached()) {
        if (ValueFlowInstancedFunction* instanced = functionHolding(seed.node))
            instanced->seeds.append(seed);
    }
}

ValueFlowSolution::ValueFlowSolution(Program& program, BuildContext& build, ValueFlowSolver::Rounds rounds, Witnesses witnesses, uint32_t mostRounds, EnumSet<ValueFlowUnknownSource> unknownSourcesLeftOut)
    : m_program(program)
    , m_bodies(reduceBodiesTimed(program, build, m_positionOfKey, m_timeToReduceBodies))
    , m_maker(m_bodies.sets())
    , m_makerPool(m_bodies.sets())
{
    m_input = {
        .program = &program,
        .universe = &m_bodies.universe(),
        .nodes = &m_bodies.nodes(),
        .build = &build,
        .escaped = ValueFlowProgramNodes::escaped(),
        .lost = ValueFlowProgramNodes::lost(),
        .placeholders = m_bodies.universe().placeholders(),
        .numberOfNodes = m_bodies.nodes().numberOfNodes(),
        .edges = m_bodies.edges(),
        .masks = m_bodies.masks(),
        .statements = m_bodies.statements(),
        .operands = m_bodies.operands(),
        .edgesOnceReached = m_bodies.edgesOnceReached(),
        .seedsOnceReached = m_bodies.seedsOnceReached(),
        .functionsEnteredOnceReached = m_bodies.functionsEnteredOnceReached(),
        .ranges = m_bodies.ranges(),
        .mostSetMembers = build.limit(LimitID::MaxValueFlowSetMembers),
        .mostMembersToldApart = build.limit(LimitID::MaxValueFlowMembersToldApart),
        .mostEdgesLogged = build.limit(LimitID::MaxValueFlowEdgesLogged),
        // The report names the statement and the position behind each site that is not proven.
        .remembersWhoLoggedEachEdge = witnesses == Witnesses::ForEveryEdge || hasFlag(build.options().reports, ReportSet::Flow),
        .mostRounds = mostRounds,
        .unknownSourcesLeftOut = unknownSourcesLeftOut,
        .refusesEvaluatedText = build.options().excludesEvaluatedText(),
    };
    PhaseTimes times(build, PassID::AnalyzeFlow);
    m_instancedFunctions = makeUnique<ValueFlowInstancedFunctions>(program, m_bodies, m_positionOfKey.span(), m_maker, build.parallelLoops());
    if (!m_instancedFunctions->isEmpty())
        m_input.instancedFunctions = m_instancedFunctions.get();
    times.didFinish("solve: instanced functions"_s);
    MonotonicTime start = MonotonicTime::now();
    m_solver = makeUnique<ValueFlowSolver>(m_input, m_bodies.sets(), runValueFlowRule, build.parallelLoops(), rounds);
    times.didFinish("solve: the solver made"_s);
    m_hasResult = m_solver->solve();
    times.didFinish("solve: rounds"_s);
    m_timeToSolve = MonotonicTime::now() - start;
    for (const std::unique_ptr<ValueSetMaker>& maker : m_solver->makersIdleAfterSolve())
        m_makerPool.giveBack(*maker);
}

ValueFlowSolution::~ValueFlowSolution() = default;

ValueSetMaker& ValueSetMakerPool::take()
{
    {
        Locker locker { m_lock };
        if (!m_idleMakers.isEmpty())
            return *m_idleMakers.takeLast();
    }
    std::unique_ptr<ValueSetMaker> maker = makeUnique<ValueSetMaker>(m_sets);
    ValueSetMaker& result = *maker;
    Locker locker { m_lock };
    m_ownMakers.append(WTF::move(maker));
    return result;
}

void ValueSetMakerPool::giveBack(ValueSetMaker& maker)
{
    Locker locker { m_lock };
    m_idleMakers.append(&maker);
}

ValueSetID ValueFlowSolution::valueOf(const ValueFlowOperand& operand)
{
    ValueSetID value = operand.constant ? operand.constant : ValueSets::nothing;
    if (operand.node)
        value = m_maker.unite(value, m_solver->valueOf(operand.node));
    return ValueFlowRuleContext::throughMaskOf(operand, value, *m_solver, m_maker);
}

// What each await of a reached function is handed and what it gives, and for an await of promises of the program alone that gives something nobody knows: whose promise holds it.
void printAwaits(PrintStream& out, ValueFlowSolution& solution)
{
    constexpr size_t numberOfFunctionsPrinted = 40;
    enum Part : uint8_t { TypedBodies, RestOfProgram, GenericVersions, Library, NumberOfParts };
    constexpr ASCIILiteral namesOfParts[NumberOfParts] { "typed bodies"_s, "the rest of the program"_s, "generic versions"_s, "the library"_s };
    enum Held : uint8_t { UnknownObject, UntrackedObject, KnownAlone, NumberOfHeld };
    enum Awaited : uint8_t { PromisesOfProgram, PromisesOfBuiltins, OtherObjects, PrimitivesAlone, NumberOfAwaited };
    constexpr ASCIILiteral namesOfAwaited[NumberOfAwaited] { "promises of async functions alone"_s, "a promise that a built-in has made among them"_s, "another object among them"_s, "primitives alone"_s };

    const Program& program = solution.program();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    const ValueSets& sets = solution.sets();
    auto whatIsHeld = [&](ValueSetID value) {
        return sets.contains(value, ValueFlowUniverse::unknownObject()) ? UnknownObject : sets.contains(value, ValueFlowUniverse::untrackedObject()) ? UntrackedObject : KnownAlone;
    };

    std::array<std::array<uint32_t, NumberOfHeld>, NumberOfParts> operands { };
    std::array<std::array<std::array<uint32_t, NumberOfHeld>, NumberOfAwaited>, NumberOfParts> resultsOfKnownOperands { };
    uint32_t promisesThatHaveEscaped = 0;
    // By whether a promise among those awaited has escaped, and whether a function among those that made them returns a placeholder.
    std::array<std::array<uint32_t, 2>, 2> awaitsThatGiveUnknownObject { };
    HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> awaitsOfFunction;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!solver.isReached(FunctionID(index)))
            continue;
        FunctionRole role = program.functions[index].role;
        Part part = role == FunctionRole::GenericVersion ? GenericVersions : isWrittenInProgram(program.functions[index]) ? RestOfProgram : Library;
        if (part == RestOfProgram && role == FunctionRole::Source && universe.genericVersionOf(FunctionID(index)))
            part = TypedBodies;
        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || statement.opcode != Opcode::Await || !statement.result)
                continue;
            ValueSetID awaited = solution.valueOf(bodies.operandsOf(statement)[0]);
            Held held = whatIsHeld(awaited);
            ++operands[part][held];
            if (held != KnownAlone)
                continue;
            Awaited kind = PrimitivesAlone;
            sets.forEachMember(awaited, [&](ValueFlowObjectID member) {
                if (!universe.isObject(member))
                    return;
                ValueFlowObjectKind objectKind = universe.kindOf(member);
                Awaited kindOfMember = objectKind == ValueFlowObjectKind::CoroutineResult ? PromisesOfProgram
                    : objectKind == ValueFlowObjectKind::ResultOfBuiltin || (objectKind == ValueFlowObjectKind::Site && sets.contains(universe.instancesOf(BuiltinClasses::Promise), member)) ? PromisesOfBuiltins : OtherObjects;
                kind = kind == PrimitivesAlone ? kindOfMember : std::max(kind, kindOfMember);
            });
            Held given = whatIsHeld(solver.valueOf(statement.result));
            ++resultsOfKnownOperands[part][kind][given];
            if (kind != PromisesOfProgram || given != UnknownObject || part == Library)
                continue;
            bool somePromiseHasEscaped = false;
            bool someFunctionReturnsPlaceholder = false;
            sets.forEachMember(awaited, [&](ValueFlowObjectID member) {
                if (!universe.isObject(member))
                    return;
                FunctionID function = universe.functionOf(member);
                bool hasEscaped = sets.contains(solver.valueOf(ValueFlowProgramNodes::escaped()), member);
                bool returnsPlaceholder = whatIsHeld(solver.valueOf(bodies.nodes().result(function, 0))) != KnownAlone;
                promisesThatHaveEscaped += hasEscaped;
                somePromiseHasEscaped |= hasEscaped;
                someFunctionReturnsPlaceholder |= returnsPlaceholder;
                if (hasEscaped || returnsPlaceholder)
                    ++awaitsOfFunction.add(function.index(), 0).iterator->value;
            });
            ++awaitsThatGiveUnknownObject[somePromiseHasEscaped][someFunctionReturnsPlaceholder];
        }
    }

    for (size_t part = 0; part < NumberOfParts; ++part) {
        out.println("awaits in ", namesOfParts[part], ": handed the unknown object ", operands[part][UnknownObject], ", the untracked object ", operands[part][UntrackedObject], ", known values alone ", operands[part][KnownAlone]);
        for (size_t kind = 0; kind < NumberOfAwaited; ++kind) {
            const auto& results = resultsOfKnownOperands[part][kind];
            out.println("  ", namesOfAwaited[kind], ": gives the unknown object ", results[UnknownObject], ", the untracked object ", results[UntrackedObject], ", known values alone ", results[KnownAlone]);
        }
    }
    out.println("promises of async functions at awaits that give the unknown object: escaped ", promisesThatHaveEscaped);
    out.println("awaits of promises of async functions alone that give the unknown object: a promise has escaped and every function returns known values ", awaitsThatGiveUnknownObject[1][0], ", a promise has escaped and a function returns a placeholder ", awaitsThatGiveUnknownObject[1][1], ", no promise has escaped and a function returns a placeholder ", awaitsThatGiveUnknownObject[0][1], ", neither ", awaitsThatGiveUnknownObject[0][0]);
    Vector<std::pair<uint32_t, uint32_t>> functions;
    for (auto [function, count] : awaitsOfFunction)
        functions.append({ count, function });
    std::ranges::sort(functions, std::greater { });
    out.println("async functions that return the unknown object or whose promise has escaped, by awaits: ", functions.size());
    for (size_t index = 0; index < std::min(functions.size(), numberOfFunctionsPrinted); ++index) {
        const Function& function = program.functions[functions[index].second];
        auto textOf = [&](StringID text) {
            return text ? String::fromUTF8(program.string(text)) : String();
        };
        out.println("  ", functions[index].first, " f", functions[index].second, " ", textOf(function.name), " ", function.module ? textOf(program.at(function.module).displayPath) : String(), ":", function.position);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

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

Vector<uint32_t> ValueFlowInstancedFunctions::findParametersThatAreKeys(const Program& program, const ParallelLoops& loops, Vector<TextPassed>& textsPassed, Vector<ValueFlowNotedOperations>& notedOperations)
{
    notedOperations.fill(0, program.functions.size());
    struct Argument {
        uint32_t index;
        uint32_t positionOfParameter;
    };
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
        Vector<TextPassed> textsPassed;
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
        notedOperations[index] = notedOperationsOf(function);
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
                if (value.opcode != Opcode::Call && value.opcode != Opcode::Construct)
                    continue;
                for (uint32_t operand = 2; operand < value.numberOfOperands() && operand < 2 + mostArgumentsLookedAtForTexts; ++operand) {
                    if (!value.operand(operand))
                        continue;
                    const Value& argument = function.at(definitionThroughSameValues(function, value.operand(operand)));
                    if (argument.opcode == Opcode::String)
                        body.textsPassed.append({ operand - 2, argument.string() });
                }
                if (!hasInstances)
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
    for (size_t chunk = 0; chunk < bodies.size(); ++chunk)
        textsPassed.appendVector(std::exchange(bodies[chunk].textsPassed, { }));

    constexpr uint32_t noList = UINT32_MAX;
    Vector<uint32_t> listOfName;
    listOfName.fill(noList, program.types.numberOfPropertyKeys());
    Vector<Vector<uint32_t, 2>> functionsUnderName;
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
    auto listsHolding = [&](uint32_t function) {
        return listsHoldingFunctions.span().subspan(endOfListsHolding[function], endOfListsHolding[function + 1] - endOfListsHolding[function]);
    };
    BitVector liesUnderNameItCalls;
    size_t numberOfCalls = 0;
    for (const Found& body : bodies.span()) {
        for (const CallPassingParameters& call : body.calls) {
            size_t number = numberOfCalls++;
            if (!call.callee && std::ranges::contains(listsHolding(call.caller), listOfName[call.nameOfCallee.index()]))
                liesUnderNameItCalls.set(number);
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

static void makeKeysOfTextsThatCallsPass(Program& program, std::span<const uint32_t> positionOfKey, std::span<const ValueFlowInstancedFunctions::TextPassed> textsPassed)
{
    static_assert(ValueFlowInstancedFunctions::mostArgumentsLookedAtForTexts == 64);
    program.types.didInternKeysThatOperationsSpell();
    uint64_t positionsOfKeys = 0;
    for (uint32_t position : positionOfKey) {
        if (position < 64)
            positionsOfKeys |= 1ull << position;
    }
    if (!positionsOfKeys)
        return;
    for (const ValueFlowInstancedFunctions::TextPassed& passed : textsPassed) {
        if (positionsOfKeys >> passed.positionOfArgument & 1)
            program.types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = passed.text });
    }
}

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
            found.append(field.declared.written());
    }));
    for (const Variable& variable : program.variables)
        visit(variable.declared.written());
    visitAll(collectInParallel<TypeID>(loops, program.functions.size(), [&](unsigned functionIndex, Vector<TypeID>& found) {
        const Function& function = program.functions[functionIndex];
        for (uint32_t index = 0; index < function.numberOfEntryParameters(); ++index) {
            if (const DeclaredType* declared = function.declaredEntryParameter(index))
                found.append(declared->written());
        }
        for (const DeclaredType& result : function.results)
            found.append(result.written());
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

void forEachEmbeddedFilePath(const Program& program, NOESCAPE const ScopedLambda<void(ModuleID, StringID)>& functor)
{
    AOT_THRESHOLD(size_t, longestPath, 4096);
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::Data || module.exports.size() != 1 || !module.body || !program.at(module.body).hasBody())
            continue;
        const Function& body = program.at(module.body);
        for (const Value& value : body.values) {
            if (value.opcode != Opcode::PutModuleVariable || value.variable() != module.exports[0].variable)
                continue;
            const Value& stored = body.at(value.valueOperand());
            if (stored.opcode == Opcode::String && program.string(stored.string()).size() <= longestPath)
                functor(ModuleID(index), stored.string());
        }
    }
}

static void internEmbeddedFilePaths(Program& program)
{
    forEachEmbeddedFilePath(program, [&](ModuleID, StringID path) {
        program.types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = path });
    });
}

static ValueFlowBodies reduceBodiesBehindKeys(Program& program, BuildContext& build, Vector<uint32_t>& positionOfKey)
{
    PhaseTimes times(build, PassID::AnalyzeFlow);
    Vector<ValueFlowInstancedFunctions::TextPassed> textsPassed;
    Vector<ValueFlowNotedOperations> notedOperations;
    positionOfKey = ValueFlowInstancedFunctions::findParametersThatAreKeys(program, build.parallelLoops(), textsPassed, notedOperations);
    makeKeysOfTextsThatCallsPass(program, positionOfKey.span(), textsPassed.span());
    internKeysOfDeclaredStringLiterals(program, build.parallelLoops());
    internEmbeddedFilePaths(program);
    times.didFinish("solve: keys"_s);
    ValueFlowBodies bodies = reduceBodies(program, build, WTF::move(notedOperations));
    times.didFinish("solve: bodies"_s);
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
        return positionOfKey[index] != noKey && nodes.endOfNodesOf(function) - nodes.firstNodeOf(function) <= mostNodesOfBody;
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

ValueFlowSolution::ValueFlowSolution(Program& program, BuildContext& build, ValueFlowSolver::Rounds rounds, Witnesses witnesses, uint32_t mostRounds, EnumSet<ValueFlowUnknownSource> unknownSourcesLeftOut, WaysOfValues waysOfValues, CoveredMembers coveredMembers)
    : m_program(program)
    , m_bodies(reduceBodiesBehindKeys(program, build, m_positionOfKey))
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
        .remembersWhoLoggedEachEdge = witnesses == Witnesses::ForEveryEdge || hasFlag(build.options().reports, ReportSet::Flow),
        .isAskedHowValuesCame = waysOfValues == WaysOfValues::AreAskedFor,
        .leavesOutWhatIsCovered = coveredMembers == CoveredMembers::AreLeftOut,
        .mostRounds = mostRounds,
        .unknownSourcesLeftOut = unknownSourcesLeftOut,
        .refusedCode = { .evaluatedText = build.options().excludesEvaluatedText(), .codeOutsideImage = build.options().excludesCodeOutsideImage() },
        .removedFeatures = build.options().removedFeatures,
    };
    PhaseTimes times(build, PassID::AnalyzeFlow);
    m_instancedFunctions = makeUnique<ValueFlowInstancedFunctions>(program, m_bodies, m_positionOfKey.span(), m_maker, build.parallelLoops());
    if (!m_instancedFunctions->isEmpty())
        m_input.instancedFunctions = m_instancedFunctions.get();
    times.didFinish("solve: instanced functions"_s);
    m_solver = makeUnique<ValueFlowSolver>(m_input, m_bodies.sets(), runValueFlowRule, build.parallelLoops(), rounds);
    times.didFinish("solve: the solver made"_s);
    m_hasResult = m_solver->solve();
    times.didFinish("solve: rounds"_s);
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

ValueSetID ValueFlowSolution::possibleValuesOf(const ValueFlowOperand& operand)
{
    ValueSetID value = operand.constant ? operand.constant : ValueSets::nothing;
    if (operand.node)
        value = m_maker.unite(value, m_solver->possibleValuesOf(operand.node));
    return ValueFlowRuleContext::throughMaskOf(operand, value, *m_solver, m_maker);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

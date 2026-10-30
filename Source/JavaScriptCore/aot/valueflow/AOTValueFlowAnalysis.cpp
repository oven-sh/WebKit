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
#include "AOTSort.h"
#include "AOTValueFlowResultsUnseenCode.h"
#include "AOTValueFlowRules.h"
#include "AOTValueFlowWidenedSolve.h"
#include <wtf/IterationStatus.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Threading.h>

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

Vector<uint32_t> ValueFlowInstancedFunctions::findParametersThatAreKeys(const Program& program, const ParallelLoops& loops, Vector<TextPassed>& textsPassed, Vector<StringID>& relativePaths, Vector<ValueFlowNotedOperations>& notedOperations)
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
        Vector<StringID> relativePaths;
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
        bool isOfProgram = isWrittenInProgram(function);
        auto note = [&](unsigned position) {
            positionOfKey[index] = std::min<uint32_t>(positionOfKey[index], position);
        };
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                switch (value.opcode) {
                case Opcode::String:
                    if (std::span<const uint8_t> text = program.string(value.string()); isOfProgram && (spanHasPrefix(text, byteCast<uint8_t>("./"_span)) || spanHasPrefix(text, byteCast<uint8_t>("../"_span))))
                        body.relativePaths.append(value.string());
                    break;
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
    for (size_t chunk = 0; chunk < bodies.size(); ++chunk) {
        textsPassed.appendVector(std::exchange(bodies[chunk].textsPassed, { }));
        relativePaths.appendVector(std::exchange(bodies[chunk].relativePaths, { }));
    }

    // `a.to = b.from` stores again under `to` what lies under `from`. The lists of functions under names are not closed over that: n names, each stored into the next, would hold n * n / 2 members.
    // Names that are stored into each other in a circle form a group. A group keeps what is stored under its names directly and knows the groups that are stored into it.
    constexpr uint32_t noList = UINT32_MAX;
    Vector<uint32_t> listOfName;
    listOfName.fill(noList, program.types.numberOfPropertyKeys());
    uint32_t numberOfLists = 0;
    auto ensureListOf = [&](uint32_t name) {
        uint32_t& list = listOfName[name];
        if (list == noList)
            list = numberOfLists++;
        return list;
    };
    using Pairs = Vector<std::pair<uint32_t, uint32_t>>;
    struct Rows {
        std::span<const uint32_t> of(uint32_t row) const LIFETIME_BOUND { return entries.span().subspan(first[row], first[row + 1] - first[row]); }

        Vector<uint32_t> first;
        Vector<uint32_t> entries;
    };
    auto layOutRows = [](uint32_t numberOfRows, Pairs&& pairs) {
        AOT::sortInLine(pairs);
        removeRepeatedElements(pairs);
        Rows rows;
        rows.first.fill(0, numberOfRows + 1);
        rows.entries.reserveInitialCapacity(pairs.size());
        for (auto [row, entry] : pairs) {
            ++rows.first[row + 1];
            rows.entries.append(entry);
        }
        for (uint32_t row = 0; row < numberOfRows; ++row)
            rows.first[row + 1] += rows.first[row];
        return rows;
    };

    Pairs functionsOfLists;
    Pairs listsStoredAgain;
    for (const Found& body : bodies.span()) {
        for (auto [name, function] : body.functionsStoredUnderNames)
            functionsOfLists.append({ ensureListOf(name), function });
    }
    for (const Found& body : bodies.span()) {
        for (auto [nameFrom, nameTo] : body.namesStoredAgain) {
            uint32_t from = ensureListOf(nameFrom);
            uint32_t to = ensureListOf(nameTo);
            if (from != to)
                listsStoredAgain.append({ from, to });
        }
    }

    // A group is numbered behind every group that it is stored into.
    Vector<uint32_t> groupOfList;
    groupOfList.fill(noList, numberOfLists);
    uint32_t numberOfGroups = 0;
    {
        Rows storedInto = layOutRows(numberOfLists, Pairs(listsStoredAgain));
        struct Visit {
            uint32_t list;
            uint32_t next;
        };
        Vector<Visit> visits;
        Vector<uint32_t> open;
        Vector<uint32_t> order;
        Vector<uint32_t> lowest;
        order.fill(0, numberOfLists);
        lowest.fill(0, numberOfLists);
        BitVector isOpen(numberOfLists);
        uint32_t nextInOrder = 1;
        auto enter = [&](uint32_t list) {
            order[list] = lowest[list] = nextInOrder++;
            open.append(list);
            isOpen.quickSet(list);
            visits.append({ list, storedInto.first[list] });
        };
        for (uint32_t root = 0; root < numberOfLists; ++root) {
            if (order[root])
                continue;
            enter(root);
            while (!visits.isEmpty()) {
                uint32_t list = visits.last().list;
                if (visits.last().next < storedInto.first[list + 1]) {
                    uint32_t other = storedInto.entries[visits.last().next++];
                    if (!order[other])
                        enter(other);
                    else if (isOpen.quickGet(other))
                        lowest[list] = std::min(lowest[list], order[other]);
                    continue;
                }
                visits.removeLast();
                if (!visits.isEmpty())
                    lowest[visits.last().list] = std::min(lowest[visits.last().list], lowest[list]);
                if (lowest[list] != order[list])
                    continue;
                for (;;) {
                    uint32_t member = open.takeLast();
                    isOpen.quickClear(member);
                    groupOfList[member] = numberOfGroups;
                    if (member == list)
                        break;
                }
                ++numberOfGroups;
            }
        }
    }

    BitVector mayMove(numberOfFunctions);
    for (const Found& body : bodies.span()) {
        for (const CallPassingParameters& call : body.calls)
            mayMove.quickSet(call.caller);
    }
    Pairs groupsOfFunctionsThatMayMove;
    for (auto& [list, function] : functionsOfLists) {
        list = groupOfList[list];
        if (mayMove.quickGet(function))
            groupsOfFunctionsThatMayMove.append({ function, list });
    }
    Pairs sourcesOfGroups;
    Pairs targetsOfGroups;
    for (auto [from, to] : listsStoredAgain) {
        if (groupOfList[from] == groupOfList[to])
            continue;
        sourcesOfGroups.append({ groupOfList[to], groupOfList[from] });
        targetsOfGroups.append({ groupOfList[from], groupOfList[to] });
    }
    listsStoredAgain = { };
    Rows functionsOf = layOutRows(numberOfGroups, WTF::move(functionsOfLists));
    Rows groupsHolding = layOutRows(numberOfFunctions, WTF::move(groupsOfFunctionsThatMayMove));
    Rows sourcesOf = layOutRows(numberOfGroups, WTF::move(sourcesOfGroups));
    Rows targetsOf = layOutRows(numberOfGroups, WTF::move(targetsOfGroups));

    // At which positions the functions under a name take their keys: one bit a position, and one for every position further on.
    constexpr uint32_t positionsCounted = 8;
    using Positions = uint16_t;
    constexpr Positions takesKeyFurtherOn = 1 << positionsCounted;
    auto slotOf = [](uint32_t position) {
        return position < positionsCounted ? position : positionsCounted;
    };
    Vector<std::array<uint32_t, positionsCounted + 1>> functionsTakingKeyAt(numberOfGroups);
    for (uint32_t group = 0; group < numberOfGroups; ++group) {
        functionsTakingKeyAt[group].fill(0);
        for (uint32_t function : functionsOf.of(group)) {
            if (positionOfKey[function] != noKey)
                ++functionsTakingKeyAt[group][slotOf(positionOfKey[function])];
        }
    }
    Vector<Positions> positionsTakenUnder;
    positionsTakenUnder.fill(0, numberOfGroups);
    auto computePositionsTakenUnder = [&](uint32_t group) {
        Positions positions = 0;
        for (uint32_t slot = 0; slot <= positionsCounted; ++slot)
            positions |= static_cast<Positions>(!!functionsTakingKeyAt[group][slot]) << slot;
        for (uint32_t source : sourcesOf.of(group))
            positions |= positionsTakenUnder[source];
        return positions;
    };
    for (uint32_t group = numberOfGroups; group--;)
        positionsTakenUnder[group] = computePositionsTakenUnder(group);
    Vector<uint32_t> groupsToRefresh;
    auto didMove = [&](uint32_t function, uint32_t positionBefore, uint32_t position) {
        for (uint32_t holder : groupsHolding.of(function)) {
            if (positionBefore != noKey)
                --functionsTakingKeyAt[holder][slotOf(positionBefore)];
            if (position != noKey)
                ++functionsTakingKeyAt[holder][slotOf(position)];
            groupsToRefresh.append(holder);
        }
        while (!groupsToRefresh.isEmpty()) {
            uint32_t group = groupsToRefresh.takeLast();
            Positions positions = computePositionsTakenUnder(group);
            if (positions == positionsTakenUnder[group])
                continue;
            positionsTakenUnder[group] = positions;
            groupsToRefresh.append(targetsOf.of(group));
        }
    };

    Vector<uint32_t> walkThatVisited;
    walkThatVisited.fill(0, numberOfGroups);
    uint32_t numberOfWalk = 0;
    Vector<uint32_t> groupsToVisit;
    // Visits the group and every group that reaches it, until the functor says that it has seen enough.
    auto forEachGroupThatReaches = [&](uint32_t group, NOESCAPE const Invocable<IterationStatus(uint32_t)> auto& functor) {
        ++numberOfWalk;
        groupsToVisit.shrink(0);
        groupsToVisit.append(group);
        walkThatVisited[group] = numberOfWalk;
        while (!groupsToVisit.isEmpty()) {
            uint32_t visited = groupsToVisit.takeLast();
            if (functor(visited) == IterationStatus::Done)
                return;
            for (uint32_t source : sourcesOf.of(visited)) {
                if (std::exchange(walkThatVisited[source], numberOfWalk) != numberOfWalk)
                    groupsToVisit.append(source);
            }
        }
    };
    size_t numberOfCalls = 0;
    for (const Found& body : bodies.span())
        numberOfCalls += body.calls.size();
    enum class LiesUnderNameItCalls : uint8_t { IsNotAsked, No, Yes };
    Vector<LiesUnderNameItCalls> liesUnderNameItCalls;
    liesUnderNameItCalls.fill(LiesUnderNameItCalls::IsNotAsked, numberOfCalls);
    auto callerLiesUnder = [&](size_t numberOfCall, uint32_t caller, uint32_t group) {
        LiesUnderNameItCalls& answer = liesUnderNameItCalls[numberOfCall];
        if (answer == LiesUnderNameItCalls::IsNotAsked) {
            answer = LiesUnderNameItCalls::No;
            std::span<const uint32_t> holders = groupsHolding.of(caller);
            if (!holders.empty()) {
                forEachGroupThatReaches(group, [&](uint32_t visited) {
                    if (!std::ranges::binary_search(holders, visited))
                        return IterationStatus::Continue;
                    answer = LiesUnderNameItCalls::Yes;
                    return IterationStatus::Done;
                });
            }
        }
        return answer == LiesUnderNameItCalls::Yes;
    };

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
                // A caller that is its own callee takes part with the position it has when its turn comes: the one it had, or one that this call has just given it.
                auto mayHandOnToItself = [&] {
                    return std::ranges::any_of(arguments, [&](const Argument& argument) {
                        return argument.index == positionBefore || std::ranges::contains(arguments, argument.index, &Argument::positionOfParameter);
                    });
                };
                if (call.callee)
                    handOnTo(call.callee.index());
                else if (uint32_t list = listOfName[call.nameOfCallee.index()]; list != noList) {
                    uint32_t group = groupOfList[list];
                    Positions taken = positionsTakenUnder[group];
                    if ((taken & takesKeyFurtherOn) || (mayHandOnToItself() && callerLiesUnder(number, index, group))) {
                        forEachGroupThatReaches(group, [&](uint32_t visited) {
                            for (uint32_t callee : functionsOf.of(visited))
                                handOnTo(callee);
                            return IterationStatus::Continue;
                        });
                    } else {
                        for (const Argument& argument : arguments) {
                            if (argument.index < positionsCounted && (taken >> argument.index & 1) && argument.positionOfParameter < positionOfKey[index]) {
                                positionOfKey[index] = argument.positionOfParameter;
                                hasMoved = true;
                            }
                        }
                    }
                }
                if (positionOfKey[index] != positionBefore)
                    didMove(index, positionBefore, positionOfKey[index]);
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
            for (TypeID contents : part.writtenContents)
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

// `for (const name of new Set(["os", "./plugin.ts"])) await import(name)`: a text is followed to a load only as a key.
static void internNamesOfModules(Program& program, std::span<const StringID> relativePaths)
{
    auto intern = [&](StringID text) {
        program.types.intern(PropertyKey { .kind = PropertyKeyKind::Text, .text = text });
    };
    std::ranges::for_each(relativePaths, intern);
    for (const Module& module : program.modules) {
        if (!module.runtimeModule || !isAskedOfHostByName(module))
            continue;
        intern(module.path);
        std::ranges::for_each(module.otherNames, intern);
        std::span<const uint8_t> path = program.string(module.path);
        if (StringID name = spanHasPrefix(path, byteCast<uint8_t>("node:"_span)) ? program.strings.find(path.subspan(5)) : StringID())
            intern(name);
    }
}

static ValueFlowBodies reduceBodiesBehindKeys(Program& program, BuildContext& build, Vector<uint32_t>& positionOfKey)
{
    PhaseTimes times(build, PassID::AnalyzeFlow);
    Vector<ValueFlowInstancedFunctions::TextPassed> textsPassed;
    Vector<StringID> relativePaths;
    Vector<ValueFlowNotedOperations> notedOperations;
    positionOfKey = ValueFlowInstancedFunctions::findParametersThatAreKeys(program, build.parallelLoops(), textsPassed, relativePaths, notedOperations);
    makeKeysOfTextsThatCallsPass(program, positionOfKey.span(), textsPassed.span());
    internKeysOfDeclaredStringLiterals(program, build.parallelLoops());
    internEmbeddedFilePaths(program);
    internNamesOfModules(program, relativePaths.span());
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
        .mostObjectsListedByMember = build.limit(LimitID::MaxValueFlowObjectsListedByMember),
        .remembersWhoLoggedEachEdge = witnesses == Witnesses::ForEveryEdge || hasFlag(build.options().reports, ReportSet::Flow),
        .isAskedHowValuesCame = waysOfValues == WaysOfValues::AreAskedFor,
        .leavesOutWhatIsCovered = coveredMembers == CoveredMembers::AreLeftOut,
        .mostRounds = mostRounds,
        .unknownSourcesLeftOut = unknownSourcesLeftOut,
        .refusedCode = { .evaluatedText = build.options().excludesEvaluatedText(), .hostEvaluatedText = build.options().excludesHostEvaluatedText(), .codeOutsideImage = build.options().excludesCodeOutsideImage() },
        .removedFeatures = build.options().removedFeatures,
    };
    PhaseTimes times(build, PassID::AnalyzeFlow);
    m_instancedFunctions = makeUnique<ValueFlowInstancedFunctions>(program, m_bodies, m_positionOfKey.span(), m_maker, build.parallelLoops());
    if (!m_instancedFunctions->isEmpty())
        m_input.instancedFunctions = m_instancedFunctions.get();
    times.didFinish("solve: instanced functions"_s);
    Vector<UnseenCodeRecord> unseenCodeOfProgramAsItIs;
    if (build.options().widensFlow) [[unlikely]] {
        m_inputAsItIs = m_input;
        m_inputAsItIs.remembersWhoLoggedEachEdge = true;
        m_inputAsItIs.isAskedHowValuesCame = true;
        m_inputAsItIs.leavesOutWhatIsCovered = false;
        m_solver = makeUnique<ValueFlowSolver>(m_inputAsItIs, m_bodies.sets(), runValueFlowRule, build.parallelLoops(), rounds);
        m_solver->solve();
        unseenCodeOfProgramAsItIs = unseenCodeRecordsOf(*this);
        m_solverOfProgramAsItIs = WTF::move(m_solver);
        m_input.seedOfWidening = build.recordedOptions().layoutSeed;
        times.didFinish("solve: the program as it is"_s);
    }
    m_solver = makeUnique<ValueFlowSolver>(m_input, m_bodies.sets(), runValueFlowRule, build.parallelLoops(), rounds);
    times.didFinish("solve: the solver made"_s);
    m_solver->solve();
    times.didFinish("solve: rounds"_s);
    if (build.options().logging.logsPhaseTimes) {
        ValueFlowBytesByTable bytes;
        m_bodies.addBytesTo(bytes);
        bytes.dump("all of the bodies"_s);
    }
    for (const std::unique_ptr<ValueSetMaker>& maker : m_solver->makersIdleAfterSolve())
        m_makerPool.giveBack(*maker);
    if (m_solverOfProgramAsItIs) [[unlikely]] {
        reportWhatIsFoundOnlyFromLess(*this, unseenCodeOfProgramAsItIs.span(), build);
        times.didFinish("solve: the two solves compared"_s);
    }
}

ValueFlowSolution::~ValueFlowSolution() = default;

auto ValueSetMakerPool::makerOfCurrentThread() -> MakerOfThread&
{
    return m_makersOfThreads[Thread::currentSingleton().uid() % numberOfThreadsToldApart];
}

ValueSetMaker& ValueSetMakerPool::take()
{
    if (ValueSetMaker* maker = makerOfCurrentThread().idleMaker.exchange(nullptr, std::memory_order_acquire))
        return *maker;
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
    ValueSetMaker* nobody = nullptr;
    if (makerOfCurrentThread().idleMaker.compare_exchange_strong(nobody, &maker, std::memory_order_release, std::memory_order_relaxed))
        return;
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

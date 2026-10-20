/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableWords.h"
#include "AOTCFG.h"
#include "AOTFlowAnalysisInternal.h"
#include "AOTFlowNames.h"
#include "AOTFlowPropertyState.h"
#include "AOTIRPrinter.h"
#include "AOTSupportTestCommands.h"
#include <algorithm>
#include <array>
#include <climits>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <wtf/CommaPrinter.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

template<typename State>
struct WordOperation {
    ASCIILiteral name;
    unsigned minimumOperands;
    unsigned maximumOperands;
    bool (*run)(State&, PrintStream& answer, std::span<const String> operands);
};

using ObjectSets = std::array<FlowObjectSet, 3>;

struct SetOperator {
    ASCIILiteral spelling;
    void (*run)(PrintStream&, FlowObjectSet& target, const FlowObjectSet& operand);
};

struct MemberOperator {
    char spelling;
    void (*run)(PrintStream&, FlowObjectSet& target, FlowObjectID member);
};

struct ValueCommandState {
    PrintStream& out;
    const Program& program;
    FlowValueTable& values;
    Vector<TypeID> parameterTypes;
};

struct KeySetCommandState {
    PrintStream& out;
    const Program& program;
    Vector<PropertyKeyID> keys;
};

struct SolveSection {
    ASCIILiteral name;
    void (*print)(PrintStream&, const FlowSolvedProgram&);
};

struct QueryCommandState {
    PrintStream& out;
    Program& program;
    const FlowResults& results;
};

} // anonymous namespace

static bool rejectWord(PrintStream& out, const String& word)
{
    printBadArgument(out, word);
    return false;
}

static std::optional<uint32_t> parseNumberedWord(StringView word, char prefix, size_t count)
{
    if (word.length() < 2 || word[0] != prefix)
        return std::nullopt;
    std::optional<uint32_t> index = parseTestNumber<uint32_t>(word.substring(1));
    return index && *index < count ? index : std::nullopt;
}

static std::optional<FunctionID> parseFunctionWord(const Program& program, StringView word)
{
    std::optional<uint32_t> index = parseNumberedWord(word, 'f', program.functions.size());
    if (!index)
        return std::nullopt;
    return FunctionID(*index);
}

template<typename State>
static const WordOperation<State>* findOperation(std::span<const WordOperation<State>> operations, const String& word)
{
    auto found = std::ranges::find_if(operations, [&](const WordOperation<State>& operation) {
        return word == operation.name;
    });
    return found == operations.end() ? nullptr : &*found;
}

template<typename State>
static void runWordOperations(State& state, std::span<const String> words, std::type_identity_t<std::span<const WordOperation<State>>> operations)
{
    if (words.empty())
        return printMissingArgument(state.out);

    while (!words.empty()) {
        const WordOperation<State>* operation = findOperation<State>(operations, words[0]);
        if (!operation)
            return printBadArgument(state.out, words[0]);

        size_t length = 1;
        while (length < words.size() && length <= operation->maximumOperands && !findOperation<State>(operations, words[length]))
            ++length;
        if (length <= operation->minimumOperands)
            return printMissingArgument(state.out);

        StringPrintStream answer;
        if (!operation->run(state, answer, words.subspan(1, length - 1)))
            return;

        CommaPrinter blank(" "_s);
        for (const String& word : words.first(length))
            state.out.print(blank, word);
        state.out.println(": "_s, answer.toUTF8CString());
        words = words.subspan(length);
    }
}

static constexpr SetOperator setOperators[] = {
    { "|="_s, [](PrintStream& out, FlowObjectSet& target, const FlowObjectSet& operand) { out.print(target.unite(operand) ? "changed"_s : "unchanged"_s); } },
    { "&="_s, [](PrintStream& out, FlowObjectSet& target, const FlowObjectSet& operand) {
        target.intersect(operand);
        out.print(target);
    } },
    { "-="_s, [](PrintStream& out, FlowObjectSet& target, const FlowObjectSet& operand) {
        target.subtract(operand);
        out.print(target);
    } },
    { "<="_s, [](PrintStream& out, FlowObjectSet& target, const FlowObjectSet& operand) { out.print(yesOrNo(target.isSubsetOf(operand))); } },
    { "=="_s, [](PrintStream& out, FlowObjectSet& target, const FlowObjectSet& operand) { out.print(yesOrNo(target == operand)); } },
    { "^"_s, [](PrintStream& out, FlowObjectSet& target, const FlowObjectSet& operand) { out.print(yesOrNo(target.overlaps(operand))); } },
    { "="_s, [](PrintStream& out, FlowObjectSet& target, const FlowObjectSet& operand) {
        target = operand;
        out.print(target);
    } },
};

static constexpr MemberOperator memberOperators[] = {
    { '+', [](PrintStream& out, FlowObjectSet& target, FlowObjectID member) { out.print(target.add(member) ? "added"_s : "present"_s); } },
    { '-', [](PrintStream& out, FlowObjectSet& target, FlowObjectID member) { out.print(target.remove(member) ? "removed"_s : "absent"_s); } },
    { '?', [](PrintStream& out, FlowObjectSet& target, FlowObjectID member) { out.print(yesOrNo(target.contains(member))); } },
};

static void printObjectSet(PrintStream& out, const FlowObjectSet& set)
{
    out.print(set, " size="_s, set.size());
    if (FlowObjectID only = set.onlyObject()) {
        if (only == unknownFlowObject)
            out.print(" only=unknown"_s);
        else
            out.print(" only=o"_s, only.index());
    }
    out.print(set.memoryInBytes() ? " out-of-line"_s : " inline"_s);
}

static FlowObjectSet* findObjectSet(ObjectSets& sets, StringView name)
{
    static constexpr ASCIILiteral names = "abc"_s;
    static_assert(names.length() == std::tuple_size_v<ObjectSets>);
    if (name.length() != 1)
        return nullptr;
    size_t index = StringView(names).find(name[0]);
    return index == notFound ? nullptr : &sets[index];
}

static bool runCollectingUnion(PrintStream& out, ObjectSets& sets, FlowObjectSet& target, StringView operation)
{
    constexpr ASCIILiteral spelling = "|="_s;
    size_t separator = operation.find('>');
    if (!operation.startsWith(spelling) || separator == notFound)
        return false;

    FlowObjectSet* source = findObjectSet(sets, operation.substring(spelling.length(), separator - spelling.length()));
    FlowObjectSet* added = findObjectSet(sets, operation.substring(separator + 1));
    if (!source || !added || added == &target || added == source)
        return false;

    out.print(target.uniteAndCollect(*source, *added) ? "changed"_s : "unchanged"_s);
    return true;
}

static bool runSetOperator(PrintStream& out, ObjectSets& sets, FlowObjectSet& target, StringView operation)
{
    auto found = std::ranges::find_if(setOperators, [&](const SetOperator& candidate) {
        return operation.startsWith(candidate.spelling);
    });
    if (found == std::end(setOperators))
        return false;
    FlowObjectSet* operand = findObjectSet(sets, operation.substring(found->spelling.length()));
    if (!operand)
        return false;
    found->run(out, target, *operand);
    return true;
}

static bool runMemberOperator(PrintStream& out, FlowObjectSet& target, StringView operation)
{
    auto found = std::ranges::find_if(memberOperators, [&](const MemberOperator& candidate) {
        return operation[0] == candidate.spelling;
    });
    std::optional<uint32_t> index = parseTestNumber<uint32_t>(operation.substring(1));
    if (found == std::end(memberOperators) || !index || *index > FlowObjectSet::maxObjectIndex)
        return false;
    found->run(out, target, FlowObjectID(*index));
    return true;
}

static bool runObjectSetWord(PrintStream& out, ObjectSets& sets, StringView word)
{
    FlowObjectSet* target = findObjectSet(sets, word.left(1));
    if (!target)
        return false;
    StringView operation = word.substring(1);
    if (operation.isEmpty()) {
        printObjectSet(out, *target);
        return true;
    }
    return runCollectingUnion(out, sets, *target, operation)
        || runSetOperator(out, sets, *target, operation)
        || runMemberOperator(out, *target, operation);
}

void runFlowObjectSetTestCommand(TestCommandContext& context)
{
    if (context.arguments.empty())
        return printMissingArgument(context.out);

    ObjectSets sets;
    for (const String& word : context.arguments) {
        StringPrintStream answer;
        if (!runObjectSetWord(answer, sets, word))
            return printBadArgument(context.out, word);
        context.out.println(word, ": "_s, answer.toUTF8CString());
    }
}

static Vector<TypeID> collectEntryParameterTypes(const Program& program)
{
    Vector<TypeID> types;
    if (program.functions.isEmpty() || !program.functions[0].hasBody())
        return types;
    const Function& function = program.functions[0];
    for (ValueID parameter : function.at(entryBlock).parameters)
        types.append(function.at(parameter).type);
    return types;
}

static std::optional<Vector<TypeID, 4>> parseParameterWords(ValueCommandState& state, std::span<const String> words)
{
    Vector<TypeID, 4> types;
    for (const String& word : words) {
        std::optional<uint32_t> index = parseNumberedWord(word, 'p', state.parameterTypes.size());
        if (!index) {
            printBadArgument(state.out, word);
            return std::nullopt;
        }
        types.append(state.parameterTypes[*index]);
    }
    return types;
}

static bool joinValues(ValueCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    auto types = parseParameterWords(state, operands);
    if (!types)
        return false;
    FlowValue result;
    for (TypeID type : *types)
        state.values.join(result, state.values.valueOfType(type), Subject { .function = FunctionID(0u) });
    state.values.dump(answer, result);
    return true;
}

template<FlowValue (FlowValueTable::*filter)(const FlowValue&, const Type&)>
static bool filterValue(ValueCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    auto types = parseParameterWords(state, operands);
    if (!types)
        return false;
    state.values.dump(answer, (state.values.*filter)(state.values.valueOfType((*types)[0]), state.program.at((*types)[1])));
    return true;
}

static bool testValueSubset(ValueCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    auto types = parseParameterWords(state, operands);
    if (!types)
        return false;
    FlowValue subset = state.values.valueOfType((*types)[0]);
    FlowValue superset = state.values.valueOfType((*types)[1]);
    answer.print(yesOrNo(state.values.isSubsetOf(subset, superset)));
    return true;
}

static bool printValueKeys(ValueCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    auto types = parseParameterWords(state, operands);
    if (!types)
        return false;
    state.values.keysOf(state.values.valueOfType((*types)[0])).dump(answer, state.program);
    return true;
}

static bool printUnknownValue(ValueCommandState& state, PrintStream& answer, std::span<const String>)
{
    state.values.dump(answer, state.values.unknownValue());
    return true;
}

void runFlowValueTestCommand(TestCommandContext& context)
{
    static constexpr WordOperation<ValueCommandState> operations[] = {
        { "join"_s, 2, UINT_MAX, joinValues },
        { "filter"_s, 2, 2, filterValue<&FlowValueTable::filter> },
        { "filter-out"_s, 2, 2, filterValue<&FlowValueTable::filterOut> },
        { "subset"_s, 2, 2, testValueSubset },
        { "keys"_s, 1, 1, printValueKeys },
        { "unknown"_s, 0, 0, printUnknownValue },
    };

    Program& program = *context.program;
    FlowObjectTable objects(program);
    TypeArena types(program.types);
    FlowValueTable values(program, objects, types, context.build);

    ValueCommandState state { context.out, program, values, collectEntryParameterTypes(program) };
    runWordOperations(state, context.arguments, operations);
}

static Vector<PropertyKeyID> collectReadKeys(const Program& program)
{
    Vector<PropertyKeyID> keys;
    if (program.functions.isEmpty())
        return keys;
    const Function& function = program.functions[0];
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::PropGet)
                keys.append(value.key());
        }
    }
    return keys;
}

static std::optional<PropertyKeyID> parseKeyWord(KeySetCommandState& state, const String& word)
{
    std::optional<uint32_t> index = parseNumberedWord(word, 'k', state.keys.size());
    if (!index) {
        printBadArgument(state.out, word);
        return std::nullopt;
    }
    return state.keys[*index];
}

static std::optional<FlowKeySet> parseKeySetWord(StringView word)
{
    if (word == "number*"_s)
        return FlowKeySet::everyNumber();
    if (word == "string*"_s)
        return FlowKeySet::everyString();
    if (word == "symbol*"_s) {
        FlowKeySet keys;
        keys.anySymbol = true;
        return keys;
    }
    if (word == "all"_s)
        return FlowKeySet::everyKey();

    if (!word.startsWith('[') || !word.endsWith(']'))
        return std::nullopt;
    StringView bounds = word.substring(1, word.length() - 2);
    size_t comma = bounds.find(',');
    if (comma == notFound)
        return std::nullopt;
    std::optional<int64_t> minimum = parseTestNumber<int64_t>(bounds.left(comma));
    std::optional<int64_t> maximum = parseTestNumber<int64_t>(bounds.substring(comma + 1));
    if (!minimum || !maximum)
        return std::nullopt;
    return FlowKeySet::index(Range { *minimum, *maximum });
}

static bool printKeyClass(KeySetCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<PropertyKeyID> key = parseKeyWord(state, operands[0]);
    if (!key)
        return false;
    answer.print(nameOf(classOf(state.program, *key)));
    return true;
}

static bool printKeyIndex(KeySetCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<PropertyKeyID> key = parseKeyWord(state, operands[0]);
    if (!key)
        return false;
    if (std::optional<uint32_t> index = arrayIndexOf(state.program, *key))
        answer.print(*index);
    else
        answer.print("none"_s);
    return true;
}

static bool printKeySet(KeySetCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<PropertyKeyID> key = parseKeyWord(state, operands[0]);
    if (!key)
        return false;
    keySetOf(state.program, *key).dump(answer, state.program);
    return true;
}

static bool uniteKeySets(KeySetCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<PropertyKeyID> first = parseKeyWord(state, operands[0]);
    if (!first)
        return false;
    std::optional<PropertyKeyID> second = parseKeyWord(state, operands[1]);
    if (!second)
        return false;
    FlowKeySet keys = keySetOf(state.program, *first);
    keys.unite(keySetOf(state.program, *second));
    keys.dump(answer, state.program);
    return true;
}

static bool testKeyName(KeySetCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<FlowKeySet> keys = parseKeySetWord(operands[0]);
    if (!keys)
        return rejectWord(state.out, operands[0]);
    std::optional<PropertyKeyID> key = parseKeyWord(state, operands[1]);
    if (!key)
        return false;
    answer.print(yesOrNo(keys->mayName(*key, state.program)));
    return true;
}

void runFlowKeySetTestCommand(TestCommandContext& context)
{
    static constexpr WordOperation<KeySetCommandState> operations[] = {
        { "class"_s, 1, 1, printKeyClass },
        { "index"_s, 1, 1, printKeyIndex },
        { "set"_s, 1, 1, printKeySet },
        { "unite"_s, 2, 2, uniteKeySets },
        { "may-name"_s, 2, 2, testKeyName },
    };

    KeySetCommandState state { context.out, *context.program, collectReadKeys(*context.program) };
    runWordOperations(state, context.arguments, operations);
}

static void printFunctionID(PrintStream& out, const FlowGraph& graph, FunctionID function)
{
    if (graph.isHost(function)) {
        out.print("host"_s);
        return;
    }
    out.print("f"_s, function.index());
}

static void printNodes(PrintStream& out, const FlowSolvedProgram& solved)
{
    solved.graph->dump(out);
}

static void printObjects(PrintStream& out, const FlowSolvedProgram& solved)
{
    for (uint32_t index = 0; index < solved.objects->size(); ++index) {
        FlowObjectID object(index);
        out.print("o"_s, index, " "_s);
        solved.objects->dump(out, object);
        out.print(" mutations="_s);
        dumpFlags(out, solved.records.possibleMutations(object));
        out.print(" uses="_s);
        dumpFlags(out, solved.records.usesOf(object));
        out.print(" events="_s);
        dumpFlags(out, solved.records.eventsOf(object));
        out.println();
    }
}

static void printLocationName(IRPrinter& printer, const FlowGraph& graph, const FlowNodeDescription& description)
{
    PrintStream& out = printer.out();
    out.print(nameOf(description.kind), "("_s);
    if (description.variable)
        printer.print(description.variable);
    if (description.object)
        graph.objects().dump(out, description.object);
    if (description.kind == FlowNodeKind::DeclaredPart || description.kind == FlowNodeKind::Position)
        out.print(", "_s, description.index);
    if (description.key) {
        out.print(", "_s);
        printer.print(description.key);
    }
    out.print(")"_s);
}

static void printLocations(PrintStream& out, const FlowSolvedProgram& solved)
{
    const FlowGraph& graph = *solved.graph;
    IRPrinter printer(out, solved.program);
    for (uint32_t index = 0; index < graph.numberOfNodes(); ++index) {
        FlowNodeID node(index);
        OptionSet<FlowLocationEvent> events = solved.records.recordOf(node).events;
        if (events.isEmpty())
            continue;
        out.print("n"_s, index, " "_s);
        printLocationName(printer, graph, graph.describe(node));
        out.print(" events="_s);
        dumpFlags(out, events);
        out.println();
    }
}

static void printSink(PrintStream& out, const FlowObjectTable& objects, ASCIILiteral name, const FlowObjectSet& sink)
{
    out.print(name, " {"_s);
    CommaPrinter comma;
    sink.forEach([&](FlowObjectID object) {
        out.print(comma);
        objects.dump(out, object);
    });
    out.println("}"_s);
}

static void printSinks(PrintStream& out, const FlowSolvedProgram& solved)
{
    FlowSolution solution = solved.solution();
    printSink(out, *solved.objects, "nativeCode"_s, solution.objectsInNativeCode());
    printSink(out, *solved.objects, "unknownJavaScript"_s, solution.objectsInUnknownJavaScript());
}

static void printEdges(PrintStream& out, const FlowSolvedProgram& solved)
{
    const FlowGraph& graph = *solved.graph;
    for (uint32_t index = 0; index < graph.numberOfNodes(); ++index) {
        graph.forEachSuccessor(FlowNodeID(index), [&](FlowNodeID successor, TypeID filter) {
            out.println("n"_s, index, " -> n"_s, successor.index(), filter ? " filtered"_s : ""_s);
        });
    }
}

static void printDirectUses(PrintStream& out, const FlowSolvedProgram& solved)
{
    for (uint32_t index = 0; index < solved.graph->numberOfNodes(); ++index) {
        OperandUses uses = solved.records.directUsesOf(FlowNodeID(index));
        if (uses.isEmpty())
            continue;
        out.print("n"_s, index, " uses="_s);
        dumpFlags(out, uses & everyFlagOf<OperandUse>());
        out.println();
    }
}

static void printCallSite(PrintStream& out, const FlowGraph& graph, const FlowCallSiteKey& callSite)
{
    out.print("at "_s);
    printFunctionID(out, graph, callSite.caller);
    if (callSite.call)
        out.print(":v"_s, callSite.call.index());
    if (callSite.outer)
        out.print(" in i"_s, callSite.outer.index());
    if (callSite.context != FlowContext::Proven)
        out.print(" "_s, nameOf(callSite.context));
}

static void printInstances(PrintStream& out, const FlowSolvedProgram& solved)
{
    const FlowGraph& graph = *solved.graph;
    for (uint32_t index = 0; index < graph.numberOfInstances(); ++index) {
        const FlowInstance& instance = graph.at(FlowInstanceID(index));
        out.print("i"_s, index, " "_s);
        printFunctionID(out, graph, instance.function);
        out.print(" "_s);
        if (instance.callSite)
            printCallSite(out, graph, *instance.callSite);
        else
            out.print(nameOf(instance.context));
        out.println(" "_s, nameOf(instance.state));
    }
}

static void printAbandonedFunctions(PrintStream& out, const FlowSolvedProgram& solved)
{
    for (size_t index : solved.records.functionsPastWorkLimit()) {
        out.print("past-work-limit "_s);
        printFunctionID(out, *solved.graph, FunctionID(index));
        out.println();
    }
}

static void printFixpointChanges(PrintStream& out, const FlowSolvedProgram& solved)
{
    if (!solved.build.options().checksFlowFixpoint) {
        out.println("error: the section needs the option check=FlowFixpoint"_s);
        return;
    }
    if (solved.fixpointChanges.isEmpty())
        out.println("nothing changes"_s);
    for (const FlowFixpointChange& change : solved.fixpointChanges)
        out.println(change.description);
}

static void printWitnessLine(PrintStream& out, char prefix, uint32_t index, ASCIILiteral subject, const Witness& witness)
{
    if (!hasFunction(witness))
        return;
    out.print(prefix, index, " "_s, subject, " f"_s, witness.function);
    if (witness.value != Wire::none)
        out.print(":v"_s, witness.value);
    out.println();
}

static void printWitnesses(PrintStream& out, const FlowSolvedProgram& solved)
{
    const FlowRecords& records = solved.records;
    for (uint32_t index = 0; index < solved.objects->size(); ++index) {
        FlowObjectID object(index);
        for (ObjectMutation mutation : records.possibleMutations(object))
            printWitnessLine(out, 'o', index, nameOf(mutation), records.witnessOf(object, mutation));
        for (FlowObjectEvent event : records.eventsOf(object))
            printWitnessLine(out, 'o', index, nameOf(event), records.witnessOf(object, event));
    }
    for (uint32_t index = 0; index < solved.graph->numberOfNodes(); ++index)
        printWitnessLine(out, 'n', index, "unknown"_s, records.unknownObjectWitness(FlowNodeID(index)));
}

static void printLookups(PrintStream& out, const FlowSolvedProgram& solved)
{
    const FlowLookups& lookups = solved.states.properties->lookups;
    out.println("lookups "_s, lookups.all.size(), ", walks "_s, lookups.numberOfWalks, ", answers taken "_s, lookups.numberOfAnswersTaken);
}

void runFlowSolveTestCommand(TestCommandContext& context)
{
    static constexpr SolveSection sections[] = {
        { "nodes"_s, printNodes },
        { "objects"_s, printObjects },
        { "locations"_s, printLocations },
        { "sinks"_s, printSinks },
        { "edges"_s, printEdges },
        { "uses"_s, printDirectUses },
        { "instances"_s, printInstances },
        { "work"_s, printAbandonedFunctions },
        { "fixpoint"_s, printFixpointChanges },
        { "witnesses"_s, printWitnesses },
        { "origins"_s, printUnknownObjectOrigins },
        { "application-origins"_s, printUnknownObjectOriginsOfApplication },
        { "mutations"_s, printObjectMutationsByWitness },
        { "access-sites"_s, printAccessSites },
        { "lookups"_s, printLookups },
    };

    std::unique_ptr<FlowSolvedProgram> solved = solveFlow(*context.program, context.program->types, context.build);
    if (context.arguments.empty())
        return printNodes(context.out, *solved);

    constexpr ASCIILiteral functionPrefix = "function:"_s;
    for (const String& word : context.arguments) {
        if (word.startsWith(functionPrefix)) {
            std::optional<FunctionID> function = parseFunctionWord(*context.program, StringView(word).substring(functionPrefix.length()));
            if (!function)
                return printBadArgument(context.out, word);
            printFunctionInSolvedProgram(context.out, *solved, *function);
            continue;
        }
        auto section = std::ranges::find_if(sections, [&](const SolveSection& candidate) {
            return word == candidate.name;
        });
        if (section == std::end(sections))
            return printBadArgument(context.out, word);
        section->print(context.out, *solved);
    }
}

template<typename Record>
static void printRecord(PrintStream& out, const Program& program, const Record& record)
{
    IRPrinter printer(out, program);
    printer.beginRecord();
    printFields(printer, record);
    printer.endRecord();
}

static void printOperandUses(PrintStream& out, OperandUses uses)
{
    constexpr OperandUses everyOperandUse = everyFlagOf<OperandUse>();
    out.print("uses="_s);
    if (uses.containsAll(everyOperandUse)) {
        out.print("all"_s);
        return;
    }
    dumpFlags(out, uses & everyOperandUse);
}

static std::optional<Vector<ParameterOrigin>> parseParameterMap(StringView word)
{
    constexpr ASCIILiteral prefix = "map=["_s;
    if (!word.startsWith(prefix) || !word.endsWith(']'))
        return std::nullopt;

    Vector<ParameterOrigin> parameterMap;
    StringView entries = word.substring(prefix.length(), word.length() - prefix.length() - 1);
    if (entries.isEmpty())
        return parameterMap;
    for (StringView entry : entries.splitAllowingEmptyEntries(',')) {
        if (entry == "-"_s) {
            parameterMap.append(ParameterOrigin { });
            continue;
        }
        std::optional<uint32_t> original = parseTestNumber<uint32_t>(entry);
        if (!original)
            return std::nullopt;
        parameterMap.append(ParameterOrigin { original });
    }
    return parameterMap;
}

static bool deriveFunction(QueryCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<FunctionID> source = parseFunctionWord(state.program, operands[0]);
    if (!source)
        return rejectWord(state.out, operands[0]);
    std::optional<Vector<ParameterOrigin>> parameterMap = parseParameterMap(operands[1]);
    if (!parameterMap)
        return rejectWord(state.out, operands[1]);

    ProgramEditor editor(state.program);
    FunctionID derived = editor.addFunctionLike(*source, Function(state.program.at(*source)));
    editor.at(derived).derivation = Derivation { .source = *source, .parameterMap = WTF::move(*parameterMap) };
    answer.print("f"_s, derived.index());
    return true;
}

static bool queryFunction(QueryCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<FunctionID> function = parseFunctionWord(state.program, operands[0]);
    if (!function)
        return rejectWord(state.out, operands[0]);
    printRecord(answer, state.program, state.results.function(*function));
    return true;
}

static bool queryParameter(QueryCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<FunctionID> function = parseFunctionWord(state.program, operands[0]);
    if (!function)
        return rejectWord(state.out, operands[0]);
    std::optional<uint32_t> parameter = parseTestNumber<uint32_t>(operands[1]);
    if (!parameter)
        return rejectWord(state.out, operands[1]);

    OperandSummary summary = state.results.parameterSummary(*function, *parameter);
    printOperandUses(answer, summary.uses);
    answer.print(" retainedIn="_s, nameOf(summary.retainedIn));
    if (summary.retainedIn == RetainedIn::Operand)
        answer.print(" retainingOperand="_s, summary.retainingOperand);
    return true;
}

static bool queryStoredValue(QueryCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    StringView word = operands[0];
    size_t dot = word.find('.');
    if (dot == notFound)
        return rejectWord(state.out, operands[0]);
    std::optional<uint32_t> layout = parseNumberedWord(word.left(dot), 'L', state.program.layouts.size());
    std::optional<uint32_t> field = parseTestNumber<uint32_t>(word.substring(dot + 1));
    if (!layout || !field)
        return rejectWord(state.out, operands[0]);
    printOperandUses(answer, state.results.storedValueUses({ LayoutID(*layout), *field }));
    return true;
}

static bool queryAllocationClass(QueryCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    std::optional<uint32_t> site = parseNumberedWord(operands[0], 'A', state.results.numberOfAllocationSites());
    if (!site)
        return rejectWord(state.out, operands[0]);
    printRecord(answer, state.program, state.results.classOf(AllocationSiteID(*site)));
    return true;
}

static bool queryBuiltinLocation(QueryCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    const BuiltinTable& builtins = state.program.builtins();
    BuiltinID holder = builtins.builtinFromToken(operands[0]);
    if (!holder)
        return rejectWord(state.out, operands[0]);
    BuiltinID member = builtins.builtinFromToken(operands[1]);
    if (!member || !builtins.at(member).name)
        return rejectWord(state.out, operands[1]);

    answer.print(nameOf(state.results.builtinLocation(member)));
    if (const BuiltinLocationFlow* location = state.results.findBuiltinLocation(holder, state.program.keyOf(builtins.at(member).name))) {
        answer.print(" "_s);
        printRecord(answer, state.program, *location);
    }
    return true;
}

static bool queryIndirectCalls(QueryCommandState& state, PrintStream& answer, std::span<const String> operands)
{
    FunctionID caller;
    if (operands[0] != "host"_s) {
        std::optional<FunctionID> function = parseFunctionWord(state.program, operands[0]);
        if (!function)
            return rejectWord(state.out, operands[0]);
        caller = *function;
    }

    CommaPrinter comma;
    answer.print("["_s);
    for (const FlowIndirectCall& call : state.results.indirectCallsFrom(caller)) {
        answer.print(comma);
        if (call.site)
            answer.print("S"_s, call.site.index(), " "_s);
        answer.print("-> f"_s, call.callee.index(), " "_s, nameOf(call.kind), " "_s, std::span(builtinCallbackTimesWords)[static_cast<size_t>(call.times)]);
        if (call.isInCycle)
            answer.print(" cycle"_s);
    }
    answer.print("]"_s);
    return true;
}

template<typename IDType>
static constexpr IDType idPastEveryTable()
{
    // An index from ProvisionalFunctions::firstIndex on names a provisional function, which only its provider can look up.
    if constexpr (std::is_same_v<IDType, FunctionID>)
        return FunctionID(ProvisionalFunctions::firstIndex - 1);
    else
        return IDType(static_cast<uint32_t>(IDType::hashTableDeletedIndex) - 1);
}

template<typename IDType>
static bool answersUnknown(NOESCAPE const Invocable<bool(IDType)> auto& isUnknown)
{
    return isUnknown(IDType()) && isUnknown(idPastEveryTable<IDType>());
}

static bool queryUnknownIDs(QueryCommandState& state, PrintStream& answer, std::span<const String>)
{
    const FlowResults& results = state.results;
    CommaPrinter blank(" "_s);
    auto print = [&](ASCIILiteral accessor, bool isUnknown) {
        answer.print(blank, accessor, "="_s, isUnknown ? "unknown"_s : "known"_s);
    };

    print("function"_s, answersUnknown<FunctionID>([&](FunctionID id) { return results.function(id) == FunctionFlow(); }));
    print("mayBeReached"_s, answersUnknown<FunctionID>([&](FunctionID id) { return results.mayBeReached(id); }));
    print("runsAtMostOnce"_s, answersUnknown<FunctionID>([&](FunctionID id) { return !results.runsAtMostOnce(id) && !results.runsAtMostOnce(state.program, id); }));
    print("parameterSummary"_s, answersUnknown<FunctionID>([&](FunctionID id) { return results.parameterSummary(id, 0) == OperandSummary(); }));
    print("indirectCallsFrom"_s, results.indirectCallsFrom(idPastEveryTable<FunctionID>()).empty());
    print("storedValueUses"_s, answersUnknown<LayoutID>([&](LayoutID id) { return results.storedValueUses({ id, 0 }) == OperandUses::all(); }));
    print("typedResultSite"_s, answersUnknown<LayoutID>([&](LayoutID id) { return !results.typedResultSite(id); }));
    print("module"_s, answersUnknown<ModuleID>([&](ModuleID id) { return results.module(id) == ModuleFlow(); }));

    print("builtinLocation"_s, answersUnknown<BuiltinID>([&](BuiltinID id) { return results.builtinLocation(id) == BuiltinLocationState::Unknown; }));
    print("findBuiltinLocation"_s, answersUnknown<BuiltinID>([&](BuiltinID holder) {
        return answersUnknown<PropertyKeyID>([&](PropertyKeyID key) { return !results.findBuiltinLocation(holder, key); });
    }));
    print("usesOf"_s, answersUnknown<BuiltinID>([&](BuiltinID id) { return results.usesOf(id).isEmpty(); }));

    print("accessSite"_s, answersUnknown<AccessSiteID>([&](AccessSiteID id) { return results.at(id) == AccessSite(); }));
    print("lossOf"_s, answersUnknown<AccessSiteID>([&](AccessSiteID id) { return results.lossOf(id) == FlowSiteLoss(); }));
    print("seesOnlyFinishedObjects"_s, answersUnknown<AccessSiteID>([&](AccessSiteID id) { return !results.seesOnlyFinishedObjects(id); }));
    print("resultSiteOf"_s, answersUnknown<AccessSiteID>([&](AccessSiteID id) { return !results.resultSiteOf(id); }));
    print("sinkCandidates"_s, answersUnknown<AccessSiteID>([&](AccessSiteID id) { return results.sinkCandidates(id, 0) == SinkCandidates(); }));
    print("typedResultType"_s, answersUnknown<AccessSiteID>([&](AccessSiteID id) { return !results.typedResultType(id); }));
    print("allocationSite"_s, answersUnknown<AllocationSiteID>([&](AllocationSiteID id) { return results.at(id) == AllocationSite(); }));
    print("classOf"_s, answersUnknown<AllocationSiteID>([&](AllocationSiteID id) { return results.classOf(id) == AllocationClass(); }));
    print("dataNodeStamp"_s, answersUnknown<AllocationSiteID>([&](AllocationSiteID id) { return results.dataNodeStamp(id, 0) == FlowDataNodeStamp(); }));
    print("allocationClass"_s, answersUnknown<AllocationClassID>([&](AllocationClassID id) { return results.at(id) == AllocationClass(); }));
    return true;
}

void runFlowQueryTestCommand(TestCommandContext& context)
{
    static constexpr WordOperation<QueryCommandState> queries[] = {
        { "derive"_s, 2, 2, deriveFunction },
        { "function"_s, 1, 1, queryFunction },
        { "parameter"_s, 2, 2, queryParameter },
        { "stored-value"_s, 1, 1, queryStoredValue },
        { "class"_s, 1, 1, queryAllocationClass },
        { "location"_s, 2, 2, queryBuiltinLocation },
        { "indirect"_s, 1, 1, queryIndirectCalls },
        { "unknown-ids"_s, 0, 0, queryUnknownIDs },
    };

    Program& program = *context.program;
    FlowResults results = analyzeFlow(program, program.types, context.build);
    applyFlowResults(program, results, context.build);
    program.setForm(IRForm::Prepared);

    QueryCommandState state { context.out, program, results };
    runWordOperations(state, context.arguments, queries);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

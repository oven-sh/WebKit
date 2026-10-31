/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTIRPrinter.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTSupportTestCommands.h"
#include "AOTValueFlowAnalysis.h"
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class PositionKind : uint8_t { Parameter, This, Result, Variable, Field };
constexpr unsigned numberOfPositionKinds = 5;
constexpr ASCIILiteral namesOfPositionKinds[numberOfPositionKinds] { "parameter"_s, "this"_s, "result"_s, "variable"_s, "field"_s };

struct Position {
    PositionKind kind { PositionKind::Parameter };
    uint32_t owner { 0 };
    uint32_t index { 0 };
};

struct Counts {
    uint32_t positions { 0 };
    uint32_t withoutArrival { 0 };
    uint32_t withLayoutOutside { 0 };
    uint32_t withUnknownObject { 0 };
    uint32_t withUntrackedObject { 0 };
    uint32_t withObjectWithoutLayout { 0 };
};

struct Pair {
    LayoutID named;
    LayoutID arriving;
    uint32_t positions { 0 };
    Position first;
};

struct Family {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Family);
    BitVector members;
    Vector<LayoutID, 2> roots;
    ValueSetID admitted;
};

} // anonymous namespace

static void printFamilies(PrintStream& out, ValueFlowSolution& solution)
{
    constexpr size_t numberOfPairsPrinted = 40;
    const Program& program = solution.program();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowProgramNodes& nodes = solution.bodies().nodes();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    ValueSetMaker maker(solution.bodies().sets());
    IRPrinter printer(out, program);
    HashMap<uint32_t, std::unique_ptr<Family>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> families;
    HashMap<uint64_t, uint32_t> pairAt;
    Vector<Pair> pairs;
    std::array<Counts, numberOfPositionKinds> countsOf;

    auto familyOf = [&](const DeclaredType& declared) -> const Family* {
        auto result = families.add(declared.written().index(), nullptr);
        if (!result.isNewEntry)
            return result.iterator->value.get();

        const Type& type = program.at(declared.written());
        auto family = makeUnique<Family>();
        for (LayoutID member : membersOf(program, type))
            family->members.set(member.index());
        if (family->members.isEmpty())
            return nullptr;
        family->admitted = universe.maskOf(maker, program, type);
        for (uint32_t member : family->members) {
            bool liesBelowAnother = false;
            for (LayoutID above : program.layouts[member].supertypes)
                liesBelowAnother |= above.index() != member && family->members.get(above.index());
            if (!liesBelowAnother)
                family->roots.append(LayoutID(member));
        }
        if (family->roots.isEmpty())
            family->roots.append(LayoutID(static_cast<uint32_t>(*family->members.begin())));
        result.iterator->value = WTF::move(family);
        return result.iterator->value.get();
    };
    auto count = [&](const Position& position, const DeclaredType& declared, ValueSetID arrived) {
        const Family* family = familyOf(declared);
        if (!family)
            return;

        auto leaveOutWhatIsCovered = [&](ValueFlowObjectID placeholder, ValueFlowNodeID covered) {
            if (solution.sets().contains(arrived, placeholder))
                arrived = maker.unite(maker.intersect(arrived, ValueSets::complement(maker.intersect(solver.possibleValuesOf(covered), universe.knownObjects()))), maker.of(placeholder));
        };
        leaveOutWhatIsCovered(ValueFlowUniverse::unknownObject(), ValueFlowProgramNodes::escaped());
        leaveOutWhatIsCovered(ValueFlowUniverse::untrackedObject(), ValueFlowProgramNodes::lost());

        Counts& counts = countsOf[static_cast<unsigned>(position.kind)];
        ++counts.positions;
        bool hasObject = false;
        bool hasUnknownObject = false;
        bool hasUntrackedObject = false;
        bool hasObjectWithoutLayout = false;
        Vector<LayoutID, 8> outside;
        solution.sets().forEachMember(arrived, [&](ValueFlowObjectID object) {
            if (!universe.isObject(object))
                return;
            hasObject = true;
            if (object == ValueFlowUniverse::unknownObject()) {
                hasUnknownObject = true;
                return;
            }
            if (object == ValueFlowUniverse::untrackedObject()) {
                hasUntrackedObject = true;
                return;
            }
            LayoutID layout = universe.layoutOf(object);
            if (!layout) {
                hasObjectWithoutLayout |= !solution.sets().contains(family->admitted, object);
                return;
            }
            if (!family->members.get(layout.index()) && !outside.contains(layout))
                outside.append(layout);
        });
        counts.withoutArrival += !hasObject;
        counts.withUnknownObject += hasUnknownObject;
        counts.withUntrackedObject += hasUntrackedObject;
        counts.withObjectWithoutLayout += hasObjectWithoutLayout;
        counts.withLayoutOutside += !outside.isEmpty();

        LayoutID named = family->roots[0];
        for (LayoutID arriving : outside) {
            auto result = pairAt.add((static_cast<uint64_t>(named.index()) << 32 | arriving.index()) + 1, pairs.size());
            if (result.isNewEntry)
                pairs.append({ named, arriving, 0, position });
            ++pairs[result.iterator->value].positions;
        }
    };

    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        FunctionID id(index);
        if (!function.hasBody() || !solver.isReached(id))
            continue;
        std::optional<unsigned> thisParameter = function.thisParameterIndex();
        for (unsigned parameter = 0; parameter < function.numberOfEntryParameters(); ++parameter) {
            if (const DeclaredType* declared = function.declaredEntryParameter(parameter))
                count({ thisParameter == parameter ? PositionKind::This : PositionKind::Parameter, index, parameter }, *declared, solver.possibleValuesOf(nodes.entryParameter(id, parameter)));
        }
        for (unsigned result = 0; result < function.results.size(); ++result)
            count({ PositionKind::Result, index, result }, function.results[result], solver.possibleValuesOf(nodes.result(id, result)));
    }
    for (uint32_t index = 0; index < program.variables.size(); ++index)
        count({ PositionKind::Variable, index, 0 }, program.variables[index].declared, solver.possibleValuesOf(nodes.variable(VariableID(index))));

    HashMap<uint64_t, ValueSetID> arrivedAt;
    Vector<LayoutID, 16> layouts;
    for (uint32_t node = nodes.numberOfNodes(); node < solver.graph().numberOfNodes(); ++node) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(node));
        if (!key || key->kind != ValueFlowMadeNodeKind::Written || key->name().kind != ValueFlowNameKind::Property)
            continue;
        ValueSetID value = solver.possibleValuesOf(ValueFlowNodeID(node));
        if (value == ValueSets::nothing)
            continue;
        layouts.shrink(0);
        solution.sets().forEachMember(key->set, [&](ValueFlowObjectID object) {
            LayoutID layout = universe.layoutOf(object);
            if (layout && !layouts.contains(layout))
                layouts.append(layout);
        });
        for (LayoutID layout : layouts) {
            auto result = arrivedAt.add((static_cast<uint64_t>(layout.index()) << 32 | key->payload) + 1, value);
            if (!result.isNewEntry)
                result.iterator->value = maker.unite(result.iterator->value, value);
        }
    }
    for (uint32_t layout = 0; layout < program.layouts.size(); ++layout) {
        std::span<const Field> fields = program.layouts[layout].fields;
        for (uint32_t field = 0; field < fields.size(); ++field) {
            if (!fields[field].key)
                continue;
            auto arrived = arrivedAt.find((static_cast<uint64_t>(layout) << 32 | fields[field].key.index()) + 1);
            count({ PositionKind::Field, layout, field }, fields[field].declared, arrived == arrivedAt.end() ? ValueSets::nothing : arrived->value);
        }
    }

    Counts total;
    for (unsigned kind = 0; kind < numberOfPositionKinds; ++kind) {
        const Counts& counts = countsOf[kind];
        out.println(namesOfPositionKinds[kind], " positions ", counts.positions, " layout-outside ", counts.withLayoutOutside, " unknown-object ", counts.withUnknownObject,
            " object-without-layout ", counts.withObjectWithoutLayout, " no-object ", counts.withoutArrival, " untracked-object ", counts.withUntrackedObject);
        total.positions += counts.positions;
        total.withLayoutOutside += counts.withLayoutOutside;
        total.withUnknownObject += counts.withUnknownObject;
        total.withUntrackedObject += counts.withUntrackedObject;
        total.withObjectWithoutLayout += counts.withObjectWithoutLayout;
        total.withoutArrival += counts.withoutArrival;
    }
    out.println("all positions ", total.positions, " layout-outside ", total.withLayoutOutside, " unknown-object ", total.withUnknownObject, " object-without-layout ", total.withObjectWithoutLayout,
        " no-object ", total.withoutArrival, " pairs ", pairs.size(), " untracked-object ", total.withUntrackedObject);

    auto printAfterBlank = [&](auto text) {
        out.print(" ");
        printer.print(text);
    };
    auto printLayout = [&](LayoutID layout) {
        const Layout& record = program.layouts[layout.index()];
        out.print("L", layout.index());
        if (record.name)
            printAfterBlank(record.name);
        if (record.module) {
            printAfterBlank(program.at(record.module).displayPath);
            out.print("@", record.position);
        }
    };
    AOT::sort(pairs, [](const Pair& a, const Pair& b) {
        return std::tuple(b.positions, a.named.index(), a.arriving.index()) < std::tuple(a.positions, b.named.index(), b.arriving.index());
    });
    for (const Pair& pair : pairs.span().first(std::min(numberOfPairsPrinted, pairs.size()))) {
        out.print("pair positions ", pair.positions, " named ");
        printLayout(pair.named);
        out.print(" arriving ");
        printLayout(pair.arriving);
        const Position& position = pair.first;
        out.print(" first ", namesOfPositionKinds[static_cast<unsigned>(position.kind)], " ");
        switch (position.kind) {
        case PositionKind::Parameter:
        case PositionKind::This:
        case PositionKind::Result: {
            const Function& function = program.functions[position.owner];
            out.print("f", position.owner, ":", position.index);
            if (function.module) {
                printAfterBlank(program.at(function.module).displayPath);
                out.print("@", function.position);
            }
            break;
        }
        case PositionKind::Variable: {
            const Variable& variable = program.variables[position.owner];
            out.print("g", position.owner);
            if (variable.name)
                printAfterBlank(variable.name);
            printAfterBlank(program.at(variable.module).displayPath);
            break;
        }
        case PositionKind::Field:
            printLayout(LayoutID(position.owner));
            out.print(" field ", position.index);
            break;
        }
        out.println();
    }
}

enum class ExpectedReceiver : uint8_t { LayoutsAlone, LayoutsNeverAllocated, LayoutsAndMore, NoLayout, AnyObject, Anything };
constexpr unsigned numberOfExpectedReceivers = 6;

static ExpectedReceiver expectedReceiverOf(const Program& program, const ValueFlowSolution& solution, const Function& function, ValueID receiver)
{
    const Type& type = program.at(function.at(receiver).expectedOrType().predicted());
    if (type.isAnything())
        return ExpectedReceiver::Anything;
    if (type.anyObject)
        return ExpectedReceiver::AnyObject;
    if (!type.layouts)
        return ExpectedReceiver::NoLayout;
    if (type.anyFunction || !type.classes.isEmpty() || !type.functions.isEmpty() || !type.builtins.isEmpty())
        return ExpectedReceiver::LayoutsAndMore;
    for (LayoutID layout : membersOf(program, type)) {
        if (program.layouts[layout.index()].constructor || solution.sets().size(solution.bodies().universe().objectsWith(layout)) > 1)
            return ExpectedReceiver::LayoutsAlone;
    }
    return ExpectedReceiver::LayoutsNeverAllocated;
}

static void printReceivers(PrintStream& out, ValueFlowSolution& solution)
{
    struct ReceiverCounts {
        std::array<uint32_t, numberOfExpectedReceivers> expectedWhereUnknown { };
        uint32_t accesses { 0 };
        uint32_t withUnknownObject { 0 };
        uint32_t withUntrackedObject { 0 };
        uint32_t withObjectWithoutLayout { 0 };
        uint32_t ofSeveralLayouts { 0 };
        uint32_t ofOneLayout { 0 };
        uint32_t withoutObject { 0 };
    };
    const Program& program = solution.program();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    std::array<ReceiverCounts, 2> countsOf;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody() || !solution.solver().isReached(FunctionID(index)))
            continue;
        ReceiverCounts& counts = countsOf[isLibraryCode(program, program.functions[index])];
        for (const ValueFlowStatement& statement : solution.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::PropGet && statement.opcode != Opcode::PropSet))
                continue;
            bool hasUnknownObject = false;
            bool hasUntrackedObject = false;
            bool hasObjectWithoutLayout = false;
            LayoutID first;
            bool hasSeveralLayouts = false;
            solution.sets().forEachMember(solution.possibleValuesOf(solution.bodies().operandsOf(statement)[0]), [&](ValueFlowObjectID object) {
                if (!universe.isObject(object))
                    return;
                if (object == ValueFlowUniverse::unknownObject()) {
                    hasUnknownObject = true;
                    return;
                }
                if (object == ValueFlowUniverse::untrackedObject()) {
                    hasUntrackedObject = true;
                    return;
                }
                LayoutID layout = universe.layoutOf(object);
                if (!layout)
                    hasObjectWithoutLayout = true;
                else if (!first)
                    first = layout;
                else
                    hasSeveralLayouts |= layout != first;
            });
            ++counts.accesses;
            if (hasUnknownObject) {
                ++counts.withUnknownObject;
                const Function& function = program.functions[index];
                ++counts.expectedWhereUnknown[static_cast<unsigned>(expectedReceiverOf(program, solution, function, function.at(statement.site).operand(0)))];
            } else if (hasUntrackedObject)
                ++counts.withUntrackedObject;
            else if (hasObjectWithoutLayout)
                ++counts.withObjectWithoutLayout;
            else if (hasSeveralLayouts)
                ++counts.ofSeveralLayouts;
            else if (first)
                ++counts.ofOneLayout;
            else
                ++counts.withoutObject;
        }
    }
    for (unsigned isLibrary = 0; isLibrary < 2; ++isLibrary) {
        const ReceiverCounts& counts = countsOf[isLibrary];
        out.println(isLibrary ? "library" : "application", " accesses ", counts.accesses, " unknown-object ", counts.withUnknownObject, " object-without-layout ", counts.withObjectWithoutLayout,
            " several-layouts ", counts.ofSeveralLayouts, " one-layout ", counts.ofOneLayout, " no-object ", counts.withoutObject, " untracked-object ", counts.withUntrackedObject);
        out.println(isLibrary ? "library" : "application", " unknown-object where the receiver is expected to be: layouts-alone ", counts.expectedWhereUnknown[0], " layouts-never-allocated ", counts.expectedWhereUnknown[1],
            " layouts-and-more ", counts.expectedWhereUnknown[2], " no-layout ", counts.expectedWhereUnknown[3], " any-object ", counts.expectedWhereUnknown[4], " anything ", counts.expectedWhereUnknown[5]);
    }
}

enum class ExpectedCallee : uint8_t { FunctionAlone, FunctionAndMore, NoFunction, AnyObject, Anything };
enum class WrittenAtCallee : uint8_t { Signature, OtherText, Nothing };
constexpr unsigned numberOfExpectedCallees = 5;
constexpr unsigned numberOfWrittenAtCallees = 3;

static ExpectedCallee expectedCalleeOf(const Program& program, const Function& function, ValueID callee)
{
    const Type& type = program.at(function.at(callee).expectedOrType().predicted());
    if (type.isAnything())
        return ExpectedCallee::Anything;
    if (type.anyObject)
        return ExpectedCallee::AnyObject;
    if (!type.anyFunction && type.functions.isEmpty())
        return ExpectedCallee::NoFunction;
    return type.layouts || !type.classes.isEmpty() || !type.builtins.isEmpty() ? ExpectedCallee::FunctionAndMore : ExpectedCallee::FunctionAlone;
}

static TypeSiteID typeSiteOf(const Program& program, const Function& function, ValueID id)
{
    for (unsigned steps = 0; id && steps < 16; ++steps) {
        const Value& value = function.at(id);
        TypeSiteID site;
        forEachTypeSite(value, [&](TypeSiteID found) {
            if (!site)
                site = found;
        });
        if (site)
            return site;
        switch (value.opcode) {
        case Opcode::Parameter: {
            size_t index = function.at(BlockID(0u)).parameters.find(id);
            const DeclaredType* declared = index == notFound ? nullptr : function.declaredEntryParameter(index);
            return declared ? declared->site : TypeSiteID();
        }
        case Opcode::GetModuleVariable:
            return program.at(value.variable()).declared.site;
        case Opcode::Upcast:
        case Opcode::Convert:
        case Opcode::Check:
        case Opcode::CheckInitialized:
        case Opcode::RequireObject:
        case Opcode::NarrowByField:
        case Opcode::RequireObjectCoercible:
            id = value.operand(0);
            continue;
        default:
            return { };
        }
    }
    return { };
}

static WrittenAtCallee writtenAtCallee(const Program& program, const Function& function, ValueID callee)
{
    TypeSiteID site = typeSiteOf(program, function, callee);
    if (!site || !program.at(site).text)
        return WrittenAtCallee::Nothing;
    StringPrintStream text;
    printString(text, program, program.at(site).text);
    return text.toString().contains("=>"_s) ? WrittenAtCallee::Signature : WrittenAtCallee::OtherText;
}

static void printCallees(PrintStream& out, ValueFlowSolution& solution)
{
    struct CalleeCounts {
        uint32_t calls { 0 };
        uint32_t withUnknownCallee { 0 };
        uint32_t withUntrackedCallee { 0 };
        std::array<std::array<uint32_t, numberOfWrittenAtCallees>, numberOfExpectedCallees> byExpectation { };
    };
    static constexpr std::array<ASCIILiteral, numberOfExpectedCallees> names { "function-alone"_s, "function-and-more"_s, "no-function"_s, "any-object"_s, "anything"_s };
    const Program& program = solution.program();
    std::array<CalleeCounts, 2> countsOf;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !solution.solver().isReached(FunctionID(index)))
            continue;
        CalleeCounts& counts = countsOf[isLibraryCode(program, function)];
        for (const ValueFlowStatement& statement : solution.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            if (statement.opcode != Opcode::Call && statement.opcode != Opcode::CallList && statement.opcode != Opcode::Construct && statement.opcode != Opcode::ConstructList)
                continue;
            ++counts.calls;
            ValueSetID callees = solution.possibleValuesOf(solution.bodies().operandsOf(statement)[0]);
            counts.withUntrackedCallee += solution.sets().contains(callees, ValueFlowUniverse::untrackedObject());
            if (!solution.sets().contains(callees, ValueFlowUniverse::unknownObject()))
                continue;
            ++counts.withUnknownCallee;
            ValueID callee = function.at(statement.site).operand(0);
            ++counts.byExpectation[static_cast<unsigned>(expectedCalleeOf(program, function, callee))][static_cast<unsigned>(writtenAtCallee(program, function, callee))];
        }
    }
    for (unsigned isLibrary = 0; isLibrary < 2; ++isLibrary) {
        const CalleeCounts& counts = countsOf[isLibrary];
        ASCIILiteral part = isLibrary ? "library"_s : "application"_s;
        out.println(part, " calls ", counts.calls, " unknown-callee ", counts.withUnknownCallee, " untracked-callee ", counts.withUntrackedCallee);
        for (unsigned expected = 0; expected < numberOfExpectedCallees; ++expected)
            out.println(part, " unknown-callee expected ", names[expected], ": signature ", counts.byExpectation[expected][0], " other-text ", counts.byExpectation[expected][1], " nothing-written ", counts.byExpectation[expected][2]);
    }
}

void runValueFlowFamiliesTestCommand(TestCommandContext& context)
{
    bool printsReceivers = false;
    bool printsCallees = false;
    for (const String& word : context.arguments) {
        if (word == "receivers"_s)
            printsReceivers = true;
        else if (word == "callees"_s)
            printsCallees = true;
        else
            return printBadArgument(context.out, word);
    }

    ValueFlowSolution solution(*context.program, context.build);
    if (printsReceivers)
        printReceivers(context.out, solution);
    if (printsCallees)
        printCallees(context.out, solution);
    if (!printsReceivers && !printsCallees)
        printFamilies(context.out, solution);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

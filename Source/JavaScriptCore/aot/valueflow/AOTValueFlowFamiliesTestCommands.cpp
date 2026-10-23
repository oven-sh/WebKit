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
#include "AOTValueFlowLog.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class PositionKind : uint8_t { Parameter, This, Result, Variable, Field };
constexpr unsigned numberOfPositionKinds = 5;

struct Position {
    PositionKind kind { PositionKind::Parameter };
    // The function, the variable or the layout.
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

// The layouts that a declared type names: its family.
struct Family {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Family);
    BitVector members;
    // The members that lie below no other member.
    Vector<LayoutID, 2> roots;
    // What the type admits beside objects with a layout.
    ValueSetID admitted;
};

class FamilyCounter {
public:
    FamilyCounter(ValueFlowSolution& solution, ValueSetMaker& maker)
        : m_solution(solution)
        , m_program(solution.program())
        , m_universe(solution.bodies().universe())
        , m_maker(maker)
    {
    }

    // What has escaped is left out where the unknown object arrives, which stands for it: native code hands every escaped object to every position it can reach.
    void count(const Position&, const DeclaredType&, ValueSetID arrived);
    void print(PrintStream&, size_t numberOfPairs);

private:
    const Family* familyOf(const DeclaredType&);
    void printPosition(PrintStream&, const Position&) const;
    void printLayout(PrintStream&, LayoutID) const;

    ValueFlowSolution& m_solution;
    const Program& m_program;
    const ValueFlowUniverse& m_universe;
    ValueSetMaker& m_maker;
    HashMap<uint32_t, std::unique_ptr<Family>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_families;
    HashMap<uint64_t, uint32_t> m_pairAt;
    Vector<Pair> m_pairs;
    std::array<Counts, numberOfPositionKinds> m_counts;
};

} // anonymous namespace

static ASCIILiteral nameOf(PositionKind kind)
{
    switch (kind) {
    case PositionKind::Parameter:
        return "parameter"_s;
    case PositionKind::This:
        return "this"_s;
    case PositionKind::Result:
        return "result"_s;
    case PositionKind::Variable:
        return "variable"_s;
    case PositionKind::Field:
        return "field"_s;
    }
    return "?"_s;
}

const Family* FamilyCounter::familyOf(const DeclaredType& declared)
{
    auto result = m_families.add(declared.type.index(), nullptr);
    if (!result.isNewEntry)
        return result.iterator->value.get();

    const Type& type = m_program.at(declared.type);
    auto family = makeUnique<Family>();
    for (LayoutID member : membersOf(m_program, type))
        family->members.set(member.index());
    if (family->members.isEmpty())
        return nullptr;
    family->admitted = m_universe.maskOf(m_maker, m_program, type);
    for (uint32_t member : family->members) {
        bool liesBelowAnother = false;
        for (LayoutID above : m_program.layouts[member].supertypes)
            liesBelowAnother |= above.index() != member && family->members.get(above.index());
        if (!liesBelowAnother)
            family->roots.append(LayoutID(member));
    }
    // Layouts that are converted to each other lie below each other.
    if (family->roots.isEmpty())
        family->roots.append(LayoutID(static_cast<uint32_t>(*family->members.begin())));
    result.iterator->value = WTF::move(family);
    return result.iterator->value.get();
}

void FamilyCounter::count(const Position& position, const DeclaredType& declared, ValueSetID arrived)
{
    const Family* family = familyOf(declared);
    if (!family)
        return;

    auto leaveOutWhatIsCovered = [&](ValueFlowObjectID placeholder, ValueFlowNodeID covered) {
        if (m_solution.sets().contains(arrived, placeholder))
            arrived = m_maker.unite(m_maker.intersect(arrived, ValueSets::complement(m_maker.intersect(m_solution.solver().valueOf(covered), m_universe.knownObjects()))), m_maker.of(placeholder));
    };
    leaveOutWhatIsCovered(ValueFlowUniverse::unknownObject(), ValueFlowProgramNodes::escaped());
    leaveOutWhatIsCovered(ValueFlowUniverse::untrackedObject(), ValueFlowProgramNodes::lost());

    Counts& counts = m_counts[static_cast<unsigned>(position.kind)];
    ++counts.positions;
    bool hasObject = false;
    bool hasUnknownObject = false;
    bool hasUntrackedObject = false;
    bool hasObjectWithoutLayout = false;
    Vector<LayoutID, 8> outside;
    m_solution.sets().forEachMember(arrived, [&](ValueFlowObjectID object) {
        if (!m_universe.isObject(object))
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
        LayoutID layout = m_universe.layoutOf(object);
        if (!layout) {
            hasObjectWithoutLayout |= !m_solution.sets().contains(family->admitted, object);
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
        auto result = m_pairAt.add((static_cast<uint64_t>(named.index()) << 32 | arriving.index()) + 1, m_pairs.size());
        if (result.isNewEntry)
            m_pairs.append({ named, arriving, 0, position });
        ++m_pairs[result.iterator->value].positions;
    }
}

void FamilyCounter::printLayout(PrintStream& out, LayoutID layout) const
{
    const Layout& record = m_program.layouts[layout.index()];
    IRPrinter printer(out, m_program);
    out.print("L", layout.index());
    if (record.name) {
        out.print(" ");
        printer.print(record.name);
    }
    if (record.module) {
        out.print(" ");
        printer.print(m_program.at(record.module).displayPath);
        out.print("@", record.position);
    }
}

void FamilyCounter::printPosition(PrintStream& out, const Position& position) const
{
    IRPrinter printer(out, m_program);
    out.print(nameOf(position.kind), " ");
    switch (position.kind) {
    case PositionKind::Parameter:
    case PositionKind::This:
    case PositionKind::Result: {
        const Function& function = m_program.functions[position.owner];
        out.print("f", position.owner, ":", position.index);
        if (function.module) {
            out.print(" ");
            printer.print(m_program.at(function.module).displayPath);
            out.print("@", function.position);
        }
        return;
    }
    case PositionKind::Variable: {
        const Variable& variable = m_program.variables[position.owner];
        out.print("g", position.owner);
        if (variable.name) {
            out.print(" ");
            printer.print(variable.name);
        }
        out.print(" ");
        printer.print(m_program.at(variable.module).displayPath);
        return;
    }
    case PositionKind::Field:
        printLayout(out, LayoutID(position.owner));
        out.print(" field ", position.index);
        return;
    }
}

void FamilyCounter::print(PrintStream& out, size_t numberOfPairs)
{
    Counts total;
    for (unsigned kind = 0; kind < numberOfPositionKinds; ++kind) {
        const Counts& counts = m_counts[kind];
        out.println(nameOf(static_cast<PositionKind>(kind)), " positions ", counts.positions, " layout-outside ", counts.withLayoutOutside, " unknown-object ", counts.withUnknownObject,
            " object-without-layout ", counts.withObjectWithoutLayout, " no-object ", counts.withoutArrival, " untracked-object ", counts.withUntrackedObject);
        total.positions += counts.positions;
        total.withLayoutOutside += counts.withLayoutOutside;
        total.withUnknownObject += counts.withUnknownObject;
        total.withUntrackedObject += counts.withUntrackedObject;
        total.withObjectWithoutLayout += counts.withObjectWithoutLayout;
        total.withoutArrival += counts.withoutArrival;
    }
    out.println("all positions ", total.positions, " layout-outside ", total.withLayoutOutside, " unknown-object ", total.withUnknownObject, " object-without-layout ", total.withObjectWithoutLayout,
        " no-object ", total.withoutArrival, " pairs ", m_pairs.size(), " untracked-object ", total.withUntrackedObject);

    AOT::sort(m_pairs, [](const Pair& a, const Pair& b) {
        return std::tuple(b.positions, a.named.index(), a.arriving.index()) < std::tuple(a.positions, b.named.index(), b.arriving.index());
    });
    for (const Pair& pair : m_pairs.span().first(std::min(numberOfPairs, m_pairs.size()))) {
        out.print("pair positions ", pair.positions, " named ");
        printLayout(out, pair.named);
        out.print(" arriving ");
        printLayout(out, pair.arriving);
        out.print(" first ");
        printPosition(out, pair.first);
        out.println();
    }
}

static void countPositionsOfFunctions(ValueFlowSolution& solution, FamilyCounter& counter)
{
    const Program& program = solution.program();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowProgramNodes& nodes = solution.bodies().nodes();
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        FunctionID id(index);
        if (!function.hasBody() || !solver.isReached(id))
            continue;
        std::optional<unsigned> thisParameter = function.thisParameterIndex();
        for (unsigned parameter = 0; parameter < function.numberOfEntryParameters(); ++parameter) {
            if (const DeclaredType* declared = function.declaredEntryParameter(parameter))
                counter.count({ thisParameter == parameter ? PositionKind::This : PositionKind::Parameter, index, parameter }, *declared, solver.valueOf(nodes.entryParameter(id, parameter)));
        }
        for (unsigned result = 0; result < function.results.size(); ++result)
            counter.count({ PositionKind::Result, index, result }, function.results[result], solver.valueOf(nodes.result(id, result)));
    }
    for (uint32_t index = 0; index < program.variables.size(); ++index)
        counter.count({ PositionKind::Variable, index, 0 }, program.variables[index].declared, solver.valueOf(nodes.variable(VariableID(index))));
}

// What is written under a name through a set of objects arrives at the field of that name of each layout that an object of the set has.
static void countFields(ValueFlowSolution& solution, FamilyCounter& counter, ValueSetMaker& maker)
{
    const Program& program = solution.program();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowUniverse& universe = solution.bodies().universe();

    HashMap<uint64_t, ValueSetID> arrivedAt;
    Vector<LayoutID, 16> layouts;
    for (uint32_t node = solution.bodies().nodes().numberOfNodes(); node < solver.graph().numberOfNodes(); ++node) {
        const ValueFlowNodeKey* key = solver.keyOfMadeNode(ValueFlowNodeID(node));
        if (!key || key->kind != ValueFlowMadeNodeKind::Written || key->name().kind != ValueFlowNameKind::Property)
            continue;
        ValueSetID value = solver.valueOf(ValueFlowNodeID(node));
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
            counter.count({ PositionKind::Field, layout, field }, fields[field].declared, arrived == arrivedAt.end() ? ValueSets::nothing : arrived->value);
        }
    }
}

enum class ExpectedReceiver : uint8_t { LayoutsAlone, LayoutsNeverAllocated, LayoutsAndMore, NoLayout, AnyObject, Anything };
constexpr unsigned numberOfExpectedReceivers = 6;

static ExpectedReceiver expectedReceiverOf(const Program& program, const ValueFlowSolution& solution, const Function& function, ValueID receiver)
{
    const Type& type = program.at(function.at(receiver).expectedOrType());
    if (type.isAnything())
        return ExpectedReceiver::Anything;
    if (type.anyObject)
        return ExpectedReceiver::AnyObject;
    if (!type.layouts)
        return ExpectedReceiver::NoLayout;
    if (type.anyFunction || !type.classes.isEmpty() || !type.functions.isEmpty() || !type.builtins.isEmpty())
        return ExpectedReceiver::LayoutsAndMore;
    // Every layout has one object for what its constructor makes, whether it has a constructor or not.
    for (LayoutID layout : membersOf(program, type)) {
        if (program.layouts[layout.index()].constructor || solution.sets().size(solution.bodies().universe().objectsWith(layout)) > 1)
            return ExpectedReceiver::LayoutsAlone;
    }
    return ExpectedReceiver::LayoutsNeverAllocated;
}

// The reads and stores by name in reached functions, by what the receiver may be.
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
            solution.sets().forEachMember(solution.valueOf(solution.bodies().operandsOf(statement)[0]), [&](ValueFlowObjectID object) {
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
    const Type& type = program.at(function.at(callee).expectedOrType());
    if (type.isAnything())
        return ExpectedCallee::Anything;
    if (type.anyObject)
        return ExpectedCallee::AnyObject;
    if (!type.anyFunction && type.functions.isEmpty())
        return ExpectedCallee::NoFunction;
    return type.layouts || !type.classes.isEmpty() || !type.builtins.isEmpty() ? ExpectedCallee::FunctionAndMore : ExpectedCallee::FunctionAlone;
}

// The place in the text whose type the checker has written down for the value: at the value itself, or at what it is a copy of.
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

// The calls in reached functions whose callee may be the unknown object, by what is expected of the callee and by what the checker has written there.
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
            ValueSetID callees = solution.valueOf(solution.bodies().operandsOf(statement)[0]);
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

static ValueID definitionThroughCopies(const Function& function, ValueID id)
{
    for (unsigned steps = 0; steps < 16; ++steps) {
        switch (function.at(id).opcode) {
        case Opcode::Upcast:
        case Opcode::Convert:
        case Opcode::Check:
        case Opcode::CheckInitialized:
        case Opcode::RequireObject:
        case Opcode::NarrowByField:
        case Opcode::RequireObjectCoercible:
            id = function.at(id).operand(0);
            continue;
        default:
            return id;
        }
    }
    return id;
}

static ASCIILiteral describeObjectsIn(ValueFlowSolution& solution, ValueSetID set)
{
    const ValueFlowUniverse& universe = solution.bodies().universe();
    bool hasUnknownObject = false;
    bool hasUntrackedObject = false;
    bool hasObjectWithoutLayout = false;
    bool hasSeveralLayouts = false;
    LayoutID first;
    solution.sets().forEachMember(set, [&](ValueFlowObjectID object) {
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
    if (hasUnknownObject)
        return "unknown-object"_s;
    if (hasUntrackedObject)
        return "untracked-object"_s;
    if (hasObjectWithoutLayout)
        return "object-without-layout"_s;
    if (hasSeveralLayouts)
        return "several-layouts"_s;
    return first ? "one-layout"_s : "no-object"_s;
}

// One line for each call in a reached function whose callee may be the unknown object, its parts set apart by tabs.
static void printCalleeSites(PrintStream& out, ValueFlowSolution& solution)
{
    const Program& program = solution.program();
    IRPrinter printer(out, program);
    HashMap<uint32_t, uint32_t> statementAt;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !solution.solver().isReached(FunctionID(index)))
            continue;
        std::span<const ValueFlowStatement> statements = solution.bodies().statementsOf(index);
        statementAt.clear();
        for (uint32_t position = 0; position < statements.size(); ++position) {
            if (statements[position].kind == ValueFlowStatementKind::Operation && statements[position].site)
                statementAt.add(statements[position].site.index() + 1, position);
        }
        for (const ValueFlowStatement& statement : statements) {
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;
            if (statement.opcode != Opcode::Call && statement.opcode != Opcode::CallList && statement.opcode != Opcode::Construct && statement.opcode != Opcode::ConstructList)
                continue;
            if (!solution.sets().contains(solution.valueOf(solution.bodies().operandsOf(statement)[0]), ValueFlowUniverse::unknownObject()))
                continue;
            out.print("site\t", isLibraryCode(program, function) ? "library" : "application", "\t");
            if (function.module)
                printer.print(program.at(function.module).displayPath);
            out.print("\t", function.locationOf(statement.site).position, "\tf", index, "\t");
            ValueID definition = definitionThroughCopies(function, function.at(statement.site).operand(0));
            const Value& value = function.at(definition);
            out.print(value.opcode, "\t");
            switch (value.opcode) {
            case Opcode::PropGet: {
                printer.print(value.key());
                auto read = statementAt.find(definition.index() + 1);
                out.print("\t", read == statementAt.end() ? "?"_s : describeObjectsIn(solution, solution.valueOf(solution.bodies().operandsOf(statements[read->value])[0])));
                break;
            }
            case Opcode::GetModuleVariable:
                if (StringID name = program.at(value.variable()).name)
                    printer.print(name);
                break;
            case Opcode::Parameter:
                out.print(function.at(BlockID(0u)).parameters.find(definition));
                break;
            default:
                break;
            }
            out.println();
        }
    }
}

// valueflow-families [receivers] [callees] [callee-sites] [pairs=<n>]: for every declared position whose type names layouts, whether an object arrives there whose layout is outside that family.
void runValueFlowFamiliesTestCommand(TestCommandContext& context)
{
    size_t numberOfPairs = 40;
    bool printsReceivers = false;
    bool printsCallees = false;
    bool printsCalleeSites = false;
    for (const String& word : context.arguments) {
        if (word == "receivers"_s) {
            printsReceivers = true;
            continue;
        }
        if (word == "callees"_s) {
            printsCallees = true;
            continue;
        }
        if (word == "callee-sites"_s) {
            printsCalleeSites = true;
            continue;
        }
        std::optional<uint32_t> number = word.startsWith("pairs="_s) ? parseInteger<uint32_t>(StringView(word).substring(6)) : std::nullopt;
        if (!number)
            return printBadArgument(context.out, word);
        numberOfPairs = *number;
    }

    ValueFlowSolution solution(*context.program, context.build);
    if (!solution.hasResult()) {
        context.out.println("stopped at a limit");
        return;
    }
    if (printsReceivers)
        printReceivers(context.out, solution);
    if (printsCallees)
        printCallees(context.out, solution);
    if (printsCalleeSites)
        printCalleeSites(context.out, solution);
    if (printsReceivers || printsCallees || printsCalleeSites)
        return;
    ValueSetMaker maker(solution.bodies().sets());
    FamilyCounter counter(solution, maker);
    countPositionsOfFunctions(solution, counter);
    countFields(solution, counter, maker);
    counter.print(context.out, numberOfPairs);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTFlowAnalysisInternal.h"
#include "AOTFlowCallState.h"
#include "AOTFlowNames.h"
#include "AOTGrouping.h"
#include "AOTIRPrinter.h"
#include "AOTSupportTestCommands.h"
#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>
#include <wtf/Noncopyable.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PrefixedIndex {
    char prefix;
    uint32_t index;
};

class FlowPrinter {
    WTF_MAKE_NONCOPYABLE(FlowPrinter);
public:
    FlowPrinter(PrintStream& out, const Program& program)
        : m_out(out)
        , m_program(program)
        , m_irPrinter(out, program)
    {
    }

    PrintStream& out() { return m_out; }

    template<typename... Items>
    void print(const Items&... items)
    {
        (printItem(items), ...);
    }

    template<typename... Items>
    void println(const Items&... items)
    {
        print(items..., "\n"_s);
    }

private:
    template<typename Tag>
    static std::optional<PrefixedIndex> prefixed(char prefix, ID<Tag> id)
    {
        return id ? std::optional(PrefixedIndex { prefix, id.index() }) : std::nullopt;
    }

    template<typename Item>
    void printItem(const Item& item) { m_irPrinter.print(item); }

    template<typename Enumerator>
        requires std::is_enum_v<Enumerator>
    void printItem(const Enumerator& enumerator) { m_out.print(nameOf(enumerator)); }

    template<typename Flag>
    void printItem(const OptionSet<Flag>& flags) { dumpFlags(m_out, flags); }

    template<typename Item>
    void printItem(const std::optional<Item>& item)
    {
        if (item)
            printItem(*item);
        else
            m_out.print("-"_s);
    }

    template<typename Tag, typename Storage>
    void printItem(const BuiltinTableID<Tag, Storage>& id)
    {
        if (id)
            m_irPrinter.print(id);
        else
            m_out.print("-"_s);
    }

    void printItem(ASCIILiteral text) { m_out.print(text); }
    void printItem(const CommaPrinter& comma) { m_out.print(comma); }
    void printItem(PrefixedIndex item) { m_out.print(item.prefix, item.index); }
    void printItem(FlowInstanceID instance) { printItem(prefixed('i', instance)); }
    void printItem(FlowNodeID node) { printItem(prefixed('n', node)); }
    void printItem(FlowStatementReference statement) { print(statement.kind, "#"_s, statement.index); }
    void printItem(FunctionID);
    void printItem(BuiltinOperand);
    void printItem(const Witness&);

    PrintStream& m_out;
    const Program& m_program;
    IRPrinter m_irPrinter;
};

void FlowPrinter::printItem(FunctionID function)
{
    if (function == FunctionID(m_program.functions.size()))
        m_out.print("host"_s);
    else
        m_irPrinter.print(function);
}

void FlowPrinter::printItem(BuiltinOperand operand)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        print("receiver"_s);
        return;
    case BuiltinOperand::Kind::Argument:
        print("argument"_s, operand.argumentIndex());
        return;
    case BuiltinOperand::Kind::ArgumentsFrom:
        print("argumentsFrom"_s, operand.argumentIndex());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowPrinter::printItem(const Witness& witness)
{
    if (!hasFunction(witness)) {
        print("-"_s);
        return;
    }
    print(FunctionID(witness.function));
    if (witness.value != Wire::none)
        print(":"_s, ValueID(witness.value));
}

class FlowSummaryPrinter {
    WTF_MAKE_NONCOPYABLE(FlowSummaryPrinter);
public:
    FlowSummaryPrinter(PrintStream& out, const Program& program, const FlowSummary& summary)
        : m_printer(out, program)
        , m_summary(summary)
    {
    }

    void printSummary();

private:
    static bool isSet(bool value) { return value; }
    static bool isSet(uint32_t) { return true; }
    static bool isSet(PrefixedIndex) { return true; }
    template<typename Tag> static bool isSet(ID<Tag> id) { return !!id; }
    template<typename Tag, typename Storage> static bool isSet(BuiltinTableID<Tag, Storage> id) { return !!id; }
    template<typename T> static bool isSet(const std::optional<T>& value) { return value.has_value(); }
    template<typename Flag> static bool isSet(OptionSet<Flag> flags) { return !flags.isEmpty(); }
    template<typename Enumerator> requires std::is_enum_v<Enumerator> static bool isSet(Enumerator) { return true; }

    static PrefixedIndex siteName(uint32_t site) { return { 's', site }; }
    std::optional<PrefixedIndex> localName(FlowLocalID) const;

    void printLocals();
    void printExternals();
    void printSites();
    void printSeed(const FlowSeed&);
    void printCopy(FlowLocalID from, FlowLocalID to);
    void printCopies();
    void printStatements();
    void printWaiters();
    void printClosureCreations();
    void printCoroutine();

    template<typename IDType>
    void printIfSet(IDType id)
    {
        if (isSet(id))
            m_printer.print(" "_s, id);
    }

    template<typename T>
    void printMember(ASCIILiteral name, const T& value)
    {
        if (isSet(value))
            m_printer.print(" "_s, name, "="_s, value);
    }

    void printMember(ASCIILiteral name, FlowLocalID);
    void printMember(ASCIILiteral name, FlowLocalList);
    void printMember(ASCIILiteral name, const FlowSeed&);

#define DECLARE_FLOW_STATEMENT_DUMP(kind, type, member, cursors) void dump(const type&);
    FOR_EACH_FLOW_STATEMENT_KIND(DECLARE_FLOW_STATEMENT_DUMP)
#undef DECLARE_FLOW_STATEMENT_DUMP

    FlowPrinter m_printer;
    const FlowSummary& m_summary;
};

std::optional<PrefixedIndex> FlowSummaryPrinter::localName(FlowLocalID local) const
{
    if (!local)
        return std::nullopt;
    if (m_summary.isExternal(local))
        return PrefixedIndex { 'x', local.index() - m_summary.numberOfLocals };
    return PrefixedIndex { 'l', local.index() };
}

void FlowSummaryPrinter::printSummary()
{
    m_printer.print("summary "_s, m_summary.function, ": parameters="_s, m_summary.numberOfEntryParameters, " results="_s, m_summary.numberOfResults);
    m_printer.println(" locals="_s, m_summary.numberOfLocals, " values="_s, m_summary.numberOfValues, " flags="_s, m_summary.flags);

    printLocals();
    printExternals();
    printSites();
    for (const FlowSeed& seed : m_summary.seeds) {
        m_printer.print("  seed "_s);
        printSeed(seed);
        m_printer.println();
    }
    printCopies();
    printStatements();
    printWaiters();
    printClosureCreations();
    printCoroutine();
}

void FlowSummaryPrinter::printLocals()
{
    Vector<uint32_t> firstValue;
    Vector<ValueID> values;
    groupByIndex(m_summary.numberOfLocals, firstValue, values, [&](const auto& visit) {
        for (uint32_t index = 0; index < m_summary.localOfValue.size(); ++index) {
            if (FlowLocalID local = m_summary.localOfValue[index])
                visit(local.index(), ValueID(index));
        }
    });

    for (uint32_t index = 0; index < m_summary.numberOfLocals; ++index) {
        std::span<const ValueID> valuesOfLocal = entriesOfGroup(firstValue, values, index);
        if (valuesOfLocal.empty())
            continue;
        m_printer.print("  "_s, PrefixedIndex { 'l', index }, " ="_s);
        CommaPrinter comma(", "_s, " "_s);
        for (ValueID value : valuesOfLocal)
            m_printer.print(comma, value);
        m_printer.println();
    }
}

void FlowSummaryPrinter::printExternals()
{
    for (uint32_t index = 0; index < m_summary.externals.size(); ++index) {
        const FlowExternalNode& external = m_summary.externals[index];
        m_printer.print("  "_s, PrefixedIndex { 'x', index }, " = "_s, external.kind());
        switch (external.kind()) {
        case FlowExternalKind::Variable:
            m_printer.print(" "_s, external.variable());
            break;
        case FlowExternalKind::Heritage:
            m_printer.print(" "_s, external.layout());
            break;
        case FlowExternalKind::NativeCode:
        case FlowExternalKind::UnknownJavaScript:
            break;
        case FlowExternalKind::SiteField:
        case FlowExternalKind::SiteCapture:
        case FlowExternalKind::SitePosition:
            m_printer.print(" "_s, siteName(external.site()), " "_s, external.index());
            break;
        case FlowExternalKind::SiteSlot:
            m_printer.print(" "_s, siteName(external.site()), " "_s, external.slot());
            break;
        case FlowExternalKind::SiteUndeclaredPart:
            m_printer.print(" "_s, siteName(external.site()));
            break;
        }
        m_printer.println();
    }
}

void FlowSummaryPrinter::printSites()
{
    for (uint32_t index = 0; index < m_summary.sites.size(); ++index) {
        const FlowSiteDescription& site = m_summary.sites[index];
        m_printer.print("  "_s, siteName(index), " = "_s, site.kind, " "_s, site.origin);
        printIfSet(site.value);
        printIfSet(site.layout);
        printIfSet(site.builtinClass);
        printIfSet(site.closureFunction);
        if (site.numberOfPositions)
            m_printer.print(" positions="_s, uint32_t { site.numberOfPositions });
        if (site.ordinal)
            m_printer.print(" ordinal="_s, site.ordinal);
        m_printer.println();
    }
}

void FlowSummaryPrinter::printSeed(const FlowSeed& seed)
{
    m_printer.print(localName(seed.node()), " "_s, seed.kind());
    switch (seed.kind()) {
    case FlowSeedKind::Primitives:
        m_printer.print(" "_s, OptionSet<PrimitiveSet>::fromRaw(seed.primitives()));
        return;
    case FlowSeedKind::Integers: {
        Range range = m_summary.ranges[seed.rangeIndex()];
        m_printer.print(" ["_s, range.minimum, ", "_s, range.maximum, "]"_s);
        return;
    }
    case FlowSeedKind::String:
        m_printer.print(" "_s, seed.string());
        return;
    case FlowSeedKind::Symbol:
        m_printer.print(" "_s, seed.symbol());
        return;
    case FlowSeedKind::Site:
        m_printer.print(" "_s, siteName(seed.site()));
        return;
    case FlowSeedKind::Builtin:
        m_printer.print(" "_s, seed.builtin());
        return;
    case FlowSeedKind::ModuleBuiltin:
        m_printer.print(" "_s, seed.builtin(), " "_s, seed.module());
        return;
    case FlowSeedKind::Namespace:
        m_printer.print(" "_s, seed.module());
        return;
    case FlowSeedKind::TypeAsRead:
        m_printer.print(" "_s, seed.type());
        return;
    case FlowSeedKind::Unknown:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSummaryPrinter::printCopy(FlowLocalID from, FlowLocalID to)
{
    m_printer.println("  copy "_s, localName(from), " -> "_s, localName(to));
}

void FlowSummaryPrinter::printCopies()
{
    for (uint32_t index = 0; index < m_summary.numberOfLocals; ++index) {
        FlowLocalID from(index);
        for (FlowLocalID to : m_summary.successors.of(from))
            printCopy(from, to);
    }
    for (auto [from, to] : m_summary.copiesFromExternals)
        printCopy(from, to);
}

void FlowSummaryPrinter::printStatements()
{
#define PRINT_FLOW_STATEMENTS(kind, type, member, cursors) \
    for (uint32_t index = 0; index < m_summary.statements.member.size(); ++index) { \
        m_printer.print("  "_s, FlowStatementReference { FlowStatementKind::kind, index }); \
        dump(m_summary.statements.member[index]); \
        m_printer.println(); \
    }
    FOR_EACH_FLOW_STATEMENT_KIND(PRINT_FLOW_STATEMENTS)
#undef PRINT_FLOW_STATEMENTS
}

void FlowSummaryPrinter::printWaiters()
{
    for (uint32_t index = 0; index < m_summary.numberOfLocals; ++index) {
        std::span<const FlowStatementReference> waiters = m_summary.waiters.of(FlowLocalID(index));
        if (waiters.empty())
            continue;
        m_printer.print("  waiters "_s, PrefixedIndex { 'l', index }, ":"_s);
        CommaPrinter comma(", "_s, " "_s);
        for (FlowStatementReference waiter : waiters)
            m_printer.print(comma, waiter);
        m_printer.println();
    }
}

void FlowSummaryPrinter::printClosureCreations()
{
    for (const FlowClosureCreation& creation : m_summary.closureCreations)
        m_printer.println("  closure "_s, localName(creation.node), " "_s, creation.function, " "_s, siteName(creation.site), " "_s, creation.use, " "_s, creation.capture);
}

void FlowSummaryPrinter::printCoroutine()
{
    const FlowCoroutineSummary& coroutine = m_summary.coroutine;
    if (coroutine.resultSite) {
        m_printer.print("  coroutine result="_s, siteName(*coroutine.resultSite));
        printMember("yielded"_s, coroutine.yielded);
        printMember("sent"_s, coroutine.sent);
        m_printer.println();
    }
    if (m_summary.restSite)
        m_printer.println("  rest "_s, siteName(*m_summary.restSite));
}

void FlowSummaryPrinter::printMember(ASCIILiteral name, FlowLocalID local)
{
    if (local)
        m_printer.print(" "_s, name, "="_s, localName(local));
}

void FlowSummaryPrinter::printMember(ASCIILiteral name, FlowLocalList list)
{
    if (!list.count)
        return;
    m_printer.print(" "_s, name, "=["_s);
    CommaPrinter comma;
    for (FlowLocalID local : m_summary.statements.list(list))
        m_printer.print(comma, localName(local));
    m_printer.print("]"_s);
}

void FlowSummaryPrinter::printMember(ASCIILiteral name, const FlowSeed& seed)
{
    m_printer.print(" "_s, name, "=("_s);
    printSeed(seed);
    m_printer.print(")"_s);
}

#define PRINT_MEMBER(name) printMember(#name ""_s, statement.name)

void FlowSummaryPrinter::dump(const FlowFilter& statement)
{
    PRINT_MEMBER(from);
    PRINT_MEMBER(to);
    if (!statement.range)
        PRINT_MEMBER(type);
    PRINT_MEMBER(kind);
    if (!statement.field)
        return;
    PRINT_MEMBER(field);
    if (!statement.range)
        return;
    Range range = m_summary.ranges[*statement.range];
    m_printer.print(" range=["_s, range.minimum, ", "_s, range.maximum, "]"_s);
}

void FlowSummaryPrinter::dump(const FlowConditionalSeed& statement)
{
    PRINT_MEMBER(from);
    PRINT_MEMBER(type);
    PRINT_MEMBER(kind);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(seed);
}

void FlowSummaryPrinter::dump(const FlowNaturalOperation& statement)
{
    PRINT_MEMBER(origin);
    PRINT_MEMBER(result);
    PRINT_MEMBER(operands);
    if (statement.kind != FlowNaturalKind::Result)
        m_printer.print(" narrows="_s, static_cast<unsigned>(statement.narrowedOperand), statement.kind == FlowNaturalKind::OperandOnTakenEdge ? " edge=taken"_s : " edge=notTaken"_s);
}

void FlowSummaryPrinter::dump(const FlowAllocation& statement)
{
    PRINT_MEMBER(origin);
    printMember("site"_s, siteName(statement.site));
    PRINT_MEMBER(prototype);
    PRINT_MEMBER(isInCycle);
}

void FlowSummaryPrinter::dump(const FlowLocationAccess& statement)
{
    PRINT_MEMBER(kind);
    PRINT_MEMBER(use);
    PRINT_MEMBER(isInCycle);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(location);
    PRINT_MEMBER(value);
}

void FlowSummaryPrinter::dump(const FlowFieldAccess& statement)
{
    PRINT_MEMBER(kind);
    PRINT_MEMBER(use);
    PRINT_MEMBER(isInCycle);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(receiver);
    PRINT_MEMBER(layout);
    PRINT_MEMBER(field);
    PRINT_MEMBER(value);
    PRINT_MEMBER(thrown);
}

void FlowSummaryPrinter::dump(const FlowPropertyAccess& statement)
{
    PRINT_MEMBER(kind);
    PRINT_MEMBER(use);
    PRINT_MEMBER(flags);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(receiver);
    PRINT_MEMBER(key);
    PRINT_MEMBER(value);
    PRINT_MEMBER(thisValue);
    PRINT_MEMBER(thrown);
}

void FlowSummaryPrinter::dump(const FlowElementAccess& statement)
{
    PRINT_MEMBER(kind);
    PRINT_MEMBER(use);
    PRINT_MEMBER(flags);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(receiver);
    PRINT_MEMBER(key);
    PRINT_MEMBER(value);
    PRINT_MEMBER(thisValue);
    PRINT_MEMBER(thrown);
}

void FlowSummaryPrinter::dump(const FlowSlotAccess& statement)
{
    PRINT_MEMBER(kind);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(receiver);
    PRINT_MEMBER(contentsSlot);
    PRINT_MEMBER(slot);
    PRINT_MEMBER(internalFieldIndex);
    PRINT_MEMBER(position);
    PRINT_MEMBER(value);
    if (statement.mayLeaveHole)
        PRINT_MEMBER(mayLeaveHole);
}

void FlowSummaryPrinter::dump(const FlowPrototypeAccess& statement)
{
    PRINT_MEMBER(kind);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(object);
    PRINT_MEMBER(value);
}

void FlowSummaryPrinter::dump(const FlowPropertyCopy& statement)
{
    PRINT_MEMBER(origin);
    PRINT_MEMBER(source);
    PRINT_MEMBER(target);
    PRINT_MEMBER(excludedKeys);
    PRINT_MEMBER(thrown);
}

void FlowSummaryPrinter::dump(const FlowCall& statement)
{
    PRINT_MEMBER(mode);
    PRINT_MEMBER(flags);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(callee);
    PRINT_MEMBER(thisValue);
    PRINT_MEMBER(newTarget);
    PRINT_MEMBER(arguments);
    PRINT_MEMBER(argumentList);
    PRINT_MEMBER(result);
    PRINT_MEMBER(thrown);
    PRINT_MEMBER(derivedLayout);
    PRINT_MEMBER(expectedResult);
}

void FlowSummaryPrinter::dump(const FlowDirectCall& statement)
{
    PRINT_MEMBER(flags);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(function);
    PRINT_MEMBER(arguments);
    PRINT_MEMBER(results);
    PRINT_MEMBER(thrown);
}

void FlowSummaryPrinter::dump(const FlowClassDefinition& statement)
{
    PRINT_MEMBER(origin);
    PRINT_MEMBER(layout);
    PRINT_MEMBER(constructor);
    PRINT_MEMBER(heritage);
    PRINT_MEMBER(prototypeParent);
    PRINT_MEMBER(isInCycle);
}

void FlowSummaryPrinter::dump(const FlowMethodDefinition& statement)
{
    PRINT_MEMBER(origin);
    PRINT_MEMBER(layout);
    PRINT_MEMBER(side);
    PRINT_MEMBER(methodKind);
    PRINT_MEMBER(index);
    PRINT_MEMBER(classObject);
    PRINT_MEMBER(function);
    PRINT_MEMBER(key);
    PRINT_MEMBER(computedKey);
}

void FlowSummaryPrinter::dump(const FlowAwait& statement)
{
    PRINT_MEMBER(origin);
    PRINT_MEMBER(value);
    PRINT_MEMBER(result);
    PRINT_MEMBER(thrown);
}

void FlowSummaryPrinter::dump(const FlowIteration& statement)
{
    PRINT_MEMBER(step);
    PRINT_MEMBER(iteratorKind);
    PRINT_MEMBER(resumeMode);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(iterable);
    PRINT_MEMBER(state);
    PRINT_MEMBER(sent);
    PRINT_MEMBER(value);
    PRINT_MEMBER(thrown);
    PRINT_MEMBER(stepIndex);
}

void FlowSummaryPrinter::dump(const FlowOperandUse& statement)
{
    PRINT_MEMBER(origin);
    PRINT_MEMBER(operandIndex);
    PRINT_MEMBER(operand);
    PRINT_MEMBER(retainer);
    PRINT_MEMBER(result);
    PRINT_MEMBER(thrown);
    PRINT_MEMBER(isTerminatorOperand);
}

void FlowSummaryPrinter::dump(const FlowModuleOperation& statement)
{
    PRINT_MEMBER(kind);
    PRINT_MEMBER(origin);
    PRINT_MEMBER(module);
    PRINT_MEMBER(runtimeModule);
    PRINT_MEMBER(specifier);
    PRINT_MEMBER(result);
    PRINT_MEMBER(thrown);
}

void FlowSummaryPrinter::dump(const FlowUnseenCodeEvent& statement)
{
    PRINT_MEMBER(origin);
    PRINT_MEMBER(cause);
}

#undef PRINT_MEMBER

struct LinkSection {
    ASCIILiteral word;
    void (*dump)(FlowPrinter&, const FlowSolvedProgram&);
};

struct OrderSection {
    ASCIILiteral word;
    uint32_t numberOfModules;
    void (*dump)(FlowPrinter&, const FlowSolvedProgram&, std::span<const ModuleID>);
};

enum class ScopeOrderLines : uint8_t { Timelines, Locations };

} // anonymous namespace

static void dumpSummary(TestCommandContext& context, const FlowSummary& summary)
{
    FlowSummaryPrinter(context.out, *context.program, summary).printSummary();
}

void runFlowSummaryTestCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    FlowSummaryInputs inputs { program, context.build };

    if (context.arguments.empty()) {
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (program.functions[index].hasBody())
                dumpSummary(context, buildFlowSummary(inputs, FunctionID(index)));
        }
        dumpSummary(context, buildHostSummary(inputs));
        return;
    }

    auto parseFunction = functionParser(program);
    for (const String& word : context.arguments) {
        if (word == "host"_s) {
            dumpSummary(context, buildHostSummary(inputs));
            continue;
        }
        std::optional<FunctionID> function = parseFunction(word);
        if (!function)
            return printBadArgument(context.out, word);
        dumpSummary(context, buildFlowSummary(inputs, *function));
    }
}

static void dumpCallLinks(FlowPrinter& printer, const FlowSolvedProgram& solved)
{
    std::span<const FlowCallLink> links = solved.records.callLinks();
    for (uint32_t index = 0; index < links.size(); ++index) {
        const FlowCallLink& link = links[index];
        printer.print("link "_s, index, ": "_s, link.callerInstance, " "_s, link.caller, " "_s, link.site);
        printer.print(" -> "_s, link.calleeInstance, " "_s, link.callee, " "_s, link.kind, " "_s, enumName(link.times));
        if (link.isInCycle)
            printer.print(" cycle"_s);
        printer.println();
    }
}

static void dumpDescription(FlowPrinter& printer, const FlowSolvedProgram& solved, const FlowNodeDescription& description)
{
    if (belongsToObject(description.kind))
        solved.objects->dump(printer.out(), description.object);

    switch (description.kind) {
    case FlowNodeKind::NativeCode:
        printer.print("nativeCode"_s);
        return;
    case FlowNodeKind::UnknownJavaScript:
        printer.print("unknownJavaScript"_s);
        return;
    case FlowNodeKind::Variable:
        printer.print(description.variable);
        return;
    case FlowNodeKind::Heritage:
        printer.print(description.layout, ".heritage"_s);
        return;
    case FlowNodeKind::DeclaredPart:
        printer.print(".part"_s, description.index);
        return;
    case FlowNodeKind::Position:
        printer.print(".position"_s, description.index);
        return;
    case FlowNodeKind::UndeclaredPart:
        printer.print(".undeclaredPart"_s);
        return;
    case FlowNodeKind::PrototypeLink:
        printer.print(".prototypeLink"_s);
        return;
    case FlowNodeKind::IndexedProperties:
        printer.print(".indexedProperties"_s);
        return;
    case FlowNodeKind::StringProperties:
        printer.print(".stringProperties"_s);
        return;
    case FlowNodeKind::SymbolProperties:
        printer.print(".symbolProperties"_s);
        return;
    case FlowNodeKind::Property:
        printer.print("["_s, description.key, "]"_s);
        return;
    case FlowNodeKind::Getter:
        printer.print(".getter["_s, description.key, "]"_s);
        return;
    case FlowNodeKind::Setter:
        printer.print(".setter["_s, description.key, "]"_s);
        return;
    case FlowNodeKind::Temporary:
        printer.print("temporary"_s);
        return;
    case FlowNodeKind::Local:
        printer.print(solved.graph->at(description.instance).function, ":"_s, PrefixedIndex { 'l', description.local.index() });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpNode(FlowPrinter& printer, const FlowSolvedProgram& solved, FlowNodeID node)
{
    printer.print(node);
    if (!node)
        return;
    printer.print("("_s);
    dumpDescription(printer, solved, solved.graph->describe(node));
    printer.print(")"_s);
}

static void dumpOperandFlows(FlowPrinter& printer, const FlowSolvedProgram& solved)
{
    for (const FlowOperandFlow& flow : solved.records.operandFlows()) {
        printer.print("operand "_s);
        dumpNode(printer, solved, flow.from);
        printer.print(" -> "_s);
        dumpNode(printer, solved, flow.parameter);
        printer.println(" link "_s, flow.link.index());
    }
}

static void dumpBuiltinCallLinks(FlowPrinter& printer, const FlowSolvedProgram& solved)
{
    for (const FlowBuiltinCallLink& link : solved.records.builtinCallLinks())
        printer.println("builtin "_s, link.callerInstance, " "_s, link.site, " "_s, link.overload, " "_s, enumName(link.mode));

    for (const FlowBuiltinOperandValue& value : solved.records.builtinOperandValues()) {
        printer.print("operand-value "_s, value.callerInstance, " "_s, value.site, " "_s, value.overload, " "_s, value.operand, " ["_s);
        CommaPrinter comma;
        for (BuiltinOperandRole role : value.roles)
            printer.print(comma, enumName(role));
        printer.println("] "_s, value.node);
    }
}

static void dumpUnknownCallees(FlowPrinter& printer, const FlowSolvedProgram& solved)
{
    for (const FlowUnknownCallee& callee : solved.records.unknownCallees())
        printer.println("unknown-callee "_s, callee.callerInstance, " "_s, callee.site, " "_s, callee.kind);
}

static void dumpModuleSet(FlowPrinter& printer, ASCIILiteral label, const BitVector& modules)
{
    printer.print(label);
    for (size_t index : modules)
        printer.print(" "_s, ModuleID(index));
    printer.println();
}

static void dumpModules(FlowPrinter& printer, const FlowSolvedProgram& solved)
{
    dumpModuleSet(printer, "evaluated"_s, solved.records.evaluatedModules());
    dumpModuleSet(printer, "namespaces"_s, solved.records.createdNamespaces());
}

static void dumpUnseenCode(FlowPrinter& printer, const FlowSolvedProgram& solved)
{
    for (const UnseenCodeRecord& record : solved.records.unseenCode())
        printer.println("unseen "_s, record.cause, " "_s, BuiltinID(record.builtin), " "_s, record.witness);
    printer.println("unknown-javascript "_s, yesOrNo(solved.records.unseenCodeCanRun()));
    printer.println("whole-program-open "_s, yesOrNo(solved.records.mayRunUnknownJavaScript()));
}

static void dumpBuiltinUses(FlowPrinter& printer, const FlowSolvedProgram& solved)
{
    for (uint32_t index = 0; index < solved.program.builtins().builtinCount(); ++index) {
        BuiltinID builtin(index);
        const FlowBuiltinUse& use = solved.records.builtinUse(builtin);
        if (!use.uses.isEmpty())
            printer.println("use "_s, builtin, " "_s, use.uses, " "_s, use.firstUse);
    }
}

static void dumpBuiltinCallStatements(FlowPrinter& printer, const FlowSolvedProgram& solved)
{
    for (const FlowBuiltinCallStatement& statement : solved.states.calls->builtins.statements) {
        const FlowBuiltinCallKey& key = statement.key;
        printer.print("statement "_s, key.summaryStatement.instance, " "_s, key.origin, " "_s);
        if (key.callee.boundFunction)
            solved.objects->dump(printer.out(), key.callee.boundFunction);
        else if (key.callee.values) {
            dumpNode(printer, solved, key.callee.values);
            printer.print(key.callee.protocols.isEmpty() ? ""_s : " protocols"_s);
        }
        else
            printer.print(key.callee.builtin, " "_s, enumName(key.callee.mode));
        printer.print(" "_s, key.kind, " "_s, enumName(key.times), key.nodes == FlowBuiltinCallNodes::Own ? " own-nodes"_s : ""_s);
        printer.println(statement.isAbandoned ? " abandoned"_s : ""_s);
    }
}

static constexpr LinkSection linkSections[] = {
    { "calls"_s, dumpCallLinks },
    { "operands"_s, dumpOperandFlows },
    { "builtins"_s, dumpBuiltinCallLinks },
    { "unknown"_s, dumpUnknownCallees },
    { "modules"_s, dumpModules },
    { "unseen"_s, dumpUnseenCode },
    { "uses"_s, dumpBuiltinUses },
    { "statements"_s, dumpBuiltinCallStatements },
};

void runFlowCallLinksTestCommand(TestCommandContext& context)
{
    BitVector selectedSections;
    for (const String& word : context.arguments) {
        auto section = std::ranges::find_if(linkSections, [&](const LinkSection& candidate) { return word == candidate.word; });
        if (section == std::end(linkSections))
            return printBadArgument(context.out, word);
        selectedSections.set(section - std::begin(linkSections));
    }

    std::unique_ptr<FlowSolvedProgram> solved = solveFlow(*context.program, context.program->types, context.build);
    FlowPrinter printer(context.out, *context.program);
    for (size_t index = 0; index < std::size(linkSections); ++index) {
        if (context.arguments.empty() || selectedSections.get(index))
            linkSections[index].dump(printer, *solved);
    }
}

static void dumpModuleOrder(FlowPrinter& printer, const FlowSolvedProgram& solved, std::span<const ModuleID>)
{
    solved.states.calls->order.modules.dump(printer.out());
}

template<ScopeOrderLines lines>
static void dumpScopeOrders(FlowPrinter& printer, const FlowSolvedProgram& solved, std::span<const ModuleID>)
{
    StringPrintStream stream;
    solved.states.calls->order.scopes.dump(stream);
    String text = stream.toString();
    for (StringView line : StringView(text).split('\n')) {
        bool isTimeline = line.startsWith("timeline "_s);
        if (isTimeline == (lines == ScopeOrderLines::Timelines))
            printer.out().println(line);
    }
}

static void dumpFailedLocations(FlowPrinter& printer, const FlowSolvedProgram& solved, std::span<const ModuleID>)
{
    const FlowOrderState& order = solved.states.calls->order;
    const FlowScopeLocationNumbering& numbering = order.scopes.numbering;
    const BitVector& failedLocations = order.locationsReadBeforeInitialization;

    for (uint32_t index = 0; index < solved.program.variables.size(); ++index) {
        VariableID variable(index);
        std::optional<uint32_t> location = numbering.locationOf(variable);
        if (location && failedLocations.get(*location))
            printer.println("read-before-initialization "_s, variable);
    }
    for (uint32_t index = 0; index < solved.program.layouts.size(); ++index) {
        LayoutID layout(index);
        for (uint32_t field = 0; auto location = numbering.locationOf(layout, field); ++field) {
            if (failedLocations.get(*location))
                printer.println("read-before-initialization "_s, layout, "."_s, field);
        }
    }
}

static void dumpCacheEntryState(FlowPrinter& printer, const FlowSolvedProgram& solved, std::span<const ModuleID> modules)
{
    printer.println("cache "_s, modules[0], " "_s, enumName(cacheEntryStateOf(solved.solution(), modules[0])));
}

static void dumpModuleOrdering(FlowPrinter& printer, const FlowSolvedProgram& solved, std::span<const ModuleID> modules)
{
    printer.println("before "_s, modules[0], " "_s, modules[1], " "_s, enumName(moduleOrderingOf(solved.solution(), modules[0], modules[1])));
}

static constexpr OrderSection orderSections[] = {
    { "modules"_s, 0, dumpModuleOrder },
    { "timelines"_s, 0, dumpScopeOrders<ScopeOrderLines::Timelines> },
    { "locations"_s, 0, dumpScopeOrders<ScopeOrderLines::Locations> },
    { "failed"_s, 0, dumpFailedLocations },
    { "cache"_s, 1, dumpCacheEntryState },
    { "before"_s, 2, dumpModuleOrdering },
};

void runFlowOrderTestCommand(TestCommandContext& context)
{
    const Program& program = *context.program;
    std::unique_ptr<FlowSolvedProgram> solved = solveFlow(program, context.program->types, context.build);
    FlowPrinter printer(context.out, program);

    if (context.arguments.empty()) {
        for (const OrderSection& section : orderSections) {
            if (!section.numberOfModules)
                section.dump(printer, *solved, { });
        }
        return;
    }

    auto parseModule = idParser<ModuleID>('m', program.modules.size());
    std::span<const String> words = context.arguments;
    while (!words.empty()) {
        const String& word = words[0];
        auto section = std::ranges::find_if(orderSections, [&](const OrderSection& candidate) { return word == candidate.word; });
        if (section == std::end(orderSections))
            return printBadArgument(context.out, word);
        words = words.subspan(1);
        if (words.size() < section->numberOfModules)
            return printMissingArgument(context.out);

        Vector<ModuleID, 2> modules;
        for (const String& moduleWord : words.first(section->numberOfModules)) {
            std::optional<ModuleID> parsedModule = parseModule(moduleWord);
            if (!parsedModule)
                return printBadArgument(context.out, moduleWord);
            modules.append(*parsedModule);
        }
        words = words.subspan(section->numberOfModules);
        section->dump(printer, *solved, modules.span());
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

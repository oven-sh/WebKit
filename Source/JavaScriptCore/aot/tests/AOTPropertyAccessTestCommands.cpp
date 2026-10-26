/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTIRPrinter.h"
#include "AOTPropertyAccessElements.h"
#include "AOTResolveAccesses.h"
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ListedValue {
    FunctionID functionID;
    const Function& function;
    FunctionAnalyses& analyses;
    ValueID id;

    const Value& value() const { return function.at(id); }
    void dump(PrintStream& out) const { out.print("F"_s, functionID.index(), " v"_s, id.index()); }
};

} // anonymous namespace

static void runOnListedValues(TestCommandContext& context, NOESCAPE const Invocable<void(const ListedValue&)> auto& functor)
{
    if (!CommandWords(context).finish())
        return;
    const Program& program = *context.program;
    for (size_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody())
            continue;
        FunctionID id(index);
        FunctionAnalyses analyses(AnalysisContext { context.build, program, nullptr, nullptr, id }, function);
        for (const Block& block : function.blocks) {
            for (ValueID value : block.values)
                functor(ListedValue { id, function, analyses, value });
        }
    }
}

static auto layoutParser(const Program& program)
{
    return [&program](StringView text) {
        size_t numberOfLayouts = program.layouts.size();
        std::optional<LayoutID> layout = idParser<LayoutID>('L', numberOfLayouts)(text);
        return layout ? layout : printedIDParser<LayoutID>(numberOfLayouts, [&](PrintStream& out, LayoutID id) { IRPrinter(out, program).print(id); })(text);
    };
}

static auto keyParser(const Program& program)
{
    return printedIDParser<PropertyKeyID>(program.types.numberOfPropertyKeys(), [&program](PrintStream& out, PropertyKeyID key) { printPropertyKey(out, program, key); });
}

static auto bracketedListParser(auto parseItem)
{
    using List = Vector<ParsedType<decltype(parseItem)>>;
    return [parseItems = listParser(parseItem)](StringView text) -> std::optional<List> {
        if (text.length() < 2 || !text.startsWith('[') || !text.endsWith(']'))
            return std::nullopt;
        return parseItems(text.substring(1, text.length() - 2));
    };
}

static auto accessParser() { return filterParser(parseEnum<Access>, [](Access access) { return access != Access::None; }); }

static std::optional<MemberStart> parseMemberStart(StringView text)
{
    constexpr auto names = enumNames<MemberStart>();
    for (size_t index = 0; index < names.size(); ++index) {
        if (!names[index].isNull() && text == names[index])
            return static_cast<MemberStart>(index);
    }
    return std::nullopt;
}

static void runPropertyAccess(TestCommandContext& context)
{
    const Program& program = *context.program;
    runOnListedValues(context, [&](const ListedValue& listed) {
        const Value& access = listed.value();
        std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(program, listed.function, access, ExpectedClassObjects::Tested);
        if (!query)
            return;
        query->maxMembers = context.build.limit(LimitID::MaxAccessPlanMembers);
        if (access.opcode == Opcode::PropGet)
            query->isOnlyCalled = isOnlyCalled(listed.analyses, listed.id);
        context.out.println(listed, " "_s, nameOf(access.opcode), " K"_s, query->key.index(), ": "_s, planPropertyAccess({ program, nullptr }, *query));
    });
}

static void runMemberLookup(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    MemberQuery query;
    query.layout = words.parseRequired("layout"_s, layoutParser(program));
    query.key = words.parseRequired("key"_s, keyParser(program));
    query.start = words.parseOptional("start"_s, parseMemberStart).value_or(MemberStart::Instance);
    query.access = words.parseOptional("access"_s, accessParser()).value_or(Access::Read);
    query.isOwnLookup = words.parseFlag("own"_s);
    if (words.finish())
        context.out.println(findMember({ program, nullptr }, query));
}

static void runPartitionMembers(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    Vector<LayoutID> layouts = words.parseRequired("layouts"_s, bracketedListParser(layoutParser(program)));
    PropertyKeyID key = words.parseRequired("key"_s, keyParser(program));
    Access access = words.parseOptional("access"_s, accessParser()).value_or(Access::Read);
    if (!words.finish())
        return;
    LayoutList candidates(layouts.span());
    context.out.println(partitionMembers({ program, nullptr }, candidates.span(), key, access));
}

static void runLayoutsWithNames(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    Vector<PropertyKeyID> names = words.parseRequired("names"_s, bracketedListParser(keyParser(program)));
    if (!words.finish())
        return;
    CommaPrinter comma;
    context.out.print("["_s);
    for (LayoutID layout : allocatedLayoutsWith(program, names.span()))
        context.out.print(comma, "L"_s, layout.index());
    context.out.println("]"_s);
}

static void runReceiverCandidates(TestCommandContext& context)
{
    runOnListedValues(context, [&](const ListedValue& listed) {
        const Value& access = listed.value();
        if (!isNamedAccess(access.opcode))
            return;
        ValueID receiver = access.operand(*receiverOf(access.opcode));
        LayoutSetID stamp = PayloadWord::receivers(access.opcode) ? access.receivers() : LayoutSetID();
        context.out.println(listed, ": "_s, receiverCandidates(*context.program, listed.function, receiver, stamp));
    });
}

static void runUnionParts(TestCommandContext& context)
{
    const Program& program = *context.program;
    PrintStream& out = context.out;
    StringID testedFunctionName = program.strings.find(byteCast<uint8_t>("parts"_s.span()));
    runOnListedValues(context, [&](const ListedValue& listed) {
        if (!testedFunctionName || listed.function.name != testedFunctionName || listed.value().opcode != Opcode::Upcast)
            return;
        UnionParts parts = unionPartsOf(program.at(listed.value().type));
        out.print("v"_s, listed.id.index(), ": "_s);
        CommaPrinter comma;
        for (const UnionPart& part : parts) {
            out.print(comma, enumName(part.kind), "("_s);
            printType(out, program, part.type);
            out.print(")"_s);
        }
        if (isClosedUnion(parts))
            out.print(" closed"_s);
        out.println();
    });
}

static void runElementAccess(TestCommandContext& context)
{
    runOnListedValues(context, [&](const ListedValue& listed) {
        const Value& access = listed.value();
        if (!elementAccessOf(access.opcode) && access.opcode != Opcode::ElemDelete)
            return;
        ElementAccessPlan plan = planElementAccessOf(*context.program, listed.function, access).value_or(ElementAccessPlan());
        context.out.println(listed, " "_s, nameOf(access.opcode), ": "_s, plan);
    });
}

static void printProvenAccess(IRPrinter& printer, const ProvenAccess& access)
{
    PrintStream& out = printer.out();
    if (!access.isComplete) {
        out.print("unknown"_s);
        return;
    }

    auto printList = [&](ASCIILiteral name, const auto& items, const auto& printItem) {
        if (items.isEmpty())
            return;
        out.print(" "_s, name, "="_s);
        printer.list(items, printItem);
    };
    auto printFunction = [&](FunctionID function) { out.print("F"_s, function.index()); };

    out.print("complete"_s);
    printList("operations"_s, access.operations, [&](Opcode operation) { printer.print(operation); });
    printList("accessors"_s, access.accessors, printFunction);
    printList("methods"_s, access.methods, printFunction);
    printList("builtins"_s, access.builtins, [&](BuiltinID builtin) { printer.print(builtin); });
    if (access.mayFindNothing)
        out.print(" nothing"_s);
    if (access.isLookupByKey)
        out.print(" lookupByKey"_s);
    if (access.mayFindPrototypeProperty)
        out.print(" prototypeProperty"_s);
    if (access.definesOwnProperty)
        out.print(" definesOwnProperty"_s);
    if (access.key)
        out.print(" key=K"_s, access.key.index());
    if (access.receivers)
        out.print(" receivers=S"_s, access.receivers.index());
}

static void runProvenAccess(TestCommandContext& context)
{
    IRPrinter printer(context.out, *context.program);
    runOnListedValues(context, [&](const ListedValue& listed) {
        const Value& access = listed.value();
        if (effectRuleOf(access.opcode) != EffectRule::Access)
            return;
        context.out.print(listed, " "_s, nameOf(access.opcode), ": "_s);
        printProvenAccess(printer, provenAccessOf(*context.program, listed.function, access));
        context.out.println();
    });
}

std::span<const TestCommand> propertyAccessTestCommands()
{
    static constexpr TestCommand commands[] {
        { "property-access"_s, IRForm::Prepared, runPropertyAccess, ProgramAsRead::TypedAndVerified },
        { "member-lookup"_s, IRForm::Prepared, runMemberLookup, ProgramAsRead::TypedAndVerified },
        { "partition-members"_s, IRForm::Prepared, runPartitionMembers, ProgramAsRead::TypedAndVerified },
        { "layouts-with-names"_s, IRForm::Prepared, runLayoutsWithNames, ProgramAsRead::TypedAndVerified },
        { "receiver-candidates"_s, IRForm::Prepared, runReceiverCandidates, ProgramAsRead::TypedAndVerified },
        { "union-parts"_s, IRForm::Prepared, runUnionParts, ProgramAsRead::TypedAndVerified },
        { "element-access"_s, IRForm::Prepared, runElementAccess, ProgramAsRead::TypedAndVerified },
        { "proven-access"_s, IRForm::Prepared, runProvenAccess, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

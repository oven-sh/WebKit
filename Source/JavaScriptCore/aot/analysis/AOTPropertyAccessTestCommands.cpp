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
#include "AOTVector.h"
#include <array>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>
#include <wtf/StringPrintStream.h>

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

struct KeyText {
    const Program& program;
    PropertyKeyID key;

    void dump(PrintStream& out) const { printPropertyKey(out, program, key); }
};

struct TypeText {
    const Program& program;
    const Type& type;

    void dump(PrintStream& out) const { printType(out, program, type); }
};

struct LayoutText {
    const Program& program;
    LayoutID layout;

    void dump(PrintStream& out) const { IRPrinter(out, program).print(layout); }
};

} // anonymous namespace

static void forEachListedValue(TestCommandContext& context, NOESCAPE const Invocable<void(const ListedValue&)> auto& functor)
{
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
    return [&program](StringView text) -> std::optional<LayoutID> {
        size_t numberOfLayouts = program.layouts.size();
        if (std::optional<LayoutID> layout = idParser<LayoutID>('L', numberOfLayouts)(text))
            return layout;
        for (size_t index = 0; index < numberOfLayouts; ++index) {
            LayoutID layout(index);
            if (text == toString(LayoutText { program, layout }))
                return layout;
        }
        return std::nullopt;
    };
}

static auto layoutListParser(const Program& program)
{
    return [parseLayouts = listParser(layoutParser(program))](StringView text) -> std::optional<Vector<LayoutID>> {
        if (text.length() < 2 || !text.startsWith('[') || !text.endsWith(']'))
            return std::nullopt;
        return parseLayouts(text.substring(1, text.length() - 2));
    };
}

static auto keyParser(const Program& program)
{
    return [&program](StringView text) -> std::optional<PropertyKeyID> {
        unsigned numberOfKeys = program.types.numberOfPropertyKeys();
        if (std::optional<PropertyKeyID> key = idParser<PropertyKeyID>('K', numberOfKeys)(text))
            return key;
        for (unsigned index = 0; index < numberOfKeys; ++index) {
            PropertyKeyID key(index);
            if (text == toString(KeyText { program, key }))
                return key;
        }
        return std::nullopt;
    };
}

static auto keyListParser(const Program& program)
{
    return [parseKeys = listParser(keyParser(program))](StringView text) -> std::optional<Vector<PropertyKeyID>> {
        if (text.length() < 2 || !text.startsWith('[') || !text.endsWith(']'))
            return std::nullopt;
        return parseKeys(text.substring(1, text.length() - 2));
    };
}

static auto accessParser()
{
    return filterParser(parseEnum<Access>, [](Access access) { return access != Access::None; });
}

template<typename E>
static auto enumNameParser()
{
    return [](StringView text) -> std::optional<E> {
        constexpr auto names = enumNames<E>();
        for (size_t index = 0; index < names.size(); ++index) {
            if (!names[index].isNull() && text == names[index])
                return static_cast<E>(index);
        }
        return std::nullopt;
    };
}

static void printPlan(TestCommandContext& context, const ListedValue& listed)
{
    const Program& program = *context.program;
    const Value& access = listed.value();
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(program, listed.function, access, ExpectedClassObjects::Tested);
    if (!query)
        return;
    query->maxMembers = context.build.limit(LimitID::MaxAccessPlanMembers);
    if (access.opcode == Opcode::PropGet)
        query->isOnlyCalled = isOnlyCalled(listed.analyses, listed.id);
    context.out.println(listed, " "_s, nameOf(access.opcode), " K"_s, query->key.index(), ": "_s, planPropertyAccess({ program, nullptr }, *query));
}

static void runPropertyAccess(TestCommandContext& context)
{
    if (!CommandWords(context).finish())
        return;
    forEachListedValue(context, [&](const ListedValue& listed) {
        printPlan(context, listed);
    });
}

static void runMemberLookup(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    MemberQuery query;
    query.layout = words.parseRequired("layout"_s, layoutParser(program));
    query.key = words.parseRequired("key"_s, keyParser(program));
    query.start = words.parseOptional("start"_s, enumNameParser<MemberStart>()).value_or(MemberStart::Instance);
    query.access = words.parseOptional("access"_s, accessParser()).value_or(Access::Read);
    query.isOwnLookup = words.parseFlag("own"_s);
    if (!words.finish())
        return;
    context.out.println(findMember({ program, nullptr }, query));
}

static void runPartitionMembers(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    Vector<LayoutID> layouts = words.parseRequired("layouts"_s, layoutListParser(program));
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
    Vector<PropertyKeyID> names = words.parseRequired("names"_s, keyListParser(program));
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
    if (!CommandWords(context).finish())
        return;
    const Program& program = *context.program;
    forEachListedValue(context, [&](const ListedValue& listed) {
        const Value& access = listed.value();
        if (!isNamedAccess(access.opcode))
            return;
        ValueID receiver = access.operand(*receiverOf(access.opcode));
        LayoutSetID stamp = PayloadWord::receivers(access.opcode) ? access.receivers() : LayoutSetID();
        context.out.println(listed, ": "_s, receiverCandidates(program, listed.function, receiver, stamp));
    });
}

static void printUnionParts(PrintStream& out, const Program& program, const ListedValue& listed)
{
    UnionParts parts = unionPartsOf(program.at(listed.value().type));
    out.print("v"_s, listed.id.index(), ": "_s);
    CommaPrinter comma;
    for (const UnionPart& part : parts)
        out.print(comma, enumName(part.kind), "("_s, TypeText { program, part.type }, ")"_s);
    if (isClosedUnion(parts))
        out.print(" closed"_s);
    out.println();
}

static void runUnionParts(TestCommandContext& context)
{
    if (!CommandWords(context).finish())
        return;
    const Program& program = *context.program;
    StringID testedFunctionName = program.strings.find(byteCast<uint8_t>("parts"_s.span()));
    if (!testedFunctionName)
        return;
    forEachListedValue(context, [&](const ListedValue& listed) {
        if (listed.function.name == testedFunctionName && listed.value().opcode == Opcode::Upcast)
            printUnionParts(context.out, program, listed);
    });
}

static void runElementAccess(TestCommandContext& context)
{
    if (!CommandWords(context).finish())
        return;
    const Program& program = *context.program;
    forEachListedValue(context, [&](const ListedValue& listed) {
        const Value& access = listed.value();
        if (!elementAccessOf(access.opcode) && access.opcode != Opcode::ElemDelete)
            return;
        ElementAccessPlan plan = planElementAccessOf(program, listed.function, access).value_or(ElementAccessPlan());
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
    if (!CommandWords(context).finish())
        return;
    const Program& program = *context.program;
    IRPrinter printer(context.out, program);
    forEachListedValue(context, [&](const ListedValue& listed) {
        const Value& access = listed.value();
        if (effectRuleOf(access.opcode) != EffectRule::Access)
            return;
        context.out.print(listed, " "_s, nameOf(access.opcode), ": "_s);
        printProvenAccess(printer, provenAccessOf(program, listed.function, access));
        context.out.println();
    });
}

std::span<const TestCommand> propertyAccessTestCommands()
{
    static constexpr std::array commands {
        TestCommand { "property-access"_s, IRForm::Prepared, runPropertyAccess, ProgramAsRead::TypedAndVerified },
        TestCommand { "member-lookup"_s, IRForm::Prepared, runMemberLookup, ProgramAsRead::TypedAndVerified },
        TestCommand { "partition-members"_s, IRForm::Prepared, runPartitionMembers, ProgramAsRead::TypedAndVerified },
        TestCommand { "layouts-with-names"_s, IRForm::Prepared, runLayoutsWithNames, ProgramAsRead::TypedAndVerified },
        TestCommand { "receiver-candidates"_s, IRForm::Prepared, runReceiverCandidates, ProgramAsRead::TypedAndVerified },
        TestCommand { "union-parts"_s, IRForm::Prepared, runUnionParts, ProgramAsRead::TypedAndVerified },
        TestCommand { "element-access"_s, IRForm::Prepared, runElementAccess, ProgramAsRead::TypedAndVerified },
        TestCommand { "proven-access"_s, IRForm::Prepared, runProvenAccess, ProgramAsRead::TypedAndVerified },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

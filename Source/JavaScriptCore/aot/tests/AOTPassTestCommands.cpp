/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTestCommand.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTIRPrinter.h"
#include "AOTIntegerRelations.h"
#include "AOTKnownValue.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTStringArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <utility>
#include <wtf/EnumTraits.h>
#include <wtf/HexNumber.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Assumption = std::pair<ValueID, KnownValue>;

struct KnownValueText {
    void dump(PrintStream&) const;

    const Program& program;
    KnownValue value;
};

struct IntegerQuery {
    enum class Kind : uint8_t { AtMost, AtMostConstant, AtLeast, WholeNumber };

    Kind kind;
    ValueID value;
    ValueID right { };
    int64_t number { 0 };
    BlockID at { };
};

} // anonymous namespace

static void forEachSelectedFunction(const Program& program, std::optional<FunctionID> onlyFunction, NOESCAPE const Invocable<void(FunctionID, const Function&)> auto& functor)
{
    for (size_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        const Function& function = program.at(id);
        if (function.hasBody() && (!onlyFunction || *onlyFunction == id))
            functor(id, function);
    }
}

static void printFunctionHeader(PrintStream& out, FunctionID id)
{
    out.println("function f", id.index());
}

void KnownValueText::dump(PrintStream& out) const
{
    out.print(enumName(value.kind()));
    switch (value.kind()) {
    case KnownValue::Kind::Unknown:
    case KnownValue::Kind::Undefined:
    case KnownValue::Kind::Null:
    case KnownValue::Kind::Object:
    case KnownValue::Kind::Function:
        return;
    case KnownValue::Kind::Boolean:
        out.print(" ", value.asBoolean() ? "true" : "false");
        return;
    case KnownValue::Kind::Integer:
        out.print(" ", value.asInteger());
        return;
    case KnownValue::Kind::Double:
        out.print(" 0x", hex(std::bit_cast<uint64_t>(value.asNumber()), 16, Lowercase));
        return;
    case KnownValue::Kind::String:
        out.print(" ");
        printString(out, program, value.asString());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<KnownValue> parseConstant(StringView word)
{
    if (word == "undefined"_s)
        return KnownValue::undefined();
    if (word == "null"_s)
        return KnownValue::null();
    if (word == "true"_s)
        return KnownValue::boolean(true);
    if (word == "false"_s)
        return KnownValue::boolean(false);
    if (word == "object"_s)
        return KnownValue::object();
    if (word == "function"_s)
        return KnownValue::function();
    if (std::optional<int64_t> integer = parseTestNumber<int64_t>(word))
        return KnownValue::integer(*integer);
    return std::nullopt;
}

static size_t smallestNumberOfValues(const Program& program, std::optional<FunctionID> onlyFunction)
{
    size_t result = std::numeric_limits<size_t>::max();
    forEachSelectedFunction(program, onlyFunction, [&](FunctionID, const Function& function) {
        result = std::min(result, function.values.size());
    });
    return result;
}

static void printSwitchCase(PrintStream& out, ValueID id, KnownValue value)
{
    out.print("v", id.index(), " switch case: ");
    if (std::optional<int64_t> switchCase = switchCaseOf(value))
        out.println(*switchCase);
    else
        out.println("none");
}

static void printKnownValues(PrintStream& out, const Program& program, StringArena& strings, const Function& function, const Vector<Assumption>& assumptions)
{
    Vector<KnownValue> evaluated(function.values.size());
    Vector<std::optional<KnownValue>> assumed(function.values.size());
    for (const auto& [value, constant] : assumptions)
        assumed[value.index()] = constant;

    auto operandValue = [&](ValueID operand) {
        if (assumed[operand.index()])
            return *assumed[operand.index()];
        if (evaluated[operand.index()].isKnown())
            return evaluated[operand.index()];
        return knownValueOf(program, function, operand);
    };
    auto printLine = [&](ValueID id, ASCIILiteral property, KnownValue value) {
        out.println("v", id.index(), " ", property, ": ", KnownValueText { program, value });
    };

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            KnownValue known = knownValueOf(program, function, id);
            KnownValue result = evaluateWithKnownOperands(program, function, value, operandValue);
            printLine(id, "known"_s, known);
            printLine(id, "evaluated"_s, result);
            if (opcodesCreatingKnownStrings.contains(value.opcode)) {
                KnownValue text = evaluateStringOperation(program, strings, function, value, operandValue);
                printLine(id, "string"_s, text);
                if (!result.isKnown())
                    result = text;
            }
            evaluated[id.index()] = result;

            KnownValue best = result.isKnown() ? result : known;
            out.println("v", id.index(), " truthy: ", enumName(best.toBoolean(program)));
            printSwitchCase(out, id, best);
        }
    }
}

static void runKnownValue(TestCommandContext& context)
{
    Program& program = *context.program;
    CommandWords words(context);
    std::optional<FunctionID> onlyFunction = words.parseOptional("function"_s, functionParser(program));
    if (words.hasFailed())
        return;

    auto parseAssumedValue = idParser<ValueID>('v', smallestNumberOfValues(program, onlyFunction));
    Vector<Assumption> assumptions = words.parseAll("assume"_s, pairParser(':', parseAssumedValue, parseConstant));
    if (!words.finish())
        return;

    StringArena strings(program.strings);
    forEachSelectedFunction(program, onlyFunction, [&](FunctionID id, const Function& function) {
        printFunctionHeader(context.out, id);
        printKnownValues(context.out, program, strings, function, assumptions);
    });
}

static std::optional<IntegerQuery::Kind> parseQueryKind(StringView key)
{
    if (key == "at-most"_s)
        return IntegerQuery::Kind::AtMost;
    if (key == "at-most-constant"_s)
        return IntegerQuery::Kind::AtMostConstant;
    if (key == "at-least"_s)
        return IntegerQuery::Kind::AtLeast;
    if (key == "whole"_s)
        return IntegerQuery::Kind::WholeNumber;
    return std::nullopt;
}

static std::optional<IntegerQuery> parseQuery(StringView word, const Function& function)
{
    size_t equalSignPosition = word.find('=');
    if (equalSignPosition == notFound)
        return std::nullopt;
    std::optional<IntegerQuery::Kind> kind = parseQueryKind(word.left(equalSignPosition));
    if (!kind)
        return std::nullopt;
    StringView fieldList = word.substring(equalSignPosition + 1);
    Vector<StringView, 4> fields;
    for (StringView field : fieldList.splitAllowingEmptyEntries(','))
        fields.append(field);

    auto parseValue = valueParser(function);
    auto parseBlock = blockParser(function);
    switch (*kind) {
    case IntegerQuery::Kind::AtMost: {
        if (fields.size() != 4)
            return std::nullopt;
        std::optional<ValueID> left = parseValue(fields[0]);
        std::optional<ValueID> right = parseValue(fields[1]);
        std::optional<int64_t> offset = parseTestNumber<int64_t>(fields[2]);
        std::optional<BlockID> at = parseBlock(fields[3]);
        if (!left || !right || !offset || !at)
            return std::nullopt;
        return IntegerQuery { .kind = *kind, .value = *left, .right = *right, .number = *offset, .at = *at };
    }
    case IntegerQuery::Kind::AtMostConstant:
    case IntegerQuery::Kind::AtLeast: {
        if (fields.size() != 3)
            return std::nullopt;
        std::optional<ValueID> value = parseValue(fields[0]);
        std::optional<int64_t> bound = parseTestNumber<int64_t>(fields[1]);
        std::optional<BlockID> at = parseBlock(fields[2]);
        if (!value || !bound || !at)
            return std::nullopt;
        return IntegerQuery { .kind = *kind, .value = *value, .number = *bound, .at = *at };
    }
    case IntegerQuery::Kind::WholeNumber: {
        if (fields.size() != 2)
            return std::nullopt;
        std::optional<ValueID> value = parseValue(fields[0]);
        std::optional<BlockID> at = parseBlock(fields[1]);
        if (!value || !at)
            return std::nullopt;
        return IntegerQuery { .kind = *kind, .value = *value, .at = *at };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void printAnswer(PrintStream& out, IntegerRelations& relations, const IntegerQuery& query)
{
    switch (query.kind) {
    case IntegerQuery::Kind::AtMost:
        out.println("v", query.value.index(), " <= v", query.right.index(), " + ", query.number, " at b", query.at.index(), ": ", enumName(relations.isAtMost(query.value, query.right, query.number, query.at)));
        return;
    case IntegerQuery::Kind::AtMostConstant:
        out.println("v", query.value.index(), " <= ", query.number, " at b", query.at.index(), ": ", enumName(relations.isAtMost(query.value, query.number, query.at)));
        return;
    case IntegerQuery::Kind::AtLeast:
        out.println("v", query.value.index(), " >= ", query.number, " at b", query.at.index(), ": ", enumName(relations.isAtLeast(query.value, query.number, query.at)));
        return;
    case IntegerQuery::Kind::WholeNumber:
        out.println("v", query.value.index(), " whole number at b", query.at.index(), ": ", enumName(relations.isWholeNumber(query.value, query.at)));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void runIntegerRelations(TestCommandContext& context)
{
    const Program& program = *context.program;
    CommandWords words(context);
    FunctionID id = words.parseRequired("function"_s, functionParser(program));
    if (words.hasFailed())
        return;

    const Function& function = program.at(id);
    Vector<IntegerQuery> queries = words.parseRemaining([&](StringView word) {
        return parseQuery(word, function);
    });
    if (!words.finish())
        return;

    FunctionAnalyses analyses(AnalysisContext { context.build, program, nullptr, nullptr, id }, function);
    IntegerRelations relations(analyses);
    printFunctionHeader(context.out, id);
    for (const IntegerQuery& query : queries)
        printAnswer(context.out, relations, query);
}

static void runBodiesWithoutCode(TestCommandContext& context)
{
    CommandWords words(context);
    if (!words.finish())
        return;
    for (size_t index : findBodiesWithoutCode(*context.program, context.build))
        context.out.println("f", index, " gets no code");
}

std::span<const TestCommand> passTestCommands()
{
    static constexpr TestCommand commands[] {
        { "known-value"_s, IRForm::Prepared, runKnownValue },
        { "integer-relations"_s, IRForm::Prepared, runIntegerRelations },
        { "bodies-without-code"_s, IRForm::Selected, runBodiesWithoutCode },
    };
    return commands;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

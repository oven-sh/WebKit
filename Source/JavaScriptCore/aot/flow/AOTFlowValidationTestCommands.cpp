/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowTestCommands.h"

#if ENABLE(AOT)

#include "AOTAnalysisTestCommands.h"
#include "AOTFlowAnalysis.h"
#include "AOTSupportTestCommands.h"
#include <algorithm>
#include <iterator>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ValueReference {
    FunctionID function;
    ValueID value;
};

struct DamageWord {
    ASCIILiteral name;
    bool (*apply)(Program&, StringView argument);
};

} // anonymous namespace

static std::optional<ValueReference> parseValueReference(const Program& program, StringView text)
{
    size_t colon = text.find(':');
    if (colon == notFound)
        return std::nullopt;
    std::optional<FunctionID> function = functionParser(program)(text.left(colon));
    if (!function)
        return std::nullopt;
    std::optional<ValueID> value = valueParser(program.at(*function))(text.substring(colon + 1));
    if (!value)
        return std::nullopt;
    return ValueReference { *function, *value };
}

static bool setMembers(Program& program, StringView argument)
{
    auto parseLayout = idParser<LayoutID>('L', program.layouts.size());
    auto parsed = pairParser(':', parseLayout, listParser(parseLayout))(argument);
    if (!parsed)
        return false;
    program.at(parsed->first).flow.members = WTF::move(parsed->second);
    return true;
}

static bool setKeepsShape(Program& program, StringView argument)
{
    std::optional<ValueReference> allocation = parseValueReference(program, argument);
    if (!allocation)
        return false;
    Value& value = program.at(allocation->function).at(allocation->value);
    if (!FlagMask::keepsShape(value.opcode))
        return false;
    value.setKeepsShape(true);
    return true;
}

static bool declareNever(Program& program, StringView argument)
{
    std::optional<VariableID> variable = idParser<VariableID>('g', program.variables.size())(argument);
    if (!variable)
        return false;
    program.at(*variable).declared.type = TypeTable::never;
    return true;
}

static bool unlinkVersion(Program& program, StringView argument)
{
    std::optional<FunctionID> version = idParser<FunctionID>('f', program.functions.size())(argument);
    if (!version || program.at(*version).role != FunctionRole::GenericVersion)
        return false;
    program.at(*version).derivation.source = FunctionID();
    return true;
}

static bool assumeVariableType(Program& program, StringView argument)
{
    size_t lastColon = argument.reverseFind(':');
    if (lastColon == notFound)
        return false;
    std::optional<ValueReference> cast = parseValueReference(program, argument.left(lastColon));
    std::optional<VariableID> variable = idParser<VariableID>('g', program.variables.size())(argument.substring(lastColon + 1));
    if (!cast || !variable)
        return false;

    Value& value = program.at(cast->function).at(cast->value);
    if (value.opcode != Opcode::Upcast && value.opcode != Opcode::Claim)
        return false;
    ValueID operand = value.valueOperand();
    value.become(Opcode::AssumeType);
    value.setValueOperand(operand);
    value.setCastType(program.at(*variable).declared.type);
    return true;
}

static bool damageProgram(Program& program, StringView word)
{
    static constexpr DamageWord damageWords[] = {
        { "members"_s, setMembers },
        { "keeps-shape"_s, setKeepsShape },
        { "declare-never"_s, declareNever },
        { "unlink-version"_s, unlinkVersion },
        { "assume"_s, assumeVariableType },
    };

    size_t equalsSign = word.find('=');
    if (equalsSign == notFound)
        return false;
    StringView name = word.left(equalsSign);
    auto damage = std::ranges::find_if(damageWords, [&](const DamageWord& entry) {
        return name == entry.name;
    });
    return damage != std::end(damageWords) && damage->apply(program, word.substring(equalsSign + 1));
}

void runFlowValidateTestCommand(TestCommandContext& context)
{
    if (context.arguments.size() > 1) {
        printBadArgument(context.out, context.arguments[1]);
        return;
    }

    Program& program = *context.program;
    FlowResults results = analyzeFlow(program, program.types, context.build);
    applyFlowResults(program, results, context.build);
    program.setForm(IRForm::Prepared);

    if (!context.arguments.empty() && !damageProgram(program, context.arguments[0])) {
        printBadArgument(context.out, context.arguments[0]);
        return;
    }
    validateFlowResults(program, results, context.build);
    context.out.println(context.build.diagnostics().hasErrors() ? "invalid"_s : "valid"_s);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFactsFeatureLines.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

bool isFeatureLineDecidedByOperandOfCall(const BuiltinFeatureUse& line)
{
    const BuiltinCondition* condition = line.condition;
    if (!condition)
        return false;
    if (condition->calledOn || condition->buildAdmitsRuntimeCode || condition->callRunsOtherCode || condition->objectNobodyKnowsIsWritten || condition->isAboutWhatProgramReplaces())
        return false;
    return !condition->isOneOf.empty() || !condition->startsWith.empty() || condition->hasProperty;
}

OperandOfCallInSight describeOperandOfCallInSight(const Program& program, const BodyValue& definition, NOESCAPE const DefinitionsInScanOfStrings& definitionOf)
{
    using Kind = OperandOfCallInSight::Kind;
    if (!definition || definition.isParameter())
        return { };
    if (definition.opcode() == Opcode::Undefined)
        return { Kind::IsLeftOut, { }, { } };
    if (definition.opcode() == Opcode::String) {
        StringID text = definition.as<Opcode::String>().string();
        return text ? OperandOfCallInSight { Kind::IsText, text, { } } : OperandOfCallInSight { };
    }
    if (StringID beginning = textThatValueBeginsWith(program, definition, definitionOf))
        return { Kind::BeginsWithText, beginning, { } };
    return { };
}

static bool beginsWithInFeatureLines(std::span<const uint8_t> text, std::span<const uint8_t> beginning)
{
    return beginning.size() <= text.size() && std::ranges::equal(beginning, text.first(beginning.size()));
}

static bool textMayFulfilInFeatureLines(const Program& program, const BuiltinCondition& condition, StringID known, bool isWholeText)
{
    std::span<const uint8_t> text = program.string(known);
    for (BuiltinName name : condition.isOneOf) {
        std::span<const uint8_t> listed = program.string(program.stringOf(name));
        if (isWholeText ? std::ranges::equal(text, listed) : beginsWithInFeatureLines(listed, text))
            return true;
    }
    for (BuiltinName name : condition.startsWith) {
        std::span<const uint8_t> listed = program.string(program.stringOf(name));
        if (beginsWithInFeatureLines(text, listed))
            return true;
        // What stands behind the beginning may bring the rest.
        if (!isWholeText && beginsWithInFeatureLines(listed, text))
            return true;
    }
    return false;
}

bool conditionOnOperandMayHold(const Program& program, const BuiltinCondition& condition, const OperandOfCallInSight& operand)
{
    bool asksForText = !condition.isOneOf.empty() || !condition.startsWith.empty();
    switch (operand.kind) {
    case OperandOfCallInSight::Kind::NobodyKnows:
        return true;
    case OperandOfCallInSight::Kind::IsLeftOut:
        return asksForText;
    case OperandOfCallInSight::Kind::IsText:
    case OperandOfCallInSight::Kind::BeginsWithText:
        // Nobody has said what a row that asks an object for a property makes of a text.
        if (condition.hasProperty)
            return true;
        return textMayFulfilInFeatureLines(program, condition, operand.text, operand.kind == OperandOfCallInSight::Kind::IsText);
    case OperandOfCallInSight::Kind::ListsItsKeys: {
        // Code of the program makes the text of an object.
        if (asksForText)
            return true;
        PropertyKeyID key = program.keyOf(condition.hasProperty);
        return key && std::ranges::contains(operand.keys, key);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorForms.h"

#if ENABLE(AOT)

#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConvertedOperator {
    bool needsConversion() const { return needsNumber[0] || needsNumber[1]; }

    SelectionGroup group;
    OperatorForm form;
    std::array<bool, 2> needsNumber;
};

} // anonymous namespace

static bool isNullableNumber(const Type& type)
{
    return type.isNumberOrNullish() || type.isUndefinedOrNull();
}

static ConvertedOperator findConversions(SelectionContext& context, ValueID id, OperatorForm form)
{
    ConvertedOperator match { groupOfOne(id), form, { false, false } };
    const Value& value = context.function().at(id);
    if (form != OperatorForm::Numbers || value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric)
        return match;

    for (uint32_t index = 0; index < value.numberOfOperands(); ++index)
        match.needsNumber[index] = !isNullableNumber(context.typeOf(value.operand(index)));
    return match;
}

static void convertOperands(GroupRewriter& rewriter, const ConvertedOperator& match)
{
    const Value& original = rewriter.original(match.group.root);
    uint32_t numberOfOperands = original.numberOfOperands();

    std::array<ValueID, 2> operands;
    for (uint32_t index = 0; index < numberOfOperands; ++index) {
        operands[index] = original.operand(index);
        if (!match.needsNumber[index])
            continue;
        Value conversion = IRBuilder::make(Opcode::ToNumber, { operands[index] });
        conversion.setForm(OperatorForm::NullableNumber);
        operands[index] = rewriter.insert(WTF::move(conversion));
    }

    Value& root = rewriter.replaceRoot(original.opcode);
    for (uint32_t index = 0; index < numberOfOperands; ++index)
        root.setOperand(index, operands[index]);
    root.setForm(match.form);
}

static constexpr size_t numberOfOpcodesWithOperatorForm = std::ranges::count_if(allMembersOf<Opcode>, hasOperatorForm);

static constexpr std::array<Opcode, numberOfOpcodesWithOperatorForm> opcodesWithOperatorForm = [] {
    std::array<Opcode, numberOfOpcodesWithOperatorForm> opcodes { };
    std::ranges::copy_if(allMembersOf<Opcode>, opcodes.begin(), hasOperatorForm);
    return opcodes;
}();

void selectOperatorForm(SelectionContext& context)
{
    TypingContext typing = context.typing();
    Function& function = context.pass().function;
    const EffectContext& effects = context.analyses().effectContext();
    uint32_t changed = 0;
    Vector<ConvertedOperator> converted;

    for (Opcode opcode : opcodesWithOperatorForm) {
        for (ValueID id : context.valuesWith(opcode)) {
            ValueLocation location = context.analyses().useLists().locationOf(id);
            if (location.placement != ValuePlacement::InBlock)
                continue;

            OperatorSite site { typing, effects, context.shapeOf(location.block) };
            OperatorForm form = chooseFormOfOperator(site, function.at(id));
            ConvertedOperator match = findConversions(context, id, form);
            if (match.needsConversion()) {
                if (!context.isConditionWithProofs(id)) {
                    converted.append(WTF::move(match));
                    continue;
                }
                form = OperatorForm::Generic;
            }

            if (function.at(id).form() == form)
                continue;
            function.edit(id, ValueEdit::Definition).setForm(form);
            ++changed;
        }
    }

    context.didSelectForms(SelectionRuleID::OperatorForm, changed);
    context.rewriteGroups(SelectionRuleID::OperatorForm, converted, convertOperands);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

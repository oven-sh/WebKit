/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorForms.h"

#if ENABLE(AOT)

#include "AOTKnownValue.h"
#include "AOTRepresentation.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConvertedOperator {
    bool needsConversion() const { return needsNumber[0] || needsNumber[1]; }

    SelectionGroup group;
    OperatorForm form;
    std::array<bool, 2> needsNumber;
};

struct TestOfStringLength {
    SelectionGroup group;
    ValueID string;
    bool holdsForEmptyString;
};

} // anonymous namespace

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

// There is one empty string, so what tells the length 0 from every other length tells that cell from every other. A string that has been opened has its length at hand.
static std::optional<TestOfStringLength> findTestOfStringLength(SelectionContext& context, ValueID id)
{
    const Function& function = context.function();
    const UseLists& useLists = context.analyses().useLists();
    const Value& test = function.at(id);
    ValueID length;
    ValueID string;
    test.forEachOperand([&](ValueID operand, uint32_t) {
        const Value& definition = function.at(definitionThroughUpcasts(function, operand));
        if (definition.opcode != Opcode::GetStringLength)
            return;
        length = operand;
        string = definition.stringOperand();
    });
    if (!length || context.isConditionWithProofs(id) || defaultRepresentationOf(context.typeOf(string)) != Representation::Cell || context.typeOf(string).isOpaque(OpaqueKind::FlatString))
        return std::nullopt;
    bool hasOtherReader = std::ranges::any_of(CastChain(function, length), [&](ValueID producer) {
        return useLists.useCount(producer) != 1;
    });
    if (hasOtherReader)
        return std::nullopt;

    auto answerFor = [&](int64_t sample) {
        return evaluateWithKnownOperands(context.program(), function, test, [&](ValueID operand) {
            return operand == length ? KnownValue::integer(sample) : knownValueOf(context.program(), function, operand);
        });
    };
    KnownValue forEmptyString = answerFor(0);
    KnownValue forOneUnit = answerFor(1);
    if (forEmptyString.kind() != KnownValue::Kind::Boolean || forOneUnit.kind() != KnownValue::Kind::Boolean)
        return std::nullopt;
    if (forEmptyString == forOneUnit || forOneUnit != answerFor(Range::length().maximum))
        return std::nullopt;
    return TestOfStringLength { groupOfOne(id), string, forEmptyString.asBoolean() };
}

static void testWhetherStringIsEmpty(GroupRewriter& rewriter, const TestOfStringLength& match)
{
    if (!match.holdsForEmptyString) {
        Value& root = rewriter.replaceRoot(Opcode::ToBoolean);
        root.setValueOperand(match.string);
        root.setForm(OperatorForm::Bits);
        return;
    }
    Value hasUnits = IRBuilder::make(Opcode::ToBoolean, { match.string });
    hasUnits.setForm(OperatorForm::Bits);
    ValueID inserted = rewriter.insert(WTF::move(hasUnits));
    rewriter.replaceRoot(Opcode::LogicalNot).setValueOperand(inserted);
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
    Vector<TestOfStringLength> testsOfStringLength;

    for (Opcode opcode : opcodesWithOperatorForm) {
        for (ValueID id : context.valuesWith(opcode)) {
            ValueLocation location = context.analyses().useLists().locationOf(id);
            if (location.placement != ValuePlacement::InBlock)
                continue;
            if (std::optional<TestOfStringLength> test = findTestOfStringLength(context, id)) {
                testsOfStringLength.append(WTF::move(*test));
                continue;
            }

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
    context.rewriteGroups(SelectionRuleID::OperatorForm, testsOfStringLength, testWhetherStringIsEmpty);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

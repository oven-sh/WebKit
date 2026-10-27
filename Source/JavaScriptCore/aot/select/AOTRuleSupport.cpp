/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuleSupport.h"

#if ENABLE(AOT)

#include "AOTEffectsOfRules.h"
#include "AOTKnownValue.h"

namespace JSC { namespace AOT { namespace Typed {

Value makeInteger(int64_t integer)
{
    Value value = IRBuilder::make(Opcode::Integer);
    value.setInteger(integer);
    return value;
}

Value makeResultAt(ValueID results, uint32_t index)
{
    Value result = IRBuilder::make(Opcode::ResultAt, { results });
    result.setIndex(index);
    return result;
}

void copyOperands(Value& target, uint32_t firstTargetOperand, const Value& source, uint32_t firstSourceOperand)
{
    RELEASE_ASSERT(firstTargetOperand <= target.numberOfOperands());
    uint32_t targetIndex = firstTargetOperand;
    for (uint32_t index = firstSourceOperand; index < source.numberOfOperands(); ++index, ++targetIndex) {
        if (targetIndex < target.numberOfOperands()) {
            target.setOperand(targetIndex, source.operand(index));
            continue;
        }
        RELEASE_ASSERT(hasVariadicOperand(target.opcode));
        target.appendVariadicOperand(source.operand(index));
    }
}

std::optional<StringID> stringConstantOf(const Program& program, const Function& function, ValueID id)
{
    std::optional<ValueID> constant = constantThroughCasts(program, function, id);
    if (!constant || function.at(*constant).opcode != Opcode::String)
        return std::nullopt;
    return function.at(*constant).string();
}

std::optional<int64_t> switchCaseOf(const Value& constant)
{
    if (constant.opcode == Opcode::Integer)
        return switchCaseOf(KnownValue::integer(constant.integer()));
    if (constant.opcode == Opcode::Double)
        return switchCaseOf(KnownValue::number(constant.number()));
    return std::nullopt;
}

std::optional<RegExpLiteralInType> singleRegExpLiteralAsCreatedOf(const Type& type)
{
    const ClassPart* part = type.singleBuiltinClass();
    if (!type.isObject() || !part || !part->provenPristine.contains(PristineAspects::AsCreated) || type.regExpLiterals().size() != 1)
        return std::nullopt;
    return type.regExpLiterals()[0];
}

// `kept <= kept`: what is dropped in one place is the subject in the other.
static bool namesProducerInSecondPlace(const Function& function, ValueID root, ValueID operand)
{
    unsigned numberOfPlaces = 0;
    function.at(root).forEachOperand([&](ValueID other, uint32_t) {
        for (ValueID producer : CastChain(function, operand)) {
            if (std::ranges::contains(CastChain(function, other), producer)) {
                ++numberOfPlaces;
                return;
            }
        }
    });
    return numberOfPlaces > 1;
}

static SelectionGroup groupWithProducersOf(SelectionContext& context, ValueID root, ValueID operand)
{
    const Function& function = context.function();
    SelectionGroup group = groupOfOne(root);
    if (namesProducerInSecondPlace(function, root, operand))
        return group;
    for (ValueID producer : CastChain(function, operand)) {
        if (function.at(producer).opcode == Opcode::Parameter || !context.analyses().isRemovableWhenUnused(producer))
            return groupOfOne(root);
        group.members.insert(0, GroupMember { .value = producer, .isUsedOutside = false, .isOnlyErased = true });
    }
    return group;
}

ClaimResult tryClaimWithoutOperand(SelectionContext& context, SelectionRuleID rule, SelectionGroup& group, ValueID droppedOperand)
{
    if (droppedOperand) {
        RELEASE_ASSERT(group.members.size() == 1);
        SelectionGroup groupWithProducers = groupWithProducersOf(context, group.root, droppedOperand);
        if (groupWithProducers.members.size() > 1 && context.tryClaim(rule, groupWithProducers) == ClaimResult::Claimed) {
            group = WTF::move(groupWithProducers);
            return ClaimResult::Claimed;
        }
    }
    return context.tryClaim(rule, group);
}

bool isOrdinaryKey(const Program& program, PropertyKeyID key)
{
    return program.at(key).kind == PropertyKeyKind::Text && program.nameClassOf(key) == NameClass::Ordinary;
}

bool hasEffectsOfUnknownCode(SelectionContext& context, ValueID id)
{
    FunctionAnalyses& analyses = context.analyses();
    return effectsOfUnknownCode(analyses.effectContext()).isSubsetOf(analyses.effectsOf(id));
}

const BuiltinOverload& overloadOf(const Program& program, const Value& builtinCall)
{
    RELEASE_ASSERT(Value::opcodesWithOverload.contains(builtinCall.opcode));
    return program.builtins().at(builtinCall.overload());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

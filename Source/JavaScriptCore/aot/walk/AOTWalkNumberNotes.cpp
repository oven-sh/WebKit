/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTFamilies.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <wtf/HashFunctions.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);
void prepareOperandsForFormNumbers(Walker&, Draft&);

bool hasMechanismsOfNumberForms(const Walker& walker)
{
    return walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::UnboxedNumbers);
}

PrimitiveKnownToFolding primitiveKnownInNumberForms(const Walker& walker, Reference reference)
{
    if (!reference || !reference.isRecord())
        return { };
    const Operation& record = walker.code().at(reference.id());
    if (record.opcode == Opcode::Double)
        return PrimitiveKnownToFolding::number(record.number());
    if (record.opcode == Opcode::String)
        return PrimitiveKnownToFolding::text(record.string());

    TypeID id = walker.typeOf(reference);
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (!type.isJavaScriptValue() || type.hasObjects())
        return { };
    switch (type.primitives) {
    case Primitive::Undefined:
        return PrimitiveKnownToFolding::undefined();
    case Primitive::Null:
        return PrimitiveKnownToFolding::null();
    case Primitive::False:
        return PrimitiveKnownToFolding::boolean(false);
    case Primitive::True:
        return PrimitiveKnownToFolding::boolean(true);
    case Primitive::Integer:
        return type.integers.isSingleValue() ? PrimitiveKnownToFolding::integer(type.integers.minimum) : PrimitiveKnownToFolding();
    case Primitive::LiteralString: {
        StringID literal = singleStringLiteralOf(type);
        return literal ? PrimitiveKnownToFolding::text(literal) : PrimitiveKnownToFolding();
    }
    default:
        return { };
    }
}

std::optional<int64_t> integerKnownInNumberForms(const Walker& walker, Reference reference)
{
    TypeID id = reference && reference.isRecord() ? walker.typeOf(reference) : TypeID();
    if (!id)
        return std::nullopt;
    Type type = walker.types().at(id);
    if (!type.isInteger() || !type.integers.isSingleValue())
        return std::nullopt;
    return type.integers.minimum;
}

Reference constantInNumberForms(Walker& walker, PrimitiveKnownToFolding primitive)
{
    Draft constant(Opcode::Undefined);
    switch (primitive.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        break;
    case PrimitiveKnownToFolding::Kind::Null:
        constant.become(Opcode::Null);
        break;
    case PrimitiveKnownToFolding::Kind::Boolean:
        constant.become(Opcode::Boolean);
        constant.setBoolean(primitive.asBoolean());
        break;
    case PrimitiveKnownToFolding::Kind::Number:
        if (isTrackedInteger(primitive.asNumber())) {
            constant.become(Opcode::Integer);
            constant.setInteger(static_cast<int64_t>(primitive.asNumber()));
        } else {
            constant.become(Opcode::Double);
            constant.setNumber(primitive.asNumber());
        }
        break;
    case PrimitiveKnownToFolding::Kind::Text:
        constant.become(Opcode::String);
        constant.setString(primitive.asText());
        break;
    case PrimitiveKnownToFolding::Kind::Unknown:
        return { };
    }
    return walker.constant(WTF::move(constant));
}

Reference integerInNumberForms(Walker& walker, int64_t integer)
{
    Draft constant(Opcode::Integer);
    constant.setInteger(integer);
    return walker.constant(WTF::move(constant));
}

Reference booleanInNumberForms(Walker& walker, bool boolean)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(boolean);
    return walker.constant(WTF::move(constant));
}

Reference buildInAllowedFormInNumberForms(Walker& walker, Draft&& draft)
{
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfNamedOperands(); ++operandIndex) {
        if (!draft.operand(operandIndex))
            return { };
    }
    constexpr OpcodeSet convertsOperandsToNumbers = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise });
    if (convertsOperandsToNumbers.contains(draft.opcode)) {
        bool isEachNumberLike = true;
        for (uint32_t operandIndex = 0; operandIndex < draft.numberOfNamedOperands(); ++operandIndex) {
            Type type = walker.types().at(walker.typeOf(draft.operand(operandIndex)));
            isEachNumberLike &= !type.isNever() && type.isNumberLike();
        }
        if (isEachNumberLike)
            prepareOperandsForFormNumbers(walker, draft);
        for (uint32_t operandIndex = 0; operandIndex < draft.numberOfNamedOperands(); ++operandIndex) {
            if (!draft.operand(operandIndex))
                return { };
        }
    }
    walker.chooseConservativeFields(draft);
    draft.setForm(formAllowedForTypedOperator(walker, draft));
    Origin origin = draft.origin();
    return boxedUnlessNumbersAreUnboxed(walker, walker.build(WTF::move(draft)), origin);
}

Reference buildOperatorInNumberForms(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    Draft draft(opcode, origin);
    draft.setOperand(0, left);
    if (right)
        draft.setOperand(1, right);
    return buildInAllowedFormInNumberForms(walker, WTF::move(draft));
}

std::optional<uint32_t> operandsNarrowedBehindTestInNumberForms(const Walker& walker, ValueID test)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isUnused(test))
        return 0;
    std::optional<BodyUser> user = index.onlyUser(test);
    if (!user)
        return std::nullopt;
    if (user->kind != BodyUseKind::TerminatorOperand)
        return 0;

    uint32_t operands = 0;
    for (BodyTarget target : index.terminator(user->block).targets()) {
        for (uint32_t operandIndex : target.proofs())
            operands |= 1u << (operandIndex & 31);
    }
    return operands;
}

void goOnWhereTestPassesInNumberForms(Walker& walker, Reference passes, BlockID otherwise)
{
    BlockID next = walker.addBlock();
    walker.branch(passes, next, otherwise);
    walker.continueIn(next);
}

Reference buildNarrowedInNumberForms(Walker& walker, Reference value, Reference test, uint32_t operandIndex, bool isOnTakenEdge, Origin origin)
{
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(value);
    proof.setTestOperand(test);
    proof.setOperandIndex(operandIndex);
    proof.setIsOnTakenEdge(isOnTakenEdge);
    return walker.build(WTF::move(proof));
}

static Reference addParameterForWaysInNumberForms(Walker& walker, BlockID meeting, std::span<const WayInNumberForms> ways, Reference WayInNumberForms::* brought)
{
    TypeID type;
    Vector<Representation, 4> forms;
    for (const WayInNumberForms& way : ways) {
        if (!way.end || !(way.*brought))
            continue;
        TypeID arriving = walker.code().at((way.*brought).id()).type;
        type = type ? joinOf(walker.types(), walker.facts().families(), type, arriving) : arriving;
        forms.append(walker.representationOf(way.*brought));
    }
    return type ? walker.addParameter(meeting, type, walker.formOfParameter(type, forms.span())) : Reference();
}

Reference meetInNumberForms(Walker& walker, BlockID meeting, std::span<const WayInNumberForms> ways, Reference* metBeside)
{
    bool someWayArrives = std::ranges::any_of(ways, [](const WayInNumberForms& way) {
        return !!way.end;
    });
    if (!someWayArrives)
        return { };

    Reference met = addParameterForWaysInNumberForms(walker, meeting, ways, &WayInNumberForms::brought);
    Reference beside = met ? addParameterForWaysInNumberForms(walker, meeting, ways, &WayInNumberForms::broughtBeside) : Reference();
    for (const WayInNumberForms& way : ways) {
        if (!way.end)
            continue;
        walker.continueIn(way.end);
        std::array<Reference, 2> arguments { way.brought, way.broughtBeside };
        walker.jumpTo(meeting, std::span<const Reference>(arguments).first(beside ? 2 : met ? 1 : 0));
    }
    walker.continueIn(meeting);
    if (metBeside)
        *metBeside = beside;
    return met;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

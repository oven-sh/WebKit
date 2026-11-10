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

OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);
void prepareOperandsForFormNumbers(Walker&, Draft&);

bool hasMechanismsOfNumberForms(const Walker& walker)
{
    return walker.reckonsOnWhatItHasBuilt() && walker.has(Mechanism::UnboxedNumbers);
}

static PrimitiveKnownToFolding onlyValueOfTypeInNumberForms(const Type& type)
{
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

PrimitiveKnownToFolding primitiveKnownInNumberForms(const Walker& walker, Reference reference, const Type& type)
{
    const Operation& record = walker.code().at(reference.id());
    if (record.opcode == Opcode::Double)
        return PrimitiveKnownToFolding::number(record.number());
    if (record.opcode == Opcode::String)
        return PrimitiveKnownToFolding::text(record.string());
    return onlyValueOfTypeInNumberForms(type);
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
    return onlyValueOfTypeInNumberForms(walker.types().at(id));
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
    draft.setForm(operatorFormFor(walker, draft));
    Origin origin = draft.origin();
    return boxNumberIfNeeded(walker, walker.build(WTF::move(draft)), origin);
}

Reference buildOperatorInNumberForms(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    Draft draft(opcode, origin);
    draft.setOperand(0, left);
    if (right)
        draft.setOperand(1, right);
    return buildInAllowedFormInNumberForms(walker, WTF::move(draft));
}

void continueIfTrue(Walker& walker, Reference passes, BlockID otherwise)
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

Reference meetInNumberForms(Walker& walker, BlockID meeting, std::span<const WayInNumberForms> ways, Reference* metBeside)
{
    auto isValue = [&](Reference brought) {
        return brought && walker.code().at(brought.id()).type;
    };
    Vector<Walker::JoinArm, 2> arms;
    size_t numberOfValues = 0;
    for (const WayInNumberForms& way : ways) {
        if (!way.end)
            continue;
        arms.append(Walker::JoinArm { way.end, { way.brought, way.broughtBeside } });
        numberOfValues = std::max<size_t>(numberOfValues, !isValue(way.brought) ? 0 : isValue(way.broughtBeside) ? 2 : 1);
    }
    if (arms.isEmpty())
        return { };

    std::array<Reference, 2> met;
    walker.joinArms(arms.span(), meeting, std::span<Reference>(met).first(numberOfValues));
    if (metBeside)
        *metBeside = met[1];
    return met[0];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

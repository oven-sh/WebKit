/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTTypeNarrowing.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesComparedQuietlyInBranches = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString | Primitive::BigInt;
static constexpr uint16_t primitivesInTotalOrderInBranches = Primitive::Null | Primitive::Boolean | Primitive::Integer | Primitive::FractionalNumber | Primitive::BigInt;

static ValueID valueBehindNamesInBranches(const Code& code, ValueID value)
{
    while (value && opcodesThatGiveSameValue.contains(code.at(value).opcode)) {
        ValueID source = code.at(value).valueOperand();
        if (source.index() >= value.index())
            return { };
        value = source;
    }
    return value;
}

static bool relationAdmitsEqualityInBranches(Opcode relation)
{
    return relation == Opcode::LessEq || relation == Opcode::GreaterEq;
}

static bool convertsQuietlyInBranches(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(primitivesComparedQuietlyInBranches);
}

std::optional<bool> answerOfRelationWithoutCompare(Walker& walker, const BodyValue& relation)
{
    Opcode opcode = relation.opcode();
    if (!allRelations.contains(opcode) || !walker.has(Mechanism::TypedOperations))
        return std::nullopt;
    Reference left = walker.referenceOf(relation.operand(0));
    Reference right = walker.referenceOf(relation.operand(1));
    if (!left || !right || !left.isRecord() || !right.isRecord())
        return std::nullopt;
    Type typeOfLeft = walker.types().at(walker.typeOf(left));
    Type typeOfRight = walker.types().at(walker.typeOf(right));
    if (!convertsQuietlyInBranches(typeOfLeft) || !convertsQuietlyInBranches(typeOfRight))
        return std::nullopt;

    const Code& code = walker.code();
    auto isAlwaysNaN = [&](Reference operand, const Type& type) {
        const Operation& record = code.at(operand.id());
        return (record.opcode == Opcode::Double && std::isnan(record.number())) || type.hasOnlyPrimitives(Primitive::Undefined);
    };
    if (isAlwaysNaN(left, typeOfLeft) || isAlwaysNaN(right, typeOfRight))
        return false;

    ValueID rootOfLeft = valueBehindNamesInBranches(code, left.id());
    ValueID rootOfRight = valueBehindNamesInBranches(code, right.id());
    if (!rootOfLeft || !rootOfRight)
        return std::nullopt;
    if (rootOfLeft != rootOfRight)
        return std::nullopt;
    if (!relationAdmitsEqualityInBranches(opcode))
        return false;
    if (typeOfLeft.hasOnlyPrimitives(primitivesInTotalOrderInBranches | Primitive::AnyString))
        return true;
    return std::nullopt;
}

bool Walker::endBlockInAnotherWay(const BodyTerminator& terminator)
{
    return terminator.kind() == TerminatorKind::Branch && endBlockWithSwitchOverAlternatives(terminator);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Branches and relations that need no test, for Folding with TypedOperations: a branch on a test whose operands have narrower names by now, a relation of a value with itself or with what compares as NaN, and a relation that a branch above has answered.
// At a suspension and at a throw: nothing. Each answer follows from values that are primitives, or from what a value is, and neither changes.
// Withheld: every branch keeps both sides, and every relation is compared.

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

// Each is converted without code, and none throws beside another of them.
static constexpr uint16_t primitivesComparedQuietlyInBranches = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString | Primitive::BigInt;
// None of them compares as NaN, so of any two of them one is less or they are equal. A string beside a number may say NaN.
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

static bool relationAsksForLesserOnLeftInBranches(Opcode relation)
{
    return relation == Opcode::Less || relation == Opcode::LessEq;
}

static bool convertsQuietlyInBranches(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(primitivesComparedQuietlyInBranches);
}

static uint64_t keyOfRelationInBranches(ValueID lesser, ValueID greater)
{
    return static_cast<uint64_t>(lesser.index()) << 32 | greater.index();
}

// ---- Relations

std::optional<bool> Walker::outcomeOfRelationAnsweredAbove(bool admitsEquality, ValueID lesser, ValueID greater) const
{
    const Frame& frame = m_frames.last();
    if (frame.answeredRelations.isEmpty() || !frame.index || !frame.block)
        return std::nullopt;
    auto found = frame.newestAnsweredRelations.find(keyOfRelationInBranches(lesser, greater));
    if (found == frame.newestAnsweredRelations.end())
        return std::nullopt;
    for (uint32_t place = found->value; place != UINT32_MAX; place = frame.answeredRelations[place].older) {
        const Frame::AnsweredRelation& answered = frame.answeredRelations[place];
        if (answered.admitsEquality == admitsEquality && frame.index->dominates(answered.block, frame.block))
            return answered.answer;
    }
    return std::nullopt;
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

    // `x < undefined`: what compares as NaN stands in no relation to anything.
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
    bool admitsEquality = relationAdmitsEqualityInBranches(opcode);
    // No value is less than itself, and each is at most itself unless it compares as NaN. Two strings are compared as strings.
    if (rootOfLeft == rootOfRight) {
        if (!admitsEquality)
            return false;
        if (typeOfLeft.hasOnlyPrimitives(primitivesInTotalOrderInBranches | Primitive::AnyString))
            return true;
        return std::nullopt;
    }

    ValueID lesser = relationAsksForLesserOnLeftInBranches(opcode) ? rootOfLeft : rootOfRight;
    ValueID greater = relationAsksForLesserOnLeftInBranches(opcode) ? rootOfRight : rootOfLeft;
    if (std::optional<bool> same = walker.outcomeOfRelationAnsweredAbove(admitsEquality, lesser, greater))
        return same;
    if (admitsEquality) {
        if (walker.outcomeOfRelationAnsweredAbove(false, lesser, greater) == true)
            return true;
    } else if (walker.outcomeOfRelationAnsweredAbove(true, lesser, greater) == false)
        return false;

    std::optional<bool> reverse = walker.outcomeOfRelationAnsweredAbove(false, greater, lesser);
    std::optional<bool> reverseOrEqual = walker.outcomeOfRelationAnsweredAbove(true, greater, lesser);
    if (reverse == true || (!admitsEquality && reverseOrEqual == true))
        return false;
    // A relation that does not hold says nothing of its reverse where either side may be NaN.
    bool areInTotalOrder = typeOfLeft.hasOnlyPrimitives(primitivesInTotalOrderInBranches) && typeOfRight.hasOnlyPrimitives(primitivesInTotalOrderInBranches);
    if (areInTotalOrder && (reverseOrEqual == false || (admitsEquality && reverse == false)))
        return true;
    return std::nullopt;
}

// ---- Branches

bool Walker::endBlockInAnotherWay(const BodyTerminator& terminator)
{
    return terminator.kind() == TerminatorKind::Branch && endBlockWithSwitchOverAlternatives(terminator);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

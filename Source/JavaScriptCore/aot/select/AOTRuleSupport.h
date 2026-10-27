/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRBuilder.h"
#include "AOTSelectionContext.h"
#include "AOTType.h"
#include "AOTValueIdentity.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

Value makeInteger(int64_t);
Value makeResultAt(ValueID results, uint32_t index);

void copyOperands(Value& target, uint32_t firstTargetOperand, const Value& source, uint32_t firstSourceOperand);

std::optional<StringID> stringConstantOf(const Program&, const Function&, ValueID);
std::optional<int64_t> switchCaseOf(const Value& constant);
// A type lists the literals that its objects were created from. Only an object that stays as created is known to have the pattern and the flags of its literal.
std::optional<RegExpLiteralInType> singleRegExpLiteralAsCreatedOf(const Type&);

// Claims the group of one root whose successor does not take the operand. The values that do nothing but produce the operand join the group, which erases them, where they have no other user.
ClaimResult tryClaimWithoutOperand(SelectionContext&, SelectionRuleID, SelectionGroup&, ValueID droppedOperand);

bool isOrdinaryKey(const Program&, PropertyKeyID);
bool hasEffectsOfUnknownCode(SelectionContext&, ValueID);

const BuiltinOverload& overloadOf(const Program&, const Value& builtinCall);

inline bool isOwnedByReturnCall(SelectionContext& context, ValueID id)
{
    BlockID block = context.analyses().useLists().blockOf(id);
    if (!block)
        return false;
    const Terminator& terminator = context.function().at(block).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalkLinks.h"
#include "AOTPlan.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

bool foldComparisonOfValueWithItself(Walker&, const BodyValue& comparison);
bool assignSourcesFieldByField(Walker&, const BodyValue& call);
inline constexpr OpcodeSet opcodesThatBuildCopyOrKeyOrderFormTakes { Opcode::StrictEq, Opcode::CopyObject, Opcode::Call };
bool buildCopyOrKeyOrderFormOfItsOpcodes(Walker&, const BodyValue&);
ALWAYS_INLINE bool buildCopyOrKeyOrderForm(Walker& walker, const BodyValue& value) { return opcodesThatBuildCopyOrKeyOrderFormTakes.contains(value.opcode()) && buildCopyOrKeyOrderFormOfItsOpcodes(walker, value); }
template<Opcode known> ALWAYS_INLINE bool buildCopyOrKeyOrderForm(Walker& walker, const BodyValueOf<known>& value)
{
    if constexpr (known == Opcode::StrictEq)
        return foldComparisonOfValueWithItself(walker, value);
    else if constexpr (known == Opcode::Call)
        return value.template as<Opcode::Call>().argumentsOperands().size() >= 2 && assignSourcesFieldByField(walker, value);
    else
        return opcodesThatBuildCopyOrKeyOrderFormTakes.contains(known) && buildCopyOrKeyOrderFormOfItsOpcodes(walker, value);
}
inline constexpr OpcodeSet opcodesThatOpenGuessOfKeyOrdersTakes { Opcode::PropGet, Opcode::PropSet };
AOT_LINK_OF_WALK(openGuessOfKeyOrders, opcodesThatOpenGuessOfKeyOrdersTakes)
inline constexpr OpcodeSet opcodesThatCallProvenAccessorTakes { Opcode::PropGet, Opcode::PropSet };
bool callProvenAccessorOfItsOpcodes(Walker&, const BodyValue&);
ALWAYS_INLINE bool callProvenAccessor(Walker& walker, const BodyValue& value) { return opcodesThatCallProvenAccessorTakes.contains(value.opcode()) && callProvenAccessorOfItsOpcodes(walker, value); }
template<Opcode known> ALWAYS_INLINE bool callProvenAccessor(Walker& walker, const BodyValueOf<known>& value)
{
    if constexpr (opcodesThatCallProvenAccessorTakes.contains(known)) {
        PropertyKeyID key = value.template as<known>().key();
        return key && !liesInThread(key) && walker.facts().linked().keysOfAccessorsExpectedInFields.get(key.index()) && callProvenAccessorOfItsOpcodes(walker, value);
    } else
        return false;
}

struct OwnKeyOrder {
    LayoutID layout;
    Vector<FieldIndex, 8> fields;
    Vector<FieldIndex, 8> created;
};
struct OwnKeyPlan {
    explicit operator bool() const { return !orders.isEmpty(); }
    Vector<OwnKeyOrder, 2> orders;
    bool layoutsAreProven { false };
    bool isDeclared { false };
    bool hasArmForEveryExpectedOrder { false };
};
inline constexpr unsigned mostArmsOfOwnKeyWalk = 2;
inline constexpr size_t mostFieldsOfOwnKeyWalk = 12;
std::optional<OwnKeyOrder> ownKeysInSight(Walker&, Reference object);
OwnKeyPlan planOwnKeyWalk(Walker&, ValueID source, Reference built, size_t mostFields = mostFieldsOfOwnKeyWalk);
bool continueIfOwnKeys(Walker&, Reference& object, const OwnKeyOrder&, bool layoutIsProven, Origin, BlockID whereTestFails);
struct WaysOutOfOwnKeyTests {
    BlockID whereLayoutDiffers;
    BlockID whereOrderDiffers;
    BlockID whereTestFails;
};
bool continueIfOwnKeys(Walker&, Reference& object, const OwnKeyOrder&, bool layoutIsProven, Origin, const WaysOutOfOwnKeyTests&);
Reference valueOfOwnKey(Walker&, Reference object, LayoutID, FieldIndex, Origin);
bool orderWordTellsOwnKeysOf(const Walker&, LayoutID);

bool callAccessorInSlot(Walker&, const BodyValue& access);
bool accessStaticFieldInVariable(Walker&, const BodyValue& access);
bool buildCopyThatMakesObject(Walker&, const BodyValue& copy);
bool fillLiteralInSight(Walker&, const BodyValue& firstCopy);
bool walkCopyApartFromSourceWithoutProperties(Walker&, const BodyValue& copy);
inline constexpr uint16_t primitivesWithoutPropertiesToCopy = Primitive::NumberLike | Primitive::AnySymbol | Primitive::BigInt;
bool defineOwnKeysOfSourceOnObjectThatStands(Walker&, const BodyValue& copy);
bool mayStandBehindCopy(const Walker&, const BodyIndex&, const BodyValue&, ValueID copy);
inline constexpr OpcodeSet opcodesThatBuildCopyWithWhatFollowsTakes { Opcode::CopyObject };
AOT_LINK_OF_WALK(buildCopyWithWhatFollows, opcodesThatBuildCopyWithWhatFollowsTakes)


} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

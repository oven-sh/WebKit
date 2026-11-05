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

inline constexpr OpcodeSet opcodesThatBuildCopyOrKeyOrderFormTakes { Opcode::StrictEq, Opcode::CopyObject, Opcode::Call };
AOT_LINK_OF_WALK(buildCopyOrKeyOrderForm, opcodesThatBuildCopyOrKeyOrderFormTakes)
inline constexpr OpcodeSet opcodesThatOpenGuessOfKeyOrdersTakes { Opcode::PropGet, Opcode::PropSet };
AOT_LINK_OF_WALK(openGuessOfKeyOrders, opcodesThatOpenGuessOfKeyOrdersTakes)
inline constexpr OpcodeSet opcodesThatCallProvenAccessorTakes { Opcode::PropGet, Opcode::PropSet };
AOT_LINK_OF_WALK(callProvenAccessor, opcodesThatCallProvenAccessorTakes)

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
OwnKeyPlan planOwnKeyWalk(Walker&, ValueID source, Reference built);
bool goOnWhereObjectHasOwnKeys(Walker&, Reference& object, const OwnKeyOrder&, bool layoutIsProven, Origin, BlockID whereTestFails);
struct WaysOutOfOwnKeyTests {
    BlockID whereLayoutDiffers;
    BlockID whereOrderDiffers;
    BlockID whereTestFails;
};
bool goOnWhereObjectHasOwnKeys(Walker&, Reference& object, const OwnKeyOrder&, bool layoutIsProven, Origin, const WaysOutOfOwnKeyTests&);
Reference valueOfOwnKey(Walker&, Reference object, LayoutID, FieldIndex, Origin);
bool orderWordTellsOwnKeysOf(const Walker&, LayoutID);

bool foldComparisonOfValueWithItself(Walker&, const BodyValue& comparison);
bool callAccessorInSlot(Walker&, const BodyValue& access);
bool accessStaticFieldInVariable(Walker&, const BodyValue& access);
bool assignSourcesFieldByField(Walker&, const BodyValue& call);
bool buildCopyThatMakesObject(Walker&, const BodyValue& copy);
inline constexpr OpcodeSet opcodesThatBuildCopyWithWhatFollowsTakes { Opcode::CopyObject };
AOT_LINK_OF_WALK(buildCopyWithWhatFollows, opcodesThatBuildCopyWithWhatFollowsTakes)

FunctionID findGetterExpectedInSlot(Walker&, TypeID lookedAt, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// AOTWalk.h includes it at its foot: it leans on the declarations there.

#if ENABLE(AOT)

#include "AOTWalkLinks.h"
#include "AOTPlan.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// ---- The three doors.
// AOTWalkCopies.cpp. A link of the chain, between buildFreshObject() and callBuiltin(). One test of the opcode against one OpcodeSet, then, each asking for its own mechanisms:
// foldComparisonOfValueWithItself > foldLookupPastOwnNames > buildAccessThroughProxyInSight > assignSourcesFieldByField > buildCopyThatMakesObject. False: nothing was built.
extern const OpcodeSet opcodesThatBuildCopyOrKeyOrderFormTakes;
AOT_LINK_OF_WALK(buildCopyOrKeyOrderForm, opcodesThatBuildCopyOrKeyOrderFormTakes)
// AOTWalkCopiesOfObjects.cpp. For buildTypedAccess(), of a CopyDataProperties that buildCopyOfPropertiesForTypedAccess() has refused. False: nothing was built, and the copy stays as written.
bool buildCopyOfProperties(Walker&, const BodyValue& copy);
// AOTWalkCopies.cpp. A term of the row of openGuessAt(): callAccessorInSlot. True: both arms are built, have left what they had to leave, and have met. False: nothing was built.
inline constexpr OpcodeSet opcodesThatOpenGuessOfKeyOrdersTakes { Opcode::PropGet, Opcode::PropSet };
AOT_LINK_OF_WALK(openGuessOfKeyOrders, opcodesThatOpenGuessOfKeyOrdersTakes)
// AOTWalkKeyOrdersAccessors.cpp. A link of the chain, behind callDirectly(): DirectCalls. Where Link has proven the half of the accessor (LinkedAccessorsOfField::getterIsProven) the access is the call. False: nothing was built.
inline constexpr OpcodeSet opcodesThatCallProvenAccessorTakes { Opcode::PropGet, Opcode::PropSet };
AOT_LINK_OF_WALK(callProvenAccessor, opcodesThatCallProvenAccessorTakes)

// ---- The walk over own keys. AOTWalkCopiesOwnKeys.cpp.
struct OwnKeyOrder {
    // Null: the object has no own key.
    LayoutID layout;
    // What a walk over enumerable string keys meets, in the order in which the object got them. Each is a plain value in a slot that can be read.
    Vector<FieldIndex, 8> fields;
    // The beginning of an order of the layout that HasKeyOrder asks for: every field with storage that the object has, private names and symbols too.
    Vector<FieldIndex, 8> created;
};
struct OwnKeyPlan {
    explicit operator bool() const { return !orders.isEmpty(); }
    // The orders of one layout side by side, the likeliest first. At most mostArmsOfOwnKeyWalk.
    Vector<OwnKeyOrder, 2> orders;
    // False: the layouts are only expected, and a ShapeTest stands in front of each arm.
    bool layoutsAreProven { false };
    bool isDeclared { false };
    // An object in a layout or an order without an arm comes by right: the operation as written takes it.
    bool hasArmForEveryExpectedOrder { false };
};
// Thresholds of the cost model: FRESH.md row 2 (750 B a copy for sources with optional fields bought nothing outside loops). Outside a loop a plan has one arm, for the order that lists every field of a layout whose fields are all required.
inline constexpr unsigned mostArmsOfOwnKeyWalk = 2;
inline constexpr size_t mostFieldsOfOwnKeyWalk = 12;
// The state knows every own key without a test: nothing but this code has the object (propertiesOfFreshObject()), or WalkState::orderWordOf() has its word and its type proves one layout. Nothing: it does not.
std::optional<OwnKeyOrder> ownKeysInSight(Walker&, Reference object);
// It builds nothing. `source`: of the body being read, for what is expected of it. An empty plan: the operation stays as written.
OwnKeyPlan planOwnKeyWalk(Walker&, ValueID source, Reference built);
// Builds ShapeTest (unless proven) and its Narrowed, HasKeyOrder over `created`, HasNoExtraProperties[hasNoFieldOutsideSlot], each ending its block with the failing edge to `whereTestFails`, and tells the state that every field lies in its slot.
// A frozen object passes, so the state is not told the order word, which would say that the object takes stores. `object` becomes the narrowed name. False: the walk stands nowhere.
bool goOnWhereObjectHasOwnKeys(Walker&, Reference& object, const OwnKeyOrder&, bool layoutIsProven, Origin, BlockID whereTestFails);
// The same where the next arm takes an object of another layout, or one of the same layout that got its keys otherwise: the failing edge of that one test leads there. Behind a failed HasKeyOrder `object` is still the narrowed name.
struct WaysOutOfOwnKeyTests {
    BlockID whereLayoutDiffers;
    BlockID whereOrderDiffers;
    BlockID whereTestFails;
};
bool goOnWhereObjectHasOwnKeys(Walker&, Reference& object, const OwnKeyOrder&, bool layoutIsProven, Origin, const WaysOutOfOwnKeyTests&);
// What the state has for the slot, else a GetSlot. Behind ownKeysInSight() or goOnWhereObjectHasOwnKeys() alone.
Reference valueOfOwnKey(Walker&, Reference object, LayoutID, FieldIndex, Origin);
// The order word of a cell of the layout says which keys the cell has got: the layout holds compiled objects, keeps orders, and has one shape.
bool orderWordTellsOwnKeysOf(const Walker&, LayoutID);

// ---- The parts. Each: false = nothing was built.
bool foldComparisonOfValueWithItself(Walker&, const BodyValue& comparison); // AOTWalkCopies.cpp . Folding
bool foldLookupPastOwnNames(Walker&, const BodyValue& access); // AOTWalkKeyOrdersAccessors.cpp . Folding, TypedOperations
bool callAccessorInSlot(Walker&, const BodyValue& access); // AOTWalkKeyOrdersAccessors.cpp . Guesses, DirectCalls
// `Meter.raw`, `Meter.raw = 1`, off the one class object of the class that declares the static field: the word of its variable, where the field lies there; else as written. Guesses, TypedOperations. True: the arms are built and have met.
bool accessStaticFieldInVariable(Walker&, const BodyValue& access);
bool buildAccessThroughProxyInSight(Walker&, const BodyValue& access); // AOTWalkKeyOrdersProxies.cpp . TypedOperations, DirectCalls
bool assignSourcesFieldByField(Walker&, const BodyValue& call); // AOTWalkCopiesOfObjects.cpp . BuiltinCalls, TypedOperations
bool buildCopyThatMakesObject(Walker&, const BodyValue& copy); // AOTWalkCopiesOfObjects.cpp . of a CopyObject
inline constexpr OpcodeSet opcodesThatBuildCopyWithWhatFollowsTakes { Opcode::CopyObject };
AOT_LINK_OF_WALK(buildCopyWithWhatFollows, opcodesThatBuildCopyWithWhatFollowsTakes) // AOTWalkCopiesFused.cpp . TypedOperations . of a CopyObject
// A link of its own, in front of whoever writes `Object.keys(o)` out as a literal of names: of an object that something else has too, with its keys in sight.
inline constexpr OpcodeSet opcodesThatBuildListOfOwnKeysOverSharedStorageTakes { Opcode::Call };
AOT_LINK_OF_WALK(buildListOfOwnKeysOverSharedStorage, opcodesThatBuildListOfOwnKeysOverSharedStorageTakes) // AOTWalkCopiesOwnKeyLists.cpp . BuiltinCalls, EscapeAnalysis

// ---- What single lines of shared files ask.
// For buildStoreForTypedAccess(): no receiver keeps an order of keys and the slot lies in the cell of each, so a store that creates the property is a PutSlot. Default false: PutSlotOrAdd.
bool creatingStoreListsNoKey(const Walker&, std::span<const LayoutID> receivers, PropertyKeyID); // AOTWalkKeyOrdersAccessors.cpp
// For findGetterOfRead(), so that the looks at loops see what callAccessorInSlot() may copy. Null: none is expected.
FunctionID findGetterExpectedInSlot(Walker&, TypeID lookedAt, PropertyKeyID); // AOTWalkKeyOrdersAccessors.cpp

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

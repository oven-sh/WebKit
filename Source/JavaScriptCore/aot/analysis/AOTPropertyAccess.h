/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

class TypeArena;

struct PropertyAccessQuery {
    Access access { Access::Read };
    MemberStart start { MemberStart::Instance };
    PropertyKeyID key;
    ReceiverCandidates receivers;
    const Type* stored { nullptr };
    uint64_t maxMembers { defaultLimitValue(LimitID::MaxAccessPlanMembers, hostArchitecture()) };
    bool isOwnLookup { false };
    bool isPrivate { false };
    bool isOnlyCalled { false };
    // Set where a plan follows from isOnlyCalled: most are the same whoever uses what is read.
    mutable bool planFollowsFromIsOnlyCalled { false };
    // The asker proves what the access does, and a plan that needs a test proves nothing.
    bool usesOnlyPlansWithoutTests { false };
    bool isListedByAllocation { false };
    // The receiver is an allocation that nothing has finished yet and that leaves the field out, as that of `{ a: 1, [key]: 2, b: 3 }` leaves out `b`.
    bool isLeftOutByUnfinishedAllocation { false };
    // A test on the way here found the slot of the field empty, or may have.
    bool expectsEmptySlot { false };
    bool storesWhatWasMadeOfField { false };
    // The read stands in hot code where addEmptySlotCases() works: the tests of what lies above the object and beside the slot may stand behind the test of the slot.
    bool mayTestSlotFirst { false };
    LayoutID declaringLayout;
    // The receiver is expected to be a class object, which no type proves: the closure of this function.
    FunctionID expectedClassObject;
    AccessSiteID site;
    // In which the access is written. It weighs guesses and decides nothing else.
    ModuleID module;

    bool readIsOnlyCalled() const
    {
        planFollowsFromIsOnlyCalled = true;
        return isOnlyCalled;
    }
};

enum class AccessForm : uint8_t {
    ByName,
    Slot,
    Constant,
    Variable,
    Namespace,
    Builtin,
    Method,
    Call,
    // An own accessor whose GetterSetter lies in the slot of its field: the getter is read out of the slot and called.
    GetterInSlot,
    Split,
};

enum class ReceiverTest : uint8_t { None, Type, ClassObject };

struct PropertyAccessPlan {
    AccessForm form { AccessForm::ByName };
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    Opcode operation { Opcode::Undefined };
    bool booleanConstant { false };
    bool storedValueFits { false };
    bool capturesReceiver { false };
    bool needsMissPath { false };
    // The receivers have the property in the slot or not at all.
    bool emptySlotMeansNoOwnProperty { false };
    // The test of the slot asks for a value of the field's type, which has neither undefined nor null. The slot has a marker for either.
    bool testsDeclaredType { false };
    ReceiverTest receiverTest { ReceiverTest::None };
    // What ReceiverTest::ClassObject tests the receiver for: a closure of this function.
    FunctionID testedClassObject;
    LayoutList receivers;
    ProofSet requiredProofs;
    MemberConditions conditions;
    MemberAnswer member;
    MemberPartition partition;
    UnprovenCause cause { UnprovenCause::Unknown };
    LimitID limit { LimitID::None };

    bool isResolved() const { return form != AccessForm::ByName && form != AccessForm::Split; }
    bool stores() const { return accessOf(operation) == Access::Store; }
    bool needsNoTest() const { return receiverTest == ReceiverTest::None && requiredProofs.isEmpty() && conditions.isEmpty() && (!stores() || storedValueFits); }

    void dump(PrintStream&) const;
};

PropertyAccessPlan planPropertyAccess(const PropertyAccessContext&, const PropertyAccessQuery&);
LimitID limitOfPropertyAccessPlan(const Program&, const PropertyAccessQuery&);

bool isNamedAccess(Opcode);
PropertyKeyID keyOfNamedAccess(const Program&, const Value& access);

// Whoever asks for tested class objects puts ReceiverTest::ClassObject in front of what it makes of the plan.
enum class ExpectedClassObjects : uint8_t { Ignored, Tested };

// LeftOut: for whoever plans without tests. What the receiver is expected to be and the layouts that were seen at the access are not looked at.
enum class PredictedReceivers : uint8_t { Listed, LeftOut };
std::optional<PropertyAccessQuery> propertyAccessQueryOf(const Program&, const Function&, const Value& access, ExpectedClassObjects = ExpectedClassObjects::Ignored, PredictedReceivers = PredictedReceivers::Listed);
// What the query says of a receiver that the function has made itself.
bool isAllocationListing(const Program&, const Function&, ValueID object, PropertyKeyID);
bool isUnfinishedAllocationLeavingOut(const Program&, const Function&, ValueID object, PropertyKeyID);

// Whether an object of the layout can be seen with the slot of its field empty: before the field is created, and once the property was deleted or has left the slot.
bool canBeEmpty(const Program&, LayoutID, PropertyKeyID);

struct TestsOfEmptySlot {
    ProofSet proofs;
    MemberConditions conditions;
};

// For an option: an optional field of records that some literal leaves out. The tests under which an object whose slot is empty answers undefined.
enum class OptionKind : uint8_t {
    RecordOption = 1 << 0,
    LazilyInitializedField = 1 << 1,
};
std::optional<TestsOfEmptySlot> testsThatOptionLeftOutReadsAsUndefined(const PropertyAccessContext&, std::span<const LayoutID> receivers, PropertyKeyID, OptionSet<OptionKind>);

struct LazyFieldCreation {
    Opcode operation;
    ProofSet proofs;
    MemberConditions conditions;
};
std::optional<LazyFieldCreation> planLazyFieldCreation(const PropertyAccessContext&, std::span<const LayoutID> receivers, PropertyKeyID);

Type storedTypeOf(const Program&, std::span<const LayoutID> receivers, PropertyKeyID, TypeArena&);

// For a plan of the form GetterInSlot: the functions that the getter in a filled slot is a closure of. Nothing where the slot of some receiver takes other getters too.
std::optional<Vector<FunctionID, 2>> gettersInSlots(const Program&, std::span<const LayoutID> receivers, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

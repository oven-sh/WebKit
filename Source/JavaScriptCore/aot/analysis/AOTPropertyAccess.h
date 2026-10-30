/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTPropertyAccessMembers.h"
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
    bool asksForDeclaredMember { false };
    bool isPrivate { false };
    bool isOnlyCalled { false };
    mutable bool planFollowsFromIsOnlyCalled { false };
    bool usesOnlyPlansWithoutTests { false };
    bool isListedByAllocation { false };
    bool isLeftOutByUnfinishedAllocation { false };
    bool expectsEmptySlot { false };
    bool storesWhatWasMadeOfField { false };
    bool mayTestSlotFirst { false };
    LayoutID declaringLayout;
    FunctionID expectedClassObject;
    AccessSiteID site;
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
    GetterInSlot,
    SetterInSlot,
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
    bool emptySlotMeansNoOwnProperty { false };
    bool testsDeclaredType { false };
    ReceiverTest receiverTest { ReceiverTest::None };
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

// The operations that look a written name up: those to which the schema gives an access and no slot.
constexpr OpcodeSet findNamedAccesses()
{
    OpcodeSet found;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (accessOf(opcode) != Access::None && accessClassOf(opcode) != AccessClass::Slot)
            found.add(opcode);
    }
    return found;
}

inline constexpr OpcodeSet namedAccesses = findNamedAccesses();

bool isNamedAccess(Opcode);
PropertyKeyID keyOfNamedAccess(const Program&, const Value& access);

enum class ExpectedClassObjects : uint8_t { Ignored, Tested };

enum class PredictedReceivers : uint8_t { Listed, LeftOut };
std::optional<PropertyAccessQuery> propertyAccessQueryOf(const Program&, const Function&, const Value& access, ExpectedClassObjects = ExpectedClassObjects::Ignored, PredictedReceivers = PredictedReceivers::Listed);
bool isAllocationListing(const Program&, const Function&, ValueID object, PropertyKeyID);
bool isUnfinishedAllocationLeavingOut(const Program&, const Function&, ValueID object, PropertyKeyID);


struct TestsOfEmptySlot {
    ProofSet proofs;
    MemberConditions conditions;

    bool isEmpty() const { return proofs.isEmpty() && conditions.isEmpty(); }
};

enum class OptionKind : uint8_t {
    RecordOption = 1 << 0,
    LazilyInitializedField = 1 << 1,
    FieldWithoutStorage = 1 << 2,
};
// For an IsSlotFilled that ends the block and has no site of its own: a test in front of a read. Its empty arm mostly reads by name, which finds a value that the slot cannot hold.
// An arm that answers by itself, with undefined or with a member from above, and does not ask whether a field lies outside its slot relies on the field staying in its slot.
bool asksObjectWhereSlotIsEmpty(const Function&, BlockID, ValueID test);

std::optional<TestsOfEmptySlot> testsThatOptionLeftOutReadsAsUndefined(const PropertyAccessContext&, std::span<const LayoutID> receivers, PropertyKeyID, OptionSet<OptionKind>);

struct MethodAboveEmptySlot {
    LayoutList layouts;
    FunctionID method;
    ProofSet proofs;
    MemberConditions conditions;
};
struct MethodsAboveEmptySlot {
    Vector<MethodAboveEmptySlot, 2> methods;
    bool areAllReceivers { true };
};
MethodsAboveEmptySlot findMethodsAboveEmptySlot(const PropertyAccessContext&, std::span<const LayoutID> receivers, PropertyKeyID);

struct LazyFieldCreation {
    Opcode operation;
    ProofSet proofs;
    MemberConditions conditions;
};
std::optional<LazyFieldCreation> planLazyFieldCreation(const PropertyAccessContext&, std::span<const LayoutID> receivers, PropertyKeyID);

Type storedTypeOf(const Program&, std::span<const LayoutID> receivers, PropertyKeyID, TypeArena&);

std::optional<Vector<FunctionID, 2>> gettersInSlots(const Program&, std::span<const LayoutID> receivers, PropertyKeyID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

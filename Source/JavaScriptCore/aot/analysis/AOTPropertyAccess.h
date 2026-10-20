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
    bool isPrivate { false };
    bool isOnlyCalled { false };
    bool isListedByAllocation { false };
    // A test on the way here found the slot of the field empty, or may have.
    bool expectsEmptySlot { false };
    LayoutID declaringLayout;
    AccessSiteID site;
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
    Split,
};

enum class ReceiverTest : uint8_t { None, Shape, Type };

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
    ReceiverTest receiverTest { ReceiverTest::None };
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

bool isNamedAccess(Opcode);
PropertyKeyID keyOfNamedAccess(const Program&, const Value& access);

std::optional<PropertyAccessQuery> propertyAccessQueryOf(const Program&, const Function&, const Value& access);

Type storedTypeOf(const Program&, std::span<const LayoutID> receivers, PropertyKeyID, TypeArena&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

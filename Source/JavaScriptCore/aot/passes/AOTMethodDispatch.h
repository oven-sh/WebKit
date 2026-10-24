/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSplitUnions.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

struct SlotStamp {
    PropertyKeyID key;
    LayoutSetID receivers;
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    AccessSiteID accessSite;
};

void setSlotStamp(Value&, const SlotStamp&);

Value makeObjectTest(Proof, ValueID object, const SlotStamp&);
// One test for all the conditions, in the order in which addCondition keeps them.
Value makeConditionsTest(TypeArena&, std::span<const MemberCondition>);
bool testsCoverConditions(std::span<const Value> tests, std::span<const MemberCondition>);

struct AccessTests {
    Vector<Value, 4> tests;
    std::optional<uint32_t> receiverLayoutTest;

    const Value* findReceiverLayoutTest() const LIFETIME_BOUND { return receiverLayoutTest ? &tests[*receiverLayoutTest] : nullptr; }
};

struct AccessTestInputs {
    ValueID receiver;
    ValueID stored;
    SlotStamp stamp;
};

Type testableStoredTypeOf(const PropertyAccessContext&, const Type& stored, const Type& fieldType, TypeArena&);

AccessTests createAccessTests(FunctionPassContext&, const PropertyAccessPlan&, const AccessTestInputs&);
std::optional<ReceiverCandidates> receiversEstablishedBy(const Program&, std::span<const Value> tests);

struct DispatchPlan {
    Vector<uint32_t, 4> arms;
    LayoutList rest;
    bool restIsLikely { false };

    bool hasRest() const { return !rest.isEmpty(); }
};

using CanEmitArm = ScopedLambda<bool(const MemberArm&)>;

DispatchPlan planDispatch(FunctionPassContext&, const MemberPartition&, const ReceiverCandidates&, ValueID site, NOESCAPE const CanEmitArm&);

ValueSplit createSplit(FunctionPassContext&, const DispatchPlan&, const MemberPartition&, ValueID receiver, BlockFrequency siteFrequency);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

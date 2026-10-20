/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessMembers.h"

#if ENABLE(AOT)

#include "AOTPropertyAccessInternal.h"
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

bool belongToOneArm(const MemberAnswer& a, const MemberAnswer& b)
{
    // An arm needs the tests of all its members, so a member that needs one would cost the proven members their proof.
    if (a.kind != b.kind || a.needsNoTest() != b.needsNoTest())
        return false;
    switch (a.kind) {
    case MemberKind::Unknown:
        return false;
    case MemberKind::Absent:
    case MemberKind::Field:
        return true;
    case MemberKind::Method:
        return a.method == b.method;
    case MemberKind::Accessor:
        return a.getter == b.getter && a.setter == b.setter;
    case MemberKind::Variable:
        return a.variable == b.variable;
    case MemberKind::Namespace:
        return a.module == b.module;
    case MemberKind::Builtin:
        return a.builtin == b.builtin;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void mergeIntoArm(MemberArm& arm, LayoutID layout, const MemberAnswer& answer)
{
    MemberAnswer& merged = arm.answer;
    arm.layouts.add(layout);
    merged.receiverProofs |= answer.receiverProofs;
    addConditions(merged.conditions, answer.conditions.span());
    if (merged.holder != answer.holder || merged.field != answer.field) {
        merged.holder = { };
        merged.field = 0;
    }
    if (merged.cause == UnprovenCause::Unknown)
        merged.cause = answer.cause;
    setLevel(merged);
}

static MemberPartition partition(const PropertyAccessContext& context, std::span<const LayoutID> candidates, MemberQuery query)
{
    MemberPartition result;
    for (LayoutID layout : candidates) {
        query.layout = layout;
        MemberAnswer answer = findMember(context, query);
        if (!answer.isKnown()) {
            result.rest.add(layout);
            continue;
        }
        size_t armIndex = result.arms.findIf([&](const MemberArm& arm) {
            return belongToOneArm(arm.answer, answer);
        });
        if (armIndex != notFound) {
            mergeIntoArm(result.arms[armIndex], layout, answer);
            continue;
        }
        result.arms.append(MemberArm { WTF::move(answer), LayoutList { layout } });
    }
    return result;
}

MemberPartition partitionMembers(const PropertyAccessContext& context, std::span<const LayoutID> candidates, PropertyKeyID key, Access access)
{
    return partition(context, candidates, { MemberStart::Instance, { }, key, access, false });
}

MemberPartition partitionOwnMembers(const PropertyAccessContext& context, std::span<const LayoutID> candidates, PropertyKeyID key)
{
    return partition(context, candidates, { MemberStart::Instance, { }, key, Access::Has, true });
}

const MemberArm* MemberPartition::findArm(MemberKind kind) const
{
    for (const MemberArm& arm : arms) {
        if (arm.answer.kind == kind)
            return &arm;
    }
    return nullptr;
}

static void dumpLayouts(PrintStream& out, const LayoutList& layouts)
{
    CommaPrinter comma;
    out.print("[");
    for (LayoutID layout : layouts)
        out.print(comma, "L", layout.index());
    out.print("]");
}

void MemberPartition::dump(PrintStream& out) const
{
    CommaPrinter newline("\n"_s);
    for (const MemberArm& arm : arms) {
        out.print(newline, "arm ");
        dumpLayouts(out, arm.layouts);
        out.print(": ", arm.answer);
    }
    if (rest.isEmpty())
        return;
    out.print(newline, "rest ");
    dumpLayouts(out, rest);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

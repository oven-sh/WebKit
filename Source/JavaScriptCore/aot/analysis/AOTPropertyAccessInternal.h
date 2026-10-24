/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPropertyAccess.h"

namespace JSC { namespace AOT { namespace Typed {

struct DeclaredMember {
    FunctionID method;
    FunctionID getter;
    FunctionID setter;
    MemberMutability mutability { MemberMutability::Fixed };
    bool isWritableData { false };

    bool isDeclared() const { return method || getter || setter; }
    bool isListedWithoutDefinition() const { return !isDeclared() && mutability != MemberMutability::Fixed; }
    bool isAccessor() const { return getter || setter; }
};

DeclaredMember findDeclaredMember(const ClassFlow&, ClassSide, PropertyKeyID);

enum class ChainStep : uint8_t { Unknown, CompiledClass, BuiltinClass, NoBase };

struct NextHolder {
    ChainStep step { ChainStep::Unknown };
    LayoutID layout;
    BuiltinClassID builtinClass;
};

NextHolder nextHolderOf(const Program&, LayoutID classLayout, ClassSide, MemberConditions&);

void setDeclaredMember(MemberAnswer&, const DeclaredMember&, LayoutID holder);
void setLevel(MemberAnswer&);
MemberAnswer finishAtChainEnd(MemberAnswer&& walked, MemberAnswer&& chainEnd);

enum class BuiltinLocationAnswer : uint8_t { Unknown, Constant };

BuiltinLocationAnswer builtinLocationAnswerOf(const PropertyAccessContext&, BuiltinID member);

enum class BuiltinHolderGain : uint8_t { Unknown, Impossible };

BuiltinHolderGain builtinHolderGainOf(const PropertyAccessContext&, BuiltinID holder, PropertyKeyID);

struct FieldArmState {
    bool isAlwaysFilled { true };
    bool isFilledAtBirth { true };
    bool hasSlotNowhere { true };
    bool isRequired { true };
    bool isAddedLate { true };
    bool admitsUndefined { false };
    bool isAddedLateInSomeMember { false };
    bool isLeftOutAtConstruction { false };
    bool hasConstructor { false };
    bool isInContainer { false };
    bool cannotMoveOut { true };
    bool mayLieOutOfLine { false };
    bool isAbsentOnPrototypes { true };
    bool inheritsNothingUnderName { true };
    bool holdsRawNumberOrBoolean { true };
    bool holdsRawValuesWhileCellIsUnmarked { true };
    bool holdsRawValueFromBirthInSomeMember { false };
    bool holdsOnlyListedFunctions { true };
    bool slotsHoldUndefinedAndNull { true };
    bool createdOnlyAtConstruction { true };
    bool hasImmutableMember { false };
    OptionSet<FieldForm> form;
    bool isAccessorInEveryMember { true };
    bool isExpectedToHaveSetterInEveryMember { true };
    bool hasDeferredMember { false };
    bool deferralIsUniform { true };
    bool holdsOnlyDeferredFunction { true };
    ClosureDeferral deferral { ClosureDeferral::Unknown };
    FunctionID deferredFunction;
    ReceiverState receivers;
    SortedSet<TypeID, 2> slotTypes;
};

PropertyAccessPlan planMemberArm(const PropertyAccessContext&, const PropertyAccessQuery&, const MemberArm&);
void planFieldArm(const PropertyAccessContext&, const PropertyAccessQuery&, const MemberArm&, PropertyAccessPlan&);
void planVariableArm(const PropertyAccessContext&, const PropertyAccessQuery&, const MemberArm&, PropertyAccessPlan&);

bool needsNoClosure(const Program&, FunctionID);

inline MemberAnswer createProvenAnswer(MemberKind kind)
{
    MemberAnswer answer;
    answer.kind = kind;
    answer.level = KnowledgeLevel::Proven;
    return answer;
}

inline void addConditionWithCause(MemberAnswer& walked, const MemberCondition& condition, UnprovenCause cause)
{
    addCondition(walked.conditions, condition);
    if (walked.cause == UnprovenCause::Unknown)
        walked.cause = cause;
}

inline void dumpProofs(PrintStream& out, ProofSet proofs)
{
    CommaPrinter comma;
    out.print("[");
    for (Proof proof : allMembersOf<Proof>) {
        if (proofs.contains(proof))
            out.print(comma, nameOf(proof));
    }
    out.print("]");
}

inline void dumpCondition(PrintStream& out, const MemberCondition& condition)
{
    switch (condition.kind) {
    case MemberCondition::Kind::MemberUnchanged:
        out.print("MemberUnchanged(L", condition.layout.index(), ", ", nameOf(condition.side), ", K", condition.key.index(), ")");
        return;
    case MemberCondition::Kind::PrototypeLinkUnchanged:
        out.print("PrototypeLinkUnchanged(L", condition.layout.index(), ", ", nameOf(condition.side), ")");
        return;
    case MemberCondition::Kind::ClassEvaluatedOnce:
        out.print("ClassEvaluatedOnce(L", condition.layout.index(), ")");
        return;
    case MemberCondition::Kind::InstancePrototypeUnchanged:
        out.print("InstancePrototypeUnchanged(L", condition.layout.index(), ")");
        return;
    case MemberCondition::Kind::MemberNotHidden:
        out.print("MemberNotHidden(L", condition.layout.index(), ", ", nameOf(condition.side), ", K", condition.key.index(), ")");
        return;
    case MemberCondition::Kind::MemberPassedByStores:
        out.print("MemberPassedByStores(L", condition.layout.index(), ", K", condition.key.index(), ")");
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline void dumpConditions(PrintStream& out, const MemberConditions& conditions)
{
    CommaPrinter comma;
    out.print("[");
    for (const MemberCondition& condition : conditions) {
        out.print(comma);
        dumpCondition(out, condition);
    }
    out.print("]");
}

inline void dumpLayouts(PrintStream& out, const LayoutList& layouts)
{
    CommaPrinter comma;
    out.print("[");
    for (LayoutID layout : layouts)
        out.print(comma, "L", layout.index());
    out.print("]");
}

inline bool isMemberOfClass(const MemberAnswer& answer)
{
    return answer.kind == MemberKind::Method || answer.kind == MemberKind::Accessor;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

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

// What the layouts of an arm say of a field together. It depends on the layouts, the key and whether the name is private, so PropertyAccessCache keeps it.
struct FieldArmState {
    bool isAlwaysFilled { true };
    bool isFilledAtBirth { true };
    bool hasSlotNowhere { true };
    bool isRequired { true };
    // No allocation fills the field and no type declares it: flow has given a name room that some store creates, often long after the construction, or never.
    bool isAddedLate { true };
    // Whether undefined is among what the field is stored with or declared to hold, in some member. An empty slot reads as undefined, so where it is not, whoever reads the field expects a filled slot.
    bool admitsUndefined { false };
    // So it is in some of the layouts: the objects that a constructor function makes, beside the type that its factories give them, which declares the field.
    bool isAddedLateInSomeMember { false };
    // Some construction of a layout that declares the field makes an object without it, as `{ children }` under a type with `color?`.
    bool isLeftOutAtConstruction { false };
    bool hasConstructor { false };
    // A namespace or an enum: born empty, filled once by the code that stands around it, and read full from then on.
    bool isInContainer { false };
    // A store of a value that the slot does not hold and a redefinition move the property to the extra properties of the object and leave the slot empty.
    bool cannotMoveOut { true };
    bool mayLieOutOfLine { false };
    bool isAbsentOnPrototypes { true };
    bool inheritsNothingUnderName { true };
    bool holdsRawNumberOrBoolean { true };
    // While the mark of the cell is off, no member's slot holds a marker for undefined or null.
    bool holdsRawValuesWhileCellIsUnmarked { true };
    bool holdsRawValueFromBirthInSomeMember { false };
    bool holdsOnlyListedFunctions { true };
    bool slotsHoldUndefinedAndNull { true };
    bool createdOnlyAtConstruction { true };
    bool hasImmutableMember { false };
    // What the properties in the slots are other than writable, enumerable, configurable data properties.
    OptionSet<FieldForm> form;
    bool isAccessorInEveryMember { true };
    bool isExpectedToHaveSetterInEveryMember { true };
    bool hasDeferredMember { false };
    bool deferralIsUniform { true };
    // Unknown code may have stored another function into the slot of an object that reached it.
    bool holdsOnlyDeferredFunction { true };
    ClosureDeferral deferral { ClosureDeferral::Unknown };
    FunctionID deferredFunction;
    ReceiverState receivers;
    SortedSet<TypeID, 2> slotTypes;
};

struct CreationTests {
    ProofSet proofs;
    MemberConditions conditions;

    bool isEmpty() const { return proofs.isEmpty() && conditions.isEmpty(); }
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

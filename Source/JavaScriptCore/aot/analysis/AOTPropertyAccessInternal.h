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

PropertyAccessPlan planMemberArm(const PropertyAccessContext&, const PropertyAccessQuery&, const MemberArm&);
void planFieldArm(const PropertyAccessContext&, const PropertyAccessQuery&, const MemberArm&, PropertyAccessPlan&);
void planVariableArm(const PropertyAccessContext&, const PropertyAccessQuery&, const MemberArm&, PropertyAccessPlan&);

bool needsNoClosure(const Program&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

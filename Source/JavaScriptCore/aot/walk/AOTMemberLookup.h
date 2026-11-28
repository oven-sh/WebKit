/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTID.h"
#include "AOTPlan.h"
#include "AOTSchemaRecords.h"
#include "AOTType.h"

namespace JSC { namespace AOT { namespace Typed {

class Program;
class TypeArena;

class WalkFacts {
public:
    explicit WalkFacts(const Facts& facts)
        : m_facts(&facts)
    {
    }

    const Facts& operator*() const { return *m_facts; }

private:
    const Facts* m_facts;
};

struct AccessContext {
    const Program& program;
    WalkFacts facts;
    const Plan& plan;
    TypeArena& types;
    FunctionID writtenIn;
};

constexpr PropertyKeyID nameOfGuardAnsweringFor(MemberConditionKind kind, PropertyKeyID key)
{
    switch (kind) {
    case MemberConditionKind::MemberUnchanged:
    case MemberConditionKind::MemberNotHidden:
    case MemberConditionKind::MemberPassedByStores:
    case MemberConditionKind::MemberUnchangedAbove:
    case MemberConditionKind::MemberAssigned:
        return key;
    case MemberConditionKind::PrototypeLinkUnchanged:
    case MemberConditionKind::ClassEvaluatedOnce:
    case MemberConditionKind::InstancePrototypeUnchanged:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

inline bool classHasOneClassObject(const AccessContext& context, LayoutID classLayout) { return (*context.facts).isEvaluatedAtMostOnce(classLayout); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

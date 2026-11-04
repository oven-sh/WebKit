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

// What a question about an access or a call is asked with. Each question is a pure function of its arguments.
struct AccessContext {
    const Program& program;
    WalkFacts facts;
    const Plan& plan;
    TypeArena& types;
    // The function in whose text the access stands.
    FunctionID writtenIn;
};

// object/AOTClassGuards.h: the guard of (layout, side, this name) answers for the condition. Null: the guard of the side does.
constexpr PropertyKeyID nameOfGuardAnsweringFor(MemberConditionKind kind, PropertyKeyID key)
{
    switch (kind) {
    case MemberConditionKind::MemberUnchanged:
    case MemberConditionKind::MemberNotHidden:
    case MemberConditionKind::MemberPassedByStores:
        return key;
    case MemberConditionKind::PrototypeLinkUnchanged:
    case MemberConditionKind::ClassEvaluatedOnce:
    case MemberConditionKind::InstancePrototypeUnchanged:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// The class has one class object for each global, which the closure table holds. So an allocation that names no prototype has the prototype object that this class object was made with.
// A class that is evaluated at most once has. Of a constructor function nothing is claimed, and the emitter asks the run time.
inline bool classHasOneClassObject(const AccessContext& context, LayoutID classLayout) { return (*context.facts).isEvaluatedAtMostOnce(classLayout); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

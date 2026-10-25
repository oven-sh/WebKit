/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

void forEachNodeOf(const ValidationRule& rule, NOESCAPE const Invocable<void(uint32_t)> auto& visit)
{
    switch (rule.kind()) {
    case ValidationRule::Kind::Integer:
    case ValidationRule::Kind::Minimum:
    case ValidationRule::Kind::Maximum:
    case ValidationRule::Kind::ExclusiveMinimum:
    case ValidationRule::Kind::ExclusiveMaximum:
    case ValidationRule::Kind::MultipleOf:
    case ValidationRule::Kind::MinimumLength:
    case ValidationRule::Kind::MaximumLength:
    case ValidationRule::Kind::Pattern:
    case ValidationRule::Kind::Format:
    case ValidationRule::Kind::Template:
    case ValidationRule::Kind::Message:
    case ValidationRule::Kind::DefaultNull:
    case ValidationRule::Kind::DefaultBoolean:
    case ValidationRule::Kind::DefaultNumber:
    case ValidationRule::Kind::DefaultString:
    case ValidationRule::Kind::UniqueItems:
    case ValidationRule::Kind::MinimumMembers:
    case ValidationRule::Kind::MaximumMembers:
    case ValidationRule::Kind::DependentMembers:
        return;
    case ValidationRule::Kind::Contains:
        visit(rule.as<ValidationRule::Contains>().node);
        return;
    case ValidationRule::Kind::Not:
        visit(rule.as<ValidationRule::Not>().node);
        return;
    case ValidationRule::Kind::If: {
        const auto& choice = rule.as<ValidationRule::If>();
        visit(choice.condition);
        visit(choice.then);
        visit(choice.otherwise);
        return;
    }
    case ValidationRule::Kind::ExactlyOne:
        for (uint32_t node : rule.as<ValidationRule::ExactlyOne>().nodes)
            visit(node);
        return;
    case ValidationRule::Kind::Every:
        for (uint32_t node : rule.as<ValidationRule::Every>().nodes)
            visit(node);
        return;
    case ValidationRule::Kind::PatternMembers:
        for (uint32_t node : rule.as<ValidationRule::PatternMembers>().nodes)
            visit(node);
        return;
    case ValidationRule::Kind::MemberNames:
        visit(rule.as<ValidationRule::MemberNames>().node);
        return;
    case ValidationRule::Kind::DependentNode:
        visit(rule.as<ValidationRule::DependentNode>().node);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// By kind and not by count: ExactlyOne of no node refuses every value, and only a plan says so.
inline bool namesNodes(const ValidationRule& rule)
{
    switch (rule.kind()) {
    case ValidationRule::Kind::Contains:
    case ValidationRule::Kind::Not:
    case ValidationRule::Kind::If:
    case ValidationRule::Kind::ExactlyOne:
    case ValidationRule::Kind::Every:
    case ValidationRule::Kind::PatternMembers:
    case ValidationRule::Kind::MemberNames:
    case ValidationRule::Kind::DependentNode:
        return true;
    case ValidationRule::Kind::Integer:
    case ValidationRule::Kind::Minimum:
    case ValidationRule::Kind::Maximum:
    case ValidationRule::Kind::ExclusiveMinimum:
    case ValidationRule::Kind::ExclusiveMaximum:
    case ValidationRule::Kind::MultipleOf:
    case ValidationRule::Kind::MinimumLength:
    case ValidationRule::Kind::MaximumLength:
    case ValidationRule::Kind::Pattern:
    case ValidationRule::Kind::Format:
    case ValidationRule::Kind::Template:
    case ValidationRule::Kind::Message:
    case ValidationRule::Kind::DefaultNull:
    case ValidationRule::Kind::DefaultBoolean:
    case ValidationRule::Kind::DefaultNumber:
    case ValidationRule::Kind::DefaultString:
    case ValidationRule::Kind::UniqueItems:
    case ValidationRule::Kind::MinimumMembers:
    case ValidationRule::Kind::MaximumMembers:
    case ValidationRule::Kind::DependentMembers:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

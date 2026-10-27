/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RecordedMember {
    SelectionGroup group;
};

} // anonymous namespace

static bool isListed(SelectionContext& context, ValueID id)
{
    return context.analyses().useLists().locationOf(id).placement == ValuePlacement::InBlock;
}

static std::optional<FunctionID> functionOf(const Function& function, ValueID closure)
{
    const Value& definition = function.at(sameObjectThroughCasts(function, closure));
    if (definition.opcode != Opcode::NewClosure)
        return std::nullopt;
    return definition.function();
}

static bool isCreatedByClassRecord(SelectionContext& context, const Value& definition)
{
    if (!definition.key())
        return false;
    std::optional<FunctionID> function = functionOf(context.function(), definition.functionOperand());
    return function && context.slots().creationOf(context.program(), definition.layout(), *function) == MemberCreation::ByClassRecord;
}

static SelectionGroup claimRecordedMember(SelectionContext& context, ValueID id)
{
    const Function& function = context.function();
    const UseLists& useLists = context.analyses().useLists();
    ValueID closure = sameObjectThroughCasts(function, function.at(id).functionOperand());
    RELEASE_ASSERT(function.at(closure).capturesOperands().size() <= 1);

    if (context.singleUser(closure) == id && useLists.blockOf(closure) == useLists.blockOf(id)) {
        SelectionGroup group = groupOf({ closure, id });
        if (context.tryClaim(SelectionRuleID::ClassDefinition, group) == ClaimResult::Claimed)
            return group;
    }

    SelectionGroup group = groupOfOne(id);
    ClaimResult result = context.tryClaim(SelectionRuleID::ClassDefinition, group);
    RELEASE_ASSERT(result == ClaimResult::Claimed || result == ClaimResult::AlreadyClaimed);
    return group;
}

void selectClassDefinition(SelectionContext& context)
{
    const Function& function = context.function();
    Vector<RecordedMember> matches;
    for (ValueID id : context.valuesWith(Opcode::DefineMethod)) {
        if (isListed(context, id) && isCreatedByClassRecord(context, function.at(id)))
            matches.append(RecordedMember { claimRecordedMember(context, id) });
    }

    context.rewriteGroups(SelectionRuleID::ClassDefinition, matches, [](GroupRewriter& rewriter, const RecordedMember&) {
        rewriter.eraseRoot();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

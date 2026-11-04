/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include <array>
#include <iterator>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr unsigned ruleIndexOf(SelectionRuleID id) { return static_cast<unsigned>(id) - 1; }

constexpr SelectionRule selectionRuleRows[] = {
#define AOT_SELECTION_RULE_ROW(name, kind) { SelectionRuleID::name, SelectionRuleKind::kind, select##name },
    FOR_EACH_AOT_SELECTION_RULE(AOT_SELECTION_RULE_ROW)
#undef AOT_SELECTION_RULE_ROW
};

constexpr std::array<SelectionRule, numberOfSelectionRules> createRulesByPriority()
{
    std::array<SelectionRule, numberOfSelectionRules> result { };
    for (const SelectionRule& row : selectionRuleRows)
        result[ruleIndexOf(row.id)] = row;
    return result;
}

constexpr std::array<SelectionRule, numberOfSelectionRules> rulesByPriority = createRulesByPriority();

constexpr bool everyRuleHasOneRow()
{
    if (std::size(selectionRuleRows) != numberOfSelectionRules)
        return false;
    for (unsigned index = 0; index < numberOfSelectionRules; ++index) {
        if (!rulesByPriority[index].apply || ruleIndexOf(rulesByPriority[index].id) != index)
            return false;
    }
    return true;
}

static_assert(everyRuleHasOneRow());

} // anonymous namespace

std::span<const SelectionRule> selectionRules()
{
    return rulesByPriority;
}

const SelectionRule& ruleOf(SelectionRuleID id)
{
    RELEASE_ASSERT(id != SelectionRuleID::None);
    return rulesByPriority[ruleIndexOf(id)];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class SelectionContext;

enum class SelectionRuleKind : uint8_t { Optional, Required };

#define FOR_EACH_AOT_SELECTION_RULE(v) \
    v(PlannedSlots, Required) \
    v(ClassDefinition, Required) \
    v(DescriptorLiteral, Optional) \
    v(AccessorGroup, Optional) \
    v(SlotStoreRun, Optional) \
    v(PropertyRun, Optional) \
    v(MethodCall, Optional) \
    v(CheckedPropertyRead, Optional) \
    v(PropertyReadAndTest, Optional) \
    v(EncodedVariableTest, Optional) \
    v(PropertyChain, Optional) \
    v(PropertyPair, Optional) \
    v(SubstringView, Optional) \
    v(SwitchString, Optional) \
    v(SwitchNumber, Optional) \
    v(StringLiteralForms, Optional) \
    v(HoistStringCharacters, Optional) \
    v(SharedStringOrder, Optional) \
    v(IntegerBound, Optional) \
    v(FreshPromiseFulfillment, Optional) \
    v(ResolvedCallTarget, Optional) \
    v(RegExpLiteralMatch, Optional) \
    v(ThrowNewError, Optional) \
    v(InlineBuiltin, Required) \
    v(StringBuilderParts, Required) \
    v(ArrayLiteralStorage, Optional) \
    v(DataEncoding, Required) \
    v(ClosureForm, Required) \
    v(NumberPaths, Optional) \
    v(OperatorForm, Required) \
    v(TestAndBranch, Optional)

inline constexpr unsigned numberOfSelectionRules = numberOfEnumerators<SelectionRuleID> - 1;
static_assert(SelectionRuleID { } == SelectionRuleID::None);

struct SelectionRule {
    SelectionRuleID id;
    SelectionRuleKind kind;
    void (*apply)(SelectionContext&);
};

std::span<const SelectionRule> selectionRules();
const SelectionRule& ruleOf(SelectionRuleID);

#define AOT_DECLARE_SELECTION_RULE(name, kind) void select##name(SelectionContext&);
FOR_EACH_AOT_SELECTION_RULE(AOT_DECLARE_SELECTION_RULE)
#undef AOT_DECLARE_SELECTION_RULE

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

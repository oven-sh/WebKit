/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTPass.h"
#include "AOTPropertyAccessElements.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The test in front of an element access that has no arm for an absent element: where it fails, the access is made by the generic copy.
enum class PresenceTest : uint8_t { InBounds, InBoundsWithHolesAsNaN, ElementPresent, InsideView };

struct PresenceQuery {
    ValueID access;
    BlockID block;
    ValueID receiver;
    Access kind { Access::Read };
    ElementAccessForm form { ElementAccessForm::ByKey };
    ArrayStorage arrayStorage { ArrayStorage::Other };
    TypedArrayType typedArrayType { NotTypedArray };
    bool keyIsInteger { false };
    bool isDense { false };
};

// Nothing where the access keeps its arm for an absent element.
std::optional<PresenceTest> choosePresenceTest(FunctionPassContext&, const PresenceQuery&);

// A wrong "no" costs speed only: the failed test takes the generic copy.
bool isTestedForAbsence(FunctionAnalyses&, ValueID read);
// Undefined converts to NaN, and the hole of Double storage is a NaN.
bool everyUseConvertsToNumber(const Program&, FunctionAnalyses&, ValueID read);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

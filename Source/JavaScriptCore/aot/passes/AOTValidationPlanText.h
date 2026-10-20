/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class StringArena;

Vector<uint8_t> writeValidationPlan(const Program&, ExpectedResultID, uint32_t rootNode);
Vector<uint8_t> writeValidationRule(const Program&, const ValidationRule&);

StringID validationPlanText(const Program&, StringArena&, ExpectedResultID, uint32_t rootNode);
StringID validationRuleText(const Program&, StringArena&, const ValidationRule&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

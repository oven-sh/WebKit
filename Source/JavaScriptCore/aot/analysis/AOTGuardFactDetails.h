/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTGuardAvailability.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr uint32_t subjectOperandIndex = 0;

Vector<LayoutID, 4> layoutsProvenBy(const Program&, const Function&, const GuardFact&);
bool provenLayoutsAreSubsetOf(const Program&, const Function&, const GuardFact&, const GuardFact& other);
bool provesSlotFilled(const Program&, const Function&, const GuardFact&, PropertyKeyID);
bool guardProvesSlotFilled(FunctionAnalyses&, const Value& slotOperation);
bool provesForUser(FunctionAnalyses&, const GuardFact&, Proof, const Value& user);

bool testReadsOnlySubject(const Program&, const Function&, const Value& test);
// The subject, unless the fact also depends on what another object holds.
ValueID exclusiveSubjectOf(FunctionAnalyses&, const GuardFact&);

// The subject is a block parameter that took the place of the value that the test, the switch or the allocation behind the fact names.
bool hasRenamedSubject(FunctionAnalyses&, const GuardFact&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

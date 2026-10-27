/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTGuardAvailability.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr uint32_t subjectOperandIndex = 0;

Vector<LayoutID, 4> layoutsProvenBy(const Program&, const Function&, const GuardFact&);
bool provenLayoutsAreSubsetOf(const Program&, const Function&, const GuardFact&, const GuardFact& other);
bool provesSlotFilled(const Program&, const Function&, const GuardFact&, PropertyKeyID);
bool guardProvesSlotFilled(FunctionAnalyses&, const Value& slotOperation);
bool provesForUser(FunctionAnalyses&, const GuardFact&, Proof, const Value& user);
bool provesAsMuchAs(const Program&, const Function&, ValueID guard, ValueID other);
ArrayStorage arrayStorageOfNewArray(const Program&, const Function&, const Value& allocation);
bool allocatesWritableStorage(const Function&, const Value& allocation);

bool testReadsOnlySubject(const Program&, const Function&, const Value& test);
ValueID exclusiveSubjectOf(FunctionAnalyses&, const GuardFact&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

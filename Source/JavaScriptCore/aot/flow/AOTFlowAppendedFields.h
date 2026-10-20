/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResultFamilies.h"

namespace JSC { namespace AOT { namespace Typed {

bool canAppendFieldsTo(const FlowFamilyInputs&, LayoutID);
// The keys that some reached operation stores on the object as plain data properties.
void appendStoredKeys(const FlowFamilyInputs&, FlowObjectID, Vector<PropertyKeyID>&);
// Sorts the keys and keeps those that can name a field which the layout and its proven bases do not declare.
void keepCandidateKeys(const FlowFamilyInputs&, std::span<const PropertyKeyID> symbolsCreatedOnce, LayoutID, Vector<PropertyKeyID>&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

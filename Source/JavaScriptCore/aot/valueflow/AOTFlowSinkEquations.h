/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OperandUses escapingUses { OperandUse::Retains, OperandUse::Returns, OperandUse::RetainsContents, OperandUse::PassesToUserCode };

inline constexpr OperandUses mutatingUses { OperandUse::WritesContents, OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor, OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

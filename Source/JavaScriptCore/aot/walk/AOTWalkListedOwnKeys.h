/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"
#include "AOTWalkLinks.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet opcodesThatBuildOwnKeyListTakes { Opcode::Call };
bool buildOwnKeyListOfItsOpcodes(Walker&, const BodyValue&);
void noteListedOwnKeys(Walker&, ValueID call);
ALWAYS_INLINE bool buildOwnKeyList(Walker& walker, const BodyValue& value) { return value.opcode() == Opcode::Call && value.as<Opcode::Call>().argumentsOperands().size() == 1 && buildOwnKeyListOfItsOpcodes(walker, value); }
template<Opcode known> ALWAYS_INLINE bool buildOwnKeyList(Walker& walker, const BodyValueOf<known>& value) { return known == Opcode::Call && value.template as<Opcode::Call>().argumentsOperands().size() == 1 && buildOwnKeyListOfItsOpcodes(walker, value); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

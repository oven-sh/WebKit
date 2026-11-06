/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTUseLists.h"

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet concatenationOpcodes { Opcode::Add, Opcode::Strcat };

bool isStringConcatenation(const Program&, const Function&, const Value&);
bool isStringBuilderPart(const Type&);
bool canBeMarkedAsLikelyKey(const Program&, const Function&, const Value&);
void markAsLikelyKey(Function&, ValueID);
bool isKeyOperand(const Program&, const Function&, const Use&);
// Of a string that is marked as a likely key: on the usual paths it is the key of accesses to properties and of nothing else, so it will be asked for its name.
bool isOnlyKeyOfProperties(const Function&, const UseLists&, ValueID string);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

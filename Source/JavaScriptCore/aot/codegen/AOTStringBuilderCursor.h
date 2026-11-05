/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOpcode.h"
#include "AOTStringBuilderWords.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <span>
#include <wtf/text/Latin1Character.h>

namespace JSC { namespace AOT { namespace Typed {

using CursorBytes = Vector<Latin1Character, 32>;

constexpr bool renewsReservation(Opcode opcode)
{
    return opcode == Opcode::StringBuilderReserve || opcode == Opcode::JSONAppendString || opcode == Opcode::JSONAppendValue;
}

bool canWriteJSONKeyAtCursor(const TypeArena&, StringID key);
CursorBytes bytesOfJSONKey(const TypeArena&, StringID key, bool opensObject, bool followsMember);
std::span<const Latin1Character> bytesOfJSONToken(JSONToken);
uint32_t bytesTakenFromReservation(const TypeArena&, Opcode cursorOperation, StringID key);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

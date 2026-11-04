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

// What the operations of the family JSONWrite write at a cursor. The walk counts the bytes of a reservation by these answers and the emitters write by them, so neither can write past what the other has reserved.

using CursorBytes = Vector<Latin1Character, 32>;

// Behind these the cursor has maxStringBuilderReservation bytes of room, whatever it had in front.
constexpr bool renewsReservation(Opcode opcode)
{
    return opcode == Opcode::StringBuilderReserve || opcode == Opcode::JSONAppendString || opcode == Opcode::JSONAppendValue;
}

// The name is Latin-1 once it is escaped, and fits a reservation with its quotes, its colon and what stands in front.
bool canWriteJSONKeyAtCursor(const TypeArena&, StringID key);
CursorBytes bytesOfJSONKey(const TypeArena&, StringID key, bool opensObject, bool followsMember);
std::span<const Latin1Character> bytesOfJSONToken(JSONToken);
// `key`: the name of a JSONAppendKey. Stores are whole words, so more is taken than is written.
uint32_t bytesTakenFromReservation(const TypeArena&, Opcode cursorOperation, StringID key);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

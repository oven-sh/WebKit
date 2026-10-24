/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTStringBuilderWords.h"
#include "AOTVector.h"
#include <span>
#include <wtf/text/Latin1Character.h>

namespace JSC { namespace AOT { namespace Typed {

using CursorBytes = Vector<Latin1Character, 32>;

constexpr bool renewsReservation(Opcode opcode)
{
    return opcode == Opcode::StringBuilderReserve || opcode == Opcode::JSONAppendString || opcode == Opcode::JSONAppendValue;
}

bool canWriteJSONKeyAtCursor(const Program&, StringID key);
CursorBytes bytesOfJSONKey(const Program&, const Value& appendKey);
std::span<const Latin1Character> bytesOfJSONToken(JSONToken);
uint32_t bytesTakenFromReservation(const Program&, const Value& cursorRow);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTResultRules.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

class BodyIndex;
class BodyIndexTable;

template<typename Operation> Effects effectsOf(const TypingContext&, const Operation&, OperandTypes, TypeID result, const Effects* foundByCaller = nullptr);
template<typename Terminator> Effects effectsOfTerminator(const TypingContext&, const Terminator&, OperandTypes, const Effects* foundByCaller = nullptr);

template<typename Operation> bool namespaceReadRunsNoCode(const Program&, const Operation&);

Effects effectsOfOperator(const TypingContext&, Opcode, std::span<const TypeID> operandTypes, TypeID result);
Effects effectsOfProtocolsOn(const TypingContext&, OptionSet<Protocol>, TypeID operand);
bool operandsMayRunCode(Opcode);
bool letsNothingElseRun(Opcode);

inline bool neitherThrowsNorRunsCode(const Effects& effects) { return effects.isRemovableWhenUnused(); }
inline bool neitherThrowsNorRunsCode(EffectTraits traits) { return traits.isRemovableWhenUnused(); }

bool hasTextByItself(const Type&);
uint64_t longestTextOf(TypeReader, const Type&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

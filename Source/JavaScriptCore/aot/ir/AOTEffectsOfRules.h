/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffects.h"
#include "AOTEffectsOfProtocols.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

struct Type;

EffectsOnEdges effectsOfOperator(const EffectContext&, const Function&, const Value&, const Effects& declared);
EffectsOnEdges effectsOfOperator(const EffectContext&, Opcode, std::span<const Type> operandTypes, const Type& result);
EffectsOnEdges effectsOfCall(const EffectContext&, const Function&, const Value&, const Effects& declared);

EffectsOnEdges effectsOfUnknownCode(const EffectContext&);
EffectsOnEdges effectsOfCalling(const EffectContext&, FunctionID);
EffectsOnEdges effectsOfCalling(const EffectContext&, BuiltinID, BuiltinCallMode);
EffectsOnEdges effectsOfCallees(const EffectContext&, const CalleeList&, BuiltinCallMode);

EffectsOnEdges effectsOfOverload(const EffectContext&, BuiltinOverloadID, TypeID receiver, std::span<const TypeID> arguments);
EffectsOnEdges effectsOfOverload(const EffectContext&, BuiltinOverloadID, BuiltinCallForm, TypeID receiver, std::span<const TypeID> arguments);
EffectsOnEdges effectsOfTypedResult(const EffectContext&, BuiltinOverloadID, ExpectedResultID);
Effects ownEffectsOf(const BuiltinOverload&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

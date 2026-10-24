/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTKnownValue.h"
#include "AOTPass.h"
#include "AOTStringRanges.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr OpcodeSet simplifiedIntrinsics {
    Opcode::IsPristineArray, Opcode::ToLength, Opcode::ToIntegerOrInfinity, Opcode::ToInt32, Opcode::ToUint32, Opcode::ToObject, Opcode::ToThis,
    Opcode::StepGenerator, Opcode::HasJSType
};
inline constexpr OpcodeSet constantListSourceOpcodes { Opcode::NewArray, Opcode::Data, Opcode::GetModuleVariable };
inline constexpr OpcodeSet constantListSpreadOpcodes { Opcode::AppendSpread, Opcode::CallBuiltin };
inline constexpr OpcodeSet constantListCallOpcodes { Opcode::Call, Opcode::CallBuiltin, Opcode::CallDirect };
inline constexpr OpcodeSet constantListUserOpcodes = constantListSpreadOpcodes | constantListCallOpcodes;

unsigned simplifyIntrinsics(FunctionPassContext&);
unsigned simplifyOwnPropertyTests(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifyPromiseAllOfLiterals(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifyPromiseResolve(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifyPrimitiveCalls(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifyInt32Conversions(FunctionPassContext&);
unsigned simplifyStringReads(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifyConcatenatingCalls(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifyMatches(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifyLastPiecesOfSplits(FunctionPassContext&, const BuiltinCallSites&);
unsigned leaveURLsUnbuilt(FunctionPassContext&);
unsigned leaveRangesOfBytesUnmade(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifySearchesOfLiterals(FunctionPassContext&);
unsigned takeConstantListsApart(FunctionPassContext&);
unsigned simplifySplitsOfConstants(FunctionPassContext&, const BuiltinCallSites&);
unsigned simplifyConcatenationsOfLiterals(FunctionPassContext&);

bool keepsTypeOf(FunctionPassContext&, ValueID original, const Value& rewritten);
void replaceKeepingType(FunctionPassContext&, ValueID original, Value&& rewritten);
std::optional<ValueID> findSingleUseDefinition(FunctionPassContext&, ValueID operand, ValueID user);
std::optional<Vector<KnownValue, 8>> findConstantsOfListFor(FunctionPassContext&, ValueID operand, ValueID user);
Value makeConstant(KnownValue);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "TypedArrayType.h"

namespace JSC { namespace AOT { namespace Typed {

struct PristineRequirement {
    BuiltinClassID builtinClass;
    PristineAspectSet aspects;

    explicit operator bool() const { return !aspects.isEmpty(); }

    friend bool operator==(const PristineRequirement&, const PristineRequirement&) = default;
};

enum class TypeFit : uint8_t { Unknown, Outside, Inside };

struct OperandFit {
    TypeFit fit { TypeFit::Unknown };
    PristineRequirement unprovenPristine;
};

struct BuiltinTypeInputs {
    const TypingContext& context;
    const BuiltinOperandTypes* operands { nullptr };
};

Type typeOfBuiltinType(const BuiltinTypeInputs&, BuiltinTypeID);
Type typeOfBuiltinTypePart(const BuiltinTypeInputs&, const BuiltinTypePart&, BuiltinFreshness);
Type typeOfBuiltinLayout(const TypingContext&, BuiltinLayoutID);

uint16_t primitivesOfBuiltinType(const BuiltinType&);
Range computeResultRange(BuiltinRangeRule, std::span<const Range> operandRanges);

OperandFit fitOf(const TypingContext&, TypeID operandOrNotPassed, BuiltinTypeID tested);
bool fitsIfPristine(const TypingContext&, TypeID operandOrNotPassed, BuiltinTypeID tested);
PristineRequirement pristineRequirementOf(const BuiltinTable&, const Type& operand, BuiltinTypeID tested);
PristineAspectSet pristineAspectsProvenBy(const Type&, BuiltinClassID);

Type typeOfBuiltinConstant(const BuiltinTable&, BuiltinID);
TypedArrayType typedArrayTypeOf(const BuiltinTable&, const Type&);
bool hasOnlyMapsAndSets(const Program&, const Type&);

// A string that was just made has no hash yet.
inline constexpr OpcodeSet makersOfStrings { Opcode::Add, Opcode::CallBuiltin, Opcode::Strcat, Opcode::Substring, Opcode::SubstringOrUndefined, Opcode::StringBuilderFinish, Opcode::StringBuilderToString, Opcode::ToString };

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "AOTCodeRole.h"
#include "AOTDeclaredType.h"
#include "AOTRepresentation.h"
#include <limits>
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Plan;
class Program;
class Walker;

struct RepresentationContext {
    const Program& program;
    const Plan& plan;
    TypeReader types;
    CodeUnit unit;
};

struct OperandForms {
    std::span<const TypeID> provenTypes;
    std::span<const Representation> representations;

    TypeID typeAt(uint32_t operandIndex) const { return operandIndex < provenTypes.size() ? provenTypes[operandIndex] : TypeID(); }
    Representation representationAt(uint32_t operandIndex) const { return operandIndex < representations.size() ? representations[operandIndex] : Representation::Unchosen; }
};

inline constexpr RepresentationSet integerRepresentations { Representation::Int32, Representation::Int64 };
inline constexpr RepresentationSet numberRepresentations { Representation::Int32, Representation::Int64, Representation::Double };
inline constexpr RepresentationSet optionalRepresentations { Representation::OptionalInt64, Representation::OptionalDouble };

inline constexpr OpcodeSet constantsInFormOfUses {
    Opcode::Undefined, Opcode::Null, Opcode::Absent, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::KeyConstant
};

inline constexpr unsigned numberOfLowestBitsOfInt32 = std::numeric_limits<int64_t>::digits - (std::numeric_limits<double>::digits - 1);

inline bool isMaskOfLowestBitsOfInt32(const Type& mask)
{
    return mask.isInteger() && mask.integers.minimum >= 0 && mask.integers.maximum < (int64_t { 1 } << numberOfLowestBitsOfInt32);
}

constexpr bool hasRegister(Representation representation)
{
    return representation != Representation::Unchosen && representation != Representation::Void;
}

constexpr bool isSameWord(Representation from, Representation to)
{
    if (from == to)
        return true;
    return (from == Representation::Cell && to == Representation::JSValue) || (from == Representation::JSValue && to == Representation::Cell);
}

template<typename Operation> Representation wantedRepresentationOf(const RepresentationContext&, const Operation&, const OperandForms&, uint32_t operandIndex);
template<typename Operation> Representation resultRepresentationOf(const RepresentationContext&, const Operation&, const OperandForms&, TypeID result, uint32_t resultIndex = 0);
template<typename Operation> bool isEqualityOfNumbers(const RepresentationContext&, const Operation&, const OperandForms&);
using WantedRepresentations = Vector<Representation, 8>;
WantedRepresentations wantedRepresentationsOf(Walker&, const Draft&);
enum class OperandsAreTaken : uint8_t {
    AsBuilt,
    ByKind,
    ByKindOrAsCell,
    ByKindWhereNotDeclared,
    ByFormOfOperator,
    ByRule,
};
extern const std::array<OperandsAreTaken, numberOfOpcodes> howOperandsAreTakenByOpcode;
inline OperandsAreTaken howOperandsAreTakenBy(Opcode opcode) { return howOperandsAreTakenByOpcode[static_cast<unsigned>(opcode)]; }
ALWAYS_INLINE constexpr Representation formTakenByEveryOperandOf(OperatorForm form, bool isBitwise)
{
    switch (form) {
    case OperatorForm::Generic:
    case OperatorForm::NullableString:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
    case OperatorForm::NullableStrings:
        return Representation::JSValue;
    case OperatorForm::Integers:
        return isBitwise ? Representation::Int32 : Representation::Unchosen;
    case OperatorForm::Booleans:
        return Representation::Boolean;
    case OperatorForm::Strings:
    case OperatorForm::BigInts:
    case OperatorForm::String:
        return Representation::Cell;
    default:
        return Representation::Unchosen;
    }
}
inline constexpr uint64_t kindsTakenAsSchemaWants = []() consteval {
    uint64_t kinds = 0;
    for (OperandKind kind : { OperandKind::Any, OperandKind::Boolean, OperandKind::Number, OperandKind::String, OperandKind::PropertyKey, OperandKind::Object,
        OperandKind::Callable, OperandKind::Array, OperandKind::Layout, OperandKind::Environment, OperandKind::EvalOperand })
        kinds |= uint64_t { 1 } << static_cast<unsigned>(kind);
    return kinds;
}();
static_assert(numberOfEnumerators<OperandKind> <= 64);
ALWAYS_INLINE Representation representationWantedByKind(TypeReader types, OperandKind kind, TypeID id)
{
    if (kind == OperandKind::Results)
        return Representation::Void;
    if (kind == OperandKind::Declared || kind == OperandKind::Convertible)
        return Representation::Unchosen;
    bool isTakenAsSchemaWants = kindsTakenAsSchemaWants >> static_cast<unsigned>(kind) & 1;
    if (holdsOnlyPrimitives(id)) {
        if (kind == OperandKind::Integer)
            return integersInID(id).isSubsetOf(Range::int32()) ? Representation::Int32 : Representation::Int64;
        if (isTakenAsSchemaWants)
            return wantsOf(kind);
        return representationWantedBy(kind, types.at(id));
    }
    const Type& type = types.recordOf(id);
    if (!type.opaque) {
        if (kind == OperandKind::Integer)
            return type.integers.isSubsetOf(Range::int32()) ? Representation::Int32 : Representation::Int64;
        if (isTakenAsSchemaWants)
            return wantsOf(kind);
    }
    return representationWantedBy(kind, type);
}
WantedRepresentations wantedRepresentationsOf(Walker&, const TerminatorDraft&);
Representation resultRepresentationOf(Walker&, const Draft&);
Representation resultRepresentationOf(Walker&, const Draft&, TypeID proven);
extern const std::array<Representation, numberOfOpcodes> formsOfResultsByOpcode;
inline Representation formOfResultOfEvery(Opcode opcode) { return formsOfResultsByOpcode[static_cast<unsigned>(opcode)]; }
extern const std::array<Representation, numberOfOpcodes> formsOfResultsOfOperatorsByOpcode;
extern const OpcodeSet opcodesWhoseResultsHaveFormsOfTheirTypes;

Representation representationStoredInSlot(const RepresentationContext&, PropertyKeyID, LayoutSetID receivers);
Representation representationLoadedFromSlot(const RepresentationContext&, PropertyKeyID, LayoutSetID receivers, TypeID loaded);
Representation representationOfCapture(const RepresentationContext&, FunctionID, uint32_t capture);
Representation representationOfVariable(const RepresentationContext&, VariableID);
Representation representationOfArrayElement(ArrayStorage);
Representation representationOfArgument(const RepresentationContext&, CodeUnit callee, uint32_t entryParameter);
Representation representationOfResult(const RepresentationContext&, CodeUnit callee, uint32_t resultIndex);

struct ConversionStep {
    Opcode opcode { Opcode::Box };
    Representation to { Representation::JSValue };

    friend bool operator==(const ConversionStep&, const ConversionStep&) = default;
};

using ConversionPath = Vector<ConversionStep, 2>;

std::optional<ConversionPath> findConversion(TypeReader, TypeID proven, Representation from, Representation to);
struct ConversionInSteps {
    std::array<ConversionStep, 2> steps;
    uint8_t numberOfSteps { 0 };
    bool isFound { false };
};
ConversionInSteps findConversionInSteps(TypeReader, TypeID proven, Representation from, Representation to);

inline constexpr unsigned maxLengthOfTextComparedInPlace = 32;

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

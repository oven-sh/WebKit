/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTResultRules.h"
#include "AOTSchemaResultRules.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

TypeID naturalTypeOfOperands(const TypingContext&, Opcode, NaturalTable, OperandTypes);
TypeID numberResultOf(TypeArena&, Opcode, OperandTypes);
TypeID integerQuotientType(TypeArena&, Range dividends, Range divisors, QuotientRounding);
TypeID wholeRemainderType(TypeArena&, Range dividends, Range divisors);

inline constexpr OpcodeSet opcodesRefinedBeyondFixedNatural {
    Opcode::URShift, Opcode::ToNumber, Opcode::StrictEq, Opcode::ToBoolean, Opcode::LogicalNot, Opcode::ToString, Opcode::ToPropertyKey,
    Opcode::ToObject, Opcode::ToThis, Opcode::TypeTest, Opcode::GetTypedArrayElement, Opcode::GetTypedArrayLength, Opcode::GetStringLength, Opcode::DivideIntegers, Opcode::RemainderOfIntegers, Opcode::RemainderByReciprocal, Opcode::WholeDivisor, Opcode::RemainderByWholeDivisor, Opcode::ToInt32,
    Opcode::ToUint32, Opcode::ToIntegerOrInfinity, Opcode::ToLength, Opcode::IntegerBound, Opcode::GetSuperBase
};
inline const OpcodeSet& opcodesWithNaturalTypeRefinedByOperation() { return opcodesRefinedBeyondFixedNatural; }
template<typename Operation> TypeID refineFixedNatural(const TypingContext&, const Operation&, OperandTypes, TypeID upperBound);

TypeID tryTypeOfFieldRead(const TypingContext&, TypeID receiver, PropertyKeyID);
TypeID tryTypeOfGlobalBindingRead(const TypingContext&, PropertyKeyID, bool mayBeShadowed);
TypeID tryTypeOfSlotContents(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
TypeID typeOfSlotContentsInReceiver(const TypingContext&, TypeID contents, TypeID receiver, PropertyKeyID);
TypeID tryTypeOfSetterInSlot(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
TypeID tryTypeOfProvenHalfInSlot(const TypingContext&, LayoutSetID receivers, PropertyKeyID, bool isSetter);
TypeID tryTypeOfDeferringSlotContents(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
TypeID tryTypeAcceptedBySlots(const TypingContext&, LayoutSetID receivers, PropertyKeyID);
TypeID tryTypeOfContentsRead(const TypingContext&, TypeID container, ContentsSlot);
TypeID tryTypeOfElementRead(const TypingContext&, TypeID container, TypeID key);
TypeID tryTypeOfArrayElementRead(const TypingContext&, TypeID array, ArrayStorage storage, bool admitsInt32Storage, bool findsElement, bool readsHoleAsNaN);
TypeID tryTypeOfAwaited(const TypingContext&, TypeID awaited);

struct ResultRules {
    using Result = TypeID;

    const TypingContext& context;
    OperandTypes operands;
    ObjectID created;
    bool wasUnproven { false };

    template<typename Operation> TypeID none(const Operation&) { return TypeTable::never; }
    template<typename Operation> TypeID constant(const Operation& operation) { return typeOfConstant(context, operation, created); }
    template<typename Operation> TypeID natural(const Operation&, const NaturalType&);
    template<typename Operation> TypeID natural(const Operation&, NaturalTable);
    template<typename Operation> TypeID sameAs(const Operation&, uint32_t operandIndex);
    template<typename Operation> TypeID allocation(const Operation&, LayoutID);
    template<typename Operation> TypeID allocation(const Operation&, BuiltinClassID);
    template<typename Operation> TypeID allocation(const Operation&, BuiltinClassID, TypeID contents);
    template<typename Operation> TypeID allocation(const Operation&, BuiltinLayoutID);
    template<typename Operation> TypeID declared(const Operation&, VariableID);
    template<typename Operation> TypeID declared(const Operation&, LayoutID, uint32_t field);
    template<typename Operation> TypeID declared(const Operation&, PropertyKeyID);
    template<typename Operation> TypeID declared(const Operation&, FunctionID, uint32_t resultIndex);
    template<typename Operation> TypeID declared(const Operation&, LayoutID);
    template<typename Operation> TypeID declared(const Operation&, ModuleID);
    template<typename Operation> TypeID declared(const Operation& operation, TypeID type)
    {
        return opcodeOf(operation) == Opcode::Unspecified ? context.types.withPrimitives(type, Primitive::Unspecified) : type;
    }
    template<typename Operation> TypeID declared(const Operation&, BuiltinOverloadID, uint32_t resultIndex);
    template<typename Operation> TypeID declaredContents(const Operation&, uint32_t operandIndex, std::optional<ContentsSlot>);
    template<typename Operation> TypeID declaredResult(const Operation&, uint32_t operandIndex, uint32_t) { return operands[operandIndex]; }
    template<typename Operation> TypeID declaredParameter(const Operation&);
    template<typename Operation> TypeID unknown(const Operation&);
    template<typename Operation> TypeID cast(const Operation&, uint32_t operandIndex, TypeID castType);
    template<typename Operation> TypeID cast(const Operation&, uint32_t operandIndex, ExpectedType);
    template<typename Operation> TypeID cast(const Operation&, uint32_t operandIndex, ProvenType);
    template<typename Operation> TypeID cast(const Operation&, uint32_t operandIndex);
    template<typename Operation> TypeID resultOperand(const Operation&, uint32_t resultIndex);

    TypeID provenOrUnknown(TypeID, UnknownSeed);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

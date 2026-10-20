/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBlock.h"
#include "AOTResultRules.h"
#include "AOTSchemaResultRules.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

Type naturalTypeFrom(const NaturalType&);
Type naturalTypeOfValue(const TypingContext&, const Value&, NaturalTable);
Type refineFixedNatural(const TypingContext&, const Value&, const Type& upperBound);

std::optional<Type> tryTypeOfFieldRead(const TypingContext&, std::span<const LayoutID> receivers, PropertyKeyID);
std::optional<Type> tryTypeOfFieldRead(const TypingContext&, const Type& receiver, PropertyKeyID);
std::optional<Type> tryTypeOfContentsRead(const TypingContext&, const Type& container, ContentsSlot);
std::optional<Type> tryTypeOfElementRead(const TypingContext&, const Type& container, const Type& key);
std::optional<Type> tryTypeOfAwaited(const TypingContext&, const Type& awaited);

struct ResultRules {
    using Result = Type;

    const TypingContext& context;
    bool wasUnproven { false };

    Type none(const Instruction&);
    Type constant(const Value&);
    Type natural(const Value&, const NaturalType&);
    Type natural(const Terminator&, const NaturalType& type) { return naturalTypeFrom(type); }
    Type natural(const Value&, NaturalTable);
    Type sameAs(const Value&, uint32_t operandIndex);
    Type sameAs(const Terminator& terminator, uint32_t operandIndex) { return context.typeOf(terminator.operand(operandIndex)); }
    Type allocation(const Value&, LayoutID);
    Type allocation(const Value&, BuiltinClassID);
    Type allocation(const Value&, BuiltinClassID, TypeID contents);
    Type allocation(const Value&, BuiltinLayoutID);
    Type declared(const Value&, VariableID);
    Type declared(const Value&, LayoutID, uint32_t field);
    Type declared(const Value&, PropertyKeyID);
    Type declared(const Value&, FunctionID, uint32_t resultIndex);
    Type declared(const Value&, LayoutID);
    Type declared(const Value&, ModuleID);
    Type declared(const Value&, TypeID);
    Type declared(const Value&, BuiltinOverloadID, uint32_t resultIndex);
    Type declared(const Terminator&, BuiltinOverloadID, uint32_t resultIndex);
    Type declaredContents(const Value&, uint32_t operandIndex, std::optional<ContentsSlot>);
    Type declaredResult(const Value&, uint32_t operandIndex, uint32_t index);
    Type declaredParameter(const Value&);
    Type unknown(const Instruction&);
    Type cast(const Value&, uint32_t operandIndex, TypeID castType);
    Type cast(const Value&, uint32_t operandIndex);
    Type resultOperand(const Value&, uint32_t resultIndex);

private:
    Type provenOrUnknown(std::optional<Type>&&);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

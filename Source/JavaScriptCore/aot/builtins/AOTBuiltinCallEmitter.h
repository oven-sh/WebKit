/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTVector.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

struct BuiltinDirectFunctionBinding;

namespace Typed {

class BuiltinCallEmitter final : public BuiltinFamilyEmitter {
public:
    explicit BuiltinCallEmitter(CodeGenerator&);

    void generateCallBuiltin(const ValueContext&);
    void generateDivideIntegers(const ValueContext&);
    void generateToInt32(const ValueContext&);
    void generateToUint32(const ValueContext&);
    void generateToIntegerOrInfinity(const ValueContext&);
    void generateNewSetFromData(const ValueContext&);
    void generateNewMapFromData(const ValueContext&);
    void generateNewArgumentList(const ValueContext&);
    void generateSharedRegExpLiteral(const ValueContext&);
    void generateIsBuiltinLocationUnchanged(const ValueContext&);
    void generateIsFunctionOfBuiltin(const ValueContext&);
    void generateGetBuiltinValue(const ValueContext&);

    Boxed callBuiltinFunction(const OperationContext&, BuiltinOverloadID, std::optional<Boxed> receiver, std::span<const Boxed> arguments);
    CellPointer builtinCell(const OperationContext&, BuiltinCellKey);
    Boxed builtinValue(const OperationContext&, BuiltinCellKey);

private:
    Boxed readCachedGetter(const ValueContext&, const BuiltinOverload&, uint32_t offsetOfCachedValue);
    Boxed readOwnSlot(const ValueContext&, const BuiltinOverload&);
    Boxed loadProperty(CellPointer object, PropertyOffset);
    Boxed callCustomGetter(const OperationContext&, BuiltinOverloadID, Boxed receiver, bool receiverIsMatched);
    struct DirectOperands {
        Vector<LValue, 8> words;
        Vector<LValue, 4> keptAlive;
        bool takesFrame { false };
    };
    DirectOperands directOperandsOf(const ValueContext&, const BuiltinDirectFunctionBinding&);
    void appendBytesOf(DirectOperands&, ValueID view, std::span<const ValueID> bounds = { });
    Boxed callDirectFunction(const ValueContext&, uint32_t number);
    StubCallResult callDirectFunctionReturningFields(const ValueContext&, uint32_t number);
    void generateFieldsOfDirectEntry(const ValueContext&);
    CellPointer makeObjectOfDirectEntry(const ValueContext&, uint32_t number);
    PropertyKeyID keyOf(const BuiltinOverload&) const;
    CellPointer calleeOf(const OperationContext&, const BuiltinOverload&);
    Integer divide(Integer dividend, Range dividends, Integer divisor, Range divisors);
    Integer roundedQuotient(QuotientRounding, Integer dividend, Range dividends, Integer divisor, Range divisors);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

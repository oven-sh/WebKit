/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTVector.h"
#include "PropertyOffset.h"
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
    void generateRemainderOfIntegers(const ValueContext&);
    void generateToInt32(const ValueContext&);
    void generateToUint32(const ValueContext&);
    void generateToIntegerOrInfinity(const ValueContext&);
    void generateNewSetFromData(const ValueContext&);
    void generateNewMapFromData(const ValueContext&);
    void generateNewFormatterWithPattern(const ValueContext&);
    void generateSetUpFormatterWithOptions(const ValueContext&);
    void generateFormatDateWithPattern(const ValueContext&);
    void generateFormatNumberWithPattern(const ValueContext&);
    void generateNewArgumentList(const ValueContext&);
    // canShareRegExpLiteral() has said yes. `at`: the operation for whose sake the object is fetched.
    CellPointer sharedRegExpLiteral(const ValueContext& at, const Operation& literal);
    void generateSharedRegExpLiteral(const ValueContext&);
    // generateSharedRegExpLiteral() finds the cell of the literal.
    bool canShareRegExpLiteral(const Operation& literal) const;
    void generateIsFunctionOfBuiltin(const ValueContext&);
    void generateGetBuiltinValue(const ValueContext&);
    void generateGetWatchedBuiltinValue(const ValueContext&);
    void generateInitializeBuiltin(const ValueContext&);

    Boxed callBuiltinFunction(const OperationContext&, BuiltinOverloadID, std::optional<Boxed> receiver, std::span<const Boxed> arguments);
    CellPointer builtinCell(const OperationContext&, BuiltinCellKey);
    Boxed builtinValue(const OperationContext&, BuiltinCellKey, RuntimeOperation fills = RuntimeOperation::FillBuiltinValue);
    // Of whatever is typed by the rows of the built-in that it calls.
    void stopUnlessNativeResultIsAsWritten(const ValueContext&, Boxed result, BuiltinID called, bool constructs);

private:
    Boxed readCachedGetter(const ValueContext&, const BuiltinOverload&, uint32_t offsetOfCachedValue);
    Boxed loadProperty(CellPointer object, PropertyOffset);
    Vector<BuiltinOverloadID, 4> ownSlotsReadBy(const ValueContext&, const BuiltinOverload&);
    Boxed readOwnSlot(const ValueContext&, const BuiltinOverload&);
    Boxed callCustomGetter(const OperationContext&, BuiltinOverloadID, Boxed receiver, bool receiverIsMatched);
    struct DirectOperands {
        Vector<LValue, 8> words;
        Vector<LValue, 4> keptAlive;
        bool takesFrame { false };
    };
    DirectOperands directOperandsOf(const ValueContext&, const BuiltinDirectFunctionBinding&);
    // `bounds`: empty for the whole view, else where the part starts and ends, in bytes.
    void appendBytesOf(DirectOperands&, ValueID view, std::span<const ValueID> bounds = { });
    Boxed callDirectFunction(const ValueContext&, uint32_t number);
    StubCallResult callDirectFunctionReturningFields(const ValueContext&, uint32_t number);
    CellPointer makeObjectOfDirectEntry(const ValueContext&, uint32_t number);
    PropertyKeyID keyOf(const BuiltinOverload&) const;
    CellPointer calleeOf(const OperationContext&, const BuiltinOverload&);
    Integer divide(Integer dividend, Range dividends, Integer divisor, Range divisors);
    Integer roundedQuotient(QuotientRounding, Integer dividend, Range dividends, Integer divisor, Range divisors);
    Integer remainderByConstant(Integer dividend, Range dividends, int64_t magnitudeOfDivisor);
    Word32 int32BitsOf(ValueID number, bool givesOnlyLowestBits);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

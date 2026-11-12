/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinFamilyEmitter.h"
#include "AOTBuiltinTableRealm.h"
#include "AOTOperationData.h"
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
    void generateMultiplyIntegers(const ValueContext&);
    void generateMultiplyAddIntegers(const ValueContext&);
    void generateReciprocalOfDivisor(const ValueContext&);
    void generateRemainderByReciprocal(const ValueContext&);
    void generateWholeDivisor(const ValueContext&);
    void generateRemainderByWholeDivisor(const ValueContext&);
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
    void generateNewArrayOfGiven(const ValueContext&);
    CellPointer sharedRegExpLiteral(const ValueContext& at, const Operation& literal);
    void generateSharedRegExpLiteral(const ValueContext&);
    bool canShareRegExpLiteral(const Operation& literal) const;
    void generateGetBuiltinValue(const ValueContext&);
    void generateGetWatchedBuiltinValue(const ValueContext&);
    void generateGetWatchedBuiltinValueOrAbsent(const ValueContext&);
    void generateInitializeBuiltin(const ValueContext&);

    struct ExpectedResult {
        std::optional<HostResult> result { };
        bool isCheckedByCallee { false };
    };
    Boxed callBuiltinFunction(const OperationContext&, BuiltinOverloadID, std::optional<Boxed> receiver, std::span<const Boxed> arguments, ExpectedResult* = nullptr);
    std::optional<HostResult> hostResultAsWritten(const ValueContext&);
    void createStaticCallee(const ValueContext&);
    CellPointer builtinCell(const OperationContext&, BuiltinCellKey);
    Boxed builtinValue(const OperationContext&, BuiltinCellKey, RuntimeOperation fills = RuntimeOperation::FillBuiltinValue, const ValueContext* heldAgainstItsRow = nullptr);
    void stopUnlessNativeResultIsAsWritten(const ValueContext&, Boxed result, BuiltinID called, bool constructs);

    struct DivisionOfSameOperands {
        ValueID division;
        bool isAhead { false };
    };
    std::optional<DivisionOfSameOperands> findDivisionOfSameOperands(const ValueContext& remainder);
    bool isEmittedWithBlock(BlockID block, BlockID later);
    Integer divide(Integer dividend, Range dividends, Integer divisor, Range divisors);
    void noteQuotient(ValueID division, Integer quotient) { m_quotientsOfDivisions.set(division.index(), quotient.value()); }

private:
    Boxed callCustomGetter(const OperationContext&, BuiltinOverloadID, Boxed receiver, bool receiverIsMatched);
    struct DirectOperands {
        Vector<LValue, 8> words;
        Vector<LValue, 4> keptAlive;
        bool takesFrame { false };
    };
    DirectOperands directOperandsOf(const ValueContext&, const BuiltinDirectFunctionBinding&);
    Boxed callDirectFunction(const ValueContext&, uint32_t number);
    PropertyKeyID keyOf(const BuiltinOverload&) const;
    CellPointer calleeOf(const OperationContext&, const BuiltinOverload&);
    void findDivisions();
    Integer roundedQuotient(QuotientRounding, Integer dividend, Range dividends, Integer divisor, Range divisors);
    Integer remainderByConstant(Integer dividend, Range dividends, int64_t magnitudeOfDivisor);
    void holdOperandsOfDivisionToToldRanges(const ValueContext&, Integer dividend, Range dividends, Integer divisor, Range divisors);
    struct LowHalfOfRemainder {
        LValue reciprocal { nullptr };
        LValue base { nullptr };
        uint64_t offset { 0 };
        LValue lowHalf { nullptr };
    };
    LowHalfOfRemainder m_lowHalfOfLastRemainder;
    using HashOfOperations = DefaultHash<uint32_t>;
    using TraitsOfOperations = WTF::UnsignedWithZeroKeyHashTraits<uint32_t>;
    struct PlacedDivision {
        ValueID division;
        uint32_t block { 0 };
        uint32_t place { 0 };
    };
    using OperandsOfDivision = std::pair<uint32_t, uint64_t>;
    OperandsOfDivision operandsOfDivision(ValueID dividend, ValueID divisor);
    HashMap<OperandsOfDivision, Vector<PlacedDivision, 1>> m_divisionsByOperands;
    HashMap<uint32_t, uint32_t, HashOfOperations, TraitsOfOperations> m_placesOfRemainders;
    HashMap<uint32_t, LValue, HashOfOperations, TraitsOfOperations> m_quotientsOfDivisions;
    bool m_hasFoundDivisions { false };
    LValue lowHalfOfProductWithReciprocal(LValue reciprocal, LValue dividend);
    Word32 int32BitsOf(ValueID number, bool givesOnlyLowestBits);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

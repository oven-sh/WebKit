/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInt32Conversions.h"
#include "AOTResultRules.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isExactInteger(const Type& type, int64_t limit = maxExactInteger)
{
    return type.isInteger() && type.integers.isSubsetOf({ -limit, limit });
}

static bool canDivideAsIntegers(QuotientRounding rounding, const Type& dividend, const Type& divisor)
{
    int64_t limit = rounding == QuotientRounding::Round ? maxExactInteger >> 2 : maxExactInteger;
    if (!isExactInteger(dividend, limit) || !isExactInteger(divisor, limit))
        return false;

    Range dividends = dividend.integers;
    Range divisors = divisor.integers;
    if (dividends.minimum == dividends.maximum && divisors.minimum == divisors.maximum)
        return false;

    bool isDivisorPositive = divisors.minimum > 0;
    bool isNeverNegative = dividends.minimum >= 0 && isDivisorPositive;
    switch (rounding) {
    case QuotientRounding::Floor:
        return isDivisorPositive;
    case QuotientRounding::Ceil:
    case QuotientRounding::Truncate:
    case QuotientRounding::Round:
        return isNeverNegative;
    case QuotientRounding::ToInt32:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct Quotient {
    ValueID dividend;
    ValueID divisor;
};

struct RoundingCall {
    ValueID call;
    QuotientRounding rounding;
    Quotient quotient;
};

struct Int32Conversion {
    ProgramPoint point;
    ValueID user;
    uint32_t operandIndex;
    Quotient quotient;
};

class IntegerDivisionReducer {
public:
    explicit IntegerDivisionReducer(FunctionPassContext& context)
        : m_context(context)
        , m_function(context.function)
    {
    }

    PassOutcome run()
    {
        collectCandidates();
        size_t numberOfCandidates = m_roundingCalls.size() + m_int32Conversions.size();
        if (!numberOfCandidates)
            return PassOutcome::Unchanged;

        replaceRoundingCalls();
        insertInt32Quotients();
        m_context.analyses.didChangeValues();
        m_context.build.statistics().count(StatisticID::IntegerDivisionsReduced, numberOfCandidates);

        narrowValueTypes(TypePropagationInputs { m_context.program, m_context.id, m_function, m_context.analyses, m_context.types, m_context.build }, m_divisions.span());
        return PassOutcome::Changed;
    }

private:
    void collectCandidates()
    {
        for (BlockID block : m_context.analyses.cfg().reversePostOrder()) {
            const BlockValues& values = m_function.at(block).values;
            for (uint32_t index = 0; index < values.size(); ++index)
                collectCandidatesAt({ block, index }, values[index]);
        }
    }

    void collectCandidatesAt(ProgramPoint point, ValueID id)
    {
        const Value& value = m_function.at(id);
        if (int32ConvertingOperations.contains(value.opcode)) {
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                collectInt32Conversion(point, id, operand, operandIndex);
            });
            return;
        }
        if (value.opcode == Opcode::CallBuiltin)
            collectCandidatesInCall(id, value);
    }

    void collectCandidatesInCall(ValueID id, const Value& call)
    {
        std::optional<InlineBuiltin> builtin = m_context.program.builtins().at(call.overload()).code.implementation.inlineBuiltin;
        if (!builtin)
            return;

        switch (*builtin) {
        case InlineBuiltin::MathFloor:
            collectRoundingCall(id, call, QuotientRounding::Floor);
            return;
        case InlineBuiltin::MathCeil:
            collectRoundingCall(id, call, QuotientRounding::Ceil);
            return;
        case InlineBuiltin::MathTrunc:
            collectRoundingCall(id, call, QuotientRounding::Truncate);
            return;
        case InlineBuiltin::MathRound:
            collectRoundingCall(id, call, QuotientRounding::Round);
            return;
        default:
            return;
        }
    }

    void collectRoundingCall(ValueID id, const Value& call, QuotientRounding rounding)
    {
        if (call.builtinCallForm() != BuiltinCallForm::Whole || call.argumentsOperands().size() != 1)
            return;

        ValueID argument = call.argumentsOperands()[0];
        if (std::optional<Quotient> quotient = findReducibleQuotient(argument, rounding))
            m_roundingCalls.append(RoundingCall { id, rounding, *quotient });
    }

    void collectInt32Conversion(ProgramPoint point, ValueID user, ValueID operand, uint32_t operandIndex)
    {
        if (std::optional<Quotient> quotient = findReducibleQuotient(operand, QuotientRounding::ToInt32))
            m_int32Conversions.append(Int32Conversion { point, user, operandIndex, *quotient });
    }

    std::optional<Quotient> findReducibleQuotient(ValueID operand, QuotientRounding rounding) const
    {
        const Value& definition = m_function.at(sameObjectThroughCasts(m_function, operand));
        if (definition.opcode != Opcode::Div)
            return std::nullopt;

        Quotient quotient { definition.leftOperand(), definition.rightOperand() };
        if (!canDivideAsIntegers(rounding, typeOf(quotient.dividend), typeOf(quotient.divisor)))
            return std::nullopt;
        return quotient;
    }

    const Type& typeOf(ValueID id) const { return m_context.program.typeOf(m_function, id); }

    void replaceRoundingCalls()
    {
        for (const RoundingCall& candidate : m_roundingCalls) {
            Value& value = m_function.at(candidate.call);
            value.become(Opcode::DivideIntegers);
            value.setDividendOperand(candidate.quotient.dividend);
            value.setDivisorOperand(candidate.quotient.divisor);
            value.setRounding(candidate.rounding);
            value.type = resultTypeOf(m_context.program, m_function, value, m_context.types);
            value.setWritten({ });
            value.setPrediction({ });
            m_divisions.append(candidate.call);
        }
    }

    void insertInt32Quotients()
    {
        IRBuilder builder(m_context.analyses.context(), m_function, m_context.types, &m_context.analyses);
        for (const Int32Conversion& candidate : m_int32Conversions) {
            Value division = IRBuilder::make(Opcode::DivideIntegers, { candidate.quotient.dividend, candidate.quotient.divisor });
            division.setRounding(QuotientRounding::ToInt32);
            builder.setOrigin(m_function.at(candidate.user).origin());
            ValueID quotient = builder.insertBefore(candidate.point, WTF::move(division));
            m_function.at(candidate.user).setOperand(candidate.operandIndex, quotient);
            m_divisions.append(quotient);
        }
        builder.applyInsertions();
    }

    FunctionPassContext& m_context;
    Function& m_function;
    Vector<RoundingCall, 4> m_roundingCalls;
    Vector<Int32Conversion, 4> m_int32Conversions;
    Vector<ValueID, 8> m_divisions;
};

} // anonymous namespace

PassOutcome reduceIntegerDivisions(FunctionPassContext& context)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    while (IntegerDivisionReducer(context).run() == PassOutcome::Changed)
        outcome = PassOutcome::Changed;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

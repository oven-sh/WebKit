/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include <wtf/CheckedArithmetic.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

class SumOverRecords {
public:
    SumOverRecords(const Code& code, const TypeArena& types, int64_t number = 0)
        : m_code(code)
        , m_types(types)
        , m_number(number)
    {
    }

    bool add(ValueID, int64_t times);
    bool takeAwayWhatIsFromZeroUp(ValueID, bool testResult = true);
    bool isFromZeroUp() const;

private:
    static constexpr unsigned mostSteps = 16;
    static constexpr unsigned mostSums = 64;
    bool isFromZeroUp(size_t firstTerm, unsigned& sumsLeft) const;
    struct Term {
        ValueID root;
        int64_t times { 0 };
        Range range;
    };

    const Code& m_code;
    const TypeArena& m_types;
    CheckedInt64 m_number;
    Vector<Term, 4> m_terms;
};

enum class StepsAreExact : bool { ByTheirTypes, AsAssumed };
std::optional<Range> findStepsFrom(const Code&, const TypeArena&, ValueID, ValueID from, StepsAreExact = StepsAreExact::ByTheirTypes);

struct SecondCounter {
    ValueID counter;
    ValueID stepped;
    Range steps;
    bool counterRises { true };
};

struct EnteringCounters {
    Range counter { Range::empty() };
    Range stepped { Range::empty() };
    bool operator==(const EnteringCounters&) const = default;
};
std::optional<Range> rangeOfSecondCounter(const Code&, const TypeArena&, const SecondCounter&, const EnteringCounters&, const Operation& test, uint32_t operandIndex, bool testIsTrue);
std::optional<EnteringCounters> findEnteringCounters(const Code&, const TypeArena&, const SecondCounter&);
bool stepsSecondCounter(const Code&, const TypeArena&, const SecondCounter&, ValueID stepped, ValueID counter);
bool secondCounterStaysWithin(const Code&, const TypeArena&, const GroundsOfPosition&);
SecondCounter secondCounterOf(const SteppedWithCounter&);
bool isSteppedWithCounter(const Code&, const TypeArena&, const SteppedWithCounter&);
bool everyWayStepsUpFrom(const Code&, const TypeArena&, ValueID, ValueID from, ValueID stepNonNegativeCheck, NOESCAPE const ScopedLambda<bool(ValueID)>& mayGoThrough, NOESCAPE const ScopedLambda<bool(BlockID)>& isReached);
bool everyWayStepsDownFrom(const Code&, const TypeArena&, ValueID, ValueID from, NOESCAPE const ScopedLambda<bool(ValueID)>& mayGoThrough, NOESCAPE const ScopedLambda<bool(BlockID)>& isReached);
bool groundsShowPositionInside(const Code&, const TypeArena&, ValueID position, const GroundsOfPosition&, NOESCAPE const ScopedLambda<bool(ValueID, bool testResult)>& holds);
bool testsShowElementPresent(const Code&, ValueID list, ValueID position, std::span<const PassedTest::Side>);
bool testsProveElementAtOrAfter(const Code&, const TypeArena&, ValueID list, ValueID position, std::span<const PassedTest::Side>);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDecimalRuns.h"
#include "AOTOperationEmitter.h"
#include "AOTReusedDecimals.h"
#include "DecimalFastPaths.h"

namespace JSC { namespace AOT { namespace Typed {

class DecimalEmitter final : public OperationEmitter {
public:
    explicit DecimalEmitter(CodeGenerator&);

    std::optional<DecimalRun> findRun(BlockID, std::span<const ValueID>);

    void beginRun(const DecimalRun&, BlockID, LBasicBlock asWritten);
    void generate(const ValueContext&, const DecimalRun::Part&);
    void makeObject(const ValueContext&);

    const ReusedDecimals& findReusedDecimals(std::span<const BlockID> order, const ScopedLambda<bool(BlockID home, BlockID user)>& dominates) LIFETIME_BOUND;
    void beginWithoutReusedObjects();
    // Without a branch, so that it can stand wherever a value is read.
    void letGo(ValueID);

private:
    struct FastPathGenerator {
        using Value = LValue;
        using Block = LBasicBlock;

        Output& out() { return emitter.m_out; }
        LType int64Type() { return Machine::Int64; }
        Block newBlock();
        void appendTo(Block);
        void continueIf(Value condition, Block otherwise) { emitter.branchUnless(Condition(condition), otherwise); }
        void branch(Value condition, Block taken, Block notTaken) { out().branch(condition, normalEdge(taken), normalEdge(notTaken)); }
        Value addressInTables(ptrdiff_t offset, Value index);
        Value loadFromTables(Value address, ptrdiff_t offset);
        Value countLeadingZeros64(Value value) { return out().ctlz32(value); }

        DecimalEmitter& emitter;
        Vector<std::pair<Block, ScopeFork>, 8> scopesOfBlocksToCome { };
    };
    using FastPaths = DecimalFastPaths<FastPathGenerator>;
    using Words = FastPaths::Words;
    using TrailingZeros = FastPaths::TrailingZeros;

    struct KnownDecimal {
        ValueID origin;
        Words words;
        TrailingZeros trailingZeros;
        // What the words are yet to be divided by.
        std::optional<Words> divisor { };
    };

    void leaveUnless(Condition);
    void requireClassAsMade();
    void requireWordOfClass(Boxed, ptrdiff_t offsetInInstance);
    KnownDecimal decimalIn(ValueID);
    void stripTrailingZerosOf(ValueID);
    TypedPointer wordThatNamesObjectOf(unsigned object);
    TypedPointer wordOfReusedObject(CellPointer, ptrdiff_t offset);
    CellPointer allocate(const ValueContext&, Words);
    Operand argument(ValueID);
    Boxed boxedArgument(const ValueContext&, ValueID);

    Words callForWords(const ValueContext&, RuntimeOperation, std::initializer_list<Operand>);
    LValue areOfDecimal(Words);
    Words wordsOrLeave(Words);
    // The first functor goes on in the block where it has no answer.
    Words wordsOfEither(NOESCAPE const Invocable<Words(LBasicBlock)> auto&, NOESCAPE const Invocable<Words()> auto& otherwise);
    Words wordsInLineOr(NOESCAPE const Invocable<Words(LBasicBlock)> auto& inLine, NOESCAPE const Invocable<Words()> auto& byCall);

    Words construct(const ValueContext&, TrailingZeros);
    Words decimalOfInteger(const ValueContext&, Integer, bool mayBeNegative, TrailingZeros);
    Words decimalOfAnyValue(const ValueContext&, Boxed, const Type&);
    Words compute(const ValueContext&, const DecimalCall&, TrailingZeros);
    Words scale(const ValueContext&, const DecimalCall&, TrailingZeros);
    template<typename T> T compare(const DecimalCall&, NOESCAPE const Invocable<T(LValue, LValue)> auto& ofIntegers, NOESCAPE const Invocable<T(Integer)> auto& ofOrder);
    Condition test(DecimalOperation, const DecimalCall&);
    Boxed answer(const ValueContext&, const DecimalCall&);
    void call(const ValueContext&, const DecimalRun::Part&);

    DecimalRuns m_runs;
    ReusedDecimals m_reused;
    Machine::StackSlot* m_reusedObjects { nullptr };
    FastPathGenerator m_fastPathGenerator { *this };
    FastPaths m_fastPaths { m_fastPathGenerator };

    const DecimalRun* m_run { nullptr };
    LBasicBlock m_asWritten { nullptr };
    bool m_computesInLine { false };
    bool m_classIsKnownToBeAsMade { false };
    Vector<KnownDecimal, 8> m_decimals;
    Vector<std::pair<ValueID, Condition>, 2> m_truths;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

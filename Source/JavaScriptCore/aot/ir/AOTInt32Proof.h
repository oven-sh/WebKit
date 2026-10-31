/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTCFG.h"
#include "AOTEdgeResults.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include "AOTBitVector.h"
#include <wtf/StackCheck.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FunctionAnalyses;

enum class BoundsKind : uint8_t {
    Unknown,
    None,
    Integers,
    Reals,
};

struct NumberBounds {
    static NumberBounds unknown(Int32ProofFailure failure) { return { .failure = failure }; }
    static NumberBounds integers(Range range) { return { .kind = BoundsKind::Integers, .range = range }; }
    static NumberBounds reals(Range range) { return { .kind = BoundsKind::Reals, .range = range }; }

    bool isBounded() const { return kind == BoundsKind::Integers || kind == BoundsKind::Reals; }
    bool isNumber() const { return isBounded() && !mayBeNullish; }
    bool isInteger() const { return kind == BoundsKind::Integers && !mayBeNullish; }
    bool isInt32OrNullish() const { return kind == BoundsKind::None || (kind == BoundsKind::Integers && range.isSubsetOf(Range::int32())); }

    BoundsKind kind { BoundsKind::Unknown };
    Range range;
    bool mayBeNullish { false };
    Int32ProofFailure failure { Int32ProofFailure::Unknown };
};

NumberBounds boundsOfType(const Type&);
NumberBounds narrowToType(const NumberBounds&, const Type&);

bool isEnteredAt(TypeSiteKind);
TypeSiteKind siteKindOf(const Program&, const DeclaredType&);
bool isAssertionOfDependency(const Program&, const DeclaredType&);

struct Int32ProofInputs {
    const Program& program;
    const Function& function;
    FunctionAnalyses& analyses;
    TypeArena& types;
    BuildContext& build;
};

class Int32Prover {
public:
    explicit Int32Prover(const Int32ProofInputs&);

    NumberBounds boundsOf(ValueID);
    bool reliesOnUnprovenFlow() const { return m_reliesOnUnprovenFlow; }

private:
    using Operands = std::span<const NumberBounds>;

    const Type& typeOf(ValueID id) const { return m_typing.typeOf(id); }

    bool exceedsWorkLimit();
    NumberBounds find(ValueID);
    NumberBounds compute(ValueID);
    NumberBounds computeParameter(ValueID);
    NumberBounds computeIncoming(ValueID parameter, Edge, uint32_t parameterIndex);
    NumberBounds computeNarrowedOperand(ValueID parameter, const EdgeResult&);
    NumberBounds computePromise(const Value&);
    NumberBounds computeCast(ValueID, const Value&);
    NumberBounds computeOperator(ValueID, const Value&);
    NumberBounds computeIntegerOperator(Opcode, Operands);
    NumberBounds computeNamedRead(ValueID, const Value&);
    NumberBounds computeCall(ValueID, const Value&);
    std::optional<NumberBounds> computeByRangeRule(BuiltinID, Operands arguments);

    BuiltinID findBuiltinMember(const Value& read);
    BuiltinID findBuiltinCallee(const Value& call);
    TypeID typeIDOfOperand(ValueID, const NumberBounds&);

    static constexpr uint32_t noDepth = UINT32_MAX;

    Int32ProofInputs m_inputs;
    TypingContext m_typing;
    WTF::StackCheck m_stackCheck;
    Vector<std::optional<NumberBounds>> m_known;
    BitVector m_knownRelyingOnUnprovenFlow;
    Vector<uint32_t> m_openDepths;
    ValueID m_subject;
    uint64_t m_steps { 0 };
    uint32_t m_depth { 0 };
    uint32_t m_outermostOpenDepthReached { noDepth };
    bool m_hasGivenUp { false };
    bool m_reliesOnUnprovenFlow { false };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

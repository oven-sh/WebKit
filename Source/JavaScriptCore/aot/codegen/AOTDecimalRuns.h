/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCode.h"
#include "Decimal128.h"
#include "DecimalOperation.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class Facts;
class Plan;
class ReusedDecimals;

// Operations that follow one another in a block and that nothing sees if their operands are Decimals. They are generated twice: first for
// Decimals, which gives up wherever it finds something else, and then as they are written, from the first one on.
struct DecimalRun {
    enum class Step : uint8_t {
        ReadsMethod,
        CallsMethod,
        ReadsConstructor,
        Constructs,
        AsksTruth,
        AsksOrder,
        IsReadThrough,
        IsMadeTwice,
        IsMadeAhead,
    };
    struct Part {
        ValueID value;
        Step step;
        bool isReadBehindRun { false };
        bool mustLackTrailingZeros { false };
        bool isOnlyRounded { false };
    };

    const Part* find(ValueID value) const LIFETIME_BOUND
    {
        auto found = std::ranges::find(parts, value, &Part::value);
        return found == parts.end() ? nullptr : &*found;
    }
    bool has(ValueID value, Step step) const
    {
        const Part* part = find(value);
        return part && part->step == step;
    }

    Vector<Part, 16> parts;
    bool isLikely { false };
    bool endsInReturnedCall { false };
};

struct DecimalCall {
    DecimalOperation operation;
    ValueID receiver;
    ValueID otherDecimal { };
    ValueID count { };
    Decimal128::RoundingMode mode { Decimal128::RoundingMode::HalfEven };
    ValueID options { };
    ValueID modeInOptions { };

    bool givesDecimal() const { return operation <= DecimalOperation::Scale10; }
    bool givesBoolean() const { return operation >= DecimalOperation::Equals && operation <= DecimalOperation::IsFinite; }
};

class DecimalRuns {
    WTF_MAKE_NONCOPYABLE(DecimalRuns);
public:
    DecimalRuns(const Program&, const Plan&, const Facts&, const Code&, const TypeArena&);

    // The values are those of the block from where the run begins.
    std::optional<DecimalRun> find(BlockID, std::span<const ValueID>, ValueID returnedCall, const ReusedDecimals&) const;

    bool isLikelyDecimal(ValueID) const;
    bool makesDecimal(ValueID) const;
    bool onlyReadsWordsOf(ValueID reader, ValueID decimal) const;

    std::optional<DecimalCall> callAt(ValueID) const;
    std::optional<DecimalOperation> testOfOrderAt(ValueID) const;
    DecimalOperation operationAsComputed(const DecimalRun&, ValueID call) const;
    std::optional<Decimal128> constantAt(ValueID) const;
    ValueID originOf(ValueID) const;

private:
    enum class Evidence : uint8_t { None, Weak, Strong };

    Type typeOf(ValueID value) const { return m_types.at(m_code.at(value).type); }
    bool mayBeDecimal(ValueID value) const { return typeOf(value).anyObject; }
    StringID textOf(PropertyKeyID) const;
    std::optional<DecimalOperation> operationNamed(PropertyKeyID) const;
    bool findOptionsOfToFixed(ValueID argument, DecimalCall&) const;
    ValueID callOfMethodReadAt(ValueID) const;
    bool readsConstructor(ValueID) const;
    bool mayConstructDecimal(ValueID) const;
    Evidence evidenceOf(ValueID call, const DecimalCall&) const;
    bool mayBeMadeTwice(const Operation&) const;
    bool mayBeMadeAhead(const Operation&) const;
    void noteReads(DecimalRun&, const ReusedDecimals&) const;

    uint32_t classOf(ValueID) const;
    void unite(ValueID, ValueID);
    Evidence evidenceFor(ValueID value) const { return m_evidence[classOf(value)]; }

    const Program& m_program;
    const Plan& m_plan;
    const Facts& m_facts;
    const Code& m_code;
    const TypeArena& m_types;
    mutable Vector<uint32_t> m_classes;
    Vector<Evidence> m_evidence;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

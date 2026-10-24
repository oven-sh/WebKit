/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffects.h"
#include "AOTEffectsOfOperands.h"
#include "AOTIR.h"
#include "AOTVector.h"
#include <array>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

enum class EffectInputs : uint8_t {
    Row,
    Payload,
    Operands,
    DefinitionsBehindCasts,
    Function,
};

struct OpcodeEffectFacts {
    EffectInputs inputs { EffectInputs::Operands };
    bool staysWithinDeclaredBound { false };
    bool mayExitSideways { false };
    AbstractHeapSet writtenHeaps;
    EffectTraitBounds traits;
};

const OpcodeEffectFacts& effectFactsOf(Opcode);

class EffectsCache {
    WTF_MAKE_NONCOPYABLE(EffectsCache);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(EffectsCache);
public:
    EffectsCache(const EffectContext&, const Function&, bool verifiesAnswers);

    const EffectsOnEdges& of(ValueID) LIFETIME_BOUND;
    const EffectsOnEdges& ofTerminator(BlockID) LIFETIME_BOUND;
    EffectTraitsOnEdges traitsOf(ValueID);
    EffectTraitsOnEdges traitsOfTerminator(BlockID);
    OperandSummary summaryOfOperand(ValueID user, uint32_t operandIndex);

    void didChangeFunction() { ++m_round; }
    void resizeToFunction();

    unsigned numberOfBlocks() const { return m_terminators.size(); }

    const EffectsOnEdges* findKept(ValueID) const LIFETIME_BOUND;
    const EffectsOnEdges* findKeptOfTerminator(BlockID) const LIFETIME_BOUND;

    uint64_t numberOfComputedAnswers() const { return m_numberOfComputedAnswers; }
    uint64_t numberOfKeptAnswers() const { return m_numberOfKeptAnswers; }

private:
    using EntryID = ID<struct EffectsCacheEntryIDTag>;

    struct Answer {
        uint64_t stamp { 0 };
        EntryID entry;
        uint32_t round { 0 };
    };

    struct StampInRound {
        uint64_t stamp { 0 };
        uint32_t round { 0 };
    };

    template<typename Carrier> void answerAgain(Answer&, EntryID& entryOfRow, const Carrier&);
    uint64_t stampInThisRoundOf(ValueID, const Value&);
    template<typename Carrier> const EffectsOnEdges* findKept(const Vector<Answer>&, size_t index, EntryID entryOfRow, const Carrier&) const LIFETIME_BOUND;
    template<typename Carrier> EntryID compute(const Carrier&);
    EntryID entryOf(ValueID);
    EntryID entryOfTerminator(BlockID);

    const EffectContext m_context;
    const Function& m_function;
    Vector<Answer> m_values;
    Vector<Answer> m_terminators;
    std::array<EntryID, numberOfOpcodes> m_entryOfOpcode { };
    std::array<EntryID, numberOfTerminatorKinds> m_entryOfTerminatorKind { };
    InternedRecords<EffectsOnEdges, EntryID, 16> m_entries;
    Vector<EffectTraitsOnEdges, 16> m_traitsOfEntries;
    UncheckedKeyHashMap<uint64_t, EntryID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_entryOfStamp;
    Vector<StampInRound> m_stampsOfUsers;
    UncheckedKeyHashMap<uint64_t, OperandSummary, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_operandSummaryOfStamp;
    uint32_t m_round { 1 };
    const bool m_verifiesAnswers;
    uint64_t m_numberOfComputedAnswers { 0 };
    uint64_t m_numberOfKeptAnswers { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

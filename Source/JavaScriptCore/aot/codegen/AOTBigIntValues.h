/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTCode.h"
#include "AOTVector.h"
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

class BigIntValues {
    WTF_MAKE_NONCOPYABLE(BigIntValues);
public:
    struct CopyInUnlikelyBlock {
        BlockID block;
        ValueID value;
        ValueID producer;
    };

    enum class FrameUse : uint8_t { Never, InLoops, Everywhere };
    BigIntValues(const Code&, const TypeArena&, FrameUse);

    bool isLikely(ValueID value) const { return m_isLikely.get(value.index()); }

    bool mayBeInFrame(ValueID value) const { return m_mayBeInFrame.get(value.index()); }
    bool mayShareItsPlace(ValueID value) const { return m_staysInBlock.get(value.index()); }
    std::span<const ValueID> valuesKeptBeside(ValueID) const LIFETIME_BOUND;
    static constexpr unsigned maxValuesKept = 2;
    ValueID quotientForRemainder(ValueID remainder) const { return m_quotients.get(remainder.index()); }

    bool hasCopies() const { return !m_copies.isEmpty(); }
    std::span<const CopyInUnlikelyBlock> copiesInUnlikelyBlock(BlockID) const LIFETIME_BOUND;
    bool isRare(BlockID block) const { return m_code.at(block).frequency == BlockFrequency::Rare && !m_rareBlocksForBigInts.get(block.index()); }
    static bool isArithmetic(const Operation&);

private:
    struct Flow {
        ValueID from;
        ValueID to;
    };
    template<typename T> using MapByIndex = UncheckedKeyHashMap<uint32_t, T, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;
    class Liveness;

    uint32_t classOf(uint32_t);
    void unite(ValueID, ValueID);
    void findLikelyValues();
    void findBlocksForBigInts();
    void findQuotients();
    void findValuesForFrame();
    void findCarriedValues(std::span<const Flow>, const BitVector& isProducer);
    bool readsOperands(const Operation&) const;
    bool mayUseFrame(BlockID block) const { return !isRare(block) && (m_frameUse == FrameUse::Everywhere || m_code.at(block).isInLoop); }

    const Code& m_code;
    const TypeArena& m_types;
    FrameUse m_frameUse;
    Vector<uint32_t> m_classes;
    BitVector m_isLikely;
    BitVector m_rareBlocksForBigInts;
    BitVector m_mayBeInFrame;
    BitVector m_staysInBlock;
    BitVector m_passesOn;
    MapByIndex<ValueID> m_quotients;
    MapByIndex<Vector<ValueID, maxValuesKept>> m_valuesKept;
    Vector<CopyInUnlikelyBlock> m_copies;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

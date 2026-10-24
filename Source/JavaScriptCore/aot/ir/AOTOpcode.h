/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIRForm.h"
#include "AOTSchemaEnums.h"
#include <array>
#include <bit>
#include <initializer_list>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

using OperandUses = OptionSet<OperandUse>;
using Protocols = OptionSet<Protocol>;

inline constexpr uint32_t numberOfInlineOperands = 3;
inline constexpr uint32_t numberOfInlinePayloadWords = 3;

constexpr bool mustBeDefinition(OperandKind kind)
{
    return kind == OperandKind::Results || kind == OperandKind::IteratorState || kind == OperandKind::Enumerator;
}

template<typename E, unsigned count>
class EnumeratorBitSet {
public:
    constexpr EnumeratorBitSet() = default;
    constexpr EnumeratorBitSet(std::initializer_list<E> enumerators)
    {
        for (E enumerator : enumerators)
            add(enumerator);
    }

    constexpr void add(E enumerator) { m_words[static_cast<unsigned>(enumerator) / 64] |= 1ull << (static_cast<unsigned>(enumerator) % 64); }
    constexpr bool contains(E enumerator) const { return m_words[static_cast<unsigned>(enumerator) / 64] >> (static_cast<unsigned>(enumerator) % 64) & 1; }
    constexpr bool isEmpty() const
    {
        uint64_t all = 0;
        for (uint64_t word : m_words)
            all |= word;
        return !all;
    }
    constexpr EnumeratorBitSet operator|(const EnumeratorBitSet& other) const
    {
        EnumeratorBitSet result;
        for (size_t i = 0; i < m_words.size(); ++i)
            result.m_words[i] = m_words[i] | other.m_words[i];
        return result;
    }
    constexpr EnumeratorBitSet& operator|=(const EnumeratorBitSet& other)
    {
        for (size_t i = 0; i < m_words.size(); ++i)
            m_words[i] |= other.m_words[i];
        return *this;
    }
    constexpr EnumeratorBitSet& operator&=(const EnumeratorBitSet& other)
    {
        for (size_t i = 0; i < m_words.size(); ++i)
            m_words[i] &= other.m_words[i];
        return *this;
    }
    constexpr bool intersects(const EnumeratorBitSet& other) const
    {
        uint64_t common = 0;
        for (size_t i = 0; i < m_words.size(); ++i)
            common |= m_words[i] & other.m_words[i];
        return common;
    }
    // In ascending order.
    template<typename Functor>
    constexpr void forEach(const Functor& functor) const
    {
        for (size_t i = 0; i < m_words.size(); ++i) {
            for (uint64_t word = m_words[i]; word; word &= word - 1)
                functor(static_cast<E>(i * 64 + std::countr_zero(word)));
        }
    }

    friend constexpr bool operator==(const EnumeratorBitSet&, const EnumeratorBitSet&) = default;

private:
    std::array<uint64_t, (count + 63) / 64> m_words { };
};

using OpcodeSet = EnumeratorBitSet<Opcode, numberOfEnumerators<Opcode>>;
using TerminatorKindSet = EnumeratorBitSet<TerminatorKind, numberOfEnumerators<TerminatorKind>>;

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaOperations.h"

namespace JSC { namespace AOT { namespace Typed {

constexpr bool isLegalIn(Opcode opcode, IRForm form) { return formsOf(opcode).contains(form); }
constexpr bool isLegalIn(TerminatorKind kind, IRForm form) { return formsOf(kind).contains(form); }

constexpr bool provesForever(Proof proof) { return proof != Proof::None && lastsOf(proof) == ProofLifetime::Forever; }
constexpr bool provesForStretch(Proof proof) { return lastsOf(proof) == ProofLifetime::Stretch; }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

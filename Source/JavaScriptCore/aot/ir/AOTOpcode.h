/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

template<typename E> struct WordOfEnumeratorBitSet { using Type = uint64_t; };

template<typename E, unsigned count>
class EnumeratorBitSet {
public:
    using Word = typename WordOfEnumeratorBitSet<E>::Type;
    static constexpr unsigned bitsPerWord = sizeof(Word) * 8;

    constexpr EnumeratorBitSet() = default;
    constexpr EnumeratorBitSet(std::initializer_list<E> enumerators)
    {
        for (E enumerator : enumerators)
            add(enumerator);
    }

    constexpr void add(E enumerator) { m_words[static_cast<unsigned>(enumerator) / bitsPerWord] |= static_cast<Word>(1) << (static_cast<unsigned>(enumerator) % bitsPerWord); }
    constexpr bool contains(E enumerator) const { return m_words[static_cast<unsigned>(enumerator) / bitsPerWord] >> (static_cast<unsigned>(enumerator) % bitsPerWord) & 1; }
    constexpr bool isEmpty() const
    {
        Word all = 0;
        for (Word word : m_words)
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
        Word common = 0;
        for (size_t i = 0; i < m_words.size(); ++i)
            common |= m_words[i] & other.m_words[i];
        return common;
    }
    template<typename Functor>
    constexpr void forEach(const Functor& functor) const
    {
        for (size_t i = 0; i < m_words.size(); ++i) {
            for (Word word = m_words[i]; word; word &= word - 1)
                functor(static_cast<E>(i * bitsPerWord + std::countr_zero(word)));
        }
    }

    friend constexpr bool operator==(const EnumeratorBitSet&, const EnumeratorBitSet&) = default;

private:
    std::array<Word, (count + bitsPerWord - 1) / bitsPerWord> m_words { };
};

using OpcodeSet = EnumeratorBitSet<Opcode, numberOfEnumerators<Opcode>>;
using TerminatorKindSet = EnumeratorBitSet<TerminatorKind, numberOfEnumerators<TerminatorKind>>;

// What a template over "anything with the schema's getters" is constrained by: Operation and Draft of walk/AOTCode.h and BodyValue of AOTBody.h hold an opcode,
// Terminator, TerminatorDraft and BodyTerminator a kind. None holds both.
template<typename Holder> concept HoldsOpcode = requires(const Holder& holder) { holder.opcode(); } || requires(const Holder& holder) { holder.opcode; };
template<typename Holder> concept HoldsTerminatorKind = requires(const Holder& holder) { holder.kind(); } || requires(const Holder& holder) { holder.kind; };

template<HoldsOpcode Operation>
constexpr Opcode opcodeOf(const Operation& operation)
{
    if constexpr (requires { operation.opcode(); })
        return operation.opcode();
    else
        return operation.opcode;
}

template<HoldsTerminatorKind Terminator>
constexpr TerminatorKind terminatorKindOf(const Terminator& terminator)
{
    if constexpr (requires { terminator.kind(); })
        return terminator.kind();
    else
        return terminator.kind;
}

} } } // namespace JSC::AOT::Typed

#include "AOTSchemaOperations.h"

#include "AOTOpcodeSets.h"

#endif // ENABLE(AOT)

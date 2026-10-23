/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTSchemaIDs.h"
#include "AOTVector.h"
#include <array>
#include <bit>
#include <iterator>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr uint32_t absentPayloadWord = UINT32_MAX;

template<typename T> struct PayloadWordCodec;

template<typename Tag>
struct PayloadWordCodec<ID<Tag>> {
    static constexpr ID<Tag> decode(uint32_t word) { return ID<Tag>(word); }
    static constexpr uint32_t encode(ID<Tag> id) { return id.index(); }
};

template<typename Tag, typename Storage>
struct PayloadWordCodec<BuiltinTableID<Tag, Storage>> {
    using IDType = BuiltinTableID<Tag, Storage>;
    static constexpr IDType decode(uint32_t word) { return IDType(word); }
    static constexpr uint32_t encode(IDType id) { return id ? id.index() : absentPayloadWord; }
};

template<typename Tag>
struct PayloadWordCodec<PlacedWord<Tag>> {
    static constexpr PlacedWord<Tag> decode(uint32_t word) { return PlacedWord<Tag>(word); }
    static constexpr uint32_t encode(PlacedWord<Tag> word) { return word.value(); }
};

template<typename E>
    requires std::is_enum_v<E>
struct PayloadWordCodec<E> {
    static constexpr E decode(uint32_t word) { return static_cast<E>(word); }
    static constexpr uint32_t encode(E enumerator) { return static_cast<uint32_t>(enumerator); }
};

template<typename F>
struct PayloadWordCodec<OptionSet<F>> {
    static constexpr OptionSet<F> decode(uint32_t word) { return OptionSet<F>::fromRaw(static_cast<typename OptionSet<F>::StorageType>(word)); }
    static constexpr uint32_t encode(OptionSet<F> flags) { return static_cast<uint32_t>(flags.toRaw()); }
};

template<>
struct PayloadWordCodec<bool> {
    static constexpr bool decode(uint32_t word) { return !!word; }
    static constexpr uint32_t encode(bool value) { return value; }
};

template<>
struct PayloadWordCodec<uint32_t> {
    static constexpr uint32_t decode(uint32_t word) { return word; }
    static constexpr uint32_t encode(uint32_t value) { return value; }
};

template<typename T>
struct PayloadWordCodec<std::optional<T>> {
    static constexpr std::optional<T> decode(uint32_t word)
    {
        if (word == absentPayloadWord)
            return std::nullopt;
        return PayloadWordCodec<T>::decode(word);
    }

    static constexpr uint32_t encode(std::optional<T> value) { return value ? PayloadWordCodec<T>::encode(*value) : absentPayloadWord; }
};

template<typename T> constexpr T decodePayloadWord(uint32_t word) { return PayloadWordCodec<T>::decode(word); }
template<typename T> constexpr uint32_t encodePayloadWord(T value) { return PayloadWordCodec<T>::encode(value); }

template<typename T> struct PayloadWords;

template<>
struct PayloadWords<int64_t> {
    static constexpr unsigned count = 2;
    static constexpr int64_t decode(std::span<const uint32_t, count> words) { return static_cast<int64_t>(static_cast<uint64_t>(words[1]) << 32 | words[0]); }
    static constexpr void encode(int64_t value, std::span<uint32_t, count> words)
    {
        words[0] = static_cast<uint32_t>(value);
        words[1] = static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32);
    }
};

template<>
struct PayloadWords<double> {
    static constexpr unsigned count = 2;
    static constexpr double decode(std::span<const uint32_t, count> words) { return std::bit_cast<double>(static_cast<uint64_t>(words[1]) << 32 | words[0]); }
    static constexpr void encode(double value, std::span<uint32_t, count> words)
    {
        uint64_t bits = std::bit_cast<uint64_t>(value);
        words[0] = static_cast<uint32_t>(bits);
        words[1] = static_cast<uint32_t>(bits >> 32);
    }
};

template<typename T, typename Holder>
T decodePayloadRecord(const Holder& holder, unsigned firstWord)
{
    std::array<uint32_t, PayloadWords<T>::count> words;
    for (unsigned i = 0; i < words.size(); ++i)
        words[i] = holder.payloadWordAt(firstWord + i);
    return PayloadWords<T>::decode(words);
}

template<typename T, typename Holder>
void encodePayloadRecord(Holder& holder, unsigned firstWord, const T& record)
{
    std::array<uint32_t, PayloadWords<T>::count> words;
    PayloadWords<T>::encode(record, words);
    for (unsigned i = 0; i < words.size(); ++i)
        holder.setPayloadWordAt(firstWord + i, words[i]);
}

template<typename T>
class PayloadListView {
public:
    static constexpr unsigned wordsPerElement = PayloadWords<T>::count;

    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = T;

        Iterator() = default;

        T operator*() const { return PayloadWords<T>::decode(m_remainingWords.template first<wordsPerElement>()); }
        Iterator& operator++()
        {
            m_remainingWords = m_remainingWords.subspan(wordsPerElement);
            return *this;
        }
        Iterator operator++(int)
        {
            Iterator result = *this;
            ++*this;
            return result;
        }
        bool operator==(const Iterator& other) const { return m_remainingWords.data() == other.m_remainingWords.data(); }

    private:
        friend PayloadListView;

        explicit Iterator(std::span<const uint32_t> remainingWords)
            : m_remainingWords(remainingWords)
        {
        }

        std::span<const uint32_t> m_remainingWords;
    };

    explicit PayloadListView(std::span<const uint32_t> words)
        : m_words(words)
    {
        RELEASE_ASSERT(!(words.size() % wordsPerElement));
    }

    size_t size() const { return m_words.size() / wordsPerElement; }
    bool isEmpty() const { return m_words.empty(); }
    T operator[](size_t index) const
    {
        RELEASE_ASSERT(index < size());
        return PayloadWords<T>::decode(m_words.subspan(index * wordsPerElement).template first<wordsPerElement>());
    }
    Iterator begin() const { return Iterator(m_words); }
    Iterator end() const { return Iterator(m_words.subspan(m_words.size())); }

private:
    std::span<const uint32_t> m_words;
};

static_assert(std::input_iterator<PayloadListView<int64_t>::Iterator>);

template<typename T>
Vector<uint32_t, 16> encodePayloadList(std::span<const T> elements)
{
    constexpr unsigned wordsPerElement = PayloadWords<T>::count;
    Vector<uint32_t, 16> words(FillWith { }, elements.size() * wordsPerElement, 0u);
    for (size_t i = 0; i < elements.size(); ++i)
        PayloadWords<T>::encode(elements[i], words.mutableSpan().subspan(i * wordsPerElement).template first<wordsPerElement>());
    return words;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <concepts>
#include <ranges>
#include <type_traits>
#include <utility>
#include <wtf/Compiler.h>

namespace JSC { namespace AOT { namespace Typed {

struct Proven;

// True in every run of the compiled program. Anybody may read it. Only Proven makes one, and from a prediction only where a test stands.
template<typename T>
class ProvenValue {
public:
    constexpr operator const T&() const LIFETIME_BOUND { return m_value; }
    constexpr const T& value() const LIFETIME_BOUND { return m_value; }
    constexpr auto begin() const LIFETIME_BOUND requires std::ranges::range<const T> { return std::ranges::begin(m_value); }
    constexpr auto end() const LIFETIME_BOUND requires std::ranges::range<const T> { return std::ranges::end(m_value); }

private:
    friend struct Proven;
    constexpr explicit ProvenValue(T value)
        : m_value(std::move(value))
    {
    }

    T m_value;
};

// What heat, an annotation or an open set says. It may be wrong in any run, so it decides only whether something is worth doing and which test to emit.
// Whatever is computed from one is one. It turns into neither its value nor a bool by itself: predicted() marks each place that acts on a guess.
template<typename T>
class Expected {
public:
    constexpr Expected() = default;
    constexpr Expected(T value)
        : m_value(std::move(value))
    {
    }
    constexpr Expected(const ProvenValue<T>& fact)
        : m_value(fact.value())
    {
    }

    constexpr const T& predicted() const& LIFETIME_BOUND { return m_value; }
    constexpr T predicted() && { return std::move(m_value); }

    // Whether two predictions say the same, for caches and for records that hold one.
    constexpr bool isSamePredictionAs(const Expected& other) const { return m_value == other.m_value; }

private:
    T m_value { };
};

// The same of something that lives elsewhere: the record of a type, a set.
template<typename T>
class Expected<const T&> {
public:
    constexpr Expected(const T& value LIFETIME_BOUND)
        : m_value(&value)
    {
    }

    constexpr const T& predicted() const { return *m_value; }

private:
    const T* m_value;
};

template<typename T> inline constexpr bool isExpectedKind = false;
template<typename T> inline constexpr bool isExpectedKind<Expected<T>> = true;

template<typename T> constexpr const T& valueUnderPrediction(const T& value LIFETIME_BOUND) { return value; }
template<typename T> constexpr const T& valueUnderPrediction(const Expected<T>& value LIFETIME_BOUND) { return value.predicted(); }

template<typename A, typename B> concept EitherIsExpected = isExpectedKind<A> || isExpectedKind<B>;

#define AOT_OPERATOR_ON_EXPECTED(op) \
    template<typename A, typename B> requires EitherIsExpected<A, B> \
    constexpr auto operator op(const A& a, const B& b) { return Expected<std::remove_cvref_t<decltype(valueUnderPrediction(a) op valueUnderPrediction(b))>>(valueUnderPrediction(a) op valueUnderPrediction(b)); }

AOT_OPERATOR_ON_EXPECTED(==)
AOT_OPERATOR_ON_EXPECTED(!=)
AOT_OPERATOR_ON_EXPECTED(<)
AOT_OPERATOR_ON_EXPECTED(<=)
AOT_OPERATOR_ON_EXPECTED(>)
AOT_OPERATOR_ON_EXPECTED(>=)
AOT_OPERATOR_ON_EXPECTED(+)
AOT_OPERATOR_ON_EXPECTED(-)
AOT_OPERATOR_ON_EXPECTED(*)
AOT_OPERATOR_ON_EXPECTED(/)

#undef AOT_OPERATOR_ON_EXPECTED

template<typename T>
constexpr Expected<bool> operator!(const Expected<T>& value) { return !value.predicted(); }

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)

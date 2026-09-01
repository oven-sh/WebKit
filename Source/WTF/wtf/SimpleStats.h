/*
 * Copyright (C) 2012, 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Atomics.h>
#include <wtf/FastMalloc.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace WTF {

// Simple and cheap way of tracking statistics if you're not worried about chopping on
// the sum of squares (i.e. the sum of squares is unlikely to exceed 2^52).
//
// These are advisory statistics that may be updated and read concurrently (e.g. JIT
// allocation stats with multiple mutator threads). All field accesses use relaxed
// atomics: individual loads/stores are well-defined, but add() is not a read-modify-write,
// so concurrent add() calls may lose updates and readers may observe mutually inconsistent
// count/sum/sumOfSquares. That is acceptable for these consumers; relaxed accesses exist
// solely to make the races defined behavior. Relaxed atomic loads/stores compile to plain
// loads/stores, so single-threaded behavior and codegen are unchanged.
class SimpleStats {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SimpleStats);
public:
    SimpleStats()
        : m_count(0)
        , m_sum(0)
        , m_sumOfSquares(0)
    {
    }

    SimpleStats(const SimpleStats& other)
        : m_count(other.m_count.load(std::memory_order_relaxed))
        , m_sum(other.m_sum.load(std::memory_order_relaxed))
        , m_sumOfSquares(other.m_sumOfSquares.load(std::memory_order_relaxed))
    {
    }

    SimpleStats& operator=(const SimpleStats& other)
    {
        m_count.store(other.m_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_sum.store(other.m_sum.load(std::memory_order_relaxed), std::memory_order_relaxed);
        m_sumOfSquares.store(other.m_sumOfSquares.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    void add(double value)
    {
        m_count.store(m_count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        m_sum.store(m_sum.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
        m_sumOfSquares.store(m_sumOfSquares.load(std::memory_order_relaxed) + value * value, std::memory_order_relaxed);
    }

    explicit operator bool() const
    {
        return !!count();
    }

    double count() const
    {
        return m_count.load(std::memory_order_relaxed);
    }

    double sum() const
    {
        return m_sum.load(std::memory_order_relaxed);
    }

    double sumOfSquares() const
    {
        return m_sumOfSquares.load(std::memory_order_relaxed);
    }

    double mean() const
    {
        return sum() / count();
    }

    // NB. This gives a biased variance as it divides by the number of samples rather
    // than the degrees of freedom. This is fine once the count grows large, which in
    // our case will happen rather quickly.
    double variance() const
    {
        double count = this->count();
        if (count < 2)
            return 0;

        // Compute <x^2> - <x>^2
        double secondMoment = sumOfSquares() / count;
        double firstMoment = sum() / count;

        double result = secondMoment - firstMoment * firstMoment;

        // It's possible to get -epsilon. Protect against this and turn it into
        // +0.
        if (result <= 0)
            return 0;

        return result;
    }

    // NB. This gives a biased standard deviation. See above.
    double standardDeviation() const
    {
        return sqrt(variance());
    }

private:
    Atomic<double> m_count;
    Atomic<double> m_sum;
    Atomic<double> m_sumOfSquares;
};

} // namespace WTF

using WTF::SimpleStats;

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTVector.h"
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/RefPtr.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC { namespace AOT {

// A list that only grows at its end. A copy shares what the list held when it was copied, and holds for itself what is appended to it later.
template<typename T>
class SharedPrefixList {
public:
    size_t size() const { return m_last ? m_last->end() : 0; }
    bool isEmpty() const { return !m_last; }

    void append(const T& value)
    {
        if (!m_last || !m_last->hasOneRef())
            m_last = adoptRef(*new Segment(RefPtr { m_last }));
        m_last->values.append(value);
    }

    template<typename Predicate>
    bool allOfFrom(size_t first, const Predicate& predicate) const
    {
        Vector<const Segment*, 8> segments;
        for (const Segment* segment = m_last.get(); segment && segment->end() > first; segment = segment->before.get())
            segments.append(segment);
        while (!segments.isEmpty()) {
            const Segment* segment = segments.takeLast();
            for (const T& value : segment->values.span().subspan(first > segment->sizeBefore ? first - segment->sizeBefore : 0)) {
                if (!predicate(value))
                    return false;
            }
        }
        return true;
    }

    template<typename Predicate>
    bool allOf(const Predicate& predicate) const { return allOfFrom(0, predicate); }

    template<typename Functor>
    void forEach(const Functor& functor) const
    {
        allOf([&](const T& value) {
            functor(value);
            return true;
        });
    }

    friend bool operator==(const SharedPrefixList& a, const SharedPrefixList& b)
    {
        if (a.size() != b.size())
            return false;
        const Segment* segmentOfA = a.m_last.get();
        const Segment* segmentOfB = b.m_last.get();
        for (size_t index = a.size(); index-- && segmentOfA != segmentOfB;) {
            if (segmentOfA->values[index - segmentOfA->sizeBefore] != segmentOfB->values[index - segmentOfB->sizeBefore])
                return false;
            if (index == segmentOfA->sizeBefore)
                segmentOfA = segmentOfA->before.get();
            if (index == segmentOfB->sizeBefore)
                segmentOfB = segmentOfB->before.get();
        }
        return true;
    }

private:
    struct Segment : ThreadSafeRefCounted<Segment> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Segment);

        explicit Segment(RefPtr<Segment>&& before)
            : before(WTF::move(before))
            , sizeBefore(this->before ? this->before->end() : 0)
        {
        }

        ~Segment()
        {
            // One after another: a row of segments may be longer than the stack is deep.
            RefPtr<Segment> rest = WTF::move(before);
            while (rest && rest->hasOneRef())
                rest = std::exchange(rest->before, nullptr);
        }

        size_t end() const { return sizeBefore + values.size(); }

        RefPtr<Segment> before;
        size_t sizeBefore;
        Vector<T> values;
    };

    RefPtr<Segment> m_last;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

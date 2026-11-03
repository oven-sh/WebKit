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
// A whole list that is appended is shared too.
template<typename T>
class SharedPrefixList {
public:
    size_t size() const { return m_last ? m_last->end() : 0; }
    bool isEmpty() const { return !m_last; }

    void append(const T& value)
    {
        if (!m_last || !m_last->hasOneRef())
            m_last = adoptRef(*new Segment(RefPtr { m_last }, nullptr));
        m_last->values.append(value);
    }

    void appendList(const SharedPrefixList& other)
    {
        if (!m_last)
            m_last = other.m_last;
        else if (other.m_last)
            m_last = adoptRef(*new Segment(RefPtr { m_last }, RefPtr { other.m_last }));
    }

    template<typename Predicate>
    bool allOfFrom(size_t first, const Predicate& predicate) const
    {
        struct Rest {
            const Segment* segment;
            size_t first;
            bool isPastListAppended;
        };
        Vector<Rest, 8> rests;
        auto addList = [&](const Segment* last, size_t first) {
            for (const Segment* segment = last; segment && segment->end() > first; segment = segment->before.get())
                rests.append({ segment, first > segment->sizeBefore ? first - segment->sizeBefore : 0, false });
        };
        addList(m_last.get(), first);
        while (!rests.isEmpty()) {
            Rest rest = rests.takeLast();
            size_t sizeOfListAppended = rest.segment->sizeOfListAppended;
            if (!rest.isPastListAppended && rest.first < sizeOfListAppended) {
                rests.append({ rest.segment, sizeOfListAppended, true });
                addList(rest.segment->listAppended.get(), rest.first);
                continue;
            }
            for (const T& value : rest.segment->values.span().subspan(rest.first - sizeOfListAppended)) {
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
        FromTheEnd restOfA(a.m_last.get());
        FromTheEnd restOfB(b.m_last.get());
        while (!restOfA.isAtEnd()) {
            if (restOfA.skipWhatIsSharedWith(restOfB))
                continue;
            if (restOfA.takeValue() != restOfB.takeValue())
                return false;
        }
        return true;
    }

private:
    // What stands in front of it, then a whole list, then values of its own.
    struct Segment : ThreadSafeRefCounted<Segment> {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Segment);

        Segment(RefPtr<Segment>&& before, RefPtr<Segment>&& listAppended)
            : before(WTF::move(before))
            , listAppended(WTF::move(listAppended))
            , sizeBefore(this->before ? this->before->end() : 0)
            , sizeOfListAppended(this->listAppended ? this->listAppended->end() : 0)
        {
        }

        ~Segment()
        {
            // One after another: a row of segments may be longer than the stack is deep.
            Vector<RefPtr<Segment>, 4> rest;
            auto take = [&](RefPtr<Segment>& segment) {
                if (segment)
                    rest.append(std::exchange(segment, nullptr));
            };
            take(before);
            take(listAppended);
            while (!rest.isEmpty()) {
                RefPtr<Segment> segment = rest.takeLast();
                if (segment->hasOneRef()) {
                    take(segment->before);
                    take(segment->listAppended);
                }
            }
        }

        size_t end() const { return sizeBefore + sizeOfListAppended + values.size(); }

        RefPtr<Segment> before;
        RefPtr<Segment> listAppended;
        size_t sizeBefore;
        size_t sizeOfListAppended;
        Vector<T> values;
    };

    class FromTheEnd {
    public:
        explicit FromTheEnd(const Segment* last)
        {
            add(last);
            findValue();
        }

        bool isAtEnd() const { return m_rests.isEmpty(); }

        const T& takeValue()
        {
            const T& value = m_rests.last().segment->values[--m_rests.last().numberOfValues];
            findValue();
            return value;
        }

        bool skipWhatIsSharedWith(FromTheEnd& other)
        {
            if (m_rests.last().segment != other.m_rests.last().segment || m_rests.last().numberOfValues != other.m_rests.last().numberOfValues)
                return false;
            m_rests.removeLast();
            other.m_rests.removeLast();
            findValue();
            other.findValue();
            return true;
        }

    private:
        // All that stands in front of the values of a segment, and the first of these.
        struct Rest {
            const Segment* segment;
            size_t numberOfValues;
        };

        void add(const Segment* last)
        {
            if (last)
                m_rests.append({ last, last->values.size() });
        }

        void findValue()
        {
            while (!m_rests.isEmpty() && !m_rests.last().numberOfValues) {
                const Segment* segment = m_rests.takeLast().segment;
                add(segment->before.get());
                add(segment->listAppended.get());
            }
        }

        Vector<Rest, 8> m_rests;
    };

    RefPtr<Segment> m_last;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

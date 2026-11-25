/*
 * Copyright (C) 2026 Anthropic PBC.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace JSC {

// What the embedder wants counted in the text of a SourceProvider, and where the counts go. The text is not changed, and
// neither is what it does: a place at which nothing starts that can be counted is left alone.
struct CoverageCounters {
    enum class Event : uint8_t {
        // Control gets to what starts at the offset: a statement, an AssignmentExpression, the right operand of && || ??, the
        // block of a function, a `catch` or a `finally`, or a clause of a switch. Of several that start there, the outermost.
        // At a ?. the chain goes on. Not what can become a part of the code around it: a literal as an element, a key, an
        // argument or the test of a clause, the right side of +=, or an operand in parentheses other than a && || ?? or ?:.
        Reached,
        // At the statement that an `if` runs: the condition is false.
        Skipped,
    };

    struct Place {
        static constexpr unsigned eventShift = 30;

        unsigned counter() const { return counterAndEvent & ((1u << eventShift) - 1); }
        Event event() const { return static_cast<Event>(counterAndEvent >> eventShift); }

        friend bool operator==(const Place&, const Place&) = default;

        uint32_t offset;
        uint32_t counterAndEvent;
    };
    using Places = std::span<const Place>;

    Places placesIn(unsigned firstOffset, unsigned lastOffset) const
    {
        auto rest = places.subspan(std::ranges::lower_bound(places, firstOffset, { }, &Place::offset) - places.begin());
        return rest.first(std::ranges::find_if(rest, [&](auto& place) { return place.offset > lastOffset; }) - rest.begin());
    }
    Places placesAt(unsigned offset) const { return placesIn(offset, offset); }

    Places places; // Sorted by offset.
    std::span<uint64_t> counts; // Added to without synchronization, so code of one thread only.
};

} // namespace JSC

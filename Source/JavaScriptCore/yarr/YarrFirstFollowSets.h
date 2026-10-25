/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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

#include <array>
#include <optional>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/StackCheck.h>
#include <wtf/Vector.h>

namespace JSC { namespace Yarr {

struct PatternAlternative;
struct PatternTerm;
struct YarrPattern;

// Equal sets have equal ids.
enum class CharacterSetID : uint32_t { Unknown = 0 };

// NotWordCharacter includes the end of the input.
enum class WordCharacterKnowledge : uint8_t { Unknown, WordCharacter, NotWordCharacter };

// UnicodeIgnoreCase is \w under /iu and /iv, which also has U+017F and U+212A.
enum class WordClassKind : uint8_t { Plain, UnicodeIgnoreCase };

struct CharacterSet {
    // Code points, both ends inclusive. Ascending, disjoint, not adjacent.
    Vector<std::pair<char32_t, char32_t>, 4> ranges;
    bool includesEndOfInput { false };

    friend bool operator==(const CharacterSet&, const CharacterSet&) = default;
};

// Bounds the time one set costs. Hit: Unknown.
static constexpr unsigned maxTermsVisitedPerSet = 96;
// Bounds the memory of one stored union. Hit: Unknown.
static constexpr unsigned maxRangesPerSet = 2048;
// Bounds the backward walk of one \b. Hit: Unknown.
static constexpr unsigned maxTermsVisitedBeforeWordBoundary = 256;

class FirstFollowSets {
public:
    const CharacterSet& set(CharacterSetID) const;
    // False when the sets overlap and when an id is Unknown.
    bool areDisjoint(CharacterSetID, CharacterSetID) const;
    WordCharacterKnowledge wordCharacterKnowledge(CharacterSetID, WordClassKind) const;

    unsigned limitHits() const { return m_limitHits; }
    void countLimitHit() { ++m_limitHits; }
    bool didExhaustStack() const { return m_didExhaustStack; }
    void countStackExhaustion()
    {
        m_didExhaustStack = true;
        ++m_limitHits;
    }

private:
    friend class CharacterSetWalker;
    friend void computeFirstFollowSets(YarrPattern&, StackCheck&);

    CharacterSetID intern(CharacterSet&&);

    Vector<CharacterSet> m_sets;
    UncheckedKeyHashMap<unsigned, Vector<CharacterSetID, 1>> m_idsByHash;
    CharacterSetID m_lineTerminators { CharacterSetID::Unknown };
    std::array<CharacterSetID, 2> m_wordCharacters { };
    std::array<CharacterSetID, 2> m_notWordCharacters { };
    unsigned m_limitHits { 0 };
    bool m_didExhaustStack { false };
};

void computeFirstFollowSets(YarrPattern&, StackCheck&);

WordClassKind wordClassKind(const YarrPattern&, const PatternTerm&);

class CharacterSetWalker {
public:
    enum class Outcome : uint8_t { GaveUp, Done };

    // A null Continuation*: the pattern, or the lookahead, ends after the alternative. A null alternative: the walk stops there.
    struct Continuation {
        const PatternAlternative* alternative;
        unsigned nextTermIndex;
        const PatternTerm* repeatedGroup;
        const Continuation* outer;
    };

    struct NextCharacters {
        CharacterSetID set { CharacterSetID::Unknown };
        // The terms walked can match without consuming, so `set` does not bound the next character.
        bool reachesEnd { false };
    };

    CharacterSetWalker(YarrPattern&, StackCheck&);

    NextCharacters firstSet(const PatternAlternative&);
    NextCharacters followSet(const PatternAlternative&, unsigned nextTermIndex, const Continuation*, std::optional<WordClassKind> failingWordBoundary = std::nullopt);
    WordCharacterKnowledge lastCharacterKnowledge(const PatternAlternative&, unsigned endTermIndex, WordClassKind);

private:
    struct Walk;

    FirstFollowSets& sets();
    Outcome chargeVisit(Walk&);
    Outcome collect(const PatternAlternative&, unsigned firstTermIndex, const Continuation*, bool passedAssertion, Walk&);
    Outcome collectLookahead(const PatternTerm&, Walk&);
    NextCharacters internMembers(Walk&);
    WordCharacterKnowledge lastCharacterKnowledge(const PatternAlternative&, unsigned endTermIndex, WordClassKind, unsigned& budget);
    WordCharacterKnowledge groupLastCharacterKnowledge(const PatternTerm&, WordClassKind, unsigned& budget);

    YarrPattern& m_pattern;
    StackCheck& m_stackCheck;
};

} } // namespace JSC::Yarr

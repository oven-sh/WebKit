/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTImageForward.h"
#include "AOTVector.h"
#include "JSExportMacros.h"
#include "YarrFlags.h"
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

using Typed::RegExpRecordID;

// Literal: code mentions the RegExp and names its record by number.
// Evaluated, Candidate: Facts say that the text may arrive at RegExp, and the run time finds the record by the hash of the text: findRegExpRecord().
// Evaluated: at some call it is one of all the texts that arrive there. Candidate: other texts may arrive wherever it does.
enum class RegExpRequestOrigin : uint8_t {
    Literal,
    Evaluated,
    Candidate,
};

enum class RegExpDeclineReason : uint8_t {
    NotSupportedByCodeGenerator,
    NestedTooDeep,
    CodeTooLarge,
    PatternTooLong,
    SectionFull,
};

struct RegExpNamedGroupStrings {
    StringID name;
    Vector<unsigned> parenIndices;
};

struct RegExpRequest {
    StringID pattern;
    // Only flags that ImageRegExp::significantFlags() keeps.
    OptionSet<Yarr::Flags> flags;
    RegExpRequestOrigin origin { RegExpRequestOrigin::Literal };
    StringID atom { };
    Vector<StringID> captureGroupNames { };
    Vector<RegExpNamedGroupStrings> namedGroups { };
};

// The literals come first, and the number of a literal's record is the index of its request. A predicted text gets a record only where that pays: encodeRegExpRecords().
class RegExpRequests {
    WTF_MAKE_TZONE_ALLOCATED(RegExpRequests);
public:
    RegExpRequests() = default;
    // Each list is sorted by pattern, then by flags, without duplicates. No predicted text is also a literal.
    JS_EXPORT_PRIVATE explicit RegExpRequests(Vector<RegExpRequest>&& literals, Vector<RegExpRequest>&& predictedTexts = { });

    std::span<const RegExpRequest> all() const { return m_requests.span(); }
    std::span<const RegExpRequest> literals() const { return all().first(m_numberOfLiterals); }
    std::span<const RegExpRequest> predictedTexts() const { return all().subspan(m_numberOfLiterals); }
    // Of a literal.
    const RegExpRequest& at(RegExpRecordID record) const { return literals()[record.index()]; }

    // Null if no code mentions the RegExp. Takes the flags as written; insignificant ones are ignored.
    JS_EXPORT_PRIVATE RegExpRecordID recordOf(StringID pattern, OptionSet<Yarr::Flags>) const;

    template<typename Functor>
    void forEachString(const Functor& functor) const
    {
        for (const RegExpRequest& request : m_requests) {
            functor(request.pattern);
            if (request.atom)
                functor(request.atom);
            for (StringID name : request.captureGroupNames) {
                if (name)
                    functor(name);
            }
            for (const RegExpNamedGroupStrings& group : request.namedGroups)
                functor(group.name);
        }
    }

private:
    Vector<RegExpRequest> m_requests;
    size_t m_numberOfLiterals { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTImageForward.h"
#include "AOTVector.h"
#include "YarrFlags.h"
#include <compare>
#include <span>
#include <tuple>
#include <wtf/OptionSet.h>
#include <wtf/PrintStream.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

namespace Typed {
class CallGraph;
class FlowResults;
class StringArena;
}

using Typed::RegExpRecordID;
using Typed::ValueID;

// Evaluated: a text that the build puts together from the constants that reach a call of RegExp. Candidate: a text that flow names for a call.
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
    TextKnownAtRunTime,
};

struct RegExpSite {
    FunctionID function;
    ValueID value;
    SourceLocation location { noSourceLocation };

    friend bool operator==(const RegExpSite& a, const RegExpSite& b) { return a.function == b.function && a.value == b.value; }
    friend auto operator<=>(const RegExpSite& a, const RegExpSite& b) { return std::tuple { a.function, a.value } <=> std::tuple { b.function, b.value }; }
};

struct RegExpNamedGroupStrings {
    StringID name;
    Vector<unsigned> parenIndices;
};

struct RegExpRequest {
    StringID pattern;
    OptionSet<Yarr::Flags> flags;
    RegExpRequestOrigin origin { RegExpRequestOrigin::Literal };
    Vector<RegExpSite, 1> sites { };
    StringID atom { };
    Vector<StringID> captureGroupNames { };
    Vector<RegExpNamedGroupStrings> namedGroups { };
};

class RegExpRequests {
    WTF_MAKE_TZONE_ALLOCATED(RegExpRequests);
public:
    RegExpRequests() = default;
    JS_EXPORT_PRIVATE RegExpRequests(Vector<RegExpRequest>&& literals, Vector<RegExpRequest>&& candidates);

    std::span<const RegExpRequest> all() const { return m_requests.span(); }
    std::span<const RegExpRequest> literals() const { return all().first(m_numberOfLiterals); }
    std::span<const RegExpRequest> candidates() const { return all().subspan(m_numberOfLiterals); }
    const RegExpRequest& at(RegExpRecordID record) const { return m_requests[record.index()]; }

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

    void dump(PrintStream&, const Typed::Program&) const;

private:
    Vector<RegExpRequest> m_requests;
    uint32_t m_numberOfLiterals { 0 };
};

JS_EXPORT_PRIVATE RegExpRequests collectRegExpRequests(const Typed::Program&, const Typed::FlowResults&, const Typed::CallGraph&, Typed::StringArena&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

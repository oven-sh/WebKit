/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImageForward.h"
#include "AOTLimits.h"
#include "RegExpKey.h"
#include "Yarr.h"
#include "YarrCharacterSetBits.h"
#include "YarrErrorCode.h"
#include "YarrFlags.h"
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

enum class RegExpMetadata : uint8_t {
    Unknown,
    Known,
};

struct RegExpNamedGroupSummary {
    String name;
    Vector<unsigned> parenIndices;
};

struct RegExpSummary {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(RegExpSummary);

    bool isValid() const { return !Yarr::hasError(syntaxError); }
    bool hasNamedGroups() const { return !captureGroupNames.isEmpty() || !namedGroups.isEmpty(); }

    Yarr::ErrorCode syntaxError { Yarr::ErrorCode::NoError };
    RegExpMetadata metadata { RegExpMetadata::Unknown };
    bool exceedsParenthesesDepth { false };
    bool hasUnicodePropertyLookup { false };
    bool sourceIsPattern { false };
    unsigned patternHash { 0 };
    unsigned numSubpatterns { 0 };
    unsigned minimumSize { 0 };
    unsigned numDuplicateNamedCaptureGroups { 0 };
    Yarr::SpecificPattern specificPattern { Yarr::SpecificPattern::None };
    String source;
    String atom;
    std::unique_ptr<const Yarr::CharacterSetWithRanges> characterSet;
    Vector<String> captureGroupNames;
    Vector<RegExpNamedGroupSummary> namedGroups;
};

#if ASAN_ENABLED || ASSERT_ENABLED
inline constexpr size_t regExpCompilerStackBytesPerLevel = 16 * KB;
#else
inline constexpr size_t regExpCompilerStackBytesPerLevel = 3 * KB;
#endif
inline constexpr size_t regExpCompilerStackSize = 64 * KB + Limits::regExpParenthesesDepth * regExpCompilerStackBytesPerLevel;

JS_EXPORT_PRIVATE bool hasStackForRegExpCompiler();

JS_EXPORT_PRIVATE RegExpSummary summarizeRegExp(StringView pattern, OptionSet<Yarr::Flags>, unsigned parenthesesDepthLimit = Limits::regExpParenthesesDepth);

class RegExpSummaries {
    WTF_MAKE_TZONE_ALLOCATED(RegExpSummaries);
    WTF_MAKE_NONCOPYABLE(RegExpSummaries);
public:
    explicit RegExpSummaries(unsigned parenthesesDepthLimit);

    JS_EXPORT_PRIVATE const RegExpSummary& summaryOf(StringID pattern, OptionSet<Yarr::Flags>, StringView patternText);
    const RegExpSummary* summaryIfComputed(StringID pattern, OptionSet<Yarr::Flags>) const;

private:
    using Key = uint64_t;

    static Key keyOf(StringID pattern, OptionSet<Yarr::Flags>);
    const RegExpSummary& summaryOfText(StringView patternText, OptionSet<Yarr::Flags>);

    const unsigned m_parenthesesDepthLimit;
    mutable Lock m_lock;
    UncheckedKeyHashMap<Key, std::unique_ptr<const RegExpSummary>, DefaultHash<Key>, WTF::UnsignedWithZeroKeyHashTraits<Key>> m_summaries WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<RegExpKey, std::unique_ptr<const RegExpSummary>> m_summariesByText WTF_GUARDED_BY_LOCK(m_lock);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

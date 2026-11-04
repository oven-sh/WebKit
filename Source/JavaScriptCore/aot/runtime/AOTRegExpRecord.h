/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "Yarr.h"
#include "YarrCharacterSetBits.h"
#include "YarrFlags.h"
#include <type_traits>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

enum class RegExpRecordForm : uint8_t {
    Code,
    Atom,
    NoCode,
    NoMetadata,
};

struct ImageRegExp {
    static constexpr OptionSet<Yarr::Flags> significantFlags(OptionSet<Yarr::Flags> flags)
    {
        return flags - OptionSet<Yarr::Flags> { Yarr::Flags::Global, Yarr::Flags::HasIndices };
    }

    static constexpr uint32_t hashOf(unsigned patternHash, OptionSet<Yarr::Flags> flags)
    {
        return patternHash * 31 + significantFlags(flags).toRaw();
    }

    RegExpRecordForm form() const { return static_cast<RegExpRecordForm>(rawForm); }
    Yarr::SpecificPattern specificPattern() const { return static_cast<Yarr::SpecificPattern>(rawSpecificPattern); }
    OptionSet<Yarr::Flags> flags() const { return OptionSet<Yarr::Flags>::fromRaw(rawFlags); }

    bool hasCharacterSet() const { return specificPattern() == Yarr::SpecificPattern::CharacterClass || specificPattern() == Yarr::SpecificPattern::CharacterClassPlus; }

    uint32_t pattern;
    uint32_t atom;
    uint32_t codeFor8Bit;
    uint32_t codeFor16Bit;
    uint32_t numSubpatterns;
    uint32_t minimumSize;
    uint32_t details;
    uint16_t rawFlags;
    uint8_t rawForm : 2;
    uint8_t isLiteral : 1;
    uint8_t sourceIsPattern : 1;
    uint8_t hasNamedGroups : 1;
    // The pattern matches no subject that holds a character of more than one byte, so it has no code for 16-bit subjects.
    uint8_t hasCodeFor8BitSubjectsAlone : 1;
    // Its code gives a search up that goes back over the same places again and again, for a program that remembers the searches that have failed.
    uint8_t handsLongSearchesOver : 1;
    uint8_t reserved : 1;
    uint8_t rawSpecificPattern;
};
static_assert(sizeof(ImageRegExp) == 32);
static_assert(std::has_unique_object_representations_v<ImageRegExp>);
static_assert(sizeof(OptionSet<Yarr::Flags>) == sizeof(uint16_t));

struct ImageRegExpHash {
    uint32_t hash;
    uint32_t record;
};
static_assert(sizeof(ImageRegExpHash) == 8);
static_assert(std::has_unique_object_representations_v<ImageRegExpHash>);

struct ImageRegExpCharacterSet {
    static constexpr size_t alignment = alignof(uint64_t);

    Yarr::CharacterSetBits bits;
};
static_assert(sizeof(ImageRegExpCharacterSet) == 40);
static_assert(alignof(ImageRegExpCharacterSet) == ImageRegExpCharacterSet::alignment);
static_assert(std::has_unique_object_representations_v<ImageRegExpCharacterSet>);
static_assert(!(sizeof(ImageRegExpCharacterSet) % alignof(Yarr::CharacterRange)));

struct ImageRegExpNamedGroups {
    static constexpr size_t alignment = alignof(uint32_t);

    uint32_t numDuplicateNamedCaptureGroups;
    uint32_t numberOfCaptureGroupNames;
    uint32_t numberOfNamedGroups;
    uint32_t numberOfWords;
};
static_assert(sizeof(ImageRegExpNamedGroups) == 16);
static_assert(alignof(ImageRegExpNamedGroups) == ImageRegExpNamedGroups::alignment);
static_assert(std::has_unique_object_representations_v<ImageRegExpNamedGroups>);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)

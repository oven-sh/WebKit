/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTRegExpRecord.h"
#include <optional>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace AOT {

class Image;

struct RegExpEntrypoints {
    const void* for8Bit;
    const void* for16Bit;
};

struct RegExpNamedGroups {
    unsigned numDuplicateNamedCaptureGroups { 0 };
    FixedVector<AtomString> captureGroupNames;
    UncheckedKeyHashMap<String, Vector<unsigned>> namedGroupToParenIndices;
};

struct RegExpNamedGroupWords {
    const ImageRegExpNamedGroups& header;
    std::span<const uint32_t> words;
};

class RegExpRecords {
public:
    JS_EXPORT_PRIVATE explicit RegExpRecords(const Image&);

    uint32_t size() const { return static_cast<uint32_t>(m_records.size()); }
    const ImageRegExp& at(RegExpRecordID record) const { return m_records[record.index()]; }
    RegExpRecordID idOf(const ImageRegExp& record) const { return RegExpRecordID(static_cast<uint32_t>(&record - m_records.data())); }

    JS_EXPORT_PRIVATE RegExpEntrypoints entrypointsOf(const ImageRegExp&) const;
    JS_EXPORT_PRIVATE Yarr::CharacterSetView characterSetOf(const ImageRegExp&) const;
    std::span<const ImageRegExpHash> recordsWithHash(uint32_t hash) const;
    RegExpNamedGroupWords namedGroupWordsOf(const ImageRegExp&) const;

private:
    std::span<const ImageRegExp> m_records;
    std::span<const ImageRegExpHash> m_hashes;
    std::span<const uint8_t> m_details;
    std::span<const uint8_t> m_code;
};

JS_EXPORT_PRIVATE const ImageRegExp* findRegExpRecord(VM&, const String& pattern, OptionSet<Yarr::Flags>);
JS_EXPORT_PRIVATE RegExpEntrypoints regExpEntrypointsOf(VM&, const ImageRegExp&);
JS_EXPORT_PRIVATE Yarr::CharacterSetView regExpCharacterSetOf(VM&, const ImageRegExp&);
JS_EXPORT_PRIVATE String regExpPatternOf(VM&, const ImageRegExp&);
JS_EXPORT_PRIVATE String regExpAtomOf(VM&, const ImageRegExp&);
JS_EXPORT_PRIVATE RegExpNamedGroups decodeRegExpNamedGroups(VM&, const ImageRegExp&);

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT)

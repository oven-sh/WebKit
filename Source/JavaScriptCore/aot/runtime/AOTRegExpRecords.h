/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTRegExpRecord.h"
#include "AOTVector.h"
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class RegExp;
class VM;

namespace AOT {

class CacheRegistry;
class Image;

struct RegExpEntrypoints {
    const void* for8Bit;
    const void* for16Bit;
};

struct RegExpNamedGroups {
    unsigned numDuplicateNamedCaptureGroups { 0 };
    FixedVector<AtomString> captureGroupNames;
    UncheckedKeyHashMap<String, WTF::Vector<unsigned>> namedGroupToParenIndices;
};

class RegExpRecords {
public:
    JS_EXPORT_PRIVATE explicit RegExpRecords(const Image&);

    uint32_t size() const { return static_cast<uint32_t>(m_records.size()); }
    const ImageRegExp& at(RegExpRecordID record) const { return m_records[record.index()]; }
    RegExpRecordID idOf(const ImageRegExp& record) const { return RegExpRecordID(static_cast<uint32_t>(&record - m_records.data())); }

private:
    std::span<const ImageRegExp> m_records;
};

JS_EXPORT_PRIVATE const ImageRegExp* findRegExpRecord(VM&, const String& pattern, OptionSet<Yarr::Flags>);
JS_EXPORT_PRIVATE RegExpEntrypoints regExpEntrypointsOf(VM&, const ImageRegExp&);
JS_EXPORT_PRIVATE Yarr::CharacterSetView regExpCharacterSetOf(VM&, const ImageRegExp&);
JS_EXPORT_PRIVATE String regExpPatternOf(VM&, const ImageRegExp&);
JS_EXPORT_PRIVATE String regExpAtomOf(VM&, const ImageRegExp&);
JS_EXPORT_PRIVATE RegExpNamedGroups decodeRegExpNamedGroups(VM&, const ImageRegExp&);

class RegExpLiterals {
    WTF_MAKE_TZONE_ALLOCATED(RegExpLiterals);
    WTF_MAKE_NONCOPYABLE(RegExpLiterals);
public:
    explicit RegExpLiterals(CacheRegistry&);

    JS_EXPORT_PRIVATE RegExp* regExpFor(VM&, RegExpRecordID, OptionSet<Yarr::Flags>);

    template<typename Visitor> void visitRoots(Visitor&);

private:
    // A cell names its record and its flags, so the set holds the cells and nothing else.
    struct CellHash {
        static unsigned hash(RegExp*);
        static bool equal(RegExp* a, RegExp* b) { return a == b; }
        static constexpr bool safeToCompareToEmptyOrDeleted = false;
    };

    const ImageRegExp* m_records { nullptr };
    UncheckedKeyHashSet<RegExp*, CellHash> m_cells;
    Vector<RegExp*> m_cellsInOrderOfCreation;
    size_t m_numberOfCellsVisited { 0 };
    uint32_t m_lastFullVisitVersion { 0 };
};

} // namespace AOT

} // namespace JSC

#endif // ENABLE(AOT)

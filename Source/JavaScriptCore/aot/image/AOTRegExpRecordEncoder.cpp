/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRegExpRecordEncoder.h"

#if ENABLE(AOT) && ENABLE(YARR_JIT)

#include "AOTBuildContext.h"
#include "AOTSectionWriter.h"
#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

static uint32_t encodeCharacterSet(SectionWriter& details, const Yarr::CharacterSetWithRanges& set)
{
    ASSERT(!set.bits.firstWideRange);
    RELEASE_ASSERT(set.bits.wideRangeCount == set.wideRanges.size());

    ImageRegExpCharacterSet header { set.bits };
    uint32_t offset = details.append(asByteSpan(header), ImageRegExpCharacterSet::alignment);
    details.append(asByteSpan(set.wideRanges.span()), alignof(Yarr::CharacterRange));
    return offset;
}

static uint32_t encodeNamedGroups(SectionWriter& details, const RegExpRequest& request, const RegExpSummary& summary)
{
    Vector<uint32_t> words;
    for (StringID name : request.captureGroupNames)
        words.append(name ? name.index() + 1 : 0);
    for (const RegExpNamedGroupStrings& group : request.namedGroups) {
        words.append(group.name.index());
        words.append(static_cast<uint32_t>(group.parenIndices.size()));
        words.appendVector(group.parenIndices);
    }

    ImageRegExpNamedGroups header {
        .numDuplicateNamedCaptureGroups = summary.numDuplicateNamedCaptureGroups,
        .numberOfCaptureGroupNames = static_cast<uint32_t>(request.captureGroupNames.size()),
        .numberOfNamedGroups = static_cast<uint32_t>(request.namedGroups.size()),
        .numberOfWords = static_cast<uint32_t>(words.size()),
    };
    uint32_t offset = details.append(asByteSpan(header), ImageRegExpNamedGroups::alignment);
    details.append(asByteSpan(words.span()), alignof(uint32_t));
    return offset;
}

static void encodeMetadata(ImageRegExp& record, const RegExpRequest& request, const RegExpSummary& summary, SectionWriter& details)
{
    record.numSubpatterns = summary.numSubpatterns;
    record.minimumSize = summary.minimumSize;
    record.rawSpecificPattern = static_cast<uint8_t>(summary.specificPattern);
    record.sourceIsPattern = summary.sourceIsPattern;
    record.hasNamedGroups = summary.hasNamedGroups();
    RELEASE_ASSERT(!record.hasNamedGroups || !record.hasCharacterSet());
    if (record.hasNamedGroups)
        record.details = encodeNamedGroups(details, request, summary);
    if (record.hasCharacterSet()) {
        RELEASE_ASSERT(summary.characterSet);
        record.details = encodeCharacterSet(details, *summary.characterSet);
    }
}

static ImageRegExp createRecord(const RegExpRequest& request, RegExpRecordForm form, const RegExpSummary& summary, const std::array<uint32_t, 2>& entries, SectionWriter& details)
{
    ASSERT(request.flags == ImageRegExp::significantFlags(request.flags));
    RELEASE_ASSERT((form == RegExpRecordForm::NoMetadata) == (summary.metadata == RegExpMetadata::Unknown));
    ImageRegExp record { };
    record.pattern = request.pattern.index();
    record.rawFlags = request.flags.toRaw();
    record.rawForm = static_cast<uint8_t>(form);
    record.isLiteral = request.origin == RegExpRequestOrigin::Literal;

    switch (summary.metadata) {
    case RegExpMetadata::Unknown:
        break;
    case RegExpMetadata::Known:
        encodeMetadata(record, request, summary, details);
        break;
    }

    switch (form) {
    case RegExpRecordForm::Code:
        record.codeFor8Bit = entries[0];
        record.codeFor16Bit = entries[1];
        break;
    case RegExpRecordForm::Atom:
        RELEASE_ASSERT(request.atom);
        record.atom = request.atom.index();
        break;
    case RegExpRecordForm::NoCode:
    case RegExpRecordForm::NoMetadata:
        break;
    }
    return record;
}

static bool hasRecord(RegExpRequestOrigin origin, RegExpRecordForm form)
{
    switch (origin) {
    case RegExpRequestOrigin::Literal:
        return true;
    case RegExpRequestOrigin::Candidate:
        break;
    }
    switch (form) {
    case RegExpRecordForm::Code:
    case RegExpRecordForm::Atom:
    // The run time need not parse the text to create the object.
    case RegExpRecordForm::NoCode:
        return true;
    case RegExpRecordForm::NoMetadata:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t outputSizeOf(const RegExpSummary& summary)
{
    return (summary.numSubpatterns + 1) * 2 + summary.numDuplicateNamedCaptureGroups;
}

RegExpSections encodeRegExpRecords(const RegExpRecordInputs& inputs, BuildContext& context)
{
    std::span<const RegExpRequest> requests = inputs.requests.all();
    RELEASE_ASSERT(requests.size() == inputs.compiled.results.size() && requests.size() == inputs.section.entries.size());

    Vector<ImageRegExp> records;
    Vector<ImageRegExpHash> hashes;
    SectionWriter details(context);
    uint32_t maxOutputSize = minRegExpOutputSize;
    for (size_t index = 0; index < requests.size(); ++index) {
        const RegExpRequest& request = requests[index];
        RegExpRecordForm form = inputs.compiled.results[index].form;
        if (!hasRecord(request.origin, form))
            continue;

        uint32_t recordNumber = static_cast<uint32_t>(records.size());
        RELEASE_ASSERT(request.origin == RegExpRequestOrigin::Candidate || recordNumber == index);
        const RegExpSummary* summary = context.regExpSummaries().summaryIfComputed(request.pattern, request.flags);
        RELEASE_ASSERT(summary);

        records.append(createRecord(request, form, *summary, inputs.section.entries[index], details));
        hashes.append(ImageRegExpHash { ImageRegExp::hashOf(summary->patternHash, request.flags), recordNumber });
        if (form == RegExpRecordForm::Code)
            maxOutputSize = std::max(maxOutputSize, outputSizeOf(*summary));
    }
    if (details.didOverflow())
        return { };

    std::ranges::sort(hashes, { }, [](const ImageRegExpHash& entry) {
        return std::pair { entry.hash, entry.record };
    });
    return { bytesOf(records), bytesOf(hashes), details.takeBytes(), maxOutputSize };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && ENABLE(YARR_JIT)
